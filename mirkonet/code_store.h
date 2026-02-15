#pragma once

#include <SPIFFS.h>
#include "config.h"

// Persistent bytecode storage on flash (SPIFFS).
// Contracts are saved as /code/<name>.bin so they survive reboots.
// Only the host node needs to persist bytecode; non-host nodes fetch on demand.

struct CodeStore {
    static bool begin() {
        if (!SPIFFS.begin(true)) {   // true = format on first use
            Serial0.println("[CodeStore] SPIFFS mount FAILED");
            return false;
        }
        Serial0.printf("[CodeStore] SPIFFS mounted (used %u / %u bytes)\n",
                       SPIFFS.usedBytes(), SPIFFS.totalBytes());
        return true;
    }

    // Save bytecode to flash. Returns true on success.
    static bool save(const char* name, const uint8_t* code, uint16_t len) {
        char path[32];
        snprintf(path, sizeof(path), "/code/%s.bin", name);

        File f = SPIFFS.open(path, FILE_WRITE);
        if (!f) {
            Serial0.printf("[CodeStore] Failed to open '%s' for write\n", path);
            return false;
        }
        size_t written = f.write(code, len);
        f.close();

        if (written != len) {
            Serial0.printf("[CodeStore] Write error: %u/%u bytes\n", (unsigned)written, len);
            return false;
        }
        Serial0.printf("[CodeStore] Saved '%s' (%d bytes)\n", name, len);
        return true;
    }

    // Load bytecode from flash. Returns code length, or 0 on failure.
    static uint16_t load(const char* name, uint8_t* outCode, uint16_t maxLen) {
        char path[32];
        snprintf(path, sizeof(path), "/code/%s.bin", name);

        if (!SPIFFS.exists(path)) return 0;

        File f = SPIFFS.open(path, FILE_READ);
        if (!f) return 0;

        size_t sz = f.size();
        if (sz == 0 || sz > maxLen) { f.close(); return 0; }

        size_t rd = f.read(outCode, sz);
        f.close();
        return (rd == sz) ? (uint16_t)sz : 0;
    }

    // Check if bytecode exists on flash.
    static bool exists(const char* name) {
        char path[32];
        snprintf(path, sizeof(path), "/code/%s.bin", name);
        return SPIFFS.exists(path);
    }

    // Remove bytecode from flash.
    static bool remove(const char* name) {
        char path[32];
        snprintf(path, sizeof(path), "/code/%s.bin", name);
        return SPIFFS.remove(path);
    }
};
