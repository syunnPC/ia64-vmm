#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Print the newest committed PLOG generation from a raw image or device."""

import argparse
import sys
from persistence import read_logs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("media", help="MBR/FAT32 image or block device")
    parser.add_argument("--crash", action="store_true", help="read the dedicated crash record")
    args = parser.parse_args()
    with open(args.media, "rb") as f:
        result = read_logs(f, args.crash)
    if result is None:
        raise ValueError("no valid committed log generation")
    generation, crash, text = result
    print(f"PLOG generation={generation} crash={int(crash)} bytes={len(text)}", file=sys.stderr)
    sys.stdout.buffer.write(text)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as e:
        sys.exit(f"read_logs: {e}")
