# sensor-system compatibility findings

Research notes from reading the thesis exposé (`sources/bachelor/expose/main.pdf`) and the FORTE
sensor-system repo (`~/sensor-system`, git.uibk.ac.at/informatik/qe/forte/sensor-system), gathered
before writing any ConnectionLib code. Scope is limited to RQ1 and RQ2 of the exposé; RQ3
(automatic per-link technology selection) is a possible future extension and is intentionally not
designed for here.

## 1. Exposé scope (RQ1 & RQ2 only)

ConnectionLib's in-scope contribution is twofold:

- A **general, technology-agnostic library** for building low-power LAN star networks (one or more
  satellite nodes reporting to a gateway), so node/gateway application code and payload stay
  identical regardless of which radio (LoRa, Zigbee, ESP-NOW) carries the traffic.
- A **systematic comparison** of LoRa/Zigbee/ESP-NOW range and efficiency under forest cover,
  built using that library as the measurement instrument (in a separate future benchmark
  application, not the library itself).

RQ1 asks which metrics/procedures allow a fair comparison across radios with fundamentally
different link characteristics. RQ2 asks how the three technologies actually compare in forest
vegetation for microcontroller-to-microcontroller communication. RQ3 (automatic per-link radio
selection) is out of scope for now.

Design constraints this implies for the library itself (steps 1–2 of the exposé's six-step
approach):

- **Declarative configuration**, not tied to a single application.
- **Per-band duty-cycle limits must be enforced** by the library, since these differ per radio
  band and constrain how much traffic a benchmark may generate.
- **Verify-before-measure ordering**: correct transport of an identical payload over each backend
  must be established first (step 2), before the library is trusted as a measurement instrument —
  its own overhead (transmission time, transmitted bytes vs. a native API) needs to be measurable
  separately from the radios' properties.
- The library needs to expose enough per-transmission data for a later benchmark app to compute:
  RSSI, SNR, PDR (the established candidates from prior forest studies), plus throughput, latency,
  energy consumption per delivered payload, and a payload-to-total-bytes efficiency measure.

## 2. sensor-system architecture as-is

FORTE/sensor-system is a three-tier deployment, not strictly two-tier:

```
client_satellite / client_central_mast  --(ESP-NOW)-->  host_central_mast  --(LTE-M)-->  backend
```

