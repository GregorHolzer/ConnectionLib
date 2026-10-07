#include "ConnectionLib/backends/lora/LoRaTransport.hpp"

#if defined(ESP32) && defined(CONNECTIONLIB_ENABLE_LORA)

#include <algorithm>
#include <cstring>
#include <utility>

#include <Arduino.h>

namespace connectionlib {

LoRaTransport::LoRaTransport(LoRaConfig config)
    : config_(config),
      dutyCycle_(config_.dutyCycle),
      spi_(HSPI),
      radio_(new Module(config_.pins.nss, config_.pins.dio0, config_.pins.reset, RADIOLIB_NC, spi_)) {}

bool LoRaTransport::setup() {
    spi_.begin(config_.pins.sck, config_.pins.miso, config_.pins.mosi, config_.pins.nss);

    int16_t state = radio_.begin(config_.frequencyMHz, config_.bandwidthKHz, config_.spreadingFactor,
                                  config_.codingRate, RADIOLIB_SX127X_SYNC_WORD, config_.txPowerDbm,
                                  config_.preambleLength);
    lastError_ = state;
    if (state != RADIOLIB_ERR_NONE) {
        return false;
    }

    // The available-to-read check in listen() is a plain digitalRead() of DIO0, which the SX1278
    // maps to RxDone by default while in continuous-receive mode — no interrupt handler needed.
    state = radio_.startReceive();
    lastError_ = state;
    return state == RADIOLIB_ERR_NONE;
}

int LoRaTransport::lastErrorCode() const {
    return lastError_;
}

SendResult LoRaTransport::send(const Address &destination, const uint8_t *payload, size_t len) {
    SendResult result;
    result.payloadBytes = len;
    result.acknowledged = false;   // raw SX1278 has no MAC-layer ack; see CONNECTIONLIB_LORA_ACK below

    auto destNodeId = destination.asLoRaNodeId();
    if (!destNodeId) {
        result.status = SendStatus::Failed;
        lastError_ = -1;
        return result;
    }

    if (len > maxPayloadBytes()) {
        result.status = SendStatus::PayloadTooLarge;
        return result;
    }

    size_t totalLen = sizeof(FrameHeader) + len;
    uint32_t estimatedMs = static_cast<uint32_t>(radio_.getTimeOnAir(totalLen) / 1000);
    uint32_t nowMs = millis();
    if (!dutyCycle_.canTransmit(nowMs, estimatedMs)) {
        result.status = SendStatus::DutyCycleExceeded;
        return result;
    }

    uint8_t buffer[kMaxRadioPacket];
    FrameHeader header{*destNodeId, config_.nodeId, seq_++, static_cast<uint8_t>(len)};
    std::memcpy(buffer, &header, sizeof(FrameHeader));
    std::memcpy(buffer + sizeof(FrameHeader), payload, len);

    uint32_t t0 = micros();
    int16_t state = radio_.transmit(buffer, totalLen);
    uint32_t t1 = micros();

    // transmit() takes the radio out of receive mode; resume receiving for the listen()-side check.
    radio_.startReceive();

    result.timestampMicros = t1;
    result.totalBytes = totalLen;

    uint32_t elapsedMs = (t1 - t0) / 1000;
    dutyCycle_.recordTransmission(millis(), elapsedMs);

    lastError_ = state;
    result.status = (state == RADIOLIB_ERR_NONE) ? SendStatus::Ok : SendStatus::Failed;

#if defined(CONNECTIONLIB_LORA_ACK)
    // Broadcasts are not acked: every receiver would answer at once and the acks would collide.
    if (state == RADIOLIB_ERR_NONE && *destNodeId != 0xFFFF) {
        result.status = SendStatus::Timeout;
        uint32_t waitStart = millis();
        while (millis() - waitStart < config_.ackTimeoutMs) {
            FrameHeader received;
            std::optional<ReceivedMessage> frame = readFrame(received);
            if (!frame) {
                delay(1);
                continue;
            }
            if ((received.flags & kAckFlag) == 0) {
                // Someone else's data, not our ack: keep it for the next listen() rather than drop it.
                if (!pendingRx_) {
                    pendingRx_ = PendingFrame{received, std::move(*frame)};
                }
                continue;
            }
            if (received.srcNodeId == *destNodeId && received.seq == header.seq) {
                result.acknowledged = true;
                result.status = SendStatus::Ok;
                break;
            }
        }
    }
#endif
    return result;
}

std::optional<ReceivedMessage> LoRaTransport::listen(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (true) {
        if (auto msg = receiveOnce()) {
            return msg;
        }
        if (millis() - start >= timeoutMs) {
            return std::nullopt;
        }
        delay(1);
    }
}

std::optional<ReceivedMessage> LoRaTransport::receiveOnce() {
    FrameHeader header;
#if defined(CONNECTIONLIB_LORA_ACK)
    std::optional<ReceivedMessage> msg;
    if (pendingRx_) {
        header = pendingRx_->header;
        msg = std::move(pendingRx_->message);
        pendingRx_.reset();
    } else {
        msg = readFrame(header);
    }
    if (!msg || (header.flags & kAckFlag) != 0) {
        return std::nullopt;   // nothing, or a stray ack for a send() that already gave up
    }
    if (header.destNodeId == config_.nodeId) {
        sendAck(header);
    }
    return msg;
#else
    return readFrame(header);
#endif
}

#if defined(CONNECTIONLIB_LORA_ACK)
void LoRaTransport::sendAck(const FrameHeader &received) {
    uint32_t estimatedMs = static_cast<uint32_t>(radio_.getTimeOnAir(sizeof(FrameHeader)) / 1000);
    if (!dutyCycle_.canTransmit(millis(), estimatedMs)) {
        return;   // an ack is airtime like any other frame; the sender will report Timeout
    }

    uint8_t buffer[sizeof(FrameHeader)];
    FrameHeader ack{received.srcNodeId, config_.nodeId, received.seq, 0, kAckFlag};
    std::memcpy(buffer, &ack, sizeof(FrameHeader));

    uint32_t t0 = micros();
    radio_.transmit(buffer, sizeof(FrameHeader));
    uint32_t t1 = micros();
    radio_.startReceive();
    dutyCycle_.recordTransmission(millis(), (t1 - t0) / 1000);
}
#endif

std::optional<ReceivedMessage> LoRaTransport::readFrame(FrameHeader &header) {
    if (digitalRead(config_.pins.dio0) != HIGH) {
        return std::nullopt;
    }

    size_t receivedLen = radio_.getPacketLength();
    if (receivedLen == 0 || receivedLen > kMaxRadioPacket) {
        radio_.startReceive();
        return std::nullopt;
    }

    uint8_t buffer[kMaxRadioPacket];
    int16_t state = radio_.readData(buffer, receivedLen);
    float rssi = radio_.getRSSI();
    float snr = radio_.getSNR();
    radio_.startReceive();

    if (state != RADIOLIB_ERR_NONE) {
        lastError_ = state;
        return std::nullopt;
    }
    if (receivedLen < sizeof(FrameHeader)) {
        return std::nullopt;
    }

    std::memcpy(&header, buffer, sizeof(FrameHeader));

    if (header.destNodeId != config_.nodeId && header.destNodeId != 0xFFFF) {
        return std::nullopt;   // not addressed to us
    }

    ReceivedMessage msg;
    msg.source = Address::loRaNode(header.srcNodeId);
    size_t payloadLen = std::min<size_t>(header.payloadLen, receivedLen - sizeof(FrameHeader));
    msg.payload.assign(buffer + sizeof(FrameHeader), buffer + sizeof(FrameHeader) + payloadLen);
    msg.timestampMicros = micros();
    msg.totalBytes = receivedLen;
    msg.rssiDbm = rssi;
    msg.snrDb = snr;
    return msg;
}

Address LoRaTransport::broadcastAddress() const {
    return Address::loRaNode(0xFFFF);
}

size_t LoRaTransport::maxPayloadBytes() const {
    return kMaxRadioPacket - sizeof(FrameHeader);
}

const char *LoRaTransport::technologyName() const {
    return "LoRa-SX1278";
}

float LoRaTransport::currentDutyCyclePercent() const {
    return dutyCycle_.currentDutyCyclePercent(millis());
}

uint32_t LoRaTransport::timeUntilNextAllowedMs(size_t payloadLen) const {
    uint32_t estimatedMs = static_cast<uint32_t>(radio_.getTimeOnAir(sizeof(FrameHeader) + payloadLen) / 1000);
    return dutyCycle_.timeUntilNextAllowedMs(millis(), estimatedMs);
}

}   // namespace connectionlib

#endif   // defined(ESP32)
