// GPU memory on the PS Vita: the one file that allocates and maps memblocks
// for GXM (plans/vita/RULES.md rule 6). It counts the live memblocks: the
// console freezes somewhere past 1,000 of them.
#ifndef OPENCMR2_GXM_MEMORY_H
#define OPENCMR2_GXM_MEMORY_H

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

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

#endif
