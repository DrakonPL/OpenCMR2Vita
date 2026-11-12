#!/usr/bin/env python3
"""Patch .velf LOAD segments from RWE (Read+Write+Execute) to R+E.

Background:
    GCC 15.2 / arm-vita-eabi-binutils marks .rodata writable when it contains
    relocations (vtables, const pointer tables, etc.), which causes the linker
    to emit a single LOAD segment with RWE permissions. The PS Vita loader on
    FW 3.65 rejects such binaries with error C1-2613-2 ("cannot start app").

    Older toolchains laid out the segments differently and did not hit this.
    Until vitasdk's linker behavior is fixed upstream, patch the offending
    p_flags in the .velf before vita-make-fself wraps it.

Usage:
    python3 fix_vita_rwe.py <path/to/foo.velf>
"""
import sys
import struct

def main(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if data[:4] != b"\x7fELF" or data[4] != 1:
        sys.exit("not an ELF32 file")

    e_phoff = struct.unpack_from("<I", data, 28)[0]
    e_phentsize = struct.unpack_from("<H", data, 42)[0]
    e_phnum = struct.unpack_from("<H", data, 44)[0]
    if e_phentsize != 32:
        sys.exit(f"unexpected phentsize {e_phentsize}")

    PT_LOAD = 1
    RWE = 0x7   # R+W+X
    RE  = 0x5   # R+X

    changed = 0
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", data, off)[0]
        p_flags = struct.unpack_from("<I", data, off + 24)[0]
        if p_type == PT_LOAD and p_flags == RWE:
            struct.pack_into("<I", data, off + 24, RE)
            changed += 1

    if changed:
        with open(path, "wb") as f:
            f.write(bytes(data))
        print(f"fix_vita_rwe: patched {changed} RWE LOAD segment(s) in {path}")
    else:
        print(f"fix_vita_rwe: no RWE LOAD segments in {path}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: fix_vita_rwe.py <file.velf>")
    main(sys.argv[1])
