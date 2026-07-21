#pragma once
#include <Arduino.h>
#include "config.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
    #define HAS_RGB_LED 1

    #if defined(RGB_BUILTIN)
        #define RGB_LED_PIN RGB_BUILTIN
    #else
        #define RGB_LED_PIN 48
    #endif
#else
    #define HAS_RGB_LED 0
    #define BUILTIN_LED_PIN 2
#endif

enum LedState : uint8_t {
    LED_OFF         = 0,
    LED_NO_WIFI     = 1,
    LED_NO_PEERS    = 2,
    LED_CONNECTED   = 3,
    LED_VALIDATOR   = 4,
    LED_PRODUCING   = 5,
    LED_SYNCING     = 6,
    LED_TX_FLASH    = 7,
    LED_BOOT        = 8,
    LED_ERROR       = 9,
};

class StatusLED {
public:
    void init() {
#if HAS_RGB_LED
        pinMode(RGB_LED_PIN, OUTPUT);
        _useRGB = true;
        setRGB(0, 0, 0);
        Serial.println("[LED] RGB NeoPixel on GPIO" + String(RGB_LED_PIN) + " (ESP32-S3)");
#else
        pinMode(BUILTIN_LED_PIN, OUTPUT);
        digitalWrite(BUILTIN_LED_PIN, LOW);
        _useRGB = false;
        Serial.println("[LED] Blue LED on GPIO2");
#endif
        _state = LED_OFF;
        _lastUpdate = 0;
        _flashEnd = 0;
        _blinkOn = false;
        _flashR = _flashG = _flashB = 0;
    }

    void setState(LedState state) {
        if (state == _state) return;
        _state = state;
        _lastUpdate = 0;
    }

    void flash(uint32_t durationMs = 150) {
        flashColor(80, 80, 80, durationMs);
    }

    void flashColor(uint8_t r, uint8_t g, uint8_t b, uint32_t durationMs = 200) {
        _flashEnd = millis() + durationMs;
        _flashR = r; _flashG = g; _flashB = b;
#if HAS_RGB_LED
        setRGB(r, g, b);
#else
        digitalWrite(BUILTIN_LED_PIN, HIGH);
#endif
    }

    void flashInitLed()      { flashColor(40, 20,  0, 200); }
    void flashInitWifi()     { flashColor( 0, 20, 60, 300); }
    void flashInitIdentity() { flashColor(30,  0, 50, 200); }
    void flashInitGenesis()  { flashColor(50, 50, 10, 400); }
    void flashInitConsensus(){ flashColor( 0, 40, 40, 200); }
    void flashInitNetwork()  { flashColor(20, 40, 20, 200); }
    void flashInitComplete() { flashColor(60, 60, 60, 600); }

    void flashValidation()   { flashColor( 0, 70,  0, 250); }
    void flashValidateIndex(){ flashColor( 0, 50, 10, 150); }
    void flashValidateHash() { flashColor( 0, 60, 20, 150); }
    void flashValidatePoS()  { flashColor(10, 70, 10, 200); }
    void flashValidateSlot() { flashColor(20, 60,  0, 150); }
    void flashStateVerified(){ flashColor( 0, 80, 20, 300); }
    void flashReject()       { flashColor(80,  0,  0, 400); }
    void flashRejectIndex()  { flashColor(80,  0, 10, 300); }
    void flashRejectHash()   { flashColor(80,  0, 20, 350); }
    void flashRejectPoS()    { flashColor(70, 10,  0, 350); }
    void flashRejectSlot()   { flashColor(60, 20,  0, 300); }
    void flashRejectCode()   { flashColor(50, 10, 20, 300); }
    void flashRejectState()  { flashColor(80,  0, 40, 400); }

    void flashGossipHeartbeat()  { flashColor(10, 30, 30, 100); }
    void flashGossipTx()         { flashColor(50, 50, 50, 150); }
    void flashGossipBlock()      { flashColor(30, 60,  0, 200); }
    void flashGossipContract()   { flashColor( 0, 40, 30, 200); }
    void flashGossipBlockAnn()   { flashColor(40, 30,  0, 150); }

    void flashSyncStart()    { flashColor(40,  0, 60, 300); }
    void flashSync()         { flashColor(40,  0, 60, 200); }
    void flashSyncApplied()  { flashColor( 0, 60, 10, 200); }
    void flashSyncReject()   { flashColor(70, 10,  0, 300); }
    void flashSyncDiverge()  { flashColor(60, 30,  0, 300); }
    void flashSyncReorg()    { flashColor(60, 40, 10, 400); }
    void flashSyncAdopt()    { flashColor(50, 50, 20, 350); }
    void flashSyncDone()     { flashColor( 0, 50, 30, 300); }
    void flashSyncFail()     { flashColor(60,  0, 10, 300); }

