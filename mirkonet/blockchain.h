#pragma once
#include "types.h"
#include "mvm.h"
#include "staking.h"


struct AccountBalance {
    NodeID   owner;
    uint32_t balance;
    uint32_t nonce;
    uint32_t lastFaucet;
    bool     used;
};

DeployCache g_deployCache;

class BlockchainState {
public:
    Block          chain[MAX_BLOCKS];
    uint32_t       chainLen = 0;    uint32_t       chainOffset = 0;
    Checkpoint     checkpoints[MAX_CHECKPOINTS];
    uint8_t        checkpointCount = 0;

    Contract       contracts[MAX_CONTRACTS];
    uint8_t        contractCount = 0;

    AccountBalance accounts[MAX_ACCOUNTS];
    uint8_t        accountCount = 0;

    Transaction    mempool[MAX_PENDING_TX];
    uint8_t        mempoolSize = 0;

    StakingEngine  staking;
    MirkoVM        vm;

    uint32_t       blockFees = 0;

    void initGenesis(const NodeID& founder) {
        Block& gen = chain[0];
        memset(&gen, 0, sizeof(Block));
        gen.header.index = 0;
        gen.header.timestamp = 0;
        gen.header.prevHash = ZERO_HASH;
        gen.header.txCount = 0;
        gen.header.slot = 0;
        gen.header.epoch = 0;
        gen.header.reward = 0;
        gen.header.stateRoot = ZERO_HASH;
        gen.computeHash();
        chainLen = 1;

        setBalance(founder, GENESIS_PER_NODE);
        staking.totalSupply = GENESIS_PER_NODE;

        uint32_t stakeAmt = GENESIS_PER_NODE / 2;
        uint32_t bal = getBalance(founder);
        bal -= stakeAmt;
        setBalance(founder, bal);
        staking.addGenesisValidator(founder, stakeAmt);

        Serial0.printf("[Genesis] Founder %s: %u liquid, %u staked\n",
                      founder.toShortStr().c_str(), getBalance(founder), stakeAmt);
    }

    void addGenesisNode(const NodeID& node) {
        if (getBalance(node) > 0) return;
        setBalance(node, GENESIS_PER_NODE);
        staking.totalSupply += GENESIS_PER_NODE;

        uint32_t stakeAmt = GENESIS_PER_NODE / 2;
        uint32_t bal = getBalance(node);
        bal -= stakeAmt;
        setBalance(node, bal);
        staking.addGenesisValidator(node, stakeAmt);
        staking.runElection(0);

        Serial0.printf("[Genesis] Node %s: %u liquid, %u staked\n",
                      node.toShortStr().c_str(), getBalance(node), stakeAmt);
    }


    uint32_t getBalance(const NodeID& owner) const {
        for (int i = 0; i < accountCount; i++)
            if (accounts[i].used && accounts[i].owner == owner)
                return accounts[i].balance;
        return 0;
    }

    bool setBalance(const NodeID& owner, uint32_t bal) {
        for (int i = 0; i < accountCount; i++) {
            if (accounts[i].used && accounts[i].owner == owner) {
                accounts[i].balance = bal;
                return true;
            }
        }
        if (accountCount < MAX_ACCOUNTS) {
            accounts[accountCount++] = { owner, bal, 0, 0, true };
            return true;
        }
        return false;
    }

    uint32_t getNonce(const NodeID& owner) const {
        for (int i = 0; i < accountCount; i++)
            if (accounts[i].used && accounts[i].owner == owner)
                return accounts[i].nonce;
        return 0;
    }

    void incrementNonce(const NodeID& sender) {
        for (int i = 0; i < accountCount; i++) {
            if (accounts[i].used && accounts[i].owner == sender) {
                accounts[i].nonce++;
                return;
            }
        }
    }

    bool transfer(const NodeID& from, const NodeID& to, uint32_t amount) {
        uint32_t fromBal = getBalance(from);
        if (fromBal < amount) return false;
        setBalance(from, fromBal - amount);
        setBalance(to, getBalance(to) + amount);
        return true;
    }


    uint32_t calcFee(const Transaction& tx, uint32_t gasUsed = 0) const {
        switch (tx.type) {
            case TxType::CALL:    return gasUsed * GAS_PRICE;
            case TxType::DEPLOY: {
                auto* e = g_deployCache.find(tx.name);
                return e ? e->codeLen * GAS_PRICE : TX_BASE_FEE;
            }
            case TxType::FAUCET:  return FAUCET_FEE;
            default:              return TX_BASE_FEE;
        }
    }

