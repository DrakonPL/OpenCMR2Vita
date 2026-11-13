// Parts of the atlas that the recorder builds from the textures of small
// blended quads (gfx_core.cpp: the trackside trees are about 300 draws of two
// triangles in a frame, with 17 textures in a stage). With all of them in one
// texture, the quads keep their order from far to near and still are one
// draw. Here: where a texture goes in the atlas, and the copy of its BC3
// blocks with a border. No renderer here: tests/texture_atlas.cpp.
#ifndef OPENCMR2_TEXTURE_ATLAS_H
#define OPENCMR2_TEXTURE_ATLAS_H

#include <stdint.h>
#include <string.h>

// A BC3 block: 4x4 texels in 16 bytes. Two alpha values and 16 indices of 3
// bits, then two colours and 16 indices of 2 bits, both in rows from the top.
constexpr int kAtlasBlockBytes = 16;

// Places rectangles, measured in blocks, in rows from the top left.
struct AtlasPacker {
    int width = 0, height = 0;      // of the atlas, in blocks
    int x = 0, y = 0, rowHeight = 0;
    void Reset(int widthBlocks, int heightBlocks)
    {
        width = widthBlocks;
        height = heightBlocks;
        x = y = rowHeight = 0;
    }
    // False where the atlas has no room.
    bool Place(int w, int h, int *outX, int *outY)
    {
        if (w <= 0 || h <= 0 || w > width)
            return false;
        if (x + w > width) {
            x = 0;
            y += rowHeight;
            rowHeight = 0;
        }
        if (y + h > height)
            return false;
        *outX = x;
        *outY = y;
        x += w;
        rowHeight = h > rowHeight ? h : rowHeight;
        return true;
    }
};

// The block with its texels mirrored left to right, top to bottom, or both.
inline void Atlas_MirrorBC3(const uint8_t *in, bool leftRight, bool topBottom, uint8_t *out)
{
    // The alpha indices: 48 bits, 3 for each texel.
    uint64_t alpha = 0;
    for (int i = 0; i < 6; i++)
        alpha |= (uint64_t)in[2 + i] << (8 * i);
    uint64_t mirrored = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            const int sx = leftRight ? 3 - x : x, sy = topBottom ? 3 - y : y;
            mirrored |= (alpha >> (3 * (sy * 4 + sx)) & 7) << (3 * (y * 4 + x));
        }
    out[0] = in[0];
    out[1] = in[1];
    for (int i = 0; i < 6; i++)
        out[2 + i] = (uint8_t)(mirrored >> (8 * i));
    // The colours, and the colour indices: a byte for each row, 2 bits for
    // each texel.
    memcpy(out + 8, in + 8, 4);
    for (int y = 0; y < 4; y++) {
        const uint8_t row = in[12 + (topBottom ? 3 - y : y)];
        out[12 + y] = leftRight ? (uint8_t)((row & 0x03) << 6 | (row & 0x0c) << 2 | (row & 0x30) >> 2 | (row & 0xc0) >> 6)
                                : row;
    }
}

// Copies a texture of w x h blocks into the atlas (atlasWidth blocks wide)
// with a border of one block on each side: the texture's own blocks start at
// block (x + 1, y + 1). A border block is the mirror of the block next to
// it, so the texel beside an edge is the edge texel again. That is what a
// clamped texture gives a linear filter at its edge: a quad drawn from the
// atlas gets the pixels it got from the texture.
inline void Atlas_CopyBC3Tile(const uint8_t *source, int w, int h, uint8_t *atlas, int atlasWidth, int x, int y)
{
    for (int by = -1; by <= h; by++)
        for (int bx = -1; bx <= w; bx++) {
            const int sx = bx < 0 ? 0 : bx >= w ? w - 1 : bx;
            const int sy = by < 0 ? 0 : by >= h ? h - 1 : by;
            const uint8_t *in = source + ((size_t)sy * w + sx) * kAtlasBlockBytes;
            uint8_t *out = atlas + ((size_t)(y + 1 + by) * atlasWidth + (x + 1 + bx)) * kAtlasBlockBytes;
            if (sx == bx && sy == by)
                memcpy(out, in, kAtlasBlockBytes);
            else
                Atlas_MirrorBC3(in, sx != bx, sy != by, out);
        }
}

#endif
