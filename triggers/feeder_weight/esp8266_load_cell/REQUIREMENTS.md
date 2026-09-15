# Feeder Weight Monitor — Reimplementation Requirements

Behavioural specification for re-implementing the hummingbird feeder weight monitor on a
new platform. Derived from the reference implementation in
`src/main.cpp`, `platformio.ini`, `README.md`, and the calibration sketch at
`../load_cell_calibration/src/main.cpp`.

**Scope.** Business logic and external interfaces only. Makes no assumption about MCU,
load cell amplifier, or ADC. Network transport, WiFi association, and OTA firmware update
are assumed to be provided by the target platform and are **out of scope** except where
the application logic must observe them (RSSI reporting, connection loss behaviour).

Each requirement is tagged:

- **[AS-BUILT]** — reproduces reference behaviour exactly. Implement as written.
- **[FIX]** — reference behaviour is defective; the requirement states the corrected
  behaviour. Rationale in §11.
- **[DECIDE]** — genuine open question, see §12. A default is given so implementation is
  not blocked.

---

## 1. System context

One monitor device per feeder. A hummingbird feeder hangs from (or rests on) a load cell.
The device continuously measures weight, detects the transient weight change caused by a
bird landing on the feeder, and publishes both the event and the underlying signal to an
MQTT broker. A downstream consumer subscribes to the trigger event and starts video
capture (`../../opencv-rtsp/capture_on_mqtt.py`).

```
                     ┌──────────── Device (this spec) ────────────┐
  load cell ──▶ amp/ADC ──▶ [§3 acquisition] ──▶ [§4 baseline] ──▶ [§5 trigger FSM]
                     │            │                    │                  │
                     │            ▼                    ▼                  ▼
                     │      /raw_batch            /average           /trigger
                     │            │                    │                  │
                     └────────────┴──────── MQTT ──────┴──────────────────┘
                                              │
                                              ▼
                                    video capture trigger
```

Three devices exist today, feeder IDs `1`, `2`, `3`.

## 2. Definitions

| Term | Meaning |
|---|---|
| **Conversion** | One completed ADC reading from the amplifier. |
| **Sample** | One filtered weight value produced by §3, in grams. This is the unit of everything downstream — batches, deltas, loop iterations. |
| **Baseline** | Slowly-adapting estimate of the unoccupied feeder weight (`calculatedAverage` in the reference). |
| **Delta** | `abs(baseline − sample)`, in grams. |
| **Settled** | Delta at or below the hysteresis band; the feeder is considered unoccupied. |
| **Loop iteration** | One pass of the main cycle: acquire one sample, evaluate the trigger FSM, append to batches, service I/O. Exactly one sample and one RSSI reading per iteration. |

## 3. Signal acquisition

The reference stacks two filters, one inside the HX711 driver library and one in
application code. The **combined** response is the requirement; a new implementation is
free to realise it differently as long as the response matches.

**FR-3.1 [AS-BUILT] — Amplifier-level filter.** Maintain a sliding window of the most
recent 18 conversions. Discard the single highest and single lowest value in the window,
and take the arithmetic mean of the remaining 16. *(Reference: `HX711_ADC` `config.h`
`SAMPLES=16`, `IGN_HIGH_SAMPLE=1`, `IGN_LOW_SAMPLE=1`; `smoothedData()`.)*

**FR-3.2 [AS-BUILT] — Application-level filter.** Average 5 consecutive outputs of
FR-3.1, taken one per new conversion. The result is one **sample**.
*(`main.cpp:265-282`, `AVERAGING_WINDOW_LENGTH = 5`.)*

> **Consequence to preserve.** The 5 values averaged in FR-3.2 come from an
> 18-deep *sliding* window, so they overlap heavily. One sample is not an
> independent 90-conversion average; its effective settling time is ~22 conversions and
> its noise rejection is closer to an 18-conversion trimmed mean than to a 90-conversion
> one. A naive "average 90 conversions" reimplementation will be measurably slower to
> respond and will miss short landings. **One sample costs 5 conversions of wall time.**

**FR-3.3 — Calibration.** Convert conversions to grams as
`grams = (filtered_counts − tare_offset) / calibration_factor`. Both terms are per-device
(§8). `calibration_factor` may be negative, which inverts sign for load cells wired in
the opposite polarity — do not take the absolute value.

