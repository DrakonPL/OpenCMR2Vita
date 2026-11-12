#!/usr/bin/env python3
"""Writes the GXM fragment program variants and their table.

Input: src/render/shaders/cg/variants.txt (the fragment keys and pipeline
triples of the PC trace, tools/trace_stats.py --variants) and the template
src/render/shaders/cg/fixed_f.cg.in.

Output:
  src/render/shaders/cg/gxp/fixed_f_<key>_t<alpha test>.cg   one per variant
  src/render/shader_variants_gen.h                            the table

A variant is a fragment key and the form of its alpha test: 0 off, 1 discard
(blending off), 2 colour and alpha 0 (blending on). A fragment key that draws
both with and without blending gets two variants. A discard in a program that
draws with blending breaks the pass on the SGX543, so blending always takes
form 2. Form 2 leaves the pixel as it was only when the blend's destination
factor is 1 at a source of 0: the script refuses other blend pairs. A key
with form 2 also gets form 1: the executor draws the depth of a blended draw
with it, without colour, so that the depth is written only where the alpha
test passes.

A variant reads the arguments of its texture stages and the alpha function
from uniforms: the general variant. An "args <key> <arguments key>" line
(render/shader_key.h Gfx_FragmentArgsKey) adds a variant of the key with them
as constants, for each form of the key. It is much shorter, and the executor
draws with it where the draw's arguments are those.

Files are written only when their text changes, so a second run changes
nothing (and leaves the .gxp files current). --check writes nothing and
fails when an output differs.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CG_DIR = ROOT / "src" / "render" / "shaders" / "cg"
GXP_DIR = CG_DIR / "gxp"
VARIANTS = CG_DIR / "variants.txt"
TEMPLATE = CG_DIR / "fixed_f.cg.in"
HEADER = ROOT / "src" / "render" / "shader_variants_gen.h"

TOP_DISABLE = 1
TOP_BUMP_OPS = (22, 23)
KIND_NONE, KIND_2D, KIND_CUBE, KIND_BUMP, KIND_FIELD, KIND_DOT, KIND_ICON = range(7)
# GFX_BLEND_*: destination factors that are 1 for a source of colour and alpha 0.
BLEND_ONE, BLEND_INVSRCCOLOR, BLEND_INVSRCALPHA = 2, 4, 6
FORM2_DEST_FACTORS = (BLEND_ONE, BLEND_INVSRCCOLOR, BLEND_INVSRCALPHA)

# Texture slots in the SDL GPU executor's binding order (gfx_gpu.cpp
# TextureBindings): the index is the slot.
SLOTS = ["TEX0", "CUBE0", "TEX1", "CUBE1", "BACKDROP", "NATIVE0", "NATIVE1"]
SAMPLER_NAMES = {"TEX0": "tex0", "CUBE0": "cube0", "TEX1": "tex1", "CUBE1": "cube1", "BACKDROP": "backdrop",
                 "NATIVE0": "native0", "NATIVE1": "native1"}


def decode_stage(bits):
    """A 14-bit stage key of render/shader_key.h: colour op, alpha op, kind, scaled."""
    color_op = bits & 31
    if color_op in (0, TOP_DISABLE):
        return {"op": TOP_DISABLE, "alpha_op": TOP_DISABLE, "kind": KIND_NONE, "scaled": 0}
    return {"op": color_op, "alpha_op": (bits >> 5) & 31, "kind": (bits >> 10) & 7, "scaled": (bits >> 13) & 1}


def decode_key(key):
    """A fragment key of render/shader_key.h (Gfx_FragmentKey)."""
    return {
        "stages": [decode_stage(key & 0x3FFF), decode_stage((key >> 14) & 0x3FFF)],
        "alpha_test": (key >> 28) & 1,
        "fog": (key >> 29) & 1,
        "fog_table": (key >> 30) & 1,
        "specular_add": (key >> 31) & 1,
        "fade": (key >> 32) & 1,
    }


def read_variants(path):
    """The fragment keys, the triples (key, blend or None, transformed) and the (key, arguments key) pairs."""
    keys, triples, args = [], [], []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        text = line.split("#", 1)[0].split()
        if not text:
            continue
        where = f"{path.name}:{number}"
        if text[0] == "fragment" and len(text) == 2:
            keys.append(int(text[1], 16))
        elif text[0] == "triple" and len(text) == 4:
            if text[2] == "off":
                blend = None
            else:
                match = re.fullmatch(r"(\d+)/(\d+)", text[2])
                if match is None:
                    raise SystemExit(f"{where}: bad blend pair {text[2]}")
                blend = (int(match.group(1)), int(match.group(2)))
            if text[3] not in ("tl", "3d"):
                raise SystemExit(f"{where}: bad vertex program {text[3]}")
            triples.append((int(text[1], 16), blend, text[3] == "tl"))
        elif text[0] == "args" and len(text) == 3:
            pair = (int(text[1], 16), int(text[2], 16))
            # The compare form is on exactly with the alpha test of the key.
            if (pair[1] >> 40 != 0) != (pair[0] >> 28 & 1 != 0) or pair[1] >> 42 != 0:
                raise SystemExit(f"{where}: arguments key {text[2]} does not fit fragment key {text[1]}")
            if pair not in args:
                args.append(pair)
        else:
            raise SystemExit(f"{where}: cannot read: {line}")
    if len(set(keys)) != len(keys):
        raise SystemExit(f"{path.name}: a fragment key is listed twice")
    for key, _, _ in triples:
        if key not in keys:
            raise SystemExit(f"{path.name}: triple of key {key:09x} without its fragment line")
    for key, _ in args:
        if key not in keys:
            raise SystemExit(f"{path.name}: args of key {key:09x} without its fragment line")
    return keys, triples, args


def alpha_form(key, blend):
    """The alpha test form of a key drawn with a blend pair (None: blending off)."""
    if not decode_key(key)["alpha_test"]:
        return 0
    if blend is None:
        return 1
    if blend[1] not in FORM2_DEST_FACTORS:
        raise SystemExit(f"key {key:09x}: alpha test with blend {blend[0]}/{blend[1]} needs a discard, "
                         f"which breaks a blended pass on the SGX543")
    return 2


def variant_name(key, form, args=None):
    """args: the arguments key of the variant, None for the general one."""
    return f"fixed_f_{key:09x}_t{form}" + ("" if args is None else f"_a{args:011x}")


ARG_SOURCES = ["diffuse", "current", "texel", "fu[FU_TEXTURE_FACTOR]", "specular"]


def arg_expression(bits):
    """One argument of an arguments key (5 bits) as an expression of fixed_f.cg.in."""
    source = bits & 7
    if source >= len(ARG_SOURCES):
        raise SystemExit(f"argument source {source} is not one the fragment program knows")
    text = ARG_SOURCES[source]
    if bits & 8:
        text = f"(1.0 - {text})"
    if bits & 16:
        text = f"({text}).aaaa"
    return text


def stage_arguments(stage, args):
    """The #define lines of a stage's four arguments."""
    names = ["COLOR1", "COLOR2", "ALPHA1", "ALPHA2"]
    if args is None:
        rows = [f"fu[FU_STAGE{stage}_{row}].{c}" for row in ("SOURCE", "COMPLEMENT", "REPLICATE") for c in "xyzw"]
        return [f"#define STAGE{stage}_{name} Arg({rows[i]}, {rows[4 + i]}, {rows[8 + i]}, diffuse, specular, current, texel)"
                for i, name in enumerate(names)]
    bits = args >> (20 * stage)
    return [f"#define STAGE{stage}_{name} {arg_expression(bits >> (5 * i) & 31)}" for i, name in enumerate(names)]


