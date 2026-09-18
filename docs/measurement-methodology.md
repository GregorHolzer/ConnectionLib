# Measurement methodology (RQ1)

Working reference for the metric set and experimental procedure used to compare LoRa,
Zigbee and ESP-NOW. This is the answer to the exposé's

> **RQ1.** Which metrics and experimental procedures allow a fair comparison between
> wireless technologies whose link characteristics differ fundamentally?

The exposé states that the procedure itself, not only the numbers it produces, is a result
of the thesis. This document is therefore structured as *justification*, not as a list:
every metric is tied to the source that motivates it, and every place where the three
technologies cannot be made comparable is named explicitly rather than averaged over.

Status: derived from the sources marked **[read]** below. Claims resting on sources not yet
retrieved are marked **[pending]** and must not be cited until checked.

## 1. Framework

`boano2018` (IoTBench) provides the classification this document uses, and states the
fairness condition directly:

> a necessary condition for a fair comparison between different protocols is that their
> behavior is determined by the exact same set of input parameters

- **Input parameters** — under the experimenter's control. Must be *identical* across the
  three technologies, or the comparison is unsound.
- **Output metrics** — the results, determined solely by the protocol given fixed inputs.
- **Observed metrics** — characterise the environment during a run. IoTBench calls these
  "necessary to enable fair comparison": without them there is no evidence that two runs
  faced comparable conditions.
- **Experiment parameters** — repetitions, acceptable variability. They do not affect the
  outputs; they determine the scope within which the outputs are valid.
- **Profile** — a concrete assignment of input parameters mapping to an application domain.

IoTBench's own worked example is a *low-rate data collection profile for environmental
monitoring*, which is exactly this project's case.

## 2. The profile

Taken from FORTE (`pfister2025`, `pfister2024`), the motivating deployment:

| Property | Value | Source |
|---|---|---|
| Topology | star, single hop (multi-hop deliberately rejected for energy) | `pfister2025` |
| Required range | ≥ 50 m | `pfister2025` |
| Node lifetime | satellites: months on battery; gateway: ~1 week | `pfister2025` |
| Environment | forest, under canopy, nodes mounted below 2 m | `pfister2025`, `becker2025` |
| Traffic | low-rate periodic collection, satellites → gateway | `pfister2025` |

The 50 m requirement is worth dwelling on: `becker2025` found ESP-NOW links **dead at 59,
61 and 64 m while working at 62 and 65 m** in open farmland. FORTE's requirement sits right
at the edge of the demonstrated range of the technology it currently uses, which is a large
part of why the comparison is worth running at all.

## 3. Input parameters — held identical across all three

| Parameter | Setting | Justification |
|---|---|---|
| Payload size | sweep within the **common** range, ≤ 84 B | Zigbee binds (§6.1). `ojo2021` shows PDR is a function of payload length, so one size is not a safe summary |
| Inter-packet interval | from the FORTE profile | `boano2018`: traffic parameters must always be input |
| Packets per test point | from §5 | — |
| TX power | normalised by EIRP where the band permits | `ojo2021` set 9.9/10.4 dBm across 433/868 MHz *specifically* to equalise system gain |
| Antenna height | fixed, < 2 m | `becker2025` used 0.95 m; `ojo2021` 1.7 m; forest sensors typically below 2 m |
| Distance / position | shared grid, all three measured at the same points | §6 |
| Topology | 1 sender, 1 receiver, single hop | matches the profile |

### 3.1 Fixed radio parameters (EU-conformant)

Every radio parameter below is held **identical across all runs and never relaxed for
benchmarking convenience**. They are the EU-legal operating point for each band, so the numbers
the comparison produces are numbers a deployed FORTE node could legally produce. Each value lives
exactly once in code, as the default in the backend's config struct.

**433 MHz — ETSI EN 300 220-2, CEPT ERC-REC 70-03 Annex 1 band h1.3.** Band 433.050–434.790 MHz,
≤ 10 mW e.r.p., duty cycle ≤ 10 % across the whole band.

