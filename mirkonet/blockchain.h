#pragma once
#include "types.h"
#include "mvm.h"
#include "staking.h"
#include "txauth.h"


typedef void (*BlockchainLedCallback)(uint8_t event, uint8_t param);
static BlockchainLedCallback g_blockchainLedCb = nullptr;


typedef void (*NvsWriteCallback)(const uint8_t* buf, uint16_t len);
static NvsWriteCallback g_nvsWriteCallback = nullptr;

struct AccountBalance {
    NodeID   owner;
    uint32_t balance;
    uint32_t nonce;
    uint32_t lastFaucet;
    bool     used;
};

struct ChainMetrics {
    uint32_t blocksProduced = 0;
    uint32_t blocksValidated = 0;
    uint32_t blocksRejected = 0;
    uint32_t txExecuted = 0;
    uint32_t txRejected = 0;
    uint32_t stateRootCalls = 0;
    uint32_t stateRootMicrosTotal = 0;
    uint32_t validateMicrosTotal = 0;
    uint32_t pruneCount = 0;
    uint32_t lastPruneBytes = 0;
    uint32_t checkpointRestoreCount = 0;
    uint32_t checkpointRejectCount = 0;
    uint32_t hardwareReplays = 0;
    uint32_t bytecodeMisses = 0;
};

struct CheckpointCert {
    NodeID signer;
    uint8_t publicKey[64];
    uint8_t signature[64];
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
    NodeID         localNode = ZERO_NODE;
    bool           localNodeSet = false;
    uint32_t       observedSlot = 0;
    bool           observedSlotSet = false;
    mutable ChainMetrics metrics;

    struct StateSnapshot {
        Contract       contracts[MAX_CONTRACTS];
        uint8_t        contractCount;
        AccountBalance accounts[MAX_ACCOUNTS];
        uint8_t        accountCount;
        StakingEngine  staking;
        uint32_t       blockFees;
    };

    StateSnapshot blockSnapScratch;
    StateSnapshot txSnapScratch;
    StateSnapshot hardwareSnapScratch;
    mutable uint8_t rootContractIdx[MAX_CONTRACTS];
    mutable uint8_t rootAccountIdx[MAX_ACCOUNTS];
    mutable uint8_t rootStakeIdx[MAX_CANDIDATES];
    mutable uint8_t rootStorageIdx[MVM_MAX_STORAGE];
    mutable Hash256 rootContractLeaves[MAX_CONTRACTS];
    mutable Hash256 rootAccountLeaves[MAX_ACCOUNTS];
    mutable Hash256 rootStakeLeaves[MAX_CANDIDATES];
    mutable Hash256 rootActiveLeaves[MAX_VALIDATORS];
    mutable uint8_t rootHashBuf[MVM_MAX_STORAGE * 8 + 128];
    CheckpointCert checkpointCertScratch[MAX_CHECKPOINT_SIGNATURES];
    AccountBalance checkpointAccountScratch[MAX_ACCOUNTS];
    StakeInfo checkpointStakeScratch[MAX_CANDIDATES];
    NodeID checkpointActiveScratch[MAX_VALIDATORS];
    Contract checkpointContractScratch[MAX_CONTRACTS];
    NodeID checkpointSeenScratch[MAX_CHECKPOINT_SIGNATURES];

    void setLocalNode(const NodeID& id) {
        localNode = id;
        localNodeSet = true;
    }

    void setObservedSlot(uint32_t slot) {
        observedSlot = slot;
        observedSlotSet = true;
    }

    void captureState(StateSnapshot& snap) const {
        memcpy(snap.contracts, contracts, sizeof(contracts));
        snap.contractCount = contractCount;
        memcpy(snap.accounts, accounts, sizeof(accounts));
        snap.accountCount = accountCount;
        snap.staking = staking;
        snap.blockFees = blockFees;
    }

    void restoreState(const StateSnapshot& snap) {
        memcpy(contracts, snap.contracts, sizeof(contracts));
        contractCount = snap.contractCount;
        memcpy(accounts, snap.accounts, sizeof(accounts));
        accountCount = snap.accountCount;
        staking = snap.staking;
        blockFees = snap.blockFees;
    }


    static NodeID genesisFounder() {
        static const uint8_t id[] = GENESIS_FOUNDER_BYTES;
        NodeID f; memcpy(f.id, id, 6);
        return f;
    }

    void initGenesis() {
        Block& gen = chain[0];
        memset(&gen, 0, sizeof(Block));
        gen.header.index = 0;
        gen.header.timestamp = 0;
        gen.header.prevHash = ZERO_HASH;
        gen.header.validator = genesisFounder();
        gen.header.txCount = 0;
        gen.header.slot = 0;
        gen.header.epoch = 0;
        gen.header.reward = 0;

        NodeID founder = genesisFounder();
        setBalance(founder, GENESIS_PER_NODE);
        staking.totalSupply = GENESIS_PER_NODE;

        uint32_t stakeAmt = GENESIS_PER_NODE / 2;
        setBalance(founder, GENESIS_PER_NODE - stakeAmt);
        staking.addGenesisValidator(founder, stakeAmt);

        gen.header.stateRoot = computeStateRoot();
        gen.computeHash();
        chainLen = 1;

        Serial0.printf("[Genesis] Hardcoded founder %s: %u liquid, %u staked, hash=%s\n",
                      founder.toShortStr().c_str(), getBalance(founder), stakeAmt,
                      gen.header.stateRoot.toShort().c_str());
    }


    bool isBootstrap() const {
        if (height() >= BOOTSTRAP_HEIGHT) return false;

        NodeID founder = genesisFounder();
        for (int i = 0; i < staking.activeCount; i++) {
            if (staking.activeSet[i] != founder) return false;
        }
        return true;
    }


