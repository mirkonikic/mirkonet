#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include "config.h"

// MirkoNet WiFi Manager + Web Dashboard
// - Network statistics and node insight
// - Smart contract deployment via assembly
// - Console command execution
// - WiFi scanner and setup

#define WIFI_CONNECT_TIMEOUT  15000
#define WIFI_NVS_NAMESPACE    "mirkonet"
#define AP_CHANNEL            1
#define DNS_PORT              53

// Callback types for decoupling from globals
typedef String (*WebCmdCallback)(const String& cmd);
typedef String (*StatsCallback)();
typedef String (*PeersCallback)();
typedef String (*ContractsCallback)();
typedef bool   (*DeployCallback)(const String& name, const String& asmSource, String& outMsg);

class WiFiManager {
public:
    bool     staConnected = false;
    String   apName;
    String   connectedSSID;
    bool     portalActive = false;

    // Callbacks set by main sketch
    WebCmdCallback  onCommand   = nullptr;
    StatsCallback   onStats     = nullptr;
    PeersCallback   onPeers     = nullptr;
    ContractsCallback onContracts = nullptr;
    DeployCallback  onDeploy    = nullptr;

    bool begin() {
        Serial.println("[WiFi] === WiFi Manager Starting ===");

        uint8_t mac[6];
        WiFi.macAddress(mac);
        char buf[32];
        snprintf(buf, sizeof(buf), "mnet_node_%02X%02X%02X",
                 mac[3], mac[4], mac[5]);
        apName = String(buf);
        Serial.println("[WiFi] AP name: " + apName);

        Serial.println("[WiFi] Setting WIFI_AP_STA mode...");
        WiFi.mode(WIFI_AP_STA);
        delay(100);

        Serial.println("[WiFi] Starting soft AP...");
        WiFi.softAP(apName.c_str(), NULL, AP_CHANNEL);
        delay(200);
        Serial.println("[WiFi] AP IP: " + WiFi.softAPIP().toString());

        startPortal();

        Preferences prefs;
        Serial.println("[WiFi] Reading NVS credentials...");
        prefs.begin(WIFI_NVS_NAMESPACE, true);
        String ssid = prefs.getString("ssid", "");
        String pass = prefs.getString("pass", "");
        prefs.end();

        if (ssid.length() > 0) {
            Serial.printf("[WiFi] Saved network: '%s' (pass length: %d)\n",
                          ssid.c_str(), pass.length());
            Serial.print("[WiFi] Connecting to STA");

            WiFi.begin(ssid.c_str(), pass.c_str());

            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED &&
                   millis() - start < WIFI_CONNECT_TIMEOUT) {
                delay(500);
                Serial.print(".");
                handlePortal();
            }

            if (WiFi.status() == WL_CONNECTED) {
                staConnected = true;
                connectedSSID = ssid;
                Serial.println(" OK!");
                Serial.println("[WiFi] STA IP: " + WiFi.localIP().toString());
                Serial.println("[WiFi] RSSI: " + String(WiFi.RSSI()) + " dBm");
            } else {
                Serial.println(" FAILED!");
                Serial.printf("[WiFi] Status code: %d\n", WiFi.status());
                Serial.println("[WiFi] Will keep trying in background");
            }
        } else {
            Serial.println("[WiFi] No saved credentials - AP-only mode");
            Serial.println("[WiFi] Connect to '" + apName + "' and open 192.168.4.1");
        }

