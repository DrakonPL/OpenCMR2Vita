// GPU memory on the PS Vita (gxm_memory.h).

#include "render/gxm_memory.h"

#include "port/sys.h"

namespace {

constexpr unsigned kPageSize = 4 * 1024;
constexpr unsigned kCdramPageSize = 256 * 1024;

// The process's live memblocks: a kernel resource, so one count.
int s_liveBlocks;

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
