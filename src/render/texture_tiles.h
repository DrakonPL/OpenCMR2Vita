// The parts of level 0 of a texture that changed, as a set of tiles of
// 8 x 8 texels. The dirt stamps of the game write up to 64 parts of 16 x 16
// texels in a frame, spread over a quarter of the car's body texture. One
// rectangle around them all was a quarter of the texture: 18 ms for each
// upload on the PS Vita. The tiles of the stamps are some kilobytes. No GXM
// here: tests/texture_tiles.cpp checks it on the PC.
#ifndef OPENCMR2_TEXTURE_TILES_H
#define OPENCMR2_TEXTURE_TILES_H

#include "render/texture_mips.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <vector>

struct GfxTileSet {
    static constexpr int kTile = 8;     // texels

    int width = 0, height = 0;          // level 0, in texels
    int tilesX = 0, tilesY = 0;
    bool any = false;                   // false: no tile
    std::vector<uint32_t> bits;         // a bit for each tile, row by row

    bool Empty() const { return !any; }

    void Clear()
    {
        if (any)
            std::fill(bits.begin(), bits.end(), 0u);
        any = false;
    }

    bool Has(int tx, int ty) const
    {
        const size_t i = (size_t)ty * (size_t)tilesX + (size_t)tx;
        return any && (bits[i >> 5] >> (i & 31) & 1u) != 0;
    }

    // Marks each tile that a part of a texture of w x h texels touches. A
    // set of another size starts again, empty.
    void Add(GfxMipRect r, int w, int h)
    {
        if (w != width || h != height) {
            width = w;
            height = h;
            tilesX = (w + kTile - 1) / kTile;
            tilesY = (h + kTile - 1) / kTile;
            bits.assign(((size_t)tilesX * (size_t)tilesY + 31) / 32, 0u);
            any = false;
        }
        r.x0 = std::max(r.x0, 0);
        r.y0 = std::max(r.y0, 0);
        r.x1 = std::min(r.x1, w);
        r.y1 = std::min(r.y1, h);
        if (r.Empty())
            return;
        for (int ty = r.y0 / kTile; ty <= (r.y1 - 1) / kTile; ty++)
            for (int tx = r.x0 / kTile; tx <= (r.x1 - 1) / kTile; tx++) {
                const size_t i = (size_t)ty * (size_t)tilesX + (size_t)tx;
                bits[i >> 5] |= 1u << (i & 31);
            }
        any = true;
    }

    // Marks each tile of another set of the same texture.
    void Add(const GfxTileSet &other)
    {
        if (!other.any)
            return;
        if (!any || other.width != width || other.height != height) {
            *this = other;
            return;
        }
        for (size_t i = 0; i < bits.size(); i++)
            bits[i] |= other.bits[i];
    }

    // Calls f(GfxMipRect) for each run of marked tiles in a row of tiles,
    // in texels and inside the texture.
    template <typename F>
    void ForEachRun(F f) const
    {
        if (!any)
            return;
        for (int ty = 0; ty < tilesY; ty++) {
            int tx = 0;
            while (tx < tilesX) {
                const size_t i = (size_t)ty * (size_t)tilesX + (size_t)tx;
                // A word with no mark that starts here: skip it.
                if ((i & 31) == 0 && bits[i >> 5] == 0) {
                    tx += 32;
                    continue;
                }
                if (!Has(tx, ty)) {
                    tx++;
                    continue;
                }
                const int start = tx;
                while (tx < tilesX && Has(tx, ty))
                    tx++;
                GfxMipRect r;
                r.x0 = start * kTile;
                r.y0 = ty * kTile;
                r.x1 = std::min(tx * kTile, width);
                r.y1 = std::min((ty + 1) * kTile, height);
                f(r);
            }
        }
    }

    // The texels of the marked tiles, for the logs.
    size_t Texels() const
    {
        size_t texels = 0;
        ForEachRun([&](GfxMipRect r) { texels += (size_t)(r.x1 - r.x0) * (size_t)(r.y1 - r.y0); });
        return texels;
    }
};

#endif
