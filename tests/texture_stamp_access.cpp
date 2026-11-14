// The renderer's texture flag for tests/texture_stamp.cpp, which sees the
// game's headers and not the renderer's.

#include "render/gfx_internal.h"

bool Test_TextureDirty(GfxTexture *t)
{
    return t->dirty;
}

void Test_ClearDirty(GfxTexture *t)
{
    t->dirty = false;
}

void Test_ChangedRect(GfxTexture *t, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = t->changedRect.x0;
    *y0 = t->changedRect.y0;
    *x1 = t->changedRect.x1;
    *y1 = t->changedRect.y1;
}

bool Test_ChangedTexel(GfxTexture *t, int x, int y)
{
    return t->changedTiles.Has(x / GfxTileSet::kTile, y / GfxTileSet::kTile);
}
