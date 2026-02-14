#include <WiFi.h>
#include <esp_mac.h>
#include "config.h"
#include "types.h"
#include "mvm.h"
#include "assembler.h"
#include "staking.h"
#include "blockchain.h"
#include "consensus.h"
#include "network.h"
#include "led.h"
#include "wifi.h"


NodeID          g_selfId;
BlockchainState g_chain;
Consensus       g_consensus;
P2PNetwork      g_net;
StatusLED       g_led;
WiFiManager     g_wifi;

uint32_t g_lastDiscovery = 0;
uint32_t g_lastHeartbeat = 0;
uint32_t g_lastSlotCheck = 0;
uint32_t g_lastSync = 0;
uint32_t g_txNonce = 0;
bool     g_genesisSynced = false;

static Block g_scratchBlock;
static Block g_scratchBlock2;

bool   g_assembling = false;
String g_asmBuffer, g_asmName;

#define GENESIS_NODES 5

Transaction buildTx(TxType type) {
    Transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.type = type;
    tx.sender = g_selfId;
    tx.nonce = g_txNonce++;
    tx.timestamp = millis();
    return tx;
}


void setup() {
    Serial0.begin(115200);
    delay(1000);

    Serial0.println("\n\n\n");
    Serial0.println("========================================");
    Serial0.println("         MirkoNet v3.0 - PoS            ");
    Serial0.println("    Open Blockchain for ESP32 Mesh      ");
    Serial0.println("========================================");
    Serial0.printf("[Boot] Chip: %s | Cores: %d | Freq: %dMHz\n",
                  ESP.getChipModel(), ESP.getChipCores(), ESP.getCpuFreqMHz());
    Serial0.printf("[Boot] Flash: %dKB | Heap: %d bytes\n",
                  ESP.getFlashChipSize()/1024, ESP.getFreeHeap());


    Serial0.println("\n[Init] Step 1/6: LED...");
    g_led.init();
    g_led.setState(LED_NO_WIFI);
    Serial0.println("[Init]   LED OK");


    Serial0.println("[Init] Step 2/6: WiFi...");
    bool wifiOk = g_wifi.begin();
    Serial0.printf("[Init]   WiFi result: STA %s\n", wifiOk ? "CONNECTED" : "not connected");
    Serial0.printf("[Init]   AP always active: %s\n", g_wifi.apName.c_str());
    Serial0.printf("[Init]   Portal: http://%s\n", WiFi.softAPIP().toString().c_str());
    Serial0.printf("[Init]   Heap after WiFi: %d\n", ESP.getFreeHeap());


    Serial0.println("[Init] Step 3/6: Identity...");
    uint8_t mac[6];
    WiFi.macAddress(mac);
    memcpy(g_selfId.id, mac, 6);
    Serial0.println("[Init]   Node ID: " + g_selfId.toString());
    Serial0.println("[Init]   Short:   " + g_selfId.toShortStr());
    if (g_selfId.isZero()) {
        Serial0.println("[Init]   WARNING: MAC is all zeros! Trying esp_efuse_mac...");
        esp_efuse_mac_get_default(mac);
        memcpy(g_selfId.id, mac, 6);
        Serial0.println("[Init]   Retry ID: " + g_selfId.toString());
    }
    Serial0.printf("[Init]   Heap after identity: %d\n", ESP.getFreeHeap());


    Serial0.println("[Init] Step 4/6: Blockchain genesis...");
    g_chain.initGenesis(g_selfId);
    Serial0.printf("[Init]   Chain height: %d\n", g_chain.height());
    Serial0.printf("[Init]   Accounts: %d\n", g_chain.accountCount);
    Serial0.printf("[Init]   Heap after genesis: %d\n", ESP.getFreeHeap());


    Serial0.println("[Init] Step 5/6: Consensus...");
    g_consensus.init(g_selfId);
    g_consensus.updateRole(g_chain.staking);
    Serial0.printf("[Init]   Role: %s\n", roleName(g_consensus.selfRole));
    Serial0.printf("[Init]   Validators: %d\n", g_chain.staking.activeCount);


    Serial0.println("[Init] Step 6/6: P2P Network...");
    g_net.init(g_selfId);
    Serial0.printf("[Init]   UDP ports: %d, %d | TCP: %d\n",
                  UDP_DISCOVERY_PORT, UDP_GOSSIP_PORT, TCP_SYNC_PORT);
    Serial0.printf("[Init]   Heap after net: %d\n", ESP.getFreeHeap());


    Serial0.println("\n======== INIT COMPLETE ========");
    Serial0.printf("  Node:       %s\n", g_selfId.toString().c_str());
    Serial0.printf("  Role:       %s\n", roleName(g_consensus.selfRole));
    Serial0.printf("  Balance:    %u tokens\n", g_chain.getBalance(g_selfId));
    const StakeInfo* si = g_chain.staking.findStake(g_selfId);
    Serial0.printf("  Staked:     %u tokens\n", si ? si->stakedAmount : 0);
    Serial0.printf("  WiFi STA:   %s\n", wifiOk ? WiFi.localIP().toString().c_str() : "not connected");
    Serial0.printf("  WiFi AP:    %s @ 192.168.4.1\n", g_wifi.apName.c_str());
    Serial0.printf("  Heap:       %d bytes free\n", ESP.getFreeHeap());
    Serial0.printf("  Blocks:     sizeof=%d x max=%d = %dB\n",
                  sizeof(Block), MAX_BLOCKS, sizeof(Block)*MAX_BLOCKS);
    Serial0.printf("  Txns:       sizeof=%d\n", sizeof(Transaction));
    Serial0.printf("  Pruning:    at %d blocks, keep %d, %d checkpoints\n",
                  PRUNE_TRIGGER, PRUNE_KEEP, MAX_CHECKPOINTS);
    Serial0.printf("  Grace:      %ds before block production (peer discovery)\n",
                  BOOT_GRACE_PERIOD / 1000);
    Serial0.printf("  Portal:     http://%s\n", WiFi.softAPIP().toString().c_str());
    Serial0.println("  Type 'help' for commands");
    Serial0.println("===============================\n");
}


