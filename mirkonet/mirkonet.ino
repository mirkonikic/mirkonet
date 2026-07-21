#include <WiFi.h>
#include <esp_mac.h>
#include "driver/gpio.h"
#include "driver/adc.h"
#include "config.h"
#include "types.h"
#include "mvm.h"
#include "txauth.h"
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

static Block g_scratchBlock;

bool   g_assembling = false;
String g_asmBuffer, g_asmName;

Transaction buildTx(TxType type) {
    Transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.type = type;
    tx.sender = g_selfId;
    tx.to = ZERO_NODE;
    uint32_t chainNonce = g_chain.getNonce(g_selfId);
    if (g_txNonce < chainNonce) g_txNonce = chainNonce;
    tx.nonce = g_txNonce++;
    tx.timestamp = millis();
    tx.gasLimit = DEFAULT_GAS_LIMIT;
    tx.gasPrice = DEFAULT_GAS_PRICE;
    return tx;
}

bool findNodeByShort(const String& shortId, NodeID& out);
void deployExamples();
void deployRelay();

String webGetStats() {
    uint32_t staked = 0;
    const StakeInfo* si = g_chain.staking.findStake(g_selfId);
    if (si) staked = si->stakedAmount;

    String j = "{";
    j += "\"nodeId\":\"" + g_selfId.toString() + "\",";
    j += "\"shortId\":\"" + g_selfId.toShortStr() + "\",";
    j += "\"role\":\"" + String(roleName(g_consensus.selfRole)) + "\",";
    j += "\"staConnected\":" + String(g_wifi.staConnected ? "true" : "false") + ",";
    j += "\"ssid\":\"" + g_wifi.connectedSSID + "\",";
    j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    j += "\"height\":" + String(g_chain.height()) + ",";
    j += "\"epoch\":" + String(g_chain.currentEpoch()) + ",";
    j += "\"balance\":" + String(g_chain.getBalance(g_selfId)) + ",";
    j += "\"staked\":" + String(staked) + ",";
    j += "\"peers\":" + String(g_net.countAlive()) + ",";
    j += "\"validators\":" + String(g_chain.staking.activeCount) + ",";
    j += "\"contracts\":" + String(g_chain.contractCount) + ",";
    j += "\"mempool\":" + String(g_chain.mempoolSize) + ",";
    j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
    j += "\"uptime\":" + String(millis() / 1000);
    j += "}";
    return j;
}

String webGetPeers() {
    String j = "[";
    bool first = true;
    for (int i = 0; i < g_net.peerCount; i++) {
        PeerInfo& p = g_net.peers[i];
        if (!p.active) continue;
        if (!first) j += ",";
        first = false;
        j += "{\"shortId\":\"" + p.nodeId.toShortStr() + "\",";
        j += "\"id\":\"" + p.nodeId.toString() + "\",";
        j += "\"ip\":\"" + p.ip.toString() + "\",";
        j += "\"height\":" + String(p.chainHeight) + ",";
        j += "\"role\":\"" + String(roleName(p.role)) + "\",";
        j += "\"alive\":" + String(p.isAlive() ? "true" : "false") + "}";
    }
    j += "]";
    return j;
}

String webGetBlocks() {
    String j = "[";
    bool first = true;
    for (uint32_t i = 0; i < g_chain.blocksInMemory(); i++) {
        const Block& b = g_chain.chain[i];
        if (!first) j += ",";
        first = false;
        j += "{\"height\":" + String(b.header.index) + ",";
        j += "\"epoch\":" + String(b.header.epoch) + ",";
        j += "\"slot\":" + String(b.header.slot) + ",";
        j += "\"validator\":\"" + b.header.validator.toShortStr() + "\",";
        j += "\"txCount\":" + String(b.header.txCount) + ",";
        j += "\"reward\":" + String(b.header.reward) + ",";
        j += "\"hash\":\"" + b.blockHash.toShort() + "\"}";
    }
    j += "]";
    return j;
}

String webGetContracts() {
    String j = "[";
    bool first = true;
    for (int i = 0; i < g_chain.contractCount; i++) {
        Contract& c = g_chain.contracts[i];
        if (!c.active) continue;
        if (!first) j += ",";
        first = false;
        j += "{\"name\":\"" + String(c.name) + "\",";
        j += "\"addr\":\"" + c.addr.toHex() + "\",";
        j += "\"codeLen\":" + String(c.codeLen) + ",";
        j += "\"deployer\":\"" + c.deployer.toShortStr() + "\",";
        j += "\"hasCode\":" + String(c.hasCode ? "true" : "false") + ",";
        j += "\"balance\":" + String(c.balance) + ",";
        j += "\"storage\":[";
        bool sfirst = true;
        for (int s = 0; s < MVM_MAX_STORAGE; s++) {
            if (!c.storage[s].used) continue;
            if (!sfirst) j += ",";
            sfirst = false;
            j += "{\"key\":" + String(c.storage[s].key) + ",\"value\":" + String(c.storage[s].value) + "}";
        }
        j += "]}";
    }
    j += "]";
    return j;
}

bool webDeploy(const String& name, const String& asmSource, String& outMsg) {
    auto r = MVMAssembler::assemble(asmSource);
    if (!r.ok) {
        outMsg = "Assembly error: " + r.error;
        return false;
    }

    g_deployCache.store(name.c_str(), r.code, r.len);

    Transaction tx = buildTx(TxType::DEPLOY);
    tx.to = ZERO_NODE;
    strncpy(tx.data, name.c_str(), 15);
    tx.deployCodeLen = r.len;
    tx.deployCodeHash = sha256(r.code, r.len);
    tx.gasLimit = r.len * DEPLOY_GAS_PER_BYTE + 1000;

    g_chain.addToMempool(tx, true);
    g_net.broadcastTx(tx);
    g_led.flashDeploy();

    char buf[128];
    snprintf(buf, sizeof(buf), "Deploy '%s' submitted (%d bytes, gasLimit=%u, next block)",
             name.c_str(), r.len, tx.gasLimit);
    outMsg = String(buf);
    Serial0.printf("[Web] %s\n", buf);
    return true;
}

bool prepareCallTransaction(Transaction& tx, const String& name, String& outMsg) {
    strncpy(tx.data, name.c_str(), 15);
    tx.data[15] = '\0';

    Contract* ct = g_chain.findContract(name.c_str());
    if (ct) {
        tx.to = ct->deployer;
        return true;
    }

    auto* meta = g_deployCache.find(name.c_str());
    if (meta && meta->used) {
        tx.to = ZERO_NODE;
        outMsg = "Contract '" + name +
                 "' not in local state yet; broadcasting call using known metadata.\n";
        return true;
    }

    outMsg = "Contract not found locally: " + name +
             ". Wait for the deploy block to sync, or run 'contracts'.\n";
    return false;
}

void resetSavedChainState() {
    Preferences prefs;
    prefs.begin(WIFI_NVS_NAMESPACE, false);
    prefs.remove("chainstate");
    prefs.end();
    Serial0.println("[Chain] Saved chainstate cleared. Rebooting...");
    delay(1000);
    ESP.restart();
}

