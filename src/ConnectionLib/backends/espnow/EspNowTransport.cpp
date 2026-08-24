#include "ConnectionLib/backends/espnow/EspNowTransport.hpp"

#if defined(ESP32) && defined(CONNECTIONLIB_ENABLE_ESPNOW)

#include <algorithm>

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace connectionlib {

EspNowTransport *EspNowTransport::activeInstance_ = nullptr;

EspNowTransport::EspNowTransport(EspNowConfig config) : config_(std::move(config)), dutyCycle_(config_.dutyCycle) {}

EspNowTransport::~EspNowTransport() {
    if (activeInstance_ == this) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
        activeInstance_ = nullptr;
    }
}

bool EspNowTransport::setup() {
    activeInstance_ = this;

    WiFi.mode(WIFI_STA);

    if (config_.wifiChannel != 0) {
        // Set the ESP-NOW channel without associating to an AP: briefly enable promiscuous mode,
        // which is the standard workaround for esp_wifi_set_channel() otherwise requiring a
        // connected station interface.
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(config_.wifiChannel, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        lastError_ = err;
        return false;
    }

    esp_now_register_recv_cb(onDataRecvTrampoline);
    esp_now_register_send_cb(onDataSentTrampoline);

    for (const Address &peer : config_.peers) {
        auto mac = peer.asMac();
        if (!mac) {
            continue;   // non-MAC address in the peer list is a config error; skip rather than fail setup()
        }
        esp_now_peer_info_t peerInfo{};
        std::copy(mac->begin(), mac->end(), peerInfo.peer_addr);
        peerInfo.channel = config_.wifiChannel;
        peerInfo.encrypt = config_.encrypt;
        peerInfo.ifidx = WIFI_IF_STA;

        err = esp_now_add_peer(&peerInfo);
        if (err != ESP_OK) {
            lastError_ = err;
            return false;
        }
    }

    return true;
}

int EspNowTransport::lastErrorCode() const {
    return lastError_;
}

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
    // rssiDbm/snrDb left nullopt: esp_now.h doesn't natively expose per-packet RSSI/SNR.

    queueTail_.store(static_cast<uint8_t>((tail + 1) % kQueueCapacity), std::memory_order_release);
    return msg;
}

Address EspNowTransport::broadcastAddress() const {
    return Address::broadcast(AddressKind::Mac6);
}

size_t EspNowTransport::maxPayloadBytes() const {
    return kMaxPayload;
}

const char *EspNowTransport::technologyName() const {
    return "ESP-NOW";
}

float EspNowTransport::currentDutyCyclePercent() const {
    return dutyCycle_.currentDutyCyclePercent(millis());
}

uint32_t EspNowTransport::timeUntilNextAllowedMs(size_t payloadLen) const {
    (void)payloadLen;
    return dutyCycle_.timeUntilNextAllowedMs(millis(), 1);
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

    queueHead_.store(nextHead, std::memory_order_release);
}

}   // namespace connectionlib

#endif   // defined(ESP32)
