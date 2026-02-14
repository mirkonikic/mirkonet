#pragma once
#include <Arduino.h>
#include "config.h"

#include "soc/gpio_struct.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
    #define HAS_RGB_LED 1
    #define RGB_LED_PIN 48
    #include <driver/rmt.h>
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
};

class StatusLED {
public:
    void init() {
#if HAS_RGB_LED
        pinMode(RGB_LED_PIN, OUTPUT);
        _useRGB = true;
        setRGB(0, 0, 0);
        Serial.println("[LED] RGB LED on GPIO48 (ESP32-S3)");
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
    }

    void setState(LedState state) {
        if (state == _state) return;
        _state = state;
        _lastUpdate = 0;
    }

    void flash(uint32_t durationMs = 150) {
        _flashEnd = millis() + durationMs;
#if HAS_RGB_LED
        setRGB(80, 80, 80);
#else
        digitalWrite(BUILTIN_LED_PIN, HIGH);
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

        if (now - _lastUpdate < 200) return;
        _lastUpdate = now;

        _blinkOn = !_blinkOn;
        uint32_t slowBlink = (now / 800) % 2;
        uint32_t fastBlink = (now / 200) % 2;
        uint32_t breathe = (now / 50) % 100;

#if HAS_RGB_LED
        switch (_state) {
            case LED_OFF:
                setRGB(0, 0, 0);
                break;
            case LED_NO_WIFI:

                { uint8_t v = (uint8_t)(slowBlink ? 40 : 5);
                  setRGB(v, 0, 0); }
                break;
            case LED_NO_PEERS:

                { uint8_t v = (uint8_t)(slowBlink ? 30 : 2);
                  setRGB(v, v/2, 0); }
                break;
            case LED_CONNECTED:

                setRGB(0, 15, 0);
                break;
            case LED_VALIDATOR:

                setRGB(0, 15, 15);
                break;
            case LED_PRODUCING:

                { uint8_t v = (uint8_t)(fastBlink ? 60 : 10);
                  setRGB(0, 0, v); }
                break;
            case LED_SYNCING:

                { uint8_t v = (uint8_t)(slowBlink ? 30 : 5);
                  setRGB(v, 0, v); }
                break;
            case LED_TX_FLASH:
                setRGB(50, 50, 50);
                break;
        }
#else
        switch (_state) {
            case LED_OFF:
            case LED_NO_WIFI:
                digitalWrite(BUILTIN_LED_PIN, LOW);
                break;
            case LED_NO_PEERS:
                digitalWrite(BUILTIN_LED_PIN, slowBlink ? HIGH : LOW);
                break;
            case LED_CONNECTED:
            case LED_VALIDATOR:
                digitalWrite(BUILTIN_LED_PIN, HIGH);
                break;
            case LED_PRODUCING:
                digitalWrite(BUILTIN_LED_PIN, fastBlink ? HIGH : LOW);
                break;
            case LED_SYNCING:
                digitalWrite(BUILTIN_LED_PIN, _blinkOn ? HIGH : LOW);
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

#if HAS_RGB_LED
    void setRGB(uint8_t r, uint8_t g, uint8_t b) {
        #if defined(neopixelWrite)
            neopixelWrite(RGB_LED_PIN, r, g, b);
        #else
            uint8_t pixels[3] = { g, r, b };
            portDISABLE_INTERRUPTS();
            for (int byte = 0; byte < 3; byte++) {
                for (int bit = 7; bit >= 0; bit--) {
                    if (pixels[byte] & (1 << bit)) {
                        GPIO.out_w1ts = (1 << RGB_LED_PIN);
                        delayMicroseconds(1);
                        GPIO.out_w1tc = (1 << RGB_LED_PIN);
                    } else {
                        GPIO.out_w1ts = (1 << RGB_LED_PIN);
                        asm volatile("nop; nop; nop;");
                        GPIO.out_w1tc = (1 << RGB_LED_PIN);
                        delayMicroseconds(1);
                    }
                }
            }
            portENABLE_INTERRUPTS();
            delayMicroseconds(80);
        #endif
    }
#endif
};
