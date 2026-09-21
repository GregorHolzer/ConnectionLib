# Measurement methodology (RQ1)

Working reference for the metric set and experimental procedure used to compare LoRa,
Zigbee and ESP-NOW. This is the answer to the exposé's

> **RQ1.** Which metrics and experimental procedures allow a fair comparison between
> wireless technologies whose link characteristics differ fundamentally?

The exposé states that the procedure itself, not only the numbers it produces, is a result
of the thesis. This document is therefore structured as *justification*, not as a list:
every metric is tied to the source that motivates it, and every place where the three
technologies cannot be made comparable is named explicitly rather than averaged over.

Status: every source in §8 has been read in full, and no claim below rests on an unretrieved one.
The convention stands for future additions — claims resting on sources not yet retrieved are marked
**[pending]** and must not be cited until checked.

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
| Supply voltage | 3.9 V, regulated by the PPK2 source meter | Energy is measured in situ (§9), so the node runs from the meter rather than its cell at every position. A fixed supply is what makes energy figures comparable between positions as well as between technologies |
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

RSSI and SNR are deliberately **demoted to observed metrics** (§5), not comparison axes. The first
reason is availability — neither is exposed on ESP-NOW. The second is that `baccour2012`, surveying
a decade of link-quality estimation, shows they are unfit for the job even where the radio does
expose them:

- **They are measured only on packets that arrived.** This is the decisive objection. Hardware
  estimators are read out of the transceiver on reception, so a link losing most of its packets
  reports the signal quality of its survivors and *overestimates* itself. The bias grows exactly
  where the link gets bad — which, per §7, is the region this comparison is aimed at.
- **They resolve good links, not intermediate ones.** Above a threshold (~−87 dBm in
  `baccour2012`'s cited study) PRR is consistently ~99 %, so a single reading answers "is this link
  excellent?" well. Below it, a shift as small as **2 dBm flips a good link to a bad one**, and the
  survey reports that neither RSSI nor LQI can differentiate links there at all.
- **The threshold is a property of the environment, not the radio.** It is reported at −90 dBm on a
  grass field, −91 dBm on a parking lot and −89 dBm in a corridor. A forest would need its own
  calibration before any RSSI figure could be read as a quality claim.
- **The SNR→PRR mapping depends on hardware and on temperature.** The theoretical route from SNR
  through bit error rate to PRR exists, but the survey concludes SNR "cannot be used as a
  standalone estimator". Temperature entering here is the same mechanism §5 records via `boano2014`.

The survey's own framing is that this literature provides "radically different (and sometimes
contradictory) results" — whether RSSI or LQI is the better estimator is, in its words, an
unanswered question. A quantity whose interpretation is contested, environment-calibrated and
biased on failing links is evidence about conditions, not an axis on which to rank three radios.

`liu2022` arrives at the same demotion from the application side — most prior comparisons report
RSSI, SNR and PDR, and it drops the first two because end users experience latency and delivered
data, not signal strength.

## 5. Observed metrics

Recorded per run to establish that runs were comparable; not compared between technologies.

- **RSSI / SNR** where the radio exposes them — recorded as conditions, with the caveat from §4
  that they describe only the packets that arrived
- **External interference** — 2.4 GHz shares spectrum with Wi-Fi; 433 MHz does not. This
  asymmetry is a threat to fairness (§6.5) and must be measured, not assumed away
- **Vegetation state** — species, density, foliage/season, wetness. Attenuation is
  frequency-dependent (`itu833`), so this interacts with the band asymmetry
- **Temperature** — on-board, not just ambient. `boano2014` (TempLab) reports that normal outdoor
  variation moves on-board temperature by up to 35 °C in an hour, which is enough to shift the
  crystal by >100 ppm and to cut received signal strength by more than 6 dB — sufficient, in the
  traces they reproduce, to take a link from 100 % to 0 % packet reception over a single day. It
  also introduces a *dynamic heterogeneity*: two nodes with identical parameters but different
  on-board temperatures perform differently, and nodes physically close together can have very
  different temperature profiles. Under a forest canopy that is a per-position property — sun
  flecks do not fall evenly — so it is recorded per run at both ends of the link
