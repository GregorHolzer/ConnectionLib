#pragma once

#include <cstdint>

#include "ConnectionLib/DutyCycleGuard.hpp"
#include "ConnectionLib/Transport.hpp"

// This backend depends on xbee-arduino and Arduino's HardwareSerial. Guarded behind an opt-in
// macro, not just `defined(ESP32)`, for the same reason as the other backends (see
// LoRaTransport.hpp): PlatformIO compiles a dependency's entire src/ tree once it's pulled in at
// all. Consumers that want this backend must both add `-DCONNECTIONLIB_ENABLE_ZIGBEE` to
// build_flags and add xbee-arduino to lib_deps.
#if defined(ESP32) && defined(CONNECTIONLIB_ENABLE_ZIGBEE)

#include <HardwareSerial.h>
#include <XBee.h>

namespace connectionlib {

/** @brief UART pin mapping to the XBee module. Defaults settled for a T-Beam v1.2 wiring. */
struct ZigbeePinConfig {
    int rx = 13;   // ESP32 RX <- XBee DOUT
    int tx = 25;   // ESP32 TX <- XBee DIN
};

/** @brief Declarative setup for ZigbeeTransport. */
struct ZigbeeConfig {
    ZigbeePinConfig pins{};
    uint32_t baud = 9600;   // XBee factory default
    DutyCycleConfig dutyCycle{100.0f, 3'600'000};   // 2.4GHz ISM: no ETSI duty-cycle mandate
};

/**
 * @brief Zigbee backend, wrapping the Digi XBee/XBee-PRO S2C module (ZB firmware) via
 * andrewrapp/xbee-arduino's API-mode driver over UART. The module itself does the network/mesh
 * layer - this class only frames ZB TX/RX requests, it never touches AT configuration.
 *
 * Ack semantics: unlike LoRa, this is a real network-layer (APS) ack. ZBTxStatusResponse reports
 * whether the destination actually received the frame (with mesh retries already applied by the
 * module itself), so SendResult::acknowledged is genuinely meaningful here, not a placeholder.
 *
 * poll() is genuinely non-blocking: xbee-arduino's readPacket() (no-argument form) returns
 * immediately whether or not a frame is waiting - matches the Transport contract exactly.
 *
 * Prerequisite this class cannot satisfy for you: two factory-default XBee S2C modules do not
 * form a network together (both default to Router). One module - conventionally whichever one
 * calls send() as the initiator - must be set to Coordinator (AT command CE=1) via XCTU first.
 * This is one-time module configuration, not something ConnectionLib does at runtime.
 */
class ZigbeeTransport : public Transport {
  public:
    explicit ZigbeeTransport(ZigbeeConfig config);

    ZigbeeTransport(const ZigbeeTransport &) = delete;
    ZigbeeTransport &operator=(const ZigbeeTransport &) = delete;

    bool setup() override;
    [[nodiscard]] int lastErrorCode() const override;
    SendResult send(const Address &destination, const uint8_t *payload, size_t len) override;
    std::optional<ReceivedMessage> poll() override;
    [[nodiscard]] Address broadcastAddress() const override;
    [[nodiscard]] size_t maxPayloadBytes() const override;
    [[nodiscard]] const char *technologyName() const override;
    [[nodiscard]] float currentDutyCyclePercent() const override;
    [[nodiscard]] uint32_t timeUntilNextAllowedMs(size_t payloadLen) const override;

  private:
    static constexpr size_t kMaxPayload = 84;         // XBee S2C ZB firmware unfragmented RF payload limit
    static constexpr uint32_t kTxStatusWaitMs = 500;   // how long to wait for the delivery ack after a send

    ZigbeeConfig config_;
    DutyCycleGuard dutyCycle_;
    HardwareSerial serial_{2};
    XBee xbee_;
    int lastError_ = 0;
};

}   // namespace connectionlib

#endif   // defined(ESP32)