def texture_slots(fields):
    """The slots a variant reads, in the order of their texture units."""
    slots = []
    for i, stage in enumerate(fields["stages"]):
        if stage["op"] == TOP_DISABLE or (i == 1 and fields["stages"][0]["op"] == TOP_DISABLE):
            continue
        if stage["kind"] == KIND_CUBE:
            slots.append(f"CUBE{i}")
        elif stage["kind"] != KIND_NONE or stage["op"] in TOP_BUMP_OPS:
            slots.append(f"TEX{i}")
        if stage["kind"] == KIND_ICON:
            slots.append(f"NATIVE{i}")
    if fields["fade"]:
        slots.append("BACKDROP")
    return slots


def variant_source(key, form, args, template):
    fields = decode_key(key)
    stage0, stage1 = fields["stages"]
    if stage0["op"] == TOP_DISABLE:
        stage1 = decode_stage(0)
    lines = [
        f"// Generated by tools/gen_gxm_shaders.py from fixed_f.cg.in for fragment key {key:09x}"
        + ("" if args is None else f" and arguments key {args:011x}") + ". Do not edit.",
        f"#define STAGE0_OP {stage0['op']}",
        f"#define STAGE0_ALPHA_OP {stage0['alpha_op']}",
        f"#define STAGE0_KIND {stage0['kind']}",
        f"#define STAGE0_SCALED {stage0['scaled']}",
        f"#define STAGE1_OP {stage1['op']}",
        f"#define STAGE1_ALPHA_OP {stage1['alpha_op']}",
        f"#define STAGE1_KIND {stage1['kind']}",
        f"#define STAGE1_SCALED {stage1['scaled']}",
        f"#define ALPHA_TEST {form}",
        f"#define ALPHA_COMPARE {3 if args is None else args >> 40 & 3}",
        f"#define FOG {fields['fog']}",
        f"#define FOG_TABLE {fields['fog_table']}",
        f"#define SPECULAR_ADD {fields['specular_add']}",
        f"#define FADE {fields['fade']}",
    ]
    lines += stage_arguments(0, args) + stage_arguments(1, args)
    for unit, slot in enumerate(texture_slots(fields)):
        kind = "samplerCUBE" if slot.startswith("CUBE") else "sampler2D"
        lines.append(f"uniform {kind} {SAMPLER_NAMES[slot]} : TEXUNIT{unit};")
    return "\n".join(lines) + "\n\n" + template