**FR-3.4 [DECIDE] — Tare acquisition.** *Reference behaviour:* the device tares at every
boot, after a 2400 ms stabilisation period, **with the feeder in place**
(`main.cpp:372`, `LoadCell.start(2000, true)`). All reported weights are therefore
*relative to the load present at boot*, not absolute feeder mass, and every reboot
silently redefines zero. Default for the new system: **persist the tare offset from
calibration and do not re-tare at boot** (§12-A).

**FR-3.5 — Sample rate.** The loop is paced by conversion availability, not by a timer;
there is no artificial loop delay (`LOOP_DELAY_MS = 0`, `main.cpp:260`). Sample period is
therefore `5 / conversion_rate` plus I/O overhead. The new implementation MUST publish
its actual sample rate as a configuration value and MUST size §6 batches from it (see
FR-6.6), because the reference's fixed batch counts only work at one particular rate.

**FR-3.6 — Sensor fault.** If the sensor does not produce conversions within a bounded
startup timeout, the device MUST report the fault on `/message` and `/status` and then
restart or retry. **[FIX]** — the reference calls `halt()`, an infinite `delay()` loop
that never reports anything and never recovers (`main.cpp:88-92, 374-378`).

## 4. Baseline tracking

**FR-4.1 [AS-BUILT] — Startup baseline.** After acquisition is up, take 5 samples and
average them; that mean is the initial baseline. Mark the device `initialized`.
Publish an initial cleared trigger (§5.7). *(`main.cpp:284-304, 392`.)*

**FR-4.2 [AS-BUILT] — Baseline update.** On each loop iteration where the FSM reaches the
**Settled** branch (§5.4), update:

```
baseline := 0.9 * baseline + 0.1 * sample
```

**FR-4.3 [AS-BUILT] — Baseline freeze.** The baseline MUST NOT be updated on any
iteration that does not reach the Settled branch. While a bird is present, or while the
weight sits anywhere above the hysteresis band, the baseline is frozen. This is
deliberate: it prevents a feeding bird from being absorbed into the baseline.

> **Consequence.** A load change larger than `TRIGGER_MAX` (40 g) — refilling the
> feeder, or the feeder being knocked — freezes the baseline permanently. The device
> stops publishing `/average`, stops detecting triggers, and only recovers on reboot.
> See FR-5.8 and §12-B.

## 5. Trigger detection

Evaluated once per loop iteration, after the sample is acquired. Let
`delta = abs(baseline − sample)`.

Constants **[AS-BUILT]** (`main.cpp:243-246`):

| Name | Value | Meaning |
|---|---|---|
| `TRIGGER_MIN` | 1.75 g | Below this, not a bird. |
| `TRIGGER_MAX` | 40.0 g | Above this, not a bird — a refill or a knock. |
| `HYSTERESIS` | 1.25 g | Settled band; must fall back inside it to clear. |
| `TRIGGER_RESET_INTERVAL` | 100 samples | Cap on a single continuous trigger. |

**FR-5.1 — Trigger branch.** If `TRIGGER_MIN < delta < TRIGGER_MAX` **and**
`triggered_readings < TRIGGER_RESET_INTERVAL` **and** the device is `initialized`:
if not already triggered, set triggered and publish a trigger-asserted event (§5.7).
Increment `triggered_readings`. Comparisons are strict on both bounds.

**FR-5.2 — Hold branch.** Else if `delta > HYSTERESIS`: increment `triggered_readings`
and take no other action. The weight has not returned to the settled band, so the bird is
assumed still present and the excursion is treated as noise. Trigger state is unchanged —
in particular, **an asserted trigger is not cleared here.**

**FR-5.3 — Settled branch.** Else (`delta <= HYSTERESIS`): if currently triggered,
publish a trigger-cleared event (§5.7), clear triggered, and reset
`triggered_readings` to 0. Then update the baseline per FR-4.2 and consider publishing
`/average` per FR-5.6.

**FR-5.4 — Counter reset.** `triggered_readings` MUST be reset to 0 **only** in the
Settled branch and **only** when a trigger was asserted. **[FIX]** — in the reference it
is a `uint8_t` incremented without bound in the Hold branch, so it wraps at 255 and can
silently re-enable the Trigger branch mid-event. Use a saturating counter of at least 16
bits.

**FR-5.5 — Long-event cap.** Once `triggered_readings` reaches
`TRIGGER_RESET_INTERVAL`, the Trigger branch is disabled for the remainder of the event.
Because FR-5.2 does not clear, the effect is that a bird staying longer than 100 samples
stops re-arming; the event still does not clear until the weight settles. The trigger
remains asserted throughout.

