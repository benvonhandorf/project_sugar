# fs/

Contents of the `res` LittleFS partition, built into an image and flashed with the
firmware by `littlefs_create_partition_image()` in the root `CMakeLists.txt`.

**`config.json` belongs here and is gitignored.** Copy `config/config.example.json` to
`fs/config.json` and fill in real values:

```sh
cp config/config.example.json fs/config.json
```

It is gitignored because it holds WiFi and broker credentials. The reference projects this
template came from committed theirs.

A build with no `fs/config.json` still works: `config_store` reports the file missing and
every optional field takes the default declared in its owning component's schema. It will
not join a network, which is the point — a device should not ship with someone else's
credentials baked in.

An SD card mounted at `/sdcard` with a `config.json` on it overrides this copy at boot,
so a unit can be reconfigured without reflashing.
