// The parts of the billboard atlas (src/render/texture_atlas.h): the packer
// places textures without overlap, and a tile's border blocks are the
// mirrors of its edge blocks. No renderer.

#include "render/texture_atlas.h"

#include <cstdio>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// The 3-bit alpha index and the 2-bit colour index of a block's texel.
int AlphaIndex(const uint8_t *block, int x, int y)
{
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++)
        bits |= (uint64_t)block[2 + i] << (8 * i);
    return (int)(bits >> (3 * (y * 4 + x)) & 7);
}

int ColorIndex(const uint8_t *block, int x, int y)
{
    return block[12 + y] >> (2 * x) & 3;
}

// A block whose bytes all come from a seed: its indices differ from texel to
// texel and from block to block.
void Fill(uint8_t *block, unsigned seed)
{
    for (int i = 0; i < kAtlasBlockBytes; i++) {
        seed = seed * 1664525u + 1013904223u;
        block[i] = (uint8_t)(seed >> 24);
    }
}

bool SameTexels(const uint8_t *a, const uint8_t *b, bool leftRight, bool topBottom)
{
    for (int i = 0; i < 2; i++)
        if (a[i] != b[i])
            return false;
    for (int i = 8; i < 12; i++)
        if (a[i] != b[i])
            return false;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            const int sx = leftRight ? 3 - x : x, sy = topBottom ? 3 - y : y;
            if (AlphaIndex(a, x, y) != AlphaIndex(b, sx, sy) || ColorIndex(a, x, y) != ColorIndex(b, sx, sy))
                return false;
        }
    return true;
}

} // namespace

int main()
{
    // The packer: rows from the top left, no overlap, a new row where the
    // width is used up, and no room below the last row.
    AtlasPacker packer;
    packer.Reset(10, 6);
    int x, y;
    REQUIRE(packer.Place(4, 3, &x, &y) && x == 0 && y == 0);
    REQUIRE(packer.Place(4, 2, &x, &y) && x == 4 && y == 0);
    REQUIRE(packer.Place(4, 3, &x, &y) && x == 0 && y == 3);    // 8 + 4 > 10: the next row, below the tallest
    REQUIRE(packer.Place(6, 3, &x, &y) && x == 4 && y == 3);
    REQUIRE(!packer.Place(4, 1, &x, &y));                      // a third row would start at 6
    REQUIRE(!packer.Place(11, 1, &x, &y) && !packer.Place(0, 1, &x, &y));
    packer.Reset(10, 6);
    REQUIRE(packer.Place(10, 6, &x, &y) && x == 0 && y == 0);

    // A mirror moves each texel's two indices and keeps the block's alpha
    // values and colours. Two mirrors give the block again.
    uint8_t block[kAtlasBlockBytes], mirror[kAtlasBlockBytes], back[kAtlasBlockBytes];
    Fill(block, 7);
    Atlas_MirrorBC3(block, true, false, mirror);
    REQUIRE(SameTexels(mirror, block, true, false) && !SameTexels(mirror, block, false, false));
    Atlas_MirrorBC3(mirror, true, false, back);
    REQUIRE(memcmp(back, block, sizeof(block)) == 0);
    Atlas_MirrorBC3(block, false, true, mirror);
    REQUIRE(SameTexels(mirror, block, false, true) && !SameTexels(mirror, block, true, true));
    Atlas_MirrorBC3(block, true, true, mirror);
    REQUIRE(SameTexels(mirror, block, true, true));
    Atlas_MirrorBC3(block, false, false, mirror);
    REQUIRE(memcmp(mirror, block, sizeof(block)) == 0);

    // A tile of 3 x 2 blocks at block (2, 1) of an atlas 8 blocks wide: its
    // own blocks start at (3, 2), and a border of mirrors is around them.
    const int w = 3, h = 2, atlasWidth = 8, atlasHeight = 6;
    std::vector<uint8_t> source((size_t)w * h * kAtlasBlockBytes);
    for (int i = 0; i < w * h; i++)
        Fill(&source[(size_t)i * kAtlasBlockBytes], 100 + i);
    std::vector<uint8_t> atlas((size_t)atlasWidth * atlasHeight * kAtlasBlockBytes, 0xee);
    Atlas_CopyBC3Tile(source.data(), w, h, atlas.data(), atlasWidth, 2, 1);
    auto at = [&](int bx, int by) { return &atlas[((size_t)by * atlasWidth + bx) * kAtlasBlockBytes]; };
    auto src = [&](int bx, int by) { return &source[((size_t)by * w + bx) * kAtlasBlockBytes]; };
    for (int by = 0; by < h; by++)
        for (int bx = 0; bx < w; bx++)
            REQUIRE(memcmp(at(3 + bx, 2 + by), src(bx, by), kAtlasBlockBytes) == 0);
    // The left and the right border: the edge blocks, mirrored left to right.
    for (int by = 0; by < h; by++) {
        REQUIRE(SameTexels(at(2, 2 + by), src(0, by), true, false));
        REQUIRE(SameTexels(at(3 + w, 2 + by), src(w - 1, by), true, false));
    }
    // The top and the bottom border, and the corners.
    for (int bx = 0; bx < w; bx++) {
        REQUIRE(SameTexels(at(3 + bx, 1), src(bx, 0), false, true));
        REQUIRE(SameTexels(at(3 + bx, 2 + h), src(bx, h - 1), false, true));
    }
    REQUIRE(SameTexels(at(2, 1), src(0, 0), true, true) && SameTexels(at(3 + w, 2 + h), src(w - 1, h - 1), true, true));
    // The texel next to the tile's left edge is the edge texel again: what a
    // clamped texture gives a linear filter there.
    REQUIRE(AlphaIndex(at(2, 2), 3, 1) == AlphaIndex(at(3, 2), 0, 1));
    REQUIRE(ColorIndex(at(3, 1), 2, 3) == ColorIndex(at(3, 2), 2, 0));
    // Nothing else of the atlas changed.
    for (int by = 0; by < atlasHeight; by++)
        for (int bx = 0; bx < atlasWidth; bx++)
            if (bx < 2 || bx > 3 + w || by < 1 || by > 2 + h)
                REQUIRE(at(bx, by)[0] == 0xee && at(bx, by)[15] == 0xee);

    printf("texture_atlas: ok\n");
    return 0;
}
