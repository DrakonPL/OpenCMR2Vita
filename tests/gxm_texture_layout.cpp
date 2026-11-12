// The linear layout of a GXM texture (render/gxm_texture_layout.h): rows
// padded to 8 texels, block rows for BC1 and BC3, the levels back to back.

#include "render/gxm_texture_layout.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// Invariants for any texture: levels back to back, GPU rows never shorter than
// the CPU rows, 32-bit rows a whole number of 8 texels.
int CheckLayout(int format, int width, int height, int mipLevels)
{
    const GxmTextureLayout l = Gxm_LinearLayout(format, width, height, mipLevels);
    unsigned end = 0;
    for (int m = 0; m < l.levels; m++) {
        const GxmLevelLayout &level = l.level[m];
        REQUIRE(level.offset == end);
        REQUIRE(level.rowBytes >= level.sourceRowBytes && level.rows >= 1);
        if (format != GFX_FORMAT_BC1 && format != GFX_FORMAT_BC3)
            REQUIRE(level.rowBytes % 32 == 0);
        end += level.rowBytes * level.rows;
    }
    REQUIRE(end == l.bytes);
    REQUIRE(l.bytes % 16 == 0 || format == GFX_FORMAT_BC1);
    return 0;
}

} // namespace

int main()
{
    // A 256x256 BGRA8 texture with 9 levels: the levels under 8 texels wide
    // are padded to 8.
    GxmTextureLayout a = Gxm_LinearLayout(GFX_FORMAT_BGRA8, 256, 256, 9);
    REQUIRE(a.levels == 9);
    REQUIRE(a.level[0].rowBytes == 1024 && a.level[0].rows == 256);
    REQUIRE(a.level[6].rowBytes == 32 && a.level[6].rows == 4 && a.level[6].sourceRowBytes == 16);
    REQUIRE(a.level[8].rowBytes == 32 && a.level[8].rows == 1 && a.level[8].sourceRowBytes == 4);
    REQUIRE(a.level[1].offset == 262144);
    REQUIRE(a.bytes == 262144 + 65536 + 16384 + 4096 + 1024 + 256 + 128 + 64 + 32);

    // A width that is not a multiple of 8 (a port-made font texture).
    GxmTextureLayout b = Gxm_LinearLayout(GFX_FORMAT_BGRA8, 100, 30, 1);
    REQUIRE(b.level[0].rowBytes == 416 && b.level[0].sourceRowBytes == 400 && b.bytes == 416 * 30);

    // BC1: rows of 4x4 blocks of 8 bytes; small levels padded to 2 blocks.
    GxmTextureLayout c = Gxm_LinearLayout(GFX_FORMAT_BC1, 64, 64, 7);
    REQUIRE(c.level[0].rowBytes == 128 && c.level[0].rows == 16 && c.level[0].sourceRowBytes == 128);
    REQUIRE(c.level[4].rowBytes == 16 && c.level[4].rows == 1 && c.level[4].sourceRowBytes == 8);
    REQUIRE(c.level[6].rowBytes == 16 && c.level[6].rows == 1 && c.level[6].sourceRowBytes == 8);
    // BC3: blocks of 16 bytes.
    GxmTextureLayout d = Gxm_LinearLayout(GFX_FORMAT_BC3, 128, 32, 1);
    REQUIRE(d.level[0].rowBytes == 512 && d.level[0].rows == 8);

    // The bump format: 3 bytes per texel in, 4 out.
    GxmTextureLayout e = Gxm_LinearLayout(GFX_FORMAT_BUMP_DUDVL, 16, 16, 1);
    REQUIRE(e.level[0].sourceRowBytes == 48 && e.level[0].rowBytes == 64);

    // Level counts are clamped to 1..16.
    REQUIRE(Gxm_LinearLayout(GFX_FORMAT_BGRX8, 8, 8, 0).levels == 1);

    for (int format = GFX_FORMAT_BGRX8; format <= GFX_FORMAT_BUMP_DUDVL; format++)
        for (int w = 1; w <= 1024; w = w * 3 + 1)
            for (int h = 1; h <= 512; h = h * 2 + 3)
                if (CheckLayout(format, w, h, 10))
                    return 1;

    printf("gxm_texture_layout: ok\n");
    return 0;
}
