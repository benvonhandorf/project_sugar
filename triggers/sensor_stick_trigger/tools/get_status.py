#!/usr/bin/env python3
"""
Fetch the device status.

    ./tools/get_status.py device.local
    ./tools/get_status.py device.local --watch 5
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request


def fetch(url):
    with urllib.request.urlopen(url, timeout=10) as response:
        return json.loads(response.read().decode())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--path", default="/api/status")
    ap.add_argument("--watch", type=float, metavar="SECONDS",
                    help="poll forever at this interval")
    args = ap.parse_args()

    url = f"http://{args.host}:{args.port}{args.path}"
    while True:
        try:
            print(json.dumps(fetch(url), indent=2))
        except (urllib.error.URLError, OSError) as exc:
            print(f"{url}: {exc}", file=sys.stderr)
            if not args.watch:
                return 1
        if not args.watch:
            return 0
        time.sleep(args.watch)


if __name__ == "__main__":
    sys.exit(main())
