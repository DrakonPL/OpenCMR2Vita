#!/usr/bin/env python3
"""Fails when a precompiled GXM shader (.gxp) is older than its source.

psp2cgc runs on Windows only (tools/compile_gxm_shaders.bat), so a Vita build
cannot rebuild a .gxp and would pack the old one in silence. The Vita build
runs this check first. It fails when:
  - the generated variants differ from what variants.txt and fixed_f.cg.in
    give (tools/gen_gxm_shaders.py --check);
  - a .cg (in cg/ or cg/gxp/) has no .gxp, or one older than the .cg or than
    cg/uniforms.h, which every program includes;
  - a .gxp has no .cg.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_gxm_shaders  # noqa: E402

CG_DIR = gen_gxm_shaders.CG_DIR
GXP_DIR = gen_gxm_shaders.GXP_DIR
SHARED = [CG_DIR / "uniforms.h"]


def problems():
    found = [f"not generated from the current inputs: {path.name}" for path in gen_gxm_shaders.differences()]
    shared = max(path.stat().st_mtime for path in SHARED)
    sources = sorted(CG_DIR.glob("*.cg")) + sorted(GXP_DIR.glob("*.cg"))
    for cg in sources:
        gxp = GXP_DIR / (cg.stem + ".gxp")
        if not gxp.exists():
            found.append(f"no .gxp: {cg.name}")
        elif gxp.stat().st_mtime < max(cg.stat().st_mtime, shared):
            found.append(f"stale: {gxp.name}")
    names = {cg.stem for cg in sources}
    found += [f"no .cg: {gxp.name}" for gxp in sorted(GXP_DIR.glob("*.gxp")) if gxp.stem not in names]
    return found, len(sources)


def main():
    found, count = problems()
    for line in found:
        print(f"check_gxp_stale: {line}")
    if found:
        print("check_gxp_stale: run tools/compile_gxm_shaders.bat on Windows")
        return 1
    print(f"check_gxp_stale: {count} shaders current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