    void flashPeer()         { flashColor( 0, 50, 30, 250); }
    void flashDiscoverySend(){ flashColor(10, 25, 20, 100); }
    void flashGenesisOk()    { flashColor( 0, 60, 30, 400); }
    void flashGenesisWin()   { flashColor(20, 60,  0, 300); }
    void flashGenesisAdopt() { flashColor(40, 40, 10, 350); }
    void flashGenesisNode()  { flashColor(30, 40, 10, 200); }

    void flashAlive()        { flashColor( 5, 15,  5, 100); }

    void flashTcpBlockReq()  { flashColor(15, 15, 40, 150); }
    void flashTcpCodeReq()   { flashColor(15, 30, 15, 150); }
    void flashTcpSent()      { flashColor(30, 30, 30, 100); }

    void flashCodeCache()    { flashColor(10, 40, 20, 150); }
    void flashCodeFetch()    { flashColor( 0, 30, 50, 200); }
    void flashCodeOk()       { flashColor( 0, 50, 20, 200); }
    void flashCodeFail()     { flashColor(60, 10,  0, 300); }
    void flashCodeHashBad()  { flashColor(80,  0, 20, 350); }

    void flashWifiConnect()  { flashColor( 0, 50,  0, 300); }
    void flashWifiDisconnect(){ flashColor(60, 10, 0, 300); }
    void flashWifiReconnect(){ flashColor(50, 30,  0, 250); }

    void flashBlock()        { flashColor( 0, 60,  0, 300); }
    void flashProduced()     { flashColor(20, 20, 80, 500); }
    void flashProducing()    { flashColor(10, 10, 60, 200); }
    void flashEpoch()        { flashColor(60,  0, 60, 500); }
    void flashPrune()        { flashColor(40, 30, 10, 200); }

    void flashSlash()        { flashColor(80,  0,  0, 600); }
    void flashElection()     { flashColor(50, 10, 60, 400); }
    void flashJailed()       { flashColor(60,  0, 20, 400); }
    void flashDowntime()     { flashColor(50, 20,  0, 300); }

    void flashTxReceived()   { flashColor(50, 50, 50, 150); }
    void flashTransfer()     { flashColor(70, 70, 70, 250); }
    void flashDeploy()       { flashColor( 0, 80, 40, 400); }
    void flashCall()         { flashColor( 0, 40, 80, 200); }
    void flashStake()        { flashColor(60, 60,  0, 300); }
    void flashUnstake()      { flashColor(70, 35,  0, 300); }
    void flashClaim()        { flashColor( 0, 60, 60, 250); }
    void flashFaucet()       { flashColor(60,  0, 50, 250); }
    void flashData()         { flashColor(40, 40, 60, 200); }
    void flashTxFail()       { flashColor(80, 10,  0, 300); }

    void flashForTxType(uint8_t txType) {
        switch (txType) {
            case 0x01: flashData(); break;
            case 0x02: flashDeploy(); break;
            case 0x03: flashCall(); break;
            case 0x04: flashTransfer(); break;
            case 0x10: flashStake(); break;
            case 0x11: flashUnstake(); break;
            case 0x14: flashClaim(); break;
            case 0x15: flashFaucet(); break;
            default:   flashTxReceived(); break;
        }
    }

    void flashForOpcode(uint8_t op) {

        static const uint8_t opColors[][3] = {
            { 80,  0,  0 },
            { 20, 40, 80 },
            { 40, 10, 70 },
            { 50, 30, 70 },
            { 30, 30, 80 },
            { 80, 40,  0 },
            { 70, 50,  0 },
            { 60, 55,  0 },
            { 70, 40, 20 },
            { 80, 30, 20 },
            {  0, 60, 60 },
            {  0, 50, 40 },
            { 10, 60, 40 },
            {  0, 50, 50 },
            { 10, 55, 30 },
            { 40, 70,  0 },
            { 50, 70, 10 },
            { 70,  0, 60 },
            { 70, 70,  0 },
            { 80, 65,  0 },
            { 70, 20, 40 },
            {  0, 60, 20 },
            { 20, 70, 40 },
            {  0, 50, 30 },
            { 50,  0, 70 },
            { 70, 20, 50 },
            { 70, 30, 20 },
            { 10, 10, 80 },
        };
        if (op <= 0x1B) {
            flashColor(opColors[op][0], opColors[op][1], opColors[op][2], 200);
        } else {
            flashColor(30, 30, 30, 150);
        }
    }