    bool canAffordFee(const NodeID& sender, TxType type) const {
        uint32_t bal = getBalance(sender);
        if (type == TxType::FAUCET) return true;
        return bal >= TX_BASE_FEE;    }

    bool chargeFee(const NodeID& sender, uint32_t fee) {
        if (fee == 0) return true;
        uint32_t bal = getBalance(sender);
        if (bal < fee) {
            fee = bal;
        }
        setBalance(sender, bal - fee);
        blockFees += fee;
        return true;
    }


    uint8_t faucet(const NodeID& requester) {
        AccountBalance* acct = nullptr;
        for (int i = 0; i < accountCount; i++) {
            if (accounts[i].used && accounts[i].owner == requester) {
                acct = &accounts[i];
                break;
            }
        }
        if (!acct) {
            if (accountCount >= MAX_ACCOUNTS) return 2;
            acct = &accounts[accountCount++];
            acct->owner = requester;
            acct->balance = 0;
            acct->nonce = 0;
            acct->lastFaucet = 0;
            acct->used = true;
        }
        if (acct->lastFaucet > 0 && millis() - acct->lastFaucet < FAUCET_COOLDOWN)
            return 1;
        acct->balance += FAUCET_AMOUNT;
        acct->lastFaucet = millis();
        staking.totalSupply += FAUCET_AMOUNT;
        Serial0.printf("[Faucet] %u tokens -> %s (balance: %u)\n",
                      FAUCET_AMOUNT, requester.toShortStr().c_str(), acct->balance);
        return 0;
    }


    Contract* findContract(const char* name) {
        for (int i = 0; i < contractCount; i++)
            if (contracts[i].active && strcmp(contracts[i].name, name) == 0)
                return &contracts[i];
        return nullptr;
    }

    Contract* deployContract(const char* name, const uint8_t* code,
                             uint16_t codeLen, const NodeID& deployer) {
        if (contractCount >= MAX_CONTRACTS) return nullptr;
        if (findContract(name)) return nullptr;
        Contract& c = contracts[contractCount++];
        c.init(name, code, codeLen, deployer, true);        Serial0.printf("[Deploy] '%s' at %s by %s (%d bytes)\n",
                      name, c.addr.toHex().c_str(),
                      deployer.toShortStr().c_str(), codeLen);
        return &c;
    }


    struct TxResult {
        bool      success;
        bool      needsCode;        MVMStatus vmStatus;
        uint32_t  gasUsed;
        uint32_t  fee;
        String    message;
    };

