// LoRa satellite example (LilyGO T-Beam, 433MHz SX1278). Sends a small counter payload to
// gateway node 1 every 5s.

#include <Arduino.h>

#include <ConnectionLib/backends/lora/LoRaTransport.hpp>

using namespace connectionlib;

static const Address gateway = Address::loRaNode(1);

LoRaConfig makeConfig() {
    LoRaConfig cfg;
    cfg.nodeId = 2;
    return cfg;
}

LoRaConfig config = makeConfig();
LoRaTransport transport(config);
uint32_t counter = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);

    if (!transport.setup()) {
        Serial.printf("LoRaTransport::setup() failed, lastErrorCode=%d\n", transport.lastErrorCode());
    }
}

void loop() {
    char payload[32];
    int len = snprintf(payload, sizeof(payload), "hello #%lu", static_cast<unsigned long>(counter++));

    SendResult result = transport.send(gateway, reinterpret_cast<const uint8_t *>(payload), static_cast<size_t>(len));

    Serial.printf("send status=%d payloadBytes=%u totalBytes=%u\n", static_cast<int>(result.status),
                   static_cast<unsigned>(result.payloadBytes), static_cast<unsigned>(result.totalBytes));

    delay(5000);
}
