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

### Antennas are not optional — fit them first

These are the **U.FL variant**: no antenna on the module itself, the whole RF output goes to
that small round connector. With it empty essentially nothing radiates, and two modules will not
find each other even sitting 10 cm apart. Any generic 2.4 GHz U.FL/IPEX antenna works, one per
module; they clip on and unclip just as easily, so check seating if a working link later dies.

This is the *first* thing to verify, because the failure mode looks like everything else. Both
modules answer AT commands normally, the coordinator forms a PAN and reports `AI=0x00`, and the
node scans forever reporting `AI=0x21` "scan found no PANs". Every symptom points at
configuration; none of it is.

Related trap: a transmit status of success only ever means *sent*, never *received*. A broadcast
returns `0x00` quite happily into a dead antenna.

### Module prerequisites

One-time module configuration; none of it can be done from firmware at runtime.

1. **ZB (ZigBee) firmware, not 802.15.4** — see the conversion procedure below.
2. **`AP=2` (API mode *with escaping*).** Not `AP=1`. See the escaping note below — this one
   costs a long debugging session if you get it wrong.
3. **`CE=1` on exactly one module.** Every module defaults to Router, and two Routers never form
   a network at all.
4. **Set `ID` (PAN id) explicitly.** The post-flash default `ID=0` means the coordinator picks a
   random PAN and routers join whatever they find, which pairs non-deterministically.

`AP`, `CE` and `ID` are all settable over the module's own UART from the host MCU — XCTU is not
needed for these, which matters when working headless. From `AP=0` use command mode (`+++`, then
`ATCE1`/`ATID3332`/`ATAP2`/`ATWR`/`ATCN`); from `AP!=0` command mode no longer exists and you must
use `0x08` AT-command API frames instead.

### `AP=2`, not `AP=1` — xbee-arduino always escapes

`andrewrapp/xbee-arduino` escapes `0x7E`, `0x7D`, `0x11` and `0x13` on every transmitted byte
unconditionally (`XBee::sendByte`), i.e. it assumes escaped API mode. On an `AP=1` module any
frame containing one of those bytes is malformed and gets **silently discarded — no status
frame, no error**, so `send()` simply times out.

What makes this vicious is that it is *data-dependent*, so it looks intermittent rather than
broken. Broadcasts worked throughout our bring-up purely by luck:

```
broadcast: 7E 00 12 10 01 00 00 00 00 00 00 FF FF FF FE 00 00 50 49 4E 47 C5   <- no escapable byte
unicast:   7E 00 13 10 01 00 13 A2 00 42 03 5C 03 ...                          <- 0x13 in length AND address
```

Every Digi address starts `0013A200`, so *every* unicast to a real module contains `0x13` and
fails, while broadcast traffic sails through. Proven by hand-building the identical frame and
writing it to the same UART unescaped: `0x8B` status in ~50 ms, versus a 1500 ms timeout through
`xbee_.send()`.

Receive is unaffected in practice: an `AP=1` module sends unescaped, and xbee-arduino's parser
only unescapes when it actually sees `0x7D`. So `poll()` appears to work fine, which further
misdirects the diagnosis.

### Converting 802.15.4 firmware to ZigBee

The modules shipped running **802.15.4** firmware, which implements a completely different API
frame set and ignores the ZB frames `ZigbeeTransport` sends. Check `AT VR`: `2xxx` is 802.15.4
(ours read `VR=2003`, `HV=2246`), `4xxx` is ZB.

| Operation       | ZigBee firmware       | 802.15.4 firmware                 |
|-----------------|-----------------------|-----------------------------------|
| Transmit        | `ZBTxRequest` `0x10`  | `0x00` (64-bit) / `0x01` (16-bit) |
| Receive         | `ZBRxResponse` `0x90` | `0x80` / `0x81`                   |
| Transmit status | `0x8B`                | `0x89`                            |
| Max payload     | 84 bytes              | 100 bytes                         |
| Ack semantics   | network-layer (APS)   | MAC-layer                         |

Same tell as the escaping bug — AT command frames are answered normally so the module looks
healthy, but transmit requests draw complete silence. `MY=0` on a supposedly-joined module is
another hint: in 802.15.4 `MY` is the 16-bit source address and defaults to 0.

Conversion needs **XCTU and a USB-TTL adapter** (a CP2102 at 3.3 V works; it cannot be done
through the ESP32 passthrough). In XCTU: Update Firmware → product family `XB24C`, function set
**ZigBee**, newest version. Notes from doing it on Arch:

- XCTU is in the AUR (`yay -S xctu`) and is x86_64 only, so it will not run on an aarch64 Pi.
- Serial devices belong to group **`uucp`** on Arch, not `dialout`. Without membership XCTU
  cannot open the port and says so unhelpfully.
- XCTU decrypts firmware *into its own install directory*, which is root-owned, and fails with
  `Error decrypting the firmware file ... (Permission denied)`. Fix:
  `sudo chgrp -R wheel /opt/xctu/radio_firmwares` (the dirs are already group-writable). A
  package upgrade resets this.
- **Closing the XCTU window does not exit XCTU** — the Java process keeps running and holds the
  serial port, so anything else reading it sees nothing.
- Flashing wipes the AT config: `AP`, `CE` and `ID` all return to defaults and must be re-applied.

### This project's modules

| Role        | 64-bit address      | On T-Beam (USB serial) | `VR`   |
|-------------|---------------------|------------------------|--------|
| Coordinator | `0013A200 42035C03` | `573C000618`           | `4061` |
| Node        | `0013A200 42035B39` | `5748001588`           | `4064` |

The differing firmware builds are benign — `ZS` (stack profile) and `EE` (encryption) match,
which is what actually has to agree for joining to work.

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
