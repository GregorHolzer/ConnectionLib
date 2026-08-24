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

/** @brief Declarative setup for LoRaTransport. */
struct LoRaConfig {
    LoRaPinConfig pins{};
    float frequencyMHz = 433.0f;   // EU-conform SX1278 variant (see docs/hardware.md)
    float bandwidthKHz = 125.0f;
    uint8_t spreadingFactor = 9;
    uint8_t codingRate = 7;
    int8_t txPowerDbm = 17;
    uint16_t preambleLength = 8;
    uint16_t nodeId = 0;   // this node's software address; 0xFFFF is reserved as broadcast
    // PLACEHOLDER: exact ETSI duty-cycle % for the 433MHz sub-band is unverified — see
    // docs/architecture.md "Open risks" #2. Do not treat as regulatory fact.
    DutyCycleConfig dutyCycle{1.0f, 3'600'000};
};

/**
 * @brief LoRa backend, wrapping RadioLib's SX1278 driver. Raw SX1278 point-to-point has no
 * PHY-layer addressing, so this class prepends a thin software header (destNodeId/srcNodeId/
 * seq/payloadLen) before every transmit and filters on it in poll() — backend-internal, never
 * exposed through the Transport interface.
 *
 * Ack semantics: fire-and-forget. acknowledged is always false in SendResult — raw SX1278 has no
 * MAC-layer ack; PDR/reliability is a future benchmark app's job via its own sequence numbers.
 *
 * poll() is genuinely non-blocking: setup() puts the radio into continuous receive via
 * startReceive(), and poll() just checks the DIO0 pin (which the SX1278 maps to RxDone by
 * default) before touching RadioLib at all.
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
    std::optional<ReceivedMessage> poll() override;
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
    };

    static constexpr size_t kMaxRadioPacket = 255;   // SX1278/RadioLib single-packet limit

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
