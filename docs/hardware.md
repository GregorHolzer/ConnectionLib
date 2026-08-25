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

**Pin numbering differs between the two module form factors**, and the user guide contains both
tables. Wire by *signal name*, not by pin number, unless you have checked which variant you hold:

| Signal | Surface-mount (SMT, p. 41) | Through-hole (TH, p. 44) |
|--------|---------------------------|--------------------------|
| GND    | 1                         | 10                       |
| VCC    | 2                         | 1                        |
| DOUT   | 3                         | 2                        |
| DIN    | 4                         | 3                        |

The modules used in this project are the **SMT** variant (`GND, VCC, DOUT, DIN` = pins 1-4).
Two SMT-only straps are worth knowing about, because either makes a module look dead while it is
in fact perfectly healthy: pin 15 `SPI_SSEL` held low at reset selects SPI instead of UART, and
pin 12 `SPI_ATTN/BOOTMODE` is documented "do not tie low on reset" — low means bootloader mode.

**Prerequisites before two modules will talk to each other.** None of these can be done from
firmware at runtime; all are one-time module configuration.

1. **`AP=1` (API mode).** `ZigbeeTransport` speaks only API frames. Factory default is `AP=0`
   (Transparent), in which the module treats those frames as ordinary payload and transmits them
   verbatim — no ack, no error, and a failure indistinguishable from bad wiring. Confirmed as the
   default on two independent modules during bring-up.
2. **`CE=1` on exactly one module.** Every module defaults to Router, so two factory-default
   modules never form a network at all.
3. **ZB (ZigBee) firmware, not 802.15.4** — see the blocker below.

Both `AP` and `CE` can be set over the module's UART from the host MCU (enter command mode with
`+++`, then `ATAP1` / `ATCE1` / `ATWR` / `ATCN`). XCTU is *not* required for this, which matters
when working headless — XCTU is a Java GUI and expects the module on a USB adapter.

**Blocker found during bring-up: the modules ship with 802.15.4 firmware, not ZigBee firmware.**
XBee S2C hardware runs either, and the two speak completely different API frame sets. Check `VR`:
a `2xxx` value (ours read `VR=2003`, `HV=2246`) is the 802.15.4 series; ZB firmware reports `4xxx`.
The practical symptom is oddly specific — AT command frames (`0x08`) are answered normally, so the
module looks alive and correctly configured, but transmit requests are met with total silence:

| Operation            | ZigBee firmware      | 802.15.4 firmware        |
|----------------------|----------------------|--------------------------|
| Transmit             | `ZBTxRequest` `0x10` | `0x00` (64-bit) / `0x01` (16-bit) |
| Receive              | `ZBRxResponse` `0x90`| `0x80` / `0x81`          |
| Transmit status      | `0x8B`               | `0x89`                   |
| Max payload          | 84 bytes             | 100 bytes                |
| Ack semantics        | network-layer (APS)  | MAC-layer                |

Verified directly against the hardware: a hand-built `0x10` frame gets no response at all, while
`0x00` and `0x01` both return `0x89` with `status=0x00` (success). `MY=0` on every module is
another tell — in 802.15.4 `MY` is the 16-bit source address and defaults to 0.

This is not merely an implementation detail. 802.15.4 is the PHY/MAC layer; ZigBee is the
mesh/network layer on top, and that network layer is exactly what the RQ1/RQ2 comparison is
meant to exercise. Either the modules get reflashed to ZB firmware (XCTU, and an XBee USB adapter
— this cannot go through the ESP32 passthrough), or the backend is rewritten against 802.15.4 and
renamed, in which case the APS-ack claim and the 84-byte limit both have to be corrected.
**Undecided as of this pass; `ZigbeeTransport` remains ZB-only and therefore non-functional on
the current hardware.**

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
