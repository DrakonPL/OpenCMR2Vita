// The spare GPU copies of a texture that the game changes in a part in each
// frame (the dirt stamps on the car's body: a texture of 1,024 x 1,024 texels
// and 32 bits with its mip levels, 5.3 MB). A copy that a frame on the GPU
// still reads is not written: the changed part goes into a spare copy that
// no frame reads, with the parts that the spare missed, and the spare and
// the copy that is drawn change places. The whole texture into new memory
// took 50 ms of each frame on the PS Vita. No GXM here:
// tests/gpu_sub_allocator.cpp checks it on the PC.
#ifndef OPENCMR2_GXM_TEXTURE_COPIES_H
#define OPENCMR2_GXM_TEXTURE_COPIES_H

#include "render/gxm_frame_ring.h"
#include "render/texture_tiles.h"

#include <stddef.h>

#include <vector>

struct GxmTextureCopies {
    // A copy for each frame in flight with the one that is drawn: a frame that
    // the GPU draws reads its copy for g_gxmFrameSlots frames.
    static size_t MaxSpares() { return (size_t)g_gxmFrameSlots - 1; }
    static constexpr unsigned kNotDrawn = ~0u;

    struct Spare {
        void *memory;
        bool whole;             // the spare misses more than a part: all of it is written
        GfxTileSet stale;       // else the tiles of level 0 that it misses (empty: none)
        unsigned lastDrawn;     // the last frame that drew with it, or kNotDrawn
    };
    std::vector<Spare> spares;

    // Tiles of level 0 changed in the copy that is drawn: each spare misses them.
    void Changed(const GfxTileSet &part)
    {
        for (Spare &s : spares)
            if (!s.whole)
                s.stale.Add(part);
    }
    // All of the texture changed (its levels too).
    void ChangedWhole()
    {
        for (Spare &s : spares)
            s.whole = true;
    }

    // A spare that no frame on the GPU reads at the frame, or -1.
    int Free(unsigned frame) const
    {
        for (size_t i = 0; i < spares.size(); i++)
            if (spares[i].lastDrawn == kNotDrawn || frame - spares[i].lastDrawn >= (unsigned)g_gxmFrameSlots)
                return (int)i;
        return -1;
    }

    // A new spare of memory that has nothing yet. False: there are
    // kMaxSpares already.
    bool Add(void *memory)
    {
        if (spares.size() >= MaxSpares())
            return false;
        spares.push_back({ memory, true, GfxTileSet(), kNotDrawn });
        return true;
    }

    // The spare i, written up to date, becomes the copy that is drawn. The
    // copy that was drawn (its memory and the last frame that drew with it)
    // is a spare that misses the tiles that changed now. Returns the memory
    // of the copy to draw.
    void *Swap(int i, void *drawn, unsigned drawnLastDrawn, const GfxTileSet &part)
    {
        void *now = spares[(size_t)i].memory;
        spares[(size_t)i] = { drawn, false, part, drawnLastDrawn };
        return now;
    }
};

#endif