void loop() {
    uint32_t now = millis();
    handleSerial();


    g_wifi.handlePortal();


    bool staUp = (WiFi.status() == WL_CONNECTED);
    if (staUp != g_wifi.staConnected) {
        g_wifi.staConnected = staUp;
        if (staUp) {
            Serial0.println("[WiFi] STA connected! IP: " + WiFi.localIP().toString());
        } else {
            Serial0.println("[WiFi] STA disconnected!");
        }
    }


    if (staUp) {
        if (now - g_lastDiscovery >= DISCOVERY_INTERVAL) {
            g_lastDiscovery = now;
            g_net.sendDiscovery(g_chain.height(), g_consensus.selfRole,
                                g_chain.currentEpoch());
        }

        if (g_consensus.selfRole == ROLE_VALIDATOR &&
            now - g_lastHeartbeat >= HEARTBEAT_INTERVAL) {
            g_lastHeartbeat = now;
            g_net.sendHeartbeat(g_chain.height(), g_consensus.getCurrentSlot());
        }

        auto disc = g_net.recvDiscovery();
        if (disc.valid && disc.type == MsgType::DISCOVERY)
            handleDiscovery(disc);

        auto gossip = g_net.recvGossip();
        if (gossip.valid)
            handleGossip(gossip);

        WiFiClient tcpClient;
        if (g_net.handleTCPSync(tcpClient))
            handleTCPRequest(tcpClient);

        if (now - g_lastSlotCheck >= 500) {
            g_lastSlotCheck = now;
            checkBlockProduction();
        }

        if (now - g_lastSync >= SYNC_COOLDOWN) {
            g_lastSync = now;
            trySync();
        }
    }


    if (!staUp) {
        static uint32_t lastReconnect = 0;
        if (now - lastReconnect > 30000) {
            lastReconnect = now;
            Preferences prefs;
            prefs.begin(WIFI_NVS_NAMESPACE, true);
            String ssid = prefs.getString("ssid", "");
            prefs.end();
            if (ssid.length() > 0) {
                Serial0.println("[WiFi] Attempting STA reconnect...");
                WiFi.reconnect();
            }
        }
    }


    static uint32_t lastAliveLog = 0;
    if (now - lastAliveLog >= 30000) {
        lastAliveLog = now;
        uint32_t sinceGenesis = now - g_consensus.genesisTime;
        bool inGrace = sinceGenesis < BOOT_GRACE_PERIOD;
        Serial0.printf("[Alive] t=%ds heap=%d wifi=%s peers=%d height=%d mem=%d ckpts=%d role=%s%s\n",
                      now / 1000,
                      ESP.getFreeHeap(),
                      staUp ? "OK" : "NO",
                      g_net.countAlive(),
                      g_chain.height(),
                      g_chain.blocksInMemory(),
                      g_chain.checkpointCount,
                      roleName(g_consensus.selfRole),
                      inGrace ? " [GRACE]" : "");
    }

    delay(10);


    if (!staUp) {
        g_led.setState(LED_NO_WIFI);
    } else if (g_net.countAlive() == 0) {
        g_led.setState(LED_NO_PEERS);
    } else if (g_consensus.selfRole == ROLE_VALIDATOR) {
        g_led.setState(LED_VALIDATOR);
    } else {
        g_led.setState(LED_CONNECTED);
    }
    g_led.update();
}


void handleDiscovery(const P2PNetwork::RecvMsg& msg) {
    uint32_t peerHeight = 0;
    NodeRole role = ROLE_LIGHT;
    uint32_t epoch = 0;

    if (msg.payloadLen >= 5) {
        memcpy(&peerHeight, msg.payload, 4);
        role = (NodeRole)msg.payload[4];
    }
    if (msg.payloadLen >= 9) {
        memcpy(&epoch, msg.payload + 5, 4);
    }

    PeerInfo* p = g_net.addOrUpdatePeer(msg.sender, msg.senderIP, peerHeight, role);


    if (g_chain.height() < 5 && g_chain.getBalance(msg.sender) == 0) {
        int genesisCount = 0;
        for (int i = 0; i < g_chain.accountCount; i++) {
            if (g_chain.accounts[i].used) {
                const StakeInfo* s = g_chain.staking.findStake(g_chain.accounts[i].owner);
                if (s && s->stakedAmount >= GENESIS_PER_NODE/2) genesisCount++;
            }
        }
        if (genesisCount < GENESIS_NODES) {
            g_chain.addGenesisNode(msg.sender);
        }
    }







    if (!g_genesisSynced && g_chain.height() <= 1 && peerHeight >= 1 &&
        (millis() - g_consensus.genesisTime) < BOOT_GRACE_PERIOD) {

        if (g_net.requestBlock(msg.senderIP, 0, g_scratchBlock2)) {
            g_genesisSynced = true;
            if (g_scratchBlock2.blockHash != g_chain.chain[0].blockHash) {

                int cmp = memcmp(g_chain.chain[0].blockHash.bytes,
                                g_scratchBlock2.blockHash.bytes, HASH_SIZE);
                if (cmp > 0) {

                    Serial0.println("[Discovery] Peer has lower genesis hash — adopting");
                    g_chain.replaceGenesis(g_scratchBlock2);
                    g_chain.setBalance(g_scratchBlock2.header.validator, GENESIS_PER_NODE);
                    g_chain.staking.totalSupply = GENESIS_PER_NODE;
                    uint32_t stakeAmt = GENESIS_PER_NODE / 2;
                    g_chain.setBalance(g_scratchBlock2.header.validator,
                                       GENESIS_PER_NODE - stakeAmt);
                    g_chain.staking.addGenesisValidator(g_scratchBlock2.header.validator, stakeAmt);
                    g_chain.addGenesisNode(g_selfId);
                    g_consensus.updateRole(g_chain.staking);
                    Serial0.printf("[Discovery] Adopted genesis from %s, hash=%s\n",
                                  msg.sender.toShortStr().c_str(),
                                  g_scratchBlock2.blockHash.toShort().c_str());
                } else {
                    Serial0.println("[Discovery] Our genesis hash is lower — we win, peer should adopt");
                }
            } else {
                Serial0.println("[Discovery] Same genesis! Already in sync.");
            }
        }
    }
}


