#pragma once
#include "types.h"
#include "mvm.h"
#include "staking.h"
#include "code_store.h"

// LED feedback callback for validation stages
// Set by mirkonet.ino to wire blockchain validation events to NeoPixel LED.
// Events:
//   0=state root verified, 1=rejection (param=code), 2=tx success (param=txType),
//   3=tx fail (param=txType), 4=validate index OK, 5=validate hash OK,
//   6=validate PoS OK, 7=validate slot OK, 8=re-executing block,
//   9=pruning, 10=epoch election
typedef void (*BlockchainLedCallback)(uint8_t event, uint8_t param);
static BlockchainLedCallback g_blockchainLedCb = nullptr;


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

    CompactHeader  finalized[MAX_FINALIZED];
    uint16_t       finalizedCount = 0;

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
        gen.header.validator = founder;
        gen.header.txCount = 0;
        gen.header.slot = 0;
        gen.header.epoch = 0;
        gen.header.reward = 0;

        // Set up founder account + staking BEFORE computing stateRoot
        setBalance(founder, GENESIS_PER_NODE);
        staking.totalSupply = GENESIS_PER_NODE;

        uint32_t stakeAmt = GENESIS_PER_NODE / 2;
        setBalance(founder, GENESIS_PER_NODE - stakeAmt);
        staking.addGenesisValidator(founder, stakeAmt);

        // Genesis block commits to the founder's initial state
        gen.header.stateRoot = computeStateRoot();
        gen.computeHash();
        chainLen = 1;

        Serial0.printf("[Genesis] Founder %s: %u liquid, %u staked, root=%s\n",
                      founder.toShortStr().c_str(), getBalance(founder), stakeAmt,
                      gen.header.stateRoot.toShort().c_str());
    }

    // Adopt a peer's genesis block: wipe local state and rebuild from
    // the genesis founder. Called when a joining node discovers an
    // existing network. selfId is added as a genesis node if different
    // from founder.
    void adoptGenesis(const Block& genesis, const NodeID& selfId) {
        resetState();
        chain[0] = genesis;
        chainLen = 1;

        NodeID founder = genesis.header.validator;
        setBalance(founder, GENESIS_PER_NODE);
        staking.totalSupply = GENESIS_PER_NODE;

        uint32_t stakeAmt = GENESIS_PER_NODE / 2;
        setBalance(founder, GENESIS_PER_NODE - stakeAmt);
        staking.addGenesisValidator(founder, stakeAmt);

        if (!(selfId == founder)) {
            addGenesisNode(selfId);
        }

        Serial0.printf("[Genesis] Adopted genesis from %s, hash=%s\n",
                      founder.toShortStr().c_str(),
                      genesis.blockHash.toShort().c_str());
    }

    void addGenesisNode(const NodeID& node) {
        if (getBalance(node) > 0) return;
        setBalance(node, GENESIS_PER_NODE);
        staking.totalSupply += GENESIS_PER_NODE;

        uint32_t stakeAmt = GENESIS_PER_NODE / 2;
        setBalance(node, GENESIS_PER_NODE - stakeAmt);
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
        uint32_t gp = tx.gasPrice > 0 ? tx.gasPrice : DEFAULT_GAS_PRICE;
        switch (tx.type) {
            case TxType::CALL:    return gasUsed * gp;
            case TxType::DEPLOY: {
                auto* e = g_deployCache.find(tx.data);
                return e ? (e->codeLen * DEPLOY_GAS_PER_BYTE) * gp / DEPLOY_GAS_PER_BYTE : gp;
            }
            case TxType::FAUCET:  return FAUCET_FEE;
            default:              return gp;
        }
    }

    bool canAffordFee(const NodeID& sender, TxType type, uint32_t gasPrice = DEFAULT_GAS_PRICE) const {
        uint32_t bal = getBalance(sender);
        if (type == TxType::FAUCET) return true;
        return bal >= gasPrice;
    }

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
        uint32_t gp = tx.gasPrice > 0 ? tx.gasPrice : DEFAULT_GAS_PRICE;

        if (tx.type != TxType::FAUCET && !canAffordFee(tx.sender, tx.type, gp)) {
            r.message = "Insufficient balance for gas (need >= " +
                        String(gp) + " tokens)";
            return r;
        }

        switch (tx.type) {

        case TxType::DEPLOY: {
            // Ethereum: to=0x0, data=contract init code
            auto* entry = g_deployCache.find(tx.data);
            if (!entry) {
                r.message = String("No deploy data for '") + tx.data + "'";
                break;
            }
            r.gasUsed = entry->codeLen * DEPLOY_GAS_PER_BYTE;
            if (tx.gasLimit > 0 && r.gasUsed > tx.gasLimit) {
                r.message = "Out of gas: need " + String(r.gasUsed) + ", limit " + String(tx.gasLimit);
                r.fee = tx.gasLimit * gp;
                chargeFee(tx.sender, r.fee);
                break;
            }
            r.fee = r.gasUsed * gp;
            uint32_t bal = getBalance(tx.sender);
            if (bal < r.fee) { r.message = "Can't afford deploy gas (" + String(r.fee) + ")"; break; }

            if (entry->hasCode) {
                Contract* c = deployContract(tx.data, entry->code,
                                              entry->codeLen, tx.sender);
                if (c) {
                    c->host = tx.sender;
                    c->hasCode = true;
                    c->codeHash = entry->codeHash;
                    CodeStore::save(tx.data, entry->code, entry->codeLen);
                    chargeFee(tx.sender, r.fee);
                    r.success = true;
                    r.message = String("Deployed '") + tx.data + "' hosted locally (" +
                                String(entry->codeLen) + "B, gas:" + String(r.gasUsed) +
                                " fee:" + String(r.fee) + ")";
                } else {
                    r.message = "Deploy failed (full or duplicate)";
                    r.fee = 0;
                }
            } else {
                if (findContract(tx.data) != nullptr) {
                    r.message = String("Contract '") + tx.data + "' already exists";
                    r.fee = 0;
                    break;
                }
                if (contractCount >= MAX_CONTRACTS) {
                    r.message = "Contract limit reached";
                    r.fee = 0;
                    break;
                }
                Contract& c = contracts[contractCount++];
                c.initRemote(tx.data, tx.sender, tx.sender, entry->codeHash);
                c.codeLen = entry->codeLen;
                chargeFee(tx.sender, r.fee);
                r.success = true;
                r.message = String("Registered '") + tx.data + "' hosted by " +
                            tx.sender.toShortStr() + " (" +
                            String(entry->codeLen) + "B, gas:" + String(r.gasUsed) +
                            " fee:" + String(r.fee) + ")";
            }
            break;
        }

        case TxType::CALL: {
            // Ethereum: to=contract address, data=calldata
            Contract* c = findContract(tx.data);
            if (!c) { r.message = String("Not found: ") + tx.data; break; }
            if (!c->hasCode) {
                r.message = String("No bytecode for '") + tx.data +
                            "' (hosted by " + c->host.toShortStr() + ")";
                r.needsCode = true;
                break;
            }
            MVMContext ctx;
            ctx.caller = tx.sender;
            ctx.argCount = tx.argCount;
            memcpy(ctx.args, tx.args, tx.argCount * 4);
            ctx.callValue = tx.value;
            ctx.blockNumber = height();
            ctx.blockTimestamp = millis();
            ctx.origin = tx.sender;

            // Set up contract resolver for cross-contract calls
            g_contractResolver = [](const char* name) -> Contract* {
                // This lambda is stateless; it relies on the global g_chain
                // being accessible. Since blockchain.h defines it inline,
                // we use extern to access the global instance.
                extern BlockchainState g_chain;
                if (!name) return nullptr;
                // Special index-based lookup: "\x01N" returns contract at index N
                if (name[0] == '\x01') {
                    int idx = atoi(name + 1);
                    if (idx >= 0 && idx < g_chain.contractCount)
                        return &g_chain.contracts[idx];
                    return nullptr;
                }
                return g_chain.findContract(name);
            };

            MVMStatus st = vm.execute(*c, ctx);
            r.vmStatus = st;
            r.gasUsed = vm.gasUsed();

            if (tx.gasLimit > 0 && r.gasUsed > tx.gasLimit) {
                r.fee = tx.gasLimit * gp;
                chargeFee(tx.sender, r.fee);
                r.success = false;
                r.message = String(c->name) + ": OUT_OF_GAS (used:" + String(r.gasUsed) +
                            " limit:" + String(tx.gasLimit) + " fee:" + String(r.fee) + ")";
                break;
            }

            r.fee = r.gasUsed * gp;
            chargeFee(tx.sender, r.fee);

            r.success = (st == VM_HALTED);
            r.message = String(c->name) + ": " + statusName(st) +
                        " (gas:" + String(r.gasUsed) + " price:" + String(gp) +
                        " fee:" + String(r.fee) + ")";
            break;
        }

        case TxType::TRANSFER: {
            // Ethereum: to=recipient, value=amount, gasLimit=21000
            r.fee = gp;
            uint32_t totalCost = tx.value + r.fee;
            uint32_t bal = getBalance(tx.sender);
            if (bal < totalCost) {
                r.message = "Need " + String(totalCost) + " (value+gas), have " + String(bal);
                r.fee = 0;
                break;
            }
            chargeFee(tx.sender, r.fee);
            bool ok = transfer(tx.sender, tx.to, tx.value);
            r.success = ok;
            r.message = ok ? "Sent " + String(tx.value) + " to " + tx.to.toShortStr() +
                             " (gas:" + String(gp) + ")"
                           : "Transfer failed";
            break;
        }

        case TxType::STAKE: {
            r.fee = gp;
            uint32_t bal = getBalance(tx.sender);
            if (bal < tx.value + r.fee) {
                r.message = "Need " + String(tx.value + r.fee) + " (stake+gas), have " + String(bal);
                r.fee = 0;
                break;
            }
            chargeFee(tx.sender, r.fee);
            bal = getBalance(tx.sender);
            uint8_t res = staking.stake(tx.sender, tx.value, bal);
            if (res == 0) {
                setBalance(tx.sender, bal);
                r.success = true;
                r.message = "Staked " + String(tx.value) + " (gas:" + String(r.fee) + ")";
            } else {
                const char* reasons[] = {"ok","Insufficient balance",
                    "Below minimum stake","Candidate slots full"};
                r.message = reasons[res];
            }
            break;
        }

        case TxType::UNSTAKE: {
            r.fee = gp;
            chargeFee(tx.sender, r.fee);
            uint8_t res = staking.unstake(tx.sender, tx.value, chainLen);
            if (res == 0) {
                r.success = true;
                r.message = "Unstaking " + String(tx.value) +
                            " (claimable in " + String(UNSTAKE_COOLDOWN) + " blocks, gas:" +
                            String(r.fee) + ")";
            } else {
                r.message = (res==1) ? "Not staking" : "Amount exceeds stake";
            }
            break;
        }

        case TxType::CLAIM: {
            r.fee = gp;
            chargeFee(tx.sender, r.fee);
            uint32_t claimed = staking.claim(tx.sender, chainLen);
            if (claimed > 0) {
                setBalance(tx.sender, getBalance(tx.sender) + claimed);
                r.success = true;
                r.message = "Claimed " + String(claimed) + " tokens (gas:" + String(r.fee) + ")";
            } else {
                r.message = "Nothing to claim (cooldown not finished)";
            }
            break;
        }

        case TxType::FAUCET: {
            // Ethereum: no equivalent (gasless meta-tx concept)
            r.fee = 0;
            uint8_t res = faucet(tx.sender);
            if (res == 0) {
                r.success = true;
                r.message = "Received " + String(FAUCET_AMOUNT) + " tokens (free)";
            } else {
                r.message = (res==1) ? "Faucet cooldown active" : "Account limit reached";
            }
            break;
        }

        case TxType::DATA: {
            r.fee = gp;
            chargeFee(tx.sender, r.fee);
            r.success = true;
            r.message = "Data stored (gas:" + String(r.fee) + ")";
            break;
        }

        default:
            r.message = "Unknown tx type";
        }

        if (r.success) {
            incrementNonce(tx.sender);
            // LED: green flash for successful tx
            if (g_blockchainLedCb) g_blockchainLedCb(2, (uint8_t)tx.type);
        } else {
            // LED: red flash for failed tx
            if (g_blockchainLedCb) g_blockchainLedCb(3, (uint8_t)tx.type);
        }
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

        for (int i = 0; i < txCount; i++) {
            // Snapshot state before execution so we can rollback if tx fails.
            // Failed txs may charge fees (modifying sender balance + blockFees)
            // but aren't included in the block — validators would never see them,
            // causing stateRoot mismatch on re-execution.
            uint32_t savedBlockFees = blockFees;
            uint32_t savedSenderBal = getBalance(txns[i].sender);

            TxResult r = executeTx(txns[i]);
            if (r.success) {
                blk.txns[blk.header.txCount++] = txns[i];
                Serial0.printf("  TX[%d] OK: %s\n", i, r.message.c_str());
                for (int e = 0; e < vm.eventCount(); e++)
                    Serial0.printf("    Event[%d] = %u\n", e, vm.event(e).value);
            } else {
                // Rollback any state changes from the failed tx (fee charges)
                // so they don't pollute the state root.
                // VM already reverts contract storage/balance on failure.
                setBalance(txns[i].sender, savedSenderBal);
                blockFees = savedBlockFees;
                Serial0.printf("  TX[%d] FAIL (rolled back): %s\n", i, r.message.c_str());
            }
        }

        blk.header.reward = BLOCK_REWARD + blockFees;

        uint32_t valBal = getBalance(validator);
        setBalance(validator, valBal + BLOCK_REWARD + blockFees);
        staking.totalSupply += BLOCK_REWARD;

        // Record block production for slashing/downtime tracking
        staking.recordBlockProduced(validator, logicalIdx);

        blk.header.stateRoot = computeStateRoot();
        blk.computeHash();
        chainLen++;

        Serial0.printf("[Block] #%d e=%d by %s | %d txns | reward=%d + fees=%d = %d tokens | %s\n",
                      blk.header.index, epoch,
                      validator.toShortStr().c_str(),
                      blk.header.txCount, BLOCK_REWARD, blockFees,
                      BLOCK_REWARD + blockFees,
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

        if (height() % EPOCH_LENGTH == 0) {
            staking.runElection(height() / EPOCH_LENGTH);
            if (g_blockchainLedCb) g_blockchainLedCb(10, 0);  // epoch election
        }

        if (chainLen >= PRUNE_TRIGGER) {
            if (g_blockchainLedCb) g_blockchainLedCb(9, 0);  // pruning
            pruneChain();
        }

        return true;
    }


    uint8_t applyNetworkBlock(const Block& blk) {
        uint32_t expectedIdx = height();

        if (blk.header.index != expectedIdx) {
            Serial0.printf("[Validate] REJECT #%d: expected #%d\n",
                          blk.header.index, expectedIdx);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 1);  // reject: wrong index
            return 1;
        }
        if (g_blockchainLedCb) g_blockchainLedCb(4, 0);  // index check OK

        Hash256 ourTip = (chainLen > 0) ? chain[chainLen-1].blockHash : ZERO_HASH;
        if (blk.header.prevHash != ourTip) {
            Serial0.printf("[Validate] REJECT #%d: prevHash mismatch\n",
                          blk.header.index);
            Serial0.printf("  Expected: %s\n", ourTip.toShort().c_str());
            Serial0.printf("  Got:      %s\n", blk.header.prevHash.toShort().c_str());
            if (g_blockchainLedCb) g_blockchainLedCb(1, 2);  // reject: prevHash
            return 2;
        }

        Block copy = blk;
        copy.computeHash();
        if (copy.blockHash != blk.blockHash) {
            Serial0.printf("[Validate] REJECT #%d: hash mismatch\n",
                          blk.header.index);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 3);  // reject: bad hash
            return 3;
        }
        if (g_blockchainLedCb) g_blockchainLedCb(5, 0);  // hash check OK

        if (staking.activeCount > 1) {
            if (!staking.isActiveValidator(blk.header.validator)) {
                Serial0.printf("[Validate] REJECT #%d: %s is NOT an active validator\n",
                              blk.header.index,
                              blk.header.validator.toShortStr().c_str());
                Serial0.print("  Active set: ");
                for (int i = 0; i < staking.activeCount; i++)
                    Serial0.print(staking.activeSet[i].toShortStr() + " ");
                Serial0.println();
                if (g_blockchainLedCb) g_blockchainLedCb(1, 5);  // reject: not validator
                return 5;
            }

            NodeID expectedProducer = staking.getSlotProducer(blk.header.slot);
            if (expectedProducer != blk.header.validator) {
                Serial0.printf("[Validate] REJECT #%d: slot %d belongs to %s, not %s\n",
                              blk.header.index, blk.header.slot,
                              expectedProducer.toShortStr().c_str(),
                              blk.header.validator.toShortStr().c_str());
                if (g_blockchainLedCb) g_blockchainLedCb(1, 6);  // reject: wrong slot
                return 6;
            }

            Serial0.printf("[Validate] PoS OK: slot %d -> %s (validator %d/%d)\n",
                          blk.header.slot,
                          blk.header.validator.toShortStr().c_str(),
                          staking.getValidatorIndex(blk.header.validator) + 1,
                          staking.activeCount);
            if (g_blockchainLedCb) g_blockchainLedCb(6, 0);  // PoS check OK
        }

        if (chainLen >= MAX_BLOCKS) {
            Serial0.println("[Validate] Chain full, pruning first...");
            if (g_blockchainLedCb) g_blockchainLedCb(9, 0);  // pruning
            pruneChain();
            if (chainLen >= MAX_BLOCKS) return 4;
        }

        // --- Check if we have all bytecode needed for CALL txns ---
        for (int i = 0; i < blk.header.txCount; i++) {
            if (blk.txns[i].type == TxType::CALL) {
                Contract* c = findContract(blk.txns[i].data);
                if (c && !c->hasCode) {
                    Serial0.printf("[Validate] REJECT #%d: missing bytecode for '%s' "
                                  "(host: %s) - cannot verify execution\n",
                                  blk.header.index, blk.txns[i].data,
                                  c->host.toShortStr().c_str());
                    if (g_blockchainLedCb) g_blockchainLedCb(1, 7);  // reject: missing code
                    return 7;  // code unavailable
                }
            }
        }

        chain[chainLen] = blk;
        blockFees = 0;

        // Snapshot account + staking state before re-execution so we can
        // roll back cleanly if stateRoot verification fails.
        AccountBalance accountSnap[MAX_ACCOUNTS];
        memcpy(accountSnap, accounts, sizeof(accounts));
        uint8_t accountCountSnap = accountCount;
        uint32_t totalSupplySnap = staking.totalSupply;
        uint32_t totalStakedSnap = staking.totalStaked;

        if (g_blockchainLedCb) g_blockchainLedCb(8, 0);  // re-executing block
        Serial0.printf("[Validate] Re-executing #%d from %s (%d txns, slot %d)\n",
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
        setBalance(blk.header.validator, valBal + BLOCK_REWARD + blockFees);
        staking.totalSupply += BLOCK_REWARD;

        // --- Ethereum-style stateRoot verification ---
        // Every node re-executes all txns and verifies the resulting
        // state matches the proposer's stateRoot. This ensures consensus
        // on contract execution output even though bytecode is only
        // permanently stored on one node.
        Hash256 computedRoot = computeStateRoot();
        if (computedRoot != blk.header.stateRoot) {
            Serial0.printf("[Validate] REJECT #%d: stateRoot MISMATCH after re-execution!\n",
                          blk.header.index);
            Serial0.printf("  Expected: %s\n", blk.header.stateRoot.toShort().c_str());
            Serial0.printf("  Computed: %s\n", computedRoot.toShort().c_str());

            // Roll back all state changes from re-execution so the node
            // remains in a clean state for subsequent validation attempts.
            memcpy(accounts, accountSnap, sizeof(accounts));
            accountCount = accountCountSnap;
            staking.totalSupply = totalSupplySnap;
            staking.totalStaked = totalStakedSnap;
            blockFees = 0;
            if (g_blockchainLedCb) g_blockchainLedCb(1, 8);  // reject: state mismatch
            return 8;  // stateRoot mismatch
        }

        Serial0.printf("[Validate] StateRoot VERIFIED: %s\n",
                      computedRoot.toShort().c_str());

        // LED: bright green for state root verification
        if (g_blockchainLedCb) g_blockchainLedCb(0, 0);

        // Record block production for slashing/downtime tracking
        staking.recordBlockProduced(blk.header.validator, blk.header.index);

        // Discard temporarily fetched bytecode from non-host contracts.
        // This is the key resource optimization: only the host node
        // permanently stores the bytecode, other nodes fetch it
        // temporarily for validation then free the memory.
        for (int i = 0; i < contractCount; i++) {
            if (contracts[i].active && contracts[i].tempCode) {
                Serial0.printf("[Validate] Discarding temp bytecode for '%s' (%dB freed)\n",
                              contracts[i].name, contracts[i].codeLen);
                contracts[i].discardTempCode();
            }
        }

        chainLen++;

        if (height() % EPOCH_LENGTH == 0) {
            uint32_t newEpoch = height() / EPOCH_LENGTH;
            Serial0.printf("\n==== EPOCH TRANSITION: %d -> %d ====\n",
                          newEpoch - 1, newEpoch);

            // Check for validator downtime before election
            for (int v = 0; v < staking.activeCount; v++) {
                StakeInfo* si = staking.findStake(staking.activeSet[v]);
                if (si && si->missedBlocks >= DOWNTIME_THRESHOLD) {
                    staking.checkDowntime(staking.activeSet[v],
                                         blk.header.index, newEpoch);
                }
            }

            staking.runElection(newEpoch);
            if (g_blockchainLedCb) g_blockchainLedCb(10, 0);  // epoch election
        }

        if (chainLen >= PRUNE_TRIGGER) {
            if (g_blockchainLedCb) g_blockchainLedCb(9, 0);  // pruning
            pruneChain();
        }

        return 0;
    }


    // Reset all state back to empty (preserving nothing)
    void resetState() {
        memset(accounts, 0, sizeof(accounts));
        accountCount = 0;
        memset(contracts, 0, sizeof(contracts));
        contractCount = 0;
        mempoolSize = 0;
        staking = StakingEngine();
        chainLen = 0;
        chainOffset = 0;
        checkpointCount = 0;
        finalizedCount = 0;
    }

    // Reset chain back to genesis, wiping blocks 1+ and rebuilding genesis state
    // Used for same-genesis fork resolution
    bool resetToGenesis(const NodeID& selfId) {
        if (chainLen == 0) return false;

        Block genesis = chain[0];  // save genesis block
        Serial0.println("[Reorg] Resetting to genesis for chain reorg");

        adoptGenesis(genesis, selfId);

        Serial0.printf("[Reorg] Reset to genesis hash=%s, height=%d\n",
                      genesis.blockHash.toShort().c_str(), height());
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

        // Sort account indices by NodeID so state root is deterministic
        // regardless of the order accounts were created in the array.
        // (Different nodes may discover peers in different order.)
        uint8_t idx[MAX_ACCOUNTS];
        uint8_t n = 0;
        for (int i = 0; i < accountCount; i++) {
            if (accounts[i].used) idx[n++] = i;
        }
        for (uint8_t a = 1; a < n; a++) {
            uint8_t key = idx[a];
            int b = a - 1;
            while (b >= 0 && memcmp(accounts[idx[b]].owner.id,
                                    accounts[key].owner.id, 6) > 0) {
                idx[b + 1] = idx[b];
                b--;
            }
            idx[b + 1] = key;
        }
        for (uint8_t i = 0; i < n && off < 460; i++) {
            memcpy(buf + off, accounts[idx[i]].owner.id, 6); off += 6;
            memcpy(buf + off, &accounts[idx[i]].balance, 4); off += 4;
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

        for (uint32_t i = 0; i < toPrune; i++) {
            if (finalizedCount < MAX_FINALIZED) {
                finalized[finalizedCount++] = CompactHeader::fromBlock(chain[i]);
            } else {
                memmove(finalized, finalized + 1,
                        (MAX_FINALIZED - 1) * sizeof(CompactHeader));
                finalized[MAX_FINALIZED - 1] = CompactHeader::fromBlock(chain[i]);
            }
        }

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
        Serial0.printf("  Finalized headers: %d\n", finalizedCount);
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
        Serial0.printf("Finalized headers: %d | Checkpoints: %d\n",
                      finalizedCount, checkpointCount);
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