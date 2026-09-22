/**
 * @brief EspNowTransport's lifecycle and configuration: construction, teardown, setup() and the
 * fixed-property accessors. The data path - send(), poll() and the C callbacks - is in
 * EspNowExchange.cpp.
 *
 * Only one instance can be live at a time. esp_now.h hands its callbacks no user pointer, so the
 * class keeps a static activeInstance_ for the trampolines to route through; the destructor
 * unregisters everything it registered, which is what lets a benchmark destroy a transport and
 * construct a differently-configured one in its place.
 */

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

}   // namespace connectionlib

#endif   // defined(ESP32)
