// LoRa gateway example (LilyGO T-Beam, 433MHz SX1278). Listens on nodeId 1 and prints every
// message it receives, including RSSI/SNR, to Serial and the onboard OLED.

#include <algorithm>
#include <cstring>

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <XPowersLib.h>

#include <ConnectionLib/backends/lora/LoRaTransport.hpp>

using namespace connectionlib;

LoRaConfig makeConfig() {
    LoRaConfig cfg;
    cfg.nodeId = 1;
    return cfg;
}

LoRaConfig config = makeConfig();
LoRaTransport transport(config);
XPowersAXP2101 PMU(Wire);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

// T-Beam's SX1278 is powered through the AXP2101 PMU's ALDO2 rail, off by default. Without this,
// LoRaTransport::setup() fails with RADIOLIB_ERR_CHIP_NOT_FOUND since the radio can't be reached
// over SPI at all. Confirmed against LilyGO's own hardware docs (ALDO2 -> Radio, I2C on
// GPIO21/22, address 0x34): https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/tree/master/docs/en/t_beam
void setupTBeamPower() {
    Wire.begin(21, 22);
    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, 21, 22)) {
        Serial.println("AXP2101 PMU not found - is this a T-Beam with AXP2101?");
        return;
    }
    PMU.setALDO2Voltage(3300);
    PMU.enableALDO2();
}

void showStatus(const char *line1, const char *line2, const char *line3 = "", const char *line4 = "") {
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(0, 10, line1);
    display.drawStr(0, 24, line2);
    display.drawStr(0, 38, line3);
    display.drawStr(0, 52, line4);
    display.sendBuffer();
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    setupTBeamPower();
    display.begin();
    showStatus("LoRa Gateway", "node 1", "starting...");

    if (!transport.setup()) {
        Serial.printf("LoRaTransport::setup() failed, lastErrorCode=%d\n", transport.lastErrorCode());
        char errLine[32];
        snprintf(errLine, sizeof(errLine), "err=%d", transport.lastErrorCode());
        showStatus("LoRa Gateway", "node 1", "setup FAILED", errLine);
        return;
    }

    Serial.println("Gateway ready, listening as LoRa node 1.");
    showStatus("LoRa Gateway", "node 1", "waiting...");
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

        char payloadLine[32];
        size_t copyLen = std::min<size_t>(received->payload.size(), sizeof(payloadLine) - 1);
        memcpy(payloadLine, received->payload.data(), copyLen);
        payloadLine[copyLen] = '\0';

        char bytesLine[32];
        snprintf(bytesLine, sizeof(bytesLine), "bytes=%u", static_cast<unsigned>(received->payload.size()));

        char signalLine[32];
        snprintf(signalLine, sizeof(signalLine), "rssi=%.0f snr=%.0f", received->rssiDbm.value_or(0.0f),
                  received->snrDb.value_or(0.0f));

        showStatus("LoRa Gateway", payloadLine, bytesLine, signalLine);
    }
}
