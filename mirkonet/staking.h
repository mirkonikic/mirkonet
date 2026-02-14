#pragma once
#include "types.h"
#include "config.h"

#define MAX_VOTES 32

class StakingEngine {
public:
    StakeInfo   stakes[MAX_CANDIDATES];
    uint8_t     stakeCount = 0;

    VoteRecord  votes[MAX_VOTES];
    uint8_t     voteCount = 0;

    NodeID      activeSet[MAX_VALIDATORS];
    uint8_t     activeCount = 0;

    uint32_t    currentEpoch = 0;
    uint32_t    totalStaked = 0;
    uint32_t    totalSupply = 0;

    uint8_t stake(const NodeID& who, uint32_t amount,
                  uint32_t& balanceRef) {
        if (amount < MIN_STAKE) return 2;
        if (balanceRef < amount) return 1;

        StakeInfo* s = findStake(who);
        if (!s) {
            if (stakeCount >= MAX_CANDIDATES) return 3;
            s = &stakes[stakeCount++];
            memset(s, 0, sizeof(StakeInfo));
            s->staker = who;
            s->used = true;
        }

        balanceRef -= amount;
        s->stakedAmount += amount;
        s->isCandidate = true;
        totalStaked += amount;

        Serial.printf("[Stake] %s staked %u tokens (total: %u)\n",
                      who.toShortStr().c_str(), amount, s->stakedAmount);
        return 0;
    }

    uint8_t unstake(const NodeID& who, uint32_t amount, uint32_t currentBlock) {
        StakeInfo* s = findStake(who);
        if (!s || !s->isCandidate) return 1;
        if (amount > s->stakedAmount) return 2;

        s->stakedAmount -= amount;
        s->unstakingAmount += amount;
        s->unstakeBlock = currentBlock;
        totalStaked -= amount;

        if (s->stakedAmount < MIN_STAKE) {
            s->isCandidate = false;
            removeVotesFor(who);
        }

        Serial.printf("[Stake] %s unstaking %u tokens (cooldown: %d blocks)\n",
                      who.toShortStr().c_str(), amount, UNSTAKE_COOLDOWN);
        return 0;
    }

    uint32_t claim(const NodeID& who, uint32_t currentBlock) {
        StakeInfo* s = findStake(who);
        if (!s || s->unstakingAmount == 0) return 0;

        if (currentBlock - s->unstakeBlock < UNSTAKE_COOLDOWN) return 0;

        uint32_t amount = s->unstakingAmount;
        s->unstakingAmount = 0;
        s->unstakeBlock = 0;

        if (s->stakedAmount == 0 && !s->isCandidate) {
            s->used = false;
        }

        Serial.printf("[Stake] %s claimed %u tokens\n",
                      who.toShortStr().c_str(), amount);
        return amount;
    }

    uint8_t vote(const NodeID& voter, const NodeID& candidate) {
        StakeInfo* voterStake = findStake(voter);
        if (!voterStake || voterStake->stakedAmount == 0) return 1;

        StakeInfo* candStake = findStake(candidate);
        if (!candStake || !candStake->isCandidate) return 2;

        for (int i = 0; i < voteCount; i++) {
            if (votes[i].used && votes[i].voter == voter) {
                removeVote(voter);
                break;
            }
        }

        if (voteCount >= MAX_VOTES) return 4;

        VoteRecord& v = votes[voteCount++];
        v.voter = voter;
        v.candidate = candidate;
        v.weight = voterStake->stakedAmount;
        v.used = true;

        recalcVotes(candidate);

        Serial.printf("[Vote] %s → %s (weight: %u)\n",
                      voter.toShortStr().c_str(),
                      candidate.toShortStr().c_str(),
                      v.weight);
        return 0;
    }

    void removeVote(const NodeID& voter) {
        for (int i = 0; i < voteCount; i++) {
            if (votes[i].used && votes[i].voter == voter) {
                NodeID oldCandidate = votes[i].candidate;
                votes[i].used = false;
                compactVotes();
                recalcVotes(oldCandidate);
                return;
            }
        }
    }

    void runElection(uint32_t epoch) {
        currentEpoch = epoch;

        for (int i = 0; i < voteCount; i++) {
            if (!votes[i].used) continue;
            StakeInfo* vs = findStake(votes[i].voter);
            votes[i].weight = vs ? vs->stakedAmount : 0;
        }

        for (int i = 0; i < stakeCount; i++) {
            if (stakes[i].used && stakes[i].isCandidate) {
                recalcVotes(stakes[i].staker);
            }
        }

        int indices[MAX_CANDIDATES];
        int n = 0;
        for (int i = 0; i < stakeCount; i++) {
            if (stakes[i].used && stakes[i].isCandidate)
                indices[n++] = i;
        }

        for (int i = 0; i < n - 1; i++) {
            for (int j = i + 1; j < n; j++) {
                if (stakes[indices[j]].votesReceived > stakes[indices[i]].votesReceived) {
                    int tmp = indices[i]; indices[i] = indices[j]; indices[j] = tmp;
                } else if (stakes[indices[j]].votesReceived == stakes[indices[i]].votesReceived) {
                    if (stakes[indices[j]].stakedAmount > stakes[indices[i]].stakedAmount) {
                        int tmp = indices[i]; indices[i] = indices[j]; indices[j] = tmp;
                    }
                }
            }
        }

        uint8_t newCount = min((int)MAX_VALIDATORS, n);

        for (int i = 0; i < n; i++) {
            StakeInfo& s = stakes[indices[i]];
            if (s.votesReceived == 0) {
                s.votesReceived = s.stakedAmount;
            }
        }

        for (int i = 0; i < n - 1; i++) {
            for (int j = i + 1; j < n; j++) {
                if (stakes[indices[j]].votesReceived > stakes[indices[i]].votesReceived) {
                    int tmp = indices[i]; indices[i] = indices[j]; indices[j] = tmp;
                }
            }
        }

        newCount = min((int)MAX_VALIDATORS, n);
        activeCount = newCount;
        for (int i = 0; i < newCount; i++) {
            activeSet[i] = stakes[indices[i]].staker;
        }

        for (int i = 0; i < activeCount - 1; i++) {
            for (int j = i + 1; j < activeCount; j++) {
                if (activeSet[j].hash32() < activeSet[i].hash32()) {
                    NodeID tmp = activeSet[i];
                    activeSet[i] = activeSet[j];
                    activeSet[j] = tmp;
                }
            }
        }

        Serial.printf("\n[Election] Epoch %u — %d validators elected:\n", epoch, activeCount);
        for (int i = 0; i < activeCount; i++) {
            StakeInfo* s = findStake(activeSet[i]);
            Serial.printf("  [%d] %s | stake=%u votes=%u\n",
                          i, activeSet[i].toShortStr().c_str(),
                          s ? s->stakedAmount : 0,
                          s ? s->votesReceived : 0);
        }
        Serial.println();
    }

