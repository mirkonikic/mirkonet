#pragma once
#include <WiFi.h>
#include <WiFiUdp.h>
#include "types.h"
#include "config.h"

class P2PNetwork {
public:
    struct NetMetrics {
        uint32_t bytesTx = 0;
        uint32_t bytesRx = 0;
        uint32_t txGossipSent = 0;
        uint32_t blockGossipSent = 0;
        uint32_t blockRequests = 0;
        uint32_t blockResponses = 0;
        uint32_t checkpointRequests = 0;
        uint32_t checkpointResponses = 0;
        uint32_t codeRequests = 0;
        uint32_t codeResponses = 0;
    };

    PeerInfo peers[MAX_PEERS];
    uint8_t  peerCount = 0;
    NodeID   selfId;
    NetMetrics metrics;

    bool init(const NodeID& self) {
        selfId = self;
        _udpDisc.begin(UDP_DISCOVERY_PORT);
        _udpGossip.begin(UDP_GOSSIP_PORT);
        _tcpServer.begin(TCP_SYNC_PORT);
        _tcpServer.setNoDelay(true);
        Serial0.printf("[Net] Listening UDP:%d,%d TCP:%d\n", UDP_DISCOVERY_PORT, UDP_GOSSIP_PORT, TCP_SYNC_PORT);
        return true;
    }

    PeerInfo* findPeer(const NodeID& id) {
        for (int i = 0; i < peerCount; i++)
            if (peers[i].active && peers[i].nodeId == id) return &peers[i];
        return nullptr;
    }

    PeerInfo* addOrUpdatePeer(const NodeID& id, IPAddress ip, uint32_t height,
                              NodeRole role, const Hash256& tipHash = ZERO_HASH) {
        if (id == selfId) return nullptr;
        PeerInfo* p = findPeer(id);
        if (p) {
            p->lastSeen = millis(); p->ip = ip;
            p->chainHeight = height; p->tipHash = tipHash; p->role = role;
            return p;
        }
        if (peerCount < MAX_PEERS) {
            PeerInfo& np = peers[peerCount++];
            np = { id, ip, millis(), height, tipHash, role, true };
            Serial0.printf("[Net] New peer: %s @ %s (%s) h=%d\n",
                          id.toShortStr().c_str(), ip.toString().c_str(),
                          roleName(role), height);
            return &np;
        }
        for (int i = 0; i < peerCount; i++) {
            if (!peers[i].isAlive()) {
                peers[i] = { id, ip, millis(), height, tipHash, role, true };
                return &peers[i];
            }
        }
        return nullptr;
    }

    uint8_t countAlive() const {
        uint8_t n = 0;
        for (int i = 0; i < peerCount; i++) if (peers[i].isAlive()) n++;
        return n;
    }


    void sendDiscovery(uint32_t height, NodeRole role, uint32_t epoch,
                       uint32_t genesisElapsed, const Hash256& tipHash) {
        uint8_t buf[64]; size_t off = 0;
        buf[off++] = (uint8_t)MsgType::DISCOVERY;
        memcpy(buf+off, selfId.id, 6); off += 6;
        memcpy(buf+off, &height, 4); off += 4;
        buf[off++] = (uint8_t)role;
        memcpy(buf+off, &epoch, 4); off += 4;
        memcpy(buf+off, &genesisElapsed, 4); off += 4;
        memcpy(buf+off, tipHash.bytes, HASH_SIZE); off += HASH_SIZE;
        _udpDisc.beginPacket(IPAddress(255,255,255,255), UDP_DISCOVERY_PORT);
        _udpDisc.write(buf, off);
        _udpDisc.endPacket();
    }

    void sendHeartbeat(uint32_t height, uint32_t slot, const Hash256& tipHash) {
        uint8_t buf[64]; size_t off = 0;
        buf[off++] = (uint8_t)MsgType::HEARTBEAT;
        memcpy(buf+off, selfId.id, 6); off += 6;
        memcpy(buf+off, &height, 4); off += 4;
        memcpy(buf+off, &slot, 4); off += 4;
        memcpy(buf+off, tipHash.bytes, HASH_SIZE); off += HASH_SIZE;
        _udpGossip.beginPacket(IPAddress(255,255,255,255), UDP_GOSSIP_PORT);
        _udpGossip.write(buf, off);
        _udpGossip.endPacket();
    }

