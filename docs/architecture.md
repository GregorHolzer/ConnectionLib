# ConnectionLib architecture

Design for the core library: a technology-agnostic C++ transport abstraction over ESP-NOW, LoRa,
and (later) Zigbee for star-topology wireless sensor networks, targeting PlatformIO/ESP32. Scope is
RQ1/RQ2 only (see [`sensor-system-findings.md`](sensor-system-findings.md)) — no RQ3 (automatic
per-link technology selection) machinery. Zigbee (Digi XBee S2C) stays interface-ready but
unimplemented for now; only ESP-NOW and LoRa (LilyGO T-Beam, SX1278 @ 433 MHz via RadioLib, see
[`hardware.md`](hardware.md)) get built in this pass.

Two design decisions worth calling out up front:
- **LoRa ack semantics: fire-and-forget.** `LoRaTransport::send()`'s `acknowledged` is always
  `false` — no app-layer ARQ. PDR/reliability is a future benchmark app's job (via its own sequence
  numbers), not the library's.
- **"Native" baseline for the verify-against-native step = raw RadioLib calls**, not a hand-rolled
  SX1278 SPI driver. The comparison measures ConnectionLib's own wrapping overhead on top of
  RadioLib, not RadioLib's overhead vs. bare-metal.

## Repo / package structure

```
library.json                       # PlatformIO manifest, NO unconditional deps (see below)
platformio.ini                     # maintainer-only: pio test envs + example builds
.clang-format                      # copied verbatim from sensor-system

include/ConnectionLib/             # public headers, namespace `connectionlib`
  Transport.hpp                    # abstract base interface
  Address.hpp
  LinkMetrics.hpp                  # LinkMetrics / SendResult / ReceivedMessage / SendStatus
  DutyCycleGuard.hpp
  backends/espnow/EspNowTransport.hpp   # + EspNowConfig
  backends/lora/LoRaTransport.hpp       # + LoRaConfig, LoRaPinConfig (T-Beam defaults)
  # backends/zigbee/  -- intentionally NOT created yet, see below

src/ConnectionLib/
  DutyCycleGuard.cpp
  backends/espnow/EspNowTransport.cpp
  backends/lora/LoRaTransport.cpp

test/
  test_address/  test_duty_cycle/       # pure logic, native host env, no hardware
  test_espnow_device/  test_lora_device/  # optional, added later once M3/M5 numbers exist
```

