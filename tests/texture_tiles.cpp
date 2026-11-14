// The set of changed tiles of a texture (render/texture_tiles.h): the tiles
// that a part touches, the runs of tiles that an executor writes, and the
// union of two sets.

#include "render/texture_tiles.h"

#include <cstdio>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

GfxMipRect Rect(int x0, int y0, int x1, int y1)
{
    GfxMipRect r;
    r.x0 = x0;
    r.y0 = y0;
    r.x1 = x1;
    r.y1 = y1;
    return r;
}

std::vector<GfxMipRect> Runs(const GfxTileSet &set)
{
    std::vector<GfxMipRect> runs;
    set.ForEachRun([&](GfxMipRect r) { runs.push_back(r); });
    return runs;
}

bool Is(GfxMipRect r, int x0, int y0, int x1, int y1)
{
    return r.x0 == x0 && r.y0 == y0 && r.x1 == x1 && r.y1 == y1;
}

} // namespace

int main()
{
    // A texture of 300 x 100 texels: 38 x 13 tiles, rows that do not start
    // a word of the bits.
    const int w = 300, h = 100;
    GfxTileSet set;
    REQUIRE(set.Empty() && Runs(set).empty());

    // A stamp of 16 x 16 texels that is not on the tiles: 3 x 3 tiles, one
    // run in each row of tiles.
    set.Add(Rect(4, 12, 20, 28), w, h);
    REQUIRE(!set.Empty() && set.tilesX == 38 && set.tilesY == 13);
    std::vector<GfxMipRect> runs = Runs(set);
    REQUIRE(runs.size() == 3);
    REQUIRE(Is(runs[0], 0, 8, 24, 16) && Is(runs[1], 0, 16, 24, 24) && Is(runs[2], 0, 24, 24, 32));
    REQUIRE(set.Texels() == 24 * 24);

    // A second stamp far away: its own runs, and no tile between the two.
    set.Add(Rect(250, 80, 266, 96), w, h);
    REQUIRE(set.Has(31, 10) && set.Has(33, 11) && !set.Has(10, 5) && !set.Has(3, 3));
    runs = Runs(set);
    REQUIRE(runs.size() == 5);
    REQUIRE(Is(runs[3], 248, 80, 272, 88) && Is(runs[4], 248, 88, 272, 96));
    REQUIRE(set.Texels() == 24 * 24 + 24 * 16);

    // A part at the edge: the runs stop at the texture's edge, and a part
    // out of the texture marks nothing.
    GfxTileSet edge;
    edge.Add(Rect(290, 95, 400, 200), w, h);
    runs = Runs(edge);
    REQUIRE(runs.size() == 2 && Is(runs[0], 288, 88, 300, 96) && Is(runs[1], 288, 96, 300, 100));
    GfxTileSet outside;
    outside.Add(Rect(400, 0, 500, 10), w, h);
    REQUIRE(outside.Empty());

    // The union: the tiles of both, and a set takes the tiles of an empty
    // one as they are.
    GfxTileSet both;
    both.Add(set);
    both.Add(edge);
    REQUIRE(both.Has(1, 1) && both.Has(32, 11) && both.Has(37, 12) && !both.Has(10, 5));
    REQUIRE(Runs(both).size() == 7);
    // The union does not change the other set.
    REQUIRE(!set.Has(37, 12));

    // Clear: nothing, and the next part starts a set of only its tiles.
    both.Clear();
    REQUIRE(both.Empty() && Runs(both).empty() && !both.Has(1, 1));
    both.Add(Rect(0, 0, 1, 1), w, h);
    runs = Runs(both);
    REQUIRE(runs.size() == 1 && Is(runs[0], 0, 0, 8, 8));

    // A run across a word of the bits: tiles 30 to 35 of row 0.
    GfxTileSet across;
    across.Add(Rect(240, 0, 288, 8), w, h);
    runs = Runs(across);
    REQUIRE(runs.size() == 1 && Is(runs[0], 240, 0, 288, 8));

    printf("texture_tiles: ok\n");
    return 0;
}
