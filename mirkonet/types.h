#pragma once
#include <Arduino.h>
#include <mbedtls/sha256.h>
#include "config.h"

struct NodeID {
    uint8_t id[6];

    bool operator==(const NodeID& o) const { return memcmp(id, o.id, 6) == 0; }
    bool operator!=(const NodeID& o) const { return !(*this == o); }

    String toString() const {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 id[0], id[1], id[2], id[3], id[4], id[5]);
        return String(buf);
    }

    String toShortStr() const {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02X%02X%02X", id[3], id[4], id[5]);
        return String(buf);
    }

    uint32_t hash32() const {
        return (uint32_t)id[0] | ((uint32_t)id[1] << 8) |
               ((uint32_t)id[2] << 16) | ((uint32_t)id[3] << 24);
    }

    bool isZero() const {
        for (int i = 0; i < 6; i++) if (id[i]) return false;
        return true;
    }
};

static const NodeID ZERO_NODE = {0};

struct Hash256 {
    uint8_t bytes[HASH_SIZE];

    bool operator==(const Hash256& o) const { return memcmp(bytes, o.bytes, HASH_SIZE) == 0; }
    bool operator!=(const Hash256& o) const { return !(*this == o); }
    bool isZero() const {
        for (int i = 0; i < HASH_SIZE; i++) if (bytes[i]) return false;
        return true;
    }

    String toShort() const {
        char buf[12];
        snprintf(buf, sizeof(buf), "%02x%02x..%02x%02x",
                 bytes[0], bytes[1], bytes[30], bytes[31]);
        return String(buf);
    }

    String toHex() const {
        char buf[65];
        for (int i = 0; i < 32; i++) snprintf(buf + i*2, 3, "%02x", bytes[i]);
        return String(buf);
    }
};

static const Hash256 ZERO_HASH = {0};

inline Hash256 sha256(const uint8_t* data, size_t len) {
    Hash256 h;
    mbedtls_sha256(data, len, h.bytes, 0);
    return h;
}

enum class TxType : uint8_t {
    DATA        = 0x01,
    DEPLOY      = 0x02,
    CALL        = 0x03,
    TRANSFER    = 0x04,
    STAKE       = 0x10,
    UNSTAKE     = 0x11,
    CLAIM       = 0x14,
    FAUCET      = 0x15,
};

struct Transaction {
    TxType   type;
    NodeID   sender;      // Ethereum: from (derived from signature)
    NodeID   to;          // Ethereum: to (zero = contract deploy)
    uint32_t nonce;
    uint32_t timestamp;
    uint32_t value;       // Ethereum: value (token amount)
    uint32_t gasLimit;    // Ethereum: gasLimit (max gas for this tx)
    uint32_t gasPrice;    // Ethereum: gasPrice (tokens per gas unit)
    char     data[16];    // Ethereum: data (contract name for CALL/DEPLOY)
    uint32_t args[MVM_MAX_ARGS];
    uint8_t  argCount;

    Hash256 hash() const {
        uint8_t buf[80];
        size_t off = 0;
        buf[off++] = (uint8_t)type;
        memcpy(buf + off, sender.id, 6); off += 6;
        memcpy(buf + off, to.id, 6); off += 6;
        memcpy(buf + off, &nonce, 4); off += 4;
        memcpy(buf + off, &timestamp, 4); off += 4;
        memcpy(buf + off, &value, 4); off += 4;
        memcpy(buf + off, &gasLimit, 4); off += 4;
        memcpy(buf + off, &gasPrice, 4); off += 4;
        memcpy(buf + off, data, 16); off += 16;
        buf[off++] = argCount;
        return sha256(buf, off);
    }
};

#define DEPLOY_CACHE_SIZE 4

// cache deployed code
struct DeployCache {
    struct Entry {
        char     name[16];
        uint8_t  code[MVM_MAX_CODE];
        uint16_t codeLen;
        Hash256  codeHash;
        bool     used;
        bool     hasCode;
    };
    Entry entries[DEPLOY_CACHE_SIZE];

    void clear() { memset(entries, 0, sizeof(entries)); }

    bool store(const char* name, const uint8_t* code, uint16_t len) {
        Entry* e = findOrAlloc(name);
        if (!e) return false;
        strncpy(e->name, name, 15); e->name[15] = '\0';
        memcpy(e->code, code, len);
        e->codeLen = len;
        e->codeHash = sha256(code, len);
        e->used = true;
        e->hasCode = true;
        return true;
    }

    bool storeMeta(const char* name, uint16_t codeLen, const Hash256& hash) {
        Entry* e = findOrAlloc(name);
        if (!e) return false;
        // Don't overwrite an entry that already has actual bytecode
        if (e->used && e->hasCode) return true;
        strncpy(e->name, name, 15); e->name[15] = '\0';
        e->codeLen = codeLen;
        e->codeHash = hash;
        e->used = true;
        e->hasCode = false;
        return true;
    }

    Entry* find(const char* name) {
        for (int i = 0; i < DEPLOY_CACHE_SIZE; i++)
            if (entries[i].used && strcmp(entries[i].name, name) == 0)
                return &entries[i];
        return nullptr;
    }

private:
    Entry* findOrAlloc(const char* name) {
        for (int i = 0; i < DEPLOY_CACHE_SIZE; i++)
            if (entries[i].used && strcmp(entries[i].name, name) == 0)
                return &entries[i];
        for (int i = 0; i < DEPLOY_CACHE_SIZE; i++)
            if (!entries[i].used) return &entries[i];
        return nullptr;
    }
};

