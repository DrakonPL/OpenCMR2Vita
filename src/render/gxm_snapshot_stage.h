// The vertex buffer snapshots of a frame on their way to the frame ring. The
// ring is uncached memory: the CPU writes it at some 130 MB/s on the PS Vita,
// and the snapshots of a frame of a wood took 4 ms of the game's thread.
// Prepare copies each snapshot into this cached memory, and Execute moves
// all of them into the ring in one copy (on the worker of the threaded
// executor). No GXM here: tests/gpu_sub_allocator.cpp checks it on the PC.
#ifndef OPENCMR2_GXM_SNAPSHOT_STAGE_H
#define OPENCMR2_GXM_SNAPSHOT_STAGE_H

#include "render/gxm_frame_ring.h"

#include <utility>
#include <vector>

struct GxmSnapshotStage {
    static constexpr unsigned kAlign = 16;      // bytes, as each snapshot in the ring

    // Copies size bytes of a snapshot. Publish sets *ringOffset to the ring
    // offset of its first byte.
    void Add(const void *source, unsigned size, unsigned *ringOffset)
    {
        const size_t start = (bytes.size() + kAlign - 1) & ~(size_t)(kAlign - 1);
        bytes.resize(start);
        bytes.insert(bytes.end(), (const unsigned char *)source, (const unsigned char *)source + size);
        places.push_back(std::make_pair(ringOffset, (unsigned)start));
    }

    unsigned Bytes() const { return (unsigned)bytes.size(); }
    const unsigned char *Data() const { return bytes.data(); }

    // The staged bytes are at ring offset base (an offset of kAlign), or
    // nowhere (GxmFrameRing::kFull): each snapshot gets its ring offset, or
    // kFull. Then the stage is empty, with its memory kept for the next frame.
    void Publish(unsigned base)
    {
        for (const std::pair<unsigned *, unsigned> &p : places)
            *p.first = base == GxmFrameRing::kFull ? GxmFrameRing::kFull : base + p.second;
        bytes.clear();
        places.clear();
    }

private:
    std::vector<unsigned char> bytes;
    std::vector<std::pair<unsigned *, unsigned>> places;    // where each snapshot's ring offset goes, its offset here
};

#endif
