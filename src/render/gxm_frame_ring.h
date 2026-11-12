// The frame's vertex and index data on the GPU: one slot per frame in flight,
// filled from its start by each frame (the Execute calls of one frame
// append). No GXM here, so tests/gpu_sub_allocator.cpp can test the clamp.
//
// A slot is free again when its frame comes round: the display queue holds
// two flips at most, so when the CPU starts frame n + 3, frame n is shown and
// the GPU is done with it.
#ifndef OPENCMR2_GXM_FRAME_RING_H
#define OPENCMR2_GXM_FRAME_RING_H

// Frames in flight: the CPU records one, the GPU draws one, the display
// shows one. Per-frame data has a slot for each.
constexpr int kGxmFrameSlots = 3;

struct GxmFrameRing {
    static constexpr unsigned kFull = ~0u;

    unsigned char *base = nullptr;  // slots * slotBytes
    unsigned slotBytes = 0;
    int slot = 0;
    unsigned used = 0;              // in the current slot
    unsigned dropped = 0;           // allocations that did not fit, this frame

    void BeginFrame(int frameSlot)
    {
        slot = frameSlot;
        used = 0;
        dropped = 0;
    }

    // The offset into base of bytes at an alignment (a power of two) in the
    // current slot, or kFull: the slot is full and the caller drops the draw.
    unsigned Alloc(unsigned bytes, unsigned align)
    {
        const unsigned start = (used + align - 1) & ~(align - 1);
        if (start > slotBytes || bytes > slotBytes - start) {
            dropped++;
            return kFull;
        }
        used = start + bytes;
        return (unsigned)slot * slotBytes + start;
    }

    unsigned char *At(unsigned offset) const { return base + offset; }
};

#endif