struct BlockHeader {
    uint32_t index;
    uint32_t timestamp;
    Hash256  prevHash;
    Hash256  stateRoot;
    NodeID   validator;
    uint8_t  txCount;
    uint32_t slot;
    uint32_t epoch;
    uint32_t reward;

    Hash256 hash() const {
        uint8_t buf[140];
        size_t off = 0;
        memcpy(buf + off, &index, 4); off += 4;
        memcpy(buf + off, &timestamp, 4); off += 4;
        memcpy(buf + off, prevHash.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, stateRoot.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, validator.id, 6); off += 6;
        buf[off++] = txCount;
        memcpy(buf + off, &slot, 4); off += 4;
        memcpy(buf + off, &epoch, 4); off += 4;
        memcpy(buf + off, &reward, 4); off += 4;
        return sha256(buf, off);
    }
};

struct Block {
    BlockHeader header;
    Hash256     blockHash;
    Transaction txns[MAX_TXN_PER_BLOCK];

    void computeHash() {
        uint8_t buf[HASH_SIZE * (1 + MAX_TXN_PER_BLOCK)];
        Hash256 hh = header.hash();
        memcpy(buf, hh.bytes, HASH_SIZE);
        for (int i = 0; i < header.txCount; i++) {
            Hash256 th = txns[i].hash();
            memcpy(buf + HASH_SIZE * (1 + i), th.bytes, HASH_SIZE);
        }
        blockHash = sha256(buf, HASH_SIZE * (1 + header.txCount));
    }
};

struct PeerInfo {
    NodeID    nodeId;
    IPAddress ip;
    uint32_t  lastSeen;
    uint32_t  chainHeight;
    NodeRole  role;
    bool      active;

    bool isAlive() const { return active && (millis() - lastSeen < PEER_TIMEOUT); }
};

struct StakeInfo {
    NodeID   staker;
    uint32_t stakedAmount;
    uint32_t unstakingAmount;
    uint32_t unstakeBlock;
    bool     isCandidate;
    bool     used;
    // Slashing state
    bool     jailed;            // validator is jailed and cannot produce blocks
    uint32_t jailUntilEpoch;    // epoch when jail ends
    uint32_t slashCount;        // total times slashed
    uint32_t missedBlocks;      // consecutive missed blocks (for downtime detection)
    uint32_t lastProducedBlock; // last block this validator produced
};


struct ContractAddr {
    uint8_t bytes[4];

    static ContractAddr fromName(const char* name) {
        ContractAddr a;
        Hash256 h = sha256((const uint8_t*)name, strlen(name));
        memcpy(a.bytes, h.bytes, 4);
        return a;
    }

    String toHex() const {
        char buf[12];
        snprintf(buf, sizeof(buf), "0x%02x%02x%02x%02x",
                 bytes[0], bytes[1], bytes[2], bytes[3]);
        return String(buf);
    }

    bool operator==(const ContractAddr& o) const { return memcmp(bytes, o.bytes, 4) == 0; }
};

struct Checkpoint {
    uint32_t fromBlock;
    uint32_t toBlock;
    Hash256  lastBlockHash;
    Hash256  stateRoot;
    Hash256  chainMerkle;
    Hash256  prevCheckpoint;
    uint32_t timestamp;

    bool used = false;

    Hash256 hash() const {
        uint8_t buf[148];
        size_t off = 0;
        memcpy(buf + off, &fromBlock, 4); off += 4;
        memcpy(buf + off, &toBlock, 4); off += 4;
        memcpy(buf + off, lastBlockHash.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, stateRoot.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, chainMerkle.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, prevCheckpoint.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf + off, &timestamp, 4); off += 4;
        return sha256(buf, off);
    }

    void print() const {
        if (!used) return;
        Serial0.printf("  Checkpoint [%d..%d] last=%s state=%s merkle=%s\n",
                      fromBlock, toBlock,
                      lastBlockHash.toShort().c_str(),
                      stateRoot.toShort().c_str(),
                      chainMerkle.toShort().c_str());
    }
};

struct CompactHeader {
    uint32_t index;
    uint32_t timestamp;
    Hash256  blockHash;
    Hash256  prevHash;
    NodeID   validator;
    uint8_t  txCount;
    uint32_t reward;

    static CompactHeader fromBlock(const Block& blk) {
        CompactHeader ch;
        ch.index     = blk.header.index;
        ch.timestamp = blk.header.timestamp;
        ch.blockHash = blk.blockHash;
        ch.prevHash  = blk.header.prevHash;
        ch.validator = blk.header.validator;
        ch.txCount   = blk.header.txCount;
        ch.reward    = blk.header.reward;
        return ch;
    }
};

enum class MsgType : uint8_t {
    DISCOVERY    = 0x01,
    HEARTBEAT    = 0x02,
    TX_GOSSIP    = 0x03,
    BLOCK_ANN    = 0x04,
    BLOCK_REQ    = 0x05,
    BLOCK_RESP   = 0x06,
    CHAIN_REQ    = 0x07,
    EPOCH_ANN    = 0x08,
    DEPLOY_DATA  = 0x09,
    BLOCK_FULL   = 0x0A,
    CONTRACT_INFO= 0x0B,
    CODE_REQ     = 0x0C,
    CODE_RESP    = 0x0D,
    CHECKPOINT_REQ  = 0x0E,
    CHECKPOINT_RESP = 0x0F,
};
