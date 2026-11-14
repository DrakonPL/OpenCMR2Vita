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
