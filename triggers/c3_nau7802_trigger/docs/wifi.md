[← docs](README.md) · [Project README](../README.md)

# WiFi and MQTT connectivity

Findings from stress-testing the physical board on 2026-09-16/17: what was broken and
what was fixed. The document is kept in the order it was learned, because two of its
early conclusions were wrong and the corrections are the useful part. Read it through, or
skip to [the corrections](#correction-the-churn-was-firmware-and-two-hours-of-soak-say-so):
nothing here turned out to be environmental.

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

**Later correction** (see below): `mqtt_pub` was stuck, but the MQTT client's lock was
not the root cause. It was held across a socket call that never returned, because the
whole lwIP stack was deadlocked by a log line from its own TCP/IP thread. The watchdog
wrapper here is still the right boundary — it bounds any such stall to a reboot — but the
thing it was catching has since been fixed at its source.

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

## Correction: the churn was firmware, and two hours of soak say so

That conclusion was wrong on both counts. The reboots were a deadlock, not weak RF, and
the access-point swinging was the connect path picking badly. A board that never moves
should not swing 26 dB.

### The reboots: logging from lwIP's TCP/IP thread

`reset_reason: "task-watchdog"` every few minutes was not the MQTT client stalling under
weak signal. `ntp_manager` v0.1.0 logged "clock set from NTP" from inside `sntp_sync_time()`,
which lwIP calls on its own TCP/IP thread with no task hop. That line reached `diag`'s
fan-out, which holds its output lock across every sink, and `cli_web` v0.1.0's sink called
`httpd_queue_work()` — a socket call, which posts to the TCP/IP mailbox and waits for the
thread already inside the sink. The thread waited for itself, permanently, holding the
output lock.

Everything that touches the network or logs then blocks. The MQTT client's task holds the
client lock across a socket call that never returns, so `mqtt_pub` blocks in
`mqtt_manager_enqueue()`; the `weight` task blocks the moment it logs. Thirty seconds
later the task watchdog reboots the device — which is exactly the symptom this document
previously attributed to `mqtt_process_receive()` stalling on a weak link.

Why it looked RF-dependent: on the far access point NTP rarely completed a sync (every
`status` in the 6-hour soak shows `time_synced: false`), so the trap sprang only
occasionally. Fixed properly, the device syncs about 11 s into every boot — and the first
build with the WiFi fixes boot-looped every ~50 s until these pins moved:
`ntp_manager` v0.1.1 (records, posts, and logs from the event loop task instead),
`diag` v0.1.1 (states the rule in the header), `cli_web` v0.2.2 and `http_server` v0.2.0
(a sink that can block hands off to its own task).

**The lesson is general**: an `ESP_LOGx` in an lwIP raw-API callback runs on the TCP/IP
thread, and any sink that touches a socket deadlocks the whole stack from there.

### The access points: fast scan joined the first, not the strongest

Fixed in `wifi_manager` v0.3.0 and `mqtt_manager` v0.1.1, both pinned here:

- **It joined the first access point it heard, not the strongest.** The connect used
  ESP-IDF's default fast scan, which stops at the first BSS matching the SSID. On
  "Oblivion" that was whichever access point sat on the lower channel, however far away
  — the -82 dBm readings. Connects now scan every channel and sort by signal.
- **Every drop cost two minutes.** The first reconnect scan waited `scan_interval_ms`
  (120 s) — the "soft reconnects, each ~1–4 min" above. Retries now start at 1 s and
  back off to that.
- **Modem power saving was on.** It is ESP-IDF's default for a station, and it misses
  beacons on a marginal link. Off unless `wifi.power_save` is set.
- **No roaming.** `CONFIG_ESP_WIFI_11KV_SUPPORT` is now enabled, so a mesh that supports
  802.11k/v can steer the device to a better access point.
- **A quick reconnect could silence MQTT for good.** `mqtt_manager` cleared its connected
  flag on link-down and only a broker reconnect set it again; a link that came back with
  the same address kept its TCP socket, so none happened. The two-minute delay hid this.

`status` now reports what is needed to tell these apart without a serial port:
`wifi_bssid` and `wifi_channel` (which access point, right now), `wifi_link_losses` and
`wifi_roams` (since boot), and `wifi_disconnect_reason`, `wifi_disconnect_rssi` and
`wifi_disconnect_ago_s` for the most recent disconnect. Reason 200 is a beacon timeout
(signal), 201 no access point found, 15 a handshake timeout, 12 a roam. `net status` on
the console shows the same.

### The result: two clean hours

Two 1-hour soaks on 2026-09-17, same room, same board, nothing moved. The first ran with
only `ntp_manager` bumped; the second with every pin above. Both were observed over MQTT
*and* by polling `/api/status` every 15 s, which is also how the device's reachability was
measured — the earlier soak had no such check.

| Metric | 6-hour soak (before) | 13:01–14:02 | 14:28–15:29 |
|---|---|---|---|
| Reconnect events per hour | ~14 | **0** | **0** |
| Reboots per hour | ~7 | **0** | **0** |
| WiFi link losses | — | 0 | 0 |
| Roams | — | 1, at boot | 0 |
| Access point held | swung between two | `08:bf:b8:20:45:20` ch7, 99% | same, 100% |
| Average RSSI | -56 to -82 swing | **-49.0 dBm** (σ 4.1) | **-56.2 dBm** (σ 3.1) |
| MQTT connected, sampled | — | 301/302 | 304/304 |
| `/api/status` answered | — | 241/242 | 243/243 |
| Heap free | 125–127 KB | 121–125 KB | 119–121 KB, flat |

The single roam is worth keeping: the boot scan saw only the far access point (-85 dBm),
the device joined it, and 802.11v steering moved it to the near one (-49 dBm) 42 s later.
MQTT was back in about a second. That is the intended behaviour of the whole change set in
one event — and the reason `roaming` defaults on.

**What these hours do not prove.** Both ran at -49 to -56 dBm, so nothing exercised the
weak-signal paths these fixes are mostly about: the reconnect backoff, the roam-not-a-
link-loss handling, and `mqtt_manager`'s connected flag across a same-address reconnect.
An overnight run is the one that would.

**On placement**: the device holds one access point at about -50 dBm and stays there. The
earlier recommendation to improve signal at the device's location is withdrawn — the
signal was never the problem.

## Correction: `rssi_batch` does carry real values

An earlier note here said every `rssi_batch` payload was zeros. It is not: each batch
carries real per-sample dBm readings, with only the first few zero, before the radio
associates. The zeros came from capturing batches on a device that spent most of its time
disconnected — `wifi_manager_get_rssi()` reports 0 when the station is not associated,
which is the correct answer to "what is the signal" when there is no link.

## A hardware note for anyone debugging this on the bench

Opening `/dev/ttyACM0` — with any tool, `idf.py monitor` included — resets the board.
The ESP32-C3's native USB-Serial/JTAG treats the host's initial control-line request as
a reset trigger regardless of the DTR/RTS values requested, so there is no way to
passively "peek" at a running device over serial; every serial session starts with a
reboot. Plan bench sessions around that — prefer MQTT for undisturbed observation, and
treat a serial capture as inherently disruptive to whatever state you were trying to
observe.
