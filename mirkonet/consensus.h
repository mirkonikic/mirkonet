#pragma once
#include "types.h"
#include "config.h"
#include "staking.h"

class Consensus {
public:
    NodeID      selfId;
    NodeRole    selfRole = ROLE_LIGHT;
    uint32_t    genesisTime = 0;
    uint32_t    lastProducedSlot = UINT32_MAX;
    uint32_t    missedSlots = 0;

    void init(const NodeID& self) {
        selfId = self;
        genesisTime = millis();
    }

    uint32_t getCurrentSlot() const {
        if (genesisTime == 0) return 0;
        return (millis() - genesisTime) / BLOCK_INTERVAL;
    }

    uint32_t getSlotStartTime(uint32_t slot) const {
        return genesisTime + slot * BLOCK_INTERVAL;
    }

    bool isWithinSlotWindow() const {
        uint32_t slot = getCurrentSlot();
        uint32_t elapsed = millis() - getSlotStartTime(slot);
        return elapsed < SLOT_TOLERANCE;
    }

    bool shouldProduce(const StakingEngine& staking, bool bootstrap,
                       uint8_t peerCount = 0) const {
        uint32_t slot = getCurrentSlot();
        if (slot == lastProducedSlot) return false;
        if (!isWithinSlotWindow()) return false;

        // During bootstrap (epoch 0, only virtual founder in active set)
        // use hash-based turn to avoid two nodes producing the same block.
        if (bootstrap) {
            // Derive a per-node priority from selfId XOR slot.  Only the
            // node whose priority puts it in the first bucket produces.
            // This staggers production so nodes don't fork at the same height.
            uint8_t h = selfId.id[0] ^ selfId.id[1] ^ selfId.id[2]
                      ^ (uint8_t)(slot & 0xFF);
            // With peerCount nodes, each node gets ~1/N of the slots.
            // peerCount==0 means we're alone, always produce.
            if (peerCount > 0 && (h % (peerCount + 1)) != 0) return false;
            return true;
        }

        if (selfRole != ROLE_VALIDATOR) return false;
        if (!staking.isActiveValidator(selfId)) return false;
        return staking.isSlotProducer(selfId, slot);
    }

    uint32_t msUntilOurSlot(const StakingEngine& staking) const {
        if (staking.activeCount == 0) return UINT32_MAX;
        if (!staking.isActiveValidator(selfId)) return UINT32_MAX;

        uint32_t slot = getCurrentSlot();
        int selfIdx = staking.getValidatorIndex(selfId);
        if (selfIdx < 0) return UINT32_MAX;

        uint32_t currentProducer = slot % staking.activeCount;
        uint32_t slotsAway;
        if ((uint32_t)selfIdx >= currentProducer)
            slotsAway = selfIdx - currentProducer;
        else
            slotsAway = staking.activeCount - currentProducer + selfIdx;

        if (slotsAway == 0) slotsAway = staking.activeCount;

        uint32_t targetTime = getSlotStartTime(slot + slotsAway);
        uint32_t now = millis();
        return (targetTime > now) ? (targetTime - now) : 0;
    }

    void updateRole(const StakingEngine& staking) {
        if (staking.isActiveValidator(selfId)) {
            selfRole = ROLE_VALIDATOR;
        } else if (staking.findStake(selfId) != nullptr) {
            const StakeInfo* s = staking.findStake(selfId);
            if (s && s->isCandidate)
                selfRole = ROLE_CANDIDATE;
            else
                selfRole = ROLE_FULL;
        } else {
            selfRole = ROLE_LIGHT;
        }
    }

    bool isFinalized(uint32_t blockIndex, uint32_t chainHeight) const {
        return (chainHeight > blockIndex) &&
               (chainHeight - blockIndex) >= FINALITY_DEPTH;
    }

    uint32_t lastFinalizedBlock(uint32_t chainHeight) const {
        if (chainHeight <= FINALITY_DEPTH) return 0;
        return chainHeight - FINALITY_DEPTH;
    }

    void printStatus(const StakingEngine& staking) const {
        Serial.println("\n══════ Consensus ══════");
        Serial.printf("Role: %s | Slot: %d | Genesis: %ds ago\n",
                      roleName(selfRole), getCurrentSlot(),
                      (millis() - genesisTime) / 1000);

        uint32_t slot = getCurrentSlot();
        NodeID producer = staking.getSlotProducer(slot);
        Serial.printf("Current producer: %s %s\n",
                      producer.toShortStr().c_str(),
                      producer == selfId ? "(US)" : "");

        if (staking.isActiveValidator(selfId)) {
            Serial.printf("Next our slot in: %u ms\n",
                          msUntilOurSlot(staking));
        }

        Serial.printf("Missed slots: %d\n", missedSlots);
        Serial.println("═══════════════════════\n");
    }
};
