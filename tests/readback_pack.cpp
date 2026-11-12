// The back buffer readback of the GXM executor (src/render/gxm_readback.h):
// the clamp of the rectangle the game asks for, and the row of display
// pixels as the 0xAARRGGBB words Gfx_ReadBackBuffer gives. No GXM.

#include "render/gxm_readback.h"

#include <cstdio>
#include <cstring>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

bool Same(const GxmReadbackRect &r, int x, int y, int width, int height)
{
    return r.x == x && r.y == y && r.width == width && r.height == height;
}

} // namespace

int main()
{
    // Inside: unchanged.
    REQUIRE(Same(Gxm_ClampReadback(100, 50, 16, 8, 960, 544), 100, 50, 16, 8));
    // Over the left and top edges: the part inside.
    REQUIRE(Same(Gxm_ClampReadback(-4, -2, 16, 8, 960, 544), 0, 0, 12, 6));
    // Over the right and bottom edges.
    REQUIRE(Same(Gxm_ClampReadback(950, 540, 16, 8, 960, 544), 950, 540, 10, 4));
    // Outside, or empty: nothing.
    REQUIRE(Gxm_ClampReadback(960, 0, 16, 8, 960, 544).width == 0);
    REQUIRE(Gxm_ClampReadback(0, -8, 16, 8, 960, 544).height == 0);
    REQUIRE(Gxm_ClampReadback(10, 10, 0, 8, 960, 544).width == 0);

    // A display pixel is the bytes R, G, B, A. The game reads 0xAARRGGBB.
    const unsigned char bytes[8] = { 0x11, 0x22, 0x33, 0x44, 0xff, 0x00, 0x80, 0x01 };
    uint32_t row[2], out[2];
    memcpy(row, bytes, sizeof(row));
    Gxm_PackReadbackRow(row, out, 2);
    REQUIRE(out[0] == 0x44112233u);
    REQUIRE(out[1] == 0x01ff0080u);

    printf("readback_pack: ok\n");
    return 0;
}
