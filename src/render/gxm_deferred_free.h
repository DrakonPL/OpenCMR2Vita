// GPU memory of a destroyed texture, freed only when the GPU is done with it:
// kGxmFrameSlots frames after the frame that was recording when the texture
// was destroyed (gxm_frame_ring.h says why that frame is then done). No GXM
// here: tests/gxm_deferred_free.cpp checks the delay.
#ifndef OPENCMR2_GXM_DEFERRED_FREE_H
#define OPENCMR2_GXM_DEFERRED_FREE_H

#include "render/gxm_frame_ring.h"

#include <stddef.h>

#include <vector>

struct GxmDeferredFree {
    struct Entry {
        void *memory;
        unsigned frame;     // the frame recording when it was let go
    };
    std::vector<Entry> entries;

    // frame: the number of the frame being recorded (frames presented so far).
    void Add(void *memory, unsigned frame) { entries.push_back({ memory, frame }); }

    // Calls release(memory) for every entry the GPU is done with, at the start
    // of recording frame.
    template <typename Release> void Collect(unsigned frame, Release release)
    {
        size_t kept = 0;
        for (const Entry &e : entries) {
            if (frame - e.frame >= (unsigned)g_gxmFrameSlots)
                release(e.memory);
            else
                entries[kept++] = e;
        }
        entries.resize(kept);
    }
};

#endif