        Serial.println("[WiFi] === Portal always active at 192.168.4.1 ===");
        return staConnected;
    }

    void handlePortal() {
        if (!portalActive) return;
        _dns.processNextRequest();
        _server.handleClient();
    }

    static void resetCredentials() {
        Preferences prefs;
        prefs.begin(WIFI_NVS_NAMESPACE, false);
        prefs.remove("ssid");
        prefs.remove("pass");
        prefs.end();
        Serial.println("[WiFi] Credentials cleared. Rebooting...");
        delay(1000);
        ESP.restart();
    }

    void printStatus() {
        Serial.println("\n====== WiFi Status ======");
        Serial.println("  Mode:      AP+STA (always-on AP)");
        Serial.println("  AP Name:   " + apName);
        Serial.println("  AP IP:     " + WiFi.softAPIP().toString());
        Serial.printf("  AP clients: %d\n", WiFi.softAPgetStationNum());
        if (staConnected) {
            Serial.println("  STA:       CONNECTED");
            Serial.println("  STA SSID:  " + connectedSSID);
            Serial.println("  STA IP:    " + WiFi.localIP().toString());
            Serial.printf("  STA RSSI:  %d dBm\n", WiFi.RSSI());
            Serial.printf("  Channel:   %d\n", WiFi.channel());
        } else {
            Serial.println("  STA:       NOT CONNECTED");
            Serial.printf("  STA status: %d\n", WiFi.status());
        }
        Serial.println("  Portal:    http://192.168.4.1 (always active)");
        Serial.println("=========================\n");
    }