    void broadcastTx(const Transaction& tx) {
        uint8_t buf[512]; size_t off = 0;
        buf[off++] = (uint8_t)MsgType::TX_GOSSIP;
        memcpy(buf+off, selfId.id, 6); off += 6;
        serializeTx(tx, buf, off);
        _udpGossip.beginPacket(IPAddress(255,255,255,255), UDP_GOSSIP_PORT);
        _udpGossip.write(buf, off);
        _udpGossip.endPacket();
        metrics.bytesTx += off;
        metrics.txGossipSent++;
        if (tx.type == TxType::DEPLOY)
            Serial0.printf("[Net] Broadcast DEPLOY tx '%s' (%d bytes)\n", tx.data, (int)off);
    }

    void broadcastFullBlock(const Block& blk) {
        static uint8_t buf[1500]; size_t off = 0;
        buf[off++] = (uint8_t)MsgType::BLOCK_FULL;
        memcpy(buf+off, selfId.id, 6); off += 6;
        serializeBlock(blk, buf, off);
        Serial0.printf("[Net] Broadcasting full block #%d (%d bytes)\n",
                      blk.header.index, (int)off);
        _udpGossip.beginPacket(IPAddress(255,255,255,255), UDP_GOSSIP_PORT);
        _udpGossip.write(buf, off);
        _udpGossip.endPacket();
        metrics.bytesTx += off;
        metrics.blockGossipSent++;
    }


    void broadcastContractInfo(const char* name, const NodeID& host, const Hash256& codeHash, uint16_t codeLen) {
        uint8_t buf[128]; size_t off = 0;
        buf[off++] = (uint8_t)MsgType::CONTRACT_INFO;
        memcpy(buf+off, selfId.id, 6); off += 6;

        memcpy(buf+off, name, 16); off += 16;
        memcpy(buf+off, host.id, 6); off += 6;
        memcpy(buf+off, codeHash.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf+off, &codeLen, 2); off += 2;
        _udpGossip.beginPacket(IPAddress(255,255,255,255), UDP_GOSSIP_PORT);
        _udpGossip.write(buf, off);
        _udpGossip.endPacket();
        metrics.bytesTx += off;
        Serial0.printf("[Net] Broadcast CONTRACT_INFO '%s' hosted by %s (%dB)\n",
                      name, host.toShortStr().c_str(), codeLen);
    }

    bool requestBlock(IPAddress peerIP, uint32_t blockIndex, Block& outBlock) {
        WiFiClient client;
        metrics.blockRequests++;
        Serial0.printf("[Sync] Requesting block #%d from %s...\n",
                      blockIndex, peerIP.toString().c_str());

        if (!client.connect(peerIP, TCP_SYNC_PORT, 3000)) {
            Serial0.println("[Sync] TCP connect failed");
            return false;
        }

        uint8_t req[16]; size_t off = 0;
        req[off++] = (uint8_t)MsgType::BLOCK_REQ;
        memcpy(req+off, selfId.id, 6); off += 6;
        memcpy(req+off, &blockIndex, 4); off += 4;
        client.write(req, off);
        client.flush();

        static uint8_t buf[1500];
        int len = 0;
        uint32_t start = millis();
        while (millis() - start < 3000) {
            int avail = client.available();
            if (avail > 0) {
                int chunk = client.read(buf + len, sizeof(buf) - len);
                len += chunk;
                metrics.bytesRx += chunk;
                start = millis();
            }
            if (len > 20 && !client.available()) break;
            delay(5);
        }
        client.stop();

        if (len < 10 || (MsgType)buf[0] != MsgType::BLOCK_RESP) {
            Serial0.printf("[Sync] Bad response (%d bytes)\n", len);
            return false;
        }

        size_t roff = 7;
        deserializeBlock(buf + roff, len - roff, outBlock);

        Serial0.printf("[Sync] Got block #%d (%d txns) hash=%s\n",
                      outBlock.header.index, outBlock.header.txCount,
                      outBlock.blockHash.toShort().c_str());
        return true;
    }

