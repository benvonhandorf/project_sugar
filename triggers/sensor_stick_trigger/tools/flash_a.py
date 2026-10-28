#!/usr/bin/env python3
"""
Flash the firmware and make the device actually boot it.

`idf.py flash` writes ota_0, but the bootloader follows `otadata`. After an OTA
the device is running ota_1, so a plain flash writes the slot that is not
selected and the device comes back running the *old* firmware -- with no error,
which is the worst way for this to go wrong.

This erases otadata as part of flashing, so "flash" means what it appears to.

    ./tools/flash_a.py -p /dev/ttyACM0
"""

import argparse
import subprocess
import sys


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    return subprocess.call(cmd)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", help="serial port, e.g. /dev/ttyACM0")
    ap.add_argument("-b", "--baud", help="flashing baud rate")
    ap.add_argument("--no-monitor", action="store_true",
                    help="do not open the monitor afterwards")
    args = ap.parse_args()

    port = ["-p", args.port] if args.port else []
    baud = ["-b", args.baud] if args.baud else []

    if run(["idf.py", *port, *baud, "flash"]) != 0:
        return 1

    # Erase the OTA selection so the bootloader falls back to the first app
    # partition -- the one we just wrote.
    print("\nErasing otadata so the freshly flashed slot is the one that boots.")
    rc = run(["parttool.py", *port, "erase_partition", "--partition-name", "otadata"])
    if rc != 0:
        print("\nCould not erase otadata. The device may still boot the previous\n"
              "firmware from the other OTA slot.", file=sys.stderr)
        return rc

    if not args.no_monitor:
        run(["idf.py", *port, "monitor"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