    TxResult executeTx(const Transaction& tx) {
        TxResult r = { false, false, VM_RUNNING, 0, 0, "" };

        if (tx.type != TxType::FAUCET && !canAffordFee(tx.sender, tx.type)) {
            r.message = "Insufficient balance for fee (need >= " +
                        String(TX_BASE_FEE) + " tokens)";
            return r;
        }

        switch (tx.type) {

        case TxType::DEPLOY: {
            auto* entry = g_deployCache.find(tx.name);
            if (!entry) {
                r.message = String("No deploy data for '") + tx.name + "'";
                break;
            }
            r.fee = entry->codeLen * GAS_PRICE;
            uint32_t bal = getBalance(tx.sender);
            if (bal < r.fee) { r.message = "Can't afford deploy fee (" + String(r.fee) + ")"; break; }

            if (entry->hasCode) {
                Contract* c = deployContract(tx.name, entry->code,
                                              entry->codeLen, tx.sender);
                if (c) {
                    c->host = tx.sender;
                    c->hasCode = true;
                    c->codeHash = entry->codeHash;
                    chargeFee(tx.sender, r.fee);
                    r.success = true;
                    r.message = String("Deployed '") + tx.name + "' hosted locally (" +
                                String(entry->codeLen) + "B, fee:" + String(r.fee) + ")";
                } else {
                    r.message = "Deploy failed (full or duplicate)";
                    r.fee = 0;
                }
            } else {
                if (findContract(tx.name) != nullptr) {
                    r.message = String("Contract '") + tx.name + "' already exists";
                    r.fee = 0;
                    break;
                }
                if (contractCount >= MAX_CONTRACTS) {
                    r.message = "Contract limit reached";
                    r.fee = 0;
                    break;
                }
                Contract& c = contracts[contractCount++];
                c.initRemote(tx.name, tx.sender, tx.sender, entry->codeHash);
                c.codeLen = entry->codeLen;
                chargeFee(tx.sender, r.fee);
                r.success = true;
                r.message = String("Registered '") + tx.name + "' hosted by " +
                            tx.sender.toShortStr() + " (" +
                            String(entry->codeLen) + "B, fee:" + String(r.fee) + ")";
            }
            break;
        }

        case TxType::CALL: {
            Contract* c = findContract(tx.target);
            if (!c) { r.message = String("Not found: ") + tx.target; break; }
            if (!c->hasCode) {
                r.message = String("No bytecode for '") + tx.target +
                            "' (hosted by " + c->host.toShortStr() + ")";
                r.needsCode = true;
                break;
            }
            MVMContext ctx;
            ctx.caller = tx.sender;
            ctx.argCount = tx.argCount;
            memcpy(ctx.args, tx.args, tx.argCount * 4);
            ctx.callValue = tx.value;

            MVMStatus st = vm.execute(*c, ctx);
            r.vmStatus = st;
            r.gasUsed = vm.gasUsed();
            r.fee = r.gasUsed * GAS_PRICE;
            chargeFee(tx.sender, r.fee);

            r.success = (st == VM_HALTED);
            r.message = String(c->name) + ": " + statusName(st) +
                        " (gas:" + String(r.gasUsed) + " fee:" + String(r.fee) + ")";
            break;
        }

        case TxType::TRANSFER: {
            r.fee = TX_BASE_FEE;
            uint32_t totalCost = tx.value + r.fee;
            uint32_t bal = getBalance(tx.sender);
            if (bal < totalCost) {
                r.message = "Need " + String(totalCost) + " (amount+fee), have " + String(bal);
                r.fee = 0;
                break;
            }
            chargeFee(tx.sender, r.fee);
            bool ok = transfer(tx.sender, tx.voteTarget, tx.value);
            r.success = ok;
            r.message = ok ? "Sent " + String(tx.value) + " tokens (fee:" + String(r.fee) + ")"
                           : "Transfer failed";
            break;
        }

        case TxType::STAKE: {
            r.fee = TX_BASE_FEE;
            uint32_t bal = getBalance(tx.sender);
            if (bal < tx.value + r.fee) {
                r.message = "Need " + String(tx.value + r.fee) + " (stake+fee), have " + String(bal);
                r.fee = 0;
                break;
            }
            chargeFee(tx.sender, r.fee);
            bal = getBalance(tx.sender);            uint8_t res = staking.stake(tx.sender, tx.value, bal);
            if (res == 0) {
                setBalance(tx.sender, bal);
                r.success = true;
                r.message = "Staked " + String(tx.value) + " (fee:" + String(r.fee) + ")";
            } else {
                const char* reasons[] = {"ok","Insufficient balance",
                    "Below minimum stake","Candidate slots full"};
                r.message = reasons[res];
            }
            break;
        }

        case TxType::UNSTAKE: {
            r.fee = TX_BASE_FEE;
            chargeFee(tx.sender, r.fee);
            uint8_t res = staking.unstake(tx.sender, tx.value, chainLen);
            if (res == 0) {
                r.success = true;
                r.message = "Unstaking " + String(tx.value) +
                            " (claimable in " + String(UNSTAKE_COOLDOWN) + " blocks, fee:" +
                            String(r.fee) + ")";
            } else {
                r.message = (res==1) ? "Not staking" : "Amount exceeds stake";
            }
            break;
        }

        case TxType::VOTE: {
            r.fee = TX_BASE_FEE;
            chargeFee(tx.sender, r.fee);
            uint8_t res = staking.vote(tx.sender, tx.voteTarget);
            if (res == 0) {
                r.success = true;
                r.message = "Voted for " + tx.voteTarget.toShortStr() +
                            " (fee:" + String(r.fee) + ")";
            } else {
                const char* reasons[] = {"ok","Must stake to vote",
                    "Candidate not found","Already voted","Vote slots full"};
                r.message = reasons[res];
            }
            break;
        }

        case TxType::UNVOTE: {
            r.fee = TX_BASE_FEE;
            chargeFee(tx.sender, r.fee);
            r.success = true;
            r.message = "Vote removed (fee:" + String(r.fee) + ")";
            break;
        }

        case TxType::CLAIM: {
            r.fee = TX_BASE_FEE;
            chargeFee(tx.sender, r.fee);
            uint32_t claimed = staking.claim(tx.sender, chainLen);
            if (claimed > 0) {
                setBalance(tx.sender, getBalance(tx.sender) + claimed);
                r.success = true;
                r.message = "Claimed " + String(claimed) + " tokens (fee:" + String(r.fee) + ")";
            } else {
                r.message = "Nothing to claim (cooldown not finished)";
            }
            break;
        }

        case TxType::FAUCET: {
            r.fee = 0;            uint8_t res = faucet(tx.sender);
            if (res == 0) {
                r.success = true;
                r.message = "Received " + String(FAUCET_AMOUNT) + " tokens (free)";
            } else {
                r.message = (res==1) ? "Faucet cooldown active" : "Account limit reached";
            }
            break;
        }

        case TxType::DATA: {
            r.fee = TX_BASE_FEE;
            chargeFee(tx.sender, r.fee);
            r.success = true;
            r.message = "Data stored (fee:" + String(r.fee) + ")";
            break;
        }

        default:
            r.message = "Unknown tx type";
        }

        if (r.success) incrementNonce(tx.sender);
        return r;
    }


