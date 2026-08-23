// ESP-NOW satellite example: sends a small counter payload to the gateway every 2s.
// Flash espnow_gateway first, read its MAC address off Serial, and put it in kGatewayMac below.

#include <Arduino.h>

#include <ConnectionLib/backends/espnow/EspNowTransport.hpp>

using namespace connectionlib;

// TODO: replace with your gateway's MAC address (printed by espnow_gateway on boot).
static const std::array<uint8_t, 6> kGatewayMac = {0x24, 0x6F, 0x28, 0x00, 0x00, 0x00};
static const Address gateway = Address::mac(kGatewayMac);

EspNowConfig makeConfig() {
    EspNowConfig cfg;
    cfg.peers.push_back(gateway);
    return cfg;
}

EspNowConfig config = makeConfig();
EspNowTransport transport(config);
uint32_t counter = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);

    if (!transport.setup()) {
        Serial.printf("EspNowTransport::setup() failed, lastErrorCode=%d\n", transport.lastErrorCode());
    }
}

void loop() {
    char payload[32];
    int len = snprintf(payload, sizeof(payload), "hello #%lu", static_cast<unsigned long>(counter++));

    SendResult result = transport.send(gateway, reinterpret_cast<const uint8_t *>(payload), static_cast<size_t>(len));

    Serial.printf("send status=%d acknowledged=%d payloadBytes=%u\n", static_cast<int>(result.status),
                   result.acknowledged, static_cast<unsigned>(result.payloadBytes));

    delay(2000);
}