void handleGossip(const P2PNetwork::RecvMsg& msg) {
    switch (msg.type) {

    case MsgType::HEARTBEAT: {
        if (msg.payloadLen >= 4) {
            uint32_t h;
            memcpy(&h, msg.payload, 4);
            PeerInfo* p = g_net.findPeer(msg.sender);
            if (p) p->chainHeight = h;
        }
        break;
    }

    case MsgType::TX_GOSSIP: {
        Transaction tx;
        P2PNetwork::deserializeTx(msg.payload, msg.payloadLen, tx);
        if (g_chain.addToMempool(tx)) {
            Serial0.println("[Gossip] TX from " + msg.sender.toShortStr() +
                           " type=" + String((int)tx.type));
            g_led.flash(150);
        }
        break;
    }

    case MsgType::BLOCK_FULL: {

        P2PNetwork::deserializeBlock(msg.payload, msg.payloadLen, g_scratchBlock);

        Serial0.printf("[Gossip] Full block #%d from %s (%d txns)\n",
                      g_scratchBlock.header.index,
                      msg.sender.toShortStr().c_str(),
                      g_scratchBlock.header.txCount);


        for (int i = 0; i < g_scratchBlock.header.txCount; i++) {
            for (int j = 0; j < g_chain.mempoolSize; j++) {
                if (g_chain.mempool[j].sender == g_scratchBlock.txns[i].sender &&
                    g_chain.mempool[j].nonce == g_scratchBlock.txns[i].nonce) {
                    memmove(&g_chain.mempool[j], &g_chain.mempool[j+1],
                            (g_chain.mempoolSize - j - 1) * sizeof(Transaction));
                    g_chain.mempoolSize--;
                    break;
                }
            }
        }


        prefetchCodeForTxns(g_scratchBlock.txns, g_scratchBlock.header.txCount);

        uint8_t result = g_chain.applyNetworkBlock(g_scratchBlock);
        if (result == 0) {
            Serial0.printf("[Gossip] Block #%d APPLIED! height=%d\n",
                          g_scratchBlock.header.index, g_chain.height());
            g_consensus.updateRole(g_chain.staking);
            g_consensus.lastProducedSlot = g_scratchBlock.header.slot;
            g_led.flash(300);
        } else {
            const char* reasons[] = {
                "OK", "wrong index", "prevHash mismatch", "bad hash",
                "chain full", "not a validator", "wrong slot producer"
            };
            Serial0.printf("[Gossip] Block #%d REJECTED: %s (code=%d)\n",
                          g_scratchBlock.header.index,
                          result < 7 ? reasons[result] : "unknown",
                          result);
        }
        break;
    }

    case MsgType::DEPLOY_DATA: {


        break;
    }

    case MsgType::BLOCK_ANN: {

        if (msg.payloadLen >= 4) {
            uint32_t blockIdx;
            memcpy(&blockIdx, msg.payload, 4);
            if (blockIdx >= g_chain.height()) {
                Serial0.printf("[Gossip] Block #%d announced (we're at %d, will sync)\n",
                              blockIdx, g_chain.height());
            }
        }
        break;
    }

    default: break;
    }
}


void handleTCPRequest(WiFiClient& client) {
    uint32_t start = millis();
    while (!client.available() && millis()-start < 2000) delay(5);
    if (!client.available()) { client.stop(); return; }

    uint8_t buf[64];
    int len = client.read(buf, sizeof(buf));
    if (len < 7) { client.stop(); return; }

    MsgType type = (MsgType)buf[0];

    if (type == MsgType::BLOCK_REQ && len >= 11) {
        uint32_t requestedIdx;
        memcpy(&requestedIdx, buf+7, 4);

        Serial0.printf("[TCP] Block #%d requested\n", requestedIdx);

        if (requestedIdx >= g_chain.chainOffset &&
            requestedIdx < g_chain.chainOffset + g_chain.chainLen) {
            uint32_t arrayIdx = requestedIdx - g_chain.chainOffset;
            g_net.sendBlock(client, g_chain.chain[arrayIdx]);
            Serial0.printf("[TCP] Sent block #%d\n", requestedIdx);
        } else {
            Serial0.printf("[TCP] Block #%d not in memory [%d..%d]\n",
                          requestedIdx, g_chain.chainOffset,
                          g_chain.chainOffset + g_chain.chainLen - 1);
        }
    }
    else if (type == MsgType::CODE_REQ && len >= 23) {

        char name[16];
        memcpy(name, buf+7, 16); name[15] = '\0';

        Serial0.printf("[TCP] Code request for '%s'\n", name);

        Contract* c = g_chain.findContract(name);
        if (c && c->hasCode && c->codeLen > 0) {

            uint8_t resp[512]; size_t off = 0;
            resp[off++] = (uint8_t)MsgType::CODE_RESP;
            memcpy(resp+off, g_selfId.id, 6); off += 6;
            memcpy(resp+off, &c->codeLen, 2); off += 2;
            memcpy(resp+off, c->code, c->codeLen); off += c->codeLen;
            client.write(resp, off);
            client.flush();
            Serial0.printf("[TCP] Sent '%s' bytecode (%d bytes)\n", name, c->codeLen);
        } else {
            Serial0.printf("[TCP] Don't have bytecode for '%s'\n", name);
        }
    }
    client.stop();
}


