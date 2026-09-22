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
        if (config_.captureRssi) {
            esp_wifi_set_promiscuous(false);
            esp_wifi_set_promiscuous_rx_cb(nullptr);
        }
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

    if (config_.captureRssi) {
        // Left enabled for the lifetime of the transport, unlike the transient enable above that
        // only exists to let esp_wifi_set_channel() work without an associated station. The filter
        // narrows the callback to data frames; management and control frames are far more numerous
        // on a busy channel and none of them can be an ESP-NOW payload.
        wifi_promiscuous_filter_t filter{};
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
        esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(onSniffTrampoline);
        esp_wifi_set_promiscuous(true);
    }

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
    // snrDb stays nullopt: 802.11 reports no per-packet SNR at all, on any core version. rssiDbm
    // is present only when EspNowConfig::captureRssi put a promiscuous sniffer alongside.
    if (queued.hasRssi) {
        msg.rssiDbm = static_cast<float>(queued.rssiDbm);
    }

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
