// GPU memory on the PS Vita: the one file that allocates and maps memblocks
// for GXM (plans/vita/RULES.md rule 6). It counts the live memblocks: the
// console freezes somewhere past 1,000 of them.
#ifndef OPENCMR2_GXM_MEMORY_H
#define OPENCMR2_GXM_MEMORY_H

#include "render/gpu_pool.h"
#include "render/gxm_frame_ring.h"

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include <vector>

struct GxmBlock {
    SceUID uid = -1;
    void *data = nullptr;
    unsigned size = 0;
    unsigned usseOffset = 0;    // USSE code blocks: the address the GPU sees
};

// A memblock of type (USER_RW_UNCACHE in 4 KB steps, USER_CDRAM_RW in 256 KB
// steps), mapped for the GPU. Logs and returns false when it cannot.
bool Gxm_AllocGpu(SceKernelMemBlockType type, unsigned size, SceGxmMemoryAttribFlags attribs, GxmBlock &block);
void Gxm_FreeGpu(GxmBlock &block);
// Memory for USSE code (the shader patcher, the fragment USSE ring).
bool Gxm_AllocVertexUsse(unsigned size, GxmBlock &block);
bool Gxm_AllocFragmentUsse(unsigned size, GxmBlock &block);
void Gxm_FreeVertexUsse(GxmBlock &block);
void Gxm_FreeFragmentUsse(GxmBlock &block);

int Gxm_LiveBlocks(void);

// Arenas for a GpuPool: memblocks of one type, or of the fallback type when
// that one has no room (CDRAM is 128 MB).
class GxmArenaSource : public GpuArenaSource {
public:
    GxmArenaSource(SceKernelMemBlockType type, SceKernelMemBlockType fallback, SceGxmMemoryAttribFlags attribs)
        : type(type), fallback(fallback), attribs(attribs)
    {
    }
    unsigned char *Alloc(unsigned bytes, int *handle) override;
    void Free(int handle) override;

private:
    SceKernelMemBlockType type, fallback;
    SceGxmMemoryAttribFlags attribs;
    std::vector<GxmBlock> blocks;   // a handle is an index; a freed one is empty
};

// The executor's GPU memory, made at init from the worst case of
// plans/vita/PLAN.md section 3: the frame ring, the static vertex pool (the
// GfxVertexBuffer copies, small fixed buffers) and the texture pool.
class GxmMemory {
public:
    GxmMemory();
    bool Init();
    ~GxmMemory();

    GxmFrameRing &FrameRing() { return ring; }
    GpuPool &Static() { return staticPool; }
    GpuPool &Textures() { return texturePool; }

    // "gxm mem: blocks N, frame ring X MB, static Y/Z MB, textures A/B MB",
    // and the arena growths since the last line.
    void Log();

private:
    GxmBlock ringMemory;
    GxmFrameRing ring;
    GxmArenaSource staticSource, textureSource;
    GpuPool staticPool, texturePool;
    int loggedGrowths = 0;
};

#endif