void fetchCodeForContract(Contract* c) {
    if (!c || c->hasCode) return;


    IPAddress hostIP;
    bool found = false;
    for (int i = 0; i < g_net.peerCount; i++) {
        if (g_net.peers[i].nodeId == c->host && g_net.peers[i].isAlive()) {
            hostIP = g_net.peers[i].ip;
            found = true;
            break;
        }
    }

    if (!found) {
        Serial0.printf("[Code] Host %s for '%s' not reachable\n",
                      c->host.toShortStr().c_str(), c->name);
        return;
    }

    uint8_t codeBuf[MVM_MAX_CODE];
    uint16_t codeLen;
    if (g_net.requestCode(hostIP, c->name, codeBuf, codeLen)) {
        if (c->cacheCode(codeBuf, codeLen)) {
            Serial0.printf("[Code] Cached '%s' bytecode (%dB, hash verified)\n",
                          c->name, codeLen);
        } else {
            Serial0.printf("[Code] HASH MISMATCH for '%s'! Rejecting bytecode\n",
                          c->name);
        }
    }
}

void prefetchCodeForTxns(const Transaction* txns, uint8_t count) {
    for (int i = 0; i < count; i++) {
        if (txns[i].type == TxType::CALL) {
            Contract* c = g_chain.findContract(txns[i].target);
            if (c && !c->hasCode) fetchCodeForContract(c);
        }
    }
}


#define EMPTY_BLOCK_INTERVAL  10

void checkBlockProduction() {
    g_consensus.updateRole(g_chain.staking);


    if (millis() - g_consensus.genesisTime < BOOT_GRACE_PERIOD) return;

    if (!g_consensus.shouldProduce(g_chain.staking)) return;

    uint32_t slot = g_consensus.getCurrentSlot();

    bool hasTx = (g_chain.mempoolSize > 0);
    bool heartbeatSlot = (slot % EMPTY_BLOCK_INTERVAL == 0);

    if (!hasTx && !heartbeatSlot) {
        g_consensus.lastProducedSlot = slot;
        return;
    }

    g_consensus.lastProducedSlot = slot;

    Serial0.printf("\n=== PRODUCING BLOCK (slot %d, epoch %d, txns=%d%s) ===\n",
                  slot, g_chain.currentEpoch(), g_chain.mempoolSize,
                  hasTx ? "" : " heartbeat");

    if (g_chain.staking.activeCount > 1) {
        Serial0.printf("  PoS: slot %d -> validator %d/%d (%s = US)\n",
                      slot,
                      g_chain.staking.getValidatorIndex(g_selfId) + 1,
                      g_chain.staking.activeCount,
                      g_selfId.toShortStr().c_str());
    }

    Transaction txns[MAX_TXN_PER_BLOCK];
    uint8_t txCount = g_chain.drainMempool(txns, MAX_TXN_PER_BLOCK);


    prefetchCodeForTxns(txns, txCount);

    if (g_chain.createBlock(g_selfId, slot, txns, txCount)) {
        g_net.broadcastFullBlock(g_chain.lastBlock());
        g_consensus.updateRole(g_chain.staking);
        g_led.setState(LED_PRODUCING);
        g_led.flash(500);
    }
}


void trySync() {
    PeerInfo* best = nullptr;
    for (int i = 0; i < g_net.peerCount; i++) {
        PeerInfo& p = g_net.peers[i];
        if (!p.isAlive()) continue;
        if (!best || p.chainHeight > best->chainHeight)
            best = &p;
    }

    if (!best) return;

    uint32_t ourHeight = g_chain.height();
    uint32_t peerHeight = best->chainHeight;
    if (peerHeight <= ourHeight) return;

    Serial0.printf("[Sync] Peer %s has height %d (we: %d)\n",
                  best->nodeId.toShortStr().c_str(), peerHeight, ourHeight);


    if (!g_net.requestBlock(best->ip, ourHeight, g_scratchBlock)) {
        Serial0.println("[Sync] Failed to download block, will retry");
        return;
    }

    prefetchCodeForTxns(g_scratchBlock.txns, g_scratchBlock.header.txCount);
    uint8_t result = g_chain.applyNetworkBlock(g_scratchBlock);

    if (result == 0) {
        Serial0.printf("[Sync] Block #%d applied. Height: %d\n",
                      ourHeight, g_chain.height());
        g_consensus.updateRole(g_chain.staking);
        return;
    }

    if (result != 2) {
        Serial0.printf("[Sync] Block #%d rejected (code=%d)\n", ourHeight, result);
        return;
    }


    Serial0.println("[Sync] Chain divergence! Downloading peer's genesis...");

    if (!g_net.requestBlock(best->ip, 0, g_scratchBlock2)) {
        Serial0.println("[Sync] Can't download peer's genesis, will retry");
        return;
    }

    Hash256 ourGenesisHash = g_chain.chain[0].blockHash;
    Hash256 peerGenesisHash = g_scratchBlock2.blockHash;

    Serial0.printf("[Sync] Our genesis:  %s\n", ourGenesisHash.toShort().c_str());
    Serial0.printf("[Sync] Peer genesis: %s\n", peerGenesisHash.toShort().c_str());

    int cmp = memcmp(ourGenesisHash.bytes, peerGenesisHash.bytes, HASH_SIZE);

    if (cmp < 0) {
        Serial0.println("[Sync] Our genesis wins (lower hash). Peer should sync to us.");
        return;
    }
    if (cmp == 0) {
        Serial0.println("[Sync] Same genesis but prevHash mismatch?! Unexpected.");
        return;
    }


    Serial0.println("[Sync] Peer's genesis wins. Adopting peer's chain...");

    if (!g_chain.replaceGenesis(g_scratchBlock2)) {
        Serial0.println("[Sync] Failed to replace genesis!");
        return;
    }

    g_chain.setBalance(g_scratchBlock2.header.validator, GENESIS_PER_NODE);
    g_chain.staking.totalSupply = GENESIS_PER_NODE;
    uint32_t stakeAmt = GENESIS_PER_NODE / 2;
    g_chain.setBalance(g_scratchBlock2.header.validator, GENESIS_PER_NODE - stakeAmt);
    g_chain.staking.addGenesisValidator(g_scratchBlock2.header.validator, stakeAmt);
    g_chain.addGenesisNode(g_selfId);
    g_consensus.updateRole(g_chain.staking);

    Serial0.printf("[Sync] Genesis adopted. Height: %d, downloading blocks...\n",
                  g_chain.height());


    for (uint32_t i = 0; i < 10; i++) {
        uint32_t idx = g_chain.height();
        if (idx >= peerHeight) break;

        if (!g_net.requestBlock(best->ip, idx, g_scratchBlock)) {
            Serial0.printf("[Sync] Download failed at #%d\n", idx);
            break;
        }

        prefetchCodeForTxns(g_scratchBlock.txns, g_scratchBlock.header.txCount);
        uint8_t r = g_chain.applyNetworkBlock(g_scratchBlock);
        if (r == 0) {
            Serial0.printf("[Sync] Applied #%d, height=%d\n", idx, g_chain.height());
            g_consensus.updateRole(g_chain.staking);
        } else {
            Serial0.printf("[Sync] Block #%d rejected (code=%d)\n", idx, r);
            break;
        }
    }

    Serial0.printf("[Sync] Resync done. Height: %d (peer: %d)\n",
                  g_chain.height(), peerHeight);
}


