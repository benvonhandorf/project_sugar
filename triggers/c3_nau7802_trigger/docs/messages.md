# MQTT messages

Every topic is a suffix under `mqtt.topic_prefix` from `config.json`, which for the
existing fleet is `sensors/feeders/{FEEDER_ID}` — no leading slash, no zero padding.
Nothing in the firmware knows the prefix; a device's whole topic tree moves by editing
one configuration value.

Machine-readable schemas for every published payload live in [`../schemas/`](../schemas/).

| Topic | Direction | QoS | Retained | Payload | Schema |
|---|---|---|---|---|---|
| `status` | pub | 0 | yes | Device health and load-cell state | [status](../schemas/status.schema.json) |
| `online` | pub | 1 | yes | `1` / `0` — birth and last will | — |
| `trigger` | pub | 0 | no | Landing asserted / cleared | [trigger](../schemas/trigger.schema.json) |
| `average` | pub | 0 | no | The unoccupied-feeder baseline | [average](../schemas/average.schema.json) |
| `raw_batch` | pub | 0 | no | A batch of weight samples | [batch](../schemas/batch.schema.json) |
| `rssi_batch` | pub | 0 | no | A batch of link RSSI readings | [batch](../schemas/batch.schema.json) |
| `environment` | pub | 0 | no | SHT4x temperature and humidity | [environment](../schemas/environment.schema.json) |
| `power` | pub | 0 | no | Supply voltage; with an INA237 also current and power | [power](../schemas/power.schema.json) |
| `message` | pub | 0 | no | Free text: re-baselines, sensor faults | — |
| `log` | pub | 0 | no | The device log, line by line | — |
| `control` | **sub** | 1 | — | `reset` or `rebaseline` | — |
| `watchdog` | **sub** | 1 | — | Anything; the payload is ignored | — |

## What each one is for

**`status`** is the one to watch. It is retained, so a dashboard that connects at any
moment gets the current state rather than nothing until the next interval, and it is a
flat object of scalars because that is the shape that survives being graphed, alerted on
and diffed.

**`online`** is the last will. The broker publishes `0` on it when the client stops
responding, which is the only way a subscriber learns about a device that vanished
rather than one that said goodbye. `status` cannot do this job: a retained `status` from
a dead device looks exactly like a retained `status` from a live one.

**`trigger`** fires twice per landing — once on assertion, once on clear — and never in
between, however long the bird stays. This is what the video capture subscribes to.

**`average`** carries the baseline: the slowly-adapting estimate of the unoccupied
feeder's weight, which is to say the syrup level. Its cadence is deliberately no
promise. The publish counter advances on every sample but the publish only happens on a
settled one, so these are irregular and are skipped entirely during activity. **A gap in
`average` is not evidence of a problem**; `status` is where that question is answered.

**`raw_batch`** and **`rssi_batch`** carry the underlying signal, one entry per sampling
iteration. They are what a threshold is re-tuned against: `trigger.min_g` and its
siblings were tuned empirically on the previous mechanical arrangement, and a different
cell, mounting or noise floor means re-deriving them from recorded `raw_batch` data
rather than carrying them over.

**`control`** takes two commands. `reset` restarts the device within a second.
`rebaseline` re-acquires the baseline at the current load — the way to recover a device
that was refilled while `trigger.rebaseline_after_s` has not yet elapsed. Anything else
is logged and counted as unrecognised, and shows up in `status`.

**`watchdog`** is a liveness ping from an external supervisor. Any message on it resets
the countdown; the payload is not read. See [Two watchdogs](#two-watchdogs).

## Delivery guarantees

Everything except the last will is QoS 0. A publish that "succeeded" reached the
transport, not the broker.

Consumers must therefore tolerate gaps in `raw_batch` and `rssi_batch`, and must use
`ts` and `duration_ms` — not arrival order — as the source of timing truth. The two
batch topics are flushed and retried independently, so their `count` fields need not
agree even though both accumulate one entry per iteration.

## Timestamps

Every payload carries `ts`, epoch milliseconds.

Where a timestamp is taken **at the moment of publishing** — `trigger`, `average`,
`environment`, `power`, `status` — it is `0` when the clock has not yet been set by NTP.
Zero rather than a plausible-looking date: the ESP32 boots in 1970, and a consumer can
drop a zero but cannot detect a decades-old timestamp it has already graphed.

In a batch, `ts` is the time the batch's **first entry** was appended, and it is
**omitted entirely** rather than zeroed when the clock was unset then. A consumer keying
on it should skip the message.

## Two watchdogs

The task watchdog catches firmware that has stopped running: the sampling task feeds it
once per sample, and `watchdog.hardware_s` sets the timeout.

The external countdown catches firmware that is running and useless. Only a message on
`watchdog` resets it, so a device that is connected, publishing and measuring nothing
still gets restarted. By default it does not start counting until the first such message
arrives (`watchdog.arm_on_first_message`), which means **a fleet with no supervisor is
not affected by it at all** — turn that off only once something is actually publishing.

The predecessor got this exactly wrong: it started the countdown at boot and then
subscribed only to `control`, so the message that would have reset it was never
delivered, and every device rebooted every 120 seconds for its whole service life. One
consequence is worth keeping in mind when reading historical data: a 257-sample batch at
2 samples/second needs 128 seconds of wall time, so `raw_batch` **never published at
all**, and the boot-time tare was re-taken every two minutes, which makes `average`
incomparable across any window longer than that.