Despite its name, `client_central_mast` is still a *client* (battery/solar node with more
sensors — RS485 Modbus, SDI12 TEROS11, camera), not the gateway. `host_central_mast` is the real
gateway/sink, on different hardware (LilyGO T-SIM7000G with an integrated LTE-M/GPS modem, vs. the
clients' plain `esp32dev`-class boards).

**ESP-NOW is the only radio actually used in the repo.** A repo-wide search confirmed:

- `DR26` (`client/libs/DR26`) is an analog dendrometer sensor (I2C ADS1115 ADC), not a radio.
- `RS485` and `SDI12` (`client/libs`) are wired sensor buses, not radio.
- **Zigbee is completely absent** — no library, no `lib_dep`, no reference anywhere in source,
  config, or docs.
- No `platformio.ini` anywhere in the repo lists a LoRa or Zigbee hardware dependency (no
  RadioLib, no Adafruit LoRa, no Zigbee stack). ESP-NOW comes from the Arduino-ESP32 core
  (`esp_now.h`), not a package dependency.

This means ConnectionLib is genuinely greenfield with respect to multi-radio abstraction — there
is no existing interface to extend, only one concrete radio implementation to design compatibly
around.

## 3. Existing ESP-NOW transport, end to end

Client side (`client/libs/espnow/ESPNow.hpp`, `Message.hpp`):

- `espnow_setup()` — inits WiFi STA, `esp_now_init`, adds the host as a peer, registers send/recv
  callbacks. Free functions, not a class.
- Addressing is **MAC-address based** (`std::array<uint8_t, 6>`), with host discovery/rediscovery
  handled via a `hostChange` message type and a broadcast fallback.
- Data model: `Message` wraps a `ClientDataPackage` DTO. Messages are batched into fixed
  `std::array<ClientDataPackage, 15>` chunks — driven by ESP-NOW's ~250-byte native MTU — and sent
  raw via `esp_now_send(mac, (uint8_t*)&chunk, sizeof(chunk))`.
- Each chunk send **blocks up to 2000ms** waiting for an ack (`was_msg_received()` polling a static
  flag set from the recv callback). On timeout, the client falls back to RTC-memory-backed local
  caching (`client/libs/caching`) rather than retrying — there is no retry inside
  `sendClientDataPackages` itself.

Host side (`host/lib/Utilities`, namespace `ESPNowUtilities`):

- A FreeRTOS task pinned to core 0 owns the ESP-NOW receive path (`createESPNowReceiveTask`).
- The recv callback sends an ack response back, then pushes the received chunk into a
  mutex-protected in-RAM queue, later drained by the LTE-M forwarding loop.

**Implication for the interface**: the small MTU, the synchronous ack-with-timeout model, and
MAC-based addressing are properties of *this one backend* (ESP-NOW), not requirements to bake into
ConnectionLib's generic API. LoRa and Zigbee have different MTUs, different (or absent) link-layer
acks, and different addressing schemes (node IDs / 16-bit network addresses rather than MACs) — the
transport interface needs an address/config abstraction general enough to cover all three, with
the ESP-NOW-specific behavior (chunking, blocking timeout, RTC-memory cache-on-failure) living in
the ESP-NOW backend implementation, not the interface.

## 4. Conventions to match for consistency

- **`ForteSensor<T>`** (`client/libs/includes/ForteSensor.hpp`) is the one existing abstract
  interface pattern in the codebase and the idiom a new transport interface should mirror
  stylistically: pure-virtual template interface, an explicit `setup()` lifecycle step separate
  from construction, `[[nodiscard]]` on getters, a self-describing accessor
  (`getSensorInformation()`), and `virtual ~T() = default`.
- **Meyers-singleton pattern** (`getInstance()`, deleted copy ctor/assignment) is the established
  idiom for classes owning a single hardware resource (`RS485HardwareSerialSingleton`,
  `SDI12Singleton`, `ModbusRTUSingleton`) — worth following if a ConnectionLib backend needs to own
  a single radio peripheral instance.
- **`.clang-format`**: LLVM base style, 120 column limit, 4-space indent/tab width.
- **Naming**: camelCase methods/variables, PascalCase classes/structs/enums. Newer code is
  consistently camelCase; only the older low-level ESP-NOW free functions use snake_case
  (`espnow_setup`, `on_data_recv`).
- **Header guards**: mixed `#ifndef`/`#define` and `#pragma once` in the codebase, with
  `#pragma once` being the more recent preference.
- **Comments**: Doxygen-style `/** @brief ... @param ... @return ... */` on public methods in
  newer/host code.
- **Toolchain**: PlatformIO, `platform = espressif32`, `framework = arduino`, `-std=gnu++17`, but
  code freely drops to ESP-IDF APIs directly alongside Arduino (`esp_now.h`, `esp_wifi.h`,
  `esp_bt.h`, FreeRTOS primitives) — a deliberate Arduino+ESP-IDF hybrid, not pure Arduino.

## 5. Commit convention note

sensor-system's own git history uses plain, short, imperative commit messages with no prefixes
(e.g. `fix wrong cam_enable assignment`, `30 minute interval for client satellite`) — it does not
use Conventional Commits. Per the user's explicit instruction, ConnectionLib's own history should
use Conventional Commits (`feat:`, `fix:`, `docs:`, etc.) going forward regardless. This is a
deliberate deviation from the reference repo's style, not an oversight or inconsistency to fix.

## 6. Design implications / open questions for the interface

- The transport interface is new architecture, not a refactor — there's nothing existing to
  extend beyond the ESP-NOW free functions described in §3.
- Addressing must generalize beyond MAC addresses to cover LoRa/Zigbee addressing schemes.
- Shape the interface so a **Zigbee backend can be added later without an interface change** (a
  technology-agnostic address/config type, and semantics that don't assume ESP-NOW's
  synchronous-ack/small-MTU model) — the Zigbee implementation itself stays out of scope for now,
  since sensor-system has no Zigbee usage to model it against yet.
- No design accommodation for RQ3 (automatic per-link technology selection) — out of scope per the
  user; don't add capability-query/scoring/reconfiguration machinery aimed at that until asked.