bool findNodeByShort(const String& shortId, NodeID& out) {
    for (int i = 0; i < g_net.peerCount; i++) {
        if (g_net.peers[i].active &&
            g_net.peers[i].nodeId.toShortStr().equalsIgnoreCase(shortId)) {
            out = g_net.peers[i].nodeId;
            return true;
        }
    }
    for (int i = 0; i < g_chain.accountCount; i++) {
        if (g_chain.accounts[i].used &&
            g_chain.accounts[i].owner.toShortStr().equalsIgnoreCase(shortId)) {
            out = g_chain.accounts[i].owner;
            return true;
        }
    }
    for (int i = 0; i < g_chain.staking.stakeCount; i++) {
        if (g_chain.staking.stakes[i].used &&
            g_chain.staking.stakes[i].staker.toShortStr().equalsIgnoreCase(shortId)) {
            out = g_chain.staking.stakes[i].staker;
            return true;
        }
    }
    return false;
}

void directDeploy(const char* name, const uint8_t* code, uint16_t len) {
    g_deployCache.store(name, code, len);

    Transaction tx = buildTx(TxType::DEPLOY);
    strncpy(tx.name, name, 15);

    g_chain.addToMempool(tx);
    g_net.broadcastTx(tx);
    Serial0.printf("  [TX] Deploy '%s' submitted (%d bytes)\n", name, len);
}

