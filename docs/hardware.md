# Hardware devices

Concrete hardware for the LoRa/Zigbee/ESP-NOW comparison (RQ1/RQ2), read from
`sources/bachelor/hardware specs/zigbee.pdf` (Digi "XBee/XBee-PRO S2C Zigbee RF Module User
Guide") and the [LilyGO T-Beam Meshtastic product page](https://lilygo.cc/products/t-beam-meshtastic?variant=51708927312053).
The three radios differ structurally in a way that matters for the library's transport
abstraction, so each is noted separately.

## Zigbee — Digi XBee/XBee-PRO S2C Zigbee RF Module

A standalone RF module, **not** a microcontroller — it talks to a host MCU over UART (250 Kb/s
max) or SPI (5 Mb/s burst).

- Operating frequency: ISM 2.4–2.5 GHz.
- Channels: 16 direct-sequence channels (XBee) / 15 (XBee-PRO), channels 11–26.
- Network topologies: point-to-point, point-to-multipoint, peer-to-peer, DigiMesh.
- Addressing: PAN ID + device address, with optional cluster IDs/endpoints.
- Range (outdoor RF line-of-sight): up to 1200 m (XBee) / up to 3200 m (XBee-PRO).
- Range (indoor/urban): up to 60 m (XBee) / up to 90 m (XBee-PRO).
- Supply voltage: 2.1–3.6 V (XBee) / 2.7–3.6 V (XBee-PRO).
- Form factor: through-hole or surface-mount.

**Implication**: needs pairing with a host ESP32 board (e.g. via UART) — unlike the other two
radios, it is not itself an MCU+radio unit.

**T-Beam v1.2 wiring** (settled for this project's actual hardware): XBee VCC → the T-Beam's 3V3
pin (right header, last pin — not the left header's 5V pin, which would exceed the XBee's 3.6V
max); GND → either header; XBee DOUT → T-Beam GPIO13 (as UART RX); XBee DIN → T-Beam GPIO25 (as
UART TX). GPIO13/25 were confirmed free on this board (not used by LoRa/I2C/GPS/PMU — only
reserved by an optional TFT add-on this board doesn't have) via `meshtastic/firmware`'s own T-Beam
variant definition. The OLED display sits directly over this header on the physical board, so
wiring in practice means soldering to the header's solder joints on the underside of the PCB
rather than plugging a connector in from the top.

**Prerequisite before two modules will talk to each other**: XBee ZB firmware defaults every
module to Router — two factory-default modules do **not** automatically form a network. One
module must be set to Coordinator (AT command `CE=1`) via XCTU first; this is one-time module
configuration, not something firmware can do at runtime.

## LoRa — LilyGO T-Beam Meshtastic

An integrated ESP32 dev board with an onboard LoRa module. The variant to use is the **SX1278,
433 MHz** — the EU-conform option (as opposed to the SX1276 868/915/923 MHz variants also sold).

- MCU: ESP32, 4 MB flash, 8 MB PSRAM.
- LoRa: SX1278 (433 MHz), sensitivity -148 dBm, transceive rate 300 kbps.
- GPS: NEO-6M module, onboard RTC crystal.
- Display: 0.96" OLED (SSD1306).
- Power: AXP2101 PMU, USB Micro or 18650 battery holder (USB can charge the battery).
- Also has Wi-Fi + Bluetooth 4.2 on the ESP32 itself.

**Implication**: unlike the Zigbee module, this ships as a single MCU+radio unit — closer in
shape to how ESP-NOW is already used in sensor-system (native ESP32 radio, no external module).

## ESP-NOW

For completeness/contrast: ESP-NOW uses the ESP32's built-in Wi-Fi radio directly (as already
confirmed in [`sensor-system-findings.md`](sensor-system-findings.md)), no separate module needed.

## Asymmetry to keep in mind

The benchmark's LoRa and ESP-NOW nodes are each a single ESP32 board, while the Zigbee node is an
ESP32 host plus a separate XBee module wired to it. This is relevant later when the library's
per-backend setup/configuration is designed.

## Toolchain

The thesis abstract states explicitly that **PlatformIO** is to be used for MCU
development/programming — consistent with (and reinforcing) the PlatformIO-based conventions
already documented from sensor-system in
[`sensor-system-findings.md`](sensor-system-findings.md).
