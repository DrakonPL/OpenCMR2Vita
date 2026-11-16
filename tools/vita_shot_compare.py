#!/usr/bin/env python3
"""Compares a PS Vita screenshot with the PC's frame of the same screen.

    tools/vita_shot_compare.py VITA.bmp PC.bmp [DIFF.bmp]

The Vita writes shot-NNNNNN.bmp in its user directory with [video]
shot_every = N (dev builds). The PC writes frame-NNNNNN.bmp with
OPENCMR2_FRAME_DUMP. Both are 32-bit BMPs at 960x544.

The tool prints the count of pixels that differ by more than 8 in a channel,
and the shift of the whole picture that gives the fewest such pixels, up to
2 pixels each way. Text edges and dithering differ between GPUs. A shift
that is not (0, 0) is a finding: a wrong Y flip or half-pixel offset moves
the whole screen. DIFF.bmp gets the differing pixels in white.
"""

import struct
import sys

THRESHOLD = 8
MAX_SHIFT = 2


def read_bmp(path):
    """Returns (width, height, rows): rows from the top, each a bytes of B, G, R, A."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if bits not in (24, 32) or compression not in (0, 3):
        raise ValueError(f"{path}: {bits}-bit, compression {compression}: only plain 24 and 32 bits")
    top_down = height < 0
    height = abs(height)
    step = width * bits // 8
    stride = (step + 3) & ~3
    rows = []
    for y in range(height):
        start = offset + y * stride
        row = data[start:start + step]
        if bits == 24:
            row = b"".join(row[i:i + 3] + b"\xff" for i in range(0, len(row), 3))
        rows.append(row)
    if not top_down:
        rows.reverse()
    return width, height, rows


def write_bmp(path, width, height, rows):
    pixels = b"".join(rows)
    header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, len(pixels), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(header + info + pixels)


def differs_at(a, b, x, dx):
    """Pixel x of row a against pixel x - dx of row b, on B, G and R."""
    i, j = x * 4, (x - dx) * 4
    return (abs(a[i] - b[j]) > THRESHOLD or abs(a[i + 1] - b[j + 1]) > THRESHOLD or
            abs(a[i + 2] - b[j + 2]) > THRESHOLD)


def count_differences(shot, control, dx=0, dy=0, mask=None):
    """Pixels of shot that differ from control moved by (dx, dy), inside both."""
    width, height, a_rows = shot
    _, _, b_rows = control
    count = 0
    for y in range(max(0, dy), min(height, height + dy)):
        a = a_rows[y]
        b = b_rows[y - dy]
        for x in range(max(0, dx), min(width, width + dx)):
            if differs_at(a, b, x, dx):
                count += 1
                if mask is not None:
                    mask[y][x * 4:x * 4 + 4] = b"\xff\xff\xff\xff"
    return count


def best_shift(shot, control):
    """The (dx, dy) with the fewest differing pixels; (0, 0) wins a tie."""
    best = (count_differences(shot, control), 0, 0)
    for dy in range(-MAX_SHIFT, MAX_SHIFT + 1):
        for dx in range(-MAX_SHIFT, MAX_SHIFT + 1):
            count = count_differences(shot, control, dx, dy) if (dx, dy) != (0, 0) else best[0]
            if count < best[0]:
                best = (count, dx, dy)
    return best


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    shot = read_bmp(argv[1])
    control = read_bmp(argv[2])
    if shot[:2] != control[:2]:
        print(f"sizes differ: {shot[0]}x{shot[1]} and {control[0]}x{control[1]}", file=sys.stderr)
        return 1
    width, height = shot[:2]
    mask = [bytearray(width * 4) for _ in range(height)] if len(argv) == 4 else None
    count = count_differences(shot, control, mask=mask)
    fewest, dx, dy = best_shift(shot, control)
    print(f"{count} of {width * height} pixels differ by more than {THRESHOLD} "
          f"({100.0 * count / (width * height):.2f}%)")
    print(f"best shift: ({dx}, {dy}) with {fewest} pixels")
    if mask is not None:
        write_bmp(argv[3], width, height, [bytes(row) for row in mask])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
