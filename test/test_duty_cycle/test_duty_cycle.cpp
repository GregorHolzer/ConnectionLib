#include <unity.h>

#include "ConnectionLib/DutyCycleGuard.hpp"

using namespace connectionlib;

void setUp() {}
void tearDown() {}

// 10% duty cycle over a 1000ms window -> 100ms of airtime allowed per window.
static DutyCycleConfig tenPercentOneSecond() {
    return DutyCycleConfig{10.0f, 1000};
}

void test_allows_within_budget() {
    DutyCycleGuard guard(tenPercentOneSecond());
    TEST_ASSERT_TRUE(guard.canTransmit(0, 50));
}

void test_rejects_over_budget() {
    DutyCycleGuard guard(tenPercentOneSecond());
    TEST_ASSERT_FALSE(guard.canTransmit(0, 150));
}

void test_recorded_transmission_consumes_budget() {
    DutyCycleGuard guard(tenPercentOneSecond());
    guard.recordTransmission(0, 60);
    // 60ms already used out of 100ms allowed -> 50ms more would exceed it.
    TEST_ASSERT_FALSE(guard.canTransmit(10, 50));
    // but 30ms more still fits.
    TEST_ASSERT_TRUE(guard.canTransmit(10, 30));
}

void test_entries_expire_out_of_window() {
    DutyCycleGuard guard(tenPercentOneSecond());
    guard.recordTransmission(0, 90);
    TEST_ASSERT_FALSE(guard.canTransmit(500, 50));   // still within the 1000ms window from t=0
    TEST_ASSERT_TRUE(guard.canTransmit(1001, 50));   // window has fully rolled past
}

void test_current_duty_cycle_percent() {
    DutyCycleGuard guard(tenPercentOneSecond());
    guard.recordTransmission(0, 50);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, guard.currentDutyCyclePercent(0));
}

void test_time_until_next_allowed_is_zero_when_allowed() {
    DutyCycleGuard guard(tenPercentOneSecond());
    TEST_ASSERT_EQUAL_UINT32(0, guard.timeUntilNextAllowedMs(0, 50));
}

void test_time_until_next_allowed_matches_expiry() {
    DutyCycleGuard guard(tenPercentOneSecond());
    guard.recordTransmission(0, 100);   // fully consumes the 100ms budget, expires at t=1000
    uint32_t wait = guard.timeUntilNextAllowedMs(0, 50);
    TEST_ASSERT_EQUAL_UINT32(1000, wait);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_allows_within_budget);
    RUN_TEST(test_rejects_over_budget);
    RUN_TEST(test_recorded_transmission_consumes_budget);
    RUN_TEST(test_entries_expire_out_of_window);
    RUN_TEST(test_current_duty_cycle_percent);
    RUN_TEST(test_time_until_next_allowed_is_zero_when_allowed);
    RUN_TEST(test_time_until_next_allowed_matches_expiry);
    return UNITY_END();
}