| `LoRaConfig` field | Value | Why this value |
|---|---|---|
| `frequencyMHz` | 433.2 | 433.0 with 125 kHz occupied bandwidth spans ~432.94–433.06 MHz — **below the band edge**. 433.2 clears both edges. |
| `bandwidthKHz` | 125 | Rules out the h1.5 sub-band (434.040–434.790, no duty-cycle limit), whose channel spacing is far narrower, so h1.3's 10 % governs. |
| `spreadingFactor` | 9 | Held fixed. `ojo2021` treats SF as a swept variable in its own right; sweeping it here would confound the technology comparison. |
| `codingRate` | 4/7 | Fixed, as above. |
| `txPowerDbm` | 7 | The limit is 10 mW (10 dBm) **e.r.p.**, so ~2 dBi of antenna gain leaves ~9 dBm radiated. The previous 17 dBm was 50 mW — five times the ceiling. |
| `dutyCycle` | 10 % / 1 h | = 360 s airtime per hour. Previously 1 %, an unverified placeholder about ten times stricter than the band requires. |

**2.4 GHz (ESP-NOW and Zigbee) — ETSI EN 300 328 V2.2.2.** Band 2400–2483.5 MHz, ≤ 100 mW
(20 dBm) EIRP, power spectral density ≤ 10 dBm/MHz for non-FHSS. **No duty-cycle limit applies:**
the standard's 10 % medium-utilisation cap binds only *non-adaptive* equipment, and both ESP-NOW
(802.11 CSMA/CA) and Zigbee (802.15.4 CSMA/CA) are adaptive. Hence `dutyCycle{100.0f, …}` in
`EspNowConfig` and `ZigbeeConfig` is correct rather than merely permissive.

**The two bands cannot be power-equalised — 10 mW versus 100 mW is law, not tuning.** Per §6.4
this is reported as a property of the technology and explicitly *not* normalised away; §3's
EIRP-normalisation row applies only within a band, which is also all `ojo2021` claimed for it.

**Consequence for run planning.** LoRa's airtime is the binding constraint, and it is a result
rather than an inconvenience (§6.3). At SF9/BW125/CR4:7 one transmission costs ~198 ms at an 8 B
payload and ~685 ms at 84 B, so a 100-exchange sweep over 8/16/32/84 B consumes ~148 s of airtime
per board — comfortably inside the 360 s hourly budget, but only **about two full sweeps per hour
are legal**. The benchmark runner reports measured airtime against that budget for every run.

## 4. Output metrics

Availability differs per technology, and a metric unavailable on one radio cannot serve as
a primary comparison axis — it can only be reported as supporting evidence.

| Metric | Definition | LoRa | Zigbee | ESP-NOW |
|---|---|---|---|---|
| **PDR** | received ÷ sent, **counted at the receiver** | ✅ | ✅ | ✅ |
| **One-way delay** | send→receive, needs clock sync | ✅ | ✅ | ✅ |
| **Jitter** | variation of one-way delay | ✅ | ✅ | ✅ |
| **Goodput** | useful payload bits ÷ time | ✅ | ✅ | ✅ |
| **Energy per delivered payload** | J per successfully received message | ✅ | ✅ | ✅ |
| **Payload efficiency** | useful payload ÷ total bytes on air | ✅ | ✅ | ✅ |
| RSSI | received signal strength | ✅ | ⚠️ separate query | ❌ not exposed |
| SNR | signal-to-noise ratio | ✅ | ❌ | ❌ |

**PDR must be counted at the receiver.** This is the single most important methodological
point, and it follows from the ack asymmetry in §6.2 — a sender-side delivery figure means
three different things across these radios and is not comparable.

**Energy per delivered payload**, not energy per transmission. FORTE's binding requirement
is months of battery life, and a technology that retransmits heavily can look efficient per
packet while being ruinous per *delivered* packet.

