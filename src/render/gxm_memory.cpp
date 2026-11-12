// GPU memory on the PS Vita (gxm_memory.h).

#include "render/gxm_memory.h"

#include "port/sys.h"

namespace {

constexpr unsigned kMB = 1024 * 1024;
constexpr unsigned kKB = 1024;
constexpr unsigned kPageSize = 4 * 1024;
constexpr unsigned kCdramPageSize = 256 * 1024;

// The process's live memblocks: a kernel resource, so one count.
int s_liveBlocks;

// The worst case of plans/vita/PLAN.md section 3 (the PC trace, VP-1.4).
constexpr unsigned kFrameVertexBytesMax = 3419360;     // a load frame in France
constexpr unsigned kFrameIndicesMax = 36852;
constexpr unsigned kVertexBufferBytesMax = 11013 * kKB; // 103 live GfxVertexBuffers
constexpr unsigned kTextureBytesMax = 78689 * kKB;      // all formats, the France race
// A frame ring slot: the frame's vertices and indices, with half again of room.
constexpr unsigned kRingSlotBytes =
    ((kFrameVertexBytesMax + kFrameIndicesMax * 2) * 3 / 2 + kMB - 1) / kMB * kMB;
constexpr unsigned kStaticArenaBytes = 4 * kMB;
constexpr unsigned kTextureArenaBytes = 16 * kMB;
constexpr unsigned kTextureReserveBytes = kTextureBytesMax / 4 * 5;   // 25 percent over

const SceGxmMemoryAttribFlags kRead = SCE_GXM_MEMORY_ATTRIB_READ;
const SceGxmMemoryAttribFlags kReadWrite =
    (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);

unsigned RoundUp(unsigned size, unsigned step)
{
    return (size + step - 1) & ~(step - 1);
}

// An unmapped memblock of at least size bytes.
bool AllocBlock(SceKernelMemBlockType type, unsigned size, GxmBlock &block)
{
    const unsigned step = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? kCdramPageSize : kPageSize;
    block.size = RoundUp(size, step);
    block.uid = sceKernelAllocMemBlock("opencmr2_gpu", type, block.size, NULL);
    if (block.uid < 0) {
        Sys_Log("gxm mem: sceKernelAllocMemBlock(%u KB): 0x%08x (%d blocks live)", block.size / 1024,
                (unsigned)block.uid, s_liveBlocks);
        block = GxmBlock();
        return false;
    }
    sceKernelGetMemBlockBase(block.uid, &block.data);
    s_liveBlocks++;
    return true;
}

void FreeBlock(GxmBlock &block)
{
    sceKernelFreeMemBlock(block.uid);
    s_liveBlocks--;
    block = GxmBlock();
}

} // namespace

bool Gxm_AllocGpu(SceKernelMemBlockType type, unsigned size, SceGxmMemoryAttribFlags attribs, GxmBlock &block)
{
    if (!AllocBlock(type, size, block))
        return false;
    const int err = sceGxmMapMemory(block.data, block.size, attribs);
    if (err < 0) {
        Sys_Log("gxm mem: sceGxmMapMemory(%u KB): 0x%08x", block.size / 1024, (unsigned)err);
        FreeBlock(block);
        return false;
    }
    return true;
}

void Gxm_FreeGpu(GxmBlock &block)
{
    if (block.uid < 0)
        return;
    sceGxmUnmapMemory(block.data);
    FreeBlock(block);
}

bool Gxm_AllocVertexUsse(unsigned size, GxmBlock &block)
{
    if (!AllocBlock(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, block))
        return false;
    const int err = sceGxmMapVertexUsseMemory(block.data, block.size, &block.usseOffset);
    if (err < 0) {
        Sys_Log("gxm mem: sceGxmMapVertexUsseMemory: 0x%08x", (unsigned)err);
        FreeBlock(block);
        return false;
    }
    return true;
}

bool Gxm_AllocFragmentUsse(unsigned size, GxmBlock &block)
{
    if (!AllocBlock(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, block))
        return false;
    const int err = sceGxmMapFragmentUsseMemory(block.data, block.size, &block.usseOffset);
    if (err < 0) {
        Sys_Log("gxm mem: sceGxmMapFragmentUsseMemory: 0x%08x", (unsigned)err);
        FreeBlock(block);
        return false;
    }
    return true;
}

void Gxm_FreeVertexUsse(GxmBlock &block)
{
    if (block.uid < 0)
        return;
    sceGxmUnmapVertexUsseMemory(block.data);
    FreeBlock(block);
}

void Gxm_FreeFragmentUsse(GxmBlock &block)
{
    if (block.uid < 0)
        return;
    sceGxmUnmapFragmentUsseMemory(block.data);
    FreeBlock(block);
}

int Gxm_LiveBlocks(void)
{
    return s_liveBlocks;
}

// ---- pools ----

unsigned char *GxmArenaSource::Alloc(unsigned bytes, int *handle)
{
    GxmBlock block;
    if (!Gxm_AllocGpu(type, bytes, attribs, block)) {
        if (fallback == type || !Gxm_AllocGpu(fallback, bytes, attribs, block))
            return nullptr;
        Sys_Log("gxm mem: no room for %u KB of memblock type 0x%08x: took type 0x%08x", bytes / kKB, (unsigned)type,
                (unsigned)fallback);
    }
    *handle = (int)blocks.size();
    blocks.push_back(block);
    return (unsigned char *)block.data;
}

void GxmArenaSource::Free(int handle)
{
    if (handle >= 0 && handle < (int)blocks.size())
        Gxm_FreeGpu(blocks[handle]);
}

GxmMemory::GxmMemory()
    : staticSource(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, kRead),
      textureSource(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, kReadWrite),
      staticPool(staticSource, kStaticArenaBytes), texturePool(textureSource, kTextureArenaBytes)
{
}

bool GxmMemory::Init()
{
    if (!Gxm_AllocGpu(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, kRingSlotBytes * 3, kRead, ringMemory))
        return false;
    ring.base = (unsigned char *)ringMemory.data;
    ring.slotBytes = kRingSlotBytes;
    if (!staticPool.Reserve(kVertexBufferBytesMax) || !texturePool.Reserve(kTextureReserveBytes))
        return false;
    loggedGrowths = staticPool.Growths() + texturePool.Growths();
    Log();
    return true;
}

GxmMemory::~GxmMemory()
{
    Gxm_FreeGpu(ringMemory);
}

void GxmMemory::Log()
{
    const int growths = staticPool.Growths() + texturePool.Growths();
    Sys_Log("gxm mem: blocks %d, frame ring %u MB, static %.1f/%.1f MB, textures %.1f/%.1f MB, growths %d",
            Gxm_LiveBlocks(), ringMemory.size / kMB, staticPool.Used() / (double)kMB,
            staticPool.Capacity() / (double)kMB, texturePool.Used() / (double)kMB,
            texturePool.Capacity() / (double)kMB, growths - loggedGrowths);
    loggedGrowths = growths;
}