    bool initFromCheckpoint(const Checkpoint& ckpt,
                            const AccountBalance* accts, uint8_t acctCount,
                            const StakeInfo* stakeSnap, uint8_t stakeCnt,
                            uint8_t activeValidatorCnt, const NodeID* activeSetSnap,
                            const Contract* contractSnap, uint8_t contractCnt,
                            uint32_t supply, uint32_t staked) {
        if (acctCount > MAX_ACCOUNTS || stakeCnt > MAX_CANDIDATES ||
            activeValidatorCnt > MAX_VALIDATORS || contractCnt > MAX_CONTRACTS)
            return false;
        resetState();

        initGenesis();


        accountCount = acctCount;
        memcpy(accounts, accts, acctCount * sizeof(AccountBalance));


        staking.stakeCount = stakeCnt;
        memcpy(staking.stakes, stakeSnap, stakeCnt * sizeof(StakeInfo));
        staking.activeCount = activeValidatorCnt;
        memcpy(staking.activeSet, activeSetSnap, activeValidatorCnt * sizeof(NodeID));
        staking.totalSupply = supply;
        staking.totalStaked = staked;

        contractCount = contractCnt;
        memcpy(contracts, contractSnap, contractCnt * sizeof(Contract));


        if (checkpointCount < MAX_CHECKPOINTS)
            checkpoints[checkpointCount++] = ckpt;


        chainOffset = ckpt.toBlock + 1;
        chainLen = 0;
        staking.currentEpoch = chainOffset / EPOCH_LENGTH;

        Serial0.printf("[Checkpoint] Initialized from checkpoint [%d..%d], "
                      "offset=%d, accounts=%d, validators=%d\n",
                      ckpt.fromBlock, ckpt.toBlock,
                      chainOffset, accountCount, staking.activeCount);


        uint32_t epochOfFirst = chainOffset / EPOCH_LENGTH;
        Serial0.printf("[Checkpoint] Epoch-link anchor: block #%d must have "
                      "prevHash=%s  (epoch %d)\n",
                      chainOffset,
                      ckpt.lastBlockHash.toShort().c_str(),
                      epochOfFirst);
        Hash256 computedRoot = computeStateRoot();
        if (computedRoot != ckpt.stateRoot) {
            Serial0.printf("[Checkpoint] REJECT: snapshot stateRoot mismatch expected=%s got=%s\n",
                          ckpt.stateRoot.toShort().c_str(),
                          computedRoot.toShort().c_str());
            resetState();
            initGenesis();
            return false;
        }
        return true;
    }


    uint16_t serializeState(uint8_t* buf, uint16_t maxLen) const {
        uint16_t off = 0;
        if (checkpointCount == 0) return 0;
        const Checkpoint& ckpt = checkpoints[checkpointCount - 1];


        memcpy(buf + off, &ckpt.fromBlock, 4); off += 4;
        memcpy(buf + off, &ckpt.toBlock, 4); off += 4;
        memcpy(buf + off, ckpt.lastBlockHash.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, ckpt.stateRoot.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, ckpt.chainMerkle.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, ckpt.prevCheckpoint.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, &ckpt.timestamp, 4); off += 4;

        if (off + 1 + 134 > maxLen) return 0;
        Hash256 ckptHash = ckpt.contentHash();
        CheckpointCert cert;
        memset(&cert, 0, sizeof(cert));
        bool hasCert = localNodeSet &&
                       TxAuth::signHash(ckptHash, cert.publicKey, cert.signature);
        if (hasCert) cert.signer = localNode;
        buf[off++] = hasCert ? 1 : 0;
        if (hasCert) {
            memcpy(buf + off, cert.signer.id, 6); off += 6;
            memcpy(buf + off, cert.publicKey, 64); off += 64;
            memcpy(buf + off, cert.signature, 64); off += 64;
        }


        if (off + 1 > maxLen) return 0;
        buf[off++] = accountCount;
        for (int i = 0; i < accountCount; i++) {
            if (off + 19 > maxLen) return 0;
            memcpy(buf + off, accounts[i].owner.id, 6); off += 6;
            memcpy(buf + off, &accounts[i].balance, 4); off += 4;
            memcpy(buf + off, &accounts[i].nonce, 4); off += 4;
            memcpy(buf + off, &accounts[i].lastFaucet, 4); off += 4;
            buf[off++] = accounts[i].used ? 1 : 0;
        }


        if (off + 1 > maxLen) return 0;
        buf[off++] = staking.stakeCount;
        for (int i = 0; i < staking.stakeCount; i++) {
            if (off + 37 > maxLen) return 0;
            const StakeInfo& s = staking.stakes[i];
            memcpy(buf + off, s.staker.id, 6); off += 6;
            memcpy(buf + off, &s.stakedAmount, 4); off += 4;
            memcpy(buf + off, &s.unstakingAmount, 4); off += 4;
            memcpy(buf + off, &s.unstakeBlock, 4); off += 4;
            buf[off++] = s.isCandidate ? 1 : 0;
            buf[off++] = s.used ? 1 : 0;
            buf[off++] = s.jailed ? 1 : 0;
            memcpy(buf + off, &s.jailUntilEpoch, 4); off += 4;
            memcpy(buf + off, &s.slashCount, 4); off += 4;
            memcpy(buf + off, &s.missedBlocks, 4); off += 4;
            memcpy(buf + off, &s.lastProducedBlock, 4); off += 4;
        }


        if (off + 1 + staking.activeCount * 6 > maxLen) return 0;
        buf[off++] = staking.activeCount;
        for (int i = 0; i < staking.activeCount; i++) {
            memcpy(buf + off, staking.activeSet[i].id, 6); off += 6;
        }


        if (off + 8 > maxLen) return 0;
        memcpy(buf + off, &staking.totalSupply, 4); off += 4;
        memcpy(buf + off, &staking.totalStaked, 4); off += 4;


        if (off + 1 > maxLen) return 0;
        buf[off++] = contractCount;
        for (int i = 0; i < contractCount; i++) {
            const Contract& c = contracts[i];
            bool includeCode = c.hasCode && !c.tempCode && c.codeLen > 0 &&
                               localNodeSet &&
                               (c.host == localNode || CONTRACT_CODE_BACKUP_CACHE);
            uint16_t codeBytes = includeCode ? c.codeLen : 0;
            if (codeBytes > MVM_MAX_CODE) return 0;
            if (off + 213 + codeBytes > maxLen) return 0;
            memcpy(buf + off, c.name, 16); off += 16;
            memcpy(buf + off, c.deployer.id, 6); off += 6;
            memcpy(buf + off, c.host.id, 6); off += 6;
            memcpy(buf + off, c.codeHash.bytes, HASH_SIZE); off += HASH_SIZE;
            memcpy(buf + off, &c.codeLen, 2); off += 2;
            memcpy(buf + off, &c.balance, 4); off += 4;
            buf[off++] = c.active ? 1 : 0;
            buf[off++] = includeCode ? 1 : 0;
            buf[off++] = c.tempCode ? 1 : 0;
            for (int s = 0; s < MVM_MAX_STORAGE; s++) {
                memcpy(buf + off, &c.storage[s].key, 4); off += 4;
                memcpy(buf + off, &c.storage[s].value, 4); off += 4;
                buf[off++] = c.storage[s].used ? 1 : 0;
            }
            if (includeCode) {
                memcpy(buf + off, c.code, codeBytes); off += codeBytes;
            }
        }

        return off;
    }


