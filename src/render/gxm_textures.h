// The GPU side of the renderer's textures on the PS Vita: the GxmTexture
// behind GfxTexture::gpu, its upload from the CPU copy, its sampler state,
// render targets, and the free that waits for the GPU.
#ifndef OPENCMR2_GXM_TEXTURES_H
#define OPENCMR2_GXM_TEXTURES_H

#include "render/gfx_internal.h"
#include "render/gxm_deferred_free.h"
#include "render/gxm_memory.h"
#include "render/gxm_texture_copies.h"
#include "render/gxm_texture_layout.h"

#include <psp2/gxm.h>

#include <stdint.h>

#include <memory>

struct GxmTexture {
    SceGxmTexture texture;
    void *memory = nullptr;         // from the texture pool
    GxmTextureLayout layout;
    uint32_t sampler = ~0u;         // the sampler state last set on texture (SamplerKey)
    unsigned generation = 0;        // counts the moves of the texture to other memory
    uint8_t minAlpha = 0;           // the least alpha of the levels uploaded (0: not known, or 0)
    unsigned lastDrawn = ~0u;       // the last frame that drew with it (~0u: none)
    bool failed = false;            // GXM refused it: never drawn, never tried again
    // The memory is a frame of a video decoder (GxmTextures::SetVideoFrame):
    // not from the pool, and the CPU copy is not uploaded.
    bool external = false;
    // A render target: its colour surface is the texture's memory.
    SceGxmRenderTarget *target = nullptr;
    SceGxmColorSurface color;
    // A cube's render target: a colour surface for each face (color unused).
    std::unique_ptr<SceGxmColorSurface[]> faces;
    void *depthMemory = nullptr;    // from the texture pool: the GPU writes it
    SceGxmDepthStencilSurface depth;
    // The mip levels made here (GxmTextures::SetMipmaps): the levels of the
    // layout past those of the CPU copy. A texture with a mip limit (the
    // atlas) keeps them, so that an upload makes those of its changed part
    // only (GfxTexture::changedRect).
    bool madeMips = false;
    std::unique_ptr<std::vector<uint8_t>[]> keptMips;
    bool uploaded = false;          // each level is in memory: a changed part can go there alone
    // The spare copies of a texture that changes in a part in each frame
    // (gxm_texture_copies.h), from the texture pool.
    GxmTextureCopies copies;
};

class GxmTextures {
public:
    explicit GxmTextures(GxmMemory &memory) : memory(memory) {}
    GxmTextures(const GxmTextures &) = delete;
    GxmTextures &operator=(const GxmTextures &) = delete;
    // The GPU must be done: the executor waits for it first.
    ~GxmTextures();

    // The GPU texture of t, made on first use. nullptr when t cannot be on the
    // GPU (a cube map that Gxm_CubeLayout refuses, a full pool).
    GxmTexture *Ensure(GfxTexture *t);
    // Copies the CPU levels of the dirty textures in. A staging texture no
    // draw has used stays on the CPU. A texture that a frame still on the GPU
    // may read gets new memory; the old goes back later. frame: the number
    // of the frame being recorded.
    void Upload(std::vector<GfxTexture *> &textures, unsigned frame);
    // For the logs: the uploads of a part of a texture (GfxTexture::changedRect
    // or changedTiles), the texels of the tiles that they wrote, and the
    // uploads of all of a texture.
    unsigned partUploads = 0, wholeUploads = 0;
    size_t tileTexels = 0;
    // frame: the number of the frame being recorded.
    void Destroy(GfxTexture *t, unsigned frame);
    // At the start of recording frame: frees what the GPU is done with.
    void Collect(unsigned frame);
    // A render target's pixels into its CPU copy. The GPU must be done.
    void Read(GfxTexture *t);
    // The texture of t over a frame of a video decoder: the luma plane, then
    // the chroma samples of each 2x2 block together (V first when vFirst),
    // in memory that is mapped for the GPU. The GPU must be done. A texture
    // that has memory of its own (made for an upload before its first
    // frame) gives it back. False when GXM refuses the frame. frame: the
    // number of the frame being recorded.
    bool SetVideoFrame(GfxTexture *t, const void *pixels, bool vFirst, unsigned frame);
    // GFX_FILTER_*, GFX_MIPFILTER_*, GFX_ADDRESS_*: set on the texture when
    // they differ from the last set.
    void SetSampler(GxmTexture &g, int minFilter, int magFilter, int mipFilter, int address);

    // Whether the BC3 textures of one level get mip levels on the GPU
    // (texture_mips.h): the scenery drawn small reads a small level. Before
    // the first texture.
    void SetMipmaps(bool on) { makeMips = on; }

    // Whether a texture was made or destroyed since the last call.
    bool TakeChanged();
    // "gxm tex: <format> <count> <KB>, ...": the live textures by format,
    // and the time the mip levels took since the line before.
    void Log();

private:
    // The rows of the changed part of level 0 (PartUploadFits), into the
    // memory that is drawn or into a spare copy. False: no memory for a spare.
    bool UploadPart(GfxTexture *t, GxmTexture *g, unsigned frame);
    bool CreateTarget(const GfxTexture *t, GxmTexture &g);
    bool CreateCubeTarget(const GfxTexture *t, GxmTexture &g);
    void Release(GxmTexture *g);

    GxmMemory &memory;
    GxmDeferredFree deferred;           // GxmTexture objects
    GxmDeferredFree deferredMemory;     // texture pool blocks replaced by an upload
    unsigned count[5] = {}, bytes[5] = {};   // live, by GFX_FORMAT_*
    bool changed = false;
    bool makeMips = false;
    uint64_t mipMicros = 0;         // making mip levels, since the last Log
    unsigned mipTextures = 0;
    bool loggedCube = false, loggedFull = false, loggedVideo = false;
};

#endif