**Payload efficiency** is where the three differ structurally rather than by tuning. LoRa
carries a 6-byte software header (`destNodeId`/`srcNodeId`/`seq`/`payloadLen`) purely
because raw SX1278 has no addressing at all, whereas Zigbee and ESP-NOW address natively.
At an 8-byte payload that is 43 % overhead for LoRa and none for the others.

RSSI and SNR are deliberately **demoted to observed metrics** (§5), not comparison axes.
They are unavailable on ESP-NOW, and `baccour2012` **[pending]** is the standard reference
for why they are weak predictors of delivery even where available.

## 5. Observed metrics

Recorded per run to establish that runs were comparable; not compared between technologies.

- **RSSI / SNR** where the radio exposes them
- **External interference** — 2.4 GHz shares spectrum with Wi-Fi; 433 MHz does not. This
  asymmetry is a threat to fairness (§6.5) and must be measured, not assumed away
- **Vegetation state** — species, density, foliage/season, wetness. Attenuation is
  frequency-dependent (`itu833`), so this interacts with the band asymmetry
- **Temperature** — known to affect low-power transceivers **[pending: verify source]**
- **Time of day / date** of each run

## 6. Fairness threats and mitigations

This is the substance of RQ1. Prior work compares within a band (`ojo2021`) or pairwise
(`liu2022` **[pending]**); comparing all three at once raises problems none of them face.
Items 1, 2 and 6 were confirmed empirically during hardware bring-up (see
[`hardware.md`](hardware.md), [`architecture.md`](architecture.md)).

**6.1 Maximum payload differs by 3×.** ESP-NOW 250 B, LoRa 249 B (255 − 6 B header),
Zigbee 84 B. *Mitigation:* sweep only within the common range (≤ 84 B). Report the
per-technology ceiling separately as a capability result, never mix it into the comparison.

**6.2 Acknowledgement semantics differ fundamentally.** LoRa is fire-and-forget (raw
SX1278 has no MAC ack at all — `acknowledged` is always false); Zigbee returns a
network-layer APS ack with the module's mesh retries already applied; ESP-NOW returns a
MAC-layer ack. *Mitigation:* count PDR at the receiver for all three. Report the ack rate
separately, per technology, as a different quantity.

**6.3 Duty-cycle regimes differ by law.** 433 MHz is ETSI-limited (`etsi300220`); 2.4 GHz
is not. *Mitigation:* respect each band's legal limit and treat the resulting ceiling on
LoRa's offered load as a genuine result rather than an artifact — a technology that cannot
legally transmit as often *is* worse for a high-rate application. Report offered vs
achieved load for all three so the constraint is visible.

**6.4 Bands propagate differently and this cannot be equalised.** Vegetation attenuation is
frequency-dependent (`itu833`). *Mitigation:* none available — normalise EIRP per `ojo2021`,
then report the band as a property of the technology. Do not claim the comparison isolates
the protocol from its band; it cannot.

**6.5 Interference environments differ.** 2.4 GHz is crowded, 433 MHz much less so.
*Mitigation:* measure interference as an observed metric per run; discard or flag runs whose
observed conditions are not comparable (`boano2018`).

**6.6 Where latency is timestamped changes the answer.** Measured on the bench: a Zigbee
broadcast blocks ~950–1020 ms inside `send()` waiting for its APS status, while a unicast
resolves in ~50 ms. A round-trip metric that starts timing *after* `send()` returns is blind
to all of it and reported 0 ms for Zigbee. *Mitigation:* define the timestamp points
explicitly — application-level send instant to application-level receive instant — and
verify against an independent observer. `becker2025` used a sniffer 5 m from the transmitter
for exactly this reason.

## 7. Experiment design

`jacob2021` (TriScale) supplies the statistics; `jacob2019` **[pending]** the reporting
requirements. TriScale partitions an evaluation into three timescales:

- **run** — one execution of the experiment
- **series** — a set of runs, sampling the distribution of conditions
- **sequel** — a repeated series, used to quantify replicability across time

A **KPI** is a given percentile of a performance metric, estimated with a stated confidence
level as a one-sided non-parametric confidence interval. A **variability score** is a pair
of one-sided CIs across sequels, estimating how similar results would be if the whole
evaluation were repeated.