- **Time of day / date** of each run

## 6. Fairness threats and mitigations

This is the substance of RQ1. Prior work compares within a band (`ojo2021`) or pairwise
(`liu2022`); comparing all three at once raises problems none of them face.

`liu2022` is the closest published comparison to this one and is worth stating precisely, because
its design decisions diverge from this document's at three points. It compares ZigBee against LoRa
in a 12-storey concrete building in Dalian across three scenarios — line-of-sight, horizontal
(same floor) and vertical (floors 4–12, at both a central and a window position) — sweeping baud
rate and packet length, and reports exactly two metrics: **RTT and PLR**. It concludes LoRa wins on
penetration, PLR and usually RTT, with ZigBee cheaper per module and lower-power.

- **Energy is excluded by assumption, not measured.** The paper states plainly that buildings have
  mains power, so battery life is not its concern; its only energy claim is that a single LoRa
  module "may be three times" a ZigBee one, in a qualitative discussion. For FORTE's months-on-
  battery satellites that is the binding requirement, which is why §4 makes energy per *delivered*
  payload a primary axis rather than a closing remark.
- **Transmit power is not equalised.** LoRa is configured at **30 dBm** — 1 W, a hundred times the
  10 mW e.r.p. that §3.1 is held to, and not a power any EU deployment may use. Whatever margin
  that buys is folded into "LoRa outperforms ZigBee". This is §6.4's threat appearing in published
  work, and the reason EIRP normalisation is an input parameter here.
- **There are no repetitions in the statistical sense.** RTT is the mean of 10 manually sent
  packets and PLR the ratio over 500, one measurement per condition, with no confidence intervals.
  §7's KPIs exist to make claims that survive the variability such a design cannot see.

None of this makes the paper wrong on its own terms — a mains-powered building study reasonably
ignores energy. It makes it non-substitutable for this comparison, and it is the concrete case for
why RQ1 needed deriving rather than borrowing.
Items 1, 2 and 6 were confirmed empirically during hardware bring-up (see
[`hardware.md`](hardware.md), [`architecture.md`](architecture.md)).

**Why these are handled by procedure rather than by a facility.** The established answer to
comparability in this field is a benchmarking testbed. `schuss2018`'s D-Cube is the reference
example: target nodes individually switched over PoE, observer modules tracing GPIO and current per
node, interference *generated* to a specified level by dedicated Wi-Fi nodes, NTP synchronisation
well below 10 µs, and experiments scheduled at night or on public holidays specifically to keep
uncontrolled interference out of the measurement. The motivation given is the one this document
starts from — without a standard methodology, divergence across experimental setups makes results
from different authors impossible to compare.

None of that is available in a forest. Interference cannot be generated to a set level, runs cannot
be deferred to a quiet night in a spectrum nobody controls, and the deployment environment is the
object of study rather than a nuisance to be suppressed. Every control a facility would supply must
therefore be replaced by a *recorded* observation (§5) plus a rule for discarding non-comparable
runs. It is worth noting that even D-Cube treats recording environmental effects and controlling
temperature as future work, so this gap is not an artefact of working outdoors on a budget.

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

**6.5 Interference environments differ, and the environment includes the experimenter.** 2.4 GHz is
crowded, 433 MHz much less so. *Mitigation:* measure interference as an observed metric per run;
discard or flag runs whose observed conditions are not comparable (`boano2018`).

`liu2022` reports the sharpest available warning here: in their vertical-central experiment, LoRa's
PLR on the top floor went from **100 % with nobody nearby to under 10 % when a researcher stood
near the transmitter**. An uncontrolled and unrecorded factor — where the person running the
experiment was standing — inverted the result completely. A field protocol must therefore fix and
record the operator's position along with the radio's, and §7's clock-sync sequence is the natural
place to pin it down.

**6.6 Where latency is timestamped changes the answer.** Measured on the bench: a Zigbee
broadcast blocks ~950–1020 ms inside `send()` waiting for its APS status, while a unicast
resolves in ~50 ms. A round-trip metric that starts timing *after* `send()` returns is blind
to all of it and reported 0 ms for Zigbee. *Mitigation:* define the timestamp points
explicitly — application-level send instant to application-level receive instant — and
verify against an independent observer. `becker2025` used a sniffer 5 m from the transmitter
for exactly this reason.

