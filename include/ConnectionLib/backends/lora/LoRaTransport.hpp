#pragma once

#include <cstdint>

#include "ConnectionLib/DutyCycleGuard.hpp"
#include "ConnectionLib/Transport.hpp"

// This backend depends on RadioLib and the Arduino SPI class. Guarded behind an opt-in macro, not
// just `defined(ESP32)`: PlatformIO compiles a dependency's *entire* src/ tree once it's pulled
// in at all, regardless of which headers a consumer actually #include's — so an ESP-NOW-only
// consumer building for ESP32 would otherwise still hit "RadioLib.h: No such file or directory"
// compiling this file, even though it never uses LoRaTransport. Consumers that want this backend
// must both add `-DCONNECTIONLIB_ENABLE_LORA` to build_flags and add RadioLib to lib_deps.
#if defined(ESP32) && defined(CONNECTIONLIB_ENABLE_LORA)

#include <RadioLib.h>
#include <SPI.h>

namespace connectionlib {

/** @brief SPI pin mapping for the SX1278. Defaults to the LilyGO T-Beam v1.1 (classic) pinout. */
struct LoRaPinConfig {
    int sck = 5;
    int miso = 19;
    int mosi = 27;
    int nss = 18;
    int reset = 23;
    int dio0 = 26;
};

/**
 * @brief Declarative setup for LoRaTransport.
 *
 * The radio defaults are the EU-legal operating point for this band, not arbitrary starting
 * values: ETSI EN 300 220-2 / CEPT ERC-REC 70-03 Annex 1 band h1.3 allows 433.050-434.790MHz at
 * at most 10mW e.r.p. with a duty cycle of at most 10% across the whole band. They are held fixed
 * across every measurement run — see docs/measurement-methodology.md "Fixed radio parameters".
 */
struct LoRaConfig {
    LoRaPinConfig pins{};
    // 433.2, not 433.0: with 125kHz of occupied bandwidth a 433.0 centre spans ~432.94-433.06MHz,
    // which falls below the 433.050MHz band edge. 433.2 sits clear of both edges.
    float frequencyMHz = 433.2f;
    float bandwidthKHz = 125.0f;
    uint8_t spreadingFactor = 9;
    uint8_t codingRate = 7;
    // 7dBm conducted, not 17: the limit is 10mW (10dBm) *e.r.p.*, so a ~2dBi antenna leaves ~9dBm
    // radiated. 17dBm would be 50mW, five times the legal ceiling.
    int8_t txPowerDbm = 7;
    uint16_t preambleLength = 8;
    uint16_t nodeId = 0;   // this node's software address; 0xFFFF is reserved as broadcast
    // 10% per hour = 360s of airtime, the h1.3 limit above. Previously 1%, which was an
    // unverified placeholder and roughly ten times stricter than the band actually requires.
    DutyCycleConfig dutyCycle{10.0f, 3'600'000};
#if defined(CONNECTIONLIB_LORA_ACK)
    // How long send() waits for the receiver's ack frame. Has to cover the ack's own airtime
    // (~140ms at the defaults above) plus the receiver's turnaround; raise it for higher SFs.
    uint32_t ackTimeoutMs = 1000;
#endif
};

/**
 * @brief LoRa backend, wrapping RadioLib's SX1278 driver. Raw SX1278 point-to-point has no
 * PHY-layer addressing, so this class prepends a thin software header (destNodeId/srcNodeId/
 * seq/payloadLen) before every transmit and filters on it in listen() — backend-internal, never
 * exposed through the Transport interface.
 *
 * Ack semantics: fire-and-forget by default. acknowledged is always false in SendResult — raw
 * SX1278 has no MAC-layer ack; PDR/reliability is a future benchmark app's job via its own
 * sequence numbers.
 *
 * Building with `-DCONNECTIONLIB_LORA_ACK` adds a software ack instead: listen() answers every
 * unicast frame it returns with a header-only ack frame, and send() waits up to
 * LoRaConfig::ackTimeoutMs for it, reporting Ok/acknowledged or SendStatus::Timeout. There are no
 * retransmissions. Things to know before enabling it:
 *   - It changes the wire format (the header grows by a flags byte, 6 -> 7), so every node on
 *     the link must be built with the same setting.
 *   - Broadcasts are never acked, so a broadcast send() behaves as without the option.
 *   - The receiver only acks from inside listen(), so the sender times out unless the peer is
 *     listening at that moment.
 *   - Acks are airtime: they count against the receiver's duty-cycle budget and are skipped,
 *     not delayed, when that budget is exhausted.
 *   - A data frame that arrives while send() is waiting for its ack is held (one slot) for the
 *     next listen() and only acked then, which may be after its sender has given up.
 *
 * Receiving needs no interrupt handler: setup() puts the radio into continuous receive via
 * startReceive(), and listen() just checks the DIO0 pin (which the SX1278 maps to RxDone by
 * default) before touching RadioLib at all. The radio is half-duplex and holds a single packet:
 * nothing is received during this node's own send(), and a packet not collected by listen()
 * before the next one arrives is lost.
 *
 * On boards where the SX1278 is behind a gated power rail (e.g. the T-Beam's AXP2101 PMU, which
 * powers the radio through its ALDO2 channel), the application must enable that rail before
 * calling setup() — otherwise it fails with RadioLib's RADIOLIB_ERR_CHIP_NOT_FOUND, since the
 * radio can't be reached over SPI at all. This is board bring-up, not this backend's job; see
 * examples/lora_gateway and examples/lora_satellite for the T-Beam's specific sequence.
 */
class LoRaTransport : public Transport {
  public:
    explicit LoRaTransport(LoRaConfig config);

    LoRaTransport(const LoRaTransport &) = delete;
    LoRaTransport &operator=(const LoRaTransport &) = delete;

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
    struct __attribute__((packed)) FrameHeader {
        uint16_t destNodeId;
        uint16_t srcNodeId;
        uint8_t seq;
        uint8_t payloadLen;
#if defined(CONNECTIONLIB_LORA_ACK)
        uint8_t flags;   // kAckFlag marks an ack frame
#endif
    };

    static constexpr size_t kMaxRadioPacket = 255;   // SX1278/RadioLib single-packet limit

    /** @brief Non-blocking: read one frame addressed to this node, if RxDone is signalled. */
    std::optional<ReceivedMessage> readFrame(FrameHeader &header);

    /** @brief Non-blocking: one received message, if any (acking it under CONNECTIONLIB_LORA_ACK). */
    std::optional<ReceivedMessage> receiveOnce();

#if defined(CONNECTIONLIB_LORA_ACK)
    static constexpr uint8_t kAckFlag = 0x01;

    struct PendingFrame {
        FrameHeader header;
        ReceivedMessage message;
    };

    void sendAck(const FrameHeader &received);

    // A data frame that arrived while send() was waiting for its ack, held for the next listen().
    std::optional<PendingFrame> pendingRx_;
#endif

    LoRaConfig config_;
    DutyCycleGuard dutyCycle_;
    SPIClass spi_;
    // mutable: getTimeOnAir() (a RadioLib query, not a state mutation from this class's own
    // perspective) is non-const in RadioLib, but timeUntilNextAllowedMs() needs to call it from a
    // const context to satisfy the Transport interface's const method.
    mutable SX1278 radio_;
    int lastError_ = 0;
    uint8_t seq_ = 0;
};

}   // namespace connectionlib

#endif   // defined(ESP32)