**How many repetitions.** This follows from the desired percentile and confidence, not from
a round number. TriScale Eq. (4):

```
N ≥ log(1 − C) / log(1 − p)
```

| percentile | C=75% | C=90% | C=95% | C=99% |
|---|---|---|---|---|
| 50th (median) | 2 | 4 | 5 | 7 |
| 25th | 5 | 9 | **11** | 17 |
| 10th | 14 | 22 | 29 | 44 |
| 5th | 28 | 45 | 59 | 90 |
| 1st | 138 | 230 | 299 | 459 |

The 11 reproduces the paper's own worked example (25th percentile at 95 % confidence). The
practical consequence is that **tail claims are expensive**: asserting a 5th-percentile PDR
with 95 % confidence needs 59 runs per point, which for a field campaign at many positions
sets the budget. Choose the percentile deliberately.

The exposé already commits to a repeatability check at a fixed reference position before
going into the field; in TriScale's terms that is a sequel, and it yields the variability
score.

**Position selection.** `becker2025` showed PDR is *not* monotonic in distance — dead links
at 59/61/64 m alongside good ones at 62/65 m. A single "maximum range" figure is therefore
not a safe summary, and positions must be sampled as a grid rather than along a line, with
several positions per distance band.

**Clock synchronisation.** One-way delay is not measurable without it. `becker2025` used
Reference Broadcast Synchronization over the wireless link after rejecting cable (impractical
over terrain) and GPS (accuracy varies under canopy). Their protocol per experiment:
sync → confirmation → start signal → run → collect → reset.

## 8. Literature

**Read for this document.** `ojo2021` (LoRa forest: RSSI/SNR/PDR, payload/SF/CR sweeps,
EIRP normalisation across bands) · `becker2025` (ESP-NOW outdoor: PDR/one-way delay/jitter,
RBS sync, 205 positions, non-monotonic PDR) · `pfister2025`, `pfister2024` (FORTE profile) ·
`boano2018` IoTBench (input/output/observed taxonomy, profiles) · `jacob2021` TriScale
(run/series/sequel, KPIs, Eq. 4) · `sundaram2024` MARS (Zigbee + LoRa radio selection;
throughput/latency only — partial read) · `corke2010` (application framing only).

**Pending retrieval — not yet cited as evidence.**

1. `baccour2012` — Baccour et al., *Radio Link Quality Estimation in Wireless Sensor
   Networks: A Survey*, ACM TOSN 8(4):1–33. Needed to justify demoting RSSI/SNR to observed.
2. `liu2022` — Liu et al., *Comparative Evaluation of the Performance of ZigBee and LoRa
   Wireless Networks in Building Environment*, Electronics 11(21):3560. Already in the `.bib`.
3. `schuss2018` — Schuß, Boano, Römer et al., *Moving Beyond Competitions: Extending D-Cube
   to Seamlessly Benchmark Low-Power Wireless Systems*, CPSBench 2018.
4. `jacob2019` — Jacob, Boano, Raza, Zimmerling, Thiele, *Towards a Methodology for
   Experimental Evaluation in Low-Power Wireless Networking*, CPS-IoTBench 2019.

## 9. Open questions

- Which percentile to claim, given §7's cost table and the number of field positions.
- Whether energy is measured per-node in situ or on the bench under replayed conditions.
**Resolved.** ~~Whether the LoRa duty-cycle ceiling is treated as a result (§6.3) or normalised
away.~~ Settled as a **result**: the EU parameter set in §3.1 is held fixed and never relaxed for
benchmarking, and the runner reports measured airtime against the legal budget. ~~The exact ETSI
figure for the 433 MHz sub-band is an unverified placeholder.~~ Verified against EN 300 220-2 and
ERC-REC 70-03 h1.3 — 10 %, not the 1 % placeholder; see §3.1, which also corrected an
out-of-band centre frequency and a TX power five times the legal ceiling.
