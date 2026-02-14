#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include "config.h"

// MirkoNet WiFi Manager
// - implement statistics and insight into the network
// - implement upload the smart contract
// - each user can have one or more esp32 devices
// - each user has exactly one node

#define WIFI_CONNECT_TIMEOUT  15000
#define WIFI_NVS_NAMESPACE    "mirkonet"
#define AP_CHANNEL            1
#define DNS_PORT              53

class WiFiManager {
public:
    bool     staConnected = false;
    String   apName;
    String   connectedSSID;
    bool     portalActive = false;

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
            Serial.println("[WiFi] No saved credentials — AP-only mode");
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
        _server.onNotFound([this]() { handleRoot(); });
        _server.begin();

        portalActive = true;
        Serial.println("[WiFi] Portal started on 192.168.4.1");
    }

    void handleRoot() {
        String staInfo = "";
        if (staConnected) {
            staInfo = "<div class='card' style='border-color:#00e676'>"
                      "<b style='color:#00e676'>Connected</b>"
                      "<p>Network: " + connectedSSID + "</p>"
                      "<p>IP: " + WiFi.localIP().toString() + "</p>"
                      "<p>Signal: " + String(WiFi.RSSI()) + " dBm</p>"
                      "</div>";
        } else {
            staInfo = "<div class='card' style='border-color:#ff5252'>"
                      "<b style='color:#ff5252'>Not Connected</b>"
                      "<p>Select a network below to connect</p>"
                      "</div>";
        }

        String html = R"rawhtml(
<!DOCTYPE html>
<html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>MirkoNet Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#0a0a0a;color:#e0e0e0;padding:20px;max-width:420px;margin:0 auto}
h1{color:#00e676;margin-bottom:4px;font-size:24px}
.sub{color:#888;margin-bottom:24px;font-size:14px}
.card{background:#1a1a1a;border:1px solid #333;border-radius:12px;padding:20px;margin-bottom:16px}
.net{display:flex;justify-content:space-between;align-items:center;padding:12px;margin:4px 0;background:#222;border-radius:8px;cursor:pointer;border:1px solid transparent}
.net:hover{border-color:#00e676}
.net .name{font-weight:600}
.net .info{font-size:12px;color:#888}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:16px}
.bars span{width:3px;background:#555;border-radius:1px}
.bars.s1 span:nth-child(1){background:#00e676}
.bars.s2 span:nth-child(1),.bars.s2 span:nth-child(2){background:#00e676}
.bars.s3 span:nth-child(1),.bars.s3 span:nth-child(2),.bars.s3 span:nth-child(3){background:#00e676}
.bars.s4 span{background:#00e676}
label{display:block;font-size:13px;color:#aaa;margin-bottom:4px;margin-top:12px}
input[type=text],input[type=password]{width:100%;padding:10px;background:#111;border:1px solid #444;border-radius:8px;color:#fff;font-size:16px}
input:focus{outline:none;border-color:#00e676}
button{width:100%;padding:12px;background:#00e676;color:#000;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer;margin-top:16px}
button:hover{background:#00c853}
.scan-btn{background:#333;color:#e0e0e0;font-size:14px;padding:8px;margin-top:8px}
.scan-btn:hover{background:#444}
#nets{min-height:40px}
.loading{text-align:center;color:#888;padding:20px}
</style>
</head><body>
<h1>MirkoNet</h1>
<p class='sub'>Blockchain Node</p>
)rawhtml" + staInfo + R"rawhtml(
<div class='card'>
<b>Available Networks</b>
<div id='nets'><div class='loading'>Scanning...</div></div>
<button class='scan-btn' onclick='scan()'>Rescan</button>
</div>
<div class='card'>
<form action='/save' method='POST'>
<label>Network Name (SSID)</label>
<input type='text' name='s' id='ssid' required placeholder='Select above or type'>
<label>Password</label>
<input type='password' name='p' id='pass' placeholder='WiFi password'>
<button type='submit'>Connect & Reboot</button>
</form>
</div>
<script>
function scan(){
  document.getElementById('nets').innerHTML='<div class="loading">Scanning...</div>';
  fetch('/scan').then(r=>r.json()).then(d=>{
    let h='';
    if(d.length==0){h='<div class="loading">No networks found</div>';}
    d.forEach(n=>{
      let s=n.rssi>-50?4:n.rssi>-65?3:n.rssi>-80?2:1;
      h+='<div class="net" onclick="pick(\''+n.ssid.replace(/'/g,"\\'")+'\')">';
      h+='<div><div class="name">'+n.ssid+'</div>';
      h+='<div class="info">'+(n.enc?'Secured':'Open')+' &middot; Ch '+n.ch+'</div></div>';
      h+='<div class="bars s'+s+'"><span style="height:4px"></span><span style="height:8px"></span><span style="height:12px"></span><span style="height:16px"></span></div>';
      h+='</div>';
    });
    document.getElementById('nets').innerHTML=h;
  }).catch(()=>{
    document.getElementById('nets').innerHTML='<div class="loading">Scan failed</div>';
  });
}
function pick(s){document.getElementById('ssid').value=s;document.getElementById('pass').focus();}
scan();
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
body{font-family:system-ui;background:#0a0a0a;color:#e0e0e0;display:flex;
justify-content:center;align-items:center;min-height:100vh;text-align:center}
.ok{font-size:48px;margin-bottom:16px}
h2{color:#00e676}
</style>
</head><body>
<div>
<div class='ok'>✓</div>
<h2>Saved!</h2>
<p>Connecting to: )rawhtml" + ssid + R"rawhtml(</p>
<p style='color:#888;margin-top:8px'>Rebooting in 3 seconds...</p>
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