void handleSerial() {
    if (!Serial0.available()) return;
    String line = Serial0.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    if (g_assembling) {
        if (line == ".") {
            g_assembling = false;
            auto r = MVMAssembler::assemble(g_asmBuffer);
            if (!r.ok) { Serial0.println("[ASM] Error: " + r.error); return; }

            g_deployCache.store(g_asmName.c_str(), r.code, r.len);

            Transaction tx = buildTx(TxType::DEPLOY);
            strncpy(tx.name, g_asmName.c_str(), 15);

            g_chain.addToMempool(tx);
            g_net.broadcastTx(tx);
            Serial0.printf("[TX] Deploy '%s' submitted (%d bytes bytecode, next block)\n",
                          g_asmName.c_str(), r.len);
        } else {
            g_asmBuffer += line + "\n";
        }
        return;
    }

    String cmd = line, arg1 = "", arg2 = "";
    int sp = line.indexOf(' ');
    if (sp > 0) {
        cmd = line.substring(0, sp);
        arg1 = line.substring(sp+1); arg1.trim();
        int sp2 = arg1.indexOf(' ');
        if (sp2 > 0) {
            arg2 = arg1.substring(sp2+1); arg2.trim();
            arg1 = arg1.substring(0, sp2);
        }
    }
    cmd.toLowerCase();

    if (cmd == "help") {
        Serial0.println("\n====== MirkoNet PoS Commands ======");
        Serial0.println("-- Info --");
        Serial0.println("  status        Node overview");
        Serial0.println("  peers         Connected peers");
        Serial0.println("  chain         Blockchain");
        Serial0.println("  accounts      All accounts + balances");
        Serial0.println("  economy       Economic stats");
        Serial0.println("  heap          Free memory");
        Serial0.println("-- PoS --");
        Serial0.println("  validators    Active validator set");
        Serial0.println("  candidates    All staking candidates");
        Serial0.println("  faucet        Request " + String(FAUCET_AMOUNT) + " tokens");
        Serial0.println("  stake <amt>   Stake tokens (min " + String(MIN_STAKE) + ")");
        Serial0.println("  unstake <amt> Begin unstake cooldown");
        Serial0.println("  claim         Claim unstaked tokens");
        Serial0.println("-- Tokens --");
        Serial0.println("  balance       Your balance");
        Serial0.println("  transfer <id> <amt>  Send tokens");
        Serial0.println("-- Contracts --");
        Serial0.println("  deploy <n>    Deploy (asm mode, end '.')");
        Serial0.println("  call <n> [a]  Execute contract");
        Serial0.println("  contracts     List contracts");
        Serial0.println("  storage <n>   Inspect storage");
        Serial0.println("  example       Deploy demos");
        Serial0.println("-- System --");
        Serial0.println("  debug         Full system diagnostics");
        Serial0.println("  checkpoints   Cryptographic chain history");
        Serial0.println("  heap          Free memory");
        Serial0.println("  wifistatus    WiFi connection info");
        Serial0.println("  wifireset     Clear WiFi, reboot to AP setup");
        Serial0.println("====================================\n");
    }
    else if (cmd == "status") {
        uint32_t staked = 0;
        const StakeInfo* si = g_chain.staking.findStake(g_selfId);
        if (si) staked = si->stakedAmount;

        Serial0.println("\n====== MirkoNet Status ======");
        Serial0.println("  Node:       " + g_selfId.toString());
        Serial0.println("  Short ID:   " + g_selfId.toShortStr());
        Serial0.println("  IP:         " + WiFi.localIP().toString());
        Serial0.printf("  Role:       %s\n", roleName(g_consensus.selfRole));
        Serial0.println("  Chain:      " + String(g_chain.height()) + " blocks (logical)");
        Serial0.println("  In memory:  " + String(g_chain.blocksInMemory()) + " blocks");
        Serial0.println("  Checkpoints:" + String(g_chain.checkpointCount));
        Serial0.println("  Epoch:      " + String(g_chain.currentEpoch()));
        Serial0.println("  Balance:    " + String(g_chain.getBalance(g_selfId)));
        Serial0.println("  Staked:     " + String(staked));
        Serial0.println("  Peers:      " + String(g_net.countAlive()));
        Serial0.println("  Validators: " + String(g_chain.staking.activeCount));
        Serial0.println("  Slot:       " + String(g_consensus.getCurrentSlot()));
        Serial0.println("  Mempool:    " + String(g_chain.mempoolSize));
        Serial0.printf("  Heap:       %d bytes\n", ESP.getFreeHeap());
        Serial0.println("=============================\n");
    }
    else if (cmd == "peers") {
        g_net.printPeers();
    }
    else if (cmd == "chain") {
        g_chain.printChain();
    }
    else if (cmd == "accounts") {
        g_chain.printAccounts();
    }
    else if (cmd == "validators" || cmd == "candidates") {
        g_chain.staking.printStatus();
    }
    else if (cmd == "economy") {
        Serial0.println("\n====== Economy ======");
        Serial0.printf("  Total supply:  %u tokens\n", g_chain.staking.totalSupply);
        Serial0.printf("  Total staked:  %u tokens (%.1f%%)\n",
                      g_chain.staking.totalStaked,
                      g_chain.staking.totalSupply > 0 ?
                      100.0*g_chain.staking.totalStaked/g_chain.staking.totalSupply : 0);
        Serial0.printf("  Block reward:  %u tokens/block (new supply)\n", BLOCK_REWARD);
        Serial0.printf("  Gas price:     %u token/gas (CALL/DEPLOY)\n", GAS_PRICE);
        Serial0.printf("  Base tx fee:   %u token (TRANSFER/STAKE)\n", TX_BASE_FEE);
        Serial0.printf("  Faucet fee:    %u (free)\n", FAUCET_FEE);
        Serial0.printf("  Min stake:     %u tokens\n", MIN_STAKE);
        Serial0.printf("  Faucet:        %u tokens (cooldown %ds)\n",
                      FAUCET_AMOUNT, FAUCET_COOLDOWN/1000);
        Serial0.printf("  Unstake delay: %d blocks\n", UNSTAKE_COOLDOWN);
        Serial0.printf("  Epoch length:  %d blocks\n", EPOCH_LENGTH);
        Serial0.printf("  Accounts:      %d\n", g_chain.accountCount);
        Serial0.println("  ---- Validator Income ----");
        Serial0.println("  Per block: BLOCK_REWARD + sum(tx fees)");
        Serial0.println("  CALL fee = gasUsed * GAS_PRICE");
        Serial0.println("  Other fee = TX_BASE_FEE flat");
        Serial0.println("=====================\n");
    }
    else if (cmd == "heap") {
        Serial0.printf("Heap: %d bytes (min: %d)\n",
                      ESP.getFreeHeap(), ESP.getMinFreeHeap());
    }

    else if (cmd == "wifireset") {
        WiFiManager::resetCredentials();
    }
    else if (cmd == "wifistatus") {
        g_wifi.printStatus();
    }
    else if (cmd == "checkpoints") {
        g_chain.printCheckpoints();
    }

    else if (cmd == "debug") {
        Serial0.println("\n====== Debug Info ======");
        Serial0.printf("  Heap free:     %d bytes\n", ESP.getFreeHeap());
        Serial0.printf("  Heap min:      %d bytes\n", ESP.getMinFreeHeap());
        Serial0.printf("  Heap total:    %d bytes\n", ESP.getHeapSize());
        Serial0.printf("  PSRAM free:    %d bytes\n", ESP.getFreePsram());
        Serial0.printf("  CPU freq:      %d MHz\n", ESP.getCpuFreqMHz());
        Serial0.printf("  Chip model:    %s\n", ESP.getChipModel());
        Serial0.printf("  Chip cores:    %d\n", ESP.getChipCores());
        Serial0.printf("  Flash size:    %d KB\n", ESP.getFlashChipSize() / 1024);
        Serial0.printf("  Uptime:        %d seconds\n", millis() / 1000);
        Serial0.printf("  WiFi status:   %s\n",
            WiFi.status() == WL_CONNECTED ? "Connected" :
            WiFi.status() == WL_DISCONNECTED ? "Disconnected" :
            WiFi.status() == WL_NO_SSID_AVAIL ? "SSID not found" :
            WiFi.status() == WL_CONNECT_FAILED ? "Auth failed" : "Other");
        if (WiFi.status() == WL_CONNECTED) {
            Serial0.printf("  WiFi SSID:     %s\n", WiFi.SSID().c_str());
            Serial0.printf("  WiFi IP:       %s\n", WiFi.localIP().toString().c_str());
            Serial0.printf("  WiFi RSSI:     %d dBm\n", WiFi.RSSI());
            Serial0.printf("  WiFi Channel:  %d\n", WiFi.channel());
            Serial0.printf("  WiFi MAC:      %s\n", WiFi.macAddress().c_str());
        }
        Serial0.println("  ---- Blockchain ----");
        Serial0.printf("  Logical height: %d\n", g_chain.height());
        Serial0.printf("  In memory:     %d / %d blocks [%d..%d]\n",
                      g_chain.blocksInMemory(), MAX_BLOCKS,
                      g_chain.chainOffset,
                      g_chain.chainOffset + g_chain.blocksInMemory() - 1);
        Serial0.printf("  Checkpoints:   %d / %d\n", g_chain.checkpointCount, MAX_CHECKPOINTS);
        Serial0.printf("  Prune trigger: %d blocks\n", PRUNE_TRIGGER);
        Serial0.printf("  Prune keep:    %d blocks\n", PRUNE_KEEP);
        Serial0.printf("  Contracts:     %d / %d\n", g_chain.contractCount, MAX_CONTRACTS);
        Serial0.printf("  Accounts:      %d / %d\n", g_chain.accountCount, MAX_ACCOUNTS);
        Serial0.printf("  Mempool:       %d / %d\n", g_chain.mempoolSize, MAX_PENDING_TX);
        Serial0.printf("  Peers:         %d / %d (alive: %d)\n",
                      g_net.peerCount, MAX_PEERS, g_net.countAlive());
        Serial0.println("  ---- Staking ----");
        Serial0.printf("  Candidates:    %d / %d\n", g_chain.staking.stakeCount, MAX_CANDIDATES);
        Serial0.printf("  Validators:    %d / %d\n", g_chain.staking.activeCount, MAX_VALIDATORS);
        Serial0.printf("  Total staked:  %u\n", g_chain.staking.totalStaked);
        Serial0.printf("  Total supply:  %u\n", g_chain.staking.totalSupply);
        Serial0.printf("  Epoch:         %d (next election: block #%d)\n",
                      g_chain.currentEpoch(),
                      (g_chain.currentEpoch() + 1) * EPOCH_LENGTH);
        Serial0.println("  ---- Consensus ----");
        Serial0.printf("  Role:          %s\n", roleName(g_consensus.selfRole));
        Serial0.printf("  Current slot:  %d\n", g_consensus.getCurrentSlot());
        Serial0.printf("  Last produced: %d\n", g_consensus.lastProducedSlot);
        Serial0.printf("  Genesis:       %d sec ago\n",
                      (millis() - g_consensus.genesisTime) / 1000);
        Serial0.println("  ---- Memory Layout ----");
        Serial0.printf("  sizeof(Block):       %d bytes\n", sizeof(Block));
        Serial0.printf("  sizeof(Transaction): %d bytes\n", sizeof(Transaction));
        Serial0.printf("  sizeof(Contract):    %d bytes\n", sizeof(Contract));
        Serial0.printf("  Chain array:         %d bytes\n", sizeof(Block) * MAX_BLOCKS);
        Serial0.printf("  Contract array:      %d bytes\n", sizeof(Contract) * MAX_CONTRACTS);
        Serial0.printf("  Mempool array:       %d bytes\n", sizeof(Transaction) * MAX_PENDING_TX);
        Serial0.println("========================\n");
    }

    else if (cmd == "faucet") {
        Transaction tx = buildTx(TxType::FAUCET);
        g_chain.addToMempool(tx);
        g_net.broadcastTx(tx);
        Serial0.println("[TX] Faucet request submitted (next block)");
    }
    else if (cmd == "balance") {
        uint32_t bal = g_chain.getBalance(g_selfId);
        const StakeInfo* si = g_chain.staking.findStake(g_selfId);
        Serial0.printf("Available: %u tokens\n", bal);
        if (si) {
            Serial0.printf("Staked:    %u tokens\n", si->stakedAmount);
            if (si->unstakingAmount > 0)
                Serial0.printf("Unstaking: %u tokens\n", si->unstakingAmount);
            Serial0.printf("Total:     %u tokens\n",
                          bal + si->stakedAmount + si->unstakingAmount);
        }
    }
    else if (cmd == "stake") {
        if (arg1.length() == 0) {
            Serial0.println("Usage: stake <amount>");
            return;
        }
        Transaction tx = buildTx(TxType::STAKE);
        tx.value = arg1.toInt();
        g_chain.addToMempool(tx);
        g_net.broadcastTx(tx);
        Serial0.printf("[TX] Stake %u submitted (next block)\n", tx.value);
    }
    else if (cmd == "unstake") {
        if (arg1.length() == 0) { Serial0.println("Usage: unstake <amount>"); return; }
        Transaction tx = buildTx(TxType::UNSTAKE);
        tx.value = arg1.toInt();
        g_chain.addToMempool(tx);
        g_net.broadcastTx(tx);
        Serial0.println("[TX] Unstake submitted (next block)");
    }
    else if (cmd == "claim") {
        Transaction tx = buildTx(TxType::CLAIM);
        g_chain.addToMempool(tx);
        g_net.broadcastTx(tx);
        Serial0.println("[TX] Claim submitted (next block)");
    }
    else if (cmd == "transfer") {
        if (arg1.length() == 0 || arg2.length() == 0) {
            Serial0.println("Usage: transfer <6-char-hex-id> <amount>");
            return;
        }
        NodeID target;
        if (!findNodeByShort(arg1, target)) {
            Serial0.println("Node not found: " + arg1);
            return;
        }
        Transaction tx = buildTx(TxType::TRANSFER);
        tx.voteTarget = target;
        tx.value = arg2.toInt();
        g_chain.addToMempool(tx);
        g_net.broadcastTx(tx);
        Serial0.printf("[TX] Transfer %u submitted (next block)\n", tx.value);
    }

    else if (cmd == "deploy") {
        if (arg1.length() == 0) { Serial0.println("Usage: deploy <name>"); return; }
        g_assembling = true; g_asmBuffer = ""; g_asmName = arg1;
        Serial0.println("Enter mVM assembly (end with '.'):");
    }
    else if (cmd == "call") {
        if (arg1.length() == 0) { Serial0.println("Usage: call <name> [args]"); return; }
        Transaction tx = buildTx(TxType::CALL);
        strncpy(tx.target, arg1.c_str(), 15);
        if (arg2.length() > 0) {
            String args = arg2;
            while (args.length() > 0 && tx.argCount < MVM_MAX_ARGS) {
                int s = args.indexOf(' ');
                String a = (s>0) ? args.substring(0,s) : args;
                tx.args[tx.argCount++] = a.toInt();
                args = (s>0) ? args.substring(s+1) : "";
                args.trim();
            }
        }
        if (g_chain.findContract(arg1.c_str()) == nullptr) {
            Serial0.println("Contract not found: " + arg1);
            return;
        }
        g_chain.addToMempool(tx);
        g_net.broadcastTx(tx);
        Serial0.println("[TX] Call '" + arg1 + "' submitted (next block)");
    }
    else if (cmd == "contracts") {
        Serial0.println("\n====== Contracts ======");
        for (int i = 0; i < g_chain.contractCount; i++) {
            Contract& c = g_chain.contracts[i];
            if (!c.active) continue;
            bool isUs = (c.host == g_selfId);
            Serial0.printf("  %s @ %s | %dB | by %s | %s %s\n",
                          c.name, c.addr.toHex().c_str(), c.codeLen,
                          c.deployer.toShortStr().c_str(),
                          c.hasCode ? "LOCAL" : "REMOTE",
                          isUs ? "(we host)" : c.host.toShortStr().c_str());
        }
        Serial0.println("=======================\n");
    }
    else if (cmd == "storage") {
        Contract* c = g_chain.findContract(arg1.c_str());
        if (c) {
            Serial0.println("\nStorage: " + String(c->name));
            for (int i = 0; i < MVM_MAX_STORAGE; i++)
                if (c->storage[i].used)
                    Serial0.printf("  [%u] = %u\n", c->storage[i].key, c->storage[i].value);
        } else Serial0.println("Not found: " + arg1);
    }
    else if (cmd == "example") {
        deployExamples();
    }
    else {
        Serial0.println("Unknown command. Type 'help'.");
    }
}

