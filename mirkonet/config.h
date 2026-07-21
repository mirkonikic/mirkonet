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
#define SLOT_TOLERANCE      4000
#define FINALITY_DEPTH      4

#define BLOCK_REWARD        10
#define MIN_STAKE           100
#define UNSTAKE_COOLDOWN    50
#define GENESIS_SUPPLY      50000
#define GENESIS_PER_NODE    10000

#define GENESIS_FOUNDER_BYTES { 0x4D, 0x49, 0x52, 0x4B, 0x4F, 0x01 }

#define BOOTSTRAP_HEIGHT    EPOCH_LENGTH
#define FAUCET_AMOUNT       50
#define FAUCET_COOLDOWN     60000
#define MAX_TXN_PER_BLOCK   4

#define DEFAULT_GAS_PRICE   1
#define MIN_GAS_PRICE       1
#define DEFAULT_GAS_LIMIT   8000
#define TX_BASE_GAS         21000
#define DEPLOY_GAS_PER_BYTE 1
#define FAUCET_FEE          0

#define MAX_BLOCKS          20
#define HASH_SIZE           32
#define MAX_FINALIZED       100
#define MAX_CHECKPOINT_STATE_BYTES 8192

#define PRUNE_TRIGGER       16
#define PRUNE_KEEP          8
#define MAX_CHECKPOINTS     8
#define CHECKPOINT_QUORUM   2
#define CHECKPOINT_SINGLE_PEER_FALLBACK 1
#define MAX_CHECKPOINT_SIGNATURES MAX_VALIDATORS
#define CONTRACT_CODE_BACKUP_CACHE 1
#define CHAINSTATE_NVS_PERSISTENCE 0

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


#define GAS_BASE            2
#define GAS_VERYLOW         3
#define GAS_LOW             5
#define GAS_MID             8
#define GAS_JUMP            8
#define GAS_SLOAD          50
#define GAS_SSTORE        100
#define GAS_SHA3           30
#define GAS_LOG            50
#define GAS_LOG_TOPIC      25
#define GAS_CALL          100
#define GAS_MEMORY          3
#define GAS_BALANCE        20
#define GAS_EMIT            5
#define GAS_GPIO           80

#define SLASH_DOUBLE_SIGN_PCT   10
#define SLASH_DOWNTIME_PCT       2
#define DOWNTIME_THRESHOLD      10
#define SLASH_JAIL_EPOCHS        2

#define GPIO_MAX_ALLOWED    21
static const uint8_t GPIO_ALLOWED_PINS[GPIO_MAX_ALLOWED] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21
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
