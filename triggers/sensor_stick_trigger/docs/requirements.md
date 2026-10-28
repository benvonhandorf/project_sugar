# Conformance against REQUIREMENTS.md

`../../feeder_weight/esp8266_load_cell/REQUIREMENTS.md` is the behavioural specification
this firmware was written to. This is what was implemented, what was decided where the
specification left a choice open, and the three places the implementation deliberately
differs.

Read the deviations (§Deviations) before the table; they are the parts that would
otherwise surprise someone comparing the two.

## Section by section

| Requirement | Where | Notes |
|---|---|---|
| FR-3.1 amplifier filter | `main/weight.c` `window_trimmed_mean()` | 18-deep sliding window, drop one high and one low, mean the remaining 16. Depth and trims are configurable. |
| FR-3.2 application filter | `main/weight.c` `acquire_sample()` | 5 trimmed means, one per new conversion, averaged into one sample. The overlap is preserved — see the comment there. |
| FR-3.3 calibration | `main/weight.c` `counts_to_grams()` | `(counts − tare) / factor`. A negative factor is accepted and not made absolute. |
| FR-3.4 tare acquisition | `main/scale_store.c` | §12-A default taken: the tare is persisted in NVS and **not** re-taken at boot. `loadcell.tare_on_boot` restores the old behaviour. |
| FR-3.5 sample rate | `loadcell.rate_sps`, `status.sample_rate_hz` | Published in the status message; batches are sized by payload rather than by a count tied to one rate. |
| FR-3.6 sensor fault | `main/weight.c` `sample_task()` | Bounded wait, then `message` + `status`, then restart. No infinite delay loop. |
| FR-4.1 startup baseline | `main/weight.c` `acquire_baseline()` | 5 samples averaged; `initialized` gates the trigger branch; an initial cleared trigger is published. |
| FR-4.2 baseline update | `main/trigger.c` | `0.9·baseline + 0.1·sample`, alpha configurable. |
| FR-4.3 baseline freeze | `main/trigger.c` | Frozen on every non-settled iteration. Tested: `between_the_thresholds`, `t3_too_small`. |
| FR-5.1–5.5 trigger FSM | `main/trigger.c` `trigger_step()` | Three branches, strict bounds, saturating 32-bit counter, cap that does not clear. Tested T2, T3, T5, T6, `cap_does_not_clear`, `bounds_are_strict`. |
| FR-5.6 average cadence | `main/trigger.c` | Counter advances every iteration, publish only on a settled one. Tested: `averages_are_irregular`. |
| FR-5.7 trigger event | `main/weight.c` `publish_trigger()` | On assertion and clear only. |
| FR-5.8 re-baseline | `main/trigger.c` + `main/weight.c` | §12-B default taken: automatic after 300 s outside the band, **and** a `rebaseline` control command, **and** `scale rebaseline` on the console. Tested T4. |
| FR-6.1 batch trigger | `main/batch.c` | Corrected deliberately: `telemetry.batch_entries` is the count published, so 256 means 256, not 257. |
| FR-6.2 batch duration | `main/batch.c` | §12-C default taken: `duration_ms` kept, and an absolute `ts` added when NTP has set the clock. |
| FR-6.3 publish retry | `main/batch.c` | See Deviation 1. |
| FR-6.4 overflow drop | `telemetry.batch_drop_entries` | Default 1024. |
| FR-6.5 independent RSSI reset | `main/batch.c` | Two `batch_t` instances sharing no state at all. |
| FR-6.6 size by payload | `main/batch.c` | Flushed at whichever of `batch_entries` and `batch_max_bytes` comes first. |
| FR-6.7 RSSI sampling | `main/weight.c` `append_telemetry()` | One reading per iteration, signed dBm. |
| FR-7.1 identity and LWT | `mqtt_manager` | Client id is the device name; `online` is the retained will. See Deviation 3. |
| FR-7.2 subscriptions | `main/app_control.c` | **Both** `control` and `watchdog`, re-applied on every reconnect. |
| FR-7.3 batch schema | `schemas/batch.schema.json` | |
| FR-7.4 trigger schema | `schemas/trigger.schema.json` | §12-D default taken: `value` is a JSON boolean. Consumer changes in [message_changes.md](message_changes.md). |
| FR-7.5 average payload | `schemas/average.schema.json` | Changed to JSON; see Deviation 3. |
| FR-7.6 reconnection | `mqtt_manager` + `wifi_manager` | Both reconnect with backoff and start themselves from `NET_EVENT`. See Deviation 2. |
| FR-7.7 service every iteration | esp-mqtt | Not applicable in the same form: esp-mqtt runs its own task, so keepalive and inbound dispatch do not depend on the sampling loop at all. The bug this requirement exists to prevent cannot occur here. |
| FR-7.8 delivery guarantees | | QoS 0 for data, QoS 1 for the will. Stated in every schema. |
| FR-7.9 debug echo | `telemetry.debug_echo`, `mqtt_log_sink` | The whole log goes to `log`; `message` carries the events worth a human reading. The sink drops rather than recurses, and counts what it dropped. |
| FR-8.1 per-device config | `config/config_schema.json` | Runtime, in `config.json`; replaceable over HTTP without reflashing. |
| FR-8.2 credential handling | `fs/config.json` (gitignored) | No credential is compiled in. The existing ones are in git history and should be rotated. |
| FR-8.3 missing id | `feeder_id` | 0 means unprovisioned: logged as an error at boot and on every status interval, `provisioned: false` in `status`, and **no measurement topic is published**. No fallback to a feeder number nobody watches. |
| FR-9.1 hardware watchdog | `main/app_control.c` | Task watchdog, `watchdog.hardware_s` (default 30 s), fed by the sampling task. |
| FR-9.2 external watchdog | `main/app_control.c` | 120 s countdown, reset by any `watchdog` message. |
| FR-9.3 defeatable | `watchdog.arm_on_first_message` | Default true: the countdown does not start until a supervisor has actually spoken. |
| FR-9.4 control commands | `main/app_control.c` | `reset` and `rebaseline`; anything else is logged and counted. |
| FR-9.5 reset reason | `main/app_status.c` | Captured at status start, published as a word in the retained `status`. |
| FR-10.1 startup sequence | `main/main.c` | Ordered and commented. |
| FR-10.2 OTA during acquisition | — | Not applicable in the same form: the HTTP server and OTA run on their own tasks, so a blocking acquisition loop cannot starve them. |
| FR-13.1–13.5 calibration | `scale` console group | `scale tare`, `scale calibrate <grams> [samples]`, `scale factor`, `scale forget`, `scale show`. Reachable over serial and over the web console at the device's address. Both values persist to NVS. The factor is reported to six significant figures with its precision. |

