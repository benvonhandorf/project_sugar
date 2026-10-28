[← docs](README.md) · [Project README](../README.md)

# Configuration

There is **one** authored description of this device's configuration:
`config/config_schema.json`. The C types, the parsers and the top-level walker are all
generated from it, so there is nothing to keep in step by hand.

## How a section works

Each section `$ref`s the schema belonging to the component that consumes it:

```json
"required": ["config_version", "wifi", "mqtt", "ntp", "http"],
"properties": {
  "wifi": { "$ref": "wifi_manager/wifi_manager_config_schema.json" },
  "mqtt": { "$ref": "mqtt_manager/mqtt_manager_config_schema.json" }
}
```

The first path segment is the **component that owns the fragment**, resolved to that
component's directory — so the reference says nothing about where the component lives.

At configure time the build derives a *sections* schema in which each of those becomes a
byte-offset-and-length slice, and generates a walker from it. The project then hands each
slice to the parser belonging to the owning component, **in place**, with no copy:

```c
#define SECTION_PARSER_wifi json_parse_wifi_manager_config_with_len
APP_CONFIG_SECTIONS(PARSE_SECTION)
```

`APP_CONFIG_SECTIONS` is generated from the same schema, so a section added to the schema
and not wired up in `config_reader.c` **fails to compile** rather than being silently left
zeroed.

## Adding a section

1. `$ref` the owning component's schema in `config/config_schema.json`, and add the key to
   `required` — a slice cannot carry a default, so write `{}` in the config file to mean
   "all defaults".
2. Add the component's generated type to `app_config_full_t` in `config_reader.h`.
3. Add one `#define SECTION_PARSER_<name>` line in `config_reader.c`.

## Defaults, and why there is no `config.json` in the repository

Every optional field carries a `default` in the schema of the component that owns it, so
there is exactly one place a default is written and an absent field still produces a
usable value. A device with no `config.json` boots onto its own access point with the
console running — which is what a freshly flashed unit needs.

`fs/config.json` is gitignored because it holds the WiFi passphrase and the broker
password. Copy `config/config.example.json` and edit it.

## Where it is read from

`/sdcard/config.json` if present, otherwise `/res/config.json` from the LittleFS
partition. A card overrides the built-in copy so a unit can be reconfigured without
reflashing; writes always go to flash, because a card can be pulled at any moment.

## Replacing it at runtime

```sh
curl -u admin:secret -X POST --data-binary @fs/config.json http://device.local/api/config
```

It is parsed before it is written, so a configuration that would not load cannot replace
one that does. The previous copy is kept as `config.json.bak`. The device restarts after
the response has been sent.

## Versioning

`config_version` has a `minimum` and `maximum` in the schema, so the generated parser
rejects a file from a newer firmware before any section is touched. Raise the maximum when
this firmware learns a newer layout.
