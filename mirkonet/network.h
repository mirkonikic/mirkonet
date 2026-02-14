#pragma once
#include <WiFi.h>
#include <WiFiUdp.h>
#include "types.h"
#include "config.h"

class P2PNetwork {
public:
    PeerInfo peers[MAX_PEERS];
    uint8_t  peerCount = 0;
    NodeID   selfId;

    bool init(const NodeID& self) {
        selfId = self;
        _udpDisc.begin(UDP_DISCOVERY_PORT);
        _udpGossip.begin(UDP_GOSSIP_PORT);
        _tcpServer.begin(TCP_SYNC_PORT);
        _tcpServer.setNoDelay(true);
        Serial0.printf("[Net] Listening UDP:%d,%d TCP:%d\n",
                      UDP_DISCOVERY_PORT, UDP_GOSSIP_PORT, TCP_SYNC_PORT);
        return true;
    }

    PeerInfo* findPeer(const NodeID& id) {
        for (int i = 0; i < peerCount; i++)
            if (peers[i].active && peers[i].nodeId == id) return &peers[i];
        return nullptr;
    }

    PeerInfo* addOrUpdatePeer(const NodeID& id, IPAddress ip,
                               uint32_t height, NodeRole role) {
        if (id == selfId) return nullptr;
        PeerInfo* p = findPeer(id);
        if (p) {
            p->lastSeen = millis(); p->ip = ip;
            p->chainHeight = height; p->role = role;
            return p;
        }
        if (peerCount < MAX_PEERS) {
            PeerInfo& np = peers[peerCount++];
            np = { id, ip, millis(), height, role, true };
            Serial0.printf("[Net] New peer: %s @ %s (%s) h=%d\n",
                          id.toShortStr().c_str(), ip.toString().c_str(),
                          roleName(role), height);
            return &np;
        }
        for (int i = 0; i < peerCount; i++) {
            if (!peers[i].isAlive()) {
                peers[i] = { id, ip, millis(), height, role, true };
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

    void sendDiscovery(uint32_t height, NodeRole role, uint32_t epoch) {
        uint8_t buf[32]; size_t off = 0;
        buf[off++] = (uint8_t)MsgType::DISCOVERY;
        memcpy(buf+off, selfId.id, 6); off += 6;
        memcpy(buf+off, &height, 4); off += 4;
        buf[off++] = (uint8_t)role;
        memcpy(buf+off, &epoch, 4); off += 4;
        _udpDisc.beginPacket(IPAddress(255,255,255,255), UDP_DISCOVERY_PORT);
        _udpDisc.write(buf, off);
        _udpDisc.endPacket();
    }

    void sendHeartbeat(uint32_t height, uint32_t slot) {
        uint8_t buf[32]; size_t off = 0;
        buf[off++] = (uint8_t)MsgType::HEARTBEAT;
        memcpy(buf+off, selfId.id, 6); off += 6;
        memcpy(buf+off, &height, 4); off += 4;
        memcpy(buf+off, &slot, 4); off += 4;
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
        if (tx.type == TxType::DEPLOY)
            Serial0.printf("[Net] Broadcast DEPLOY tx '%s' (%d bytes)\n", tx.name, (int)off);
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
    }

    bool requestBlock(IPAddress peerIP, uint32_t blockIndex, Block& outBlock) {
        WiFiClient client;
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
        if (off+4<=len) { memcpy(&blk.header.slot, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&blk.header.epoch, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&blk.header.reward, data+off, 4); off+=4; }
        if (off+HASH_SIZE<=len) { memcpy(blk.blockHash.bytes, data+off, HASH_SIZE); off+=HASH_SIZE; }
        for (int i = 0; i < blk.header.txCount && off < len; i++) {
            deserializeTx(data + off, len - off, blk.txns[i]);
            off += txSerializedSize(blk.txns[i]);
        }
    }

    static void serializeTx(const Transaction& tx, uint8_t* buf, size_t& off) {
        buf[off++] = (uint8_t)tx.type;
        memcpy(buf+off, tx.sender.id, 6); off += 6;
        memcpy(buf+off, &tx.nonce, 4); off += 4;
        memcpy(buf+off, &tx.timestamp, 4); off += 4;
        memcpy(buf+off, &tx.value, 4); off += 4;
        memcpy(buf+off, tx.target, 16); off += 16;
        memcpy(buf+off, tx.name, 16); off += 16;
        memcpy(buf+off, tx.voteTarget.id, 6); off += 6;
        buf[off++] = tx.argCount;
        for (int i = 0; i < tx.argCount; i++)
            memcpy(buf+off, &tx.args[i], 4), off += 4;

        if (tx.type == TxType::DEPLOY) {
            auto* entry = g_deployCache.find(tx.name);
            if (entry) {
                memcpy(buf+off, &entry->codeLen, 2); off += 2;
                memcpy(buf+off, entry->codeHash.bytes, HASH_SIZE); off += HASH_SIZE;
            } else {
                uint16_t zero = 0;
                memcpy(buf+off, &zero, 2); off += 2;
                memset(buf+off, 0, HASH_SIZE); off += HASH_SIZE;
            }
        }
    }

    static void deserializeTx(const uint8_t* data, uint16_t len, Transaction& tx) {
        size_t off = 0;
        memset(&tx, 0, sizeof(tx));
        if (len < 1) return;
        tx.type = (TxType)data[off++];
        if (off+6<=len) { memcpy(tx.sender.id, data+off, 6); off+=6; }
        if (off+4<=len) { memcpy(&tx.nonce, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&tx.timestamp, data+off, 4); off+=4; }
        if (off+4<=len) { memcpy(&tx.value, data+off, 4); off+=4; }
        if (off+16<=len) { memcpy(tx.target, data+off, 16); off+=16; }
        if (off+16<=len) { memcpy(tx.name, data+off, 16); off+=16; }
        if (off+6<=len) { memcpy(tx.voteTarget.id, data+off, 6); off+=6; }
        if (off+1<=len) { tx.argCount = data[off++]; }
        for (int i=0; i<tx.argCount && off+4<=len; i++)
            memcpy(&tx.args[i], data+off, 4), off+=4;

        if (tx.type == TxType::DEPLOY && off+2+HASH_SIZE <= len) {
            uint16_t codeLen;
            Hash256 codeHash;
            memcpy(&codeLen, data+off, 2); off += 2;
            memcpy(codeHash.bytes, data+off, HASH_SIZE); off += HASH_SIZE;
            if (codeLen > 0)
                g_deployCache.storeMeta(tx.name, codeLen, codeHash);
        }
    }

    static size_t txSerializedSize(const Transaction& tx) {
        size_t sz = 1 + 6 + 4 + 4 + 4 + 16 + 16 + 6 + 1 + tx.argCount * 4;
        if (tx.type == TxType::DEPLOY)
            sz += 2 + HASH_SIZE;
        return sz;
    }

    bool requestCode(IPAddress hostIP, const char* name,
                     uint8_t* outCode, uint16_t& outLen) {
        WiFiClient client;
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
        uint32_t start = millis();
        while (millis() - start < 3000) {
            int avail = client.available();
            if (avail > 0) {
                int chunk = client.read(buf + len, sizeof(buf) - len);
                len += chunk;
                start = millis();
            }
            if (len > 10 && !client.available()) break;
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