**ConnectionLib is lib-only** — no `examples/` in this repo. Runnable examples
(`espnow_satellite`/`espnow_gateway`, `lora_satellite`/`lora_gateway`, and eventually the
verify-against-native harnesses below) live in the separate
[`ConnectionLib-examples`](https://github.com/GregorHolzer/ConnectionLib-examples) repo, which
includes this repo as a git submodule so the lib source stays readable/buildable from there.

**RadioLib is not a `library.json` dependency.** It's scoped per-example (`lib_deps =
jgromes/RadioLib@^6` in each LoRa example's own `platformio.ini`, in the examples repo), so an
ESP-NOW-only consumer of ConnectionLib never pulls it in.

**Each backend is also guarded behind an opt-in build macro**
(`CONNECTIONLIB_ENABLE_ESPNOW`/`CONNECTIONLIB_ENABLE_LORA`), not just `#if defined(ESP32)` — this
was tightened after discovering that PlatformIO compiles a dependency's **entire `src/` tree**
once it's pulled in at all, not just the translation units whose headers a consumer actually
`#include`s. An `#if defined(ESP32)`-only guard was not enough to stop an ESP-NOW-only consumer
from hitting `RadioLib.h: No such file or directory` compiling `LoRaTransport.cpp`, since that file
still gets compiled as part of ConnectionLib's library archive regardless of what the consumer's
own `main.cpp` includes. Consumers now must both add the backend's enable macro to `build_flags`
and add its hardware dependency (RadioLib for LoRa) to `lib_deps`; forgetting either produces an
immediate, clear compile error rather than a silent unconditional dependency. No umbrella header
still exists that includes every backend — trade-off: no single "install and it all just works"
header; acceptable given the examples repo shows exactly what to add per backend.

Namespace `connectionlib` wraps everything — a deliberate departure from sensor-system's bare
global-class style (`ForteSensor`), justified because ConnectionLib is a redistributable package
pulled into other projects, where symbol-collision risk is real in a way it isn't for
sensor-system's own app-local code.

## Core interface

**`Address`** (`include/ConnectionLib/Address.hpp`) — tagged POD, not `std::variant` (avoids
exception-throwing accessors on an embedded target):
```cpp
enum class AddressKind : uint8_t { Mac6, XBee64_16, LoRaNodeId16, Broadcast };
class Address {
  public:
    static Address mac(const std::array<uint8_t, 6> &mac);
    static Address xbee(uint64_t addr64, uint16_t addr16 = 0xFFFE);
    static Address loRaNode(uint16_t nodeId);
    static Address broadcast(AddressKind forKind);
    [[nodiscard]] AddressKind kind() const;
    [[nodiscard]] std::optional<std::array<uint8_t, 6>> asMac() const;
    [[nodiscard]] std::optional<XBeeAddressPair> asXBee() const;
    [[nodiscard]] std::optional<uint16_t> asLoRaNodeId() const;
    bool operator==(const Address &other) const;
  private:
    AddressKind kind_;
    std::array<uint8_t, 8> bytes_{};
    uint16_t aux16_ = 0;   // XBee 16-bit network address only
};
```
`0xFFFF` is reserved as the LoRa broadcast/"no node ID" sentinel.

**Per-backend `Config`** — no shared base beyond duty-cycle; each is co-located with its
`Transport` subclass (forcing a common shape would produce unused fields per backend):
```cpp
struct EspNowConfig {
    std::vector<Address> peers;
    uint8_t wifiChannel = 0;         // 0 = use current STA channel
    bool encrypt = false;
    DutyCycleConfig dutyCycle{100.0f, 3'600'000};   // 2.4GHz ISM: no ETSI duty-cycle mandate
};
struct LoRaPinConfig {               // T-Beam v1.1 classic defaults, override for other boards
    int sck = 5, miso = 19, mosi = 27, nss = 18, reset = 23, dio0 = 26;
};
struct LoRaConfig {
    LoRaPinConfig pins{};
    float frequencyMHz = 433.0f;     // EU SX1278 variant
    float bandwidthKHz = 125.0f;
    uint8_t spreadingFactor = 9;
    uint8_t codingRate = 7;
    int8_t txPowerDbm = 17;
    uint16_t preambleLength = 8;
    uint16_t nodeId = 0;             // this node's software address
    DutyCycleConfig dutyCycle{1.0f, 3'600'000};  // PLACEHOLDER — exact ETSI 433MHz figure unverified, see risks
};
```

**`LinkMetrics` / `SendResult` / `ReceivedMessage`** (`include/ConnectionLib/LinkMetrics.hpp`):
```cpp
struct LinkMetrics {
    uint32_t timestampMicros;
    size_t totalBytes;                  // actual bytes on the wire/air, incl. backend framing
    std::optional<float> rssiDbm;       // nullopt where the backend/hardware doesn't expose it
    std::optional<float> snrDb;
};
enum class SendStatus : uint8_t { Ok, Timeout, DutyCycleExceeded, PayloadTooLarge, NotReady, Failed };
struct SendResult : LinkMetrics {
    SendStatus status;
    size_t payloadBytes;
    bool acknowledged;                  // backend-defined; see per-backend sections
};
struct ReceivedMessage : LinkMetrics {
    Address source;
    std::vector<uint8_t> payload;
};
```

**`Transport`** (`include/ConnectionLib/Transport.hpp`) — mirrors `ForteSensor<T>`'s idiom (pure
virtual, explicit `setup()` lifecycle step, `[[nodiscard]]` getters, self-describing accessor,
`virtual ~Transport() = default`) but is **not templated**: all backends move the same unit (raw
bytes), so templating on a payload type would work against technology-agnosticism.
```cpp
class Transport {
  public:
    virtual bool setup() = 0;                              // can genuinely fail (bad wiring, esp_now_init error)
    [[nodiscard]] virtual int lastErrorCode() const = 0;    // backend-native code, for diagnostics
    virtual SendResult send(const Address &destination, const uint8_t *payload, size_t len) = 0;
    virtual std::optional<ReceivedMessage> poll() = 0;      // non-blocking, call from loop()
    [[nodiscard]] virtual Address broadcastAddress() const = 0;
    [[nodiscard]] virtual size_t maxPayloadBytes() const = 0;   // MTU; caller chunks, base class never auto-chunks
    [[nodiscard]] virtual const char *technologyName() const = 0;
    [[nodiscard]] virtual float currentDutyCyclePercent() const = 0;
    [[nodiscard]] virtual uint32_t timeUntilNextAllowedMs(size_t payloadLen) const = 0;
    virtual ~Transport() = default;
};
```
No auto-chunking: if `len > maxPayloadBytes()`, `send()` returns `SendStatus::PayloadTooLarge`.
Chunking/retry policy (sensor-system's 15×`ClientDataPackage` + RTC-cache-on-timeout) is an
app-level decision, not something to bake into a technology-agnostic interface.

**`ProtocolKind`** (`include/ConnectionLib/Transport.hpp`) — `enum class ProtocolKind : uint8_t {
EspNow, LoRa, Zigbee };` (Zigbee reserved/unimplemented, matching `AddressKind`'s existing
precedent). A shared vocabulary type so application code can select a backend via a runtime value
instead of the choice being implicit in which concrete class is written in source — e.g. a future
benchmark app choosing the protocol from a config setting rather than which binary got flashed.
`EspNowTransport`/`LoRaTransport` already have public constructors and both derive from
`Transport`, so `std::unique_ptr<Transport> t = std::make_unique<EspNowTransport>(config);` already
works without any factory — `ProtocolKind` is the only piece that was actually missing.

**Deliberately no `createTransport(ProtocolKind, ...)` factory in this library.** Each backend has
its own `Config` type and header, and `LoRaTransport.hpp` transitively pulls in RadioLib — a
unified factory here would need to reference every backend's types together, forcing RadioLib onto
every consumer even an ESP-NOW-only one, which is exactly the unconditional-dependency problem the
RadioLib-scoping decision above already avoids. The actual `switch (kind) { ... }` dispatch belongs
in application code, which already knows which backends it has chosen to link in — same reasoning
already applied to board bring-up (T-Beam pins, AXP2101 power), which also lives in the examples
repo, not this library.

**Receive model: blocking `send()` (bounded by a timeout) + polled `poll()`, no async callbacks.**
Justification: ESP-NOW's existing pattern already blocks on a flag; RadioLib's interrupt-driven
path needs ISR-safe code with no equivalent on the other backends; `xbee-arduino` (the eventual
Zigbee driver) has **no callback path at all** — `readPacket()` is fundamentally a polled read. A
poll-based receive is the one model all three backends can honestly implement identically, which
is what keeps app code identical across backends. It's also what makes the native-vs-library timing
comparison in the verify step clean (`t0 = micros(); send(); t1 = micros();`).

## Duty-cycle enforcement

Shared, backend-agnostic component used by composition (`include/ConnectionLib/DutyCycleGuard.hpp`):
```cpp
struct DutyCycleConfig { float maxDutyCyclePercent; uint32_t windowMs; };
class DutyCycleGuard {
  public:
    explicit DutyCycleGuard(DutyCycleConfig config);
    [[nodiscard]] bool canTransmit(uint32_t estimatedDurationMs) const;     // admission check, before spending airtime
    void recordTransmission(uint32_t actualDurationMs);                     // ledger credit, after send resolves
    [[nodiscard]] float currentDutyCyclePercent() const;
    [[nodiscard]] uint32_t timeUntilNextAllowedMs(uint32_t estimatedDurationMs) const;
  private:
    DutyCycleConfig config_;
    // fixed-capacity ring buffer of (startTimeMs, durationMs), pruned of entries older than windowMs
};
```
Each backend owns a `DutyCycleGuard` built from its config; `Transport::currentDutyCyclePercent()`/
`timeUntilNextAllowedMs()` forward to it. Ledger entries use **measured elapsed wall-clock time**
(the same interval already captured in `SendResult.timestampMicros`), not a per-backend PHY-airtime
formula — keeps accounting uniform across backends. `radio.getTimeOnAir(len)` (RadioLib) is used
only as LoRa's pre-flight `canTransmit()` estimate.

**Reject, not block-until-allowed**: when `canTransmit()` fails, `send()` returns
`SendStatus::DutyCycleExceeded` immediately. A satellite node blocking internally would either
busy-wait (wastes battery) or need its own sleep policy — out of place in a general transport
library. It also keeps "duty-cycle throttled" distinguishable from "link failed" in the data a
future benchmark app collects for RQ1. `EspNowConfig`'s default duty cycle is effectively unlimited
(100%, 2.4GHz ISM has no ETSI mandate); a future benchmark app can still configure a matching
artificial cap on ESP-NOW to create a level playing field against LoRa's regulatory one.

## ESP-NOW backend

`EspNowTransport : Transport` wraps `esp_now.h` directly (Arduino-ESP32 core, no external dep,
matching sensor-system exactly).

- **Backend-internal**: peer registration (`esp_now_add_peer()` per `Address` in
  `EspNowConfig.peers`) in `setup()`; `WiFi.mode(WIFI_STA)` + `esp_now_init()` sequencing; the
  static-flag-from-recv-callback pattern for detecting completion, with a configurable timeout
  (default 2000ms, matching sensor-system's existing value). No chunking, no RTC-memory
  cache-on-timeout fallback — `send()` just returns `SendStatus::Timeout` and lets the caller decide.
- **Surfaced**: `send()`/`poll()`/`SendResult` per the generic interface.
  `SendResult.acknowledged = (status == ESP_NOW_SEND_SUCCESS)` from `esp_now_send_cb` — reuses the
  WiFi driver's MAC-layer ack for free, no app-layer ack needed.
  `maxPayloadBytes()` returns ESP-NOW's native MTU (~250 bytes).
  `rssiDbm`/`snrDb` are left `nullopt` — see risks below.
  `poll()` drains a small internal queue fed by the recv callback (callback itself stays minimal:
  push raw bytes, no allocation/parsing inside it).

## LoRa backend

`LoRaTransport : Transport` wraps RadioLib's `SX1278` class. `LoRaPinConfig` defaults to the T-Beam
v1.1 classic pinout (SCK=5, MISO=19, MOSI=27, NSS=18, RST=23, DIO0=26), overridable for other boards.

`setup()`: `SPI.begin(pins.sck, pins.miso, pins.mosi, pins.nss)` → construct RadioLib
`Module(pins.nss, pins.dio0, pins.reset)` + `SX1278 radio(&module)` → `radio.begin(frequencyMHz,
bandwidthKHz, spreadingFactor, codingRate, ..., txPowerDbm, preambleLength)`, mapping RadioLib's
`int16_t` return to `bool`/`lastErrorCode()`. (Re-verify exact RadioLib constructor/overload
signatures against whichever version gets pinned — the API has shifted across major versions.)

**Software addressing frame (backend-internal)**: raw SX1278 point-to-point has no PHY-layer
addressing — every receiver in range gets every packet. `LoRaTransport` prepends a thin header
before `radio.transmit()`:
```cpp
struct LoRaFrameHeader { uint16_t destNodeId; uint16_t srcNodeId; uint8_t seq; uint8_t payloadLen; };
```
`poll()` parses this header and drops (`std::nullopt`) any frame not addressed to `config.nodeId`
or the broadcast sentinel.

**Ack: fire-and-forget (confirmed).** `acknowledged` is always `false` for LoRa — raw SX1278 has no
MAC-layer ack, and building an app-layer ARQ here would add protocol complexity better left to a
future benchmark app's own sequence-number-based PDR computation. This means "send success" means
different things across backends (MAC-confirmed for ESP-NOW vs. "bits clocked out" for LoRa) —
worth keeping in mind when the benchmark app interprets results, but not a library-level problem.

**RSSI/SNR**: `radio.getRSSI()`/`getSNR()` called immediately after a successful `receive()` inside
`poll()` — works natively for LoRa, no workaround needed.

**Duty cycle**: `LoRaConfig.dutyCycle` feeds a `DutyCycleGuard` exactly as ESP-NOW does.

## Zigbee — interface-ready, not implemented

No `backends/zigbee/` directory yet (an empty dir isn't git-trackable anyway, and per CLAUDE.md's
bias against speculative scaffolding, a placeholder folder adds nothing enforceable). Confirmed the
existing abstractions already accommodate it when the time comes:
- `Address::xbee(addr64, addr16)`/`asXBee()` already model XBee's 64-bit (SH+SL) + 16-bit network
  address scheme (matches `XBeeAddress64` in `andrewrapp/xbee-arduino`, the standard API-mode driver
  for this module).
- A future `ZigbeeConfig` would follow the same "plain struct" shape as the other two configs
  (UART handle, baud, own 64-bit address, PAN ID, `DutyCycleConfig`) — no `Transport` changes needed.
- `Transport::poll()`'s non-blocking contract already matches `xbee-arduino`'s only model
  (`readPacket(timeout)`).
- ZB TX Status frames (0x8B) carry a real network-layer delivery ack, so `acknowledged` would map
  naturally from that later — noted for completeness, not designed further.

## Native-vs-library verification (exposé step 2)

Lives in `examples/verify_native_espnow/` and `examples/verify_native_lora/`, not `test/` — this is
a data-collection run whose acceptable bounds aren't known until it's actually been run once, not a
pass/fail assertion. Each example sends a fixed payload N times via the native path (raw
`esp_now_send()` / raw `radio.transmit()`, confirmed as the right "native" baseline for LoRa — i.e.
RadioLib's own overhead vs. bare-metal SPI is out of scope) and N times via
`EspNowTransport::send()`/`LoRaTransport::send()`, timing both with `micros()` and logging
`native_bytes,native_us,library_bytes,library_us` per iteration over serial for external analysis.
Once a human has looked at one run's numbers and decided what "acceptable overhead" means, this can
be promoted to a Unity device test (`test/test_espnow_device/`, `test/test_lora_device/`) with a
`TEST_ASSERT` bound as a regression guard — that's milestone M3.5/M5.5, not part of M3/M5, since the
bound can't be chosen correctly before the example has actually run once.

## Build order

| # | Milestone | Verification |
|---|---|---|
| M0 | Repo scaffolding: `library.json`, `platformio.ini`, directory layout, `.clang-format` copied from sensor-system. | `pio run` against a trivial stub confirms manifest+layout are valid. |
| M1 | Core value types: `Address`, `LinkMetrics`/`SendResult`/`ReceivedMessage`, `DutyCycleGuard`. Pure C++17, no hardware. | `pio test -e native` runs `test_address`/`test_duty_cycle` on the host — no ESP32 needed; `DutyCycleGuard`'s ring-buffer math is fully host-testable with synthetic clocks. |
| M2 | `EspNowTransport` — has an existing reference implementation to validate against, so built before the unfamiliar LoRa backend. | Two ESP32 boards, `examples/espnow_satellite`+`espnow_gateway`; confirm payload arrives and `acknowledged` flips correctly with peer in/out of range. |
| M3 | ESP-NOW native-vs-library harness. | Plausible, explainable overhead numbers from `examples/verify_native_espnow`. |
| M3.5 | (optional) Promote to `test/test_espnow_device/` with bound assertions. | `pio test` passes on hardware. |
| M4 | `LoRaTransport` — RadioLib, T-Beam pins, addressing frame, duty-cycle integration. | Two T-Beam boards, `examples/lora_satellite`+`lora_gateway`; confirm transport works, `rssiDbm`/`snrDb` populate, and rapid-fire `send()` eventually trips `DutyCycleExceeded`. |
| M5 | LoRa native-vs-library harness. | Plausible numbers from `examples/verify_native_lora`. |
| M5.5 | (optional) Promote to `test/test_lora_device/`. | Same pattern as M3.5. |
| M6 | This document, README quickstart per backend, Doxygen audit. | Human review. |
| M7 | Zigbee/XBee backend — explicitly out of scope for this pass, listed as the natural next step. | — |

## Open risks (flagged, not silently resolved)

1. **ESP-NOW RSSI/SNR isn't natively exposed by `esp_now.h`.** A promiscuous-mode sniffer callback
   workaround exists but is version-dependent and fiddly. Shipping `nullopt` for now with a
   documented TODO; a separate spike if the benchmark app later needs it.
2. **Exact ETSI duty-cycle % for the T-Beam's 433MHz sub-band is unverified.** `LoRaConfig`'s
   default (1%, 1h window) is a placeholder marked TODO/citation-needed in code, not asserted as
   regulatory fact.
3. **Thread-safety**: nothing here is safe for concurrent FreeRTOS-task access (matches
   sensor-system's client-side single-`loop()` pattern). If a future gateway use wants
   sensor-system's host-side mutex-protected-queue pattern, that needs to be added deliberately.
4. **`std::vector`/heap use** in `EspNowConfig.peers` and `ReceivedMessage.payload` — a deliberate
   simplicity-over-embedded-purism trade-off (ESP32 has ample heap for this workload) rather than
   fixed-capacity containers.
5. **`library.json` license/publishing target** left open — MIT is a reasonable default (matches
   RadioLib's own licensing) unless the user wants something else; whether this ever gets published
   to the PlatformIO registry vs. just referenced as a git dependency affects how much manifest
   polish is worth doing now.
