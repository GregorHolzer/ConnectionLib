// LoRa gateway example (LilyGO T-Beam, 433MHz SX1278). Listens on nodeId 1 and prints every
// message it receives, including RSSI/SNR.

#include <Arduino.h>

#include <ConnectionLib/backends/lora/LoRaTransport.hpp>

using namespace connectionlib;

LoRaConfig makeConfig() {
    LoRaConfig cfg;
    cfg.nodeId = 1;
    return cfg;
}

LoRaConfig config = makeConfig();
LoRaTransport transport(config);

void setup() {
    Serial.begin(115200);
    delay(1000);

    if (!transport.setup()) {
        Serial.printf("LoRaTransport::setup() failed, lastErrorCode=%d\n", transport.lastErrorCode());
        return;
    }

    Serial.println("Gateway ready, listening as LoRa node 1.");
}

void loop() {
    auto received = transport.poll();
    if (received.has_value()) {
        Serial.printf("Received %u bytes, rssi=%.1fdBm snr=%.1fdB: \"", static_cast<unsigned>(received->payload.size()),
                       received->rssiDbm.value_or(0.0f), received->snrDb.value_or(0.0f));
        for (uint8_t byte : received->payload) {
            Serial.write(byte);
        }
        Serial.println("\"");
    }
}
