# Message changes: what other systems have to do

This firmware replaces the ESP8266 feeder monitor in
`triggers/feeder_weight/esp8266_load_cell`. It keeps the topic tree
(`sensors/feeders/{ID}/…`) and the topic names, but four payloads changed shape.

Three of the four are **breaking**. They are listed first, with the exact edit each
consumer needs.

---

## 1. `trigger` — `value` is now a JSON boolean · **BREAKING**

```diff
- { "value": 1, "delta": 3.42}
+ {"ts":1757193600000,"value":true,"delta_g":3.42,"weight_g":103.42,"baseline_g":100.00}
```

Three things changed: `value` is `true`/`false` rather than `1`/`0`, `delta` is renamed
`delta_g` for its unit, and the leading space is gone.

**Why.** The old payload had three descriptions and no two agreed. `CLAUDE.md` documented
`true`/`false`. The code emitted `1`/`0` inside a JSON object. The consumer —
`opencv-rtsp/capture_on_mqtt.py` — did `payload.lower() == 'true'` against the whole
payload, which could never match anything the device sent. Something had to move, and
making the code match its own documentation and its consumer's expectation is the
version that leaves one description standing instead of three.

**What to change in `opencv-rtsp/capture_on_mqtt.py`,** in `on_trigger_event()`:

```diff
- message_str = message.payload.decode('utf-8')
- print(message_str)
-
- if message_str.lower() == 'true':
+ event = json.loads(message.payload)
+ print(event)
+
+ if event.get('value'):
```

**And a second, independent bug in the same file.** `on_connect()` does

```python
client.subscribe('/sensors/feeders/2/trigger', qos=0)
```

**with a leading slash**. That is a different topic from the one the device publishes to
— MQTT does not treat a leading slash as optional, it treats it as an empty first level.
It has to go:

```diff
- client.subscribe('/sensors/feeders/2/trigger', qos=0)
+ client.subscribe('sensors/feeders/2/trigger', qos=0)
```

Fixing only the payload parsing will not make video capture start working. Both edits are
needed, and this one was hiding behind the other.

**While you are in that file:** the MQTT username and password are hard-coded literals in
it, and they are committed. So are the WiFi PSK and the OTA password in the predecessor's
`src/main.cpp` and `platformio.ini`. This firmware reads all of them from `fs/config.json`,
which is gitignored — but the existing credentials have been in a public history and
should be treated as compromised and rotated. Rotate them once, on the broker, and update
`config.json` on each device and this script together.

---

## 2. `average` — now a JSON object · **BREAKING**

```diff
- -0.412
+ {"ts":1757193600000,"baseline_g":974.318}
```

**Why.** A bare formatted float carries no unit, no time, and no room to ever carry one.
Every other topic here is JSON; this was the single exception, and it forced any consumer
of it to special-case parsing.

**Also worth knowing:** the numbers themselves change meaning. See §5 below — the old
device published weight relative to whatever was on the cell at boot, and this one
publishes absolute feeder mass.

```diff
- grams = float(msg.payload)
+ grams = json.loads(msg.payload)["baseline_g"]
```

---

## 3. `status` and `reset` — one retained JSON document · **BREAKING**

```diff
  topic: sensors/feeders/3/status
- online
+ {"ts":…,"firmware":"sensor_stick_trigger","feeder_id":3,"provisioned":true,
+  "uptime_s":8134,"reset_reason":"poweron","weight_g":974.318,…}

  topic: sensors/feeders/3/reset
- 4
+ (topic removed; the reason is status.reset_reason)

  topic: sensors/feeders/3/online          <- new
+ 1 / 0    (retained, QoS 1, last will)
```

**Why.** The old `status` topic did two incompatible jobs at once. A retained
`online`/`offline` string is a *liveness* signal — it must be the last will, so the
broker can publish it for a device that vanished — and it has no room for anything else.
A status *document* is a periodic report of what the device is doing, and it must not be
the last will, because a broker cannot synthesise one.

So the two jobs are now two topics: `online` is the retained last will (the standard
`mqtt_manager` behaviour, so every device in the fleet reports liveness identically), and
`status` is a retained JSON document with about forty fields.

`reset` is gone as a topic. The reset reason is `status.reset_reason` and is a **word**
now (`"panic"`, `"brownout"`, `"task-watchdog"`) rather than the platform's integer code,
which needed the vendor's header to read. The predecessor also captured the exception
cause at boot and then never published it anywhere.

