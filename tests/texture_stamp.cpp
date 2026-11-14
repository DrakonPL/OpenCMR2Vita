// The dirt stamps of the game (CGraphics::LockTexture, BlendPixel,
// UnlockTexture, game/Graphics.cpp): a texture of 32-bit pixels gets the
// stamp, and the renderer gets the part that changed. A compressed texture
// gets no pixel, and stays as it is on the GPU: no new upload in each frame
// of a wet stage.

#include "port/types.h"
#include "Graphics.h"
#include "Texture.h"
#include "port/gfx.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

// The renderer's texture (src/render/gfx_internal.h): its CPU copy and its
// flag for the GPU, as the executors read them.
struct GfxTexture;
bool Test_TextureDirty(GfxTexture *t);
void Test_ClearDirty(GfxTexture *t);
void Test_ChangedRect(GfxTexture *t, int *x0, int *y0, int *x1, int *y1);

namespace {

// Stamps the pixel x, y of a lock of the rectangle from left, top (as
// StageObject_StampPendingEventDraws locks a quarter of the texture).
int Stamp(Texture &texture, int x = 3, int y = 2, int left = 0, int top = 0)
{
    BYTE colour[4] = { 0x2f, 0x27, 0x14, 0xff };
    RECT rect = { left, top, texture.width, texture.height };
    CGraphics::LockTexture(&texture, left != 0 || top != 0 ? &rect : NULL);
    CGraphics::BlendPixel(&texture, (unsigned)x, (unsigned)y, colour);
    CGraphics::UnlockTexture(&texture);
    return 0;
}

bool ChangedIs(GfxTexture *t, int x0, int y0, int x1, int y1)
{
    int a, b, c, d;
    Test_ChangedRect(t, &a, &b, &c, &d);
    return a == x0 && b == y0 && c == x1 && d == y1;
}

} // namespace

int main()
{
    // A texture of 32-bit pixels: the stamp is in its CPU copy, and it goes
    // to the GPU again.
    Texture plain;
    memset(&plain, 0, sizeof(plain));
    plain.width = plain.height = 16;
    plain.pSurface = Gfx_CreateTexture(16, 16, GFX_FORMAT_BGRA8, 1, 0);
    REQUIRE(plain.pSurface != NULL);
    Test_ClearDirty(plain.pSurface);
    Stamp(plain);
    GfxLockedRect rect;
    REQUIRE(Gfx_LockTexture(plain.pSurface, 0, 0, &rect));
    DWORD pixel;
    memcpy(&pixel, (const BYTE *)rect.pixels + 2 * rect.pitch + 3 * 4, 4);
    Gfx_UnlockTexture(plain.pSurface, 0, 0);
    REQUIRE(pixel == GFX_RGBA(0x2f, 0x27, 0x14, 0xff));
    Test_ClearDirty(plain.pSurface);
    Stamp(plain);
    REQUIRE(Test_TextureDirty(plain.pSurface) && ChangedIs(plain.pSurface, 3, 2, 4, 3));
    // The part of a lock of a rectangle is in the texture's pixels, and two
    // stamps before the upload give the part with both.
    Stamp(plain, 1, 4, 8, 8);
    REQUIRE(ChangedIs(plain.pSurface, 3, 2, 10, 13));
    REQUIRE(Gfx_LockTexture(plain.pSurface, 0, 0, &rect));
    memcpy(&pixel, (const BYTE *)rect.pixels + 12 * rect.pitch + 9 * 4, 4);
    REQUIRE(pixel == GFX_RGBA(0x2f, 0x27, 0x14, 0xff));
    // A lock of all of the texture (here the renderer's): all of it changed.
    Gfx_UnlockTexture(plain.pSurface, 0, 0);
    REQUIRE(ChangedIs(plain.pSurface, 0, 0, 0, 0));
    Stamp(plain);
    REQUIRE(ChangedIs(plain.pSurface, 0, 0, 0, 0));

    // A BC3 texture: no pixel changes, and it does not go to the GPU again.
    Texture compressed;
    memset(&compressed, 0, sizeof(compressed));
    compressed.width = compressed.height = 16;
    compressed.pSurface = Gfx_CreateTexture(16, 16, GFX_FORMAT_BC3, 1, 0);
    REQUIRE(compressed.pSurface != NULL);
    REQUIRE(Gfx_LockTexture(compressed.pSurface, 0, 0, &rect));
    std::vector<BYTE> before((const BYTE *)rect.pixels, (const BYTE *)rect.pixels + 4 * 4 * 16);
    Gfx_UnlockTexture(compressed.pSurface, 0, 0);
    Test_ClearDirty(compressed.pSurface);
    Stamp(compressed);
    REQUIRE(!Test_TextureDirty(compressed.pSurface));
    REQUIRE(Gfx_LockTexture(compressed.pSurface, 0, 0, &rect));
    REQUIRE(memcmp(before.data(), rect.pixels, before.size()) == 0);
    Gfx_UnlockTexture(compressed.pSurface, 0, 0);
    // The lock table is free again: a later lock of another texture works.
    Test_ClearDirty(plain.pSurface);
    Stamp(plain);
    REQUIRE(Test_TextureDirty(plain.pSurface));

    Gfx_DestroyTexture(plain.pSurface);
    Gfx_DestroyTexture(compressed.pSurface);
    printf("texture_stamp: ok\n");
    return 0;
}
