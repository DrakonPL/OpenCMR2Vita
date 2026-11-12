// The GPU side of the renderer's textures on the PS Vita: the GxmTexture
// behind GfxTexture::gpu, its upload from the CPU copy, its sampler state,
// render targets, and the free that waits for the GPU.
#ifndef OPENCMR2_GXM_TEXTURES_H
#define OPENCMR2_GXM_TEXTURES_H

#include "render/gfx_internal.h"
#include "render/gxm_deferred_free.h"
#include "render/gxm_memory.h"
#include "render/gxm_texture_layout.h"

#include <psp2/gxm.h>

#include <stdint.h>

struct GxmTexture {
    SceGxmTexture texture;
    void *memory = nullptr;         // from the texture pool
    GxmTextureLayout layout;
    uint32_t sampler = ~0u;         // the sampler state last set on texture (SamplerKey)
    // A render target: its colour surface is the texture's memory.
    SceGxmRenderTarget *target = nullptr;
    SceGxmColorSurface color;
    void *depthMemory = nullptr;    // from the static pool
    SceGxmDepthStencilSurface depth;
};

class GxmTextures {
public:
    explicit GxmTextures(GxmMemory &memory) : memory(memory) {}
    GxmTextures(const GxmTextures &) = delete;
    GxmTextures &operator=(const GxmTextures &) = delete;
    // The GPU must be done: the executor waits for it first.
    ~GxmTextures();

    // The GPU texture of t, made on first use. nullptr when t cannot be on the
    // GPU (cube maps for now, a full pool).
    GxmTexture *Ensure(GfxTexture *t);
    // Copies the CPU levels of the dirty textures in. A staging texture no
    // draw has used stays on the CPU.
    void Upload(std::vector<GfxTexture *> &textures);
    // frame: the number of the frame being recorded.
    void Destroy(GfxTexture *t, unsigned frame);
    // At the start of recording frame: frees what the GPU is done with.
    void Collect(unsigned frame);
    // A render target's pixels into its CPU copy. The GPU must be done.
    void Read(GfxTexture *t);
    // GFX_FILTER_*, GFX_MIPFILTER_*, GFX_ADDRESS_*: set on the texture when
    // they differ from the last set.
    void SetSampler(GxmTexture &g, int minFilter, int magFilter, int mipFilter, int address);

    // Whether a texture was made or destroyed since the last call.
    bool TakeChanged();
    // "gxm tex: <format> <count> <KB>, ...": the live textures by format.
    void Log() const;

private:
    bool CreateTarget(const GfxTexture *t, GxmTexture &g);
    void Release(GxmTexture *g);

    GxmMemory &memory;
    GxmDeferredFree deferred;
    unsigned count[5] = {}, bytes[5] = {};   // live, by GFX_FORMAT_*
    bool changed = false;
    bool loggedCube = false, loggedFull = false;
};

#endif
