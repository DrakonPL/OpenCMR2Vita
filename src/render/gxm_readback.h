// The back buffer readback of the GXM executor (gfx_gxm.cpp) as pure
// functions: the rectangle the game asks for, clamped to the back buffer,
// and a row of the display buffer as the pixels Gfx_ReadBackBuffer gives.
// No GXM: tests/readback_pack.cpp checks it on the PC.
#ifndef OPENCMR2_GXM_READBACK_H
#define OPENCMR2_GXM_READBACK_H

#include <stdint.h>

struct GxmReadbackRect {
    int x = 0, y = 0, width = 0, height = 0;
};

// The part of (x, y, width, height) inside a back buffer of backWidth x
// backHeight. Empty (width or height 0) when nothing of it is inside.
inline GxmReadbackRect Gxm_ClampReadback(int x, int y, int width, int height, int backWidth, int backHeight)
{
    GxmReadbackRect r;
    const int left = x < 0 ? 0 : x, top = y < 0 ? 0 : y;
    const int right = x + width > backWidth ? backWidth : x + width;
    const int bottom = y + height > backHeight ? backHeight : y + height;
    if (right <= left || bottom <= top)
        return r;
    r.x = left;
    r.y = top;
    r.width = right - left;
    r.height = bottom - top;
    return r;
}

// A row of the display buffer (SCE_GXM_COLOR_FORMAT_A8B8G8R8: the bytes R, G,
// B, A, a little-endian word 0xAABBGGRR) as 0xAARRGGBB words.
inline void Gxm_PackReadbackRow(const uint32_t *src, uint32_t *dst, int count)
{
    for (int i = 0; i < count; i++) {
        const uint32_t p = src[i];
        dst[i] = (p & 0xff00ff00u) | (p & 0xffu) << 16 | (p >> 16 & 0xffu);
    }
}

#endif
