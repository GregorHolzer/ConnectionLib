#include "ConnectionLib/FragmentingTransport.hpp"

#include <algorithm>
#include <utility>

namespace connectionlib {

FragmentingTransport::FragmentingTransport(Transport &inner, FragmentingConfig config)
    : inner_(inner), config_(config) {}

bool FragmentingTransport::setup() {
    return inner_.setup();
}

int FragmentingTransport::lastErrorCode() const {
    return inner_.lastErrorCode();
}

size_t FragmentingTransport::fragmentPayloadBytes() const {
    size_t innerMax = inner_.maxPayloadBytes();
    return innerMax > kHeaderBytes ? innerMax - kHeaderBytes : 0;
}

SendResult FragmentingTransport::sendFragments(const Address &destination, const uint8_t *payload, size_t len,
                                               uint8_t msgId, uint8_t &nextFragment) {
    size_t perFragment = fragmentPayloadBytes();
    // An empty payload still travels as one (header-only) packet.
    size_t fragCount = len == 0 ? 1 : (len + perFragment - 1) / perFragment;

    SendResult result;
    result.status = SendStatus::Ok;
    result.payloadBytes = len;
    result.acknowledged = true;

    std::vector<uint8_t> packet;
    while (nextFragment < fragCount) {
        size_t offset = nextFragment * perFragment;
        size_t chunk = std::min(perFragment, len - offset);
        packet.assign({msgId, nextFragment, static_cast<uint8_t>(fragCount)});
        packet.insert(packet.end(), payload + offset, payload + offset + chunk);

        SendResult sent = inner_.send(destination, packet.data(), packet.size());
        result.status = sent.status;
        if (sent.status != SendStatus::Ok) {
            result.acknowledged = false;
            return result;
        }
        result.totalBytes += sent.totalBytes;
        result.acknowledged = result.acknowledged && sent.acknowledged;
        result.timestampMicros = sent.timestampMicros;
        result.rssiDbm = sent.rssiDbm;
        result.snrDb = sent.snrDb;
        ++nextFragment;
    }
    return result;
}

void FragmentingTransport::flushQueue() {
    while (!queue_.empty()) {
        QueuedMessage &msg = queue_.front();
        SendResult result =
            sendFragments(msg.destination, msg.payload.data(), msg.payload.size(), msg.msgId, msg.nextFragment);
        if (result.status == SendStatus::DutyCycleExceeded) {
            return;   // still locked; the backend rejected without spending airtime
        }
        queue_.pop_front();   // fully sent, or failed for another reason and dropped
    }
}

SendResult FragmentingTransport::send(const Address &destination, const uint8_t *payload, size_t len) {
    flushQueue();

    SendResult result;
    result.payloadBytes = len;
    if (len > maxPayloadBytes()) {
        result.status = SendStatus::PayloadTooLarge;
        return result;
    }

    uint8_t msgId = nextMsgId_++;
    uint8_t nextFragment = 0;
    // Queue behind anything still waiting, to keep messages in order.
    bool locked = !queue_.empty() || inner_.timeUntilNextAllowedMs(len) != 0;
    if (!locked) {
        result = sendFragments(destination, payload, len, msgId, nextFragment);
        if (result.status != SendStatus::DutyCycleExceeded) {
            return result;
        }
    }

    if (queue_.size() >= config_.maxQueuedMessages) {
        result.status = SendStatus::DutyCycleExceeded;
        return result;
    }
    queue_.push_back({destination, std::vector<uint8_t>(payload, payload + len), msgId, nextFragment});
    result.status = SendStatus::Queued;
    return result;
}

std::optional<ReceivedMessage> FragmentingTransport::listen(uint32_t timeoutMs) {
    flushQueue();

    uint32_t waitMs = timeoutMs;
    while (true) {
        std::optional<ReceivedMessage> packet = inner_.listen(waitMs);
        if (!packet) {
            return std::nullopt;
        }
        if (auto message = accept(*packet)) {
            return message;
        }
        if (!hasPartialFrom(packet->source)) {
            return std::nullopt;   // packet was not usable; no clock here to resume the caller's wait
        }
        waitMs = config_.fragmentTimeoutMs;   // a message is arriving: stay until it is complete
    }
}

std::optional<ReceivedMessage> FragmentingTransport::accept(const ReceivedMessage &packet) {
    if (packet.payload.size() < kHeaderBytes) {
        return std::nullopt;
    }
    uint8_t msgId = packet.payload[0];
    uint8_t fragIndex = packet.payload[1];
    uint8_t fragCount = packet.payload[2];
    if (fragCount == 0 || fragIndex >= fragCount) {
        return std::nullopt;
    }

    auto slot = std::find_if(partials_.begin(), partials_.end(),
                             [&](const PartialMessage &p) { return p.source == packet.source; });
    bool sameMessage = slot != partials_.end() && slot->msgId == msgId && slot->fragCount == fragCount;

    if (sameMessage && fragIndex < slot->nextFragment) {
        return std::nullopt;   // duplicate of a fragment already taken
    }
    if (!sameMessage || fragIndex != slot->nextFragment) {
        // A different message from this source, or a gap: whatever was in progress is lost.
        if (slot != partials_.end()) {
            partials_.erase(slot);
        }
        if (fragIndex != 0) {
            return std::nullopt;
        }
        if (partials_.size() >= config_.maxPartialMessages) {
            if (partials_.empty()) {
                return std::nullopt;
            }
            partials_.erase(partials_.begin());
        }
        partials_.push_back({packet.source, msgId, fragCount, 0, {}, 0});
        slot = partials_.end() - 1;
    }

    size_t bodyLen = packet.payload.size() - kHeaderBytes;
    if (slot->payload.size() + bodyLen > config_.maxMessageBytes) {
        partials_.erase(slot);
        return std::nullopt;
    }
    slot->payload.insert(slot->payload.end(), packet.payload.begin() + kHeaderBytes, packet.payload.end());
    slot->totalBytes += packet.totalBytes;
    ++slot->nextFragment;
    if (slot->nextFragment < slot->fragCount) {
        return std::nullopt;
    }

    ReceivedMessage message = packet;   // source and link metrics of the last fragment
    message.payload = std::move(slot->payload);
    message.totalBytes = slot->totalBytes;
    partials_.erase(slot);
    return message;
}

bool FragmentingTransport::hasPartialFrom(const Address &source) const {
    return std::any_of(partials_.begin(), partials_.end(),
                       [&](const PartialMessage &p) { return p.source == source; });
}

Address FragmentingTransport::broadcastAddress() const {
    return inner_.broadcastAddress();
}

size_t FragmentingTransport::maxPayloadBytes() const {
    return std::min(config_.maxMessageBytes, fragmentPayloadBytes() * kMaxFragments);
}

const char *FragmentingTransport::technologyName() const {
    return inner_.technologyName();
}

float FragmentingTransport::currentDutyCyclePercent() const {
    return inner_.currentDutyCyclePercent();
}

uint32_t FragmentingTransport::timeUntilNextAllowedMs(size_t payloadLen) const {
    return inner_.timeUntilNextAllowedMs(payloadLen);
}

size_t FragmentingTransport::queuedMessages() const {
    return queue_.size();
}

}   // namespace connectionlib