    void sendBlock(WiFiClient& client, const Block& blk) {
        static uint8_t buf[1500]; size_t off = 0;
        buf[off++] = (uint8_t)MsgType::BLOCK_RESP;
        memcpy(buf+off, selfId.id, 6); off += 6;
        serializeBlock(blk, buf, off);
        client.write(buf, off);
        client.flush();
        metrics.bytesTx += off;
        metrics.blockResponses++;
    }

    struct RecvMsg {
        MsgType   type;
        NodeID    sender;
        IPAddress senderIP;
        uint8_t   payload[1500];
        uint16_t  payloadLen;
        bool      valid;
    };

    RecvMsg recvDiscovery() { return recvUDP(_udpDisc); }
    RecvMsg recvGossip()    { return recvUDP(_udpGossip); }

    bool handleTCPSync(WiFiClient& out) {
        WiFiClient c = _tcpServer.available();
        if (c) { out = c; return true; }
        return false;
    }

    void printPeers() const {
        Serial0.println("\n====== Peers ======");
        for (int i = 0; i < peerCount; i++) {
            if (!peers[i].active) continue;
            Serial0.printf("  %s @ %s | h=%d | %s | %s\n",
                          peers[i].nodeId.toString().c_str(),
                          peers[i].ip.toString().c_str(),
                          peers[i].chainHeight,
                          roleName(peers[i].role),
                          peers[i].isAlive() ? "alive" : "DEAD");
        }
        Serial0.printf("Active: %d\n", countAlive());
        Serial0.println("===================\n");
    }

    static void serializeBlock(const Block& blk, uint8_t* buf, size_t& off) {
        memcpy(buf+off, &blk.header.index, 4); off += 4;
        memcpy(buf+off, &blk.header.timestamp, 4); off += 4;
        memcpy(buf+off, blk.header.prevHash.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf+off, blk.header.stateRoot.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf+off, blk.header.validator.id, 6); off += 6;
        buf[off++] = blk.header.txCount;
        memcpy(buf+off, &blk.header.slot, 4); off += 4;
        memcpy(buf+off, &blk.header.epoch, 4); off += 4;
        memcpy(buf+off, &blk.header.reward, 4); off += 4;
        memcpy(buf+off, blk.blockHash.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf+off, blk.validatorPublicKey, 64); off += 64;
        memcpy(buf+off, blk.validatorSignature, 64); off += 64;
        for (int i = 0; i < blk.header.txCount; i++)
            serializeTx(blk.txns[i], buf, off);
    }

    static void deserializeBlock(const uint8_t* data, uint16_t len, Block& blk) {
        memset(&blk, 0, sizeof(Block));
        size_t off = 0;
        if (off+4<=len) { memcpy(&blk.header.index, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&blk.header.timestamp, data+off, 4); off+=4; }
        if (off+HASH_SIZE<=len) { memcpy(blk.header.prevHash.bytes, data+off, HASH_SIZE); off+=HASH_SIZE; }
        if (off+HASH_SIZE<=len) { memcpy(blk.header.stateRoot.bytes, data+off, HASH_SIZE); off+=HASH_SIZE; }
        if (off+6<=len) { memcpy(blk.header.validator.id, data+off, 6); off+=6; }
        if (off+1<=len) { blk.header.txCount = data[off++]; }
        if (blk.header.txCount > MAX_TXN_PER_BLOCK)
            blk.header.txCount = MAX_TXN_PER_BLOCK;
        if (off+4<=len) { memcpy(&blk.header.slot, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&blk.header.epoch, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&blk.header.reward, data+off, 4); off+=4; }
        if (off+HASH_SIZE<=len) { memcpy(blk.blockHash.bytes, data+off, HASH_SIZE); off+=HASH_SIZE; }
        if (off+64<=len) { memcpy(blk.validatorPublicKey, data+off, 64); off+=64; }
        if (off+64<=len) { memcpy(blk.validatorSignature, data+off, 64); off+=64; }
        for (int i = 0; i < blk.header.txCount && off < len; i++) {
            deserializeTx(data + off, len - off, blk.txns[i]);
            off += txSerializedSize(blk.txns[i]);
        }
    }

