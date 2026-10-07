#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ConnectionLib/Address.hpp"

namespace connectionlib {

/** @brief Fields common to both a completed send and a received message. */
struct LinkMetrics {
    uint32_t timestampMicros = 0;   // when tx-complete/rx was observed
    size_t totalBytes = 0;          // actual bytes on the wire/air, incl. backend framing
    std::optional<float> rssiDbm;   // nullopt where the backend/hardware doesn't expose it
    std::optional<float> snrDb;     // nullopt where the backend/hardware doesn't expose it
};

/** @brief Outcome of a Transport::send() call. */
enum class SendStatus : uint8_t { Ok, Timeout, DutyCycleExceeded, PayloadTooLarge, NotReady, Failed, Queued };

/** @brief Result of a Transport::send() call, for both the caller and a future benchmark app. */
struct SendResult : LinkMetrics {
    SendStatus status = SendStatus::Failed;
    size_t payloadBytes = 0;    // application payload bytes requested (subset of totalBytes)
    bool acknowledged = false;  // backend-defined; see each backend's docs for what this means
};

/** @brief One message returned from Transport::listen(). */
struct ReceivedMessage : LinkMetrics {
    Address source;
    std::vector<uint8_t> payload;
};

}   // namespace connectionlib
