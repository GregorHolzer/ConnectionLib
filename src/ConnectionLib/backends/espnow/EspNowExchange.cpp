/**
 * @brief EspNowTransport's data path: send(), poll(), and the three static C callbacks that drive
 * both. Split out of EspNowTransport.cpp, which keeps the lifecycle and configuration half.
 *
 * The callbacks live here rather than with the receive path alone because one of them belongs to
 * the *send* side: onDataSentTrampoline sets the sendDone_ flag that send() blocks on. All three
 * run in WiFi task context, not the caller's, which is why they do as little as possible and hand
 * off through the lock-free queue and the atomic flags declared in the header.
 */

#include "ConnectionLib/backends/espnow/EspNowTransport.hpp"

#if defined(ESP32) && defined(CONNECTIONLIB_ENABLE_ESPNOW)

#include <algorithm>

#include <Arduino.h>
#include <esp_wifi.h>

namespace connectionlib {

SendResult EspNowTransport::send(const Address &destination, const uint8_t *payload, size_t len) {
    SendResult result;
    result.payloadBytes = len;

    auto mac = destination.asMac();
    if (!mac) {
        result.status = SendStatus::Failed;
        lastError_ = -1;
        return result;
    }

    if (len > maxPayloadBytes()) {
        result.status = SendStatus::PayloadTooLarge;
        return result;
    }

    // ESP-NOW airtime is sub-millisecond at its native rate; round up to a conservative 1ms for
    // the pre-flight admission estimate. Actual accounting after the send uses measured time.
    uint32_t nowMs = millis();
    if (!dutyCycle_.canTransmit(nowMs, 1)) {
        result.status = SendStatus::DutyCycleExceeded;
        return result;
    }

    sendDone_ = false;
    sendStatus_ = ESP_NOW_SEND_FAIL;

    uint32_t t0 = micros();
    esp_err_t err = esp_now_send(mac->data(), payload, len);
    if (err != ESP_OK) {
        lastError_ = err;
        result.status = SendStatus::Failed;
        return result;
    }

    uint32_t deadline = millis() + config_.sendTimeoutMs;
    while (!sendDone_ && millis() < deadline) {
        delay(1);
    }
    uint32_t t1 = micros();

    result.timestampMicros = t1;
    result.totalBytes = len;

    uint32_t elapsedMs = (t1 - t0) / 1000;
    dutyCycle_.recordTransmission(millis(), elapsedMs);

    if (!sendDone_) {
        result.status = SendStatus::Timeout;
        result.acknowledged = false;
        return result;
    }

    result.acknowledged = (sendStatus_ == ESP_NOW_SEND_SUCCESS);
    result.status = result.acknowledged ? SendStatus::Ok : SendStatus::Failed;
    return result;
}

std::optional<ReceivedMessage> EspNowTransport::poll() {
    uint8_t tail = queueTail_.load(std::memory_order_relaxed);
    uint8_t head = queueHead_.load(std::memory_order_acquire);
    if (tail == head) {
        return std::nullopt;
    }

    const QueuedMessage &queued = queue_[tail];
    ReceivedMessage msg;
    msg.source = Address::mac(queued.mac);
    msg.payload.assign(queued.data.begin(), queued.data.begin() + queued.len);
    msg.timestampMicros = queued.timestampMicros;
    msg.totalBytes = queued.len;
    // snrDb stays nullopt: 802.11 reports no per-packet SNR at all, on any core version. rssiDbm
    // is present only when EspNowConfig::captureRssi put a promiscuous sniffer alongside.
    if (queued.hasRssi) {
        msg.rssiDbm = static_cast<float>(queued.rssiDbm);
    }

    queueTail_.store(static_cast<uint8_t>((tail + 1) % kQueueCapacity), std::memory_order_release);
    return msg;
}

void EspNowTransport::onSniffTrampoline(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (activeInstance_ == nullptr || buf == nullptr || type != WIFI_PKT_DATA) {
        return;
    }
    // 802.11 data frame: addr2 (the transmitter) sits at offset 10 of the MAC header. Reading only
    // the header keeps this callback short - it runs in WiFi task context, where anything slow
    // shows up as jitter in the very measurements this exists to annotate.
    auto *packet = static_cast<wifi_promiscuous_pkt_t *>(buf);
    if (packet->rx_ctrl.sig_len < 16) {
        return;
    }
    const uint8_t *source = packet->payload + 10;
    std::copy(source, source + 6, activeInstance_->sniffMac_.begin());
    activeInstance_->sniffRssiDbm_ = static_cast<int8_t>(packet->rx_ctrl.rssi);
    activeInstance_->sniffValid_ = true;
}

void EspNowTransport::onDataRecvTrampoline(const uint8_t *mac, const uint8_t *data, int len) {
    if (activeInstance_ == nullptr || len <= 0) {
        return;
    }
    activeInstance_->handleRecv(mac, data, static_cast<size_t>(len));
}

void EspNowTransport::onDataSentTrampoline(const uint8_t *mac, esp_now_send_status_t status) {
    (void)mac;
    if (activeInstance_ == nullptr) {
        return;
    }
    activeInstance_->sendStatus_ = status;
    activeInstance_->sendDone_ = true;
}

void EspNowTransport::handleRecv(const uint8_t *mac, const uint8_t *data, size_t len) {
    uint8_t head = queueHead_.load(std::memory_order_relaxed);
    uint8_t nextHead = static_cast<uint8_t>((head + 1) % kQueueCapacity);
    if (nextHead == queueTail_.load(std::memory_order_acquire)) {
        return;   // queue full, drop the message
    }

    QueuedMessage &slot = queue_[head];
    std::copy(mac, mac + 6, slot.mac.begin());
    size_t copyLen = std::min(len, kMaxPayload);
    std::copy(data, data + copyLen, slot.data.begin());
    slot.len = static_cast<uint8_t>(copyLen);
    slot.timestampMicros = micros();
    // Only trust the sniffed value when it came from the same station as this ESP-NOW frame. The
    // sniffer sees every data frame on the channel, so without the match a neighbouring network's
    // RSSI could be attached to our packet - a wrong number being worse than no number.
    slot.hasRssi = false;
    if (config_.captureRssi && sniffValid_ && std::equal(sniffMac_.begin(), sniffMac_.end(), mac)) {
        slot.hasRssi = true;
        slot.rssiDbm = sniffRssiDbm_;
        sniffValid_ = false;
    }

    queueHead_.store(nextHead, std::memory_order_release);
}

}   // namespace connectionlib

#endif   // defined(ESP32)