def header_source(variants, triples):
    index = {(key, form): i for i, (key, form, args) in enumerate(variants) if args is None}
    out = [
        "// Generated by tools/gen_gxm_shaders.py from src/render/shaders/cg/variants.txt. Do not edit.",
        "// The GXM fragment program variants (shaders/cg/gxp/fixed_f_*.cg) and the",
        "// pipeline triples the game draws with, for the executor's patcher cache.",
        "#ifndef OPENCMR2_SHADER_VARIANTS_GEN_H",
        "#define OPENCMR2_SHADER_VARIANTS_GEN_H",
        "",
        "#include <stdint.h>",
        "",
        "// A draw's texture slots, in the order of the SDL GPU executor's bindings.",
        "enum GxmTextureSlot {",
        "    GXM_SLOT_NONE = -1,",
    ]
    out += [f"    GXM_SLOT_{slot}," for slot in SLOTS]
    out += [
        "};",
        f"#define GXM_TEXTURE_UNITS {len(SLOTS)}",
        "",
        "// alphaTest: 0 off, 1 discard (blending off), 2 colour and alpha 0 (blending on).",
        "// args: the arguments key (shader_key.h) the variant has as constants, or",
        "// GXM_ARGS_GENERAL: it reads the arguments from uniforms.",
        "// units[n]: the slot the program reads through TEXUNITn.",
        "#define GXM_ARGS_GENERAL (~0ull)",
        "struct GxmFragmentVariant {",
        "    uint64_t key;",
        "    int alphaTest;",
        "    uint64_t args;",
        "    const char *name;",
        "    const void *gxp;    // NULL off the Vita",
        "    int8_t units[GXM_TEXTURE_UNITS];",
        "};",
        "",
        "// Whether alpha test form 2 leaves a pixel as it was with this destination",
        "// factor (GFX_BLEND_*): the factor is 1 for a source of colour and alpha 0.",
        "inline bool Gxm_Form2Keeps(int destBlend)",
        "{",
        "    return " + " || ".join(f"destBlend == {factor}" for factor in FORM2_DEST_FACTORS) + ";",
        "}",
        "",
        "// A fragment key drawn with a blend pair (srcBlend 0: blending off), after",
        "// the vertex program of pre-transformed or 3D vertices.",
        "struct GxmPipelineTriple {",
        "    int variant;    // index into kGxmFragmentVariants: the general variant of the key",
        "    int srcBlend, destBlend;",
        "    bool transformed;",
        "};",
        "",
        "#ifdef VITA",
        'extern "C" {',
    ]
    out += [f"extern const unsigned char {variant_name(*v)}_gxp_start[];" for v in variants]
    out += [
        "}",
        "#define GXM_GXP(name) name##_gxp_start",
        "#else",
        "#define GXM_GXP(name) nullptr",
        "#endif",
        "",
        "static const GxmFragmentVariant kGxmFragmentVariants[] = {",
    ]
    for key, form, args in variants:
        units = [f"GXM_SLOT_{slot}" for slot in texture_slots(decode_key(key))]
        units += ["GXM_SLOT_NONE"] * (len(SLOTS) - len(units))
        name = variant_name(key, form, args)
        args_text = "GXM_ARGS_GENERAL" if args is None else f"0x{args:011x}ull"
        out.append(f'    {{ 0x{key:09x}ull, {form}, {args_text}, "{name}", GXM_GXP({name}), {{ {", ".join(units)} }} }},')
    out += [
        "};",
        "",
        "static const GxmPipelineTriple kGxmPipelineTriples[] = {",
    ]
    for key, blend, transformed in sorted(triples, key=lambda t: (t[0], t[1] or (0, 0), t[2])):
        form = alpha_form(key, blend)
        src, dst = blend if blend is not None else (0, 0)
        out.append(f"    {{ {index[(key, form)]}, {src}, {dst}, {'true' if transformed else 'false'} }},")
    out += ["};", "", "#endif", ""]
    return "\n".join(out)


