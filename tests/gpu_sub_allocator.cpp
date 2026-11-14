// The GPU memory bookkeeping of the GXM executor without GXM: the
// sub-allocator (render/gpu_sub_allocator.h), the pool over it
// (render/gpu_pool.h) with heap arenas, the frame ring's clamp
// (render/gxm_frame_ring.h), and the snapshot stage (render/gxm_snapshot_stage.h).

#include "render/gpu_pool.h"
#include "render/gxm_frame_ring.h"
#include "render/gxm_snapshot_stage.h"
#include "render/gxm_texture_copies.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

constexpr std::align_val_t kArenaAlign{4096};   // bytes, as memblocks are

// Heap arenas, aligned as memblocks are, counted.
struct HeapSource : GpuArenaSource {
    std::vector<unsigned char *> blocks;
    int live = 0;
    int limit = 1000;   // arenas it hands out before it refuses

    unsigned char *Alloc(unsigned bytes, int *handle) override
    {
        if (live >= limit)
            return nullptr;
        unsigned char *p = (unsigned char *)::operator new((bytes + 4095) / 4096 * 4096, kArenaAlign);
        *handle = (int)blocks.size();
        blocks.push_back(p);
        live++;
        return p;
    }
    void Free(int handle) override
    {
        ::operator delete(blocks[handle], kArenaAlign);
        blocks[handle] = nullptr;
        live--;
    }
};

int SubAllocator()
{
    std::vector<unsigned char> memory(1024);
    GpuSubAllocator a;
    a.AddArena(memory.data(), 1024, 0);
    const unsigned block = GpuSubAllocator::Total(100);   // 128: 112 payload + 16 header

    // Payloads are 16-byte aligned, after their header, in order (first fit).
    unsigned char *p0 = (unsigned char *)a.Alloc(100);
    unsigned char *p1 = (unsigned char *)a.Alloc(100);
    unsigned char *p2 = (unsigned char *)a.Alloc(100);
    REQUIRE(p0 == memory.data() + GpuSubAllocator::kHeader);
    REQUIRE(p1 == p0 + block && p2 == p1 + block);
    REQUIRE(((uintptr_t)p0 - (uintptr_t)memory.data()) % GpuSubAllocator::kAlign == 0);
    REQUIRE(a.Live() == 3 && a.Used() == 3 * block);

    // A freed range is the first fit for the next allocation of its size.
    REQUIRE(a.Free(p1) == 0);
    REQUIRE(a.Alloc(100) == p1);

    // Freed in any order, the neighbours merge: the arena is whole again.
    REQUIRE(a.Free(p1) == 0 && a.Free(p0) == 0 && a.Free(p2) == 0);
    REQUIRE(a.ArenaIsWhole(0));
    REQUIRE(a.Live() == 0 && a.Used() == 0);

    // A second free of the same pointer is refused.
    REQUIRE(a.Free(p0) == -1);

    // No range is large enough: nullptr, and nothing changes.
    REQUIRE(a.Alloc(2000) == nullptr);
    REQUIRE(a.ArenaIsWhole(0));
    return 0;
}

int Pool()
{
    HeapSource source;
    {
        GpuPool pool(source, 64 * 1024);

        // Reserve makes the arenas at load: no growth after it.
        REQUIRE(pool.Reserve(100 * 1024));
        REQUIRE(source.live == 2 && pool.Capacity() == 128 * 1024);
        void *small = pool.Alloc(1000);
        REQUIRE(small != nullptr && pool.Growths() == 0 && pool.Blocks() == 2);

        // More than half an arena takes its own block, and gives it back.
        void *large = pool.Alloc(40 * 1024);
        REQUIRE(large != nullptr && source.live == 3 && pool.Blocks() == 3);
        REQUIRE(pool.Used() >= 40 * 1024 + 1000);
        pool.Free(large);
        REQUIRE(source.live == 2 && pool.Blocks() == 2);

        // Half an arena and less comes from the arenas, until they are full;
        // then one new arena, counted as a growth.
        std::vector<void *> halves;
        for (int i = 0; i < 4; i++)
            halves.push_back(pool.Alloc(30 * 1024));
        REQUIRE(source.live == 2 && pool.Growths() == 0);
        void *more = pool.Alloc(30 * 1024);
        REQUIRE(more != nullptr && source.live == 3 && pool.Growths() == 1);

        // When the source has no more, Alloc fails without a crash.
        source.limit = source.live;
        REQUIRE(pool.Alloc(40 * 1024) == nullptr);
        for (void *p : halves)
            pool.Free(p);
        pool.Free(more);
        pool.Free(small);
        for (size_t i = 0; i < pool.Arenas().ArenaCount(); i++)
            REQUIRE(pool.Arenas().ArenaIsWhole(i));
    }
    // The pool gives every arena back.
    REQUIRE(source.live == 0);
    return 0;
}