    void flashVMStatus(uint8_t vmStatus) {
        switch (vmStatus) {
            case 1:  flashValidation(); break;
            case 2:  flashColor(80, 40,  0, 300); break;
            case 3:  flashColor(70, 30,  0, 300); break;
            case 4:  flashColor(60, 25,  0, 300); break;
            case 5:  flashColor(70, 70,  0, 350); break;
            case 6:  flashColor(60,  0, 60, 300); break;
            case 7:  flashColor(80,  0, 30, 300); break;
            case 8:  flashColor(80,  0,  0, 300); break;
            case 9:  flashColor(70, 50,  0, 300); break;
            case 10: flashColor(60, 20,  0, 300); break;
            case 11: flashColor(50,  0, 50, 300); break;
            case 12: flashColor(60,  0, 30, 300); break;
            case 13: flashColor(70, 20,  0, 300); break;
            default: flashTxFail(); break;
        }
    }

    void bootSequence() {
#if HAS_RGB_LED
        uint8_t colors[][3] = {
            {40,0,0}, {0,40,0}, {0,0,40}, {40,40,0}, {0,40,40}, {40,0,40}, {40,40,40}
        };
        for (int i = 0; i < 7; i++) {
            setRGB(colors[i][0], colors[i][1], colors[i][2]);
            delay(80);
        }
        setRGB(0, 0, 0);
#else
        for (int i = 0; i < 6; i++) {
            digitalWrite(BUILTIN_LED_PIN, (i % 2) ? HIGH : LOW);
            delay(80);
        }
        digitalWrite(BUILTIN_LED_PIN, LOW);
#endif
    }