**What to change:**

| Was | Now |
|---|---|
| subscribe `…/status`, compare to `"online"` | subscribe `…/online`, compare to `"1"` |
| subscribe `…/reset`, look up the integer | read `status.reset_reason`, a string |
| — | subscribe `…/status` for everything else |

Full field list: [`schemas/status.schema.json`](../schemas/status.schema.json).

---

## 4. `raw_batch` and `rssi_batch` — added fields, same shape · compatible

```diff
- {"duration":128500,"data":[974.318,974.201,…]}
+ {"ts":1757193600000,"duration_ms":128500,"count":256,"unit":"g","data":[974.318,…]}
```

`duration` is renamed `duration_ms` — that is the one edit needed — and three fields are
added. `ts` is the epoch time of the batch's **first** entry, so a batch can finally be
placed on a timeline rather than only measured internally; it is omitted (not zeroed)
when the clock was unset. `count` makes a truncated message detectable. `unit`
distinguishes the two topics without knowing which one you subscribed to.

```diff
- per_sample_ms = payload["duration"] / len(payload["data"])
+ per_sample_ms = payload["duration_ms"] / payload["count"]
```

**One behavioural change worth expecting.** The old device published **257** entries per
batch, not 256: it flushed when the buffer *exceeded* the batch size. This one publishes
`telemetry.batch_entries` exactly — 256 by default. A consumer that hardcoded 257
somewhere will see one fewer.

**And batches will now actually arrive.** They did not before: at 2 samples/second a
257-sample batch needs 128 seconds, and the device rebooted every 120. Any pipeline that
was written against `raw_batch` has never been exercised against real traffic.

---

## 5. Weights are absolute now · **affects interpretation, not parsing**

The old firmware tared at every boot **with the feeder in place**, so every weight it
published was a change relative to whatever was hanging on the cell at that moment — and
every reboot silently redefined zero. Since it also rebooted every two minutes (§4),
`average` was not comparable across any window longer than that.

This firmware persists the tare in NVS and does not re-tare at boot, so `weight_g` and
`baseline_g` are **absolute feeder mass in grams**. That is what makes tracking syrup
consumption over days possible, which appears to have been the point of the project all
along — `hummingbird_data_raw.txt` at the repository root records absolute weights around
970 g, so an earlier revision did this too.

A consumer that plotted the old values as "change since boot" should now plot them as
mass. Historical data from the old firmware and new data are **not** on the same scale
and should not be concatenated.

To reproduce the old behaviour, set `loadcell.tare_on_boot: true`. Do not, unless you
know why you want it.

---

## 6. New topics · additive

`environment` (SHT4x temperature and humidity) and `power` (INA237 bus voltage, current,
power, die temperature) are new — the old board had neither part. Nothing has to change
to keep working; subscribe to them if you want them.

**A board with no INA237 reports voltage only.** The ESP32-S3 board has no INA237, only a
500k/100k divider from the input to GPIO 1. On such a board `power` carries
`{"ts":…,"bus_v":…,"healthy":true}` and **no** `current_a`, `power_w` or `temperature_c`,
and `status` omits `current_a` and `power_w` the same way — they are absent rather than
0, because a zero would read as a measured zero. `status.ina237` is `"not-fitted"`, a
fourth value beside `ok`, `failed` and `not-tried`. A consumer that requires `current_a`
must be relaxed to treat it as optional; `schemas/power.schema.json` no longer requires
it. A board with an INA237 publishes exactly what it did before.

---

## Migration order

The device and its consumers cannot both change at the instant of a firmware update, so:

1. **Update the consumers first.** Every edit above is backwards-compatible with the old
   device except the `status`/`online` split, and that one can be made tolerant: subscribe
   to both `…/status` and `…/online`, and treat `status == "online"` or `online == "1"` as
   alive. A JSON `status` fails to compare equal to `"online"`, so the old branch simply
   stops matching once the device updates.
2. **Fix the `capture_on_mqtt.py` topic slash at the same time.** It has never worked, so
   there is no regression risk, and leaving it means the payload fix looks like it failed.
3. **Then update the devices**, one at a time. `feeder_id` must be set in `config.json`
   before a device publishes anything at all — an unprovisioned device reports
   `provisioned: false` and publishes no measurements, deliberately, rather than falling
   back to a feeder number nobody watches.