**FR-5.6 [AS-BUILT] — Average publication cadence.** Maintain a monotonic count of loop
iterations since startup. In the Settled branch, when that count is a multiple of 20,
publish the current baseline to `/average`. Because the counter advances on every
iteration but the publish only happens in the Settled branch, `/average` messages are
irregular and may be skipped entirely during activity. *(`main.cpp:481-488`.)*

**FR-5.7 — Trigger event.** Publish to `/trigger` on assertion and on clear only — not
on every triggered sample. Payload per FR-7.4.

**FR-5.8 [FIX] — Re-baseline recovery.** If the device remains outside the settled band
continuously for a configurable interval (default: **300 s**), it MUST re-run FR-4.1 to
re-acquire the baseline at the new load level, clear any asserted trigger, and publish a
`/message` noting the re-baseline. This closes the FR-4.3 lockup on refill. Without it,
a refill requires a manual reboot.

## 6. Telemetry batching

Two independent batches: weight samples and RSSI. Both accumulate one entry per loop
iteration.

**FR-6.1 — Batch trigger.** Append the value, then publish when the buffered count
**exceeds** the batch size. With the reference batch size of 256 this means **257 entries
are published per batch**, not 256 (`main.cpp:429, 499`). Preserve or correct this
deliberately; do not reproduce it by accident.

**FR-6.2 — Batch duration.** Record a timestamp immediately **after** the first entry of
a batch is appended. On publish, report `duration` = milliseconds from that timestamp to
the publish attempt. There is no absolute start timestamp in the payload; consumers
reconstruct per-sample times by dividing `duration` by entry count.
**[DECIDE]** — see §12-C; an absolute epoch timestamp is strongly preferred.

**FR-6.3 — Publish failure retry.** If the publish fails, retain the buffer and retry on
each subsequent batch-size crossing, continuing to accumulate. Both `duration` and entry
count grow across retries and remain mutually consistent.

**FR-6.4 — Overflow drop.** If the buffered count exceeds **1024**, discard the batch and
reset the buffer regardless of publish success. This bounds memory and staleness.
*(`main.cpp:439, 507`.)*

**FR-6.5 [FIX] — Independent RSSI reset.** The RSSI batch MUST reset its buffer and
timestamp independently of the weight batch. In the reference both counters cross the
threshold on the same iteration, and a weight-publish failure desynchronises them
permanently, so `duration` on the two topics stops being comparable.

**FR-6.6 [FIX] — Size batches by payload, not by count.** The batch MUST be flushed when
the *serialised payload* would exceed the transport's maximum publishable payload, or
when the entry count is reached, whichever comes first. The reference fixes the count at
257 against a 2048-byte MQTT buffer (`main.cpp:190`); at ~7 bytes per formatted value
plus separators a batch lands at roughly 1.8–2.1 kB and **fails intermittently once
readings exceed two integer digits**, which is exactly what FR-6.3 exists to paper over.

**FR-6.7 — RSSI sampling.** Sample link RSSI once per loop iteration, at the same cadence
as weight, and record it as a signed integer in dBm (one byte suffices; range
approximately −100 to 0).

## 7. MQTT interface

All topics are rooted at `sensors/feeders/{FEEDER_ID}/`. `FEEDER_ID` is the decimal
device number (`"1"`, `"2"`, `"3"`), no zero padding, no leading slash on the root.

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `/status` | pub | yes | `online` / `offline` |
| `/reset` | pub | yes | decimal reset reason code |
| `/raw_batch` | pub | no | batch object, float array |
| `/average` | pub | no | formatted float, grams |
| `/trigger` | pub | no | trigger object |
| `/message` | pub | no | free-text debug string |
| `/rssi_batch` | pub | no | batch object, int array |
| `/control` | **sub** | — | command string |
| `/watchdog` | **sub** | — | any (liveness ping) |

**FR-7.1 — Connection identity and LWT.** Connect with client ID equal to the device
hostname. Register a Last Will on `/status`, QoS 1, retained, payload `offline`. On
successful connect, publish `online` retained to `/status` and the reset reason code
retained to `/reset`. *(`main.cpp:144-147`.)* Client IDs MUST be unique across devices;
a collision disconnects the earlier client (`README.md`, Troubleshooting).

**FR-7.2 — Subscriptions.** On every successful connect, subscribe to **both**
`/control` and `/watchdog`. **[FIX]** — the reference subscribes only to `/control`
(`main.cpp:162`), which is the root cause of the reboot loop in §11-A.