    bool deserializeCheckpointState(const uint8_t* buf, uint16_t len) {
        uint16_t off = 0;
        Checkpoint ckpt;
        memset(&ckpt, 0, sizeof(ckpt));
        ckpt.used = true;

        if (off + 4 + 4 + HASH_SIZE * 4 + 4 > len) return false;
        memcpy(&ckpt.fromBlock, buf + off, 4); off += 4;
        memcpy(&ckpt.toBlock, buf + off, 4); off += 4;
        memcpy(ckpt.lastBlockHash.bytes, buf + off, HASH_SIZE); off += HASH_SIZE;
        memcpy(ckpt.stateRoot.bytes, buf + off, HASH_SIZE); off += HASH_SIZE;
        memcpy(ckpt.chainMerkle.bytes, buf + off, HASH_SIZE); off += HASH_SIZE;
        memcpy(ckpt.prevCheckpoint.bytes, buf + off, HASH_SIZE); off += HASH_SIZE;
        memcpy(&ckpt.timestamp, buf + off, 4); off += 4;

        if (off >= len) return false;
        uint8_t certCnt = buf[off++];
        if (certCnt > MAX_CHECKPOINT_SIGNATURES) return false;
        CheckpointCert* certs = checkpointCertScratch;
        memset(certs, 0, sizeof(checkpointCertScratch));
        for (int i = 0; i < certCnt; i++) {
            if (off + 134 > len) return false;
            memcpy(certs[i].signer.id, buf + off, 6); off += 6;
            memcpy(certs[i].publicKey, buf + off, 64); off += 64;
            memcpy(certs[i].signature, buf + off, 64); off += 64;
        }


        if (off >= len) return false;
        uint8_t acctCnt = buf[off++];
        if (acctCnt > MAX_ACCOUNTS) return false;
        AccountBalance* accts = checkpointAccountScratch;
        memset(accts, 0, sizeof(checkpointAccountScratch));
        for (int i = 0; i < acctCnt; i++) {
            if (off + 19 > len) return false;
            memcpy(accts[i].owner.id, buf + off, 6); off += 6;
            memcpy(&accts[i].balance, buf + off, 4); off += 4;
            memcpy(&accts[i].nonce, buf + off, 4); off += 4;
            memcpy(&accts[i].lastFaucet, buf + off, 4); off += 4;
            accts[i].used = buf[off++] != 0;
        }


        if (off >= len) return false;
        uint8_t stakeCnt = buf[off++];
        if (stakeCnt > MAX_CANDIDATES) return false;
        StakeInfo* stakeSnap = checkpointStakeScratch;
        memset(stakeSnap, 0, sizeof(checkpointStakeScratch));
        for (int i = 0; i < stakeCnt; i++) {
            if (off + 37 > len) return false;
            memcpy(stakeSnap[i].staker.id, buf + off, 6); off += 6;
            memcpy(&stakeSnap[i].stakedAmount, buf + off, 4); off += 4;
            memcpy(&stakeSnap[i].unstakingAmount, buf + off, 4); off += 4;
            memcpy(&stakeSnap[i].unstakeBlock, buf + off, 4); off += 4;
            stakeSnap[i].isCandidate = buf[off++] != 0;
            stakeSnap[i].used = buf[off++] != 0;
            stakeSnap[i].jailed = buf[off++] != 0;
            memcpy(&stakeSnap[i].jailUntilEpoch, buf + off, 4); off += 4;
            memcpy(&stakeSnap[i].slashCount, buf + off, 4); off += 4;
            memcpy(&stakeSnap[i].missedBlocks, buf + off, 4); off += 4;
            memcpy(&stakeSnap[i].lastProducedBlock, buf + off, 4); off += 4;
        }


        if (off >= len) return false;
        uint8_t activeCnt = buf[off++];
        if (activeCnt > MAX_VALIDATORS) return false;
        NodeID* activeSnap = checkpointActiveScratch;
        memset(activeSnap, 0, sizeof(checkpointActiveScratch));
        for (int i = 0; i < activeCnt; i++) {
            if (off + 6 > len) return false;
            memcpy(activeSnap[i].id, buf + off, 6); off += 6;
        }


        uint32_t supply = 0, staked = 0;
        if (off + 8 > len) return false;
        memcpy(&supply, buf + off, 4); off += 4;
        memcpy(&staked, buf + off, 4); off += 4;

        Contract* contractSnap = checkpointContractScratch;
        memset(contractSnap, 0, sizeof(checkpointContractScratch));
        uint8_t contractCnt = 0;
        if (off < len) {
            contractCnt = buf[off++];
            if (contractCnt > MAX_CONTRACTS) return false;
            for (int i = 0; i < contractCnt; i++) {
                if (off + 213 > len) return false;
                Contract& c = contractSnap[i];
                memset(&c, 0, sizeof(Contract));
                memcpy(c.name, buf + off, 16); off += 16; c.name[15] = '\0';
                c.addr = ContractAddr::fromName(c.name);
                memcpy(c.deployer.id, buf + off, 6); off += 6;
                memcpy(c.host.id, buf + off, 6); off += 6;
                memcpy(c.codeHash.bytes, buf + off, HASH_SIZE); off += HASH_SIZE;
                memcpy(&c.codeLen, buf + off, 2); off += 2;
                memcpy(&c.balance, buf + off, 4); off += 4;
                c.active = buf[off++] != 0;
                bool includeCode = buf[off++] != 0;
                bool wasTempCode = buf[off++] != 0;
                c.hasCode = false;
                c.tempCode = false;
                for (int s = 0; s < MVM_MAX_STORAGE; s++) {
                    memcpy(&c.storage[s].key, buf + off, 4); off += 4;
                    memcpy(&c.storage[s].value, buf + off, 4); off += 4;
                    c.storage[s].used = buf[off++] != 0;
                }
                if (c.codeLen > MVM_MAX_CODE) return false;
                if (includeCode) {
                    if (off + c.codeLen > len) return false;
                    memcpy(c.code, buf + off, c.codeLen); off += c.codeLen;
                    if (sha256(c.code, c.codeLen) != c.codeHash) return false;
                    c.hasCode = true;
                    c.tempCode = wasTempCode;
                }
            }
        }

        Hash256 ckptHash = ckpt.contentHash();
        uint8_t validCerts = 0;
        NodeID* seen = checkpointSeenScratch;
        memset(seen, 0, sizeof(checkpointSeenScratch));
        for (int i = 0; i < certCnt; i++) {
            bool duplicate = false;
            for (int j = 0; j < validCerts; j++) {
                if (seen[j] == certs[i].signer) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            bool authorized = ckpt.toBlock < BOOTSTRAP_HEIGHT;
            for (int j = 0; j < activeCnt && !authorized; j++) {
                if (activeSnap[j] == certs[i].signer) authorized = true;
            }
            if (!authorized) continue;
            if (!TxAuth::verifyHash(ckptHash, certs[i].publicKey,
                                    certs[i].signature, certs[i].signer))
                continue;
            seen[validCerts++] = certs[i].signer;
        }
        if (validCerts == 0) {
            metrics.checkpointRejectCount++;
            Serial0.println("[Checkpoint] REJECT: no valid checkpoint certificate");
            return false;
        }


        if (checkpointCount > 0) {
            const Checkpoint& latest = checkpoints[checkpointCount - 1];
            if (ckpt.fromBlock == latest.toBlock + 1) {


                Hash256 expectedPrev = latest.contentHash();
                if (ckpt.prevCheckpoint != expectedPrev) {
                    Serial0.printf("[Checkpoint] REJECT: prevCheckpoint chain broken "
                                  "[%d..%d]->?->[%d..%d]\n",
                                  latest.fromBlock, latest.toBlock,
                                  ckpt.fromBlock, ckpt.toBlock);
                    Serial0.printf("  Expected: %s\n", expectedPrev.toShort().c_str());
                    Serial0.printf("  Got:      %s\n", ckpt.prevCheckpoint.toShort().c_str());
                    return false;
                }
                Serial0.printf("[Checkpoint] prevCheckpoint chain OK [%d..%d]->[%d..%d]\n",
                              latest.fromBlock, latest.toBlock,
                              ckpt.fromBlock, ckpt.toBlock);
            } else {


                Serial0.printf("[Checkpoint] REJECT: unverifiable checkpoint gap "
                              "our=[%d..%d] recv=[%d..%d]\n",
                              latest.fromBlock, latest.toBlock,
                              ckpt.fromBlock, ckpt.toBlock);
                metrics.checkpointRejectCount++;
                return false;
            }
        } else {
            Serial0.printf("[Checkpoint] First checkpoint (TOFU), contentHash=%s\n",
                          ckpt.contentHash().toShort().c_str());
        }

        bool ok = initFromCheckpoint(ckpt, accts, acctCnt, stakeSnap, stakeCnt,
                                     activeCnt, activeSnap, contractSnap,
                                     contractCnt, supply, staked);
        if (ok) {
            Hash256 restoredRoot = computeStateRoot();
            if (restoredRoot != ckpt.stateRoot) {
                Serial0.printf("[Checkpoint] REJECT: stateRoot mismatch after restore\n");
                Serial0.printf("  Expected: %s\n", ckpt.stateRoot.toShort().c_str());
                Serial0.printf("  Restored: %s\n", restoredRoot.toShort().c_str());
                resetToGenesis();
                metrics.checkpointRejectCount++;
                return false;
            }
        }
        if (ok) metrics.checkpointRestoreCount++;
        else metrics.checkpointRejectCount++;
        return ok;
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
                return e ? e->codeLen * gp : gp;
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


    uint8_t faucet(const NodeID& requester, uint32_t currentBlock) {
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
        uint32_t faucetCooldownBlocks =
            max((uint32_t)1, (uint32_t)(FAUCET_COOLDOWN / BLOCK_INTERVAL));
        if (acct->lastFaucet > 0 &&
            currentBlock - acct->lastFaucet < faucetCooldownBlocks)
            return 1;


        uint32_t amount = isBootstrap() ? GENESIS_PER_NODE : FAUCET_AMOUNT;
        acct->balance += amount;
        acct->lastFaucet = currentBlock;
        staking.totalSupply += amount;
        Serial0.printf("[Faucet] %u tokens -> %s (balance: %u)%s\n",
                      amount, requester.toShortStr().c_str(), acct->balance,
                      isBootstrap() ? " [bootstrap]" : "");
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
        c.init(name, code, codeLen, deployer, true);
        Serial0.printf("[Deploy] '%s' at %s by %s (%d bytes)\n",
                      name, c.addr.toHex().c_str(),
                      deployer.toShortStr().c_str(), codeLen);
        return &c;
    }


    struct TxResult {
        bool      success;
        bool      needsCode;
        MVMStatus vmStatus;
        uint32_t  gasUsed;
        uint32_t  fee;
        String    message;
    };

    TxResult executeTx(const Transaction& tx, uint32_t blockNumber,
                       uint32_t blockTimestamp, bool hardwareEffects = false) {
        TxResult r = { false, false, VM_RUNNING, 0, 0, "" };
        uint32_t gp = tx.gasPrice > 0 ? tx.gasPrice : DEFAULT_GAS_PRICE;

        if (!TxAuth::verify(tx)) {
            r.message = "Invalid transaction signature";
            metrics.txRejected++;
            return r;
        }
        if (tx.nonce != getNonce(tx.sender)) {
            r.message = "Invalid nonce: got " + String(tx.nonce) +
                        ", expected " + String(getNonce(tx.sender));
            metrics.txRejected++;
            return r;
        }
        if (gp < MIN_GAS_PRICE) {
            r.message = "Gas price below minimum";
            metrics.txRejected++;
            return r;
        }

        if (tx.type != TxType::FAUCET && !canAffordFee(tx.sender, tx.type, gp)) {
            r.message = "Insufficient balance for gas (need >= " +
                        String(gp) + " tokens)";
            metrics.txRejected++;
            return r;
        }

        switch (tx.type) {

        case TxType::DEPLOY: {

            auto* entry = g_deployCache.find(tx.data);
            if (!entry) {
                r.message = String("No deploy data for '") + tx.data + "'";
                break;
            }
            if (entry->codeLen != tx.deployCodeLen ||
                entry->codeHash != tx.deployCodeHash) {
                r.message = "Deploy bytecode metadata mismatch";
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
            ctx.blockNumber = blockNumber;
            ctx.blockTimestamp = blockTimestamp;
            ctx.origin = tx.sender;
            ctx.localNode = localNode;
            ctx.localNodeSet = localNodeSet;
            ctx.allowHardwareEffects = hardwareEffects;


            g_contractResolver = [](const char* name) -> Contract* {


                extern BlockchainState g_chain;
                if (!name) return nullptr;

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

            r.fee = 0;
            uint8_t res = faucet(tx.sender, blockNumber);
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
            metrics.txExecuted++;
            incrementNonce(tx.sender);

            if (g_blockchainLedCb) g_blockchainLedCb(2, (uint8_t)tx.type);
        } else {
            metrics.txRejected++;

            if (g_blockchainLedCb) g_blockchainLedCb(3, (uint8_t)tx.type);
        }
        return r;
    }


    void recordSkippedSlotsForBlock(uint32_t blockSlot, const NodeID& producer) {
        if (isBootstrap() || staking.activeCount <= 1 || chainLen == 0) return;

        uint32_t prevSlot = chain[chainLen - 1].header.slot;
        if (blockSlot <= prevSlot + 1) return;

        for (uint32_t missedSlot = prevSlot + 1; missedSlot < blockSlot; missedSlot++) {
            NodeID expected = staking.getSlotProducer(missedSlot);
            if (expected.isZero() || expected == producer) continue;

            StakeInfo* missed = staking.findStake(expected);
            if (missed && missed->used && !missed->jailed) {
                missed->missedBlocks++;
                Serial0.printf("[Downtime] %s missed slot %d before block slot %d\n",
                              expected.toShortStr().c_str(), missedSlot, blockSlot);
            }
        }
    }


    bool createBlock(const NodeID& validator, uint32_t slot,
                     Transaction* txns, uint8_t txCount) {
        if (chainLen >= MAX_BLOCKS) return false;
        if (txCount > MAX_TXN_PER_BLOCK) txCount = MAX_TXN_PER_BLOCK;

        captureState(blockSnapScratch);

        uint32_t logicalIdx = chainOffset + chainLen;
        if (slot < logicalIdx) {
            Serial0.printf("[Block] Refusing slot %d for height %d\n",
                          slot, logicalIdx);
            return false;
        }
        if (chainLen > 0 && slot <= chain[chainLen - 1].header.slot) {
            Serial0.printf("[Block] Refusing non-monotonic slot %d after %d\n",
                          slot, chain[chainLen - 1].header.slot);
            restoreState(blockSnapScratch);
            return false;
        }
        if (!isBootstrap()) {
            if (!staking.isActiveValidator(validator)) {
                Serial0.printf("[Block] Refusing non-validator producer %s\n",
                              validator.toShortStr().c_str());
                restoreState(blockSnapScratch);
                return false;
            }
            NodeID expectedProducer = staking.getSlotProducer(slot);
            if (expectedProducer != validator) {
                Serial0.printf("[Block] Refusing slot %d: expected %s, got %s\n",
                              slot,
                              expectedProducer.toShortStr().c_str(),
                              validator.toShortStr().c_str());
                restoreState(blockSnapScratch);
                return false;
            }
        }
        uint32_t epoch = logicalIdx / EPOCH_LENGTH;
        blockFees = 0;
        Block& blk = chain[chainLen];
        memset(&blk, 0, sizeof(Block));
        blk.header.index = logicalIdx;
        blk.header.timestamp = millis();
        if (chainLen > 0)
            blk.header.prevHash = chain[chainLen-1].blockHash;
        else if (checkpointCount > 0)
            blk.header.prevHash = checkpoints[checkpointCount - 1].lastBlockHash;
        else
            blk.header.prevHash = ZERO_HASH;
        blk.header.validator = validator;
        blk.header.slot = slot;
        blk.header.epoch = epoch;
        blk.header.txCount = 0;

        for (int i = 0; i < txCount; i++) {
            captureState(txSnapScratch);

            TxResult r = executeTx(txns[i], blk.header.index,
                                   blk.header.timestamp, false);
            if (r.success) {
                blk.txns[blk.header.txCount++] = txns[i];
                Serial0.printf("  TX[%d] OK: %s\n", i, r.message.c_str());
                for (int e = 0; e < vm.eventCount(); e++)
                    Serial0.printf("    Event[%d] = %u\n", e, vm.event(e).value);
            } else {
                restoreState(txSnapScratch);
                Serial0.printf("  TX[%d] FAIL (rolled back): %s\n", i, r.message.c_str());
            }
        }

        blk.header.reward = BLOCK_REWARD + blockFees;

        recordSkippedSlotsForBlock(slot, validator);

        uint32_t valBal = getBalance(validator);
        setBalance(validator, valBal + BLOCK_REWARD + blockFees);
        staking.totalSupply += BLOCK_REWARD;


        staking.recordBlockProduced(validator, logicalIdx);

        blk.header.stateRoot = computeStateRoot();
        blk.computeHash();
        if (!TxAuth::signHash(blk.blockHash, blk.validatorPublicKey,
                              blk.validatorSignature)) {
            restoreState(blockSnapScratch);
            Serial0.println("[Block] Signing failed; block discarded");
            return false;
        }
        chainLen++;

        applyHostHardwareEffects(blk);

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

        metrics.blocksProduced++;
        return true;
    }


    bool validateBlock(const Block& blk) const {
        if (blk.header.index != height()) return false;
        if (chainLen > 0 && blk.header.prevHash != chain[chainLen-1].blockHash)
            return false;
        Block copy = blk;
        copy.computeHash();
        if (copy.blockHash != blk.blockHash) return false;
        if (!TxAuth::verifyHash(blk.blockHash, blk.validatorPublicKey,
                                blk.validatorSignature, blk.header.validator))
            return false;
        return true;
    }

    uint8_t applyNetworkBlock(const Block& blk, bool liveSlotCheck = true) {
        uint32_t validateStarted = micros();
        uint32_t expectedIdx = height();

        if (blk.header.index != expectedIdx) {
            Serial0.printf("[Validate] REJECT #%d: expected #%d\n",
                          blk.header.index, expectedIdx);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 1);
            metrics.blocksRejected++;
            return 1;
        }
        if (g_blockchainLedCb) g_blockchainLedCb(4, 0);
        if (blk.header.epoch != blk.header.index / EPOCH_LENGTH) {
            Serial0.printf("[Validate] REJECT #%d: bad epoch %d\n",
                          blk.header.index, blk.header.epoch);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 1);
            metrics.blocksRejected++;
            return 1;
        }
        if (blk.header.slot < blk.header.index) {
            Serial0.printf("[Validate] REJECT #%d: slot %d before height %d\n",
                          blk.header.index, blk.header.slot, blk.header.index);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 6);
            metrics.blocksRejected++;
            return 6;
        }
        if (liveSlotCheck && observedSlotSet && blk.header.slot > observedSlot + 1) {
            Serial0.printf("[Validate] REJECT #%d: future slot %d > observed %d\n",
                          blk.header.index, blk.header.slot, observedSlot);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 6);
            metrics.blocksRejected++;
            return 6;
        }


        Hash256 ourTip;
        if (chainLen > 0) {
            ourTip = chain[chainLen - 1].blockHash;
        } else if (chainOffset > 0 && checkpointCount > 0) {
            ourTip = checkpoints[checkpointCount - 1].lastBlockHash;
        } else {
            ourTip = ZERO_HASH;
        }
        if (blk.header.prevHash != ourTip) {
            Serial0.printf("[Validate] REJECT #%d: prevHash mismatch\n",
                          blk.header.index);
            Serial0.printf("  Expected: %s\n", ourTip.toShort().c_str());
            Serial0.printf("  Got:      %s\n", blk.header.prevHash.toShort().c_str());
            if (g_blockchainLedCb) g_blockchainLedCb(1, 2);
            metrics.blocksRejected++;
            return 2;
        }
        if (chainLen > 0 && blk.header.slot <= chain[chainLen - 1].header.slot) {
            Serial0.printf("[Validate] REJECT #%d: non-monotonic slot %d <= %d\n",
                          blk.header.index, blk.header.slot,
                          chain[chainLen - 1].header.slot);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 6);
            metrics.blocksRejected++;
            return 6;
        }

        Block copy = blk;
        copy.computeHash();
        if (copy.blockHash != blk.blockHash) {
            Serial0.printf("[Validate] REJECT #%d: hash mismatch\n",
                          blk.header.index);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 3);
            metrics.blocksRejected++;
            return 3;
        }
        if (!TxAuth::verifyHash(blk.blockHash, blk.validatorPublicKey,
                                blk.validatorSignature, blk.header.validator)) {
            Serial0.printf("[Validate] REJECT #%d: validator signature invalid\n",
                          blk.header.index);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 3);
            metrics.blocksRejected++;
            return 3;
        }
        if (g_blockchainLedCb) g_blockchainLedCb(5, 0);


        if (isBootstrap()) {
            Serial0.printf("[Validate] Bootstrap mode: accepting block from %s\n",
                          blk.header.validator.toShortStr().c_str());
            if (g_blockchainLedCb) g_blockchainLedCb(6, 0);
        } else {
            if (staking.activeCount == 0) {
                Serial0.printf("[Validate] REJECT #%d: no active validators\n",
                              blk.header.index);
                if (g_blockchainLedCb) g_blockchainLedCb(1, 5);
                metrics.blocksRejected++;
                return 5;
            }
            if (!staking.isActiveValidator(blk.header.validator)) {
                Serial0.printf("[Validate] REJECT #%d: %s is NOT an active validator\n",
                              blk.header.index,
                              blk.header.validator.toShortStr().c_str());
                Serial0.print("  Active set: ");
                for (int i = 0; i < staking.activeCount; i++)
                    Serial0.print(staking.activeSet[i].toShortStr() + " ");
                Serial0.println();
                if (g_blockchainLedCb) g_blockchainLedCb(1, 5);
                metrics.blocksRejected++;
                return 5;
            }

            NodeID expectedProducer = staking.getSlotProducer(blk.header.slot);
            if (expectedProducer != blk.header.validator) {
                Serial0.printf("[Validate] REJECT #%d: slot %d belongs to %s, not %s\n",
                              blk.header.index, blk.header.slot,
                              expectedProducer.toShortStr().c_str(),
                              blk.header.validator.toShortStr().c_str());
                if (g_blockchainLedCb) g_blockchainLedCb(1, 6);
                metrics.blocksRejected++;
                return 6;
            }

            Serial0.printf("[Validate] PoS OK: slot %d -> %s (validator %d/%d)\n",
                          blk.header.slot,
                          blk.header.validator.toShortStr().c_str(),
                          staking.getValidatorIndex(blk.header.validator) + 1,
                          staking.activeCount);
            if (g_blockchainLedCb) g_blockchainLedCb(6, 0);
        }

        if (chainLen >= MAX_BLOCKS) {
            Serial0.println("[Validate] Chain full, pruning first...");
            if (g_blockchainLedCb) g_blockchainLedCb(9, 0);
            pruneChain();
            if (chainLen >= MAX_BLOCKS) {
                metrics.blocksRejected++;
                return 4;
            }
        }


        for (int i = 0; i < blk.header.txCount; i++) {
            if (blk.txns[i].type == TxType::CALL) {
                Contract* c = findContract(blk.txns[i].data);
                if (c && !c->hasCode) {
                    Serial0.printf("[Validate] REJECT #%d: missing bytecode for '%s' "
                                  "(host: %s) - cannot verify execution\n",
                                  blk.header.index, blk.txns[i].data,
                                  c->host.toShortStr().c_str());
                    if (g_blockchainLedCb) g_blockchainLedCb(1, 7);
                    metrics.bytecodeMisses++;
                    metrics.blocksRejected++;
                    return 7;
                }
            }
        }

        chain[chainLen] = blk;
        blockFees = 0;

        captureState(blockSnapScratch);

        if (g_blockchainLedCb) g_blockchainLedCb(8, 0);
        Serial0.printf("[Validate] Re-executing #%d from %s (%d txns, slot %d)\n",
                      blk.header.index,
                      blk.header.validator.toShortStr().c_str(),
                      blk.header.txCount,
                      blk.header.slot);

        for (int i = 0; i < blk.header.txCount; i++) {
            TxResult r = executeTx(blk.txns[i], blk.header.index,
                                   blk.header.timestamp, false);
            Serial0.printf("  TX[%d] %s: %s\n", i,
                          r.success ? "OK" : "FAIL", r.message.c_str());
            for (int e = 0; e < vm.eventCount(); e++)
                Serial0.printf("    Event[%d] = %u\n", e, vm.event(e).value);
            if (!r.success) {
                Serial0.printf("[Validate] REJECT #%d: included TX[%d] failed\n",
                              blk.header.index, i);
                restoreState(blockSnapScratch);
                if (g_blockchainLedCb) g_blockchainLedCb(1, 8);
                metrics.blocksRejected++;
                return 8;
            }
        }

        recordSkippedSlotsForBlock(blk.header.slot, blk.header.validator);

        uint32_t valBal = getBalance(blk.header.validator);
        setBalance(blk.header.validator, valBal + BLOCK_REWARD + blockFees);
        staking.totalSupply += BLOCK_REWARD;


        staking.recordBlockProduced(blk.header.validator, blk.header.index);


        Hash256 computedRoot = computeStateRoot();
        if (computedRoot != blk.header.stateRoot) {
            Serial0.printf("[Validate] REJECT #%d: stateRoot MISMATCH after re-execution!\n",
                          blk.header.index);
            Serial0.printf("  Expected: %s\n", blk.header.stateRoot.toShort().c_str());
            Serial0.printf("  Computed: %s\n", computedRoot.toShort().c_str());

            restoreState(blockSnapScratch);
            if (g_blockchainLedCb) g_blockchainLedCb(1, 8);
            metrics.blocksRejected++;
            return 8;
        }

        Serial0.printf("[Validate] StateRoot VERIFIED: %s\n",
                      computedRoot.toShort().c_str());


        if (g_blockchainLedCb) g_blockchainLedCb(0, 0);

        applyHostHardwareEffects(blk);


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

            staking.runElection(newEpoch);
            if (g_blockchainLedCb) g_blockchainLedCb(10, 0);
        }

        if (chainLen >= PRUNE_TRIGGER) {
            if (g_blockchainLedCb) g_blockchainLedCb(9, 0);
            pruneChain();
        }

        metrics.blocksValidated++;
        metrics.validateMicrosTotal += micros() - validateStarted;
        return 0;
    }


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


    bool resetToGenesis() {
        Serial0.println("[Reorg] Resetting to genesis for chain reorg");
        resetState();
        initGenesis();
        Serial0.printf("[Reorg] Reset to genesis hash=%s, height=%d\n",
                      chain[0].blockHash.toShort().c_str(), height());
        return true;
    }

    bool contractUsesHardwareOpcode(const Contract& c) const {
        uint16_t pc = 0;
        while (pc < c.codeLen && pc < MVM_MAX_CODE) {
            uint8_t byte = c.code[pc];
            uint8_t op = DECODE_OP(byte);
            uint8_t mod = DECODE_MOD(byte);
            if (op == OP5_GPIO) return true;
            if (op == OP5_PUSH) {
                if (mod == 1) pc += 2;
                else if (mod == 2) pc += 3;
                else if (mod == 3) pc += 5;
                else pc++;
            } else {
                pc++;
            }
        }
        return false;
    }

    void applyHostHardwareEffects(const Block& blk) {
        if (!localNodeSet || g_gpioCallback == nullptr) return;

        bool hasLocalHardwareCall = false;
        for (int i = 0; i < blk.header.txCount; i++) {
            if (blk.txns[i].type != TxType::CALL) continue;
            Contract* c = findContract(blk.txns[i].data);
            if (c && c->active && c->hasCode && c->host == localNode &&
                contractUsesHardwareOpcode(*c)) {
                hasLocalHardwareCall = true;
                break;
            }
        }
        if (!hasLocalHardwareCall) return;

        captureState(hardwareSnapScratch);
        Serial0.printf("[Hardware] Replaying host-only side effects for block #%d\n",
                      blk.header.index);
        for (int i = 0; i < blk.header.txCount; i++) {
            if (blk.txns[i].type != TxType::CALL) continue;
            Contract* c = findContract(blk.txns[i].data);
            if (!c || !c->active || !c->hasCode || c->host != localNode ||
                !contractUsesHardwareOpcode(*c)) {
                continue;
            }
            MVMContext ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.caller = blk.txns[i].sender;
            ctx.argCount = min((uint8_t)blk.txns[i].argCount, (uint8_t)MVM_MAX_ARGS);
            memcpy(ctx.args, blk.txns[i].args, ctx.argCount * sizeof(uint32_t));
            ctx.callValue = blk.txns[i].value;
            ctx.blockNumber = blk.header.index;
            ctx.blockTimestamp = blk.header.timestamp;
            ctx.origin = blk.txns[i].sender;
            ctx.localNode = localNode;
            ctx.localNodeSet = true;
            ctx.allowHardwareEffects = true;
            g_contractResolver = [](const char* name) -> Contract* {
                extern BlockchainState g_chain;
                if (!name) return nullptr;
                if (name[0] == '\x01') {
                    int idx = atoi(name + 1);
                    if (idx >= 0 && idx < g_chain.contractCount)
                        return &g_chain.contracts[idx];
                    return nullptr;
                }
                return g_chain.findContract(name);
            };
            MVMStatus st = vm.execute(*c, ctx);
            metrics.hardwareReplays++;
            Serial0.printf("  HW TX[%d] %s\n", i, statusName(st));
        }
        restoreState(hardwareSnapScratch);
    }


    Hash256 merkleRoot(Hash256* leaves, uint8_t count) const {
        if (count == 0) return ZERO_HASH;
        uint8_t n = count;
        while (n > 1) {
            uint8_t half = (n + 1) / 2;
            for (uint8_t i = 0; i < n / 2; i++) {
                uint8_t pair[HASH_SIZE * 2];
                memcpy(pair, leaves[i * 2].bytes, HASH_SIZE);
                memcpy(pair + HASH_SIZE, leaves[i * 2 + 1].bytes, HASH_SIZE);
                leaves[i] = sha256(pair, sizeof(pair));
            }
            if (n % 2 == 1) leaves[half - 1] = leaves[n - 1];
            n = half;
        }
        return leaves[0];
    }

    Hash256 contractStateHash(const Contract& c) const {
        size_t off = 0;
        memcpy(rootHashBuf + off, c.name, 16); off += 16;
        memcpy(rootHashBuf + off, c.addr.bytes, 4); off += 4;
        memcpy(rootHashBuf + off, c.deployer.id, 6); off += 6;
        memcpy(rootHashBuf + off, c.host.id, 6); off += 6;
        memcpy(rootHashBuf + off, c.codeHash.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(rootHashBuf + off, &c.codeLen, 2); off += 2;
        memcpy(rootHashBuf + off, &c.balance, 4); off += 4;

        uint8_t sn = 0;
        for (int i = 0; i < MVM_MAX_STORAGE; i++)
            if (c.storage[i].used) rootStorageIdx[sn++] = i;
        for (uint8_t a = 1; a < sn; a++) {
            uint8_t key = rootStorageIdx[a];
            int b = a - 1;
            while (b >= 0 && c.storage[rootStorageIdx[b]].key > c.storage[key].key) {
                rootStorageIdx[b + 1] = rootStorageIdx[b];
                b--;
            }
            rootStorageIdx[b + 1] = key;
        }
        for (uint8_t i = 0; i < sn; i++) {
            memcpy(rootHashBuf + off, &c.storage[rootStorageIdx[i]].key, 4); off += 4;
            memcpy(rootHashBuf + off, &c.storage[rootStorageIdx[i]].value, 4); off += 4;
        }
        return sha256(rootHashBuf, off);
    }

    Hash256 computeStateRoot() const {
        uint32_t started = micros();
        metrics.stateRootCalls++;

        uint8_t cn = 0;
        for (int i = 0; i < contractCount; i++)
            if (contracts[i].active) rootContractIdx[cn++] = i;
        for (uint8_t a = 1; a < cn; a++) {
            uint8_t key = rootContractIdx[a];
            int b = a - 1;
            while (b >= 0 && strncmp(contracts[rootContractIdx[b]].name,
                                     contracts[key].name, 16) > 0) {
                rootContractIdx[b + 1] = rootContractIdx[b];
                b--;
            }
            rootContractIdx[b + 1] = key;
        }
        for (uint8_t i = 0; i < cn; i++)
            rootContractLeaves[i] = contractStateHash(contracts[rootContractIdx[i]]);
        Hash256 contractsRoot = merkleRoot(rootContractLeaves, cn);

        uint8_t an = 0;
        for (int i = 0; i < accountCount; i++)
            if (accounts[i].used) rootAccountIdx[an++] = i;
        for (uint8_t a = 1; a < an; a++) {
            uint8_t key = rootAccountIdx[a];
            int b = a - 1;
            while (b >= 0 && memcmp(accounts[rootAccountIdx[b]].owner.id,
                                    accounts[key].owner.id, 6) > 0) {
                rootAccountIdx[b + 1] = rootAccountIdx[b];
                b--;
            }
            rootAccountIdx[b + 1] = key;
        }
        for (uint8_t i = 0; i < an; i++) {
            size_t off = 0;
            const AccountBalance& a = accounts[rootAccountIdx[i]];
            memcpy(rootHashBuf + off, a.owner.id, 6); off += 6;
            memcpy(rootHashBuf + off, &a.balance, 4); off += 4;
            memcpy(rootHashBuf + off, &a.nonce, 4); off += 4;
            memcpy(rootHashBuf + off, &a.lastFaucet, 4); off += 4;
            rootAccountLeaves[i] = sha256(rootHashBuf, off);
        }
        Hash256 accountsRoot = merkleRoot(rootAccountLeaves, an);

        uint8_t sn = 0;
        for (int i = 0; i < staking.stakeCount; i++)
            if (staking.stakes[i].used) rootStakeIdx[sn++] = i;
        for (uint8_t a = 1; a < sn; a++) {
            uint8_t key = rootStakeIdx[a];
            int b = a - 1;
            while (b >= 0 && memcmp(staking.stakes[rootStakeIdx[b]].staker.id,
                                    staking.stakes[key].staker.id, 6) > 0) {
                rootStakeIdx[b + 1] = rootStakeIdx[b];
                b--;
            }
            rootStakeIdx[b + 1] = key;
        }
        for (uint8_t i = 0; i < sn; i++) {
            size_t off = 0;
            const StakeInfo& s = staking.stakes[rootStakeIdx[i]];
            memcpy(rootHashBuf + off, s.staker.id, 6); off += 6;
            memcpy(rootHashBuf + off, &s.stakedAmount, 4); off += 4;
            memcpy(rootHashBuf + off, &s.unstakingAmount, 4); off += 4;
            memcpy(rootHashBuf + off, &s.unstakeBlock, 4); off += 4;
            rootHashBuf[off++] = s.isCandidate ? 1 : 0;
            rootHashBuf[off++] = s.jailed ? 1 : 0;
            memcpy(rootHashBuf + off, &s.jailUntilEpoch, 4); off += 4;
            memcpy(rootHashBuf + off, &s.slashCount, 4); off += 4;
            memcpy(rootHashBuf + off, &s.missedBlocks, 4); off += 4;
            memcpy(rootHashBuf + off, &s.lastProducedBlock, 4); off += 4;
            rootStakeLeaves[i] = sha256(rootHashBuf, off);
        }
        Hash256 stakesRoot = merkleRoot(rootStakeLeaves, sn);

        for (uint8_t i = 0; i < staking.activeCount; i++)
            rootActiveLeaves[i] = sha256(staking.activeSet[i].id, 6);
        Hash256 activeRoot = merkleRoot(rootActiveLeaves, staking.activeCount);

        size_t off = 0;
        memcpy(rootHashBuf + off, contractsRoot.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(rootHashBuf + off, accountsRoot.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(rootHashBuf + off, stakesRoot.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(rootHashBuf + off, activeRoot.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(rootHashBuf + off, &staking.totalStaked, 4); off += 4;
        memcpy(rootHashBuf + off, &staking.totalSupply, 4); off += 4;
        memcpy(rootHashBuf + off, &staking.currentEpoch, 4); off += 4;
        rootHashBuf[off++] = staking.activeCount;

        Hash256 root = sha256(rootHashBuf, off);
        metrics.stateRootMicrosTotal += micros() - started;
        return root;
    }


    bool addToMempool(Transaction tx, bool localSubmit = false) {
        if (localSubmit) {
            if (!TxAuth::sign(tx)) return false;
        }
        if (!TxAuth::verify(tx)) return false;
        if (tx.nonce < getNonce(tx.sender)) return false;
        if (mempoolSize >= MAX_PENDING_TX) return false;
        for (int i = 0; i < mempoolSize; i++)
            if (mempool[i].sender == tx.sender && mempool[i].nonce == tx.nonce)
                return false;
        mempool[mempoolSize++] = tx;
        return true;
    }

    uint8_t drainMempool(Transaction* out, uint8_t maxTx) {


        for (uint8_t a = 1; a < mempoolSize; a++) {
            Transaction key = mempool[a];
            int b = a - 1;
            while (b >= 0) {
                int cmp = memcmp(mempool[b].sender.id, key.sender.id, 6);
                if (cmp > 0 || (cmp == 0 && mempool[b].nonce > key.nonce)) {
                    mempool[b + 1] = mempool[b];
                    b--;
                } else {
                    break;
                }
            }
            mempool[b + 1] = key;
        }

        bool remove[MAX_PENDING_TX];
        memset(remove, 0, sizeof(remove));
        NodeID selectedSenders[MAX_TXN_PER_BLOCK];
        uint32_t nextNonce[MAX_TXN_PER_BLOCK];
        uint8_t senderCnt = 0;
        uint8_t count = 0;

        for (uint8_t i = 0; i < mempoolSize && count < maxTx; i++) {
            uint32_t baseNonce = getNonce(mempool[i].sender);
            if (mempool[i].nonce < baseNonce) {
                remove[i] = true;
                continue;
            }

            int senderIdx = -1;
            for (uint8_t s = 0; s < senderCnt; s++) {
                if (selectedSenders[s] == mempool[i].sender) {
                    senderIdx = s;
                    break;
                }
            }
            if (senderIdx < 0) {
                if (senderCnt >= MAX_TXN_PER_BLOCK) continue;
                senderIdx = senderCnt++;
                selectedSenders[senderIdx] = mempool[i].sender;
                nextNonce[senderIdx] = baseNonce;
            }

            if (mempool[i].nonce == nextNonce[senderIdx]) {
                out[count++] = mempool[i];
                nextNonce[senderIdx]++;
                remove[i] = true;
            }
        }

        uint8_t w = 0;
        for (uint8_t r = 0; r < mempoolSize; r++) {
            if (!remove[r]) mempool[w++] = mempool[r];
        }
        mempoolSize = w;
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
        uint32_t toPrune = chainLen;
        if (toPrune == 0) return;
        metrics.pruneCount++;

        Serial0.printf("\n==== PRUNING: deleting %d blocks [%d..%d], keeping %d ====\n",
                      toPrune, chainOffset, chainOffset + toPrune - 1, 0);

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
            prevCkpt = checkpoints[checkpointCount - 1].contentHash();

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

        Serial0.println("  Checkpoint represents current tip state");

        memset(chain, 0, sizeof(chain));
        chainOffset += toPrune;
        chainLen = 0;

        uint32_t memEnd = chainLen > 0 ? chainOffset + chainLen - 1 : chainOffset;
        Serial0.printf("  Chain now: blocks [%d..%d] (%d in memory)\n",
                      chainOffset, memEnd, chainLen);
        Serial0.printf("  Total logical height: %d\n", height());
        Serial0.printf("  Finalized headers: %d\n", finalizedCount);
        Serial0.printf("  Checkpoints: %d (covering blocks 0..%d)\n",
                      checkpointCount, chainOffset - 1);


        if (g_nvsWriteCallback) {
            static uint8_t nvsBuf[MAX_CHECKPOINT_STATE_BYTES];
            uint16_t nvsLen = serializeState(nvsBuf, sizeof(nvsBuf));
            if (nvsLen > 0) {
                metrics.lastPruneBytes = nvsLen;
                g_nvsWriteCallback(nvsBuf, nvsLen);
                Serial0.printf("  NVS: saved %d bytes of chain state\n", nvsLen);
            }
        }

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
        uint32_t memEnd = chainLen > 0 ? chainOffset + chainLen - 1 : chainOffset;
        Serial0.printf("Blocks in memory: [%d..%d] (%d blocks)\n",
                      chainOffset, memEnd, chainLen);
        Serial0.printf("Total logical height: %d\n", height());
        Serial0.println("=======================================\n");
    }


    const Block& lastBlock() const { return chain[chainLen-1]; }
    Hash256 tipHash() const {
        if (chainLen > 0) return chain[chainLen - 1].blockHash;
        if (chainOffset > 0 && checkpointCount > 0)
            return checkpoints[checkpointCount - 1].lastBlockHash;
        return ZERO_HASH;
    }
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
        uint32_t memEnd = chainLen > 0 ? chainOffset + chainLen - 1 : chainOffset;
        Serial0.printf("In memory: %d blocks [%d..%d] | Logical height: %d | Epoch: %d\n",
                      chainLen, chainOffset, memEnd,
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
