// The least alpha of a texture level and of a vertex range
// (src/render/gxm_alpha.h), on known blocks, pixels and vertices. No GXM.

#include "render/gxm_alpha.h"

#include <cstdio>
#include <cstring>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// A BC3 block with the alpha end values and one code for all sixteen texels.
void Bc3Block(uint8_t *block, int a0, int a1, int code)
{
    memset(block, 0, 16);
    block[0] = (uint8_t)a0;
    block[1] = (uint8_t)a1;
    uint64_t codes = 0;
    for (int i = 0; i < 16; i++)
        codes |= (uint64_t)code << (3 * i);
    for (int i = 0; i < 6; i++)
        block[2 + i] = (uint8_t)(codes >> (8 * i));
}

} // namespace

int main()
{
    uint8_t block[16];
    // The first end value above the second: each code is between them.
    Bc3Block(block, 200, 40, 0);
    REQUIRE(Gxm_MinAlphaBC3Block(block) == 40);
    // Not above: codes 6 and 7 are 0 and 255. An opaque block has neither 6.
    Bc3Block(block, 255, 255, 0);
    REQUIRE(Gxm_MinAlphaBC3Block(block) == 255);
    Bc3Block(block, 90, 180, 7);
    REQUIRE(Gxm_MinAlphaBC3Block(block) == 90);
    Bc3Block(block, 255, 255, 6);
    REQUIRE(Gxm_MinAlphaBC3Block(block) == 0);
    // One texel with code 6, the last: still found.
    Bc3Block(block, 255, 255, 0);
    block[7] = (uint8_t)(6 << 5);
    REQUIRE(Gxm_MinAlphaBC3Block(block) == 0);

    // A level of BC3 blocks: the least of them.
    uint8_t level[48];
    Bc3Block(level, 255, 255, 0);
    Bc3Block(level + 16, 250, 120, 1);
    Bc3Block(level + 32, 255, 255, 0);
    REQUIRE(Gxm_MinAlpha(GFX_FORMAT_BC3, level, sizeof(level)) == 120);
    REQUIRE(Gxm_MinAlpha(GFX_FORMAT_BC3, level, 16) == 255);

    // BC1: transparent only with the first colour not above the second and a
    // texel with code 3.
    uint8_t bc1[8] = { 0x00, 0x80, 0x00, 0x40, 0xff, 0xff, 0xff, 0xff };
    REQUIRE(Gxm_MinAlpha(GFX_FORMAT_BC1, bc1, 8) == 255);
    bc1[1] = 0x40;
    bc1[3] = 0x80;
    REQUIRE(Gxm_MinAlpha(GFX_FORMAT_BC1, bc1, 8) == 0);
    memset(bc1 + 4, 0xaa, 4);   // code 2 everywhere
    REQUIRE(Gxm_MinAlpha(GFX_FORMAT_BC1, bc1, 8) == 255);

    // 32-bit pixels: the alpha is the fourth byte. BGRX has none.
    const uint8_t pixels[12] = { 1, 2, 3, 200, 4, 5, 6, 17, 7, 8, 9, 255 };
    REQUIRE(Gxm_MinAlpha(GFX_FORMAT_BGRA8, pixels, sizeof(pixels)) == 17);
    REQUIRE(Gxm_MinAlpha(GFX_FORMAT_BGRX8, pixels, sizeof(pixels)) == 255);
    // A format the scan does not know: no bound.
    REQUIRE(Gxm_MinAlpha(GFX_FORMAT_BUMP_DUDVL, pixels, sizeof(pixels)) == 0);

    // Vertices: the least byte at an offset, by the stride.
    const uint8_t vertices[3][8] = { { 0, 0, 0, 0, 9, 9, 9, 240 }, { 0, 0, 0, 0, 9, 9, 9, 64 }, { 0, 0, 0, 0, 9, 9, 9, 255 } };
    REQUIRE(Gxm_MinVertexByte(vertices, 8, 3, 7) == 64);
    REQUIRE(Gxm_MinVertexByte(vertices, 8, 1, 7) == 240);
    REQUIRE(Gxm_MinVertexByte(vertices, 8, 3, 4) == 9);

    printf("gxm_alpha: ok\n");
    return 0;
}