**FR-7.3 — Batch payload schema.** Both `/raw_batch` and `/rssi_batch`:

```json
{"duration": <uint, milliseconds>, "data": [ <v>, <v>, ... ]}
```

Weight values are formatted to 3 decimal places, minimum field width 5
(`0.000`, `-1.234`, `140.625`). RSSI values are plain signed integers. No whitespace
between tokens.

**FR-7.4 — Trigger payload schema.** Reference emits, verbatim including the leading
space and the integer-valued `value` field:

```json
{ "value": 1, "delta": 3.42}
```

`value` is `1` asserted / `0` cleared. `delta` is formatted to 2 decimal places. On
clear, `delta` carries the settling delta, not the peak. **[DECIDE]** — this contradicts
both `CLAUDE.md` ("true/false with delta") and the only known consumer, which does
`payload.lower() == 'true'` against the whole payload
(`../../opencv-rtsp/capture_on_mqtt.py:28`) and therefore never matches. See §12-D.

**FR-7.5 — Average payload.** Bare formatted float in grams, 3 decimal places, e.g.
`-0.412`. Not JSON, no unit suffix.

**FR-7.6 — Reconnection.** On loss of broker connection, attempt reconnect. If the
network link is also down, re-establish it first. After **10** consecutive failed
attempts, restart the device. Back off ~3 s between MQTT attempts and ~5 s after a link
re-establishment. *(`main.cpp:131-163`.)*

**FR-7.7 [FIX] — Service the MQTT client every iteration.** Inbound message dispatch and
keepalive MUST be serviced on every loop iteration. **The reference services the client
only inside the raw-batch branch** (`main.cpp:434, 444`), i.e. roughly once per 257
samples. Two consequences, both observed-behaviour bugs: keepalive PINGREQ is never sent
within the default 15 s keepalive window, so the broker drops the connection and the
device reconnects continuously; and a `/control` command is not acted on until the next
batch boundary.

**FR-7.8 — Delivery guarantees.** Data publishes are fire-and-forget (QoS 0). A
"successful" publish means the message was handed to the transport, not that the broker
received it. Only the LWT uses QoS 1. Consumers MUST tolerate gaps in `/raw_batch` and
`/rssi_batch`; the `duration` field, not message arrival order, is the source of timing
truth.

**FR-7.9 — Debug echo.** When a debug flag is set, every diagnostic line written to the
local console is also published to `/message`. The flag defaults off and is enabled only
around the batch-failure path in the reference (`main.cpp:445-448`). An implementation
MUST guard against re-entry: a publish failure inside the debug echo path must not itself
attempt to publish.

## 8. Per-device configuration

**FR-8.1** The following are per-device and MUST be settable without editing shared
logic — build-time flags are acceptable, runtime configuration is preferred:

| Item | Reference value(s) |
|---|---|
| Feeder ID | `1`, `2`, `3`; `999` if unset (with a build warning) |
| Hostname | `feedermonitor{N}` — see §11-C |
| Calibration factor | `1`: `-915.044983`  `2`: `953.304993`  `3`: `906.79` |
| Tare offset | not currently persisted — see FR-3.4 |
| Broker host / port | `sleeperservice.local` : 1883 |
| Broker credentials | user `sensor_writer` |

**FR-8.2 [FIX] — Credential handling.** WiFi PSK, MQTT password, and OTA password are
hard-coded string literals in `src/main.cpp` and `platformio.ini`, and are committed to
git. The new implementation MUST source them from a non-committed configuration file,
provisioning partition, or build secret. These credentials should be treated as
compromised and rotated.

**FR-8.3 — Missing ID.** Building or provisioning without a feeder ID MUST fail loudly.
The reference falls back to ID `999` with a compile warning, which is easy to miss and
produces a device that publishes to a topic nobody watches.

## 9. Liveness and control

**FR-9.1 — Hardware watchdog.** Enable the platform hardware watchdog with a 30 s
timeout. The main loop must feed it; blocking operations must not exceed it.
*(`main.cpp:7, 384`.)*

**FR-9.2 — External watchdog.** Maintain a countdown initialised to **120 000 ms**,
decremented by the wall-clock time elapsed each loop iteration. Any message received on
`/watchdog` resets it to the full interval. On reaching zero, flush pending output and
reset the device. This exists so an external supervisor can force recovery of a device
that is connected but not functioning.

