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
 * REQUIRES ZB (ZigBee) FIRMWARE. XBee S2C hardware also ships with 802.15.4 firmware, which
 * implements a completely different API frame set (transmit 0x00/0x01 with status 0x89, receive
 * 0x80/0x81) and simply ignores the ZB frames used here. Check AT VR: 4xxx is ZB, 2xxx is
 * 802.15.4. The symptom of the mismatch is deceptive - AT command frames are still answered, so
 * the module looks alive and correctly configured, but every send() returns SendStatus::Timeout
 * because no status frame is ever emitted. Confirmed against hardware; see docs/hardware.md.
 *
 * Prerequisites this class cannot satisfy for you - one-time module configuration, not something
 * ConnectionLib does at runtime:
 *   - AP=1 (API mode). Factory default is 0 (Transparent), in which the module would transmit
 *     these API frames as raw payload: no ack, no error, silent failure.
 *   - CE=1 on exactly one module - conventionally whichever calls send() as the initiator.
 *     Every module defaults to Router, and two Routers never form a network.
 * Both can be set over the module's own UART from the host MCU (+++, then ATAP1/ATCE1/ATWR/ATCN);
 * XCTU is not required, which matters when working headless.
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