    bool createBlock(const NodeID& validator, uint32_t slot,
                     Transaction* txns, uint8_t txCount) {
        if (chainLen >= MAX_BLOCKS) return false;
        if (txCount > MAX_TXN_PER_BLOCK) txCount = MAX_TXN_PER_BLOCK;

        uint32_t logicalIdx = chainOffset + chainLen;
        uint32_t epoch = logicalIdx / EPOCH_LENGTH;
        blockFees = 0;
        Block& blk = chain[chainLen];
        memset(&blk, 0, sizeof(Block));
        blk.header.index = logicalIdx;
        blk.header.timestamp = millis();
        blk.header.prevHash = chain[chainLen-1].blockHash;
        blk.header.validator = validator;
        blk.header.slot = slot;
        blk.header.epoch = epoch;
        blk.header.txCount = 0;

        uint32_t totalFees = 0;

        for (int i = 0; i < txCount; i++) {
            TxResult r = executeTx(txns[i]);
            if (r.success) {
                blk.txns[blk.header.txCount++] = txns[i];
                totalFees += r.fee;
                Serial0.printf("  TX[%d] OK: %s\n", i, r.message.c_str());
                for (int e = 0; e < vm.eventCount(); e++)
                    Serial0.printf("    Event[%d] = %u\n", e, vm.event(e).value);
            } else {
                totalFees += r.fee;
                Serial0.printf("  TX[%d] FAIL: %s\n", i, r.message.c_str());
            }
        }

        uint32_t totalReward = BLOCK_REWARD + totalFees;
        blk.header.reward = totalReward;

        uint32_t valBal = getBalance(validator);
        setBalance(validator, valBal + BLOCK_REWARD);        setBalance(validator, getBalance(validator) + blockFees);
        staking.totalSupply += BLOCK_REWARD;
        blk.header.stateRoot = computeStateRoot();
        blk.computeHash();
        chainLen++;

        Serial0.printf("[Block] #%d e=%d by %s | %d txns | reward=%d + fees=%d = %d tokens | %s\n",
                      blk.header.index, epoch,
                      validator.toShortStr().c_str(),
                      blk.header.txCount, BLOCK_REWARD, totalFees, totalReward,
                      blk.blockHash.toShort().c_str());

        if (height() % EPOCH_LENGTH == 0) {
            uint32_t newEpoch = height() / EPOCH_LENGTH;
            Serial0.printf("\n==== EPOCH TRANSITION: %d -> %d ====\n",
                          newEpoch - 1, newEpoch);
            staking.runElection(newEpoch);
        }

        if (chainLen >= PRUNE_TRIGGER) {
            pruneChain();
        }

        return true;
    }


    bool validateBlock(const Block& blk) const {
        if (blk.header.index != height()) return false;
        if (chainLen > 0 && blk.header.prevHash != chain[chainLen-1].blockHash)
            return false;
        Block copy = blk;
        copy.computeHash();
        if (copy.blockHash != blk.blockHash) return false;
        return true;
    }

