[← docs](README.md) · [Project README](../README.md)

# Updating firmware

The partition table has two app slots and no `factory`. The running firmware writes the
other slot, then boots it; the previous firmware stays where it is, so a bad update can be
undone.

## Flashing over a cable

```sh
./tools/flash_a.py -p /dev/ttyACM0
```

**Not `idf.py flash`.** That writes `ota_0`, but the bootloader follows `otadata`. After
an OTA the device is running `ota_1`, so a plain flash writes the slot that is not
selected and the device comes back running the *old* firmware — with no error, which is
the worst way for this to go wrong. `flash_a.py` erases `otadata` as part of flashing.

## Over the air

```sh
./tools/ota_upload.py device.local build/c3_nau7802_trigger.bin -u admin
```

The image header and app descriptor are checked after roughly the first kilobyte, so the
wrong binary is refused quickly and by name rather than after the whole transfer. The
endpoint is authenticated; anything that can replace the firmware is.

Failures name the step that failed, because a rejected signature, a partition too small
and an interrupted upload need completely different responses.

## Rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on. A newly written image boots **on trial**:
if it never confirms itself, the next reset returns to the previous firmware.

`app_main()` confirms at the very end, after everything has been brought up. That
placement is the entire mechanism — an image that crashes during start-up never reaches
that line and undoes itself. **Do not move it earlier.**

If a device must prove more than "it started" — that it reached the broker, say — confirm
from there instead, and leave `app_main()` to say nothing:

```
sys confirm     # mark this image good
sys rollback    # return to the previous firmware and reboot
```

## Renaming the project

OTA refuses an image whose app descriptor names a different project, because uploading the
wrong firmware is the common accident. After changing `project()` in `CMakeLists.txt`, the
next OTA is rejected until the running firmware is one built under the new name — flash
over a cable once to cross that gap.

## Signing

Set `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` and give the project a signing key;
an image not signed by it then fails verification at the end of the upload. Without it,
anything that reaches the authenticated endpoint is accepted.