void deployExamples() {
    Serial0.println("\n[Example] Deploying demo contracts (mVM v2, 4-bit ISA)...\n");

    {
        String src = R"(
ARG 0
DUP
PUSH 0
EQ
JUMPI @read
DUP
PUSH 1
EQ
JUMPI @inc
REVERT
read:
  POP
  PUSH 0
  SLOAD
  EMIT
  HALT
inc:
  POP
  PUSH 0
  SLOAD
  PUSH 1
  ADD
  DUP
  PUSH 0
  SWAP
  SSTORE
  EMIT
  HALT
)";
        auto r = MVMAssembler::assemble(src);
        if (r.ok) {
            Serial0.printf("  counter: %d bytes (v1 was 63)\n", r.len);
            directDeploy("counter", r.code, r.len);
        } else Serial0.println("counter err: " + r.error);
    }

    {
        String src = R"(
ARG 0
DUP
PUSH 0
EQ
JUMPI @balance
DUP
PUSH 1
EQ
JUMPI @mint
DUP
PUSH 2
EQ
JUMPI @xfer
REVERT
balance:
  POP
  CALLER
  SLOAD
  EMIT
  HALT
mint:
  POP
  CALLER
  DUP
  SLOAD
  ARG 1
  ADD
  SSTORE
  PUSH 1
  EMIT
  HALT
xfer:
  POP
  CALLER
  DUP
  SLOAD
  DUP
  ARG 2
  GT
  NOT
  JUMPI @ok
  REVERT
ok:
  ARG 2
  SUB
  CALLER
  SWAP
  SSTORE
  ARG 1
  DUP
  SLOAD
  ARG 2
  ADD
  SSTORE
  PUSH 2
  EMIT
  HALT
)";
        auto r = MVMAssembler::assemble(src);
        if (r.ok) {
            Serial0.printf("  token:   %d bytes\n", r.len);
            directDeploy("token", r.code, r.len);
        } else Serial0.println("token err: " + r.error);
    }

    {
        String src = R"(
ARG 0
DUP
PUSH 0
EQ
JUMPI @count
DUP
PUSH 1
EQ
JUMPI @vote
DUP
PUSH 2
EQ
JUMPI @tally
REVERT
count:
  POP
  PUSH 0
  SLOAD
  EMIT
  HALT
vote:
  POP
  CALLER
  PUSH 12
  MOD
  PUSH 100
  ADD
  DUP
  SLOAD
  PUSH 0
  EQ
  NOT
  JUMPI @done
  DUP
  PUSH 1
  SWAP
  SSTORE
  ARG 1
  PUSH 200
  ADD
  DUP
  SLOAD
  PUSH 1
  ADD
  SSTORE
  PUSH 0
  DUP
  SLOAD
  PUSH 1
  ADD
  SSTORE
  PUSH 1
  EMIT
  HALT
done:
  POP
  PUSH 0
  EMIT
  HALT
tally:
  POP
  ARG 1
  PUSH 200
  ADD
  SLOAD
  EMIT
  HALT
)";
        auto r = MVMAssembler::assemble(src);
        if (r.ok) {
            Serial0.printf("  voting:  %d bytes\n", r.len);
            directDeploy("voting", r.code, r.len);
        } else Serial0.println("voting err: " + r.error);
    }

    Serial0.println("\n[Example] 3 contracts submitted to mempool.");
    Serial0.println("  Wait for next block(s) to deploy, then try:");
    Serial0.println("  call counter 1    (increment)");
    Serial0.println("  call counter 0    (read)");
    Serial0.println("  call token 1 100  (mint 100)");
    Serial0.println("  call token 0      (balance)");
    Serial0.println("  call voting 1 1   (vote option 1)");
    Serial0.println("  call voting 2 1   (tally option 1)\n");
}
