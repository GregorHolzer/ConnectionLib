#include "ConnectionLib/backends/zigbee/ZigbeeTransport.hpp"

#if defined(ESP32) && defined(CONNECTIONLIB_ENABLE_ZIGBEE)

#include <Arduino.h>

namespace connectionlib {

ZigbeeTransport::ZigbeeTransport(ZigbeeConfig config) : config_(config), dutyCycle_(config_.dutyCycle) {}

bool ZigbeeTransport::setup() {
    serial_.begin(config_.baud, SERIAL_8N1, config_.pins.rx, config_.pins.tx);
    xbee_.begin(serial_);
    return true;   // xbee-arduino has no explicit init handshake to fail; a bad wiring only
                    // surfaces later as send()/poll() never seeing a response.
}

int ZigbeeTransport::lastErrorCode() const {
    return lastError_;
}

SendResult ZigbeeTransport::send(const Address &destination, const uint8_t *payload, size_t len) {
    SendResult result;
    result.payloadBytes = len;

    auto destXBee = destination.asXBee();
    if (!destXBee) {
        result.status = SendStatus::Failed;
        lastError_ = -1;
        return result;
    }

    if (len > maxPayloadBytes()) {
        result.status = SendStatus::PayloadTooLarge;
        return result;
    }

    uint32_t nowMs = millis();
    if (!dutyCycle_.canTransmit(nowMs, 1)) {
        result.status = SendStatus::DutyCycleExceeded;
        return result;
    }

    XBeeAddress64 addr64(destXBee->addr64);
    ZBTxRequest request(addr64, const_cast<uint8_t *>(payload), static_cast<uint8_t>(len));

    uint32_t t0 = micros();
    xbee_.send(request);

    // Wait for the ZB TX Status frame specifically - other frames (e.g. an incoming RX) may
    // arrive first and get skipped over without consuming the whole wait budget on them.
    bool acknowledged = false;
    bool statusSeen = false;
    uint32_t waitStart = millis();
    while (millis() - waitStart < kTxStatusWaitMs) {
        int remaining = static_cast<int>(kTxStatusWaitMs - (millis() - waitStart));
        if (remaining <= 0 || !xbee_.readPacket(remaining)) {
            break;
        }
        if (xbee_.getResponse().getApiId() == ZB_TX_STATUS_RESPONSE) {
            ZBTxStatusResponse txStatus;
            xbee_.getResponse().getZBTxStatusResponse(txStatus);
            acknowledged = txStatus.isSuccess();
            statusSeen = true;
            break;
        }
    }
    uint32_t t1 = micros();

    result.timestampMicros = t1;
    result.totalBytes = len;
    result.acknowledged = acknowledged;

    uint32_t elapsedMs = (t1 - t0) / 1000;
    dutyCycle_.recordTransmission(millis(), elapsedMs);

    result.status = statusSeen ? (acknowledged ? SendStatus::Ok : SendStatus::Failed) : SendStatus::Timeout;
    return result;
}

std::optional<ReceivedMessage> ZigbeeTransport::poll() {
    xbee_.readPacket();   // true non-blocking form: returns immediately either way
    if (!xbee_.getResponse().isAvailable()) {
        return std::nullopt;
    }
    if (xbee_.getResponse().getApiId() != ZB_RX_RESPONSE) {
        return std::nullopt;   // e.g. a stray TX status frame - not a received message
    }

    ZBRxResponse rx;
    xbee_.getResponse().getZBRxResponse(rx);

    ReceivedMessage msg;
    msg.source = Address::xbee(rx.getRemoteAddress64().get(), rx.getRemoteAddress16());
    msg.payload.assign(rx.getData(), rx.getData() + rx.getDataLength());
    msg.timestampMicros = micros();
    msg.totalBytes = rx.getDataLength();
    // rssiDbm/snrDb left nullopt: the ZB RX frame doesn't carry signal quality - a separate `DB`
    // AT query would be needed after the fact, not implemented in this pass.
    return msg;
}

Address ZigbeeTransport::broadcastAddress() const {
    return Address::broadcast(AddressKind::XBee64_16);
}

size_t ZigbeeTransport::maxPayloadBytes() const {
    return kMaxPayload;
}

const char *ZigbeeTransport::technologyName() const {
    return "Zigbee-XBeeS2C";
}

float ZigbeeTransport::currentDutyCyclePercent() const {
    return dutyCycle_.currentDutyCyclePercent(millis());
}

uint32_t ZigbeeTransport::timeUntilNextAllowedMs(size_t payloadLen) const {
    (void)payloadLen;
    return dutyCycle_.timeUntilNextAllowedMs(millis(), 1);
}

}   // namespace connectionlib

#endif   // defined(ESP32) && defined(CONNECTIONLIB_ENABLE_ZIGBEE)
