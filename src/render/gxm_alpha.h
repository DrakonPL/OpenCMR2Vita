// The least alpha of a texture level and of a vertex range. The GXM executor
// (gfx_gxm.cpp) leaves out the depth pass of a blended draw where no pixel can
// fail the alpha test: with a least texture alpha and a least vertex alpha, a
// lower bound of the draw's alpha (shader_key.h, Gfx_AlphaLowerBound) tells.
// No GXM: tests/gxm_alpha.cpp checks it.
#ifndef OPENCMR2_GXM_ALPHA_H
#define OPENCMR2_GXM_ALPHA_H

#include "port/gfx.h"

#include <stddef.h>
#include <stdint.h>

// A BC3 block: the two alpha end values, then sixteen 3-bit codes. With the
// first above the second, each code is a value between them. Otherwise codes
// 6 and 7 are 0 and 255.
inline int Gxm_MinAlphaBC3Block(const uint8_t *block)
{
    const int a0 = block[0], a1 = block[1];
    if (a0 > a1)
        return a1;
    uint64_t codes = 0;
    for (int i = 0; i < 6; i++)
        codes |= (uint64_t)block[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++)
        if ((codes >> (3 * i) & 7) == 6)
            return 0;
    return a0;
}

// A BC1 block: two colours, then sixteen 2-bit codes. With the first colour
// not above the second, code 3 is transparent black.
inline int Gxm_MinAlphaBC1Block(const uint8_t *block)
{
    const unsigned c0 = block[0] | block[1] << 8, c1 = block[2] | block[3] << 8;
    if (c0 > c1)
        return 255;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++)
            if ((block[4 + i] >> (2 * k) & 3) == 3)
                return 0;
    return 255;
}

// The least alpha (0..255) of a level in the CPU copy's layout
// (GfxTexture::levels). A filtered read gives no less. 0 for a format whose
// alpha is not known here.
inline int Gxm_MinAlpha(int format, const uint8_t *data, size_t bytes)
{
    int least = 255;
    switch (format) {
    case GFX_FORMAT_BGRX8:
        return 255;
    case GFX_FORMAT_BGRA8:
        for (size_t i = 3; i < bytes && least > 0; i += 4)
            least = data[i] < least ? data[i] : least;
        return least;
    case GFX_FORMAT_BC3:
        for (size_t i = 0; i + 16 <= bytes && least > 0; i += 16) {
            const int a = Gxm_MinAlphaBC3Block(data + i);
            least = a < least ? a : least;
        }
        return least;
    case GFX_FORMAT_BC1:
        for (size_t i = 0; i + 8 <= bytes && least > 0; i += 8) {
            const int a = Gxm_MinAlphaBC1Block(data + i);
            least = a < least ? a : least;
        }
        return least;
    default:
        return 0;
    }
}

// The least byte at offset in count vertices of stride bytes: the alpha of a
// vertex colour is the last byte of its little-endian ARGB word.
inline int Gxm_MinVertexByte(const void *vertices, size_t stride, unsigned count, size_t offset)
{
    const uint8_t *p = (const uint8_t *)vertices + offset;
    int least = 255;
    for (unsigned i = 0; i < count; i++, p += stride)
        least = *p < least ? *p : least;
    return least;
}

#endif
