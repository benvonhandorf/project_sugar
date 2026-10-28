#!/usr/bin/env python3
"""
Upload firmware to a running device over HTTP.

    ./tools/ota_upload.py device.local build/sensor_stick_trigger.bin -u admin

The device validates the image header before taking the whole upload, so a wrong
binary is refused in about a kilobyte rather than after the transfer. It reboots
into the new image after responding.
"""

import argparse
import getpass
import os
import sys
import urllib.error
import urllib.request


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="device hostname or address")
    ap.add_argument("firmware", help="path to the .bin")
    ap.add_argument("-u", "--user", default="admin", help="HTTP username")
    ap.add_argument("-P", "--password", help="HTTP password; prompted if omitted")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--path", default="/api/ota")
    args = ap.parse_args()

    if not os.path.isfile(args.firmware):
        print(f"No such file: {args.firmware}", file=sys.stderr)
        return 1
    if not args.firmware.endswith(".bin"):
        print("That is not a .bin. Upload the image from the build directory,\n"
              "not the .elf.", file=sys.stderr)
        return 1

    password = args.password or getpass.getpass(f"Password for {args.user}: ")
    payload = open(args.firmware, "rb").read()

    url = f"http://{args.host}:{args.port}{args.path}"
    manager = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    manager.add_password(None, url, args.user, password)
    opener = urllib.request.build_opener(urllib.request.HTTPBasicAuthHandler(manager))

    print(f"Uploading {len(payload)} bytes to {url}")
    request = urllib.request.Request(url, data=payload, method="POST")
    request.add_header("Content-Type", "application/octet-stream")

    try:
        # Generous: the device erases and writes flash as it receives.
        with opener.open(request, timeout=180) as response:
            print(response.read().decode(errors="replace").strip())
    except urllib.error.HTTPError as exc:
        # The device explains which step failed; that text is the useful part.
        print(f"{exc.code} {exc.reason}: {exc.read().decode(errors='replace')}",
              file=sys.stderr)
        return 1
    except urllib.error.URLError as exc:
        print(f"Could not reach {url}: {exc.reason}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
