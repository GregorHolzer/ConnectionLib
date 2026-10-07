#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "ConnectionLib/DutyCycleGuard.hpp"
#include "ConnectionLib/Transport.hpp"

// This backend depends on the Arduino-ESP32 core's esp_now.h. Guarded behind an opt-in macro, not
// just `defined(ESP32)`, for the same reason as LoRaTransport (see its header): PlatformIO
// compiles a dependency's entire src/ tree once pulled in at all, so a LoRa-only consumer would
// otherwise still get ESP-NOW's code (and flash footprint) compiled in unasked. Consumers that
// want this backend add `-DCONNECTIONLIB_ENABLE_ESPNOW` to build_flags.
#if defined(ESP32) && defined(CONNECTIONLIB_ENABLE_ESPNOW)

#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace connectionlib {

/** @brief Declarative setup for EspNowTransport. */
struct EspNowConfig {
    std::vector<Address> peers;   // Address::mac(...) entries to register as ESP-NOW peers
    uint8_t wifiChannel = 0;      // 0 = use whatever channel WiFi is already on
    bool encrypt = false;
    uint32_t sendTimeoutMs = 2000;   // matches sensor-system's existing ESP-NOW ack-wait timeout
    // 2.4GHz ISM under ETSI EN 300 328: no duty-cycle limit applies. The standard's 10%
    // medium-utilisation cap binds only *non-adaptive* equipment, and ESP-NOW inherits 802.11's
    // CSMA/CA, so it qualifies as adaptive and is exempt. See docs/measurement-methodology.md.
    DutyCycleConfig dutyCycle{100.0f, 3'600'000};
};

/**
 * @brief ESP-NOW backend. Wraps esp_now.h directly (Arduino-ESP32 core), matching sensor-system's
 * existing client-side transport. Peer registration, WiFi/esp_now_init sequencing, and the
 * ack-wait timeout stay internal; send()/listen() surface the technology-agnostic Transport interface.
 *
 * Only one EspNowTransport may be active at a time per device — esp_now's C callback API has no
 * user-data parameter, so this class routes callbacks through a single static instance pointer,
 * matching sensor-system's own single-instance-per-device usage.
 */
class EspNowTransport : public Transport {
  public:
    explicit EspNowTransport(EspNowConfig config);
    ~EspNowTransport() override;

    EspNowTransport(const EspNowTransport &) = delete;
    EspNowTransport &operator=(const EspNowTransport &) = delete;

    bool setup() override;
    [[nodiscard]] int lastErrorCode() const override;
    SendResult send(const Address &destination, const uint8_t *payload, size_t len) override;
    std::optional<ReceivedMessage> listen(uint32_t timeoutMs) override;
    [[nodiscard]] Address broadcastAddress() const override;
    [[nodiscard]] size_t maxPayloadBytes() const override;
    [[nodiscard]] const char *technologyName() const override;
    [[nodiscard]] float currentDutyCyclePercent() const override;
    [[nodiscard]] uint32_t timeUntilNextAllowedMs(size_t payloadLen) const override;

  private:
    static constexpr size_t kMaxPayload = 250;   // ESP-NOW's native single-packet payload limit
    static constexpr size_t kQueueCapacity = 8;

    struct QueuedMessage {
        std::array<uint8_t, 6> mac{};
        std::array<uint8_t, kMaxPayload> data{};
        uint8_t len = 0;
        uint32_t timestampMicros = 0;
    };

    static void onDataRecvTrampoline(const uint8_t *mac, const uint8_t *data, int len);
    static void onDataSentTrampoline(const uint8_t *mac, esp_now_send_status_t status);
    void handleRecv(const uint8_t *mac, const uint8_t *data, size_t len);

    /** @brief Non-blocking: pop one message off the receive queue, if any. */
    std::optional<ReceivedMessage> receiveOnce();

    EspNowConfig config_;
    DutyCycleGuard dutyCycle_;
    int lastError_ = 0;

    // SPSC ring buffer: producer is the recv callback (WiFi task context), consumer is listen()
    // (caller's loop() context). Indices are atomics so no FreeRTOS mutex is needed for this
    // single-producer/single-consumer case.
    std::array<QueuedMessage, kQueueCapacity> queue_{};
    std::atomic<uint8_t> queueHead_{0};
    std::atomic<uint8_t> queueTail_{0};

    // Set by the send callback, which then gives sendSignal_. send() blocks on that semaphore
    // (bounded by sendTimeoutMs), so it returns when the result is in rather than on the next
    // RTOS tick: a 1 ms polling loop here used to add up to a millisecond to every send.
    SemaphoreHandle_t sendSignal_ = nullptr;
    volatile bool sendDone_ = false;
    volatile esp_now_send_status_t sendStatus_ = ESP_NOW_SEND_FAIL;

    static EspNowTransport *activeInstance_;
};

}   // namespace connectionlib

#endif   // defined(ESP32)
