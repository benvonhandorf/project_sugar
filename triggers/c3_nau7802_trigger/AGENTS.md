# AGENTS.md

Orientation for anyone — human or agent — working in this project. Read this before
changing code; it should save most of a codebase scan.

## What this is

Firmware for a hummingbird feeder weight trigger: an ESP32-C3 with a NAU7802 load cell
front end, an SHT4x and an INA237, all three on one I2C bus. Built from `idf_template`.

The parts that every project needs — configuration, WiFi, mDNS, NTP, MQTT, a command
shell, HTTP, OTA — and the three device drivers live in **shared components** in a
separate repository (`esp_components`), fetched by tag — see `main/idf_component.yml`.
What is *here* is the part specific to this device: the signal chain, the trigger state
machine, the batching, and what gets published.

The behavioural specification this was written to is
`../feeder_weight/esp8266_load_cell/REQUIREMENTS.md`, derived from the ESP8266 firmware
this replaces. `docs/requirements.md` says what was implemented and where this
deliberately differs. **Read `docs/message_changes.md` before touching a payload** — three
topics changed shape and other systems in this repository depend on them.

That split is the whole design. If you find yourself editing a shared component to make
this project work, ask first whether the component is missing a general capability or
whether the project is reaching through an abstraction it should be using.

ESP-IDF **v6.0.2**. C11, `-Wall -Wextra -Werror` on `main`.

## Layout

```
config/config_schema.json   the ONE description of this device's configuration
schemas/                    JSON Schema for every published MQTT payload
fs/                         LittleFS image contents; config.json is gitignored
main/
  main.c                    app_main: bring-up order, and the reasons for it
  config_reader.{c,h}       loads the config; dispatches sections to their owners
  board.{c,h}               buses, device handles, part bring-up
  weight.{c,h}              acquisition, the filter chain, the sampling task
  trigger.{c,h}             the trigger state machine, as pure arithmetic
  batch.{c,h}               telemetry batching and its byte budget
  scale_store.{c,h}         tare and calibration factor in NVS
  sensors.{c,h}             SHT4x and INA237, on their own task
  app_control.{c,h}         control/watchdog topics, task and external watchdogs
  app_status.{c,h}          what this device reports
  app_console.{c,h}         this project's shell commands
  app_http.{c,h}            this project's HTTP routes
  app_time.{c,h}            epoch milliseconds, or 0 when the clock is unset
test/                       host tests for trigger.c
partitions/                 A/B OTA layouts for 4/8/16 MB
tools/                      flashing, OTA upload, status, MQTT tail
```

### Three tasks, and what owns what

- **`weight`** owns the NAU7802. Nothing else may read a conversion. Anything that needs
  the converter — a tare, a calibration, a re-baseline — is submitted to it and serviced
  between samples, so no second caller can read conversions out from under the filter.
- **`sensors`** owns the SHT4x and the INA237. Its own task rather than an `esp_timer`
  callback: an SHT4x high-repeatability measurement blocks for about 9 ms, and timer
  callbacks share one task with everything else that uses a timer.
- **`watchdog`** owns the external countdown, and is where deferred work from the MQTT
  callbacks lands. Both `control` handlers set a flag rather than acting, because they
  run on the MQTT client task and that task is also what sends keepalives.

## Conventions

- **Never `printf()`.** Use `diag_printf()`, or output never reaches the web console.
  `ESP_LOGx` is already routed through the same fan-out.
- **Report command failures with `diag_error()`** — it prefixes `ERR:` so a host script
  can detect failure without parsing prose — and return `-1`; return `0` on success.
- **Parse arguments with the `cli_parse_*` helpers**, never `atoi()`. They must consume
  the whole token, so `gpio read foo` is an error rather than a read of pin 0.
- **`main` must never set `REQUIRES` or `PRIV_REQUIRES`.** `project.cmake` grants it an
  implicit dependency on every component in the build *only* while both are unset; naming
  one silently strips the rest.
- **A new file must be added to `SRCS` in `main/CMakeLists.txt`** or it is never compiled,
  silently.

## Adding things

**A configuration section.** Add a `$ref` to `config/config_schema.json` naming the
component that owns the schema, add the field to `app_config_full_t` and a
`SECTION_PARSER_<name>` line in `config_reader.c`. Forgetting the last step does not
compile: the generated `APP_CONFIG_SECTIONS` X-macro expands to something that needs it.

**A console command.** Add a `cli_command_t` row to a group in `app_console.c`. Commands
are `<group> <command>`; a command receives `argv[0] == its own name`. Deeper grouping is
a hyphenated group name (`i2c-nau7802`), not a third token.

**An HTTP route.** Add an `http_route_t` row in `app_http.c`. Set `require_auth` on
anything that exposes credentials or changes the device.

**Something this device measures.** `app_status.c`. Keep the JSON a flat object of
scalars — that is the shape that survives being graphed, alerted on and diffed. Update
`schemas/status.schema.json` in the same commit; it is the published contract, and a
consumer written against it is entitled to `additionalProperties: false` meaning what it
says.