    void update() {
        uint32_t now = millis();

        if (_flashEnd > 0) {
            if (now >= _flashEnd) {
                _flashEnd = 0;
                _lastUpdate = 0;
            } else {
                return;
            }
        }

        if (now - _lastUpdate < 30) return;
        _lastUpdate = now;

        _blinkOn = !_blinkOn;

#if HAS_RGB_LED
        switch (_state) {
            case LED_OFF:
                setRGB(0, 0, 0);
                break;

            case LED_NO_WIFI:

                {
                    uint32_t phase = (now % 1200);

                    uint8_t v;
                    if (phase < 600)
                        v = (uint8_t)(3 + (uint32_t)57 * phase / 600);
                    else
                        v = (uint8_t)(3 + (uint32_t)57 * (1200 - phase) / 600);
                    setRGB(v, 0, 0);
                }
                break;

            case LED_NO_PEERS:

                {
                    uint32_t phase = (now % 2000);
                    uint8_t r, g;
                    if (phase < 1000) {

                        r = (uint8_t)(4 + (uint32_t)46 * phase / 1000);
                        g = (uint8_t)(2 + (uint32_t)20 * phase / 1000);
                    } else {

                        uint32_t down = phase - 1000;
                        r = (uint8_t)(4 + (uint32_t)46 * (1000 - down) / 1000);
                        g = (uint8_t)(2 + (uint32_t)20 * (1000 - down) / 1000);
                    }
                    setRGB(r, g, 0);
                }
                break;

            case LED_CONNECTED:

                {
                    uint32_t phase = (now % 2000);
                    uint8_t v;
                    if (phase < 120) {

                        v = (uint8_t)(5 + (uint32_t)35 * phase / 120);
                    } else if (phase < 240) {

                        v = (uint8_t)(5 + (uint32_t)35 * (240 - phase) / 120);
                    } else if (phase < 400) {

                        v = 5;
                    } else if (phase < 520) {

                        v = (uint8_t)(5 + (uint32_t)45 * (phase - 400) / 120);
                    } else if (phase < 640) {

                        v = (uint8_t)(5 + (uint32_t)45 * (640 - phase) / 120);
                    } else {

                        v = 5;
                    }
                    setRGB(0, v, 0);
                }
                break;

            case LED_VALIDATOR:

                {
                    uint32_t phase = (now % 1800);
                    uint8_t v;
                    if (phase < 100) {
                        v = (uint8_t)(8 + (uint32_t)47 * phase / 100);
                    } else if (phase < 200) {
                        v = (uint8_t)(8 + (uint32_t)47 * (200 - phase) / 100);
                    } else if (phase < 350) {
                        v = 8;
                    } else if (phase < 450) {
                        v = (uint8_t)(8 + (uint32_t)52 * (phase - 350) / 100);
                    } else if (phase < 550) {
                        v = (uint8_t)(8 + (uint32_t)52 * (550 - phase) / 100);
                    } else {

                        uint32_t rest = (phase - 550);
                        uint32_t restCycle = rest % 500;
                        if (restCycle < 250)
                            v = (uint8_t)(6 + (uint32_t)6 * restCycle / 250);
                        else
                            v = (uint8_t)(6 + (uint32_t)6 * (500 - restCycle) / 250);
                    }
                    setRGB(0, v, v);
                }
                break;

            case LED_PRODUCING:

                {
                    uint32_t phase = (now % 400);
                    if (phase < 80) {

                        uint8_t v = (uint8_t)(30 + (uint32_t)50 * phase / 80);
                        setRGB(v / 3, v / 3, v);
                    } else if (phase < 160) {

                        uint8_t v = (uint8_t)(30 + (uint32_t)50 * (160 - phase) / 80);
                        setRGB(v / 3, v / 3, v);
                    } else if (phase < 240) {

                        uint8_t v = (uint8_t)(20 + (uint32_t)40 * (phase - 160) / 80);
                        setRGB(0, 0, v);
                    } else if (phase < 320) {
                        uint8_t v = (uint8_t)(20 + (uint32_t)40 * (320 - phase) / 80);
                        setRGB(0, 0, v);
                    } else {
                        setRGB(0, 0, 12);
                    }
                }
                break;

            case LED_SYNCING:

                {
                    uint32_t phase = (now % 1500);
                    uint8_t r, b;

                    if (phase < 500) {
                        r = (uint8_t)(4 + (uint32_t)50 * phase / 500);
                        b = (uint8_t)(4 + (uint32_t)50 * phase / 500);
                    } else if (phase < 700) {

                        r = (uint8_t)(40 + (uint32_t)14 * (700 - phase) / 200);
                        b = r;
                    } else {

                        uint32_t down = phase - 700;
                        r = (uint8_t)(4 + (uint32_t)36 * (800 - (down > 800 ? 800 : down)) / 800);
                        b = r;
                    }
                    setRGB(r, 0, b);
                }
                break;

            case LED_TX_FLASH:
                setRGB(50, 50, 50);
                break;

            case LED_BOOT:

                {
                    uint32_t phase = (now % 3000);
                    uint8_t v;
                    if (phase < 1500)
                        v = (uint8_t)((uint32_t)50 * phase / 1500);
                    else
                        v = (uint8_t)((uint32_t)50 * (3000 - phase) / 1500);
                    setRGB(0, v / 3, v);
                }
                break;

            case LED_ERROR:

                {
                    uint32_t phase = (now % 800);
                    uint8_t v;
                    if (phase < 60)
                        v = 70;
                    else if (phase < 120)
                        v = 0;
                    else if (phase < 180)
                        v = 70;
                    else
                        v = 0;
                    setRGB(v, 0, 0);
                }
                break;
        }
#else
        uint32_t slowBlink = (now / 800) % 2;
        uint32_t fastBlink = (now / 200) % 2;
        uint32_t heartbeat = (now % 2000);
        bool hbOn = (heartbeat < 120) || (heartbeat >= 400 && heartbeat < 520);
        switch (_state) {
            case LED_OFF:
                digitalWrite(BUILTIN_LED_PIN, LOW);
                break;
            case LED_NO_WIFI:

                digitalWrite(BUILTIN_LED_PIN, ((now / 600) % 2) ? HIGH : LOW);
                break;
            case LED_NO_PEERS:
                digitalWrite(BUILTIN_LED_PIN, slowBlink ? HIGH : LOW);
                break;
            case LED_CONNECTED:

                digitalWrite(BUILTIN_LED_PIN, hbOn ? HIGH : LOW);
                break;
            case LED_VALIDATOR:

                { uint32_t vhb = (now % 1500);
                  bool vOn = (vhb < 100) || (vhb >= 300 && vhb < 400);
                  digitalWrite(BUILTIN_LED_PIN, vOn ? HIGH : LOW);
                }
                break;
            case LED_PRODUCING:

                digitalWrite(BUILTIN_LED_PIN, ((now / 100) % 2) ? HIGH : LOW);
                break;
            case LED_SYNCING:
                digitalWrite(BUILTIN_LED_PIN, _blinkOn ? HIGH : LOW);
                break;
            case LED_ERROR:

                { uint32_t ep = (now % 800);
                  bool eOn = (ep < 60) || (ep >= 120 && ep < 180);
                  digitalWrite(BUILTIN_LED_PIN, eOn ? HIGH : LOW);
                }
                break;
            default:
                break;
        }
#endif
    }

    LedState getState() const { return _state; }

private:
    LedState _state;
    bool     _useRGB;
    uint32_t _lastUpdate;
    uint32_t _flashEnd;
    bool     _blinkOn;
    uint8_t  _flashR, _flashG, _flashB;

#if HAS_RGB_LED
    void setRGB(uint8_t r, uint8_t g, uint8_t b) {


        neopixelWrite(RGB_LED_PIN, r, g, b);
    }
#endif
};