## 7. Experiment design

`jacob2021` (TriScale) supplies the statistics; `jacob2019`, by the same first author, supplies the
reporting requirements. TriScale partitions an evaluation into three timescales:

- **run** — one execution of the experiment
- **series** — a set of runs, sampling the distribution of conditions
- **sequel** — a repeated series, used to quantify replicability across time

A **KPI** is a given percentile of a performance metric, estimated with a stated confidence
level as a one-sided non-parametric confidence interval. A **variability score** is a pair
of one-sided CIs across sequels, estimating how similar results would be if the whole
evaluation were repeated.

**What must be reported.** `jacob2019` decomposes an evaluation into *experiments* → *analysis* →
*synthesis* and insists the methodology answer five questions in advance: which metrics to compute,
which raw data to collect, how many samples, how to synthesise them, and how many experiments to
run. Three of its prescriptions bind this document:

- **Do not synthesise with the mean and standard deviation.** Low-power wireless measurements are
  routinely non-normal, so mean-and-variance summaries are inappropriate however common they are.
  Report percentiles with non-parametric CIs — which is what the KPIs above already are.
- **Report the conservative bound of the CI, not its centre.** The direction depends on the metric:
  for PDR, where higher is better, report the **lower** bound; for energy, where lower is better,
  report the **upper** bound. A run that quotes the point estimate is claiming more than it measured.
- **Collect raw data as fine-grained as the harness allows.** Their example is exactly this
  project's: store send and receive timestamps rather than their difference, because the difference
  yields only latency while the pair also yields jitter. The `sendUs`/`rttUs` split forced by §6.6
  satisfies this, and it is the reason the Zigbee timestamping fault was recoverable at all.

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
sets the budget.

**Decision: the 25th percentile at 95 % confidence, so N = 11 runs per test point.** The median was
rejected deliberately. `becker2025`'s central finding is that links fail non-monotonically — dead at
59, 61 and 64 m while working at 62 and 65 m — so the interesting behaviour of these technologies in
a forest is in the bad links, and a median says nothing about them. The 25th percentile reaches into
that lower tail while staying affordable, and it is the percentile TriScale itself works through.
Going further down costs disproportionately: the 10th needs 29 runs and the 5th needs 59, which at
any realistic number of positions is a campaign that does not finish.

This is also why §4 keeps PDR rather than RSSI as the primary reliability axis. `baccour2012`'s
hardware estimators are blindest precisely in the region a 25th-percentile claim is about, so the
two decisions have to agree: a tail claim needs a metric that counts failures.

> `jacob2019` gives 6 samples for a 95 % CI on the median where §7's table gives 5. The difference
> is one-sided versus two-sided intervals; this document follows TriScale's one-sided convention
> throughout, since every claim here is directional ("PDR is at least…", "energy is at most…").

**N = 11 collides with LoRa's duty cycle, and the collision decides the run length.** From §3.1, one
100-exchange sweep over 8/16/32/84 B costs ~148 s of airtime against a legal budget of 360 s per
hour. Eleven of those is ~1630 s of airtime, so:

```
11 runs × 148 s airtime ÷ 360 s per hour ≈ 4.5 hours of wall clock per position — LoRa alone
```

A dozen positions would be ~54 h of LoRa field time before setup, clock sync or the other two
technologies. That is not a campaign anyone completes, and it is a constraint on the *run*, not on
the percentile: N = 11 is fixed by the claim being made, whereas the 100 exchanges per payload size
are an input parameter (§3) inherited from the bench ping-pong. Shortening the run is the lever, and
`jacob2019` independently recommends exactly that shape — *"many short tests rather than a few long
tests"* — because repetitions buy confidence and length does not.

The benchmark suite has already taken this step for its own campaign: `bench/technologies/lora.py`
sets `EXCHANGES = 20`, commented *"deliberately lower than the 2.4 GHz benchmarks; airtime is the
constraint"*. Carrying that number into the field gives ~29.6 s of airtime per run and

```
11 runs × 29.6 s airtime ÷ 360 s per hour ≈ 0.9 hours per position — LoRa alone
```