**A new published topic.** A schema in `schemas/`, a row in `docs/messages.md`, and the
publish itself. A payload that changes shape also needs an entry in
`docs/message_changes.md`, because other systems in this repository parse these.

**A change to the trigger logic.** `main/trigger.c`, and a test in `test/`. That file is
separate from `weight.c` precisely so it can be exercised without hardware: it depends on
nothing but `math.h`.

## Things that will bite

- **The SHT4x and the INA237 have overlapping addresses.** `0x44`-`0x46` is legal for
  both. `board_init()` refuses to start either part when the two configured addresses are
  equal, because two devices on one address makes both return nonsense while each
  driver's own probe still sees something answering — a fault that otherwise gets
  diagnosed as a bad sensor.
- **A reset INA237 is undetectable, and harmless.** The driver writes only `SHUNT_CAL`,
  leaving `CONFIG` and `ADC_CONFIG` at their reset values — and the value it writes, 4096,
  *is* `SHUNT_CAL`'s reset value. So `shunt_cal_matches` reads true whether or not the
  part has been reset, and that is fine here: a part that browns out comes back configured
  exactly as this firmware configured it. Do not add it to the health check; it is a
  condition that is never false. (This is not true of every part in that family — the
  `ina239` driver has to read `CONFIG` back, because it writes the ADC range.)
- **The trigger thresholds are inherited, not measured.** `trigger.min_g` and its siblings
  were tuned empirically on the ESP8266 device's mechanical arrangement. A different cell,
  mounting or noise floor invalidates them, and they must be re-derived from `raw_batch`
  recorded on *this* hardware before anyone trusts an event count.
- **`loadcell.calibration_factor` is a placeholder.** 1125.0 was supplied as a starting
  value pending a real measurement. `scale calibrate` produces the real one.
- **NVS beats `config.json` for the tare and the factor.** A device that reports a weight
  you did not expect after editing `config.json` is probably using a measured value.
  `scale show` says which, and `scale forget` drops the stored pair.
- **A batch payload must fit the MQTT buffer.** `telemetry.batch_max_bytes` (1400) has to
  stay under `CONFIG_MQTT_BUFFER_SIZE` (2048) in `sdkconfig.defaults`. Raising one without
  the other makes every batch publish fail, and it fails looking like a broker problem.

- **`idf.py flash` is not enough.** It writes `ota_0` while the bootloader follows
  `otadata`, so after an OTA a plain flash leaves the device running the old firmware,
  silently. Use `tools/flash_a.py`.
- **OTA refuses an image from a differently-named project.** Renaming `project()` in
  `CMakeLists.txt` means the next OTA is rejected until the running firmware is one built
  under the new name. That is deliberate — uploading the wrong binary is the common
  accident — but it surprises people once.
- **The image is on trial after an OTA.** `app_main()` confirms it at the very end, so
  firmware that crashes during start-up rolls back on the next reset. Do not move that
  call earlier; it would confirm images that never worked.
- **`http_server` refuses to start with authentication enabled and no password.** That is
  not a bug. Set one in `config.json`.
- **A shared component is pinned to a git tag, not a version range.** Upgrading one is
  editing its `version:` in `main/idf_component.yml` to another tag; there is no range
  that picks up a newer release on its own. To edit a component and this project
  together, build with `-DESP_COMPONENTS_DIR=../esp_components` — a local component of
  that name wins over the fetched one, and the pin is then ignored for it. Anything
  changed that way has to be committed and tagged in `esp_components`, and the pin here
  moved to the new tag, before a plain `idf.py build` sees it.
- **A build with `ESP_COMPONENTS_DIR` set rewrites `dependencies.lock`**, replacing the
  git pins with local paths — the component manager records what it resolved, and there
  is no way to point it at a different lockfile. Committing that would un-pin the project
  for everyone. A plain `idf.py reconfigure` puts the pins back; check `git diff` before
  committing after working that way.
- **`dependencies.lock` is committed and target-specific.** It pins the managed component
  versions, so `idf.py set-target` rewrites it. That churn belongs in the commit; do not
  gitignore it, or two people get different component versions.
- **`sdkconfig` is generated and gitignored.** Edit `sdkconfig.defaults` or
  `sdkconfig.defaults.<target>`, then `idf.py reconfigure`.
- **`.vscode/settings.json` sets `IDF_TARGET` as an environment variable**, which outranks
  everything else. Switching targets without updating it fails with a CMake cache
  mismatch; build from a plain shell to avoid it.

## Verifying a change

```sh
idf.py build                       # -Werror is on for main
./tools/flash_a.py -p /dev/ttyACM0
```

On the device: `sys info` over serial, the same shell in a browser at the device's
address, `./tools/get_status.py <host>`, and an OTA round trip with
`./tools/ota_upload.py`.