int FrameRing()
{
    std::vector<unsigned char> memory(3 * 1000);
    GxmFrameRing ring;
    ring.base = memory.data();
    ring.slotBytes = 1000;

    // Each frame fills its slot from the start, aligned.
    ring.BeginFrame(1);
    REQUIRE(ring.Alloc(10, 4) == 1000);
    REQUIRE(ring.Alloc(8, 16) == 1016);
    ring.BeginFrame(2);
    REQUIRE(ring.Alloc(500, 4) == 2000);

    // A frame larger than the slot: the allocation that does not fit fails
    // and is counted, nothing is written past the slot, smaller ones that
    // still fit go on.
    ring.BeginFrame(0);
    REQUIRE(ring.Alloc(900, 4) == 0);
    REQUIRE(ring.Alloc(200, 4) == GxmFrameRing::kFull);
    REQUIRE(ring.dropped == 1 && ring.used == 900);
    REQUIRE(ring.Alloc(100, 4) == 900);
    REQUIRE(ring.used == 1000);
    // Alignment past the end, and a size that would wrap the arithmetic.
    REQUIRE(ring.Alloc(1, 16) == GxmFrameRing::kFull);
    REQUIRE(ring.Alloc(0xfffffff0u, 4) == GxmFrameRing::kFull);
    REQUIRE(ring.dropped == 3);
    REQUIRE(ring.used <= ring.slotBytes);

    // The next frame in the slot starts empty.
    ring.BeginFrame(0);
    REQUIRE(ring.dropped == 0 && ring.Alloc(1000, 4) == 0);
    return 0;
}

int SnapshotStage()
{
    GxmSnapshotStage stage;
    unsigned char a[20], b[7], c[32];
    for (unsigned i = 0; i < sizeof(a); i++)
        a[i] = (unsigned char)(i + 1);
    for (unsigned i = 0; i < sizeof(b); i++)
        b[i] = (unsigned char)(100 + i);
    for (unsigned i = 0; i < sizeof(c); i++)
        c[i] = (unsigned char)(200 + i);
    unsigned offsetA = 1, offsetB = 1, offsetC = 1;

    // Each snapshot starts at an offset of kAlign; Publish gives each its
    // ring offset, and its bytes are there after one copy of the stage.
    stage.Add(a, sizeof(a), &offsetA);
    stage.Add(b, sizeof(b), &offsetB);
    stage.Add(c, sizeof(c), &offsetC);
    REQUIRE(stage.Bytes() == 48 + sizeof(c));
    std::vector<unsigned char> ring(1024);
    const unsigned base = 256;
    memcpy(ring.data() + base, stage.Data(), stage.Bytes());
    stage.Publish(base);
    REQUIRE(offsetA == base && offsetB == base + 32 && offsetC == base + 48);
    REQUIRE(offsetB % GxmSnapshotStage::kAlign == 0 && offsetC % GxmSnapshotStage::kAlign == 0);
    REQUIRE(memcmp(ring.data() + offsetA, a, sizeof(a)) == 0);
    REQUIRE(memcmp(ring.data() + offsetB, b, sizeof(b)) == 0);
    REQUIRE(memcmp(ring.data() + offsetC, c, sizeof(c)) == 0);

    // After Publish the stage is empty: the next frame starts at offset 0.
    REQUIRE(stage.Bytes() == 0);
    stage.Add(b, sizeof(b), &offsetB);
    stage.Publish(base);
    REQUIRE(offsetB == base);

    // A ring with no room: each snapshot gets kFull, and its draws are dropped.
    stage.Add(a, sizeof(a), &offsetA);
    stage.Add(c, sizeof(c), &offsetC);
    stage.Publish(GxmFrameRing::kFull);
    REQUIRE(offsetA == GxmFrameRing::kFull && offsetC == GxmFrameRing::kFull && stage.Bytes() == 0);
    return 0;
}

