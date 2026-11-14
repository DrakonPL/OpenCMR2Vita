// The mip levels of a BC3 texture that the game made with its first level
// only (the scenery, the ground, the trees). The PS Vita reads a texture
// drawn small from such a level, and its texture cache misses less
// (gxm_textures.cpp). Each level is a 2x2 box of the level before it, encoded
// again by a range fit (fast at the load of a stage). No GXM here:
// tests/gfx_record.cpp checks it on the PC.
#ifndef OPENCMR2_TEXTURE_MIPS_H
#define OPENCMR2_TEXTURE_MIPS_H

#include <stdint.h>

#include <vector>

// A part of level 0, in texels; x1 and y1 are past its end.
struct GfxMipRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Empty() const { return x1 <= x0 || y1 <= y0; }
};

// The smallest rectangle with both (an empty one adds nothing).
inline GfxMipRect Gfx_MipRectUnion(GfxMipRect a, GfxMipRect b)
{
    if (a.Empty())
        return b;
    if (b.Empty())
        return a;
    GfxMipRect r;
    r.x0 = a.x0 < b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 < b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
    return r;
}

// The levels of a texture of width x height down to 1 x 1, at most limit
// (0: no limit).
int Gfx_MipLevelCount(int width, int height, int limit);

// The part of level 0 whose levels Gfx_MakeBC3Mips makes for a changed part:
// out to whole blocks of the last level, and inside the texture. An empty
// rectangle: all of the texture.
GfxMipRect Gfx_MipPartOf(GfxMipRect changed, int width, int height, int count);

// The blocks of a level (0: the texture) that have texels of a part of level
// 0 (as Gfx_MipPartOf gives it): x0, y0 the first block, x1, y1 past the
// last. An executor that has the levels on the GPU writes these blocks for
// a changed part, and no other block changed.
GfxMipRect Gfx_MipPartBlocks(GfxMipRect part, int width, int height, int level);

// Levels 1 .. count - 1 of a BC3 texture into mips[1 .. count - 1], from
// level0 (rows of 4x4 blocks, as GfxTexture::levels). changed: the part of
// level 0 that changed since mips were made. The blocks of mips outside its
// levels' part stay; a mips level of the wrong size is made whole. An empty
// rectangle makes all of it.
void Gfx_MakeBC3Mips(const uint8_t *level0, int width, int height, int count, GfxMipRect changed,
                     std::vector<uint8_t> mips[]);

#endif