    bool addBlock(const Block& blk) {
        if (!validateBlock(blk)) return false;
        if (chainLen >= MAX_BLOCKS) return false;

        chain[chainLen] = blk;
        blockFees = 0;
        for (int i = 0; i < blk.header.txCount; i++)
            executeTx(blk.txns[i]);

        uint32_t valBal = getBalance(blk.header.validator);
        setBalance(blk.header.validator, valBal + BLOCK_REWARD + blockFees);
        staking.totalSupply += BLOCK_REWARD;
        chainLen++;

        if (height() % EPOCH_LENGTH == 0)
            staking.runElection(height() / EPOCH_LENGTH);

        if (chainLen >= PRUNE_TRIGGER)
            pruneChain();

        return true;
    }


    uint8_t applyNetworkBlock(const Block& blk) {
        uint32_t expectedIdx = height();

        if (blk.header.index != expectedIdx) {
            Serial0.printf("[Validate] REJECT #%d: expected #%d\n",
                          blk.header.index, expectedIdx);
            return 1;
        }

        Hash256 ourTip = (chainLen > 0) ? chain[chainLen-1].blockHash : ZERO_HASH;
        if (blk.header.prevHash != ourTip) {
            Serial0.printf("[Validate] REJECT #%d: prevHash mismatch\n",
                          blk.header.index);
            Serial0.printf("  Expected: %s\n", ourTip.toShort().c_str());
            Serial0.printf("  Got:      %s\n", blk.header.prevHash.toShort().c_str());
            return 2;
        }

        Block copy = blk;
        copy.computeHash();
        if (copy.blockHash != blk.blockHash) {
            Serial0.printf("[Validate] REJECT #%d: hash mismatch\n",
                          blk.header.index);
            return 3;
        }

        if (staking.activeCount > 1) {
            if (!staking.isActiveValidator(blk.header.validator)) {
                Serial0.printf("[Validate] REJECT #%d: %s is NOT an active validator\n",
                              blk.header.index,
                              blk.header.validator.toShortStr().c_str());
                Serial0.print("  Active set: ");
                for (int i = 0; i < staking.activeCount; i++)
                    Serial0.print(staking.activeSet[i].toShortStr() + " ");
                Serial0.println();
                return 5;
            }

            NodeID expectedProducer = staking.getSlotProducer(blk.header.slot);
            if (expectedProducer != blk.header.validator) {
                Serial0.printf("[Validate] REJECT #%d: slot %d belongs to %s, not %s\n",
                              blk.header.index, blk.header.slot,
                              expectedProducer.toShortStr().c_str(),
                              blk.header.validator.toShortStr().c_str());
                return 6;
            }

            Serial0.printf("[Validate] DPoS OK: slot %d -> %s (validator %d/%d)\n",
                          blk.header.slot,
                          blk.header.validator.toShortStr().c_str(),
                          staking.getValidatorIndex(blk.header.validator) + 1,
                          staking.activeCount);
        }

        if (chainLen >= MAX_BLOCKS) {
            Serial0.println("[Validate] Chain full, pruning first...");
            pruneChain();
            if (chainLen >= MAX_BLOCKS) return 4;
        }

        chain[chainLen] = blk;
        blockFees = 0;

        Serial0.printf("[Validate] ACCEPTED #%d from %s (%d txns, slot %d)\n",
                      blk.header.index,
                      blk.header.validator.toShortStr().c_str(),
                      blk.header.txCount,
                      blk.header.slot);

        for (int i = 0; i < blk.header.txCount; i++) {
            TxResult r = executeTx(blk.txns[i]);
            Serial0.printf("  TX[%d] %s: %s\n", i,
                          r.success ? "OK" : "FAIL", r.message.c_str());
            for (int e = 0; e < vm.eventCount(); e++)
                Serial0.printf("    Event[%d] = %u\n", e, vm.event(e).value);
        }

        uint32_t valBal = getBalance(blk.header.validator);
        setBalance(blk.header.validator, valBal + BLOCK_REWARD);
        setBalance(blk.header.validator, getBalance(blk.header.validator) + blockFees);
        staking.totalSupply += BLOCK_REWARD;

        chainLen++;

        if (height() % EPOCH_LENGTH == 0) {
            uint32_t newEpoch = height() / EPOCH_LENGTH;
            Serial0.printf("\n==== EPOCH TRANSITION: %d -> %d ====\n",
                          newEpoch - 1, newEpoch);
            staking.runElection(newEpoch);
        }

        if (chainLen >= PRUNE_TRIGGER)
            pruneChain();

        return 0;
    }


