# c3_nau7802_trigger

Firmware for a hummingbird feeder weight trigger: an ESP32-C3 with a NAU7802 load cell
front end, an SHT4x and an INA237, publishing weight, landing events and telemetry over
MQTT.

A feeder hangs from a load cell. The device measures its weight continuously, detects the
transient change a bird makes when it lands, and publishes the event so a downstream
consumer can start recording video. The slowly-adapting baseline underneath that — the
unoccupied feeder's weight — is syrup consumption, tracked over days.

It replaces the ESP8266 firmware in `../feeder_weight/esp8266_load_cell`, whose behaviour
is specified in that directory's `REQUIREMENTS.md`. **Payloads on three topics changed
shape**; see [docs/message_changes.md](docs/message_changes.md) for what every consumer
has to do.

## What it does

| | |
|---|---|
| **Weight** | 18-deep trimmed sliding mean, 5-deep application average on top, calibrated to grams against a persisted tare and factor. Absolute feeder mass, not a change since boot. |
| **Landing detection** | A three-branch state machine with a settled band, a frozen baseline while anything is happening, and an automatic re-baseline after a refill. Tested off-target. |
| **Telemetry** | Batched weight and RSSI, flushed by payload size or entry count, with both a duration and an absolute start time. |
| **Environment** | SHT4x temperature and humidity on its own interval. |
| **Power** | INA237 bus voltage, current and power, with the health bits that say when a reading cannot be trusted. |
| **Liveness** | A task watchdog for firmware that stopped, and an external countdown for firmware that is running and useless — the second armed only once a supervisor has actually spoken. |
| **The rest** | Configuration, WiFi, mDNS, NTP, MQTT, a shell over serial and a browser, HTTP management and OTA with rollback, all from shared components. |

## Getting started

```sh
cp config/config.example.json fs/config.json    # then edit it: feeder_id, WiFi, broker
idf.py set-target esp32c3
idf.py build
./tools/flash_a.py -p /dev/ttyACM0
```

`fs/config.json` is gitignored and holds the real credentials. A build without one still
boots — onto its own access point, with the console running, ready to be configured —
but it will not publish anything until `feeder_id` is set to something other than 0.

Every component is fetched by the tag named in `main/idf_component.yml`; nothing has to
be cloned beside this project. To work on a component and this firmware at the same time:

```sh
idf.py -DESP_COMPONENTS_DIR=../../../esp_components build
```

That rewrites `dependencies.lock` to point at the checkout — do not commit it that way;
`idf.py reconfigure` without the override restores the pins. See
[AGENTS.md](AGENTS.md).

## The board

Three parts, all on one I2C bus:

| Part | Address | What it is |
|---|---|---|
| NAU7802 | `0x2A`, fixed | 24-bit bridge ADC; the load cell front end |
| SHT4x | `0x44`–`0x46` | Temperature and humidity |
| INA237 | `0x40`–`0x4F` | Bus voltage, current and power across an external shunt |

`i2c scan` probes every address on the bus and names what answered. It is the first
thing to run when a part fails to come up: three parts that all fail their own probe
are usually one bus that is not there, and a scan that finds *nothing* points at the
pins, the pull-ups or the sensor supply rather than at any part. The boot log says the
same thing on its own when all three fail.

The last two overlap: `0x44`–`0x46` is legal for both. If `board.sht4x_address` and
`board.ina237_address` are equal, neither part is started and the console says so —
two devices answering one address makes both of them return nonsense while each
driver's own probe still sees something answering.

## Calibrating

Over the serial port, or the same shell in a browser at the device's address:

```
scale show                    what the device believes, and where it got it
scale tare 64                 with the cell unloaded
scale calibrate 140.6 64      with a known mass on it
```

Both numbers are stored in NVS and survive a reflash. `config.json` carries the factory
defaults; NVS wins over them, and `scale forget` drops the stored pair so the configured
values apply again — which is the way back from a bad calibration without a cable.

The calibration refuses when the move away from the tare is inside the noise of the two
averages, so a calibration taken with nothing on the pan is reported rather than
accepted.

## Messages

Every topic is a suffix under `mqtt.topic_prefix`, which for this fleet is
`sensors/feeders/{id}`. Full reference in [docs/messages.md](docs/messages.md);
machine-readable schemas in [schemas/](schemas/).

```
status         retained JSON: health, weight, calibration, watchdog
online         retained 1/0, the last will
trigger        a landing asserted or cleared
average        the baseline weight -- syrup level
raw_batch      a batch of weight samples
rssi_batch     a batch of link RSSI
environment    temperature and humidity
power          bus voltage, current, power
message, log   text
control        <- reset | rebaseline
watchdog       <- a liveness ping from a supervisor
```

## Testing

```sh
make -C test        # the trigger state machine, on the host, no hardware
idf.py build        # -Werror is on for main
```

The trigger tests are the acceptance tests from `REQUIREMENTS.md` §14 that describe the
detection logic. They exist because none of them can be run on a bench — they are about
the shape of the branches, and every way of getting that wrong produces output that looks
like a plausible feeding event.

## Layout

```
config/       config_schema.json -- the one authored description of the configuration
schemas/      JSON Schema for every published MQTT payload
fs/           contents of the LittleFS partition (config.json is gitignored)
main/
  main.c            app_main: bring-up order, and the reasons for it
  board.c           buses, device handles, part bring-up -- the only file that
                    knows what is wired where
  weight.c          acquisition, the filter chain, the sampling task
  trigger.c         the trigger state machine as pure arithmetic  <- tested
  batch.c           telemetry batching and its byte budget
  scale_store.c     tare and calibration factor in NVS
  sensors.c         SHT4x and INA237, on their own task
  app_control.c     control/watchdog topics and both watchdogs
  app_status.c      what this device reports
test/         host tests for trigger.c
docs/         messages, the changes consumers need, requirements conformance
partitions/   4 MB / 8 MB / 16 MB A/B layouts
tools/        flashing, OTA upload, status, MQTT tail
```

See [AGENTS.md](AGENTS.md) for the conventions this project follows and
[docs/requirements.md](docs/requirements.md) for how it maps onto the specification it
was written to — including the three places it deliberately differs.
