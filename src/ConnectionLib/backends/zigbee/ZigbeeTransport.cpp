#include "ConnectionLib/backends/zigbee/ZigbeeTransport.hpp"

#if defined(ESP32) && defined(CONNECTIONLIB_ENABLE_ZIGBEE)

#include <Arduino.h>

#include <utility>

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

    // Pass the peer's 16-bit address through rather than using xbee-arduino's shorter
    // ZBTxRequest(addr64, payload, len) constructor, which hardcodes addr16 to
    // ZB_BROADCAST_ADDRESS (0xFFFE) = "unknown, go discover it" and so discards the addr16 that
    // Address::xbee() carries and poll() fills in from the sender. Measured against hardware
    // both forms deliver, so this is about not throwing away known information rather than a
    // correctness fix. Broadcast is unaffected: Address::broadcast(XBee64_16) already sets 0xFFFE.
    XBeeAddress64 addr64(destXBee->addr64);
    ZBTxRequest request(addr64, destXBee->addr16, ZB_BROADCAST_RADIUS_MAX_HOPS, ZB_TX_UNICAST,
                        const_cast<uint8_t *>(payload), static_cast<uint8_t>(len), DEFAULT_FRAME_ID);

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
        uint8_t apiId = xbee_.getResponse().getApiId();
        if (apiId == ZB_TX_STATUS_RESPONSE) {
            ZBTxStatusResponse txStatus;
            xbee_.getResponse().getZBTxStatusResponse(txStatus);
            acknowledged = txStatus.isSuccess();
            statusSeen = true;
            break;
        }
        if (apiId == ZB_RX_RESPONSE && !pendingRx_) {
            // Hold it for the next poll() rather than dropping it. The peer's reply commonly
            // lands inside this window, since a broadcast status takes ~1s to resolve.
            pendingRx_ = buildReceived();
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

ReceivedMessage ZigbeeTransport::buildReceived() {
    ZBRxResponse rx;
    xbee_.getResponse().getZBRxResponse(rx);

    ReceivedMessage msg;
    msg.source = Address::xbee(rx.getRemoteAddress64().get(), rx.getRemoteAddress16());
    msg.payload.assign(rx.getData(), rx.getData() + rx.getDataLength());
    msg.timestampMicros = micros();
    msg.totalBytes = rx.getDataLength();
    // snrDb stays nullopt: the module reports no SNR through any AT command. rssiDbm costs a
    // UART round trip, so it happens only when asked for.
    if (config_.captureRssi) {
        msg.rssiDbm = queryLastRssi();
    }
    return msg;
}

std::optional<float> ZigbeeTransport::queryLastRssi() {
    // `DB` returns a single byte: the *magnitude* of the last received packet's RSSI in dBm, so
    // the sign has to be put back. It reflects whichever packet arrived most recently, which is
    // why this is issued here, immediately after building that packet, rather than batched.
    uint8_t command[] = {'D', 'B'};
    AtCommandRequest request(command);
    xbee_.send(request);

    uint32_t waitStart = millis();
    while (millis() - waitStart < kRssiQueryWaitMs) {
        int remaining = static_cast<int>(kRssiQueryWaitMs - (millis() - waitStart));
        if (remaining <= 0 || !xbee_.readPacket(remaining)) {
            break;
        }
        if (xbee_.getResponse().getApiId() != AT_COMMAND_RESPONSE) {
            // Anything else arriving mid-query is abandoned rather than stashed: recursing into
            // buildReceived() from inside a receive is not worth the reentrancy, and a dropped
            // frame here shows up honestly as a delivery failure rather than as a silent repair.
            continue;
        }
        AtCommandResponse response;
        xbee_.getResponse().getAtCommandResponse(response);
        if (response.isOk() && response.getValueLength() >= 1) {
            return -static_cast<float>(response.getValue()[0]);
        }
        break;
    }
    return std::nullopt;
}

std::optional<ReceivedMessage> ZigbeeTransport::poll() {
    if (pendingRx_) {   // arrived while send() was waiting for its TX status
        ReceivedMessage msg = std::move(*pendingRx_);
        pendingRx_.reset();
        return msg;
    }

    xbee_.readPacket();   // true non-blocking form: returns immediately either way
    if (!xbee_.getResponse().isAvailable()) {
        return std::nullopt;
    }
    if (xbee_.getResponse().getApiId() != ZB_RX_RESPONSE) {
        return std::nullopt;   // e.g. a stray TX status frame - not a received message
    }
    return buildReceived();
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
