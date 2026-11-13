// The mip levels of a BC3 texture that the game made with its first level
// only (the scenery, the ground, the trees). The PS Vita reads a texture
// drawn small from such a level, and its texture cache misses less
// (gxm_textures.cpp). Each level is a 2x2 box of the level before it, encoded
// again (stb_dxt). No GXM here: tests/gfx_record.cpp checks it on the PC.
#ifndef OPENCMR2_TEXTURE_MIPS_H
#define OPENCMR2_TEXTURE_MIPS_H

#include <stdint.h>

#include <vector>

// A part of level 0, in texels; x1 and y1 are past its end.
struct GfxMipRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Empty() const { return x1 <= x0 || y1 <= y0; }
};

// The levels of a texture of width x height down to 1 x 1, at most limit
// (0: no limit).
int Gfx_MipLevelCount(int width, int height, int limit);

// Levels 1 .. count - 1 of a BC3 texture into mips[1 .. count - 1], from
// level0 (rows of 4x4 blocks, as GfxTexture::levels). changed: the part of
// level 0 that changed since mips were made. The blocks of mips outside its
// levels' part stay; a mips level of the wrong size is made whole. An empty
// rectangle makes all of it.
void Gfx_MakeBC3Mips(const uint8_t *level0, int width, int height, int count, GfxMipRect changed,
                     std::vector<uint8_t> mips[]);

#endif