// A texture that changes in a part in each frame and is drawn in each frame:
// no copy that a frame on the GPU reads is written, three copies do, and
// each copy gets each part that it missed.
int TextureCopies()
{
    GxmTextureCopies copies;
    char memory[4];     // the copies' memory: 0 drawn, 1 to 3 new
    const int w = 256, h = 64;
    void *drawn = &memory[0];
    unsigned lastDrawn = GxmTextureCopies::kNotDrawn;
    // Which frame last drew each memory, and the parts each copy has.
    unsigned drawnAt[4] = { 0, 0, 0, 0 };
    std::vector<GfxMipRect> has[4];
    std::vector<GfxMipRect> all;
    int added = 1;
    for (unsigned frame = 10; frame < 40; frame++) {
        // A stamp in a new place in each frame, far from the last.
        GfxMipRect stamp;
        stamp.x0 = (int)(frame % 8) * 32;
        stamp.y0 = (int)(frame % 3) * 20;
        stamp.x1 = stamp.x0 + 16;
        stamp.y1 = stamp.y0 + 16;
        GfxTileSet part;
        part.Add(stamp, w, h);
        all.push_back(stamp);
        if (lastDrawn == GxmTextureCopies::kNotDrawn || frame - lastDrawn >= (unsigned)kGxmFrameSlots) {
            has[(char *)drawn - memory].push_back(stamp);
            copies.Changed(part);
        } else {
            int spare = copies.Free(frame);
            if (spare < 0) {
                REQUIRE(copies.Add(&memory[added++]));
                spare = (int)copies.spares.size() - 1;
            }
            copies.Changed(part);
            GxmTextureCopies::Spare &s = copies.spares[(size_t)spare];
            const int index = (int)((char *)s.memory - memory);
            // A frame that the GPU draws does not read the spare.
            REQUIRE(drawnAt[index] == 0 || frame - drawnAt[index] >= (unsigned)kGxmFrameSlots);
            if (!s.whole) {
                // The stale tiles of the spare hold each part that it misses.
                for (const GfxMipRect &p : all) {
                    bool in = false;
                    for (const GfxMipRect &e : has[index])
                        in = in || (e.x0 == p.x0 && e.y0 == p.y0);
                    if (!in)
                        for (int ty = p.y0 / GfxTileSet::kTile; ty <= (p.y1 - 1) / GfxTileSet::kTile; ty++)
                            for (int tx = p.x0 / GfxTileSet::kTile; tx <= (p.x1 - 1) / GfxTileSet::kTile; tx++)
                                REQUIRE(s.stale.Has(tx, ty));
                }
                // And no more than the parts of the last frames: not one
                // rectangle around them.
                REQUIRE(s.stale.Texels() <= (size_t)kGxmFrameSlots * 24 * 24);
            }
            has[index] = all;
            drawn = copies.Swap(spare, drawn, lastDrawn, part);
            lastDrawn = GxmTextureCopies::kNotDrawn;
        }
        // The frame draws with the copy that is drawn.
        lastDrawn = frame;
        drawnAt[(char *)drawn - memory] = frame;
        REQUIRE(has[(char *)drawn - memory].size() == all.size());
    }
    // Three copies with the one that is drawn, and no more.
    REQUIRE(added == 3 && copies.spares.size() == 2);
    REQUIRE(!copies.Add(&memory[3]));
    // A whole change: each spare is written whole.
    copies.ChangedWhole();
    for (const GxmTextureCopies::Spare &s : copies.spares)
        REQUIRE(s.whole);
    return 0;
}

} // namespace

int main()
{
    if (SubAllocator() || Pool() || FrameRing() || SnapshotStage() || TextureCopies())
        return 1;
    printf("gpu_sub_allocator: ok\n");
    return 0;
}
