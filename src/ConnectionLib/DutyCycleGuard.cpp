/**
 * @brief Rolling-window airtime ledger backing the admission checks in DutyCycleGuard.hpp.
 *
 * Entries older than the window are pruned lazily, on read, so the guard needs no timer of its own
 * and stays host-testable against a synthetic clock.
 */

#include "ConnectionLib/DutyCycleGuard.hpp"

#include <algorithm>

namespace connectionlib {

DutyCycleGuard::DutyCycleGuard(DutyCycleConfig config) : config_(config) {}

void DutyCycleGuard::pruneExpired(uint32_t nowMs) const {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                   [&](const Entry &e) { return e.startTimeMs + config_.windowMs <= nowMs; }),
                    entries_.end());
}

uint32_t DutyCycleGuard::usedMsInWindow(uint32_t nowMs) const {
    pruneExpired(nowMs);
    uint32_t used = 0;
    for (const auto &e : entries_) {
        used += e.durationMs;
    }
    return used;
}

uint32_t DutyCycleGuard::allowedMsInWindow() const {
    return static_cast<uint32_t>(config_.windowMs * (config_.maxDutyCyclePercent / 100.0f));
}

bool DutyCycleGuard::canTransmit(uint32_t nowMs, uint32_t estimatedDurationMs) const {
    return usedMsInWindow(nowMs) + estimatedDurationMs <= allowedMsInWindow();
}

void DutyCycleGuard::recordTransmission(uint32_t nowMs, uint32_t actualDurationMs) {
    pruneExpired(nowMs);
    entries_.push_back({nowMs, actualDurationMs});
}

float DutyCycleGuard::currentDutyCyclePercent(uint32_t nowMs) const {
    if (config_.windowMs == 0) {
        return 0.0f;
    }
    return (static_cast<float>(usedMsInWindow(nowMs)) / static_cast<float>(config_.windowMs)) * 100.0f;
}

uint32_t DutyCycleGuard::timeUntilNextAllowedMs(uint32_t nowMs, uint32_t estimatedDurationMs) const {
    if (canTransmit(nowMs, estimatedDurationMs)) {
        return 0;
    }

    uint32_t used = usedMsInWindow(nowMs);
    uint32_t allowed = allowedMsInWindow();
    uint32_t needToFree = (used + estimatedDurationMs > allowed) ? (used + estimatedDurationMs - allowed) : 0;

    uint32_t freed = 0;
    for (const auto &e : entries_) {
        freed += e.durationMs;
        if (freed >= needToFree) {
            uint32_t expiresAt = e.startTimeMs + config_.windowMs;
            return (expiresAt > nowMs) ? (expiresAt - nowMs) : 0;
        }
    }

    // Freeing every tracked entry still wouldn't be enough (estimatedDurationMs alone exceeds the
    // budget) — fall back to a full window as a conservative "try again later" answer.
    return config_.windowMs;
}

}   // namespace connectionlib