String webHandleCommand(const String& line) {
    String out = "";
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
        out += "====== MirkoNet PoS Commands ======\n";
        out += "-- Info --\n";
        out += "  status        Node overview\n";
        out += "  peers         Connected peers\n";
        out += "  chain         Blockchain\n";
        out += "  accounts      All accounts + balances\n";
        out += "  economy       Economic stats\n";
        out += "  heap          Free memory\n";
        out += "-- PoS --\n";
        out += "  validators    Active validator set\n";
        out += "  candidates    All staking candidates\n";
        out += "  faucet        Request " + String(FAUCET_AMOUNT) + " tokens\n";
        out += "  stake <amt>   Stake tokens (min " + String(MIN_STAKE) + ")\n";
        out += "  unstake <amt> Begin unstake cooldown\n";
        out += "  claim         Claim unstaked tokens\n";
        out += "-- Tokens --\n";
        out += "  balance       Your balance\n";
        out += "  transfer <id> <amt>  Send tokens\n";
        out += "-- Contracts --\n";
        out += "  call <n> [a]  Execute contract\n";
        out += "  contracts     List contracts\n";
        out += "  storage <n>   Inspect storage\n";
        out += "  example       Deploy demos\n";
        out += "  relay         Deploy relay contract\n";
        out += "-- Hardware (via smart contracts) --\n";
        out += "  call relay 1 <pin>  Turn ON relay\n";
        out += "  call relay 2 <pin>  Turn OFF relay\n";
        out += "  call relay 3 <pin>  Toggle relay\n";
        out += "  call relay 0 <pin>  Read state\n";
        out += "  Opcodes: GPIOWRITE GPIOREAD GPIOMODE ADCREAD\n";
        out += "  GPIO: 1-21 | ADC: 1-10 (12-bit)\n";
        out += "-- System --\n";
        out += "  debug         Full diagnostics\n";
        out += "  checkpoints   Chain history\n";
        out += "  chainreset    Clear saved chain, reboot\n";
        out += "  wifistatus    WiFi info\n";
        out += "  wifireset     Clear WiFi, reboot\n";
        out += "====================================\n";
    }
    else if (cmd == "status") {
        uint32_t staked = 0;
        const StakeInfo* si = g_chain.staking.findStake(g_selfId);
        if (si) staked = si->stakedAmount;
        out += "====== MirkoNet Status ======\n";
        out += "  Node:       " + g_selfId.toString() + "\n";
        out += "  Short ID:   " + g_selfId.toShortStr() + "\n";
        out += "  IP:         " + WiFi.localIP().toString() + "\n";
        char rbuf[32]; snprintf(rbuf, sizeof(rbuf), "  Role:       %s\n", roleName(g_consensus.selfRole)); out += rbuf;
        out += "  Chain:      " + String(g_chain.height()) + " blocks\n";
        out += "  Epoch:      " + String(g_chain.currentEpoch()) + "\n";
        out += "  Balance:    " + String(g_chain.getBalance(g_selfId)) + "\n";
        out += "  Staked:     " + String(staked) + "\n";
        out += "  Peers:      " + String(g_net.countAlive()) + "\n";
        out += "  Validators: " + String(g_chain.staking.activeCount) + "\n";
        out += "  Slot:       " + String(g_consensus.getCurrentSlot()) + "\n";
        out += "  Mempool:    " + String(g_chain.mempoolSize) + "\n";
        char hbuf[32]; snprintf(hbuf, sizeof(hbuf), "  Heap:       %d bytes\n", ESP.getFreeHeap()); out += hbuf;
        out += "=============================\n";
    }
    else if (cmd == "peers") {
        out += "====== Peers ======\n";
        for (int i = 0; i < g_net.peerCount; i++) {
            PeerInfo& p = g_net.peers[i];
            if (!p.active) continue;
            char pbuf[128];
            snprintf(pbuf, sizeof(pbuf), "  %s @ %s  h=%d  %s  %s\n",
                     p.nodeId.toShortStr().c_str(), p.ip.toString().c_str(),
                     p.chainHeight, roleName(p.role),
                     p.isAlive() ? "alive" : "lost");
            out += pbuf;
        }
        if (g_net.peerCount == 0) out += "  (none)\n";
        out += "===================\n";
    }
    else if (cmd == "chain") {
        out += "====== Blockchain ======\n";
        char cbuf[64];
        snprintf(cbuf, sizeof(cbuf), "  Height: %d  In-memory: %d  Offset: %d\n",
                 g_chain.height(), g_chain.blocksInMemory(), g_chain.chainOffset);
        out += cbuf;
        for (uint32_t i = 0; i < g_chain.blocksInMemory(); i++) {
            Block& b = g_chain.chain[i];
            snprintf(cbuf, sizeof(cbuf), "  #%d  txns=%d  by=%s  reward=%d\n",
                     b.header.index, b.header.txCount,
                     b.header.validator.toShortStr().c_str(), b.header.reward);
            out += cbuf;
        }
        out += "========================\n";
    }
    else if (cmd == "accounts") {
        out += "====== Accounts ======\n";
        for (int i = 0; i < g_chain.accountCount; i++) {
            if (!g_chain.accounts[i].used) continue;
            char abuf[80];
            snprintf(abuf, sizeof(abuf), "  %s  bal=%u  nonce=%u\n",
                     g_chain.accounts[i].owner.toShortStr().c_str(),
                     g_chain.accounts[i].balance,
                     g_chain.accounts[i].nonce);
            out += abuf;
        }
        out += "======================\n";
    }
    else if (cmd == "economy") {
        out += "====== Economy ======\n";
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "  Total supply:  %u tokens\n", g_chain.staking.totalSupply); out += ebuf;
        snprintf(ebuf, sizeof(ebuf), "  Total staked:  %u tokens (%.1f%%)\n",
                 g_chain.staking.totalStaked,
                 g_chain.staking.totalSupply > 0 ?
                 100.0*g_chain.staking.totalStaked/g_chain.staking.totalSupply : 0); out += ebuf;
        snprintf(ebuf, sizeof(ebuf), "  Block reward:  %u tokens/block\n", BLOCK_REWARD); out += ebuf;
        snprintf(ebuf, sizeof(ebuf), "  Min stake:     %u tokens\n", MIN_STAKE); out += ebuf;
        snprintf(ebuf, sizeof(ebuf), "  Faucet:        %u tokens (cooldown %ds)\n", FAUCET_AMOUNT, FAUCET_COOLDOWN/1000); out += ebuf;
        snprintf(ebuf, sizeof(ebuf), "  Accounts:      %d\n", g_chain.accountCount); out += ebuf;
        out += "=====================\n";
    }
    else if (cmd == "heap") {
        char hbuf[64];
        snprintf(hbuf, sizeof(hbuf), "Heap: %d bytes (min: %d)\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
        out += hbuf;
    }
    else if (cmd == "balance") {
        uint32_t bal = g_chain.getBalance(g_selfId);
        const StakeInfo* si = g_chain.staking.findStake(g_selfId);
        char bbuf[80];
        snprintf(bbuf, sizeof(bbuf), "Available: %u tokens\n", bal); out += bbuf;
        if (si) {
            snprintf(bbuf, sizeof(bbuf), "Staked:    %u tokens\n", si->stakedAmount); out += bbuf;
            if (si->unstakingAmount > 0) {
                snprintf(bbuf, sizeof(bbuf), "Unstaking: %u tokens\n", si->unstakingAmount); out += bbuf;
            }
            snprintf(bbuf, sizeof(bbuf), "Total:     %u tokens\n", bal + si->stakedAmount + si->unstakingAmount); out += bbuf;
        }
    }
    else if (cmd == "validators" || cmd == "candidates") {
        out += "====== Staking ======\n";
        for (int i = 0; i < g_chain.staking.stakeCount; i++) {
            StakeInfo& si = g_chain.staking.stakes[i];
            if (!si.used) continue;
            char sbuf[128];
            snprintf(sbuf, sizeof(sbuf), "  %s  staked=%u  candidate=%s  jailed=%s\n",
                     si.staker.toShortStr().c_str(), si.stakedAmount,
                     si.isCandidate ? "yes" : "no",
                     si.jailed ? "yes" : "no");
            out += sbuf;
        }
        out += "=====================\n";
    }
    else if (cmd == "contracts") {
        out += "====== Contracts ======\n";
        for (int i = 0; i < g_chain.contractCount; i++) {
            Contract& c = g_chain.contracts[i];
            if (!c.active) continue;
            char cbuf[128];
            snprintf(cbuf, sizeof(cbuf), "  %s @ %s | %dB | by %s | %s\n",
                     c.name, c.addr.toHex().c_str(), c.codeLen,
                     c.deployer.toShortStr().c_str(),
                     c.hasCode ? "LOCAL" : "REMOTE");
            out += cbuf;
        }
        if (g_chain.contractCount == 0) out += "  (none)\n";
        out += "=======================\n";
    }
    else if (cmd == "storage") {
        Contract* c = g_chain.findContract(arg1.c_str());
        if (c) {
            out += "Storage: " + String(c->name) + "\n";
            for (int i = 0; i < MVM_MAX_STORAGE; i++) {
                if (c->storage[i].used) {
                    char sbuf[40];
                    snprintf(sbuf, sizeof(sbuf), "  [%u] = %u\n", c->storage[i].key, c->storage[i].value);
                    out += sbuf;
                }
            }
        } else out += "Not found: " + arg1 + "\n";
    }
    else if (cmd == "debug") {
        char dbuf[128];
        out += "====== Debug Info ======\n";
        snprintf(dbuf, sizeof(dbuf), "  Heap free:     %d bytes\n", ESP.getFreeHeap()); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Heap min:      %d bytes\n", ESP.getMinFreeHeap()); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  CPU freq:      %d MHz\n", ESP.getCpuFreqMHz()); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Chip:          %s (%d cores)\n", ESP.getChipModel(), ESP.getChipCores()); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Flash:         %d KB\n", ESP.getFlashChipSize() / 1024); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Uptime:        %d seconds\n", millis() / 1000); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  WiFi:          %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Blocks:        %d (in-mem: %d/%d)\n", g_chain.height(), g_chain.blocksInMemory(), MAX_BLOCKS); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Contracts:     %d/%d\n", g_chain.contractCount, MAX_CONTRACTS); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Accounts:      %d/%d\n", g_chain.accountCount, MAX_ACCOUNTS); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Mempool:       %d/%d\n", g_chain.mempoolSize, MAX_PENDING_TX); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Peers:         %d/%d (alive: %d)\n", g_net.peerCount, MAX_PEERS, g_net.countAlive()); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Candidates:    %d/%d\n", g_chain.staking.stakeCount, MAX_CANDIDATES); out += dbuf;
        snprintf(dbuf, sizeof(dbuf), "  Validators:    %d/%d\n", g_chain.staking.activeCount, MAX_VALIDATORS); out += dbuf;
        out += "========================\n";
    }
    else if (cmd == "wifistatus") {
        out += "====== WiFi Status ======\n";
        out += "  AP Name:   " + g_wifi.apName + "\n";
        out += "  AP IP:     " + WiFi.softAPIP().toString() + "\n";
        char wbuf[64];
        snprintf(wbuf, sizeof(wbuf), "  AP clients: %d\n", WiFi.softAPgetStationNum()); out += wbuf;
        if (g_wifi.staConnected) {
            out += "  STA:       CONNECTED\n";
            out += "  STA SSID:  " + g_wifi.connectedSSID + "\n";
            out += "  STA IP:    " + WiFi.localIP().toString() + "\n";
            snprintf(wbuf, sizeof(wbuf), "  STA RSSI:  %d dBm\n", WiFi.RSSI()); out += wbuf;
        } else {
            out += "  STA:       NOT CONNECTED\n";
        }
        out += "=========================\n";
    }
    else if (cmd == "faucet") {
        Transaction tx = buildTx(TxType::FAUCET);
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_led.flashFaucet();
        out += "Faucet request submitted (next block)\n";
    }
    else if (cmd == "stake") {
        if (arg1.length() == 0) { out += "Usage: stake <amount>\n"; }
        else {
            Transaction tx = buildTx(TxType::STAKE);
            tx.value = arg1.toInt();
            g_chain.addToMempool(tx, true);
            g_net.broadcastTx(tx);
            g_led.flashStake();
            char sbuf[64]; snprintf(sbuf, sizeof(sbuf), "Stake %u submitted (next block)\n", tx.value); out += sbuf;
        }
    }
    else if (cmd == "unstake") {
        if (arg1.length() == 0) { out += "Usage: unstake <amount>\n"; }
        else {
            Transaction tx = buildTx(TxType::UNSTAKE);
            tx.value = arg1.toInt();
            g_chain.addToMempool(tx, true);
            g_net.broadcastTx(tx);
            g_led.flashUnstake();
            out += "Unstake submitted (next block)\n";
        }
    }
    else if (cmd == "claim") {
        Transaction tx = buildTx(TxType::CLAIM);
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_led.flashClaim();
        out += "Claim submitted (next block)\n";
    }
    else if (cmd == "transfer") {
        if (arg1.length() == 0 || arg2.length() == 0) {
            out += "Usage: transfer <6-char-hex-id> <amount>\n";
        } else {
            NodeID target;
            if (!findNodeByShort(arg1, target)) {
                out += "Node not found: " + arg1 + "\n";
                g_led.flashTxFail();
            } else {
                Transaction tx = buildTx(TxType::TRANSFER);
                tx.to = target;
                tx.value = arg2.toInt();
                g_chain.addToMempool(tx, true);
                g_net.broadcastTx(tx);
                g_led.flashTransfer();
                char tbuf[80]; snprintf(tbuf, sizeof(tbuf), "Transfer %u to %s submitted (next block)\n",
                                        tx.value, target.toShortStr().c_str()); out += tbuf;
            }
        }
    }
    else if (cmd == "call") {
        if (arg1.length() == 0) { out += "Usage: call <name> [args]\n"; }
        else {
            Transaction tx = buildTx(TxType::CALL);
            String callMsg;
            if (!prepareCallTransaction(tx, arg1, callMsg)) {
                out += callMsg;
            } else {
                out += callMsg;
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
                g_chain.addToMempool(tx, true);
                g_net.broadcastTx(tx);
                g_led.flashCall();
                out += "Call '" + arg1 + "' submitted (gasLimit=" + String(tx.gasLimit) + ", next block)\n";
            }
        }
    }
    else if (cmd == "checkpoints") {
        out += "====== Checkpoints ======\n";
        for (int i = 0; i < g_chain.checkpointCount; i++) {
            if (!g_chain.checkpoints[i].used) continue;
            char cpbuf[128];
            snprintf(cpbuf, sizeof(cpbuf), "  [%d..%d] hash=%s\n",
                     g_chain.checkpoints[i].fromBlock, g_chain.checkpoints[i].toBlock,
                     g_chain.checkpoints[i].lastBlockHash.toShort().c_str());
            out += cpbuf;
        }
        if (g_chain.checkpointCount == 0) out += "  (none)\n";
        out += "=========================\n";
    }
    else if (cmd == "wifireset") {
        out += "Resetting WiFi credentials and rebooting...\n";
        WiFiManager::resetCredentials();
    }
    else if (cmd == "chainreset") {
        out += "Clearing saved chain state and rebooting...\n";
        resetSavedChainState();
    }
    else if (cmd == "example") {
        out += "Use the serial console for 'example' (deploys demo contracts)\n";
    }
    else if (cmd == "relay") {
        deployRelay();
        out += "Relay contract submitted to mempool.\n";
        out += "After next block, use:\n";
        out += "  call relay 1 <pin>  Turn ON\n";
        out += "  call relay 2 <pin>  Turn OFF\n";
        out += "  call relay 3 <pin>  Toggle\n";
        out += "  call relay 0 <pin>  Read state\n";
    }
    else {
        out += "Unknown command. Type 'help'.\n";
    }

    return out;
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
    g_led.bootSequence();
    g_led.setState(LED_NO_WIFI);
    g_led.flashInitLed();
    Serial0.println("[Init]   LED OK");


    Serial0.println("[Init] Step 2/7: Flash storage...");

    Serial0.println("[Init] Step 3/7: WiFi...");
    bool wifiOk = g_wifi.begin();
    g_led.flashInitWifi();
    Serial0.printf("[Init]   WiFi result: STA %s\n", wifiOk ? "CONNECTED" : "not connected");
    Serial0.printf("[Init]   AP always active: %s\n", g_wifi.apName.c_str());
    Serial0.printf("[Init]   Portal: http://%s\n", WiFi.softAPIP().toString().c_str());
    Serial0.printf("[Init]   Heap after WiFi: %d\n", ESP.getFreeHeap());


    Serial0.println("[Init] Step 4/7: Identity...");
    uint8_t mac[6];
    WiFi.macAddress(mac);
    memcpy(g_selfId.id, mac, 6);
    {
        Preferences prefs;
        prefs.begin(WIFI_NVS_NAMESPACE, false);
        NodeID authId;
        if (TxAuth::begin(prefs, authId)) {
            g_selfId = authId;
            Serial0.println("[Init]   Transaction key loaded/generated");
        } else {
            Serial0.println("[Init]   WARNING: transaction key init failed, using MAC identity");
        }
        prefs.end();
    }
    g_led.flashInitIdentity();
    Serial0.println("[Init]   Node ID: " + g_selfId.toString());
    Serial0.println("[Init]   Short:   " + g_selfId.toShortStr());
    if (g_selfId.isZero()) {
        Serial0.println("[Init]   WARNING: MAC is all zeros! Trying esp_efuse_mac...");
        esp_efuse_mac_get_default(mac);
        memcpy(g_selfId.id, mac, 6);
        Serial0.println("[Init]   Retry ID: " + g_selfId.toString());
    }
    Serial0.printf("[Init]   Heap after identity: %d\n", ESP.getFreeHeap());


    Serial0.println("[Init] Step 5/7: Blockchain genesis...");
    g_chain.setLocalNode(g_selfId);
    g_chain.initGenesis();
    g_led.flashInitGenesis();


