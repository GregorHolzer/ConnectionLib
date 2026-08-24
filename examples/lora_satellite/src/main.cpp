// LoRa satellite example (LilyGO T-Beam, 433MHz SX1278). Sends a small counter payload to
// gateway node 1 every 5s, showing status on Serial and the onboard OLED.

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <XPowersLib.h>

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
XPowersAXP2101 PMU(Wire);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
uint32_t counter = 0;

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

void showStatus(const char *line1, const char *line2, const char *line3 = "") {
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(0, 10, line1);
    display.drawStr(0, 24, line2);
    display.drawStr(0, 38, line3);
    display.sendBuffer();
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    setupTBeamPower();
    display.begin();
    showStatus("LoRa Satellite", "node 2", "starting...");

    if (!transport.setup()) {
        Serial.printf("LoRaTransport::setup() failed, lastErrorCode=%d\n", transport.lastErrorCode());
        char errLine[32];
        snprintf(errLine, sizeof(errLine), "err=%d", transport.lastErrorCode());
        showStatus("LoRa Satellite", "node 2", errLine);
    }
}

void loop() {
    char payload[32];
    int len = snprintf(payload, sizeof(payload), "hello #%lu", static_cast<unsigned long>(counter++));

    SendResult result = transport.send(gateway, reinterpret_cast<const uint8_t *>(payload), static_cast<size_t>(len));

    Serial.printf("send status=%d payloadBytes=%u totalBytes=%u\n", static_cast<int>(result.status),
                   static_cast<unsigned>(result.payloadBytes), static_cast<unsigned>(result.totalBytes));

    char countLine[32];
    snprintf(countLine, sizeof(countLine), "sent #%lu", static_cast<unsigned long>(counter));
    char statusLine[32];
    snprintf(statusLine, sizeof(statusLine), "status=%d", static_cast<int>(result.status));
    showStatus("LoRa Satellite", countLine, statusLine);

    delay(5000);
}
