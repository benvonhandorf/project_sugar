[← docs](README.md) · [Project README](../README.md)

# WiFi and MQTT connectivity

Findings from a stress-testing session against the physical board on 2026-09-16/17:
what was broken, what was fixed, and what remains an environmental condition rather
than a firmware bug.

## The bug: a silent, unrecoverable MQTT wedge

`main/publish_queue.c` runs a dedicated `mqtt_pub` task specifically so a stalled MQTT
client can't block `esp_timer` or the sampling task (see `publish_queue.h`). But the
task itself was not watchdog-subscribed, on the reasoning that "whatever the MQTT
client does while it sorts itself out is its own business."

On real hardware that reasoning didn't hold. `esp_mqtt_client_publish()` and
`esp_mqtt_client_enqueue()` both take the client's `MQTT_API_LOCK`, and the client's
own task holds that lock for the whole of a `MQTT_STATE_CONNECTED` loop iteration —
including a blocking socket read in `mqtt_process_receive()`. Under weak signal that
read can stall well past its nominal timeout, and while it does, *every* publish call
in the system blocks on the same lock. Observed worst case: the device stayed
LWT-"connected" but published nothing — no `status`, no batches, nothing — for **over
7 minutes**, with no automatic recovery, until the broker's own keepalive timeout
eventually fired the LWT from the far end.

**Fix** (`main/publish_queue.c`, `main/publish_queue.h`):

- `publisher_task` now calls `mqtt_manager_enqueue()` (copies into the outbox and
  returns) instead of the blocking `mqtt_manager_publish()`, reducing routine lock
  hold time.
- It also subscribes to the task watchdog, but *only* around the enqueue call, not the
  idle wait on the queue — so a legitimately quiet publish queue never trips it. If the
  underlying client wedges anyway (confirmed still possible — see below), the watchdog
  now catches it and the device self-heals via a full reboot in ~6–10 s instead of
  hanging indefinitely.

Both `esp_mqtt_client_publish()` and `esp_mqtt_client_enqueue()` live in the
`espressif__mqtt` managed component, fetched by version — not something this project
edits directly (see `AGENTS.md` on shared components). The watchdog wrapper is the
correct fix at this project's boundary: it can't stop the library's lock from being
held too long, but it bounds the blast radius to a fast, automatic reboot.

Confirmed live via serial: a `task_wdt` panic naming `mqtt_pub` as the stuck task,
followed by automatic recovery, reproduced multiple times both before and after the
fix — the difference is that after the fix, every one of those events actually
recovers instead of leaving the device silently dead.

## What a `task-watchdog` reset reason means

`status.reset_reason: "task-watchdog"` after a reboot is this mechanism working as
designed, not evidence of a new problem — check `status.uptime_s` first. A device
that's been rebooting every few minutes for hours is a symptom worth investigating
(see below); a single one after a rare stall is the safety net doing its job.

## Residual condition: RSSI-driven reconnect churn

A 6-hour hands-off soak test (2026-09-17 00:07–06:07 EDT, MQTT-observed only, no
resets) with the fix in place:

| Metric | Result |
|---|---|
| Total reconnect events | 86 |
| — full reboots (task-watchdog catch + recover) | 42, each ~6–10 s |
| — soft MQTT reconnects (no reboot) | 44, each ~1–4 min |
| Average time between events | ~4.2 min |
| Sensor faults (I2C / loadcell / SHT4x / INA237) | 0 |
| Heap free, sampled across the run | 125–127 KB, flat |
| Telemetry delivered despite the churn | 67 batches, 62 baseline updates |
| RSSI observed | swung from **-56 dBm to -82 dBm** |

No silent hang recurred. But the underlying wedge condition is still fairly frequent —
about once every 4–5 minutes on average — and every reboot's `status` showed the
device roaming between at least two physical access points on the same SSID
("Oblivion") with very different signal quality. Weak RF is exactly the condition that
makes `mqtt_process_receive()` stall past its nominal timeout, so the residual churn
looks environmental (device placement / AP coverage) rather than a code defect. The
next lever for *fewer* blips, as opposed to guaranteed recovery from them, is physical:
improve signal at the device's location, not another firmware change.

## Minor, unrelated observation

Every `rssi_batch` payload captured during the soak test was all zeros — the
per-sample RSSI telemetry isn't being populated with real values. Doesn't affect
connectivity or stability; worth a separate look if that data matters to a consumer.

## A hardware note for anyone debugging this on the bench

Opening `/dev/ttyACM0` — with any tool, `idf.py monitor` included — resets the board.
The ESP32-C3's native USB-Serial/JTAG treats the host's initial control-line request as
a reset trigger regardless of the DTR/RTS values requested, so there is no way to
passively "peek" at a running device over serial; every serial session starts with a
reboot. Plan bench sessions around that — prefer MQTT for undisturbed observation, and
treat a serial capture as inherently disruptive to whatever state you were trying to
observe.
