// A pool of GPU memory: a few large arenas, sub-allocated (gpu_sub_allocator.h),
// so that the process holds tens of memblocks, not thousands. No GXM here:
// the arenas come from a GpuArenaSource, GPU memblocks on the Vita
// (gxm_memory.cpp) and the heap in tests/gpu_sub_allocator.cpp.
#ifndef OPENCMR2_GPU_POOL_H
#define OPENCMR2_GPU_POOL_H

#include "render/gpu_sub_allocator.h"

#include <vector>

struct GpuArenaSource {
    virtual ~GpuArenaSource() {}
    // At least bytes of memory, or nullptr; *handle names it for Free.
    virtual unsigned char *Alloc(unsigned bytes, int *handle) = 0;
    virtual void Free(int handle) = 0;
};

class GpuPool {
public:
    GpuPool(GpuArenaSource &source, unsigned arenaBytes) : source(source), arenaBytes(arenaBytes) {}
    GpuPool(const GpuPool &) = delete;
    GpuPool &operator=(const GpuPool &) = delete;

    ~GpuPool()
    {
        for (const OwnBlock &b : own)
            source.Free(b.handle);
        for (size_t i = 0; i < arenas.ArenaCount(); i++)
            source.Free(arenas.GetArena(i).handle);
    }

    // Arenas for bytes of allocations, made now (at load), not in a frame.
    bool Reserve(unsigned bytes)
    {
        while ((unsigned)arenas.Capacity() < bytes)
            if (!AddArena())
                return false;
        return true;
    }

    // 16-byte aligned. More than half an arena takes its own block. When the
    // arenas are full, a new one: Growths() counts them (each is a hitch).
    void *Alloc(unsigned bytes)
    {
        if (bytes > arenaBytes / 2) {
            OwnBlock b;
            b.data = source.Alloc(bytes, &b.handle);
            if (b.data == nullptr)
                return nullptr;
            b.bytes = bytes;
            own.push_back(b);
            return b.data;
        }
        if (void *p = arenas.Alloc(bytes))
            return p;
        if (!AddArena())
            return nullptr;
        growths++;
        return arenas.Alloc(bytes);
    }

    void Free(void *p)
    {
        if (p == nullptr)
            return;
        for (size_t i = 0; i < own.size(); i++)
            if (own[i].data == p) {
                source.Free(own[i].handle);
                own.erase(own.begin() + (std::ptrdiff_t)i);
                return;
            }
        arenas.Free(p);
    }

    // Bytes handed out (with the sub-allocator's headers) and held.
    unsigned Used() const
    {
        unsigned used = (unsigned)arenas.Used();
        for (const OwnBlock &b : own)
            used += b.bytes;
        return used;
    }
    unsigned Capacity() const
    {
        unsigned capacity = (unsigned)arenas.Capacity();
        for (const OwnBlock &b : own)
            capacity += b.bytes;
        return capacity;
    }
    int Growths() const { return growths; }
    int Blocks() const { return (int)(arenas.ArenaCount() + own.size()); }
    const GpuSubAllocator &Arenas() const { return arenas; }

private:
    struct OwnBlock {
        unsigned char *data = nullptr;
        unsigned bytes = 0;
        int handle = -1;
    };

    bool AddArena()
    {
        int handle;
        unsigned char *base = source.Alloc(arenaBytes, &handle);
        if (base == nullptr)
            return false;
        arenas.AddArena(base, arenaBytes, handle);
        return true;
    }

    GpuArenaSource &source;
    const unsigned arenaBytes;
    GpuSubAllocator arenas;
    std::vector<OwnBlock> own;
    int growths = 0;
};

#endif
