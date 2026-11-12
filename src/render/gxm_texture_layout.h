// Where the levels of a texture sit in GPU memory. No GXM here:
// tests/gxm_texture_layout.cpp checks it.
//
// 32-bit formats are linear, as sceGxmTextureInitLinear reads them: each
// level's rows padded to 8 texels, the levels back to back (the official
// mip_generation sample and vitaGL agree; Andromeda proved it on the device).
// With padded rows any width works, mipmaps too.
//
// BC1 and BC3 are swizzled: GXM refuses them linear (SCE_GXM_ERROR_UNSUPPORTED
// on the device). A level is its power-of-two size in 4x4 blocks, each block
// at its Morton index (Gxm_SwizzledBlock), the levels back to back, as vitaGL
// uploads them.
//
// Cube maps (Gxm_CubeLayout) are 32-bit, square, a power of two and one
// level: the render targets of the car reflections. Each face is swizzled
// texel by texel, the six faces back to back (+X, -X, +Y, -Y, +Z, -Z).
#ifndef OPENCMR2_GXM_TEXTURE_LAYOUT_H
#define OPENCMR2_GXM_TEXTURE_LAYOUT_H

#include "port/gfx.h"

struct GxmLevelLayout {
    unsigned offset;            // from the texture's base
    unsigned rowBytes;          // on the GPU: a row of texels, or of blocks
    unsigned rows;              // on the GPU: texel rows, or block rows
    unsigned sourceRowBytes;    // in the CPU copy (GfxTexture::levels)
    unsigned sourceRows;
};

struct GxmTextureLayout {
    bool swizzled = false;      // block formats: rowBytes / blockBytes blocks a row
    unsigned blockBytes = 0;
    int levels = 0;
    GxmLevelLayout level[16];
    unsigned bytes = 0;         // all levels (of all faces)
    unsigned faceBytes = 0;     // a cube: from one face to the next (0: not a cube)
};

// x in the odd bits, y in the even bits.
inline unsigned Gxm_Interleave(unsigned v)
{
    v &= 0x0000ffff;
    v = (v | (v << 8)) & 0x00ff00ff;
    v = (v | (v << 4)) & 0x0f0f0f0f;
    v = (v | (v << 2)) & 0x33333333;
    v = (v | (v << 1)) & 0x55555555;
    return v;
}

// The index of block (x, y) in a swizzled level of width x height blocks
// (powers of two): Morton order in squares of the smaller side, the squares
// one after another along the longer side.
inline unsigned Gxm_SwizzledBlock(unsigned x, unsigned y, unsigned width, unsigned height)
{
    const unsigned side = width < height ? width : height;
    const unsigned square = width < height ? y / side : x / side;
    return square * side * side + (Gxm_Interleave(x % side) << 1 | Gxm_Interleave(y % side));
}

inline unsigned Gxm_PowerOfTwo(unsigned v)
{
    unsigned p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

inline GxmTextureLayout Gxm_TextureLayout(int format, int width, int height, int mipLevels)
{
    constexpr unsigned kRowTexels = 8;  // SCE_GXM_TEXTURE_IMPLICIT_STRIDE_ALIGNMENT
    GxmTextureLayout layout;
    layout.swizzled = format == GFX_FORMAT_BC1 || format == GFX_FORMAT_BC3;
    layout.blockBytes = format == GFX_FORMAT_BC1 ? 8 : 16;
    const unsigned sourceTexelBytes = format == GFX_FORMAT_BUMP_DUDVL ? 3 : 4;
    layout.levels = mipLevels < 1 ? 1 : mipLevels > 16 ? 16 : mipLevels;
    for (int m = 0; m < layout.levels; m++) {
        const unsigned w = width >> m > 0 ? (unsigned)width >> m : 1u;
        const unsigned h = height >> m > 0 ? (unsigned)height >> m : 1u;
        GxmLevelLayout &l = layout.level[m];
        l.offset = layout.bytes;
        if (layout.swizzled) {
            l.rowBytes = (Gxm_PowerOfTwo(w) + 3) / 4 * layout.blockBytes;
            l.rows = (Gxm_PowerOfTwo(h) + 3) / 4;
            l.sourceRowBytes = (w + 3) / 4 * layout.blockBytes;
            l.sourceRows = (h + 3) / 4;
        } else {
            l.rowBytes = (w + kRowTexels - 1) / kRowTexels * kRowTexels * 4;
            l.rows = h;
            l.sourceRowBytes = w * sourceTexelBytes;
            l.sourceRows = h;
        }
        layout.bytes += l.rowBytes * l.rows;
    }
    return layout;
}

// The smallest cube side Gxm_CubeLayout takes: from 32 texels a face is a
// multiple of 4 KB, so the faces need no padding between them.
constexpr int kGxmMinCubeSize = 32;

// A cube of size x size texels: false when GXM cannot take it as laid out
// here (not a power of two, below kGxmMinCubeSize, or more than one level).
inline bool Gxm_CubeLayout(int size, int mipLevels, GxmTextureLayout *layout)
{
    if (size < kGxmMinCubeSize || Gxm_PowerOfTwo((unsigned)size) != (unsigned)size || mipLevels > 1)
        return false;
    *layout = GxmTextureLayout();
    layout->swizzled = true;
    layout->blockBytes = 4;     // a texel
    layout->levels = 1;
    GxmLevelLayout &l = layout->level[0];
    l.offset = 0;
    l.rowBytes = l.sourceRowBytes = (unsigned)size * 4;
    l.rows = l.sourceRows = (unsigned)size;
    layout->faceBytes = l.rowBytes * l.rows;
    layout->bytes = 6 * layout->faceBytes;
    return true;
}

#endif
