#!/usr/bin/env python3
"""tools/vita_shot_compare.py on small synthetic BMPs: both row orders, the
count of differing pixels, and the shift of a whole picture."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import vita_shot_compare as shots  # noqa: E402

W, H = 24, 12


def picture(dx=0, dy=0):
    """A white block on black, moved by (dx, dy)."""
    rows = []
    for y in range(H):
        row = bytearray(W * 4)
        for x in range(W):
            if 8 <= x - dx < 14 and 3 <= y - dy < 8:
                row[x * 4:x * 4 + 4] = b"\xff\xff\xff\xff"
        rows.append(bytes(row))
    return W, H, rows


def write_bottom_up_24(path, rows):
    """A 24-bit BMP stored from the bottom, as many tools write it."""
    import struct
    stride = (W * 3 + 3) & ~3
    body = b"".join(b"".join(r[x * 4:x * 4 + 3] for x in range(W)).ljust(stride, b"\0") for r in reversed(rows))
    header = struct.pack("<2sIHHI", b"BM", 54 + len(body), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(header + info + body)


def main():
    failures = 0

    def check(condition, text):
        nonlocal failures
        if not condition:
            print("FAIL:", text)
            failures += 1

    with tempfile.TemporaryDirectory() as d:
        top = os.path.join(d, "top.bmp")
        bottom = os.path.join(d, "bottom.bmp")
        shots.write_bmp(top, *picture())
        write_bottom_up_24(bottom, picture()[2])
        a, b = shots.read_bmp(top), shots.read_bmp(bottom)
        check(a[:2] == (W, H) and b[:2] == (W, H), "both sizes read")
        check(shots.count_differences(a, b) == 0, "the two row orders give the same picture")
        check(a[2][3][8 * 4] == 0xff and a[2][8][8 * 4] == 0, "the block starts on row 3 from the top")

    same = picture()
    check(shots.best_shift(same, same) == (0, 0, 0), "a picture against itself: no shift")
    moved = picture(1, -1)
    check(shots.count_differences(moved, same) > 0, "a moved block differs")
    check(shots.best_shift(moved, same) == (0, 1, -1), "the shift of the whole picture is found")
    if failures:
        return 1
    print("vita_shot_compare: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