    static void serializeTx(const Transaction& tx, uint8_t* buf, size_t& off) {
        buf[off++] = (uint8_t)tx.type;
        memcpy(buf+off, tx.sender.id, 6); off += 6;
        memcpy(buf+off, tx.to.id, 6); off += 6;
        memcpy(buf+off, &tx.nonce, 4); off += 4;
        memcpy(buf+off, &tx.timestamp, 4); off += 4;
        memcpy(buf+off, &tx.value, 4); off += 4;
        memcpy(buf+off, &tx.gasLimit, 4); off += 4;
        memcpy(buf+off, &tx.gasPrice, 4); off += 4;
        memcpy(buf+off, tx.data, 16); off += 16;
        buf[off++] = tx.argCount;
        for (int i = 0; i < tx.argCount; i++)
            memcpy(buf+off, &tx.args[i], 4), off += 4;
        memcpy(buf+off, &tx.deployCodeLen, 2); off += 2;
        memcpy(buf+off, tx.deployCodeHash.bytes, HASH_SIZE); off += HASH_SIZE;
        memcpy(buf+off, tx.publicKey, 64); off += 64;
        memcpy(buf+off, tx.signature, 64); off += 64;
    }

    static void deserializeTx(const uint8_t* data, uint16_t len, Transaction& tx) {
        size_t off = 0;
        memset(&tx, 0, sizeof(tx));
        if (len < 1) return;
        tx.type = (TxType)data[off++];
        if (off+6<=len) { memcpy(tx.sender.id, data+off, 6); off+=6; }
        if (off+6<=len) { memcpy(tx.to.id, data+off, 6); off+=6; }
        if (off+4<=len) { memcpy(&tx.nonce, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&tx.timestamp, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&tx.value, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&tx.gasLimit, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&tx.gasPrice, data+off, 4); off+=4; }
        if (off+16<=len) { memcpy(tx.data, data+off, 16); off+=16; }
        if (off+1<=len) { tx.argCount = data[off++]; }
        if (tx.argCount > MVM_MAX_ARGS)
            tx.argCount = MVM_MAX_ARGS;
        for (int i=0; i<tx.argCount && off+4<=len; i++)
            memcpy(&tx.args[i], data+off, 4), off+=4;
        if (off+2+HASH_SIZE+64+64 <= len) {
            memcpy(&tx.deployCodeLen, data+off, 2); off += 2;
            memcpy(tx.deployCodeHash.bytes, data+off, HASH_SIZE); off += HASH_SIZE;
            memcpy(tx.publicKey, data+off, 64); off += 64;
            memcpy(tx.signature, data+off, 64); off += 64;
            if (tx.type == TxType::DEPLOY && tx.deployCodeLen > 0)
                g_deployCache.storeMeta(tx.data, tx.deployCodeLen, tx.deployCodeHash);
        }
    }

    static size_t txSerializedSize(const Transaction& tx) {
        return 1 + 6 + 6 + 4 + 4 + 4 + 4 + 4 + 16 + 1 +
               tx.argCount * 4 + 2 + HASH_SIZE + 64 + 64;
    }


