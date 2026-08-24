#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "ConnectionLib/Address.hpp"
#include "ConnectionLib/LinkMetrics.hpp"

namespace connectionlib {

/**
 * @brief Which concrete Transport backend to use. A shared vocabulary type so application code
 * can select a backend via a runtime value (e.g. a config setting) instead of the choice being
 * implicit in which concrete class (EspNowTransport, LoRaTransport, ...) is written in source.
 *
 * There is deliberately no createTransport(ProtocolKind, ...) factory in this library: each
 * backend has its own Config type and header (LoRaTransport.hpp transitively pulls in RadioLib),
 * so a unified factory here would force every consumer to depend on every backend. The
 * `switch (kind) { ... }` dispatch belongs in application code, which already knows which
 * backends it has chosen to link in.
 */
enum class ProtocolKind : uint8_t { EspNow, LoRa, Zigbee };   // Zigbee: reserved, unimplemented

/**
 * @brief Technology-agnostic transport for a star-topology wireless sensor network. Application
 * code is written once against this interface; which concrete backend (ESP-NOW, LoRa, later
 * Zigbee) actually carries the traffic can change without touching the caller.
 *
 * Receive model is poll-based: send() blocks (bounded by an internal timeout) until the backend's
 * own definition of "done" resolves; poll() is non-blocking and should be called repeatedly from
 * the application's loop(). No async/callback path is exposed here, since not every backend can
 * honestly offer one (xbee-arduino's readPacket() is fundamentally polled).
 *
 * Never auto-chunks: if a payload exceeds maxPayloadBytes(), send() returns
 * SendStatus::PayloadTooLarge. Chunking/retry policy is an application-level decision.
 */
class Transport {
  public:
    /** @brief Initialize the underlying hardware using the config supplied at construction. @return true if ready. */
    virtual bool setup() = 0;

    /** @brief Backend-native error/status code from the last failed operation, for diagnostics. */
    [[nodiscard]] virtual int lastErrorCode() const = 0;

    /** @brief Send payload to destination. Blocks (bounded by an internal timeout) until resolved. */
    virtual SendResult send(const Address &destination, const uint8_t *payload, size_t len) = 0;

    /** @brief Non-blocking poll for one received message, if any is available right now. */
    virtual std::optional<ReceivedMessage> poll() = 0;

    [[nodiscard]] virtual Address broadcastAddress() const = 0;

    /** @brief Maximum application payload bytes this backend can carry in a single send(). */
    [[nodiscard]] virtual size_t maxPayloadBytes() const = 0;

    /** @brief e.g. "ESP-NOW", "LoRa-SX1278". */
    [[nodiscard]] virtual const char *technologyName() const = 0;

    [[nodiscard]] virtual float currentDutyCyclePercent() const = 0;

    /** @brief How long until a send() of payloadLen bytes would be admitted by the duty-cycle budget; 0 if already allowed. */
    [[nodiscard]] virtual uint32_t timeUntilNextAllowedMs(size_t payloadLen) const = 0;

    virtual ~Transport() = default;
};

}   // namespace connectionlib
