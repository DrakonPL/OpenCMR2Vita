#!/usr/bin/env python3
"""Fails when a precompiled GXM shader (.gxp) is older than its source.

psp2cgc runs on Windows only (tools/compile_gxm_shaders.bat), so a Vita build
cannot rebuild a .gxp and would pack the old one in silence. The Vita build
runs this check first.

After a compile, --record writes cg/gxp/sources.txt: for each program, a hash
of the text it was compiled from (its .cg and cg/uniforms.h, which every
program includes). The check compares the hashes, not the file times: git
writes files in its own order, so after a checkout a .cg can be newer than
its .gxp with nothing changed. The check fails when:
  - the generated variants differ from what variants.txt and fixed_f.cg.in
    give (tools/gen_gxm_shaders.py --check);
  - a .cg has no .gxp, or its text is not the text the .gxp was compiled from;
  - a .gxp has no .cg.
"""

import argparse
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_gxm_shaders  # noqa: E402

CG_DIR = gen_gxm_shaders.CG_DIR
GXP_DIR = gen_gxm_shaders.GXP_DIR
SHARED = [CG_DIR / "uniforms.h"]
RECORD = GXP_DIR / "sources.txt"


def sources():
    return sorted(CG_DIR.glob("*.cg")) + sorted(GXP_DIR.glob("*.cg"))


def source_hash(cg):
    """The hash of a program's source text, line ends ignored."""
    digest = hashlib.sha256()
    for path in [cg] + SHARED:
        digest.update(path.read_text().encode())
    return digest.hexdigest()


def read_record():
    if not RECORD.exists():
        return {}
    return dict(line.split() for line in RECORD.read_text().splitlines() if line.strip())


def problems():
    found = [f"not generated from the current inputs: {path.name}" for path in gen_gxm_shaders.differences()]
    record = read_record()
    for cg in sources():
        if not (GXP_DIR / (cg.stem + ".gxp")).exists():
            found.append(f"no .gxp: {cg.name}")
        elif record.get(cg.stem) != source_hash(cg):
            found.append(f"stale: {cg.stem}.gxp")
    names = {cg.stem for cg in sources()}
    found += [f"no .cg: {gxp.name}" for gxp in sorted(GXP_DIR.glob("*.gxp")) if gxp.stem not in names]
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--record", action="store_true",
                        help="after a compile: record the source of every .gxp in cg/gxp/sources.txt")
    args = parser.parse_args()
    if args.record:
        lines = [f"{cg.stem} {source_hash(cg)}" for cg in sources()]
        RECORD.write_text("\n".join(lines) + "\n", newline="\n")
    found = problems()
    for line in found:
        print(f"check_gxp_stale: {line}")
    if found:
        print("check_gxp_stale: run tools/compile_gxm_shaders.bat on Windows")
        return 1
    print(f"check_gxp_stale: {len(sources())} shaders current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