def outputs():
    """Every file the generator owns, path -> text."""
    keys, triples, args = read_variants(VARIANTS)
    template = TEMPLATE.read_text()
    variants = set()
    for key in keys:
        forms = {alpha_form(key, blend) for k, blend, _ in triples if k == key}
        if not forms:
            raise SystemExit(f"{VARIANTS.name}: fragment key {key:09x} has no triple (its blending is unknown)")
        # A blended draw that writes depth draws its depth first with the
        # discard form (the executor's depth pass).
        if 2 in forms:
            forms.add(1)
        variants |= {(key, form, None) for form in forms}
        variants |= {(key, form, a) for form in forms for k, a in args if k == key}
    # The general variant of a key and form first, then those of its arguments keys.
    variants = sorted(variants, key=lambda v: (v[0], v[1], -1 if v[2] is None else v[2]))
    files = {GXP_DIR / f"{variant_name(*v)}.cg": variant_source(*v, template) for v in variants}
    files[HEADER] = header_source(variants, triples)
    return files


def orphans(files):
    """Generated variants (and their .gxp) that the variant list no longer has."""
    wanted = {path.stem for path in files if path.parent == GXP_DIR}
    return sorted(p for p in GXP_DIR.glob("fixed_f_*") if p.suffix in (".cg", ".gxp") and p.stem not in wanted)


def differences():
    """The outputs that are missing or differ, and the orphans: empty when current."""
    files = outputs()
    stale = [path for path, text in files.items() if not path.exists() or path.read_text() != text]
    return stale + orphans(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="write nothing; fail when an output differs")
    args = parser.parse_args()
    if args.check:
        stale = differences()
        for path in stale:
            print(f"gen_gxm_shaders: not current: {path.relative_to(ROOT)}")
        return 1 if stale else 0
    files = outputs()
    GXP_DIR.mkdir(exist_ok=True)
    written = 0
    for path, text in files.items():
        if path.exists() and path.read_text() == text:
            continue
        path.write_text(text, newline="\n")
        written += 1
    for path in orphans(files):
        path.unlink()
        print(f"gen_gxm_shaders: removed {path.relative_to(ROOT)}")
    variants = sum(1 for path in files if path.parent == GXP_DIR)
    print(f"gen_gxm_shaders: {variants} variants, {written} files written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
