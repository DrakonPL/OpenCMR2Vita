// The parts of a vertex buffer that the game changed a short time ago
// (Gfx_LockVertexBufferRange). The game keeps many meshes in one vertex
// buffer and writes one mesh again (the stage lighting, the reflection of a
// car). A lock of the whole buffer made each draw from it copy its vertices
// for some frames: 815 KB in a frame of a wood on the PS Vita, for 105 KB
// that changed.
//
// The buffer is in blocks of kBlock vertices. A block that a lock touched is
// recent for g_gfxRecentFrames frames: a draw that touches it takes its vertices
// from the frame's snapshot of the buffer (GfxFrame::snapshots), each other
// draw refers to the GPU copy. In a wood the stage lighting writes each mesh
// in the view again in each frame: all of them are recent then, and the
// frame copies each vertex once, as with a lock of the whole buffer. A block that rested is
// written into the GPU copy (a patch) before the first draw that refers to
// it: the frames that the GPU still draws took its vertices from their
// snapshots, so no frame reads the part of the GPU copy that changes.
// g_gfxRecentFrames is the number of frames that can wait for the GPU when a
// frame starts (GfxExecutor::FramesInFlight): two, or three with the GXM
// executor's [video] frames_in_flight = 3 (gxm_frame_ring.h).
#ifndef OPENCMR2_GFX_RANGE_LOCKS_H
#define OPENCMR2_GFX_RANGE_LOCKS_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

// Set by the recorder from its executor, before the first frame.
inline uint32_t g_gfxRecentFrames = 2;

struct GfxRangeLocks {
    static constexpr uint32_t kBlock = 32;          // vertices

    // The game locks count vertices from first, in the frame of that number
    // (1 or more).
    void Lock(uint32_t first, uint32_t count, uint32_t frame)
    {
        if (count == 0)
            return;
        const size_t last = (first + count - 1) / kBlock;
        if (frames.size() <= last)
            frames.resize(last + 1, 0);
        for (size_t b = first / kBlock; b <= last; b++) {
            marked += frames[b] == 0 ? 1 : 0;
            frames[b] = frame;
        }
    }

    // Whether a draw of count vertices from first, in a frame, touches a
    // block that is recent.
    bool Recent(uint32_t first, uint32_t count, uint32_t frame) const
    {
        if (marked == 0 || count == 0)
            return false;
        const size_t last = (first + count - 1) / kBlock;
        for (size_t b = first / kBlock; b <= last && b < frames.size(); b++)
            if (frames[b] != 0 && frame - frames[b] < g_gfxRecentFrames)
                return true;
        return false;
    }

    // The blocks that rested, as runs of vertices: patch(first, end) for
    // each run, in rising order, with end at most vertexCount. The blocks
    // are then as not locked.
    template <typename Patch> void TakeRested(uint32_t frame, uint32_t vertexCount, Patch patch)
    {
        if (marked == 0)
            return;
        size_t run = frames.size();     // the first block of the run that is open
        for (size_t b = 0; b <= frames.size(); b++) {
            const bool rested = b < frames.size() && frames[b] != 0 && frame - frames[b] >= g_gfxRecentFrames;
            if (rested) {
                frames[b] = 0;
                marked--;
                if (run == frames.size())
                    run = b;
            } else if (run != frames.size()) {
                const uint32_t first = (uint32_t)(run * kBlock), end = (uint32_t)(b * kBlock);
                if (first < vertexCount)
                    patch(first, end < vertexCount ? end : vertexCount);
                run = frames.size();
            }
        }
    }

    bool Any() const { return marked != 0; }

private:
    std::vector<uint32_t> frames;   // for each block: the frame of its last lock; 0: not locked, or patched
    uint32_t marked = 0;            // the blocks with a frame
};

// A stamp for each block of a vertex buffer (the blocks of GfxRangeLocks):
// the recording whose small batches refer to the vertices of the block
// (GfxSmallItem::buffer). The stamps of a buffer only rise.
struct GfxBlockStamps {
    void Mark(uint32_t first, uint32_t count, uint32_t stamp)
    {
        if (count == 0)
            return;
        const size_t last = (first + count - 1) / GfxRangeLocks::kBlock;
        if (stamps.size() <= last)
            stamps.resize(last + 1, 0);
        for (size_t b = first / GfxRangeLocks::kBlock; b <= last; b++)
            stamps[b] = stamp;
        latest = stamp > latest ? stamp : latest;
    }

    // The largest stamp of the blocks that count vertices from first touch,
    // and that of all blocks. 0: no stamp.
    uint32_t Latest(uint32_t first, uint32_t count) const
    {
        uint32_t most = 0;
        if (count == 0)
            return most;
        const size_t last = (first + count - 1) / GfxRangeLocks::kBlock;
        for (size_t b = first / GfxRangeLocks::kBlock; b <= last && b < stamps.size(); b++)
            most = stamps[b] > most ? stamps[b] : most;
        return most;
    }
    uint32_t Latest() const { return latest; }

private:
    std::vector<uint32_t> stamps;
    uint32_t latest = 0;
};

// Whether count vertices from first touch one of the patches, which are in
// rising order and apart: each has first and end (the vertex after its last).
template <typename Patch> bool Gfx_TouchesPatch(const Patch *patches, size_t patchCount, uint32_t first, uint32_t count)
{
    size_t low = 0, high = patchCount;      // the first patch that ends after first
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (patches[middle].end > first)
            high = middle;
        else
            low = middle + 1;
    }
    return low < patchCount && patches[low].first < first + count;
}

#endif