which is a campaign that fits in a working day per handful of positions. **20 is therefore the
presumptive value, but the choice is recorded as open** (§9): the bench figure was set for an
OFAT sweep over 16 radio configurations, whereas the field campaign holds §3.1's parameters fixed
and varies position instead, so the number is a precedent rather than a derivation. §5's observed
metrics are what will show whether a 20-exchange run still spans comparable conditions.

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
throughput/latency only — partial read) · `corke2010` (application framing only) ·
`jacob2019` (experiments/analysis/synthesis, the ban on mean-and-variance synthesis,
conservative CI bounds, raw-data granularity, short-tests-over-long-tests) · `schuss2018` D-Cube
(what a benchmarking *facility* controls, and why a field study cannot) · `boano2014` TempLab
(temperature's effect on link quality, clock drift and battery, and its heterogeneity across
nearby nodes) · `liu2022` (ZigBee vs LoRa in a concrete building; RTT/PLR only, LoRa at an
unequalised 30 dBm, and the operator-proximity effect in §6.5) · `baccour2012` (link-quality
estimation survey; why RSSI/SNR cannot serve as comparison axes — §4).

**Pending retrieval.** None — as of 2026-09-21 every source cited here has been read in full.

`jacob2019`, `schuss2018`, `boano2014` and `baccour2012` are **not yet in**
`~/Nextcloud/Uni/bachelor/thesis/literature.bib` and need entries before the thesis section cites
them. PDFs are in `~/Nextcloud/Uni/bachelor/related-work/`; author copies of the first three are
free at `carloalbertoboano.com/documents/`.

## 9. Open questions

**Open.**

- **How many exchanges per test point.** Raised by the percentile decision below: the ping-pong
  harness's 100 exchanges per payload size make a single LoRa run cost ~148 s of airtime, and
  eleven of those exceed four hours per position under the 10 % duty cycle. The benchmark suite
  already runs LoRa at 20 (`bench/technologies/lora.py`), which would give ~0.9 h per position, so
  20 is the presumptive answer — but it was chosen for a different sweep and should be confirmed
  rather than inherited. Must be fixed before the field campaign is scheduled. See §7.

**Resolved.**

- ~~Which percentile to claim, given §7's cost table and the number of field positions.~~ The
  **25th percentile at 95 % confidence, N = 11 runs per point.** The median was rejected because
  `becker2025`'s non-monotonic failures put the interesting behaviour in the lower tail, which a
  median cannot see; the 10th and 5th percentiles reach further into that tail but cost 29 and 59
  runs per point. See §7 for the derivation and for the duty-cycle consequence.
- ~~Whether energy is measured per-node in situ or on the bench under replayed conditions.~~
  **In situ, at each field position.** Composing energy analytically from bench per-attempt cost and
  field PDR would assume the retransmission behaviour it is trying to measure; measuring in place
  captures it directly and makes energy-per-delivered-payload a per-position output metric rather
  than a bench constant. Three consequences are accepted openly:
  - Every position must be reachable with a host laptop and the PPK2, which constrains the position
    grid in §7 — a position that cannot be instrumented cannot contribute an energy figure.
  - The instrumented node runs with its cell removed and its USB VBUS cut, sourced at a fixed 3.9 V
    (§3). This perturbs the device, but identically for all three technologies, so it does not bias
    the comparison it is used for.
  - A fixed supply also means cell behaviour is **excluded by construction**. `boano2014` lists
    early battery depletion among temperature's effects; that mechanism is outside what this
    measurement can see, and a lifetime claim derived from these figures must say so.
- ~~Whether the LoRa duty-cycle ceiling is treated as a result (§6.3) or normalised away.~~ Settled
  as a **result**: the EU parameter set in §3.1 is held fixed and never relaxed for benchmarking,
  and the runner reports measured airtime against the legal budget.
- ~~The exact ETSI figure for the 433 MHz sub-band is an unverified placeholder.~~ Verified against
  EN 300 220-2 and ERC-REC 70-03 h1.3 — 10 %, not the 1 % placeholder; see §3.1, which also
  corrected an out-of-band centre frequency and a TX power five times the legal ceiling.