#if CHAINSTATE_NVS_PERSISTENCE
    {
        Preferences prefs;
        prefs.begin(WIFI_NVS_NAMESPACE, true);
        size_t blobLen = prefs.getBytesLength("chainstate");
        if (blobLen > 10) {
            static uint8_t nvsBuf[MAX_CHECKPOINT_STATE_BYTES];
            prefs.getBytes("chainstate", nvsBuf,
                           min((size_t)MAX_CHECKPOINT_STATE_BYTES, blobLen));
            prefs.end();
            if (g_chain.deserializeCheckpointState(nvsBuf, (uint16_t)blobLen)) {
                Serial0.printf("[Init]   Restored from NVS: height=%d accounts=%d validators=%d\n",
                              g_chain.height(), g_chain.accountCount,
                              g_chain.staking.activeCount);
            } else {
                Serial0.println("[Init]   NVS state restore failed, starting from genesis");
            }
        } else {
            prefs.end();
            Serial0.println("[Init]   No saved state in NVS, starting from genesis");
        }
    }
#else
    {
        Preferences prefs;
        prefs.begin(WIFI_NVS_NAMESPACE, false);
        if (prefs.getBytesLength("chainstate") > 0) {
            prefs.remove("chainstate");
            Serial0.println("[Init]   Removed stale saved chainstate from NVS");
        }
        prefs.end();
    }
    Serial0.println("[Init]   Saved chainstate restore disabled; starting from genesis");
#endif

    Serial0.printf("[Init]   Genesis hash: %s\n", g_chain.chain[0].blockHash.toShort().c_str());
    Serial0.printf("[Init]   Chain height: %d\n", g_chain.height());
    Serial0.printf("[Init]   Accounts: %d\n", g_chain.accountCount);
    Serial0.printf("[Init]   Heap after genesis: %d\n", ESP.getFreeHeap());


    Serial0.println("[Init] Step 6/7: Consensus...");
    g_consensus.init(g_selfId);
    g_consensus.updateRole(g_chain.staking);
    g_led.flashInitConsensus();
    Serial0.printf("[Init]   Role: %s\n", roleName(g_consensus.selfRole));
    Serial0.printf("[Init]   Validators: %d\n", g_chain.staking.activeCount);


    Serial0.println("[Init] Step 7/7: P2P Network...");
    g_net.init(g_selfId);
    g_led.flashInitNetwork();
    Serial0.printf("[Init]   UDP ports: %d, %d | TCP: %d\n",
                  UDP_DISCOVERY_PORT, UDP_GOSSIP_PORT, TCP_SYNC_PORT);
    Serial0.printf("[Init]   Heap after net: %d\n", ESP.getFreeHeap());


#if CHAINSTATE_NVS_PERSISTENCE

    g_nvsWriteCallback = [](const uint8_t* buf, uint16_t len) {
        Preferences prefs;
        prefs.begin(WIFI_NVS_NAMESPACE, false);
        prefs.putBytes("chainstate", buf, len);
        prefs.end();
    };
    Serial0.println("[Init] NVS chain persistence enabled");
#else
    g_nvsWriteCallback = nullptr;
    Serial0.println("[Init] NVS chain persistence disabled");