    void addGenesisValidator(const NodeID& who, uint32_t stakeAmount) {
        if (stakeCount >= MAX_CANDIDATES) return;

        StakeInfo& s = stakes[stakeCount++];
        memset(&s, 0, sizeof(StakeInfo));
        s.staker = who;
        s.stakedAmount = stakeAmount;
        s.isCandidate = true;
        s.votesReceived = stakeAmount;
        s.used = true;
        totalStaked += stakeAmount;

        if (activeCount < MAX_VALIDATORS) {
            activeSet[activeCount++] = who;
        }
    }

    StakeInfo* findStake(const NodeID& who) {
        for (int i = 0; i < stakeCount; i++)
            if (stakes[i].used && stakes[i].staker == who)
                return &stakes[i];
        return nullptr;
    }

    const StakeInfo* findStake(const NodeID& who) const {
        for (int i = 0; i < stakeCount; i++)
            if (stakes[i].used && stakes[i].staker == who)
                return &stakes[i];
        return nullptr;
    }

    bool isActiveValidator(const NodeID& who) const {
        for (int i = 0; i < activeCount; i++)
            if (activeSet[i] == who) return true;
        return false;
    }

    int getValidatorIndex(const NodeID& who) const {
        for (int i = 0; i < activeCount; i++)
            if (activeSet[i] == who) return i;
        return -1;
    }

    NodeID getSlotProducer(uint32_t slot) const {
        if (activeCount == 0) return ZERO_NODE;
        return activeSet[slot % activeCount];
    }

    bool isSlotProducer(const NodeID& who, uint32_t slot) const {
        if (activeCount == 0) return false;
        return activeSet[slot % activeCount] == who;
    }

    void printStatus() const {
        Serial.println("\n══════ DPoS Status ══════");
        Serial.printf("Epoch: %u | Total staked: %u | Supply: %u\n",
                      currentEpoch, totalStaked, totalSupply);

        Serial.println("\nActive Validators:");
        for (int i = 0; i < activeCount; i++) {
            const StakeInfo* s = findStake(activeSet[i]);
            Serial.printf("  [%d] %s | stake=%u votes=%u\n",
                          i, activeSet[i].toString().c_str(),
                          s ? s->stakedAmount : 0,
                          s ? s->votesReceived : 0);
        }

        Serial.println("\nAll Candidates:");
        for (int i = 0; i < stakeCount; i++) {
            if (!stakes[i].used) continue;
            bool active = isActiveValidator(stakes[i].staker);
            Serial.printf("  %s | stake=%u votes=%u unstaking=%u %s%s\n",
                          stakes[i].staker.toString().c_str(),
                          stakes[i].stakedAmount,
                          stakes[i].votesReceived,
                          stakes[i].unstakingAmount,
                          stakes[i].isCandidate ? "CANDIDATE " : "",
                          active ? "★ACTIVE" : "");
        }

        Serial.println("\nVotes:");
        for (int i = 0; i < voteCount; i++) {
            if (!votes[i].used) continue;
            Serial.printf("  %s → %s (weight %u)\n",
                          votes[i].voter.toShortStr().c_str(),
                          votes[i].candidate.toShortStr().c_str(),
                          votes[i].weight);
        }
        Serial.println("=======================\n");
    }

private:
    void recalcVotes(const NodeID& candidate) {
        StakeInfo* s = findStake(candidate);
        if (!s) return;
        uint32_t total = 0;
        for (int i = 0; i < voteCount; i++) {
            if (votes[i].used && votes[i].candidate == candidate)
                total += votes[i].weight;
        }
        s->votesReceived = total;
    }

    void removeVotesFor(const NodeID& candidate) {
        for (int i = 0; i < voteCount; i++) {
            if (votes[i].used && votes[i].candidate == candidate)
                votes[i].used = false;
        }
        compactVotes();
    }

    void compactVotes() {
        int w = 0;
        for (int r = 0; r < voteCount; r++) {
            if (votes[r].used) {
                if (w != r) votes[w] = votes[r];
                w++;
            }
        }
        voteCount = w;
    }
};
