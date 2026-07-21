#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <esp_mac.h>
#include "config.h"

#define WIFI_CONNECT_TIMEOUT  15000
#define WIFI_NVS_NAMESPACE    "mirkonet"
#define AP_CHANNEL            1
#define DNS_PORT              53

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
    String   lastScanJson = "[]";
    bool     scanRunning = false;
    uint32_t scanStarted = 0;


    WebCmdCallback  onCommand   = nullptr;
    StatsCallback   onStats     = nullptr;
    StatsCallback   onBlocks    = nullptr;
    PeersCallback   onPeers     = nullptr;
    ContractsCallback onContracts = nullptr;
    DeployCallback  onDeploy    = nullptr;

    bool begin() {
        Serial.println("[WiFi] === WiFi Manager Starting ===");

        uint8_t mac[6];

        esp_efuse_mac_get_default(mac);
        char buf[32];
        snprintf(buf, sizeof(buf), "mnet_node_%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

        _server.on("/api/stats", HTTP_GET, [this]() { handleApiStats(); });
        _server.on("/api/blocks", HTTP_GET, [this]() { handleApiBlocks(); });
        _server.on("/api/peers", HTTP_GET, [this]() { handleApiPeers(); });
        _server.on("/api/contracts", HTTP_GET, [this]() { handleApiContracts(); });
        _server.on("/api/console", HTTP_POST, [this]() { handleApiConsole(); });
        _server.on("/api/deploy", HTTP_POST, [this]() { handleApiDeploy(); });
        _server.onNotFound([this]() { handleRoot(); });
        _server.begin();

        portalActive = true;
        Serial.println("[WiFi] Portal started on 192.168.4.1");
    }


    void handleApiStats() {
        if (onStats) {
            _server.send(200, "application/json", onStats());
        } else {
            _server.send(503, "application/json", "{\"error\":\"not ready\"}");
        }
    }

    void handleApiBlocks() {
        if (onBlocks) {
            _server.send(200, "application/json", onBlocks());
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


    void handleWifiSetupPage() {
        String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MirkoNet WiFi Setup</title>
<style>
*{box-sizing:border-box}
body{margin:0;min-height:100vh;background:#0f172a;color:#f1f5f9;font-family:Arial,sans-serif;display:flex;align-items:center;justify-content:center;padding:20px}
.wrap{width:100%;max-width:480px}
.brand{font-size:28px;font-weight:800;margin-bottom:8px}.brand span{color:#3b82f6}
.sub{color:#94a3b8;margin-bottom:20px;line-height:1.4}
.card{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:18px;margin-bottom:14px}
button{width:100%;border:0;border-radius:8px;padding:12px 14px;font-weight:700;cursor:pointer;background:#3b82f6;color:white}
button.secondary{background:transparent;border:1px solid #334155;color:#f1f5f9}
input{width:100%;background:#020617;border:1px solid #334155;color:white;padding:12px 14px;border-radius:8px;margin-bottom:10px;font-size:15px}
.net{display:flex;justify-content:space-between;align-items:center;background:#020617;border:1px solid #334155;border-radius:8px;padding:10px 12px;margin-top:8px;cursor:pointer}
.net:hover{border-color:#3b82f6}.rssi{color:#94a3b8;font-size:13px}.msg{color:#94a3b8;margin-top:10px;font-size:14px}
</style>
</head>
<body>
<div class="wrap">
  <div class="brand">Mirko<span>Net</span></div>
  <div class="sub">Connect this ESP32-S3 node to your 2.4 GHz WiFi network. The device will reboot after saving, then the blockchain dashboard will load.</div>
  <div class="card">
    <button class="secondary" onclick="scan()">Scan Networks</button>
    <div id="nets" class="msg">Tap scan, then choose your SSID.</div>
  </div>
  <div class="card">
    <input id="ssid" placeholder="SSID" autocomplete="off">
    <input id="pass" placeholder="Password" type="password">
    <button onclick="save()">Save WiFi & Reboot</button>
    <div id="msg" class="msg"></div>
  </div>
</div>
<script>
const el = id => document.getElementById(id);
async function scan(){
  el('nets').textContent = 'Scanning...';
  try {
    for (let i = 0; i < 12; i++) {
      const r = await fetch('/scan', {cache:'no-store'});
      const data = await r.json();
      const nets = Array.isArray(data) ? data : (data.networks || []);
      if (!data.scanning || nets.length) {
        el('nets').innerHTML = nets.map(n =>
          `<div class="net" onclick="pick('${String(n.ssid).replace(/'/g,"\\'")}')"><span>${n.ssid}</span><span class="rssi">${n.rssi} dBm</span></div>`
        ).join('') || '<div class="msg">No networks found. Try again.</div>';
        if (!data.scanning) return;
      }
      await new Promise(resolve => setTimeout(resolve, 700));
    }
    el('nets').innerHTML = '<div class="msg">Still scanning. Try again in a moment.</div>';
  } catch(e) {
    el('nets').textContent = 'Scan failed. Reconnect to the MirkoNet AP and retry.';
  }
}
function pick(s){ el('ssid').value = s; el('pass').focus(); }
async function save(){
  const ssid = el('ssid').value.trim();
  const pass = el('pass').value;
  if(!ssid){ el('msg').textContent = 'SSID is required.'; return; }
  el('msg').textContent = 'Saving credentials...';
  const r = await fetch('/save', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:`s=${encodeURIComponent(ssid)}&p=${encodeURIComponent(pass)}`
  });
  document.body.innerHTML = await r.text();
}
scan();
</script>
</body>
</html>
)rawhtml";
        _server.send(200, "text/html", html);
    }

    void handleRoot() {
    if (!staConnected) {
        handleWifiSetupPage();
        return;
    }
    String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>MirkoNet Explorer</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;700&family=JetBrains+Mono:wght@400;700&display=swap" rel="stylesheet">
<style>
:root {
    --bg-app: #0f172a;
    --bg-panel: #1e293b;
    --bg-input: #020617;
    --border: #334155;
    --primary: #3b82f6;
    --primary-dim: rgba(59, 130, 246, 0.1);
    --accent: #8b5cf6;
    --success: #10b981;
    --danger: #ef4444;
    --text-main: #f1f5f9;
    --text-sub: #94a3b8;
    --font-ui: 'Inter', sans-serif;
    --font-mono: 'JetBrains Mono', monospace;
    --sidebar-w: 240px;
    --nav-h: 60px;
}

* { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
body { margin: 0; background: var(--bg-app); color: var(--text-main); font-family: var(--font-ui); display: flex; height: 100vh; overflow: hidden; }

.sidebar { width: var(--sidebar-w); background: var(--bg-panel); border-right: 1px solid var(--border); display: flex; flex-direction: column; z-index: 50; }
.brand { padding: 1.5rem; font-size: 1.25rem; font-weight: 800; letter-spacing: -0.02em; display: flex; align-items: center; gap: 10px; color: white; border-bottom: 1px solid var(--border); }
.brand span { color: var(--primary); }
.nav { flex: 1; padding: 1rem; overflow-y: auto; display: flex; flex-direction: column; gap: 4px; }
.nav-item { padding: 12px; border-radius: 8px; color: var(--text-sub); cursor: pointer; font-weight: 500; font-size: 0.9rem; display: flex; align-items: center; gap: 12px; transition: 0.2s; }
.nav-item:hover { background: rgba(255,255,255,0.05); color: white; }
.nav-item.active { background: var(--primary); color: white; box-shadow: 0 4px 12px rgba(59,130,246,0.3); }

.mobile-nav { display: none; position: fixed; bottom: 0; left: 0; width: 100%; background: var(--bg-panel); border-top: 1px solid var(--border); height: var(--nav-h); justify-content: space-around; align-items: center; z-index: 100; padding-bottom: env(safe-area-inset-bottom); }
.mn-item { display: flex; flex-direction: column; align-items: center; justify-content: center; font-size: 0.7rem; color: var(--text-sub); gap: 4px; width: 100%; height: 100%; }
.mn-item.active { color: var(--primary); }
.mn-item svg { width: 20px; height: 20px; stroke-width: 2px; }

.main { flex: 1; display: flex; flex-direction: column; position: relative; overflow: hidden; }
.header { padding: 1rem 2rem; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; background: rgba(15, 23, 42, 0.8); backdrop-filter: blur(10px); }
.page-title { font-size: 1.1rem; font-weight: 700; }
.status-pill { font-size: 0.75rem; padding: 4px 10px; background: rgba(16,185,129,0.15); color: var(--success); border-radius: 99px; border: 1px solid rgba(16,185,129,0.3); font-weight: 600; display: flex; align-items: center; gap: 6px; }
.status-dot { width: 6px; height: 6px; background: currentColor; border-radius: 50%; box-shadow: 0 0 8px currentColor; }

.content { flex: 1; overflow-y: auto; padding: 1.5rem; scroll-behavior: smooth; }
.page { display: none; animation: fadeIn 0.3s ease; max-width: 1200px; margin: 0 auto; }
.page.active { display: block; }
@keyframes fadeIn { from { opacity: 0; transform: translateY(5px); } to { opacity: 1; transform: translateY(0); } }

.grid-2 { display: grid; grid-template-columns: repeat(auto-fit, minmax(350px, 1fr)); gap: 1.5rem; }
.grid-4 { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap: 1rem; }

.card { background: var(--bg-panel); border: 1px solid var(--border); border-radius: 12px; padding: 1.25rem; position: relative; overflow: hidden; }
.card-head { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem; }
.card-title { font-size: 0.85rem; font-weight: 700; text-transform: uppercase; letter-spacing: 0.05em; color: var(--text-sub); }
.metric-val { font-size: 1.75rem; font-weight: 700; font-family: var(--font-mono); color: white; margin-top: 5px; }

.table-wrap { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; font-size: 0.9rem; }
th { text-align: left; padding: 12px; color: var(--text-sub); border-bottom: 1px solid var(--border); font-weight: 600; font-size: 0.75rem; text-transform: uppercase; }
td { padding: 12px; border-bottom: 1px solid rgba(255,255,255,0.05); font-family: var(--font-mono); color: var(--text-main); }
tr:last-child td { border-bottom: none; }
.badge { padding: 3px 8px; border-radius: 4px; font-size: 0.7rem; font-weight: 700; text-transform: uppercase; }
.bg-blue { background: rgba(59, 130, 246, 0.2); color: #60a5fa; }
.bg-green { background: rgba(16, 185, 129, 0.2); color: #34d399; }

.btn { display: inline-flex; align-items: center; justify-content: center; padding: 10px 16px; border-radius: 8px; font-weight: 600; cursor: pointer; border: none; font-size: 0.9rem; transition: 0.2s; gap: 8px; }
.btn-primary { background: var(--primary); color: white; }
.btn-primary:hover { background: #2563eb; }
.btn-outline { background: transparent; border: 1px solid var(--border); color: var(--text-main); }
.btn-sm { padding: 6px 12px; font-size: 0.8rem; }

input, textarea { width: 100%; background: var(--bg-input); border: 1px solid var(--border); color: white; padding: 10px 14px; border-radius: 8px; font-family: var(--font-mono); font-size: 0.9rem; margin-bottom: 10px; outline: none; }
input:focus { border-color: var(--primary); }

.graph-container { width: 100%; height: 200px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 8px; position: relative; overflow: hidden; }
svg { width: 100%; height: 100%; }
.line-path { fill: none; stroke: var(--primary); stroke-width: 2; vector-effect: non-scaling-stroke; }
.area-path { fill: var(--primary-dim); stroke: none; }

.term-wrap { background: #000; border-radius: 8px; border: 1px solid var(--border); height: 600px; display: flex; flex-direction: column; font-family: var(--font-mono); font-size: 0.85rem; }
.term-out { flex: 1; padding: 1rem; overflow-y: auto; white-space: pre-wrap; color: #cbd5e1; }
.term-in { display: flex; border-top: 1px solid var(--border); padding: 5px; }
.term-input { border: none; background: transparent; margin: 0; }

@media (max-width: 768px) {
    .sidebar { display: none; }
    .mobile-nav { display: flex; }
    .main { padding-bottom: var(--nav-h); }
    .header { padding: 1rem; }
    .content { padding: 1rem; }
    .grid-2 { grid-template-columns: 1fr; }
}
</style>
</head>
<body>

<div class="sidebar">
    <div class="brand">Mirko<span>Net</span></div>
    <div class="nav">
        <div class="nav-item active" onclick="nav('dash')">Dashboard</div>
        <div class="nav-item" onclick="nav('wallet')">Wallet</div>
        <div class="nav-item" onclick="nav('chain')">Chain & Mempool</div>
        <div class="nav-item" onclick="nav('economy')">Economy</div>
        <div class="nav-item" onclick="nav('metrics')">Metrics & Graph</div>
        <div class="nav-item" onclick="nav('contracts')">Contracts</div>
        <div class="nav-item" onclick="nav('wifi')">WiFi Setup</div>
        <div class="nav-item" onclick="nav('console')">Console</div>
    </div>
</div>

<div class="main">
    <div class="header">
        <div class="page-title" id="header-title">Dashboard</div>
        <div class="status-pill"><div class="status-dot"></div> MAINNET</div>
    </div>

    <div class="content">

        <div id="page-dash" class="page active">
            <div class="grid-4">
                <div class="card">
                    <div class="card-title">Block Height</div>
                    <div class="metric-val" id="d-height">0</div>
                </div>
                <div class="card">
                    <div class="card-title">Peers</div>
                    <div class="metric-val" id="d-peers">0</div>
                </div>
                <div class="card">
                    <div class="card-title">Role</div>
                    <div class="metric-val" id="d-role" style="font-size:1.2rem">---</div>
                </div>
                <div class="card">
                    <div class="card-title">Uptime</div>
                    <div class="metric-val" id="d-uptime" style="font-size:1.2rem">0s</div>
                </div>
            </div>

            <div class="grid-2" style="margin-top:1.5rem">
                <div class="card">
                    <div class="card-head">
                        <div class="card-title">Connected Peers</div>
                        <button class="btn btn-outline btn-sm" onclick="fetchPeers()">Refresh</button>
                    </div>
                    <div class="table-wrap">
                        <table id="peer-table">
                            <thead><tr><th>ID</th><th>IP</th><th>H</th><th>Role</th></tr></thead>
                            <tbody><tr><td colspan="4">Loading...</td></tr></tbody>
                        </table>
                    </div>
                </div>
                <div class="card">
                    <div class="card-title">Node Information</div>
                    <div style="margin-top:1rem; display:flex; flex-direction:column; gap:12px; font-family:var(--font-mono); font-size:0.9rem">
                        <div style="display:flex; justify-content:space-between"><span>Node ID:</span> <span id="d-id">---</span></div>
                        <div style="display:flex; justify-content:space-between"><span>IP Addr:</span> <span id="d-ip">---</span></div>
                        <div style="display:flex; justify-content:space-between"><span>WiFi AP:</span> <span id="d-wifi">MirkoNet</span></div>
                        <div style="display:flex; justify-content:space-between"><span>Heap:</span> <span id="d-heap">---</span></div>
                    </div>
                </div>
            </div>
        </div>

        <div id="page-wallet" class="page">
            <div class="card" style="background:linear-gradient(135deg, #1e40af, #3b82f6); border:none; text-align:center; padding:2rem; margin-bottom:1.5rem">
                <div style="color:rgba(255,255,255,0.7); font-size:0.8rem; font-weight:700; letter-spacing:1px">AVAILABLE BALANCE</div>
                <div style="font-size:3rem; font-weight:800; color:white; font-family:var(--font-mono); margin:10px 0" id="w-bal">0.00</div>
                <div style="color:rgba(255,255,255,0.8); font-size:0.9rem">Staked: <span id="w-staked">0</span> | Unstaking: <span id="w-unst">0</span></div>
            </div>

            <div class="grid-2">
                <div class="card">
                    <div class="card-title">Transfer</div>
                    <div style="margin-top:1rem">
                        <input type="text" id="tx-to" placeholder="Recipient Short ID (e.g. A1B2C3)">
                        <input type="number" id="tx-amt" placeholder="Amount">
                        <button class="btn btn-primary" style="width:100%" onclick="doTx()">Sign & Send</button>
                    </div>
                </div>
                <div class="card">
                    <div class="card-title">Staking Operations</div>
                    <div style="margin-top:1rem">
                        <input type="number" id="stake-amt" placeholder="Amount">
                        <div style="display:flex; gap:10px">
                            <button class="btn btn-primary" style="flex:1" onclick="cmd('stake '+get('stake-amt').value)">Stake</button>
                            <button class="btn btn-outline" style="flex:1" onclick="cmd('unstake '+get('stake-amt').value)">Unstake</button>
                        </div>
                        <div style="margin-top:10px; padding-top:10px; border-top:1px solid var(--border)">
                            <button class="btn btn-outline" style="width:100%" onclick="cmd('claim')">Claim Rewards</button>
                        </div>
                        <div style="margin-top:10px">
                            <button class="btn btn-outline" style="width:100%; border-style:dashed" onclick="cmd('faucet')">Request Faucet</button>
                        </div>
                    </div>
                </div>
            </div>
        </div>

        <div id="page-chain" class="page">
            <div class="grid-2">
                <div class="card">
                    <div class="card-head"><div class="card-title">Mempool Status</div> <span class="badge bg-blue" id="c-mempool">0 Pending</span></div>
                    <div style="color:var(--text-sub); font-size:0.9rem">Transactions waiting to be included in the next block.</div>
                </div>
                <div class="card">
                    <div class="card-head"><div class="card-title">Chain Stats</div></div>
                     <div style="display:flex; justify-content:space-between; font-size:0.9rem; margin-bottom:5px"><span>Epoch:</span> <span class="mono" id="c-epoch">0</span></div>
                     <div style="display:flex; justify-content:space-between; font-size:0.9rem"><span>Slot:</span> <span class="mono" id="c-slot">0</span></div>
                </div>
            </div>
            <br>
            <div class="card">
                <div class="card-head">
                    <div class="card-title">Recent Blocks</div>
                    <button class="btn btn-outline btn-sm" onclick="fetchChain()">Refresh</button>
                </div>
                <div class="table-wrap">
                    <table id="chain-table">
                        <thead><tr><th>Height</th><th>Txns</th><th>Validator</th><th>Reward</th></tr></thead>
                        <tbody></tbody>
                    </table>
                </div>
            </div>
        </div>

        <div id="page-economy" class="page">
            <div class="grid-4">
                <div class="card">
                    <div class="card-title">Total Supply</div>
                    <div class="metric-val" id="e-supply" style="font-size:1.4rem">---</div>
                </div>
                <div class="card">
                    <div class="card-title">Total Staked</div>
                    <div class="metric-val" id="e-staked" style="font-size:1.4rem">---</div>
                </div>
                <div class="card">
                    <div class="card-title">Block Reward</div>
                    <div class="metric-val" id="e-reward" style="font-size:1.4rem">---</div>
                </div>
                <div class="card">
                    <div class="card-title">Accounts</div>
                    <div class="metric-val" id="e-accounts" style="font-size:1.4rem">---</div>
                </div>
            </div>
            <br>
            <div class="card">
                <div class="card-title">Economics Overview</div>
                <p style="color:var(--text-sub); margin-top:10px">
                    The network operates on a Proof-of-Stake consensus. Staked tokens secure the network.
                    <br><br>
                    <b>Inflation:</b> Rewards are minted per block.
                    <br>
                    <b>Staking Ratio:</b> <span id="e-ratio" style="color:var(--success); font-weight:bold">0%</span> of supply is locked.
                </p>
            </div>
        </div>

        <div id="page-metrics" class="page">
            <div class="card">
                <div class="card-title">Heap Memory (Free Bytes)</div>
                <div class="graph-container">
                    <svg viewBox="0 0 100 100" preserveAspectRatio="none">
                        <path id="graph-area" class="area-path" d="M0,100 L100,100 L100,100 L0,100Z" />
                        <path id="graph-line" class="line-path" d="M0,100 L100,100" />
                    </svg>
                </div>
                <div style="display:flex; justify-content:space-between; margin-top:5px; font-size:0.8rem; color:var(--text-sub)">
                    <span>60s ago</span>
                    <span>Now</span>
                </div>
            </div>
        </div>

        <div id="page-contracts" class="page">
            <div class="grid-2">
                <div class="card">
                    <div class="card-title">Deploy Contract</div>
                    <div style="margin-top:1rem">
                        <input type="text" id="c-name" placeholder="Name">
                        <textarea id="c-code" placeholder="MVM Assembly..." style="height:120px"></textarea>
                        <button class="btn btn-primary" style="width:100%" onclick="deploy()">Deploy</button>
                    </div>
                </div>
                <div class="card">
                    <div class="card-head">
                        <div class="card-title">Deployed Contracts</div>
                        <button class="btn btn-outline btn-sm" onclick="fetchContracts()">Fetch</button>
                    </div>
                    <div id="contract-list"></div>
                </div>
            </div>
        </div>

        <div id="page-wifi" class="page">
            <div class="grid-2">
                <div class="card">
                    <div class="card-head">
                        <div class="card-title">Networks</div>
                        <button class="btn btn-outline btn-sm" onclick="scanWifi()">Scan</button>
                    </div>
                    <div id="wifi-list" style="display:flex; flex-direction:column; gap:8px"></div>
                </div>
                <div class="card">
                    <div class="card-title">Station Credentials</div>
                    <div style="margin-top:1rem">
                        <input type="text" id="wifi-ssid" placeholder="SSID">
                        <input type="password" id="wifi-pass" placeholder="Password">
                        <button class="btn btn-primary" style="width:100%" onclick="saveWifi()">Save & Reboot</button>
                    </div>
                </div>
            </div>
        </div>

        <div id="page-console" class="page">
            <div class="term-wrap">
                <div class="term-out" id="term-out">MirkoNet NodeOS v3.0 [Connected]</div>
                <div class="term-in">
                    <span style="color:var(--success); margin-right:8px">➜</span>
                    <input type="text" class="term-input" id="term-in" style="flex:1" autocomplete="off">
                    <button class="btn btn-primary btn-sm" onclick="runTerm()">Send</button>
                </div>
            </div>
        </div>

    </div>
</div>

<div class="mobile-nav">
    <div class="mn-item active" onclick="nav('dash')"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor"><rect x="3" y="3" width="7" height="7"></rect><rect x="14" y="3" width="7" height="7"></rect><rect x="14" y="14" width="7" height="7"></rect><rect x="3" y="14" width="7" height="7"></rect></svg>Dash</div>
    <div class="mn-item" onclick="nav('wallet')"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor"><path d="M20 12V8H6a2 2 0 0 1-2-2c0-1.1.9-2 2-2h12v4"></path><path d="M4 6v12c0 1.1.9 2 2 2h14v-4"></path><path d="M18 12a2 2 0 0 0-2 2c0 1.1.9 2 2 2h4v-4h-4z"></path></svg>Wallet</div>
    <div class="mn-item" onclick="nav('chain')"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor"><line x1="8" y1="6" x2="21" y2="6"></line><line x1="8" y1="12" x2="21" y2="12"></line><line x1="8" y1="18" x2="21" y2="18"></line></svg>Chain</div>
    <div class="mn-item" onclick="nav('contracts')"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"></path></svg>Code</div>
    <div class="mn-item" onclick="nav('console')"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor"><polyline points="4 17 10 11 4 5"></polyline><line x1="12" y1="19" x2="20" y2="19"></line></svg>Term</div>
</div>

<script>
const get = (id) => document.getElementById(id);
const parse = (text, key) => { const m = text.match(new RegExp(key + ":\\s+(.+)", "i")); return m ? m[1].trim() : "---"; };

function nav(id) {
    document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
    get('page-'+id).classList.add('active');

    document.querySelectorAll('.nav-item').forEach(e => e.classList.toggle('active', e.innerText.toLowerCase().includes(id)));
    document.querySelectorAll('.mn-item').forEach(e => {
        let map = { 'dash':'dash', 'wallet':'wallet', 'chain':'chain', 'contracts':'contracts', 'console':'console', 'economy':'dash', 'metrics':'dash' };
    });


    get('header-title').innerText = id.charAt(0).toUpperCase() + id.slice(1);


    if(id==='dash') { fetchStatus(); fetchPeers(); }
    if(id==='wallet') fetchStatus();
    if(id==='chain') fetchChain();
    if(id==='contracts') fetchContracts();
    if(id==='wifi') scanWifi();
    if(id==='economy') fetchEconomy();
}


async function cmd(c, silent=false) {
    try {
        const r = await fetch('/api/console', { method: 'POST', body: c });
        const d = await r.json();
        if(!silent) {
            const t = get('term-out');
            t.innerText += `\n➜ ${c}\n${d.output}`;
            t.scrollTop = t.scrollHeight;
        }
        return d.output;
    } catch(e) { return ""; }
}


async function fetchStatus() {
    const raw = await cmd('status', true);
    if(!raw) return;


    get('d-height').innerText = parse(raw, "Chain").split(" ")[0];
    get('d-peers').innerText = parse(raw, "Peers");
    get('d-role').innerText = parse(raw, "Role");
    get('d-id').innerText = parse(raw, "Short ID");
    get('d-ip').innerText = parse(raw, "IP");
    get('d-heap').innerText = parse(raw, "Heap");


    get('w-bal').innerText = parseFloat(parse(raw, "Balance")).toFixed(2);
    get('w-staked').innerText = parse(raw, "Staked");


    const dbg = await cmd('debug', true);
    get('d-uptime').innerText = parse(dbg, "Uptime") + "s";


    const hVal = parseInt(parse(raw, "Heap"));
    pushGraph(hVal);
}

async function fetchPeers() {
    const raw = await cmd('peers', true);
    const rows = raw.split('\n').filter(l => l.includes('@')).map(l => {
        const p = l.trim().split(/\s+/);
        return `<tr>
            <td class="mono">${p[0]}</td>
            <td>${p[2]}</td>
            <td>${p[3].split('=')[1]||'?'}</td>
            <td><span class="badge bg-blue">${p[4]}</span></td>
        </tr>`;
    }).join('');
    get('peer-table').querySelector('tbody').innerHTML = rows || '<tr><td colspan="4">No peers connected</td></tr>';
}

async function fetchChain() {

    try {
        const r = await fetch('/api/blocks');
        const blocks = await r.json();
        const rows = blocks.slice(-10).reverse().map(b => `<tr>
            <td style="color:var(--primary)">#${b.height}</td>
            <td>${b.txCount}</td>
            <td>${b.validator}</td>
            <td>${b.reward}</td>
        </tr>`).join('');
        get('chain-table').querySelector('tbody').innerHTML = rows || '<tr><td colspan="4">Chain is empty</td></tr>';
    } catch(e) {
        get('chain-table').querySelector('tbody').innerHTML = '<tr><td colspan="4">Error loading chain</td></tr>';
    }


    const s = await cmd('status', true);
    get('c-mempool').innerText = parse(s, "Mempool") + " Pending";
    get('c-epoch').innerText = parse(s, "Epoch");
    get('c-slot').innerText = parse(s, "Slot");
}

async function fetchEconomy() {
    const raw = await cmd('economy', true);
    get('e-supply').innerText = parse(raw, "Total supply");
    get('e-staked').innerText = parse(raw, "Total staked").split(" ")[0];
    get('e-reward').innerText = parse(raw, "Block reward");
    get('e-accounts').innerText = parse(raw, "Accounts");


    const staked = parseInt(parse(raw, "Total staked"));
    const supply = parseInt(parse(raw, "Total supply"));
    if(supply > 0) get('e-ratio').innerText = ((staked/supply)*100).toFixed(1) + "%";
}

async function fetchContracts() {
    const raw = await cmd('contracts', true);
    const html = raw.split('\n').filter(l => l.includes('|')).map(l => {
        const parts = l.split('|');
        return `<div class="card" style="margin-bottom:10px; padding:10px">
            <div style="font-weight:700">${parts[0].split('@')[0]}</div>
            <div style="font-size:0.8rem; color:var(--text-sub); font-family:var(--font-mono)">${parts[0].split('@')[1]}</div>
            <div style="font-size:0.8rem; margin-top:5px"><span class="badge bg-green">${parts[3]}</span> Size: ${parts[2]}</div>
        </div>`;
    }).join('');
    get('contract-list').innerHTML = html || "No contracts.";
}

async function scanWifi() {
    const list = get('wifi-list');
    list.innerHTML = '<div style="color:var(--text-sub)">Scanning...</div>';
    try {
        for (let i = 0; i < 12; i++) {
            const r = await fetch('/scan', {cache:'no-store'});
            const data = await r.json();
            const nets = Array.isArray(data) ? data : (data.networks || []);
            if (!data.scanning || nets.length) {
                list.innerHTML = nets.map(n => `
                    <button class="btn btn-outline" style="justify-content:space-between" onclick="get('wifi-ssid').value='${String(n.ssid).replace(/'/g, "\\'")}'">
                        <span>${n.ssid}</span><span>${n.rssi} dBm</span>
                    </button>
                `).join('') || '<div style="color:var(--text-sub)">No networks found</div>';
                if (!data.scanning) return;
            }
            await new Promise(resolve => setTimeout(resolve, 700));
        }
        list.innerHTML = '<div style="color:var(--text-sub)">Still scanning. Try again in a moment.</div>';
    } catch(e) {
        list.innerHTML = '<div style="color:var(--danger)">Scan failed</div>';
    }
}

async function saveWifi() {
    const ssid = get('wifi-ssid').value;
    const pass = get('wifi-pass').value;
    if(!ssid) return alert('SSID required');
    const r = await fetch('/save', {
        method:'POST',
        headers:{'Content-Type':'application/x-www-form-urlencoded'},
        body:`s=${encodeURIComponent(ssid)}&p=${encodeURIComponent(pass)}`
    });
    document.body.innerHTML = await r.text();
}

let graphData = new Array(30).fill(0);
function pushGraph(val) {
    graphData.shift();
    graphData.push(val);
    drawGraph();
}
function drawGraph() {
    const min = Math.min(...graphData) * 0.9;
    const max = Math.max(...graphData) * 1.1;
    const range = max - min || 1;

    const points = graphData.map((v, i) => {
        const x = (i / (graphData.length - 1)) * 100;
        const y = 100 - ((v - min) / range) * 100;
        return `${x},${y}`;
    }).join(' ');

    get('graph-line').setAttribute('d', `M${points}`);
    get('graph-area').setAttribute('d', `M0,100 ${points} L100,100 Z`);
}

function doTx() {
    const to = get('tx-to').value;
    const amt = get('tx-amt').value;
    if(to && amt) { cmd(`transfer ${to} ${amt}`); alert('Sent!'); }
}

function deploy() {
    const n = get('c-name').value;
    const c = get('c-code').value;
    if(n && c) {
        fetch('/api/deploy', {
            method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
            body:`name=${encodeURIComponent(n)}&code=${encodeURIComponent(c)}`
        }).then(r=>r.json()).then(d=> { alert('Deployed'); fetchContracts(); });
    }
}

function runTerm() {
    const i = get('term-in');
    if(i.value) { cmd(i.value); i.value = ''; }
}
get('term-in').addEventListener('keydown', e=>{if(e.key==='Enter')runTerm()});

fetchStatus();
setInterval(fetchStatus, 4000);

</script>
</body>
</html>
)rawhtml";
    _server.send(200, "text/html", html);
}
    String scanResultsJson(int n) {
        String json = "[";
        bool first = true;
        for (int i = 0; i < n && i < 15; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;
            if (!first) json += ",";
            first = false;
            ssid.replace("\\", "\\\\");
            ssid.replace("\"", "\\\"");
            json += "{\"ssid\":\"" + ssid + "\",";
            json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
            json += "\"ch\":" + String(WiFi.channel(i)) + ",";
            json += "\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN) + "}";
        }
        json += "]";
        return json;
    }

    int beginAsyncWifiScan() {
        WiFi.scanDelete();
        return WiFi.scanNetworks(true, true, false, 180);
    }

    void startWifiScan() {
        WiFi.mode(WIFI_AP_STA);
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.disconnect(false, false, 250);
        }
        delay(120);

        int r = beginAsyncWifiScan();
        if (r == WIFI_SCAN_FAILED && WiFi.status() != WL_CONNECTED) {
            Serial.printf("[WiFi] Async scan start failed with STA status %d; retrying\n",
                          WiFi.status());
            WiFi.disconnect(false, false, 250);
            delay(200);
            r = beginAsyncWifiScan();
        }

        scanRunning = (r == WIFI_SCAN_RUNNING || r == -1);
        if (scanRunning) scanStarted = millis();
        Serial.printf("[WiFi] Async scan start result: %d\n", r);

        if (r >= 0) {
            lastScanJson = scanResultsJson(r);
            WiFi.scanDelete();
            scanRunning = false;
            Serial.printf("[WiFi] Found %d networks immediately\n", r);
        } else if (r == WIFI_SCAN_FAILED) {
            lastScanJson = "[]";
            WiFi.scanDelete();
            scanRunning = false;
        }
    }

    void handleScan() {
        Serial.println("[WiFi] Web scan requested...");
        _server.sendHeader("Cache-Control", "no-store");

        int complete = WiFi.scanComplete();
        if (scanRunning && complete >= 0) {
            lastScanJson = scanResultsJson(complete);
            Serial.printf("[WiFi] Async scan complete: %d networks\n", complete);
            WiFi.scanDelete();
            scanRunning = false;
        } else if (scanRunning && complete < -1) {
            Serial.printf("[WiFi] Async scan failed: %d\n", complete);
            WiFi.scanDelete();
            scanRunning = false;
        } else if (scanRunning && millis() - scanStarted > 12000) {
            Serial.println("[WiFi] Async scan timed out");
            WiFi.scanDelete();
            scanRunning = false;
        }

        if (!scanRunning) startWifiScan();

        if (scanRunning) {
            _server.send(200, "application/json",
                         "{\"scanning\":true,\"networks\":" + lastScanJson + "}");
            return;
        }

        WiFi.scanDelete();
        _server.send(200, "application/json",
                     "{\"scanning\":false,\"networks\":" + lastScanJson + "}");
    }

    void handleSave() {
        String ssid = _server.arg("s");
        String pass = _server.arg("p");

        if (ssid.length() == 0) {
            _server.send(400, "text/plain", "SSID required");
            return;
        }

        Serial.printf("[WiFi] Saving: SSID='%s' pass_len=%d\n", ssid.c_str(), pass.length());

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
