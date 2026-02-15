#pragma once

#define UDP_DISCOVERY_PORT  8710
#define UDP_GOSSIP_PORT     8711
#define TCP_SYNC_PORT       8712

#define MAX_PEERS           12
#define DISCOVERY_INTERVAL  5000
#define HEARTBEAT_INTERVAL  3000
#define PEER_TIMEOUT        20000
#define SYNC_COOLDOWN       10000
#define BOOT_GRACE_PERIOD   15000

#define MAX_VALIDATORS      7
#define MAX_CANDIDATES      16
#define EPOCH_LENGTH        20
#define BLOCK_INTERVAL      6000
#define SLOT_TOLERANCE      2000
#define FINALITY_DEPTH      4

#define BLOCK_REWARD        10
#define MIN_STAKE           100
#define UNSTAKE_COOLDOWN    50
#define GENESIS_SUPPLY      50000
#define GENESIS_PER_NODE    10000
#define FAUCET_AMOUNT       50
#define FAUCET_COOLDOWN     60000
#define MAX_TXN_PER_BLOCK   4

#define DEFAULT_GAS_PRICE   1
#define MIN_GAS_PRICE       1
#define DEFAULT_GAS_LIMIT   8000
#define TX_BASE_GAS         21000
#define DEPLOY_GAS_PER_BYTE 200
#define FAUCET_FEE          0

#define MAX_BLOCKS          20
#define HASH_SIZE           32
#define MAX_FINALIZED       100

#define PRUNE_TRIGGER       16
#define PRUNE_KEEP          8
#define MAX_CHECKPOINTS     8

#define MVM_MAX_STACK       48
#define MVM_MAX_STORAGE     16
#define MVM_MAX_GAS         8000
#define MVM_MAX_CODE        256
#define MVM_MAX_EVENTS      8
#define MVM_MAX_ARGS        8
#define MVM_MAX_MEMORY      64
#define MVM_MAX_LOG_TOPICS  4
#define MVM_MAX_LOGS        8
#define MVM_MAX_CALL_DEPTH  4

// --- Per-opcode gas costs (Ethereum-inspired, scaled for ESP32) ---
#define GAS_BASE            2    // HALT, POP, PUSH, DUP, SWAP, etc
#define GAS_VERYLOW         3    // ADD, SUB, NOT, LT, GT, EQ, ISZERO
#define GAS_LOW             5    // MUL, DIV, MOD
#define GAS_MID             8    // AND, OR, XOR, SHL, SHR, BYTE, SIGNEXTEND
#define GAS_JUMP            8    // JUMP, JUMPI
#define GAS_SLOAD          50    // Storage read
#define GAS_SSTORE        100    // Storage write
#define GAS_SHA3           30    // SHA3/Keccak
#define GAS_LOG            50    // LOG base
#define GAS_LOG_TOPIC      25    // per indexed topic
#define GAS_CALL          100    // contract-to-contract call base
#define GAS_MEMORY          3    // MLOAD/MSTORE
#define GAS_BALANCE        20    // BALANCE, CALLER, context lookups
#define GAS_EMIT            5    // legacy EMIT (kept for compat)
#define GAS_GPIO           80    // GPIO read/write (privileged hardware access)

// --- Slashing ---
#define SLASH_DOUBLE_SIGN_PCT   10   // 10% slash for double-signing
#define SLASH_DOWNTIME_PCT       2   // 2% slash for prolonged downtime
#define DOWNTIME_THRESHOLD      10   // blocks missed before downtime slash
#define SLASH_JAIL_EPOCHS        2   // epochs a slashed validator is jailed

// --- GPIO allowlist (safe pins for smart contract control) ---
// ESP32-S3: avoid 0 (boot), 26-32 (SPI flash/PSRAM), 43-44 (UART0).
// GPIO 1-21 are general-purpose safe pins for relay/LED/sensor control.
// ADC1: GPIO 1-10. ADC2: GPIO 11-20 (unavailable when WiFi active).
// ESP32-S3 has no DAC peripheral.
#define GPIO_MAX_ALLOWED    21
static const uint8_t GPIO_ALLOWED_PINS[GPIO_MAX_ALLOWED] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,  // digital I/O + ADC1
    11, 12, 13, 14, 15, 16, 17, 18,  // digital I/O (+ ADC2)
    19, 20, 21                        // digital I/O
};

#define MAX_CONTRACTS       8
#define MAX_ACCOUNTS        24
#define MAX_PENDING_TX      12

enum NodeRole : uint8_t {
    ROLE_LIGHT      = 0,
    ROLE_FULL       = 1,
    ROLE_CANDIDATE  = 2,
    ROLE_VALIDATOR  = 3,
};

inline const char* roleName(NodeRole r) {
    switch (r) {
        case ROLE_LIGHT:     return "LIGHT";
        case ROLE_FULL:      return "FULL";
        case ROLE_CANDIDATE: return "CANDIDATE";
        case ROLE_VALIDATOR: return "VALIDATOR";
        default:             return "?";
    }
}