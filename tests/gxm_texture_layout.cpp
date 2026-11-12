// The layout of a GXM texture (render/gxm_texture_layout.h): linear 32-bit
// levels with rows padded to 8 texels; swizzled BC1 and BC3 levels of
// power-of-two blocks in Morton order; the levels back to back.

#include "render/gxm_texture_layout.h"

#include <cstdio>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// Invariants for any texture: levels back to back, GPU rows never shorter
// than the CPU rows, linear 32-bit rows a whole number of 8 texels.
int CheckLayout(int format, int width, int height, int mipLevels)
{
    const GxmTextureLayout l = Gxm_TextureLayout(format, width, height, mipLevels);
    unsigned end = 0;
    for (int m = 0; m < l.levels; m++) {
        const GxmLevelLayout &level = l.level[m];
        REQUIRE(level.offset == end);
        REQUIRE(level.rowBytes >= level.sourceRowBytes && level.rows >= level.sourceRows && level.rows >= 1);
        if (!l.swizzled)
            REQUIRE(level.rowBytes % 32 == 0);
        end += level.rowBytes * level.rows;
    }
    REQUIRE(end == l.bytes && l.bytes % 8 == 0);
    return 0;
}

// Every block of a swizzled level has its own index inside the level.
int CheckSwizzle(unsigned width, unsigned height)
{
    std::vector<bool> used(width * height, false);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            const unsigned i = Gxm_SwizzledBlock(x, y, width, height);
            REQUIRE(i < width * height && !used[i]);
            used[i] = true;
        }
    return 0;
}

} // namespace

int main()
{
    // A 256x256 BGRA8 texture with 9 levels: the levels under 8 texels wide
    // are padded to 8.
    GxmTextureLayout a = Gxm_TextureLayout(GFX_FORMAT_BGRA8, 256, 256, 9);
    REQUIRE(!a.swizzled && a.levels == 9);
    REQUIRE(a.level[0].rowBytes == 1024 && a.level[0].rows == 256);
    REQUIRE(a.level[6].rowBytes == 32 && a.level[6].rows == 4 && a.level[6].sourceRowBytes == 16);
    REQUIRE(a.level[8].rowBytes == 32 && a.level[8].rows == 1 && a.level[8].sourceRowBytes == 4);
    REQUIRE(a.level[1].offset == 262144);
    REQUIRE(a.bytes == 262144 + 65536 + 16384 + 4096 + 1024 + 256 + 128 + 64 + 32);

    // A width that is not a multiple of 8 (a port-made font texture).
    GxmTextureLayout b = Gxm_TextureLayout(GFX_FORMAT_BGRA8, 100, 30, 1);
    REQUIRE(b.level[0].rowBytes == 416 && b.level[0].sourceRowBytes == 400 && b.bytes == 416 * 30);

    // BC1: swizzled, 4x4 blocks of 8 bytes; a level of 4x4 texels and less is one block.
    GxmTextureLayout c = Gxm_TextureLayout(GFX_FORMAT_BC1, 64, 64, 7);
    REQUIRE(c.swizzled && c.blockBytes == 8);
    REQUIRE(c.level[0].rowBytes == 128 && c.level[0].rows == 16 && c.level[0].sourceRowBytes == 128);
    REQUIRE(c.level[4].rowBytes == 8 && c.level[4].rows == 1 && c.level[4].sourceRowBytes == 8);
    REQUIRE(c.level[6].rowBytes == 8 && c.level[6].rows == 1 && c.level[6].offset == c.bytes - 8);
    REQUIRE(c.bytes == 2048 + 512 + 128 + 32 + 8 + 8 + 8);
    // BC3: blocks of 16 bytes.
    GxmTextureLayout d = Gxm_TextureLayout(GFX_FORMAT_BC3, 128, 32, 1);
    REQUIRE(d.swizzled && d.level[0].rowBytes == 512 && d.level[0].rows == 8 && d.bytes == 4096);
    // A BC texture that is not a power of two takes the power-of-two size on the GPU.
    GxmTextureLayout e = Gxm_TextureLayout(GFX_FORMAT_BC1, 100, 30, 1);
    REQUIRE(e.level[0].rowBytes == 32 * 8 && e.level[0].rows == 8);
    REQUIRE(e.level[0].sourceRowBytes == 25 * 8 && e.level[0].sourceRows == 8);

    // The bump format: 3 bytes per texel in, 4 out.
    GxmTextureLayout f = Gxm_TextureLayout(GFX_FORMAT_BUMP_DUDVL, 16, 16, 1);
    REQUIRE(f.level[0].sourceRowBytes == 48 && f.level[0].rowBytes == 64);

    // Level counts are clamped to 1..16.
    REQUIRE(Gxm_TextureLayout(GFX_FORMAT_BGRX8, 8, 8, 0).levels == 1);

    // The block order: x in the odd bits, y in the even bits, in squares of
    // the smaller side, the squares along the longer side (vitaGL's order).
    REQUIRE(Gxm_SwizzledBlock(0, 1, 4, 4) == 1);
    REQUIRE(Gxm_SwizzledBlock(1, 0, 4, 4) == 2);
    REQUIRE(Gxm_SwizzledBlock(1, 1, 4, 4) == 3);
    REQUIRE(Gxm_SwizzledBlock(2, 0, 4, 4) == 8);
    REQUIRE(Gxm_SwizzledBlock(3, 3, 4, 4) == 15);
    REQUIRE(Gxm_SwizzledBlock(2, 0, 4, 2) == 4 && Gxm_SwizzledBlock(3, 1, 4, 2) == 7);
    REQUIRE(Gxm_SwizzledBlock(0, 2, 2, 4) == 4 && Gxm_SwizzledBlock(1, 3, 2, 4) == 7);
    REQUIRE(Gxm_SwizzledBlock(5, 0, 8, 1) == 5);
    for (unsigned w = 1; w <= 64; w *= 2)
        for (unsigned h = 1; h <= 64; h *= 2)
            if (CheckSwizzle(w, h))
                return 1;

    for (int format = GFX_FORMAT_BGRX8; format <= GFX_FORMAT_BUMP_DUDVL; format++)
        for (int w = 1; w <= 1024; w = w * 3 + 1)
            for (int h = 1; h <= 512; h = h * 2 + 3)
                if (CheckLayout(format, w, h, 10))
                    return 1;

    printf("gxm_texture_layout: ok\n");
    return 0;
}