## Acceptance tests

T1–T6 from §14, plus five more the specification implies but does not list, run on the
host with no hardware:

```sh
make -C test
```

They exercise `main/trigger.c` directly — which is why that file exists separately from
`weight.c`. T7–T15 concern the transport and the watchdogs and are not covered by an
automated test; T16 (replaying `hummingbird_data_raw.txt`) is not implemented, and is the
most valuable one still missing, because it is the only one that would confirm the
thresholds carry over to this hardware at all.

## Deviations

### 1. FR-6.3 and FR-6.6 contradict each other; FR-6.6 wins

FR-6.3 says a failed publish retains the buffer and retries on each subsequent batch
crossing, with `duration` and count *growing* across retries. FR-6.6 says the batch must
be flushed before the serialised payload exceeds the transport limit. A buffer cannot
both grow without bound and stay under a byte cap.

FR-6.6 is marked `[FIX]` and explicitly notes that FR-6.3 "exists to paper over" the
payload bug, so the payload cap is the requirement with the reason behind it. What
`batch.c` does instead: on a failed publish it keeps the buffer and retries on the next
append, so a broker that was briefly away costs nothing. Incoming samples are dropped —
and counted — while the retry is outstanding, and once
`batch_drop_entries` is reached the batch is discarded, which is FR-6.4's bound on
staleness applied to the same situation.

The consequence: a published batch always has mutually consistent `count` and
`duration_ms`, and a long outage loses samples rather than memory. The predecessor lost
the whole batch in that case anyway.

### 2. FR-7.6's restart-after-10-failures is not implemented

`wifi_manager` and `mqtt_manager` reconnect with their own backoff and do not count
consecutive failures up to a restart. The external watchdog (FR-9.2) covers the same
ground from outside and covers it better: it restarts a device that is *not working*,
whatever the reason, rather than one that has failed to reach a broker ten times — which
on a network outage is a device working correctly and waiting.

If the count is wanted, it belongs in `mqtt_manager` as a general capability, not here.

### 3. Payloads changed shape

`status`, `average` and `trigger` are not byte-compatible with the predecessor, and
`reset` is gone. Every change, the reason, and the exact consumer edit is in
[message_changes.md](message_changes.md). This is the part that requires work outside
this repository.

## Not implemented

- **T16, the recorded-data replay.** `hummingbird_data_raw.txt` (565k lines, absolute
  weights around 970 g) would go through `trigger.c` unchanged — it is already a pure
  function over samples — so this is a test-harness job rather than a firmware one. It is
  the right way to confirm §12-E: the thresholds were tuned on the previous mechanical
  arrangement, and nothing yet says they carry over.
- **Threshold re-tuning.** `trigger.min_g` and its siblings are carried over as-is. They
  should be re-derived from `raw_batch` recorded on this hardware before the device is
  trusted, exactly as §12-E says.
- **A real calibration factor.** `loadcell.calibration_factor` is 1125.0 as supplied, and
  is a placeholder. `scale calibrate` produces the real one and stores it in NVS, where it
  wins over the configured value.
