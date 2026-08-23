// ESP-NOW gateway example: prints its own MAC address (put that into espnow_satellite's
// kGatewayMac), then prints every message it receives.

#include <Arduino.h>
#include <WiFi.h>

#include <ConnectionLib/backends/espnow/EspNowTransport.hpp>

using namespace connectionlib;

EspNowConfig config;   // no fixed peers needed to receive: esp_now_register_recv_cb accepts any sender
EspNowTransport transport(config);

void setup() {
    Serial.begin(115200);
    delay(1000);

    if (!transport.setup()) {
        Serial.printf("EspNowTransport::setup() failed, lastErrorCode=%d\n", transport.lastErrorCode());
        return;
    }

    Serial.print("Gateway ready. This device's MAC address (put into espnow_satellite's kGatewayMac): ");
    Serial.println(WiFi.macAddress());
}

void loop() {
    auto received = transport.poll();
    if (received.has_value()) {
        Serial.printf("Received %u bytes: \"", static_cast<unsigned>(received->payload.size()));
        for (uint8_t byte : received->payload) {
            Serial.write(byte);
        }
        Serial.println("\"");
    }
    delay(10);
}