#endif


    g_wifi.onStats     = webGetStats;
    g_wifi.onBlocks    = webGetBlocks;
    g_wifi.onPeers     = webGetPeers;
    g_wifi.onContracts = webGetContracts;
    g_wifi.onCommand   = webHandleCommand;
    g_wifi.onDeploy    = webDeploy;
    Serial0.println("[Init] Web dashboard callbacks registered");


    g_blockchainLedCb = [](uint8_t event, uint8_t param) {
        switch (event) {
            case 0:  g_led.flashStateVerified(); break;
            case 1:
                switch (param) {
                    case 1: g_led.flashRejectIndex(); break;
                    case 2: g_led.flashRejectHash();  break;
                    case 3: g_led.flashRejectHash();  break;
                    case 5: g_led.flashRejectPoS();   break;
                    case 6: g_led.flashRejectSlot();  break;
                    case 7: g_led.flashRejectCode();  break;
                    case 8: g_led.flashRejectState(); break;
                    default: g_led.flashReject();     break;
                }
                break;
            case 2:  g_led.flashForTxType(param); break;
            case 3:  g_led.flashTxFail(); break;
            case 4:  g_led.flashValidateIndex(); break;
            case 5:  g_led.flashValidateHash(); break;
            case 6:  g_led.flashValidatePoS(); break;
            case 7:  g_led.flashValidateSlot(); break;
            case 8:  g_led.flashSync(); break;
            case 9:  g_led.flashPrune(); break;
            case 10: g_led.flashElection(); break;
        }
    };
    Serial0.println("[Init] Blockchain LED callback registered");


    g_gpioCallback = [](uint8_t op, uint8_t pin, uint32_t value) -> uint32_t {
        switch (op) {
            case 0: {
                gpio_set_level((gpio_num_t)pin, value ? 1 : 0);
                Serial0.printf("[GPIO] pin %d -> %s\n", pin, value ? "HIGH" : "LOW");
                return 0;
            }
            case 1: {
                uint32_t v = (uint32_t)gpio_get_level((gpio_num_t)pin);
                Serial0.printf("[GPIO] pin %d = %s\n", pin, v ? "HIGH" : "LOW");
                return v;
            }
            case 2: {
                gpio_config_t io_conf = {};
                io_conf.pin_bit_mask = (1ULL << pin);
                io_conf.mode         = value ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
                io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
                io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
                io_conf.intr_type    = GPIO_INTR_DISABLE;
                gpio_config(&io_conf);
                Serial0.printf("[GPIO] pin %d configured as %s\n",
                               pin, value ? "OUTPUT" : "INPUT");
                return 0;
            }
            case 3: {
                if (pin >= 1 && pin <= 10) {
                    adc1_channel_t ch = (adc1_channel_t)(pin - 1);
                    adc1_config_width(ADC_WIDTH_BIT_12);
                    adc1_config_channel_atten(ch, ADC_ATTEN_DB_12);
                    uint32_t v = (uint32_t)adc1_get_raw(ch);
                    Serial0.printf("[ADC] pin %d ch%d = %u\n", pin, (int)ch, v);
                    return v;
                }
                Serial0.printf("[ADC] pin %d not an ADC1 pin on ESP32-S3\n", pin);
                return 0;
            }
            case 4:
                Serial0.printf("[DAC] not supported on ESP32-S3\n");
                return 0;
        }
        return 0;
    };
    Serial0.println("[Init] GPIO callback registered");
    Serial0.printf("[Init] Allowed GPIO pins:");
    for (int i = 0; i < GPIO_MAX_ALLOWED; i++)
        Serial0.printf(" %d", GPIO_ALLOWED_PINS[i]);
    Serial0.println();

    g_led.flashInitComplete();
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
    Serial0.printf("  Pruning:    at %d blocks, keep %d, %d checkpoints, %d finalized max\n",
                  PRUNE_TRIGGER, PRUNE_KEEP, MAX_CHECKPOINTS, MAX_FINALIZED);
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
            g_led.flashWifiConnect();
        } else {
            Serial0.println("[WiFi] STA disconnected!");
            g_led.flashWifiDisconnect();
        }
    }


    if (staUp) {
        if (now - g_lastDiscovery >= DISCOVERY_INTERVAL) {
            g_lastDiscovery = now;
            g_net.sendDiscovery(g_chain.height(), g_consensus.selfRole, g_chain.currentEpoch(),
                               millis() - g_consensus.genesisTime, g_chain.tipHash());
            g_led.flashDiscoverySend();
        }

        if (g_consensus.selfRole == ROLE_VALIDATOR &&
            now - g_lastHeartbeat >= HEARTBEAT_INTERVAL) {
            g_lastHeartbeat = now;
            g_net.sendHeartbeat(g_chain.height(), g_consensus.getCurrentSlot(), g_chain.tipHash());
            g_led.flashGossipHeartbeat();
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

        if (now - g_lastSync >= SYNC_COOLDOWN) {
            g_lastSync = now;
            trySync();
        }

        if (now - g_lastSlotCheck >= 500) {
            g_lastSlotCheck = now;
            checkBlockProduction();
        }
    }


    if (!staUp && !g_wifi.scanRunning) {
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
                g_led.flashWifiReconnect();
            }
        }
    }


    static uint32_t lastAliveLog = 0;
    if (now - lastAliveLog >= 30000) {
        lastAliveLog = now;
        uint32_t sinceGenesis = now - g_consensus.genesisTime;
        bool inGrace = sinceGenesis < BOOT_GRACE_PERIOD;
        Serial0.printf("[Alive] t=%ds heap=%d wifi=%s peers=%d height=%d mem=%d fin=%d ckpts=%d role=%s%s\n",
                      now / 1000,
                      ESP.getFreeHeap(),
                      staUp ? "OK" : "NO",
                      g_net.countAlive(),
                      g_chain.height(),
                      g_chain.blocksInMemory(),
                      g_chain.finalizedCount,
                      g_chain.checkpointCount,
                      roleName(g_consensus.selfRole),
                      inGrace ? " [GRACE]" : "");
        g_led.flashAlive();
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
    Hash256 tipHash = ZERO_HASH;

    if (msg.payloadLen >= 5) {
        memcpy(&peerHeight, msg.payload, 4);
        role = (NodeRole)msg.payload[4];
    }
    if (msg.payloadLen >= 9) {
        memcpy(&epoch, msg.payload + 5, 4);
    }
    if (msg.payloadLen >= 45) {
        memcpy(tipHash.bytes, msg.payload + 13, HASH_SIZE);
    }

    int prevCount = g_net.peerCount;
    PeerInfo* p = g_net.addOrUpdatePeer(msg.sender, msg.senderIP, peerHeight, role, tipHash);


    if (g_net.peerCount > prevCount) {
        g_led.flashPeer();
    }


    if (msg.payloadLen >= 13) {
        uint32_t peerElapsed;
        memcpy(&peerElapsed, msg.payload + 9, 4);
        if (g_chain.chainLen <= 1) {
            g_consensus.syncGenesisTime(peerElapsed, millis());
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
            if (p) {
                p->chainHeight = h;
                p->lastSeen = millis();
                if (msg.payloadLen >= 40)
                    memcpy(p->tipHash.bytes, msg.payload + 8, HASH_SIZE);
            }
        }
        g_led.flashGossipHeartbeat();
        break;
    }

    case MsgType::TX_GOSSIP: {
        Transaction tx;
        P2PNetwork::deserializeTx(msg.payload, msg.payloadLen, tx);
        if (g_chain.addToMempool(tx)) {
            Serial0.println("[Gossip] TX from " + msg.sender.toShortStr() +
                           " type=" + String((int)tx.type));
            g_led.flashGossipTx();
        }
        break;
    }

    case MsgType::BLOCK_FULL: {

        P2PNetwork::deserializeBlock(msg.payload, msg.payloadLen, g_scratchBlock);

        Serial0.printf("[Gossip] Full block #%d from %s (%d txns)\n",
                      g_scratchBlock.header.index,
                      msg.sender.toShortStr().c_str(),
                      g_scratchBlock.header.txCount);

        if (PeerInfo* p = g_net.findPeer(msg.sender)) {
            uint32_t announcedHeight = g_scratchBlock.header.index + 1;
            if (announcedHeight >= p->chainHeight) {
                p->chainHeight = announcedHeight;
                p->tipHash = g_scratchBlock.blockHash;
            }
            p->lastSeen = millis();
        }

        uint32_t localHeight = g_chain.height();
        if (g_scratchBlock.header.index < localHeight) {
            Serial0.printf("[Gossip] Ignoring stale block #%d (local height=%d)\n",
                          g_scratchBlock.header.index, localHeight);
            break;
        }
        if (g_scratchBlock.header.index > localHeight) {
            Serial0.printf("[Gossip] Missing blocks before #%d (local height=%d); sync will catch up\n",
                          g_scratchBlock.header.index, localHeight);
            g_led.flashSyncStart();
            break;
        }

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

        g_led.flashGossipBlock();
        g_chain.setObservedSlot(g_consensus.getCurrentSlot());
        uint8_t result = g_chain.applyNetworkBlock(g_scratchBlock);
        if (result == 0) {
            Serial0.printf("[Gossip] Block #%d APPLIED! height=%d\n",
                          g_scratchBlock.header.index, g_chain.height());


            for (uint8_t l = 0; l < g_chain.vm.logCount(); l++) {
                const MVMLog& log = g_chain.vm.log(l);
                Serial0.printf("    Log[%d] data=%u topics=%d", l, log.data, log.topicCount);
                for (int t = 0; t < log.topicCount; t++)
                    Serial0.printf(" t%d=%u", t, log.topics[t]);
                Serial0.println();
            }


            for (int i = 0; i < g_scratchBlock.header.txCount; i++) {
                if (g_scratchBlock.txns[i].type == TxType::CALL) {
                    g_led.flashForOpcode(g_chain.vm.dominantOpcode());
                } else {
                    g_led.flashForTxType((uint8_t)g_scratchBlock.txns[i].type);
                }
            }


            for (int i = 0; i < g_scratchBlock.header.txCount; i++) {
                if (g_scratchBlock.txns[i].type == TxType::DEPLOY) {
                    Contract* c = g_chain.findContract(g_scratchBlock.txns[i].data);
                    if (c && c->active) {
                        g_net.broadcastContractInfo(c->name, c->host,
                                                    c->codeHash, c->codeLen);
                    }
                }
            }

            g_consensus.updateRole(g_chain.staking);
            g_consensus.lastProducedSlot = g_scratchBlock.header.slot;


            if (g_chain.height() % EPOCH_LENGTH == 0)
                g_led.flashEpoch();
            else
                g_led.flashValidation();
        } else {
            const char* reasons[] = {
                "OK", "wrong index", "prevHash mismatch", "bad hash",
                "chain full", "not a validator", "wrong slot producer",
                "missing bytecode", "stateRoot mismatch"
            };
            Serial0.printf("[Gossip] Block #%d REJECTED: %s (code=%d)\n",
                          g_scratchBlock.header.index,
                          result < 9 ? reasons[result] : "unknown",
                          result);

            switch (result) {
                case 1: g_led.flashRejectIndex(); break;
                case 2: g_led.flashRejectHash();  break;
                case 3: g_led.flashRejectHash();  break;
                case 5: g_led.flashRejectPoS();   break;
                case 6: g_led.flashRejectSlot();  break;
                case 7: g_led.flashRejectCode();  break;
                case 8: g_led.flashRejectState(); break;
                default: g_led.flashReject();     break;
            }
        }
        break;
    }

    case MsgType::DEPLOY_DATA: {
        break;
    }

    case MsgType::CONTRACT_INFO: {

        if (msg.payloadLen >= 16 + 6 + HASH_SIZE + 2) {
            char name[16];
            NodeID host;
            Hash256 codeHash;
            uint16_t codeLen;
            size_t off = 0;
            memcpy(name, msg.payload + off, 16); off += 16; name[15] = '\0';
            memcpy(host.id, msg.payload + off, 6); off += 6;
            memcpy(codeHash.bytes, msg.payload + off, HASH_SIZE); off += HASH_SIZE;
            memcpy(&codeLen, msg.payload + off, 2); off += 2;


            Contract* existing = g_chain.findContract(name);
            g_deployCache.storeMeta(name, codeLen, codeHash);
            if (!existing) {
                Serial0.printf("[Gossip] Learned deploy metadata for '%s' hosted by %s (%dB, hash=%s)\n",
                              name, host.toShortStr().c_str(), codeLen,
                              codeHash.toShort().c_str());
                g_led.flashGossipContract();
            } else {
                if (existing->host != host) {
                    Serial0.printf("[Gossip] Updated host for '%s': %s -> %s\n",
                                  name, existing->host.toShortStr().c_str(),
                                  host.toShortStr().c_str());
                    existing->host = host;
                }
                existing->codeLen = codeLen;
                existing->codeHash = codeHash;
            }
        }
        break;
    }

    case MsgType::BLOCK_ANN: {

        if (msg.payloadLen >= 4) {
            uint32_t blockIdx;
            memcpy(&blockIdx, msg.payload, 4);
            if (blockIdx >= g_chain.height()) {
                Serial0.printf("[Gossip] Block #%d announced (we're at %d, will sync)\n",
                              blockIdx, g_chain.height());
                g_led.flashGossipBlockAnn();
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
        g_led.flashTcpBlockReq();

        if (requestedIdx >= g_chain.chainOffset &&
            requestedIdx < g_chain.chainOffset + g_chain.chainLen) {
            uint32_t arrayIdx = requestedIdx - g_chain.chainOffset;
            g_net.sendBlock(client, g_chain.chain[arrayIdx]);
            Serial0.printf("[TCP] Sent block #%d\n", requestedIdx);
            g_led.flashTcpSent();
        } else {
            if (g_chain.chainLen == 0) {
                Serial0.printf("[TCP] Block #%d not in memory; pruned through #%d, use checkpoint\n",
                              requestedIdx,
                              g_chain.chainOffset > 0 ? g_chain.chainOffset - 1 : 0);
            } else {
                uint32_t memEnd = g_chain.chainOffset + g_chain.chainLen - 1;
                Serial0.printf("[TCP] Block #%d not in memory [%d..%d]\n",
                              requestedIdx, g_chain.chainOffset, memEnd);
            }
            g_led.flashTxFail();
        }
    }
    else if (type == MsgType::CHECKPOINT_REQ && len >= 7) {
        Serial0.println("[TCP] Checkpoint requested");
        g_led.flashTcpBlockReq();

        if (g_chain.checkpointCount > 0) {
            static uint8_t resp[MAX_CHECKPOINT_STATE_BYTES + 16]; size_t off = 0;
            resp[off++] = (uint8_t)MsgType::CHECKPOINT_RESP;
            memcpy(resp + off, g_selfId.id, 6); off += 6;

            uint16_t stateLen = g_chain.serializeState(resp + off,
                                                       MAX_CHECKPOINT_STATE_BYTES);
            off += stateLen;
            client.write(resp, off);
            client.flush();
            g_net.metrics.bytesTx += off;
            g_net.metrics.checkpointResponses++;
            Serial0.printf("[TCP] Sent checkpoint (%d bytes state)\n", stateLen);
            g_led.flashTcpSent();
        } else {
            Serial0.println("[TCP] No checkpoint available yet");
            g_led.flashTxFail();
        }
    }
    else if (type == MsgType::CODE_REQ && len >= 23) {

        char name[16];
        memcpy(name, buf+7, 16); name[15] = '\0';

        Serial0.printf("[TCP] Code request for '%s'\n", name);
        g_led.flashTcpCodeReq();

        Contract* c = g_chain.findContract(name);
        if (c && c->hasCode && c->codeLen > 0) {

            uint8_t resp[512]; size_t off = 0;
            resp[off++] = (uint8_t)MsgType::CODE_RESP;
            memcpy(resp+off, g_selfId.id, 6); off += 6;
            memcpy(resp+off, &c->codeLen, 2); off += 2;
            memcpy(resp+off, c->code, c->codeLen); off += c->codeLen;
            client.write(resp, off);
            client.flush();
            g_net.metrics.bytesTx += off;
            g_net.metrics.codeResponses++;
            Serial0.printf("[TCP] Sent '%s' bytecode (%d bytes)\n", name, c->codeLen);
            g_led.flashTcpSent();
        } else {
            Serial0.printf("[TCP] Don't have bytecode for '%s'\n", name);
            g_led.flashCodeFail();
        }
    }
    client.stop();
}


void fetchCodeForContract(Contract* c) {
    if (!c || c->hasCode) return;


    bool weAreHost = (c->host == g_selfId);


    auto* cached = g_deployCache.find(c->name);
    if (cached && cached->hasCode) {
        if (c->cacheCode(cached->code, cached->codeLen, !weAreHost)) {
            Serial0.printf("[Code] Loaded '%s' from deploy cache (%dB%s)\n",
                          c->name, cached->codeLen,
                          weAreHost ? "" : ", temporary");
            g_led.flashCodeCache();
            return;
        }
    }


    if (weAreHost) {
        Serial0.printf("[Code] We host '%s' but lost bytecode, asking peers...\n", c->name);
    }

    g_led.flashCodeFetch();


    IPAddress hostIP;
    bool found = false;
    if (!weAreHost) {
        for (int i = 0; i < g_net.peerCount; i++) {
            if (g_net.peers[i].nodeId == c->host && g_net.peers[i].isAlive()) {
                hostIP = g_net.peers[i].ip;
                found = true;
                break;
            }
        }
    }

    if (found) {
        uint8_t codeBuf[MVM_MAX_CODE];
        uint16_t codeLen;
        if (g_net.requestCode(hostIP, c->name, codeBuf, codeLen)) {
            bool keepBackup = CONTRACT_CODE_BACKUP_CACHE &&
                              g_chain.staking.isActiveValidator(g_selfId);
            if (c->cacheCode(codeBuf, codeLen, !weAreHost && !keepBackup)) {
                Serial0.printf("[Code] Fetched '%s' from host (%dB, hash verified, %s)\n",
                              c->name, codeLen,
                              (weAreHost || keepBackup) ? "cached backup" : "temporary for validation");
                g_led.flashCodeOk();
                return;
            } else {
                Serial0.printf("[Code] HASH MISMATCH for '%s' from host!\n", c->name);
                g_led.flashCodeHashBad();
            }
        }
    }


    for (int i = 0; i < g_net.peerCount; i++) {
        if (!g_net.peers[i].isAlive()) continue;
        if (found && g_net.peers[i].ip == hostIP) continue;

        uint8_t codeBuf[MVM_MAX_CODE];
        uint16_t codeLen;
        if (g_net.requestCode(g_net.peers[i].ip, c->name, codeBuf, codeLen)) {
            bool keepBackup = CONTRACT_CODE_BACKUP_CACHE &&
                              g_chain.staking.isActiveValidator(g_selfId);
            if (c->cacheCode(codeBuf, codeLen, !weAreHost && !keepBackup)) {
                Serial0.printf("[Code] Fetched '%s' from peer %s (%dB, hash verified, %s)\n",
                              c->name, g_net.peers[i].nodeId.toShortStr().c_str(), codeLen,
                              (weAreHost || keepBackup) ? "cached backup" : "temporary for validation");
                g_led.flashCodeOk();
                return;
            }
        }
    }

    Serial0.printf("[Code] Could not fetch '%s' from any peer\n", c->name);
    g_led.flashCodeFail();
}

void prefetchCodeForTxns(const Transaction* txns, uint8_t count) {
    for (int i = 0; i < count; i++) {
        if (txns[i].type == TxType::CALL) {
            Contract* c = g_chain.findContract(txns[i].data);
            if (c && !c->hasCode) fetchCodeForContract(c);
        }
    }
}


#define EMPTY_BLOCK_INTERVAL  10


static bool g_bootstrapDone = false;
static bool g_bootstrapFaucetPending = false;
static bool g_bootstrapStakePending  = false;

void autoBootstrap() {
    if (g_bootstrapDone) return;
    if (g_chain.getBalance(g_selfId) >= GENESIS_PER_NODE) {
        g_bootstrapFaucetPending = false;

        const StakeInfo* si = g_chain.staking.findStake(g_selfId);
        if (si && si->stakedAmount >= MIN_STAKE) {
            g_bootstrapDone = true;
            g_bootstrapStakePending = false;
            Serial0.println("[Bootstrap] Done — faucet + stake confirmed");
            return;
        }
        if (g_bootstrapStakePending) return;

        Transaction tx = buildTx(TxType::STAKE);
        tx.value = GENESIS_PER_NODE / 2;
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_bootstrapStakePending = true;
        Serial0.printf("[Bootstrap] Auto-stake %u submitted\n", tx.value);
        return;
    }

    if (g_chain.getBalance(g_selfId) == 0 && !g_bootstrapFaucetPending) {
        Transaction tx = buildTx(TxType::FAUCET);
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_bootstrapFaucetPending = true;
        Serial0.println("[Bootstrap] Auto-faucet submitted");
    }
}

void checkBlockProduction() {
    bool bootstrap = g_chain.isBootstrap();
    g_consensus.updateRole(g_chain.staking);

    if (millis() - g_consensus.genesisTime < BOOT_GRACE_PERIOD) return;

    if (networkNeedsSync()) {
        static uint32_t lastSyncGateLog = 0;
        if (millis() - lastSyncGateLog > 5000) {
            lastSyncGateLog = millis();
            Serial0.println("[Produce] Paused: network head is ahead/different, syncing first");
        }
        return;
    }


    if (bootstrap) autoBootstrap();


    bool shouldProd = false;
    if (bootstrap) {
        uint32_t slot = g_consensus.getCurrentSlot();
        if (slot != g_consensus.lastProducedSlot && g_consensus.isWithinSlotWindow()) {


            uint8_t myRank = 0;
            uint8_t totalNodes = 1;
            for (int i = 0; i < g_net.peerCount; i++) {
                if (!g_net.peers[i].isAlive()) continue;
                totalNodes++;
                if (memcmp(g_net.peers[i].nodeId.id, g_selfId.id, 6) < 0) myRank++;
            }
            shouldProd = ((slot % totalNodes) == myRank);
        }
    } else {
        shouldProd = g_consensus.shouldProduce(g_chain.staking, false, g_net.countAlive());
    }

    if (!shouldProd) return;

    uint32_t slot = g_consensus.getCurrentSlot();
    if (slot < g_chain.height()) {
        return;
    }

    bool hasTx = (g_chain.mempoolSize > 0);
    bool heartbeatSlot = (slot % EMPTY_BLOCK_INTERVAL == 0);

    if (!hasTx && !heartbeatSlot) {
        g_consensus.lastProducedSlot = slot;
        return;
    }

    g_consensus.lastProducedSlot = slot;

    g_led.flashProducing();
    Serial0.printf("\n=== PRODUCING BLOCK (slot %d, epoch %d, txns=%d%s%s) ===\n",
                  slot, g_chain.currentEpoch(), g_chain.mempoolSize,
                  hasTx ? "" : " heartbeat",
                  bootstrap ? " BOOTSTRAP" : "");

    if (!bootstrap && g_chain.staking.activeCount > 1) {
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

        for (uint8_t l = 0; l < g_chain.vm.logCount(); l++) {
            const MVMLog& log = g_chain.vm.log(l);
            Serial0.printf("    Log[%d] data=%u topics=%d", l, log.data, log.topicCount);
            for (int t = 0; t < log.topicCount; t++)
                Serial0.printf(" t%d=%u", t, log.topics[t]);
            Serial0.println();
        }


        const Block& produced = g_chain.lastBlock();
        for (int i = 0; i < produced.header.txCount; i++) {
            if (produced.txns[i].type == TxType::CALL) {

                g_led.flashForOpcode(g_chain.vm.dominantOpcode());
            }
        }

        g_net.broadcastFullBlock(produced);


        for (int i = 0; i < produced.header.txCount; i++) {
            if (produced.txns[i].type == TxType::DEPLOY) {
                Contract* c = g_chain.findContract(produced.txns[i].data);
                if (c && c->active) {
                    g_net.broadcastContractInfo(c->name, c->host,
                                                c->codeHash, c->codeLen);
                }
            }
        }

        g_consensus.updateRole(g_chain.staking);
        g_led.flashProduced();

        if (g_chain.blocksInMemory() >= PRUNE_TRIGGER) {
            g_led.flashPrune();
            g_chain.pruneChain();
        }
    }
}


Hash256 checkpointFingerprint(const uint8_t* buf, uint16_t len) {
    const uint16_t contentLen = 4 + 4 + HASH_SIZE * 4;
    if (len < contentLen + 4) return ZERO_HASH;
    return sha256(buf, contentLen);
}


bool checkpointHasValidCert(const uint8_t* buf, uint16_t len, NodeID* outSigner = nullptr) {
    uint16_t off = 0;
    Checkpoint ckpt;
    memset(&ckpt, 0, sizeof(ckpt));
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
    if (certCnt == 0 || certCnt > MAX_CHECKPOINT_SIGNATURES) return false;
    CheckpointCert certs[MAX_CHECKPOINT_SIGNATURES];
    memset(certs, 0, sizeof(certs));
    for (uint8_t i = 0; i < certCnt; i++) {
        if (off + 134 > len) return false;
        memcpy(certs[i].signer.id, buf + off, 6); off += 6;
        memcpy(certs[i].publicKey, buf + off, 64); off += 64;
        memcpy(certs[i].signature, buf + off, 64); off += 64;
    }

    if (off >= len) return false;
    uint8_t acctCnt = buf[off++];
    if (acctCnt > MAX_ACCOUNTS) return false;
    if (off + acctCnt * 19 > len) return false;
    off += acctCnt * 19;
    if (off >= len) return false;
    uint8_t stakeCnt = buf[off++];
    if (stakeCnt > MAX_CANDIDATES) return false;
    if (off + stakeCnt * 37 > len) return false;
    off += stakeCnt * 37;
    if (off >= len) return false;
    uint8_t activeCnt = buf[off++];
    if (activeCnt > MAX_VALIDATORS || off + activeCnt * 6 > len) return false;
    NodeID active[MAX_VALIDATORS];
    memset(active, 0, sizeof(active));
    for (uint8_t i = 0; i < activeCnt; i++) {
        memcpy(active[i].id, buf + off, 6); off += 6;
    }

    Hash256 h = ckpt.contentHash();
    for (uint8_t i = 0; i < certCnt; i++) {
        bool authorized = ckpt.toBlock < BOOTSTRAP_HEIGHT;
        for (uint8_t j = 0; j < activeCnt && !authorized; j++) {
            if (active[j] == certs[i].signer) authorized = true;
        }
        if (!authorized) continue;
        if (TxAuth::verifyHash(h, certs[i].publicKey,
                               certs[i].signature, certs[i].signer)) {
            if (outSigner) *outSigner = certs[i].signer;
            return true;
        }
    }
    return false;
}


bool checkpointHasQuorum(PeerInfo* source, const uint8_t* ckptBuf,
                         uint16_t ckptLen) {
    Hash256 fp = checkpointFingerprint(ckptBuf, ckptLen);
    if (fp == ZERO_HASH) return false;
    NodeID sourceSigner;
    if (!checkpointHasValidCert(ckptBuf, ckptLen, &sourceSigner)) {
        Serial0.println("[Sync] Checkpoint rejected: source has no valid certificate");
        return false;
    }

    uint8_t votes = 1;
    uint8_t reachable = 1;
    NodeID signers[MAX_CHECKPOINT_SIGNATURES];
    memset(signers, 0, sizeof(signers));
    signers[0] = sourceSigner;
    static uint8_t otherBuf[MAX_CHECKPOINT_STATE_BYTES];
    for (int i = 0; i < g_net.peerCount && votes < CHECKPOINT_QUORUM; i++) {
        PeerInfo& p = g_net.peers[i];
        if (!p.isAlive()) continue;
        if (source && p.ip == source->ip) continue;

        uint16_t otherLen = 0;
        if (!g_net.requestCheckpoint(p.ip, otherBuf, otherLen)) continue;
        reachable++;
        Hash256 otherFp = checkpointFingerprint(otherBuf, otherLen);
        NodeID signer;
        if (otherFp == fp && checkpointHasValidCert(otherBuf, otherLen, &signer)) {
            bool duplicate = false;
            for (uint8_t s = 0; s < votes; s++) {
                if (signers[s] == signer) duplicate = true;
            }
            if (!duplicate) signers[votes++] = signer;
        }
    }

    Serial0.printf("[Sync] Checkpoint quorum: %d/%d for %s\n",
                  votes, CHECKPOINT_QUORUM, fp.toShort().c_str());
    if (votes >= CHECKPOINT_QUORUM) return true;

#if CHECKPOINT_SINGLE_PEER_FALLBACK
    if (reachable < CHECKPOINT_QUORUM &&
        (g_chain.height() <= 1 || g_chain.checkpointCount > 0)) {
        Serial0.println("[Sync] Checkpoint single-peer fallback: no quorum peers reachable");
        return true;
    }
#endif

    return false;
}

int compareHash(const Hash256& a, const Hash256& b) {
    return memcmp(a.bytes, b.bytes, HASH_SIZE);
}


bool networkNeedsSync() {
    uint32_t ourHeight = g_chain.height();
    Hash256 ourTip = g_chain.tipHash();

    for (int i = 0; i < g_net.peerCount; i++) {
        PeerInfo& p = g_net.peers[i];
        if (!p.isAlive()) continue;
        if (p.chainHeight > ourHeight) return true;
        if (p.chainHeight == ourHeight && ourHeight > 1 &&
            p.tipHash != ZERO_HASH && p.tipHash != ourTip &&
            compareHash(p.tipHash, ourTip) < 0) {
            return true;
        }
    }
    return false;
}


void trySync() {
    uint32_t ourHeight = g_chain.height();
    Hash256 ourTip = g_chain.tipHash();
    PeerInfo* best = nullptr;
    PeerInfo* sameHeightFork = nullptr;
    for (int i = 0; i < g_net.peerCount; i++) {
        PeerInfo& p = g_net.peers[i];
        if (!p.isAlive()) continue;
        if (p.chainHeight > ourHeight && (!best || p.chainHeight > best->chainHeight)) {
            best = &p;
        } else if (p.chainHeight == ourHeight && ourHeight > 1 &&
                   p.tipHash != ZERO_HASH && p.tipHash != ourTip) {
            if (!sameHeightFork || compareHash(p.tipHash, sameHeightFork->tipHash) < 0)
                sameHeightFork = &p;
        }
    }

    if (!best && sameHeightFork) {
        if (compareHash(sameHeightFork->tipHash, ourTip) >= 0) return;

        Serial0.printf("[Sync] Same-height fork at height %d. Adopting peer %s tip %s over %s\n",
                      ourHeight,
                      sameHeightFork->nodeId.toShortStr().c_str(),
                      sameHeightFork->tipHash.toShort().c_str(),
                      ourTip.toShort().c_str());
        g_led.flashSyncReorg();

        uint32_t targetHeight = sameHeightFork->chainHeight;
        g_chain.resetToGenesis();
        g_consensus.updateRole(g_chain.staking);

        for (uint32_t i = 0; i < 10; i++) {
            uint32_t idx = g_chain.height();
            if (idx >= targetHeight) break;
            if (!g_net.requestBlock(sameHeightFork->ip, idx, g_scratchBlock)) {
                Serial0.printf("[Sync] Fork adopt download failed at #%d\n", idx);
                break;
            }
            prefetchCodeForTxns(g_scratchBlock.txns, g_scratchBlock.header.txCount);
            g_chain.setObservedSlot(g_consensus.getCurrentSlot());
            uint8_t r = g_chain.applyNetworkBlock(g_scratchBlock, false);
            if (r == 0) {
                Serial0.printf("[Sync] Fork adopt applied #%d, height=%d\n",
                              idx, g_chain.height());
                g_consensus.updateRole(g_chain.staking);
                g_led.flashSyncApplied();
            } else {
                Serial0.printf("[Sync] Fork adopt: block #%d failed (code=%d)\n", idx, r);
                g_led.flashSyncReject();
                break;
            }
        }
        return;
    }

    if (!best) return;

    uint32_t peerHeight = best->chainHeight;
    if (peerHeight <= ourHeight) return;

    Serial0.printf("[Sync] Peer %s has height %d (we: %d)\n",
                  best->nodeId.toShortStr().c_str(), peerHeight, ourHeight);

    g_led.setState(LED_SYNCING);
    g_led.flashSyncStart();

    bool haveNextBlock = g_net.requestBlock(best->ip, ourHeight, g_scratchBlock);
    if (!haveNextBlock) {
        for (int i = 0; i < g_net.peerCount; i++) {
            PeerInfo& p = g_net.peers[i];
            if (!p.isAlive()) continue;
            if (p.ip == best->ip) continue;
            if (p.chainHeight <= ourHeight) continue;
            if (g_net.requestBlock(p.ip, ourHeight, g_scratchBlock)) {
                best = &p;
                peerHeight = best->chainHeight;
                haveNextBlock = true;
                Serial0.printf("[Sync] Using alternate peer %s for block #%d\n",
                              best->nodeId.toShortStr().c_str(), ourHeight);
                break;
            }
        }
    }


    if (!haveNextBlock) {


        Serial0.println("[Sync] Peer doesn't have our next block — trying checkpoint sync");
        g_led.flashSyncDiverge();

        static uint8_t ckptBuf[MAX_CHECKPOINT_STATE_BYTES];
        uint16_t ckptLen = 0;
        if (g_net.requestCheckpoint(best->ip, ckptBuf, ckptLen)) {
            if (!checkpointHasQuorum(best, ckptBuf, ckptLen)) {
                Serial0.println("[Sync] Checkpoint rejected: no peer quorum");
                g_led.flashSyncReject();
            } else if (g_chain.deserializeCheckpointState(ckptBuf, ckptLen)) {
                Serial0.printf("[Sync] Checkpoint applied! Chain offset=%d\n",
                              g_chain.chainOffset);
                g_consensus.updateRole(g_chain.staking);
                g_led.flashSyncApplied();


                bool epochLinkProved = false;
                for (uint32_t i = 0; i < 10; i++) {
                    uint32_t idx = g_chain.height();
                    if (idx >= peerHeight) break;
                    if (!g_net.requestBlock(best->ip, idx, g_scratchBlock)) break;
                    prefetchCodeForTxns(g_scratchBlock.txns, g_scratchBlock.header.txCount);
                    g_chain.setObservedSlot(g_consensus.getCurrentSlot());
                    uint8_t r = g_chain.applyNetworkBlock(g_scratchBlock, false);
                    if (r == 0) {
                        if (!epochLinkProved) {


                            Serial0.printf("[Sync] Epoch-link PROVED: #%d.prevHash=%s\n",
                                          g_scratchBlock.header.index,
                                          g_scratchBlock.header.prevHash.toShort().c_str());
                            epochLinkProved = true;
                        }
                        Serial0.printf("[Sync] Applied #%d, height=%d\n", idx, g_chain.height());
                        g_consensus.updateRole(g_chain.staking);
                        g_led.flashSyncApplied();
                    } else {
                        Serial0.printf("[Sync] Block #%d rejected (code=%d)\n", idx, r);
                        g_led.flashSyncReject();
                        break;
                    }
                }
                Serial0.printf("[Sync] Checkpoint sync done. Height: %d\n", g_chain.height());
                g_led.flashSyncDone();
            } else {
                Serial0.println("[Sync] Checkpoint deserialization failed");
                g_led.flashSyncFail();
            }
        } else {
            Serial0.println("[Sync] Checkpoint request failed, will retry");
            g_led.flashSyncFail();
        }
        return;
    }


    prefetchCodeForTxns(g_scratchBlock.txns, g_scratchBlock.header.txCount);
    g_chain.setObservedSlot(g_consensus.getCurrentSlot());
    uint8_t result = g_chain.applyNetworkBlock(g_scratchBlock, false);

    if (result == 0) {
        Serial0.printf("[Sync] Block #%d applied. Height: %d\n",
                      ourHeight, g_chain.height());
        g_consensus.updateRole(g_chain.staking);
        g_led.flashSyncApplied();
        return;
    }

    if (result == 2) {


        Serial0.printf("[Sync] Chain forked at #%d. Peer is ahead (%d > %d). Reorging...\n",
                      ourHeight, peerHeight, ourHeight);
        g_led.flashSyncReorg();

        g_chain.resetToGenesis();
        g_consensus.updateRole(g_chain.staking);


        for (uint32_t i = 0; i < 10; i++) {
            uint32_t idx = g_chain.height();
            if (idx >= peerHeight) break;
            if (!g_net.requestBlock(best->ip, idx, g_scratchBlock)) {
                Serial0.printf("[Sync] Download failed at #%d\n", idx);
                break;
            }
            prefetchCodeForTxns(g_scratchBlock.txns, g_scratchBlock.header.txCount);
            g_chain.setObservedSlot(g_consensus.getCurrentSlot());
            uint8_t r = g_chain.applyNetworkBlock(g_scratchBlock, false);
            if (r == 0) {
                Serial0.printf("[Sync] Reorg applied #%d, height=%d\n",
                              idx, g_chain.height());
                g_consensus.updateRole(g_chain.staking);
                g_led.flashSyncApplied();
            } else {
                Serial0.printf("[Sync] Reorg: block #%d failed (code=%d)\n", idx, r);
                g_led.flashSyncReject();
                break;
            }
        }

        Serial0.printf("[Sync] Reorg done. Height: %d (peer: %d)\n",
                      g_chain.height(), peerHeight);
        g_led.flashSyncDone();
        return;
    }

    Serial0.printf("[Sync] Block #%d rejected (code=%d)\n", ourHeight, result);
    g_led.flashSyncReject();
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

    tx.to = ZERO_NODE;
    strncpy(tx.data, name, 15);
    tx.deployCodeLen = len;
    tx.deployCodeHash = sha256(code, len);
    tx.gasLimit = len * DEPLOY_GAS_PER_BYTE + 1000;

    g_chain.addToMempool(tx, true);
    g_net.broadcastTx(tx);
    g_led.flashDeploy();
    Serial0.printf("  [TX] Deploy '%s' submitted (%d bytes, gasLimit=%u)\n",
                  name, len, tx.gasLimit);
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
            tx.to = ZERO_NODE;
            strncpy(tx.data, g_asmName.c_str(), 15);
            tx.deployCodeLen = r.len;
            tx.deployCodeHash = sha256(r.code, r.len);
            tx.gasLimit = r.len * DEPLOY_GAS_PER_BYTE + 1000;

            g_chain.addToMempool(tx, true);
            g_net.broadcastTx(tx);
            g_led.flashDeploy();
            Serial0.printf("[TX] Deploy '%s' submitted (%d bytes, gasLimit=%u, next block)\n",
                          g_asmName.c_str(), r.len, tx.gasLimit);
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
        Serial0.println("  relay         Deploy relay contract");
        Serial0.println("-- Hardware (via smart contracts) --");
        Serial0.println("  call relay 1 <pin>  Turn ON relay");
        Serial0.println("  call relay 2 <pin>  Turn OFF relay");
        Serial0.println("  call relay 3 <pin>  Toggle relay");
        Serial0.println("  call relay 0 <pin>  Read state");
        Serial0.println("  Opcodes: GPIOWRITE GPIOREAD GPIOMODE ADCREAD");
        Serial0.println("  GPIO: 1-21 | ADC: 1-10 (12-bit)");
        Serial0.println("-- System --");
        Serial0.println("  debug         Full system diagnostics");
        Serial0.println("  checkpoints   Cryptographic chain history");
        Serial0.println("  metrics       Thesis data counters");
        Serial0.println("  metrics reset Clear thesis counters");
        Serial0.println("  heap          Free memory");
        Serial0.println("  chainreset    Clear saved chain state, reboot");
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
        Serial0.printf("  Default gas price: %u token/gas\n", DEFAULT_GAS_PRICE);
        Serial0.printf("  Min gas price:     %u token/gas\n", MIN_GAS_PRICE);
        Serial0.printf("  Default gas limit: %u gas\n", DEFAULT_GAS_LIMIT);
        Serial0.printf("  Deploy gas/byte:   %u gas\n", DEPLOY_GAS_PER_BYTE);
        Serial0.printf("  Faucet fee:        %u (free)\n", FAUCET_FEE);
        Serial0.printf("  Min stake:         %u tokens\n", MIN_STAKE);
        Serial0.printf("  Faucet:            %u tokens (cooldown %ds)\n",
                      FAUCET_AMOUNT, FAUCET_COOLDOWN/1000);
        Serial0.printf("  Unstake delay:     %d blocks\n", UNSTAKE_COOLDOWN);
        Serial0.printf("  Epoch length:      %d blocks\n", EPOCH_LENGTH);
        Serial0.printf("  Accounts:          %d\n", g_chain.accountCount);
        Serial0.println("  ---- Validator Income (Ethereum-style) ----");
        Serial0.println("  Per block: BLOCK_REWARD + sum(gasUsed * gasPrice)");
        Serial0.println("  CALL fee  = gasUsed * tx.gasPrice");
        Serial0.println("  DEPLOY fee = codeLen * DEPLOY_GAS_PER_BYTE * tx.gasPrice");
        Serial0.println("  Other fee  = tx.gasPrice (flat)");
        Serial0.println("=====================\n");
    }
    else if (cmd == "heap") {
        Serial0.printf("Heap: %d bytes (min: %d)\n",
                      ESP.getFreeHeap(), ESP.getMinFreeHeap());
    }

    else if (cmd == "wifireset") {
        WiFiManager::resetCredentials();
    }
    else if (cmd == "chainreset") {
        resetSavedChainState();
    }
    else if (cmd == "wifistatus") {
        g_wifi.printStatus();
    }
    else if (cmd == "checkpoints") {
        g_chain.printCheckpoints();
    }

    else if (cmd == "metrics") {
        if (arg1 == "reset") {
            g_chain.metrics = ChainMetrics();
            g_net.metrics = P2PNetwork::NetMetrics();
            Serial0.println("[Metrics] Reset counters");
            return;
        }
        const ChainMetrics& cm = g_chain.metrics;
        const P2PNetwork::NetMetrics& nm = g_net.metrics;
        uint32_t avgRoot = cm.stateRootCalls ?
            cm.stateRootMicrosTotal / cm.stateRootCalls : 0;
        uint32_t avgValidate = cm.blocksValidated ?
            cm.validateMicrosTotal / cm.blocksValidated : 0;

        Serial0.println("\n====== Thesis Metrics ======");
        Serial0.printf("  Height logical,in_mem,offset: %u,%u,%u\n",
                      g_chain.height(), g_chain.blocksInMemory(), g_chain.chainOffset);
        Serial0.printf("  Heap free,min,total: %u,%u,%u\n",
                      ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getHeapSize());
        Serial0.printf("  State counts accounts,contracts,stakes,validators,mempool: %u,%u,%u,%u,%u\n",
                      g_chain.accountCount, g_chain.contractCount,
                      g_chain.staking.stakeCount, g_chain.staking.activeCount,
                      g_chain.mempoolSize);
        Serial0.printf("  Blocks produced,validated,rejected: %u,%u,%u\n",
                      cm.blocksProduced, cm.blocksValidated, cm.blocksRejected);
        Serial0.printf("  TX executed,rejected: %u,%u\n", cm.txExecuted, cm.txRejected);
        Serial0.printf("  StateRoot calls,total_us,avg_us: %u,%u,%u\n",
                      cm.stateRootCalls, cm.stateRootMicrosTotal, avgRoot);
        Serial0.printf("  Validate total_us,avg_us: %u,%u\n",
                      cm.validateMicrosTotal, avgValidate);
        Serial0.printf("  Prune count,last_checkpoint_bytes: %u,%u\n",
                      cm.pruneCount, cm.lastPruneBytes);
        Serial0.printf("  Checkpoint restore,reject: %u,%u\n",
                      cm.checkpointRestoreCount, cm.checkpointRejectCount);
        Serial0.printf("  Hardware replays,bytecode_misses: %u,%u\n",
                      cm.hardwareReplays, cm.bytecodeMisses);
        Serial0.printf("  Net bytes_tx,bytes_rx: %u,%u\n", nm.bytesTx, nm.bytesRx);
        Serial0.printf("  Net tx_gossip,block_gossip: %u,%u\n",
                      nm.txGossipSent, nm.blockGossipSent);
        Serial0.printf("  Net block_req,block_resp: %u,%u\n",
                      nm.blockRequests, nm.blockResponses);
        Serial0.printf("  Net ckpt_req,ckpt_resp: %u,%u\n",
                      nm.checkpointRequests, nm.checkpointResponses);
        Serial0.printf("  Net code_req,code_resp: %u,%u\n",
                      nm.codeRequests, nm.codeResponses);
        Serial0.println("  CSV header:");
        Serial0.println("  height,in_mem,heap_free,heap_min,accounts,contracts,stakes,validators,blocks_prod,blocks_val,blocks_rej,tx_ok,tx_rej,root_calls,root_avg_us,validate_avg_us,prunes,ckpt_bytes,bytes_tx,bytes_rx,code_req,code_resp,bytecode_miss");
        Serial0.printf("  CSV data: %u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                      g_chain.height(), g_chain.blocksInMemory(),
                      ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                      g_chain.accountCount, g_chain.contractCount,
                      g_chain.staking.stakeCount, g_chain.staking.activeCount,
                      cm.blocksProduced, cm.blocksValidated, cm.blocksRejected,
                      cm.txExecuted, cm.txRejected, cm.stateRootCalls, avgRoot,
                      avgValidate, cm.pruneCount, cm.lastPruneBytes,
                      nm.bytesTx, nm.bytesRx, nm.codeRequests, nm.codeResponses,
                      cm.bytecodeMisses);
        Serial0.println("============================\n");
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
        uint32_t memEnd = g_chain.blocksInMemory() > 0
                        ? g_chain.chainOffset + g_chain.blocksInMemory() - 1
                        : g_chain.chainOffset;
        Serial0.printf("  In memory:     %d / %d blocks [%d..%d]\n",
                      g_chain.blocksInMemory(), MAX_BLOCKS,
                      g_chain.chainOffset, memEnd);
        Serial0.printf("  Finalized:     %d / %d compact headers\n", g_chain.finalizedCount, MAX_FINALIZED);
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
        Serial0.printf("  Finalized array:     %d bytes\n", sizeof(CompactHeader) * MAX_FINALIZED);
        Serial0.printf("  Contract array:      %d bytes\n", sizeof(Contract) * MAX_CONTRACTS);
        Serial0.printf("  Mempool array:       %d bytes\n", sizeof(Transaction) * MAX_PENDING_TX);
        Serial0.println("========================\n");
    }

    else if (cmd == "faucet") {
        Transaction tx = buildTx(TxType::FAUCET);
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_led.flashFaucet();
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
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_led.flashStake();
        Serial0.printf("[TX] Stake %u submitted (next block)\n", tx.value);
    }
    else if (cmd == "unstake") {
        if (arg1.length() == 0) { Serial0.println("Usage: unstake <amount>"); return; }
        Transaction tx = buildTx(TxType::UNSTAKE);
        tx.value = arg1.toInt();
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_led.flashUnstake();
        Serial0.println("[TX] Unstake submitted (next block)");
    }
    else if (cmd == "claim") {
        Transaction tx = buildTx(TxType::CLAIM);
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_led.flashClaim();
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
            g_led.flashTxFail();
            return;
        }
        Transaction tx = buildTx(TxType::TRANSFER);
        tx.to = target;
        tx.value = arg2.toInt();
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_led.flashTransfer();
        Serial0.printf("[TX] Transfer %u to %s submitted (next block)\n",
                      tx.value, target.toShortStr().c_str());
    }

    else if (cmd == "deploy") {
        if (arg1.length() == 0) { Serial0.println("Usage: deploy <name>"); return; }
        g_assembling = true; g_asmBuffer = ""; g_asmName = arg1;
        Serial0.println("Enter mVM assembly (end with '.'):");
    }
    else if (cmd == "call") {
        if (arg1.length() == 0) { Serial0.println("Usage: call <name> [args]"); return; }
        Transaction tx = buildTx(TxType::CALL);
        String callMsg;
        if (!prepareCallTransaction(tx, arg1, callMsg)) {
            Serial0.print(callMsg);
            return;
        }
        if (callMsg.length() > 0) Serial0.print(callMsg);
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
        g_chain.addToMempool(tx, true);
        g_net.broadcastTx(tx);
        g_led.flashCall();
        Serial0.println("[TX] Call '" + arg1 + "' submitted (gasLimit=" +
                        String(tx.gasLimit) + ", next block)");
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
    else if (cmd == "relay") {
        deployRelay();
    }
    else {
        Serial0.println("Unknown command. Type 'help'.");
    }
}

void deployExamples() {
    Serial0.println("\n[Example] Deploying demo contracts (mVM v3, 5-bit ISA)...\n");

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


    {
        String src = R"(
; relay - blockchain-controlled GPIO (active-LOW)
; arg0: 0=status 1=on 2=off 3=toggle
; arg1: pin number
; Note: GPIO is inverted for active-LOW relay modules
;   ON  = GPIO LOW  (PUSH 0 GPIOWRITE)
;   OFF = GPIO HIGH (PUSH 1 GPIOWRITE)
ARG 0
DUP
PUSH 0
EQ
JUMPI @status
DUP
PUSH 1
EQ
JUMPI @on
DUP
PUSH 2
EQ
JUMPI @off
DUP
PUSH 3
EQ
JUMPI @toggle
REVERT
status:
  POP
  ARG 1
  SLOAD
  EMIT
  HALT
on:
  POP
  ARG 1
  PUSH 1
  GPIOMODE
  ARG 1
  PUSH 0
  GPIOWRITE
  ARG 1
  PUSH 1
  SSTORE
  PUSH 1
  EMIT
  HALT
off:
  POP
  ARG 1
  PUSH 1
  GPIOMODE
  ARG 1
  PUSH 1
  GPIOWRITE
  ARG 1
  PUSH 0
  SSTORE
  PUSH 0
  EMIT
  HALT
toggle:
  POP
  ARG 1
  PUSH 1
  GPIOMODE
  ARG 1
  SLOAD
  PUSH 0
  EQ
  JUMPI @t_on
  ARG 1
  PUSH 1
  GPIOWRITE
  ARG 1
  PUSH 0
  SSTORE
  PUSH 0
  EMIT
  HALT
t_on:
  ARG 1
  PUSH 0
  GPIOWRITE
  ARG 1
  PUSH 1
  SSTORE
  PUSH 1
  EMIT
  HALT
)";
        auto r = MVMAssembler::assemble(src);
        if (r.ok) {
            Serial0.printf("  relay:   %d bytes\n", r.len);
            directDeploy("relay", r.code, r.len);
        } else Serial0.println("relay err: " + r.error);
    }

    Serial0.println("\n[Example] 4 contracts submitted to mempool.");
    Serial0.println("  Wait for next block(s) to deploy, then try:");
    Serial0.println("  call counter 1    (increment)");
    Serial0.println("  call counter 0    (read)");
    Serial0.println("  call token 1 100  (mint 100)");
    Serial0.println("  call token 0      (balance)");
    Serial0.println("  call voting 1 1   (vote option 1)");
    Serial0.println("  call voting 2 1   (tally option 1)");
    Serial0.println("  call relay 1 4    (turn ON relay on GPIO 4)");
    Serial0.println("  call relay 2 4    (turn OFF relay on GPIO 4)");
    Serial0.println("  call relay 3 4    (toggle relay on GPIO 4)");
    Serial0.println("  call relay 0 4    (read relay state on GPIO 4)\n");
}

