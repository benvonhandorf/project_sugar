#!/usr/bin/env python3
"""
Follow a device's MQTT topics, including the log.

    ./tools/mqtt_tail.py broker.local 'device/esp-device/#'

Requires mosquitto_sub. Credentials come from the command line or the
MQTT_USERNAME and MQTT_PASSWORD environment variables, never from this file --
the scripts this replaces had a broker password committed in them.
"""

import argparse
import os
import shutil
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("broker")
    ap.add_argument("topic", nargs="?", default="#")
    ap.add_argument("-u", "--user", default=os.environ.get("MQTT_USERNAME"))
    ap.add_argument("-P", "--password", default=os.environ.get("MQTT_PASSWORD"))
    ap.add_argument("--port", type=int, default=1883)
    args = ap.parse_args()

    if not shutil.which("mosquitto_sub"):
        print("mosquitto_sub not found (apt install mosquitto-clients)", file=sys.stderr)
        return 1

    cmd = ["mosquitto_sub", "-h", args.broker, "-p", str(args.port),
           "-t", args.topic, "--verbose"]
    if args.user:
        cmd += ["-u", args.user]
    if args.password:
        cmd += ["-P", args.password]

    print("+", " ".join("***" if a == args.password else a for a in cmd), flush=True)
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
