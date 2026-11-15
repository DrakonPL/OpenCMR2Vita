// The frame's vertex and index data on the GPU: one slot per frame in flight,
// filled from its start by each frame (the Execute calls of one frame
// append). No GXM here, so tests/gpu_sub_allocator.cpp can test the clamp.
//
// A slot is free again when its frame comes round: the display queue holds
// g_gxmFrameSlots - 1 flips at most, so when the CPU starts frame
// n + g_gxmFrameSlots, frame n is shown and the GPU is done with it.
#ifndef OPENCMR2_GXM_FRAME_RING_H
#define OPENCMR2_GXM_FRAME_RING_H

// Frames in flight: the CPU records one, the GPU draws one, the display
// shows one. Per-frame data has a slot for each. [video] frames_in_flight = 3
// gives one more slot (g_gxmFrameSlots 4): the GPU then has two frames to draw
// when the CPU starts one, and the worker waits less for it at the present,
// for one more frame between the input and the picture.
constexpr int kGxmFrameSlots = 3;
constexpr int kGxmMaxFrameSlots = 4;
// The slots in use, set once by the GXM device before its first frame.
inline int g_gxmFrameSlots = kGxmFrameSlots;

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