**FR-9.3 [FIX] — Watchdog must be defeatable.** Because FR-9.2 depends on an external
supervisor that may not exist, the countdown MUST either be disabled by default and
armed only after the first `/watchdog` message is received, or be configurable to
disabled. See §11-A.

**FR-9.4 — Control commands.** On `/control`, payload `reset` (exact, case-sensitive)
restarts the device. Unrecognised payloads are logged and ignored.

**FR-9.5 — Reset reason.** Capture the platform reset reason and exception cause at the
earliest point in startup, before any subsystem initialisation, and publish the reason
per FR-7.1. Retaining it means a consumer can see why a device last restarted without
being connected at the time.

## 10. Startup sequence

**FR-10.1** In order (`main.cpp:357-393`):

1. Capture reset reason / exception cause.
2. Initialise console output.
3. Initialise the sensor; stabilise; acquire or load tare (FR-3.4). On timeout, FR-3.6.
4. Apply the calibration factor.
5. Enable the hardware watchdog.
6. Bring up network, OTA, and broker connection (out of scope, but must complete before
   step 7 so startup diagnostics are publishable).
7. Acquire the startup baseline (FR-4.1).
8. Publish an initial cleared trigger, `{"value": 0, "delta": 0.00}`.

**FR-10.2** OTA must remain serviceable during the blocking acquisition loops of steps 3
and 7, otherwise a device that fails calibration cannot be recovered over the air. The
reference does this by calling the OTA handler inside the sample-averaging wait
(`main.cpp:275, 290`); on the new platform, ensure the equivalent.

---

## 11. Defects in the reference implementation

These are behaviours a faithful port would inherit. Each is addressed by a **[FIX]**
requirement above.

**A. The device reboots roughly every two minutes.** `watchdog_timer` starts at 120 000 ms
and is decremented every loop (`main.cpp:406-415`). The only thing that resets it is a
message on `/watchdog` — and `/watchdog` **is never subscribed to**
(`main.cpp:162` subscribes `/control` only), so the broker never delivers one. The
countdown therefore always reaches zero and calls `ESP.reset()`.

This has knock-on effects that likely explain other observed symptoms: at a 10 SPS
conversion rate one sample takes ~500 ms, so a 257-sample raw batch needs ~128 s of
wall time — longer than the 120 s reboot cycle, meaning `/raw_batch` would **never**
publish. It also means the boot-time tare (FR-3.4) is re-taken every two minutes, so
`/average` is not comparable across any window longer than that. Fixed by FR-7.2 and
FR-9.3.

**B. MQTT is serviced once per batch, not once per loop.** See FR-7.7. Causes continuous
keepalive-driven reconnection and up-to-a-full-batch latency on `/control`.

**C. Hostname / OTA target mismatch.** Uncommitted working-tree changes rename hostnames
from `feedermonitor01` to `feedermonitor1`, but `platformio.ini` still targets
`feedermonitor01.local`, `feedermonitor02.local`, `feedermonitor03.local` for OTA, and
the calibration sketch still uses the zero-padded form. Pick one convention and apply it
to hostname, mDNS name, and OTA target together. Note that renaming the hostname also
renames the MQTT client ID (FR-7.1).

**D. Trigger payload does not match its documented contract or its consumer.** See
FR-7.4. `CLAUDE.md` documents `true`/`false`; the code emits `1`/`0` inside a JSON
object; the video-capture consumer string-compares the whole payload to `'true'` and
subscribes to `/sensors/feeders/2/trigger` — **with a leading slash**, a different topic
from the one published to. As written, the video trigger cannot fire. Verify against the
actually-deployed consumer before freezing the payload format.

**E. Batch payload can exceed the transport buffer.** See FR-6.6.

**F. `triggered_readings` overflow.** See FR-5.4.

**G. Unreachable / dead code.** `dtostrf` results are computed into the shared `fmt`
buffer and then discarded in `publishTrigger` (`main.cpp:307-310`); the trigger-clear
path prints a stale `fmt` (`main.cpp:476`); `reset_exception` is captured and never
published (`main.cpp:109, 361`); a single global `fmt` buffer is shared by all formatting
paths and is not re-entrant.

## 12. Open questions

**A. Tare persistence (FR-3.4).** Should the device measure absolute feeder mass, or
weight change relative to boot? Absolute mass enables tracking syrup consumption over
days — apparently the point of the project — but requires persisting the tare offset and
a documented re-tare procedure. *Default: persist the tare; do not re-tare at boot.*