    bool requestCheckpoint(IPAddress peerIP, uint8_t* outBuf, uint16_t& outLen) {
        WiFiClient client;
        metrics.checkpointRequests++;
        Serial0.printf("[Sync] Requesting checkpoint from %s...\n",
                      peerIP.toString().c_str());

        if (!client.connect(peerIP, TCP_SYNC_PORT, 3000)) {
            Serial0.println("[Sync] TCP connect failed for checkpoint");
            return false;
        }

        uint8_t req[8]; size_t off = 0;
        req[off++] = (uint8_t)MsgType::CHECKPOINT_REQ;
        memcpy(req+off, selfId.id, 6); off += 6;
        client.write(req, off);
        client.flush();

        int len = 0;
        uint32_t start = millis();
        while (millis() - start < 5000) {
            int avail = client.available();
            if (avail > 0) {
                int chunk = client.read(outBuf + len, MAX_CHECKPOINT_STATE_BYTES - len);
                len += chunk;
                metrics.bytesRx += chunk;
                start = millis();
            }
            if (len > 20 && !client.available()) break;
            delay(5);
        }
        client.stop();

        if (len < 10 || (MsgType)outBuf[0] != MsgType::CHECKPOINT_RESP) {
            Serial0.printf("[Sync] Bad checkpoint response (%d bytes)\n", len);
            return false;
        }


        outLen = len - 7;
        memmove(outBuf, outBuf + 7, outLen);

        Serial0.printf("[Sync] Got checkpoint snapshot (%d bytes)\n", outLen);
        return true;
    }

    bool requestCode(IPAddress hostIP, const char* name,
                     uint8_t* outCode, uint16_t& outLen) {
        WiFiClient client;
        metrics.codeRequests++;
        Serial0.printf("[Code] Requesting bytecode '%s' from %s\n",
                      name, hostIP.toString().c_str());

        if (!client.connect(hostIP, TCP_SYNC_PORT, 3000)) {
            Serial0.println("[Code] TCP connect failed");
            return false;
        }

        uint8_t req[32]; size_t off = 0;
        req[off++] = (uint8_t)MsgType::CODE_REQ;
        memcpy(req+off, selfId.id, 6); off += 6;
        memcpy(req+off, name, 16); off += 16;
        client.write(req, off);
        client.flush();

        uint8_t buf[512];
        int len = 0;
        int expectedLen = 9;
        uint32_t start = millis();
        while (millis() - start < 3000) {
            int avail = client.available();
            if (avail > 0) {
                int chunk = client.read(buf + len, sizeof(buf) - len);
                len += chunk;
                metrics.bytesRx += chunk;
                start = millis();
            }
            if (len >= 9 && expectedLen == 9) {
                uint16_t announcedLen = 0;
                memcpy(&announcedLen, buf + 7, 2);
                if (announcedLen > 0 && announcedLen <= MVM_MAX_CODE)
                    expectedLen = 9 + announcedLen;
            }
            if (len >= expectedLen) break;
            delay(5);
        }
        client.stop();

        if (len < 9 || (MsgType)buf[0] != MsgType::CODE_RESP) {
            Serial0.printf("[Code] Bad response (%d bytes)\n", len);
            return false;
        }

        size_t roff = 7;
        memcpy(&outLen, buf+roff, 2); roff += 2;
        if (outLen == 0 || outLen > MVM_MAX_CODE || roff + outLen > (size_t)len) {
            Serial0.println("[Code] Invalid code length");
            return false;
        }
        memcpy(outCode, buf+roff, outLen);

        Serial0.printf("[Code] Got '%s' bytecode (%d bytes)\n", name, outLen);
        return true;
    }

private:
    WiFiUDP    _udpDisc;
    WiFiUDP    _udpGossip;
    WiFiServer _tcpServer{TCP_SYNC_PORT};

    RecvMsg recvUDP(WiFiUDP& udp) {
        RecvMsg msg; msg.valid = false;
        int sz = udp.parsePacket();
        if (sz <= 0) return msg;
        static uint8_t buf[1500];
        int len = udp.read(buf, sizeof(buf));
        if (len < 7) return msg;
        metrics.bytesRx += len;
        msg.type = (MsgType)buf[0];
        memcpy(msg.sender.id, buf+1, 6);
        msg.senderIP = udp.remoteIP();
        msg.payloadLen = len - 7;
        if (msg.payloadLen > 0) memcpy(msg.payload, buf+7, msg.payloadLen);
        if (msg.sender == selfId) return msg;
        msg.valid = true;
        return msg;
    }
};
