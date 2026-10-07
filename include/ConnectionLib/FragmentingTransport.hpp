#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "ConnectionLib/Transport.hpp"

namespace connectionlib {

/** @brief Limits for FragmentingTransport. The defaults suit a handful of peers on an ESP32. */
struct FragmentingConfig {
    size_t maxMessageBytes = 1024;       // largest message accepted for send and reassembly
    size_t maxPartialMessages = 4;       // reassembly slots (one per sending peer)
    size_t maxQueuedMessages = 4;        // send queue used while duty-cycle locked; 0 = no queuing
    uint32_t fragmentTimeoutMs = 5000;   // max wait for the next fragment of a message in progress
};

/**
 * @brief Opt-in wrapper that lets an application ignore the wrapped backend's packet size limit
 * and its duty-cycle lock. Wrap any Transport; the backend itself is untouched, so applications
 * that do not wrap keep the backend's native wire format and per-packet SendResult.
 *
 * Splitting: send() splits a payload into as many packets as the backend needs and listen()
 * returns it reassembled. Every packet carries a 3-byte header (msgId/fragIndex/fragCount) in
 * front of the application bytes, including messages that fit in one packet - so BOTH ends of a
 * link must use this wrapper. All-or-nothing with no retransmission: a message with a lost
 * fragment is never delivered. SendResult/ReceivedMessage describe the whole message (totalBytes
 * summed over its packets, acknowledged only if every packet was, the rest from the last packet).
 *
 * Queuing: when the backend is duty-cycle locked, send() keeps the message (up to
 * maxQueuedMessages) and returns SendStatus::Queued instead of DutyCycleExceeded. Queued messages
 * go out, in order, at the start of the next send() or listen() once the budget allows; those
 * calls then also block for that airtime. A queued message whose packet later fails for any
 * other reason is dropped without notice - there is no callback path to report it. The queue
 * lives in RAM: check queuedMessages() before deep sleep.
 *
 * Receiving: once the first packet of a message has arrived, listen() stays until the message is
 * complete, even past its own timeout, waiting up to fragmentTimeoutMs for each further packet.
 * If that runs out it returns std::nullopt but keeps the partial message, so a sender that was
 * duty-cycle locked part-way through can finish it during a later listen(). Packets must arrive
 * in order; the receiver has to call listen() often enough that the backend's own receive buffer
 * does not overflow between packets. listen() may return std::nullopt before its timeout if a
 * packet arrived that was not usable.
 *
 * Pure C++17 with no clock of its own: all waiting is delegated to the wrapped listen().
 */
class FragmentingTransport : public Transport {
  public:
    /** @brief inner must outlive this object and is still set up through setup(). */
    explicit FragmentingTransport(Transport &inner, FragmentingConfig config = {});

    bool setup() override;
    [[nodiscard]] int lastErrorCode() const override;
    SendResult send(const Address &destination, const uint8_t *payload, size_t len) override;
    std::optional<ReceivedMessage> listen(uint32_t timeoutMs) override;
    [[nodiscard]] Address broadcastAddress() const override;
    [[nodiscard]] size_t maxPayloadBytes() const override;
    [[nodiscard]] const char *technologyName() const override;
    [[nodiscard]] float currentDutyCyclePercent() const override;
    [[nodiscard]] uint32_t timeUntilNextAllowedMs(size_t payloadLen) const override;

    /** @brief Messages accepted with SendStatus::Queued that have not been fully sent yet. */
    [[nodiscard]] size_t queuedMessages() const;

  private:
    static constexpr size_t kHeaderBytes = 3;       // msgId, fragIndex, fragCount
    static constexpr size_t kMaxFragments = 255;    // fragCount is one byte

    struct QueuedMessage {
        Address destination;
        std::vector<uint8_t> payload;
        uint8_t msgId;
        uint8_t nextFragment;
    };

    struct PartialMessage {
        Address source;
        uint8_t msgId;
        uint8_t fragCount;
        uint8_t nextFragment;
        std::vector<uint8_t> payload;
        size_t totalBytes;
    };

    [[nodiscard]] size_t fragmentPayloadBytes() const;

    /** @brief Send fragments from nextFragment on; stops at, and reports, the first one that is not Ok. */
    SendResult sendFragments(const Address &destination, const uint8_t *payload, size_t len, uint8_t msgId,
                             uint8_t &nextFragment);
    void flushQueue();

    /** @brief Feed one received packet into reassembly; returns the message it completes, if any. */
    std::optional<ReceivedMessage> accept(const ReceivedMessage &packet);
    [[nodiscard]] bool hasPartialFrom(const Address &source) const;

    Transport &inner_;
    FragmentingConfig config_;
    uint8_t nextMsgId_ = 0;
    std::deque<QueuedMessage> queue_;
    std::vector<PartialMessage> partials_;   // oldest first
};

}   // namespace connectionlib
