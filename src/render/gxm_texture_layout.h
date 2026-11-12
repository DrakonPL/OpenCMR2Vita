// Where the levels of a texture sit in GPU memory, as sceGxmTextureInitLinear
// reads them: each level's rows padded to 8 texels, the levels back to back
// (the official mip_generation sample and vitaGL agree; Andromeda proved it
// on the device). With padded rows any width works, mipmaps too. For the
// block formats a row is a row of 4x4 blocks. No GXM here:
// tests/gxm_texture_layout.cpp checks it.
#ifndef OPENCMR2_GXM_TEXTURE_LAYOUT_H
#define OPENCMR2_GXM_TEXTURE_LAYOUT_H

#include "port/gfx.h"

struct GxmLevelLayout {
    unsigned offset;        // from the texture's base
    unsigned rowBytes;      // on the GPU
    unsigned rows;          // texel rows, or block rows
    unsigned sourceRowBytes;    // in the CPU copy (GfxTexture::levels)
};

struct GxmTextureLayout {
    int levels = 0;
    GxmLevelLayout level[16];
    unsigned bytes = 0;     // all levels
};

inline GxmTextureLayout Gxm_LinearLayout(int format, int width, int height, int mipLevels)
{
    constexpr unsigned kRowTexels = 8;  // SCE_GXM_TEXTURE_IMPLICIT_STRIDE_ALIGNMENT
    const bool blocks = format == GFX_FORMAT_BC1 || format == GFX_FORMAT_BC3;
    const unsigned blockBytes = format == GFX_FORMAT_BC1 ? 8 : 16;
    const unsigned sourceTexelBytes = format == GFX_FORMAT_BUMP_DUDVL ? 3 : 4;
    GxmTextureLayout layout;
    layout.levels = mipLevels < 1 ? 1 : mipLevels > 16 ? 16 : mipLevels;
    for (int m = 0; m < layout.levels; m++) {
        const unsigned w = width >> m > 0 ? (unsigned)width >> m : 1u;
        const unsigned h = height >> m > 0 ? (unsigned)height >> m : 1u;
        const unsigned padded = (w + kRowTexels - 1) / kRowTexels * kRowTexels;
        GxmLevelLayout &l = layout.level[m];
        l.offset = layout.bytes;
        if (blocks) {
            l.rowBytes = padded / 4 * blockBytes;
            l.rows = (h + 3) / 4;
            l.sourceRowBytes = (w + 3) / 4 * blockBytes;
        } else {
            l.rowBytes = padded * 4;
            l.rows = h;
            l.sourceRowBytes = w * sourceTexelBytes;
        }
        layout.bytes += l.rowBytes * l.rows;
    }
    return layout;
}

#endif
