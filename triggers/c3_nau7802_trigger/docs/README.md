# docs/

How to extend this project. The shared components document themselves in
`esp_components/<component>/README.md`; this covers what belongs *here*.

- [MQTT messages](messages.md) — every topic, what it is for, and the delivery
  guarantees. Machine-readable schemas are in [`../schemas/`](../schemas/).
- [Message changes](message_changes.md) — **read this before deploying.** Three payloads
  changed shape from the ESP8266 firmware this replaces, and other systems in this
  repository parse them.
- [Requirements conformance](requirements.md) — what was implemented against
  `REQUIREMENTS.md`, the choices taken where it left one open, and the three places this
  deliberately differs.
- [Configuration](configuration.md) — how one schema becomes the parsers, and how to add
  a section.
- [Bring-up order](bringup.md) — what `app_main()` does and why the order is what it is.
- [Updating firmware](ota.md) — flashing, OTA, rollback, and the failure modes.

Keep documentation beside the code and change it in the same commit. That is the reason
it lives in the repository rather than a wiki.