    bool replaceGenesis(const Block& peerGenesis) {
        if (peerGenesis.header.index != 0) return false;

        if (chainLen > 0 && chain[0].blockHash == peerGenesis.blockHash)
            return true;

        Serial0.println("[Sync] Replacing genesis with peer's genesis");

        memset(accounts, 0, sizeof(accounts));
        accountCount = 0;
        memset(contracts, 0, sizeof(contracts));
        contractCount = 0;
        mempoolSize = 0;
        staking = StakingEngine();
        chainLen = 0;
        chainOffset = 0;
        checkpointCount = 0;

        chain[0] = peerGenesis;
        chainLen = 1;

        Serial0.printf("[Sync] New genesis hash: %s\n",
                      peerGenesis.blockHash.toShort().c_str());
        return true;
    }


    Hash256 computeStateRoot() const {
        uint8_t buf[512];
        size_t off = 0;
        for (int i = 0; i < contractCount && off < 400; i++) {
            if (!contracts[i].active) continue;
            Hash256 ch = contracts[i].stateHash();
            memcpy(buf + off, ch.bytes, HASH_SIZE); off += HASH_SIZE;
        }
        for (int i = 0; i < accountCount && off < 460; i++) {
            if (!accounts[i].used) continue;
            memcpy(buf + off, accounts[i].owner.id, 6); off += 6;
            memcpy(buf + off, &accounts[i].balance, 4); off += 4;
        }
        memcpy(buf + off, &staking.totalStaked, 4); off += 4;
        memcpy(buf + off, &staking.activeCount, 1); off += 1;
        return (off > 0) ? sha256(buf, off) : ZERO_HASH;
    }


    bool addToMempool(const Transaction& tx) {
        if (mempoolSize >= MAX_PENDING_TX) return false;
        for (int i = 0; i < mempoolSize; i++)
            if (mempool[i].sender == tx.sender && mempool[i].nonce == tx.nonce)
                return false;
        mempool[mempoolSize++] = tx;
        return true;
    }

    uint8_t drainMempool(Transaction* out, uint8_t maxTx) {
        uint8_t count = min(mempoolSize, maxTx);
        memcpy(out, mempool, count * sizeof(Transaction));
        if (count < mempoolSize)
            memmove(mempool, mempool + count,
                    (mempoolSize - count) * sizeof(Transaction));
        mempoolSize -= count;
        return count;
    }


    Hash256 computeBlockMerkle(uint32_t from, uint32_t to) const {
        if (from >= to) return ZERO_HASH;
        uint32_t count = to - from;

        Hash256 leaves[MAX_BLOCKS];
        for (uint32_t i = 0; i < count; i++)
            leaves[i] = chain[from + i].blockHash;

        uint32_t n = count;
        while (n > 1) {
            uint32_t half = (n + 1) / 2;
            for (uint32_t i = 0; i < n / 2; i++) {
                uint8_t pair[HASH_SIZE * 2];
                memcpy(pair, leaves[i*2].bytes, HASH_SIZE);
                memcpy(pair + HASH_SIZE, leaves[i*2+1].bytes, HASH_SIZE);
                leaves[i] = sha256(pair, HASH_SIZE * 2);
            }
            if (n % 2 == 1) {
                leaves[half - 1] = leaves[n - 1];
            }
            n = half;
        }
        return leaves[0];
    }

