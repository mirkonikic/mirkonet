#pragma once
#include <Arduino.h>
#include "config.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
    #define HAS_RGB_LED 1
    // Use RGB_BUILTIN if defined by the board, else default to GPIO 48
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

    // --- Block-level events ---
    void flashBlock()   { flashColor(0, 60, 0, 300); }    // Green: block accepted
    void flashReject()  { flashColor(80, 0, 0, 400); }    // Red: block rejected
    void flashEpoch()   { flashColor(60, 0, 60, 500); }   // Purple: epoch transition
    void flashSync()    { flashColor(40, 0, 60, 200); }   // Purple: sync progress
    void flashPeer()    { flashColor(0, 50, 30, 250); }   // Teal: new peer joined
    void flashProduced(){ flashColor(20, 20, 80, 500); }  // Blue-white: we produced a block

    // --- Validation feedback (green family) ---
    void flashValidation()   { flashColor(0, 70, 0, 250); }    // Bright green: generic validation pass
    void flashStateVerified(){ flashColor(0, 80, 20, 300); }   // Green-lime: state root verified
    void flashGenesisOk()    { flashColor(0, 60, 30, 400); }   // Green-teal: genesis sync OK

    // --- Rejection feedback (red family) ---
    void flashTxFail()       { flashColor(80, 10, 0, 300); }   // Red-orange: tx execution failed
    void flashHashMismatch() { flashColor(80, 0, 20, 350); }   // Red-pink: hash/state mismatch
    void flashSlashDetected(){ flashColor(80, 0, 0, 600); }    // Deep red: slashing event

    // --- Transaction type flashes (white base + type-specific) ---
    void flashTxReceived()   { flashColor(50, 50, 50, 150); }  // White: any tx received via gossip
    void flashTransfer()     { flashColor(70, 70, 70, 250); }  // Bright white: token transfer
    void flashDeploy()       { flashColor(0, 80, 40, 400); }   // Green-cyan: contract deploy
    void flashCall()         { flashColor(0, 40, 80, 200); }   // Blue: contract call
    void flashStake()        { flashColor(60, 60, 0, 300); }   // Yellow: staking
    void flashUnstake()      { flashColor(70, 35, 0, 300); }   // Orange: unstaking
    void flashClaim()        { flashColor(0, 60, 60, 250); }   // Cyan: claim tokens
    void flashFaucet()       { flashColor(60, 0, 50, 250); }   // Magenta: faucet request
    void flashData()         { flashColor(40, 40, 60, 200); }  // Blue-grey: data tx

    // --- MVM opcode category flashes (called after VM execution) ---
    void flashOpcodeArith()  { flashColor(70, 40, 0, 200); }   // Orange: arithmetic (ADD/SUB/MUL/DIV/MOD)
    void flashOpcodeLogic()  { flashColor(0, 60, 60, 200); }   // Cyan: logic/bitwise (AND/OR/XOR/NOT/CMP)
    void flashOpcodeStack()  { flashColor(20, 20, 70, 200); }  // Blue: stack ops (PUSH/POP/DUP/SWAP)
    void flashOpcodeStorage(){ flashColor(60, 60, 0, 250); }   // Yellow: storage (SLOAD/SSTORE)
    void flashOpcodeFlow()   { flashColor(60, 0, 60, 200); }   // Magenta: control flow (JMP/HALT)
    void flashOpcodeCrypto() { flashColor(50, 0, 70, 250); }   // Purple: crypto (SHA3)
    void flashOpcodeContext(){ flashColor(0, 50, 40, 200); }   // Teal: context (ARG/BALANCE/CALLVALUE)
    void flashOpcodeEvent()  { flashColor(70, 20, 40, 200); }  // Pink: events (EMIT/LOG)
    void flashOpcodeCall()   { flashColor(10, 10, 80, 250); }  // Deep blue: inter-contract (XCALL)

    // --- VM execution result flash ---
    void flashVMStatus(uint8_t vmStatus) {
        switch (vmStatus) {
            case 1:  flashValidation(); break;      // VM_HALTED (success) -> green
            case 2:  flashColor(80, 40, 0, 300); break; // VM_REVERTED -> orange-red
            case 3:  // VM_ERR_STACK_OVER
            case 4:  flashColor(70, 30, 0, 300); break; // VM_ERR_STACK_UNDER -> orange
            case 5:  flashColor(70, 70, 0, 350); break; // VM_ERR_OUT_OF_GAS -> yellow
            case 6:  flashColor(60, 0, 60, 300); break; // VM_ERR_BAD_JUMP -> magenta
            case 7:  flashColor(80, 0, 30, 300); break; // VM_ERR_CODE_BOUNDS -> red-pink
            case 8:  flashColor(80, 0, 0, 300); break;  // VM_ERR_DIV_ZERO -> red
            case 9:  flashColor(70, 50, 0, 300); break; // VM_ERR_STORAGE_FULL -> amber
            case 10: flashColor(60, 20, 0, 300); break; // VM_ERR_BAD_ARG -> dark orange
            case 11: flashColor(50, 0, 50, 300); break; // VM_ERR_CALL_DEPTH -> purple
            case 12: flashColor(60, 0, 30, 300); break; // VM_ERR_CALL_FAIL -> dark red-pink
            case 13: flashColor(70, 20, 0, 300); break; // VM_ERR_MEM_BOUNDS -> orange
            default: flashTxFail(); break;
        }
    }

    // --- Flash for transaction type ---
    void flashForTxType(uint8_t txType) {
        switch (txType) {
            case 0x01: flashData(); break;       // DATA
            case 0x02: flashDeploy(); break;     // DEPLOY
            case 0x03: flashCall(); break;       // CALL
            case 0x04: flashTransfer(); break;   // TRANSFER
            case 0x10: flashStake(); break;      // STAKE
            case 0x11: flashUnstake(); break;    // UNSTAKE
            case 0x14: flashClaim(); break;      // CLAIM
            case 0x15: flashFaucet(); break;     // FAUCET
            default:   flashTxReceived(); break;
        }
    }

    // --- Flash for MVM opcode category ---
    void flashForOpcode(uint8_t op) {
        // op is the 5-bit opcode (already decoded)
        if (op <= 0x00)      flashOpcodeFlow();    // HALT
        else if (op <= 0x04) flashOpcodeStack();   // PUSH/POP/DUP/SWAP
        else if (op <= 0x09) flashOpcodeArith();   // ADD/SUB/MUL/DIV/MOD
        else if (op <= 0x0E) flashOpcodeLogic();   // AND/OR/XOR/SHL/SHR
        else if (op <= 0x10) flashOpcodeLogic();   // CMP/NOT
        else if (op == 0x11) flashOpcodeFlow();    // JMP
        else if (op <= 0x13) flashOpcodeStorage(); // SLOAD/SSTORE
        else if (op == 0x14) flashOpcodeEvent();   // EMIT
        else if (op <= 0x17) flashOpcodeContext();  // ARG/BALANCE/CALLVALUE
        else if (op == 0x18) flashOpcodeCrypto();  // SHA3
        else if (op == 0x19) flashOpcodeEvent();   // LOG
        else if (op == 0x1A) flashOpcodeContext();  // EXT (context ops)
        else if (op == 0x1B) flashOpcodeCall();    // XCALL
        else                 flashTxFail();        // unknown
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

        // Update every 30ms for smooth animations
        if (now - _lastUpdate < 30) return;
        _lastUpdate = now;

        _blinkOn = !_blinkOn;

#if HAS_RGB_LED
        switch (_state) {
            case LED_OFF:
                setRGB(0, 0, 0);
                break;

            case LED_NO_WIFI:
                // Urgent red pulse: smooth sine-like breathing, 1.2s period
                {
                    uint32_t phase = (now % 1200);
                    // Triangle wave 0→60→0 with a floor at 3 so it never fully goes dark
                    uint8_t v;
                    if (phase < 600)
                        v = (uint8_t)(3 + (uint32_t)57 * phase / 600);
                    else
                        v = (uint8_t)(3 + (uint32_t)57 * (1200 - phase) / 600);
                    setRGB(v, 0, 0);
                }
                break;

            case LED_NO_PEERS:
                // Searching orange sweep: alternates between orange and dim amber, 2s period
                {
                    uint32_t phase = (now % 2000);
                    uint8_t r, g;
                    if (phase < 1000) {
                        // Ramp up
                        r = (uint8_t)(4 + (uint32_t)46 * phase / 1000);
                        g = (uint8_t)(2 + (uint32_t)20 * phase / 1000);
                    } else {
                        // Ramp down
                        uint32_t down = phase - 1000;
                        r = (uint8_t)(4 + (uint32_t)46 * (1000 - down) / 1000);
                        g = (uint8_t)(2 + (uint32_t)20 * (1000 - down) / 1000);
                    }
                    setRGB(r, g, 0);
                }
                break;

            case LED_CONNECTED:
                // Green heartbeat: double-pulse every 2s (like a real heartbeat)
                {
                    uint32_t phase = (now % 2000);
                    uint8_t v;
                    if (phase < 120) {
                        // First beat up
                        v = (uint8_t)(5 + (uint32_t)35 * phase / 120);
                    } else if (phase < 240) {
                        // First beat down
                        v = (uint8_t)(5 + (uint32_t)35 * (240 - phase) / 120);
                    } else if (phase < 400) {
                        // Brief pause
                        v = 5;
                    } else if (phase < 520) {
                        // Second beat up (stronger)
                        v = (uint8_t)(5 + (uint32_t)45 * (phase - 400) / 120);
                    } else if (phase < 640) {
                        // Second beat down
                        v = (uint8_t)(5 + (uint32_t)45 * (640 - phase) / 120);
                    } else {
                        // Rest
                        v = 5;
                    }
                    setRGB(0, v, 0);
                }
                break;

            case LED_VALIDATOR:
                // Cyan heartbeat with a glow: double-pulse + gentle background glow
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
                        // Gentle glow during rest
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
                // Rapid blue strobe with white peaks during block production
                {
                    uint32_t phase = (now % 400);
                    if (phase < 80) {
                        // Bright white-blue flash
                        uint8_t v = (uint8_t)(30 + (uint32_t)50 * phase / 80);
                        setRGB(v / 3, v / 3, v);
                    } else if (phase < 160) {
                        // Fade back
                        uint8_t v = (uint8_t)(30 + (uint32_t)50 * (160 - phase) / 80);
                        setRGB(v / 3, v / 3, v);
                    } else if (phase < 240) {
                        // Second pulse
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
                // Magenta chase: pulsing at medium speed, 1.5s period
                {
                    uint32_t phase = (now % 1500);
                    uint8_t r, b;
                    // Smooth triangle wave with quick bright peak
                    if (phase < 500) {
                        r = (uint8_t)(4 + (uint32_t)50 * phase / 500);
                        b = (uint8_t)(4 + (uint32_t)50 * phase / 500);
                    } else if (phase < 700) {
                        // Hold near peak briefly
                        r = (uint8_t)(40 + (uint32_t)14 * (700 - phase) / 200);
                        b = r;
                    } else {
                        // Slow fade down
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
                // Blue breathing - smooth cycle, 3s period
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
                // Aggressive red double-flash strobe, 800ms period
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
                // Pulsing blink
                digitalWrite(BUILTIN_LED_PIN, ((now / 600) % 2) ? HIGH : LOW);
                break;
            case LED_NO_PEERS:
                digitalWrite(BUILTIN_LED_PIN, slowBlink ? HIGH : LOW);
                break;
            case LED_CONNECTED:
                // Heartbeat pattern
                digitalWrite(BUILTIN_LED_PIN, hbOn ? HIGH : LOW);
                break;
            case LED_VALIDATOR:
                // Faster heartbeat
                { uint32_t vhb = (now % 1500);
                  bool vOn = (vhb < 100) || (vhb >= 300 && vhb < 400);
                  digitalWrite(BUILTIN_LED_PIN, vOn ? HIGH : LOW);
                }
                break;
            case LED_PRODUCING:
                // Rapid strobe
                digitalWrite(BUILTIN_LED_PIN, ((now / 100) % 2) ? HIGH : LOW);
                break;
            case LED_SYNCING:
                digitalWrite(BUILTIN_LED_PIN, _blinkOn ? HIGH : LOW);
                break;
            case LED_ERROR:
                // Double-flash
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
        // neopixelWrite() is available in Arduino ESP32 core >= 2.0 for S3.
        // It uses the RMT peripheral internally and handles pin > 31 correctly.
        // The previous bit-banging fallback was broken for GPIO 48 because
        // GPIO.out_w1ts only controls pins 0-31 (pin 48 needs GPIO.out1_w1ts).
        // The old #if defined(neopixelWrite) check also failed because
        // neopixelWrite is a function declaration, not a preprocessor macro.
        neopixelWrite(RGB_LED_PIN, r, g, b);
    }
#endif
};
