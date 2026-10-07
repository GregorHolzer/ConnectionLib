#pragma once

#include <cstdint>
#include <optional>

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
    // Same reasoning as EspNowConfig: ETSI EN 300 328 imposes no duty-cycle limit, and its 10%
    // medium-utilisation cap applies only to non-adaptive equipment - 802.15.4 uses CSMA/CA.
    DutyCycleConfig dutyCycle{100.0f, 3'600'000};
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
 * listen() polls xbee-arduino's readPacket() (no-argument form), which returns immediately
 * whether or not a frame is waiting, so listen(0) is a true non-blocking check.
 *
 * REQUIRES ZB (ZigBee) FIRMWARE. XBee S2C hardware also ships with 802.15.4 firmware, which
 * implements a completely different API frame set (transmit 0x00/0x01 with status 0x89, receive
 * 0x80/0x81) and simply ignores the ZB frames used here. Check AT VR: 4xxx is ZB, 2xxx is
 * 802.15.4. The symptom of the mismatch is deceptive - AT command frames are still answered, so
 * the module looks alive and correctly configured, but every send() returns SendStatus::Timeout
 * because no status frame is ever emitted. Confirmed against hardware; see docs/hardware.md.
 *
 * Prerequisites this class cannot satisfy for you - one-time module configuration, not something
 * ConnectionLib does at runtime. All of them fail the same way, as a send() timeout on a link
 * that looks correctly configured, so check them in this order:
 *   - Antennas fitted. These modules are the U.FL variant and radiate essentially nothing
 *     without one, however close together they sit.
 *   - AP=2, NOT AP=1. xbee-arduino escapes 0x7E/0x7D/0x11/0x13 on every transmitted byte
 *     unconditionally, i.e. it assumes escaped API mode. An AP=1 module silently discards any
 *     frame containing one of those, with no status frame and no error. It presents as
 *     intermittent rather than broken: broadcasts to 0x...FFFF contain no escapable byte and
 *     work, while every unicast fails, since all Digi addresses begin 0013A200.
 *   - CE=1 on exactly one module - conventionally whichever calls send() as the initiator.
 *     Every module defaults to Router, and two Routers never form a network.
 *   - ID (PAN id) set explicitly; the default 0 pairs non-deterministically.
 * AP/CE/ID are all settable over the module's own UART from the host MCU (from AP=0 via +++ and
 * ATAP2/ATCE1/ATID/ATWR/ATCN; from AP!=0 via 0x08 AT-command frames, since command mode no
 * longer exists). XCTU is only needed for changing the firmware itself.
 *
 * See docs/hardware.md for the full bring-up procedure and the evidence behind each of these.
 */
class ZigbeeTransport : public Transport {
  public:
    explicit ZigbeeTransport(ZigbeeConfig config);

    ZigbeeTransport(const ZigbeeTransport &) = delete;
    ZigbeeTransport &operator=(const ZigbeeTransport &) = delete;

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
    static constexpr size_t kMaxPayload = 84;         // XBee S2C ZB firmware unfragmented RF payload limit
    // How long to wait for the delivery ack after a send. Measured on hardware: the first
    // transmission of a session takes >500ms while the stack sets up a route, every one after
    // it takes ~1ms. At the previous 500ms budget that made packet #1 of every run report
    // Timeout on a perfectly good link.
    static constexpr uint32_t kTxStatusWaitMs = 1500;

    /** @brief Parse the frame currently held by xbee_ as a ReceivedMessage. */
    ReceivedMessage buildReceived();

    /** @brief Non-blocking: return the held-over message or one RX frame, if any is waiting. */
    std::optional<ReceivedMessage> receiveOnce();

    ZigbeeConfig config_;
    DutyCycleGuard dutyCycle_;
    HardwareSerial serial_{2};
    XBee xbee_;
    int lastError_ = 0;

    // A message that arrived while send() was waiting for its TX status, held over for the next
    // listen(). Without this the reply to a just-sent request is routinely lost: a ZB broadcast
    // status takes ~1s to come back, which is ample time for the peer to answer, and send()
    // would otherwise discard that frame while scanning for its own status.
    // Single-slot on purpose: one in-flight request/response is the pattern this library
    // targets. A second message arriving inside the same send() window is still dropped.
    std::optional<ReceivedMessage> pendingRx_;
};

}   // namespace connectionlib

#endif   // defined(ESP32)