    void pruneChain() {
        uint32_t toPrune = chainLen - PRUNE_KEEP;
        if (toPrune == 0) return;

        Serial0.printf("\n==== PRUNING: deleting %d blocks [%d..%d], keeping %d ====\n",
                      toPrune, chainOffset, chainOffset + toPrune - 1, PRUNE_KEEP);

        Hash256 merkle = computeBlockMerkle(0, toPrune);

        Hash256 prevCkpt = ZERO_HASH;
        if (checkpointCount > 0)
            prevCkpt = checkpoints[checkpointCount - 1].hash();

        Checkpoint ckpt;
        ckpt.fromBlock     = chainOffset;
        ckpt.toBlock       = chainOffset + toPrune - 1;
        ckpt.lastBlockHash = chain[toPrune - 1].blockHash;
        ckpt.stateRoot     = computeStateRoot();
        ckpt.chainMerkle   = merkle;
        ckpt.prevCheckpoint = prevCkpt;
        ckpt.timestamp     = millis();
        ckpt.used          = true;

        if (checkpointCount < MAX_CHECKPOINTS) {
            checkpoints[checkpointCount++] = ckpt;
        } else {
            memmove(checkpoints, checkpoints + 1,
                    (MAX_CHECKPOINTS - 1) * sizeof(Checkpoint));
            checkpoints[MAX_CHECKPOINTS - 1] = ckpt;
        }

        Serial0.printf("  Checkpoint: blocks [%d..%d]\n", ckpt.fromBlock, ckpt.toBlock);
        Serial0.printf("  Last block hash:  %s\n", ckpt.lastBlockHash.toShort().c_str());
        Serial0.printf("  Merkle root:      %s\n", ckpt.chainMerkle.toShort().c_str());
        Serial0.printf("  State root:       %s\n", ckpt.stateRoot.toShort().c_str());

        if (chain[toPrune].header.prevHash == ckpt.lastBlockHash) {
            Serial0.println("  Link verified: chain continues from checkpoint");
        } else {
            Serial0.println("  WARNING: link mismatch!");
        }

        memmove(chain, chain + toPrune, PRUNE_KEEP * sizeof(Block));
        chainOffset += toPrune;
        chainLen = PRUNE_KEEP;

        Serial0.printf("  Chain now: blocks [%d..%d] (%d in memory)\n",
                      chainOffset, chainOffset + chainLen - 1, chainLen);
        Serial0.printf("  Total logical height: %d\n", height());
        Serial0.printf("  Checkpoints: %d (covering blocks 0..%d)\n",
                      checkpointCount, chainOffset - 1);
        Serial0.println("==== PRUNE COMPLETE ====\n");
    }


    void printCheckpoints() const {
        Serial0.println("\n====== Cryptographic Checkpoints ======");
        if (checkpointCount == 0) {
            Serial0.println("  No checkpoints yet (no pruning performed)");
        }
        for (int i = 0; i < checkpointCount; i++) {
            checkpoints[i].print();
        }
        Serial0.printf("Blocks in memory: [%d..%d] (%d blocks)\n",
                      chainOffset, chainOffset + chainLen - 1, chainLen);
        Serial0.printf("Total logical height: %d\n", height());
        Serial0.println("=======================================\n");
    }


    const Block& lastBlock() const { return chain[chainLen-1]; }
    uint32_t height() const { return chainOffset + chainLen; }    uint32_t currentEpoch() const { return height() / EPOCH_LENGTH; }
    uint32_t blocksInMemory() const { return chainLen; }

    void printChain() const {
        Serial0.println("\n====== MirkoNet Chain ======");
        if (chainOffset > 0) {
            Serial0.printf("Pruned: blocks 0..%d (see 'checkpoints')\n", chainOffset - 1);
        }
        for (uint32_t i = 0; i < chainLen; i++) {
            const Block& b = chain[i];
            Serial0.printf("#%d e=%d slot=%d by=%s tx=%d rwd=%d %s\n",
                          b.header.index, b.header.epoch, b.header.slot,
                          b.header.validator.toShortStr().c_str(),
                          b.header.txCount, b.header.reward,
                          b.blockHash.toShort().c_str());
        }
        Serial0.printf("In memory: %d blocks [%d..%d] | Logical height: %d | Epoch: %d\n",
                      chainLen, chainOffset, chainOffset + chainLen - 1,
                      height(), currentEpoch());
        Serial0.printf("Checkpoints: %d\n", checkpointCount);
        Serial0.println("============================\n");
    }

    void printAccounts() const {
        Serial0.println("\n====== Accounts ======");
        for (int i = 0; i < accountCount; i++) {
            if (!accounts[i].used) continue;
            const StakeInfo* s = staking.findStake(accounts[i].owner);
            Serial0.printf("  %s | bal=%u",
                          accounts[i].owner.toString().c_str(),
                          accounts[i].balance);
            if (s && s->stakedAmount > 0)
                Serial0.printf(" staked=%u", s->stakedAmount);
            if (s && s->unstakingAmount > 0)
                Serial0.printf(" unstaking=%u", s->unstakingAmount);
            if (staking.isActiveValidator(accounts[i].owner))
                Serial0.print(" *VAL");
            Serial0.println();
        }
        Serial0.println("======================\n");
    }
};