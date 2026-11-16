#!/usr/bin/env python3
"""Fails when a name in a .vpk has a byte outside ASCII: VitaShell does not install it."""

import sys
import zipfile


def non_ascii_names(names):
    return [name for name in names if not name.isascii()]


def main():
    if len(sys.argv) != 2:
        print("usage: check_vpk_names.py <file.vpk>", file=sys.stderr)
        return 2
    with zipfile.ZipFile(sys.argv[1]) as vpk:
        names = vpk.namelist()
    bad = non_ascii_names(names)
    for name in bad:
        print(f"check_vpk_names: not ASCII: {name!r}", file=sys.stderr)
    if bad:
        return 1
    print(f"check_vpk_names: {len(names)} names, all ASCII")
    return 0


if __name__ == "__main__":
    sys.exit(main())
