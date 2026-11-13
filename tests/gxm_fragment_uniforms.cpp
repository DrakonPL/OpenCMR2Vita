// The fragment uniforms of the GXM executor (src/render/gxm_fragment_uniforms.h):
// draws that differ only in textures whose size their program does not read
// have the same rows, so the executor does not give the rows again. No GXM.

#include "render/gxm_fragment_uniforms.h"

#include <cstdio>
#include <cstring>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

bool SameRows(const GfxDrawState &a, const GfxDrawState &b)
{
    float rowsA[FU_ROWS][4], rowsB[FU_ROWS][4];
    Gxm_FillFragmentUniforms(a, 960, 544, true, rowsA);
    Gxm_FillFragmentUniforms(b, 960, 544, true, rowsB);
    return memcmp(rowsA, rowsB, sizeof(rowsA)) == 0;
}

} // namespace

int main()
{
    GfxTexture road{}, grass{}, digits{}, dot{};
    road.format = grass.format = digits.format = dot.format = GFX_FORMAT_BGRA8;
    road.width = road.height = 256;
    grass.width = 128;
    grass.height = 64;
    digits.width = digits.height = 32;
    dot.width = dot.height = 16;
    dot.flags = GFX_TEXTURE_MENU_DOT;

    // Scenery: a filtered texture with mipmaps. The program does not read its
    // size: another texture of another size gives the same rows.
    GfxDrawState scenery;
    scenery.stages[0].texture = &road;
    scenery.stages[0].magFilter = GFX_FILTER_LINEAR;
    scenery.stages[0].mipFilter = GFX_MIPFILTER_LINEAR;
    scenery.stages[1].colorOp = GFX_TOP_DISABLE;
    REQUIRE(!Gxm_ReadsTextureSize(scenery.stages[0]));
    GfxDrawState other = scenery;
    other.stages[0].texture = &grass;
    REQUIRE(SameRows(scenery, other));
    float rows[FU_ROWS][4];
    Gxm_FillFragmentUniforms(scenery, 960, 544, true, rows);
    REQUIRE(rows[FU_TEX_SIZE0][0] == 0.0f && rows[FU_TEX_SIZE1][0] == 0.0f);

    // What the program reads still changes the rows.
    other.alphaRef = 0x80;
    REQUIRE(!SameRows(scenery, other));
    other = scenery;
    other.textureFactor = 0x80ffffff;
    REQUIRE(!SameRows(scenery, other));
    other = scenery;
    other.fadeStart = 50.0f;
    other.fadeEnd = 90.0f;
    REQUIRE(!SameRows(scenery, other));

    // A sprite: point filtered without mipmaps. Its program reads the size.
    GfxDrawState hud;
    hud.stages[0].texture = &digits;
    hud.stages[1].colorOp = GFX_TOP_DISABLE;
    REQUIRE(Gxm_ReadsTextureSize(hud.stages[0]));
    Gxm_FillFragmentUniforms(hud, 960, 544, false, rows);
    REQUIRE(rows[FU_TEX_SIZE0][0] == 32.0f && rows[FU_TEX_SIZE0][1] == 32.0f);
    other = hud;
    other.stages[0].texture = &road;
    REQUIRE(!SameRows(hud, other));

    // A menu dot in stage 1, filtered: its program reads the size also.
    GfxDrawState menu = scenery;
    menu.stages[1].colorOp = GFX_TOP_MODULATE;
    menu.stages[1].texture = &dot;
    menu.stages[1].magFilter = GFX_FILTER_LINEAR;
    REQUIRE(Gxm_ReadsTextureSize(menu.stages[1]));
    Gxm_FillFragmentUniforms(menu, 960, 544, false, rows);
    REQUIRE(rows[FU_TEX_SIZE1][0] == 16.0f && rows[FU_TEX_SIZE0][0] == 0.0f);

    // No backdrop copy: the fade range is beyond any distance, with different
    // edges.
    REQUIRE(rows[FU_DISTANCE_FADE][0] == kGxmNoFadeStart && rows[FU_DISTANCE_FADE][1] == kGxmNoFadeEnd);
    REQUIRE(kGxmNoFadeStart < kGxmNoFadeEnd);

    printf("gxm_fragment_uniforms: ok\n");
    return 0;
}
