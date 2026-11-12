// The GPU memory bookkeeping of the GXM executor without GXM: the
// sub-allocator (render/gpu_sub_allocator.h), the pool over it
// (render/gpu_pool.h) with heap arenas, and the frame ring's clamp
// (render/gxm_frame_ring.h).

#include "render/gpu_pool.h"
#include "render/gxm_frame_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// Heap arenas, aligned as memblocks are, counted.
struct HeapSource : GpuArenaSource {
    std::vector<unsigned char *> blocks;
    int live = 0;
    int limit = 1000;   // arenas it hands out before it refuses

    unsigned char *Alloc(unsigned bytes, int *handle) override
    {
        if (live >= limit)
            return nullptr;
        unsigned char *p = (unsigned char *)aligned_alloc(4096, (bytes + 4095) / 4096 * 4096);
        *handle = (int)blocks.size();
        blocks.push_back(p);
        live++;
        return p;
    }
    void Free(int handle) override
    {
        free(blocks[handle]);
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

} // namespace

int main()
{
    if (SubAllocator() || Pool() || FrameRing())
        return 1;
    printf("gpu_sub_allocator: ok\n");
    return 0;
}