**B. Refill handling (FR-5.8).** Should a large step change re-baseline automatically
(risking absorbing a genuine event), or require an explicit `/control` command such as
`rebaseline`? *Default: automatic after 300 s outside the settled band, plus a
`rebaseline` control command.*

**C. Batch timestamps (FR-6.2).** Add an absolute start timestamp to batch payloads?
Requires a time source on the device. *Default: yes if the platform has NTP; keep
`duration` as well for backward compatibility.*

**D. Trigger payload format (FR-7.4).** Match the deployed consumer, match `CLAUDE.md`,
or define a new schema and update consumers? *Default: keep the JSON object, change
`value` to a JSON boolean, and update the consumer to parse JSON — but confirm against
the running consumer first.*

**E. Trigger thresholds.** `TRIGGER_MIN` of 1.75 g against a hummingbird mass of roughly
3–6 g implies partial weight transfer (perching while hovering). These constants were
tuned empirically on the existing hardware. If the new mechanical arrangement differs —
different load cell, different mounting, different noise floor — they MUST be re-tuned
against recorded `/raw_batch` data rather than carried over. `hummingbird_data_raw.txt`
at the project root (565k lines, `[ISO8601] <grams>`, ~1.2 Hz, absolute weights around
970 g) is a usable starting corpus — and is itself evidence that an earlier revision
recorded **absolute** feeder mass, which bears on §12-A.

## 13. Calibration procedure

Requirements on the calibration tooling, derived from `README.md` and
`../load_cell_calibration/src/main.cpp`.

**FR-13.1** Provide an interactive calibration mode reachable over both the local console
and the network (the reference uses Telnet), so a mounted device can be recalibrated
without disassembly.

**FR-13.2** Commands: `t` tare, `r` run the guided calibration, `c` manually enter and
optionally persist a calibration factor.

**FR-13.3** Guided calibration: prompt to unload the cell → tare → prompt to place a known
mass → accept the mass in grams (reference weight: **140.6 g**) → refresh the filter
dataset so the window contains only loaded readings → compute
`calibration_factor = filtered_counts / known_mass` → report it.

**FR-13.4** Persist the calibration factor **and** the tare offset to non-volatile storage
(the reference offers EEPROM persistence in the calibration sketch but the production
firmware ignores it and uses a hard-coded constant — resolve this; §12-A).

**FR-13.5** Report the calibration factor with at least 6 significant figures; existing
values carry 9.

## 14. Acceptance tests

Testable without hardware if the sensor is behind an interface (FR-3) fed by recorded or
synthetic conversion data.

| # | Setup | Expected |
|---|---|---|
| T1 | Steady load, no change | `/average` published every 20th settled iteration; no `/trigger`; baseline converges to the load within ~50 samples |
| T2 | Step of +3 g for 10 samples, then return | Exactly two `/trigger` messages: asserted at the step, cleared when delta re-enters 1.25 g |
| T3 | Step of +1.0 g | No trigger — below `TRIGGER_MIN` |
| T4 | Step of +60 g held | No trigger — above `TRIGGER_MAX`; baseline frozen; re-baseline fires after the FR-5.8 interval |
| T5 | Step of +3 g held for 400 samples | Single assert, no repeats, no counter wrap, clear on return (FR-5.4/5.5) |
| T6 | Trigger asserted, delta then oscillates between 1.5 g and 3 g | Trigger stays asserted throughout; no re-publish |
| T7 | Broker rejects one `/raw_batch` publish | Buffer retained, next attempt carries a larger count and a larger `duration`; RSSI batch unaffected (FR-6.5) |
| T8 | Buffer reaches 1025 entries | Batch discarded, buffer reset, device continues |
| T9 | Formatted batch payload would exceed the transport limit | Flushed early at a safe size; no failed publish (FR-6.6) |
| T10 | Broker connection dropped | Reconnect; retained `online` and reset reason republished; `/control` and `/watchdog` both re-subscribed (FR-7.2) |
| T11 | 10 consecutive failed connects | Device restarts |
| T12 | No `/watchdog` message for 120 s, watchdog armed | Device restarts |
| T13 | No `/watchdog` message ever, watchdog never armed | Device does **not** restart (FR-9.3) |
| T14 | `reset` on `/control` | Restart within one loop iteration (FR-7.7) |
| T15 | Sensor produces no conversions at boot | Fault reported, then restart or retry — never a silent hang (FR-3.6) |
| T16 | Replay `hummingbird_data_raw.txt` | Trigger events match the events identified in the reference data |
