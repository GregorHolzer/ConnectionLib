#pragma once

#include <cstdint>
#include <vector>

namespace connectionlib {

/** @brief A backend/band's duty-cycle budget: at most maxDutyCyclePercent airtime per windowMs. */
struct DutyCycleConfig {
    float maxDutyCyclePercent;
    uint32_t windowMs;
};

/**
 * @brief Tracks recent airtime usage against a rolling window and admits/rejects further
 * transmissions accordingly. Backend-agnostic: every method takes the current time explicitly
 * (nowMs) rather than reading a clock itself, so it stays pure C++17 and host-testable with
 * synthetic clocks; a Transport backend supplies millis() when calling in.
 *
 * Does not handle millis()-style timer wraparound (~49 days) — acceptable for benchmark-run
 * timescales.
 */
class DutyCycleGuard {
  public:
    explicit DutyCycleGuard(DutyCycleConfig config);

    /** @brief Admission check: would a send of this estimated duration stay within budget right now? */
    [[nodiscard]] bool canTransmit(uint32_t nowMs, uint32_t estimatedDurationMs) const;

    /** @brief Ledger credit: call once a send has actually resolved, with its measured duration. */
    void recordTransmission(uint32_t nowMs, uint32_t actualDurationMs);

    [[nodiscard]] float currentDutyCyclePercent(uint32_t nowMs) const;

    /** @brief How long until a send of this estimated duration would be admitted; 0 if already allowed. */
    [[nodiscard]] uint32_t timeUntilNextAllowedMs(uint32_t nowMs, uint32_t estimatedDurationMs) const;

  private:
    struct Entry {
        uint32_t startTimeMs;
        uint32_t durationMs;
    };

    void pruneExpired(uint32_t nowMs) const;
    [[nodiscard]] uint32_t usedMsInWindow(uint32_t nowMs) const;
    [[nodiscard]] uint32_t allowedMsInWindow() const;

    DutyCycleConfig config_;
    mutable std::vector<Entry> entries_;
};

}   // namespace connectionlib