private:
    WebServer  _server{80};
    DNSServer  _dns;

    void startPortal() {
        Serial.println("[WiFi] Starting DNS server...");
        _dns.start(DNS_PORT, "*", WiFi.softAPIP());

        Serial.println("[WiFi] Registering HTTP routes...");
        _server.on("/", HTTP_GET, [this]() { handleRoot(); });
        _server.on("/scan", HTTP_GET, [this]() { handleScan(); });
        _server.on("/save", HTTP_POST, [this]() { handleSave(); });
        _server.on("/status", HTTP_GET, [this]() { handleWebStatus(); });
        // API endpoints
        _server.on("/api/stats", HTTP_GET, [this]() { handleApiStats(); });
        _server.on("/api/peers", HTTP_GET, [this]() { handleApiPeers(); });
        _server.on("/api/contracts", HTTP_GET, [this]() { handleApiContracts(); });
        _server.on("/api/console", HTTP_POST, [this]() { handleApiConsole(); });
        _server.on("/api/deploy", HTTP_POST, [this]() { handleApiDeploy(); });
        _server.onNotFound([this]() { handleRoot(); });
        _server.begin();

        portalActive = true;
        Serial.println("[WiFi] Portal started on 192.168.4.1");
    }

    // ==================== API Handlers ====================

    void handleApiStats() {
        if (onStats) {
            _server.send(200, "application/json", onStats());
        } else {
            _server.send(503, "application/json", "{\"error\":\"not ready\"}");
        }
    }

    void handleApiPeers() {
        if (onPeers) {
            _server.send(200, "application/json", onPeers());
        } else {
            _server.send(503, "application/json", "{\"error\":\"not ready\"}");
        }
    }

    void handleApiContracts() {
        if (onContracts) {
            _server.send(200, "application/json", onContracts());
        } else {
            _server.send(503, "application/json", "{\"error\":\"not ready\"}");
        }
    }

    void handleApiConsole() {
        String cmd = _server.arg("plain");
        cmd.trim();
        if (cmd.length() == 0) {
            _server.send(400, "application/json", "{\"error\":\"empty command\"}");
            return;
        }
        if (onCommand) {
            String result = onCommand(cmd);
            // Escape JSON
            result.replace("\\", "\\\\");
            result.replace("\"", "\\\"");
            result.replace("\n", "\\n");
            result.replace("\r", "");
            result.replace("\t", "\\t");
            _server.send(200, "application/json", "{\"output\":\"" + result + "\"}");
        } else {
            _server.send(503, "application/json", "{\"error\":\"not ready\"}");
        }
    }

    void handleApiDeploy() {
        String name = _server.arg("name");
        String code = _server.arg("code");
        name.trim();
        code.trim();
        if (name.length() == 0 || code.length() == 0) {
            _server.send(400, "application/json", "{\"error\":\"name and code required\"}");
            return;
        }
        if (name.length() > 15) {
            _server.send(400, "application/json", "{\"error\":\"name max 15 chars\"}");
            return;
        }
        if (onDeploy) {
            String msg;
            bool ok = onDeploy(name, code, msg);
            msg.replace("\\", "\\\\");
            msg.replace("\"", "\\\"");
            msg.replace("\n", "\\n");
            _server.send(ok ? 200 : 400, "application/json",
                        "{\"ok\":" + String(ok ? "true" : "false") +
                        ",\"message\":\"" + msg + "\"}");
        } else {
            _server.send(503, "application/json", "{\"error\":\"not ready\"}");
        }
    }

    // ==================== Web Pages ====================

    void handleRoot() {
        String html = R"rawhtml(
<!DOCTYPE html>
<html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>MirkoNet</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'SF Mono',SFMono-Regular,Consolas,'Liberation Mono',Menlo,monospace;background:#0a0a0f;color:#c8c8d0;min-height:100vh}
.header{background:linear-gradient(135deg,#0d1117 0%,#161b22 100%);border-bottom:1px solid #21262d;padding:16px 20px;display:flex;align-items:center;gap:12px}
.header h1{font-size:18px;color:#00e676;font-weight:700;letter-spacing:-0.5px}
.header .ver{font-size:11px;color:#484f58;background:#21262d;padding:2px 8px;border-radius:10px}
.tabs{display:flex;background:#0d1117;border-bottom:1px solid #21262d;overflow-x:auto;-webkit-overflow-scrolling:touch}
.tab{padding:12px 20px;cursor:pointer;color:#484f58;font-size:13px;font-weight:600;border-bottom:2px solid transparent;white-space:nowrap;transition:all .2s}
.tab:hover{color:#c8c8d0}
.tab.active{color:#00e676;border-bottom-color:#00e676}
.page{display:none;padding:16px;max-width:600px;margin:0 auto}
.page.active{display:block}
.card{background:#161b22;border:1px solid #21262d;border-radius:8px;padding:16px;margin-bottom:12px}
.card h3{font-size:13px;color:#00e676;margin-bottom:12px;text-transform:uppercase;letter-spacing:1px}
.row{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #21262d1a;font-size:13px}
.row:last-child{border-bottom:none}
.row .k{color:#484f58}.row .v{color:#c8c8d0;font-weight:600}
.badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:11px;font-weight:700}
.badge-green{background:#00e67622;color:#00e676;border:1px solid #00e67644}
.badge-red{background:#f8514922;color:#f85149;border:1px solid #f8514944}
.badge-blue{background:#58a6ff22;color:#58a6ff;border:1px solid #58a6ff44}
.badge-yellow{background:#d29a0022;color:#e3b341;border:1px solid #d29a0044}
.badge-purple{background:#bc8cff22;color:#bc8cff;border:1px solid #bc8cff44}
.peer{background:#0d1117;border:1px solid #21262d;border-radius:6px;padding:10px 12px;margin:6px 0;display:flex;justify-content:space-between;align-items:center;font-size:12px}
.peer .id{color:#58a6ff;font-weight:600}.peer .ip{color:#484f58}
input,textarea,select{width:100%;padding:10px;background:#0d1117;border:1px solid #21262d;border-radius:6px;color:#c8c8d0;font-family:inherit;font-size:13px;margin-bottom:8px}
input:focus,textarea:focus{outline:none;border-color:#00e676}
textarea{min-height:160px;resize:vertical;line-height:1.5}
label{display:block;font-size:11px;color:#484f58;margin-bottom:4px;text-transform:uppercase;letter-spacing:0.5px}
.btn{display:inline-block;padding:10px 20px;border:none;border-radius:6px;font-family:inherit;font-size:13px;font-weight:600;cursor:pointer;transition:all .15s}
.btn-primary{background:#00e676;color:#0a0a0f}.btn-primary:hover{background:#00c853}
.btn-secondary{background:#21262d;color:#c8c8d0}.btn-secondary:hover{background:#30363d}
.btn-danger{background:#f8514933;color:#f85149;border:1px solid #f8514944}.btn-danger:hover{background:#f8514955}
.btn{width:100%;text-align:center;margin-top:8px}
.console-wrap{background:#0d1117;border:1px solid #21262d;border-radius:6px;overflow:hidden}
.console-input{display:flex;border-top:1px solid #21262d}
.console-input input{border:none;border-radius:0;margin:0;padding:10px 12px}
.console-input button{background:#00e676;color:#0a0a0f;border:none;padding:10px 16px;font-weight:700;cursor:pointer;font-family:inherit;white-space:nowrap}
.console-output{padding:12px;min-height:200px;max-height:400px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;font-size:12px;line-height:1.6;color:#8b949e}
.console-output .cmd-line{color:#00e676}
.net{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;margin:4px 0;background:#0d1117;border:1px solid #21262d;border-radius:6px;cursor:pointer;transition:border-color .15s}
.net:hover{border-color:#00e676}
.net .name{font-weight:600;font-size:13px}.net .info{font-size:11px;color:#484f58;margin-top:2px}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:16px}
.bars span{width:3px;background:#21262d;border-radius:1px}
.bars.s1 span:nth-child(1){background:#f85149}
.bars.s2 span:nth-child(1),.bars.s2 span:nth-child(2){background:#d29a00}
.bars.s3 span:nth-child(1),.bars.s3 span:nth-child(2),.bars.s3 span:nth-child(3){background:#00e676}
.bars.s4 span{background:#00e676}
.stat-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.stat-box{background:#0d1117;border:1px solid #21262d;border-radius:6px;padding:12px;text-align:center}
.stat-box .num{font-size:24px;font-weight:700;color:#00e676}
.stat-box .lbl{font-size:10px;color:#484f58;text-transform:uppercase;letter-spacing:1px;margin-top:4px}
.contract-item{background:#0d1117;border:1px solid #21262d;border-radius:6px;padding:12px;margin:6px 0}
.contract-item .cname{color:#bc8cff;font-weight:700;font-size:14px}
.contract-item .cmeta{font-size:11px;color:#484f58;margin-top:4px}
.loading{text-align:center;color:#484f58;padding:20px;font-size:13px}
.empty{text-align:center;color:#484f58;padding:32px;font-size:13px}
.quick-cmds{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:12px}
.quick-cmd{padding:6px 12px;background:#21262d;border:1px solid #30363d;border-radius:4px;color:#8b949e;font-size:11px;cursor:pointer;font-family:inherit;transition:all .15s}
.quick-cmd:hover{background:#30363d;color:#c8c8d0;border-color:#484f58}
.asm-help{font-size:11px;color:#484f58;line-height:1.6;margin-top:8px}
.asm-help code{background:#21262d;padding:1px 5px;border-radius:3px;color:#8b949e;font-size:11px}
@media(max-width:480px){.stat-grid{grid-template-columns:1fr 1fr}.tabs{gap:0}.tab{padding:10px 14px;font-size:12px}}
</style>
</head><body>

<div class='header'>
<h1>MirkoNet</h1>
<span class='ver'>v3.0 PoS</span>
</div>

<div class='tabs'>
<div class='tab active' onclick='showTab("dash")'>Dashboard</div>
<div class='tab' onclick='showTab("contracts")'>Contracts</div>
<div class='tab' onclick='showTab("console")'>Console</div>
<div class='tab' onclick='showTab("wifi")'>WiFi</div>
</div>

<!-- ============ DASHBOARD ============ -->
<div id='page-dash' class='page active'>
<div id='dash-status'></div>
<div class='stat-grid' id='dash-grid'></div>
<div class='card' style='margin-top:12px'>
<h3>Peers</h3>
<div id='dash-peers'><div class='loading'>Loading...</div></div>
</div>
</div>

<!-- ============ CONTRACTS ============ -->
<div id='page-contracts' class='page'>
<div class='card'>
<h3>Deploy Contract</h3>
<label>Contract Name</label>
<input type='text' id='deploy-name' placeholder='my_contract' maxlength='15'>
<label>MVM Assembly Code</label>
<textarea id='deploy-code' placeholder='PUSH 0
SLOAD
PUSH 1
ADD
DUP
PUSH 0
SWAP
SSTORE
EMIT
HALT'></textarea>
<button class='btn btn-primary' onclick='deployContract()'>Deploy Contract</button>
<div id='deploy-result'></div>
<div class='asm-help'>
<b>Instruction Set:</b> <code>PUSH n</code> <code>POP</code> <code>DUP</code> <code>SWAP</code>
<code>ADD</code> <code>SUB</code> <code>MUL</code> <code>DIV</code> <code>MOD</code>
<code>EQ</code> <code>GT</code> <code>LT</code> <code>NOT</code>
<code>AND</code> <code>OR</code> <code>XOR</code> <code>SHL</code> <code>SHR</code>
<code>JUMP @label</code> <code>JUMPI @label</code>
<code>SLOAD</code> <code>SSTORE</code> <code>EMIT</code>
<code>ARG n</code> <code>CALLER</code> <code>BALANCE</code> <code>CALLVALUE</code>
<code>HALT</code> <code>REVERT</code> <code>SHA3</code>
<code>MLOAD</code> <code>MSTORE</code> <code>CALL</code> <code>DELEGATECALL</code><br>
Labels: <code>name:</code> (line by itself). Max 256 bytes compiled.
</div>
</div>
<div class='card'>
<h3>Deployed Contracts</h3>
<div id='contract-list'><div class='loading'>Loading...</div></div>
<button class='btn btn-secondary' onclick='loadContracts()'>Refresh</button>
</div>
</div>

<!-- ============ CONSOLE ============ -->
<div id='page-console' class='page'>
<div class='card' style='padding:12px'>
<h3>Quick Commands</h3>
<div class='quick-cmds'>
<button class='quick-cmd' onclick='runCmd("status")'>status</button>
<button class='quick-cmd' onclick='runCmd("peers")'>peers</button>
<button class='quick-cmd' onclick='runCmd("chain")'>chain</button>
<button class='quick-cmd' onclick='runCmd("balance")'>balance</button>
<button class='quick-cmd' onclick='runCmd("accounts")'>accounts</button>
<button class='quick-cmd' onclick='runCmd("economy")'>economy</button>
<button class='quick-cmd' onclick='runCmd("validators")'>validators</button>
<button class='quick-cmd' onclick='runCmd("contracts")'>contracts</button>
<button class='quick-cmd' onclick='runCmd("faucet")'>faucet</button>
<button class='quick-cmd' onclick='runCmd("heap")'>heap</button>
<button class='quick-cmd' onclick='runCmd("debug")'>debug</button>
<button class='quick-cmd' onclick='runCmd("help")'>help</button>
</div>
</div>
<div class='console-wrap'>
<div class='console-output' id='console-out'>MirkoNet Console v3.0\nType a command or use quick buttons above.\n</div>
<div class='console-input'>
<input type='text' id='console-in' placeholder='Enter command...' onkeydown='if(event.key==="Enter")runConsole()'>
<button onclick='runConsole()'>Run</button>
</div>
</div>
</div>

<!-- ============ WIFI ============ -->
<div id='page-wifi' class='page'>
<div class='card' id='wifi-status'></div>
<div class='card'>
<h3>Available Networks</h3>
<div id='nets'><div class='loading'>Scanning...</div></div>
<button class='btn btn-secondary' onclick='scanWifi()'>Rescan Networks</button>
</div>
<div class='card'>
<h3>Connect to Network</h3>
<form action='/save' method='POST'>
<label>Network Name (SSID)</label>
<input type='text' name='s' id='ssid' required placeholder='Select above or type manually'>
<label>Password</label>
<input type='password' name='p' id='pass' placeholder='WiFi password'>
<button type='submit' class='btn btn-primary'>Connect & Reboot</button>
</form>
</div>
<div class='card'>
<h3>Reset</h3>
<p style='font-size:12px;color:#484f58;margin-bottom:8px'>Clear saved WiFi credentials and reboot into AP-only mode.</p>
<button class='btn btn-danger' onclick="if(confirm('Reset WiFi credentials?'))fetch('/api/console',{method:'POST',body:'wifireset'})">Reset WiFi</button>
</div>
</div>

<script>
let activeTab='dash';
function showTab(t){
  activeTab=t;
  document.querySelectorAll('.tab').forEach((e,i)=>e.classList.toggle('active',['dash','contracts','console','wifi'][i]===t));
  document.querySelectorAll('.page').forEach(e=>e.classList.remove('active'));
  document.getElementById('page-'+t).classList.add('active');
  if(t==='dash')loadDash();
  else if(t==='contracts')loadContracts();
  else if(t==='wifi')scanWifi();
}

// ---- Dashboard ----
function loadDash(){
  fetch('/api/stats').then(r=>r.json()).then(d=>{
    let st=d.staConnected?
      "<div class='card' style='border-color:#00e67644'><div class='row'><span class='k'>Status</span><span class='badge badge-green'>Online</span></div>"+
      "<div class='row'><span class='k'>Node ID</span><span class='v'>"+d.nodeId+"</span></div>"+
      "<div class='row'><span class='k'>Short ID</span><span class='v'>"+d.shortId+"</span></div>"+
      "<div class='row'><span class='k'>Role</span><span class='v'><span class='badge badge-blue'>"+d.role+"</span></span></div>"+
      "<div class='row'><span class='k'>IP</span><span class='v'>"+d.ip+"</span></div>"+
      "<div class='row'><span class='k'>WiFi</span><span class='v'>"+d.ssid+" ("+d.rssi+" dBm)</span></div></div>"
      :
      "<div class='card' style='border-color:#f8514944'><div class='row'><span class='k'>Status</span><span class='badge badge-red'>AP Only</span></div>"+
      "<div class='row'><span class='k'>Node ID</span><span class='v'>"+d.nodeId+"</span></div>"+
      "<div class='row'><span class='k'>Short ID</span><span class='v'>"+d.shortId+"</span></div>"+
      "<div class='row'><span class='k'>Role</span><span class='v'><span class='badge badge-blue'>"+d.role+"</span></span></div></div>";
    document.getElementById('dash-status').innerHTML=st;
    let g="<div class='stat-box'><div class='num'>"+d.height+"</div><div class='lbl'>Block Height</div></div>";
    g+="<div class='stat-box'><div class='num'>"+d.peers+"</div><div class='lbl'>Peers</div></div>";
    g+="<div class='stat-box'><div class='num'>"+d.balance+"</div><div class='lbl'>Balance</div></div>";
    g+="<div class='stat-box'><div class='num'>"+d.staked+"</div><div class='lbl'>Staked</div></div>";
    g+="<div class='stat-box'><div class='num'>"+d.validators+"</div><div class='lbl'>Validators</div></div>";
    g+="<div class='stat-box'><div class='num'>"+d.contracts+"</div><div class='lbl'>Contracts</div></div>";
    g+="<div class='stat-box'><div class='num'>"+d.epoch+"</div><div class='lbl'>Epoch</div></div>";
    g+="<div class='stat-box'><div class='num'>"+(d.heap/1024).toFixed(1)+"K</div><div class='lbl'>Free Heap</div></div>";
    document.getElementById('dash-grid').innerHTML=g;
  }).catch(()=>{document.getElementById('dash-status').innerHTML="<div class='card'><div class='loading'>Failed to load</div></div>";});

  fetch('/api/peers').then(r=>r.json()).then(d=>{
    if(d.length===0){document.getElementById('dash-peers').innerHTML="<div class='empty'>No peers connected</div>";return;}
    let h='';
    d.forEach(p=>{
      let badge=p.alive?"<span class='badge badge-green'>alive</span>":"<span class='badge badge-red'>lost</span>";
      h+="<div class='peer'><div><span class='id'>"+p.shortId+"</span> <span class='ip'>"+p.ip+"</span></div>";
      h+="<div>h:"+p.height+" <span class='badge badge-"+(p.role==='VALIDATOR'?'purple':p.role==='CANDIDATE'?'yellow':'blue')+"'>"+p.role+"</span> "+badge+"</div></div>";
    });
    document.getElementById('dash-peers').innerHTML=h;
  }).catch(()=>{});
}

// ---- Contracts ----
function loadContracts(){
  fetch('/api/contracts').then(r=>r.json()).then(d=>{
    if(d.length===0){document.getElementById('contract-list').innerHTML="<div class='empty'>No contracts deployed</div>";return;}
    let h='';
    d.forEach(c=>{
      h+="<div class='contract-item'><div class='cname'>"+c.name+"</div>";
      h+="<div class='cmeta'>Address: "+c.addr+" | "+c.codeLen+"B | by "+c.deployer+" | "+(c.hasCode?"LOCAL":"REMOTE")+"</div>";
      if(c.storage&&c.storage.length>0){
        h+="<div style='margin-top:6px;font-size:11px;color:#8b949e'>";
        c.storage.forEach(s=>{h+="["+s.key+"] = "+s.value+" ";});
        h+="</div>";
      }
      h+="</div>";
    });
    document.getElementById('contract-list').innerHTML=h;
  }).catch(()=>{document.getElementById('contract-list').innerHTML="<div class='loading'>Failed to load</div>";});
}

function deployContract(){
  let name=document.getElementById('deploy-name').value.trim();
  let code=document.getElementById('deploy-code').value.trim();
  let el=document.getElementById('deploy-result');
  if(!name||!code){el.innerHTML="<p style='color:#f85149;margin-top:8px;font-size:13px'>Name and code are required</p>";return;}
  el.innerHTML="<p style='color:#484f58;margin-top:8px;font-size:13px'>Deploying...</p>";
  fetch('/api/deploy',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'name='+encodeURIComponent(name)+'&code='+encodeURIComponent(code)})
  .then(r=>r.json()).then(d=>{
    if(d.ok){el.innerHTML="<p style='color:#00e676;margin-top:8px;font-size:13px'>"+d.message+"</p>";loadContracts();}
    else{el.innerHTML="<p style='color:#f85149;margin-top:8px;font-size:13px'>"+d.message+"</p>";}
  }).catch(()=>{el.innerHTML="<p style='color:#f85149;margin-top:8px;font-size:13px'>Deploy request failed</p>";});
}

// ---- Console ----
let cmdHistory=[],histIdx=-1;
document.addEventListener('DOMContentLoaded',()=>{
  let inp=document.getElementById('console-in');
  inp.addEventListener('keydown',e=>{
    if(e.key==='ArrowUp'&&cmdHistory.length>0){histIdx=Math.min(histIdx+1,cmdHistory.length-1);inp.value=cmdHistory[histIdx];}
    if(e.key==='ArrowDown'){histIdx=Math.max(histIdx-1,-1);inp.value=histIdx>=0?cmdHistory[histIdx]:'';}
  });
});

function runCmd(c){
  document.getElementById('console-in').value=c;
  runConsole();
}

function runConsole(){
  let inp=document.getElementById('console-in');
  let cmd=inp.value.trim();
  if(!cmd)return;
  cmdHistory.unshift(cmd);histIdx=-1;
  let out=document.getElementById('console-out');
  out.innerHTML+="<span class='cmd-line'>&gt; "+cmd+"</span>\n";
  inp.value='';
  fetch('/api/console',{method:'POST',body:cmd}).then(r=>r.json()).then(d=>{
    out.innerHTML+=d.output+"\n";
    out.scrollTop=out.scrollHeight;
  }).catch(()=>{
    out.innerHTML+="[error] Request failed\n";
    out.scrollTop=out.scrollHeight;
  });
}

// ---- WiFi ----
function scanWifi(){
  let el=document.getElementById('wifi-status');
  fetch('/status').then(r=>r.json()).then(d=>{
    if(d.sta){
      el.innerHTML="<h3>Connection</h3><div class='row'><span class='k'>Status</span><span class='badge badge-green'>Connected</span></div>"+
        "<div class='row'><span class='k'>Network</span><span class='v'>"+d.ssid+"</span></div>"+
        "<div class='row'><span class='k'>IP Address</span><span class='v'>"+d.ip+"</span></div>"+
        "<div class='row'><span class='k'>Signal</span><span class='v'>"+d.rssi+" dBm</span></div>";
    }else{
      el.innerHTML="<h3>Connection</h3><div class='row'><span class='k'>Status</span><span class='badge badge-red'>Not Connected</span></div>"+
        "<div class='row'><span class='k'>Info</span><span class='v'>Select a network below</span></div>";
    }
  }).catch(()=>{});

  document.getElementById('nets').innerHTML='<div class="loading">Scanning...</div>';
  fetch('/scan').then(r=>r.json()).then(d=>{
    let h='';
    if(d.length===0){h='<div class="empty">No networks found</div>';}
    d.forEach(n=>{
      let s=n.rssi>-50?4:n.rssi>-65?3:n.rssi>-80?2:1;
      h+="<div class='net' onclick=\"pick('"+n.ssid.replace(/'/g,"\\'")+"')\">";
      h+="<div><div class='name'>"+n.ssid+"</div>";
      h+="<div class='info'>"+(n.enc?"Secured":"Open")+" &middot; Ch "+n.ch+" &middot; "+n.rssi+" dBm</div></div>";
      h+="<div class='bars s"+s+"'><span style='height:4px'></span><span style='height:8px'></span><span style='height:12px'></span><span style='height:16px'></span></div>";
      h+="</div>";
    });
    document.getElementById('nets').innerHTML=h;
  }).catch(()=>{
    document.getElementById('nets').innerHTML='<div class="loading">Scan failed - try again</div>';
  });
}

function pick(s){document.getElementById('ssid').value=s;document.getElementById('pass').focus();}

// Auto-load dashboard on start
loadDash();
// Auto-refresh dashboard every 10s
setInterval(()=>{if(activeTab==='dash')loadDash();},10000);
</script>
</body></html>
)rawhtml";
        _server.send(200, "text/html", html);
    }

    void handleScan() {
        Serial.println("[WiFi] Web scan requested...");
        int n = WiFi.scanNetworks();
        Serial.printf("[WiFi] Found %d networks\n", n);
        String json = "[";
        for (int i = 0; i < n && i < 15; i++) {
            if (i > 0) json += ",";
            String ssid = WiFi.SSID(i);
            ssid.replace("\"", "\\\"");
            json += "{\"ssid\":\"" + ssid + "\",";
            json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
            json += "\"ch\":" + String(WiFi.channel()) + ",";
            json += "\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN) + "}";
        }
        json += "]";
        WiFi.scanDelete();
        _server.send(200, "application/json", json);
    }

    void handleSave() {
        String ssid = _server.arg("s");
        String pass = _server.arg("p");

        if (ssid.length() == 0) {
            _server.send(400, "text/plain", "SSID required");
            return;
        }

        Serial.printf("[WiFi] Saving: SSID='%s' pass_len=%d\n",
                      ssid.c_str(), pass.length());

        Preferences prefs;
        prefs.begin(WIFI_NVS_NAMESPACE, false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();

        String html = R"rawhtml(
<!DOCTYPE html>
<html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{font-family:'SF Mono',Consolas,monospace;background:#0a0a0f;color:#c8c8d0;display:flex;
justify-content:center;align-items:center;min-height:100vh;text-align:center}
.ok{font-size:48px;margin-bottom:16px;color:#00e676}
h2{color:#00e676}
</style>
</head><body>
<div>
<div class='ok'>&#10003;</div>
<h2>Saved!</h2>
<p>Connecting to: )rawhtml" + ssid + R"rawhtml(</p>
<p style='color:#484f58;margin-top:8px'>Rebooting in 3 seconds...</p>
</div>
</body></html>
)rawhtml";
        _server.send(200, "text/html", html);
        delay(3000);
        ESP.restart();
    }

    void handleWebStatus() {
        String json = "{";
        json += "\"sta\":" + String(staConnected ? "true" : "false") + ",";
        json += "\"ssid\":\"" + connectedSSID + "\",";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
        json += "\"uptime\":" + String(millis() / 1000);
        json += "}";
        _server.send(200, "application/json", json);
    }
};
