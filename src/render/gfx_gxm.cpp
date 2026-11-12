// GXM executor (PS Vita). It draws nothing yet: it is the null executor.

#include "render/gfx_internal.h"

GfxExecutor *Gfx_CreateGxmExecutor(void)
{
    return Gfx_CreateNullExecutor();
}