void deployRelay() {
    Serial0.println("\n[Relay] Deploying relay contract...\n");

    String src = R"(
; relay - blockchain-controlled GPIO (active-LOW)
; arg0: 0=status 1=on 2=off 3=toggle
; arg1: pin number
; Note: GPIO is inverted for active-LOW relay modules
;   ON  = GPIO LOW  (PUSH 0 GPIOWRITE)
;   OFF = GPIO HIGH (PUSH 1 GPIOWRITE)
ARG 0
DUP
PUSH 0
EQ
JUMPI @status
DUP
PUSH 1
EQ
JUMPI @on
DUP
PUSH 2
EQ
JUMPI @off
DUP
PUSH 3
EQ
JUMPI @toggle
REVERT
status:
  POP
  ARG 1
  SLOAD
  EMIT
  HALT
on:
  POP
  ARG 1
  PUSH 1
  GPIOMODE
  ARG 1
  PUSH 0
  GPIOWRITE
  ARG 1
  PUSH 1
  SSTORE
  PUSH 1
  EMIT
  HALT
off:
  POP
  ARG 1
  PUSH 1
  GPIOMODE
  ARG 1
  PUSH 1
  GPIOWRITE
  ARG 1
  PUSH 0
  SSTORE
  PUSH 0
  EMIT
  HALT
toggle:
  POP
  ARG 1
  PUSH 1
  GPIOMODE
  ARG 1
  SLOAD
  PUSH 0
  EQ
  JUMPI @t_on
  ARG 1
  PUSH 1
  GPIOWRITE
  ARG 1
  PUSH 0
  SSTORE
  PUSH 0
  EMIT
  HALT
t_on:
  ARG 1
  PUSH 0
  GPIOWRITE
  ARG 1
  PUSH 1
  SSTORE
  PUSH 1
  EMIT
  HALT
)";

    auto r = MVMAssembler::assemble(src);
    if (r.ok) {
        Serial0.printf("  relay: %d bytes\n", r.len);
        directDeploy("relay", r.code, r.len);
        Serial0.println("\n[Relay] Contract submitted to mempool.");
        Serial0.println("  After next block, use:");
        Serial0.println("  call relay 1 <pin>  Turn ON");
        Serial0.println("  call relay 2 <pin>  Turn OFF");
        Serial0.println("  call relay 3 <pin>  Toggle");
        Serial0.println("  call relay 0 <pin>  Read state\n");
    } else {
        Serial0.println("[Relay] Assembly error: " + r.error);
    }
}
