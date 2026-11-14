// The GPU side of the renderer's textures on the PS Vita (gxm_textures.h).

#include "render/gxm_textures.h"

#include "port/sys.h"
#include "render/gxm_alpha.h"
#include "render/texture_mips.h"

#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <string.h>

namespace {

constexpr int kFormats = 5;     // GFX_FORMAT_BGRX8 .. GFX_FORMAT_BUMP_DUDVL
const char *const kFormatNames[kFormats] = { "BGRX8", "BGRA8", "BC1", "BC3", "BUMP" };
// The CPU copy is BGRA bytes: a little-endian ARGB word. The bump format is
// converted to R, G, B, A bytes (dU, dV, L, 255), as the SDL GPU executor does.
const SceGxmTextureFormat kGxmFormats[kFormats] = {
    SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB, SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR,
    SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
};
constexpr unsigned kTile = 32;  // SCE_GXM_TILE_SIZEX, SCE_GXM_TILE_SIZEY

unsigned Align(unsigned value, unsigned step)
{
    return (value + step - 1) / step * step;
}

bool Check(const char *call, int err)
{
    if (err < 0)
        Sys_Log("gxm tex: %s: 0x%08x", call, (unsigned)err);
    return err >= 0;
}

// The texels x0..x1-1 of the rows y0..y1-1 of a level of a linear layout,
// from the CPU copy into the GPU layout.
void CopyLinearPart(int format, const uint8_t *src, const GxmLevelLayout &l, GfxMipRect part, uint8_t *dst)
{
    const unsigned sourceTexel = format == GFX_FORMAT_BUMP_DUDVL ? 3 : 4;
    for (int y = part.y0; y < part.y1 && (unsigned)y < l.rows; y++) {
        const uint8_t *in = src + (size_t)y * l.sourceRowBytes + (size_t)part.x0 * sourceTexel;
        uint8_t *out = dst + (size_t)y * l.rowBytes + (size_t)part.x0 * 4;
        const unsigned texels = (unsigned)(part.x1 - part.x0);
        if (format == GFX_FORMAT_BGRX8) {
            for (unsigned x = 0; x < texels; x++) {
                uint32_t p;
                memcpy(&p, in + x * 4, 4);
                p |= 0xff000000u;
                memcpy(out + x * 4, &p, 4);
            }
        } else if (format == GFX_FORMAT_BUMP_DUDVL) {
            for (unsigned x = 0; x < texels; x++) {
                out[x * 4] = in[x * 3];
                out[x * 4 + 1] = in[x * 3 + 1];
                out[x * 4 + 2] = in[x * 3 + 2];
                out[x * 4 + 3] = 0xff;
            }
        } else {
            memcpy(out, in, (size_t)texels * 4);
        }
    }
}

// One level of the CPU copy into its GPU layout.
void CopyLevel(int format, const uint8_t *src, const GxmTextureLayout &layout, const GxmLevelLayout &l, uint8_t *dst)
{
    if (layout.swizzled) {
        const unsigned width = l.rowBytes / layout.blockBytes;
        for (unsigned y = 0; y < l.sourceRows; y++)
            for (unsigned x = 0; x < l.sourceRowBytes / layout.blockBytes; x++)
                memcpy(dst + (size_t)Gxm_SwizzledBlock(x, y, width, l.rows) * layout.blockBytes,
                       src + (size_t)y * l.sourceRowBytes + x * layout.blockBytes, layout.blockBytes);
        return;
    }
    GfxMipRect all;
    all.x1 = (int)(l.sourceRowBytes / (format == GFX_FORMAT_BUMP_DUDVL ? 3 : 4));
    all.y1 = (int)l.rows;
    CopyLinearPart(format, src, l, all, dst);
}

// Whether the texture's change is tiles of level 0 that can go to the GPU
// alone (GfxTexture::changedTiles): a linear layout of one face whose levels
// are those of the CPU copy, that is in memory.
bool PartUploadFits(const GfxTexture *t, const GxmTexture *g)
{
    return g->uploaded && !g->layout.swizzled && g->layout.faceBytes == 0 && !g->madeMips && g->target == nullptr &&
           !t->changedRect.Empty() && !t->changedTiles.Empty() && !t->levels[0][0].empty();
}

// The marked tiles of level 0 of the CPU copy into memory in the layout of g.
void CopyTiles(const GfxTexture *t, const GxmTexture *g, const GfxTileSet &tiles, uint8_t *memory)
{
    const GxmLevelLayout &l0 = g->layout.level[0];
    tiles.ForEachRun([&](GfxMipRect r) { CopyLinearPart(t->format, t->levels[0][0].data(), l0, r, memory + l0.offset); });
}

// Writes all levels of the CPU copy into memory in the layout of g.
// Returns the least alpha of them (0: not known).
int CopyAllLevels(const GfxTexture *t, const GxmTexture *g, const std::vector<uint8_t> *mips, uint8_t *memory)
{
    const int faces = g->layout.faceBytes != 0 ? 6 : 1;
    int minAlpha = g->target == nullptr ? 255 : 0;
    for (int face = 0; face < faces; face++)
        for (int m = 0; m < g->layout.levels; m++) {
            const GxmLevelLayout &l = g->layout.level[m];
            const size_t bytes = (size_t)l.sourceRowBytes * l.sourceRows;
            const std::vector<uint8_t> &source = m < t->mipLevels || mips == nullptr ? t->levels[face][m] : mips[m];
            if (source.size() < bytes) {
                minAlpha = 0;
                continue;
            }
            CopyLevel(t->format, source.data(), g->layout, l, memory + (size_t)face * g->layout.faceBytes + l.offset);
            if (minAlpha > 0) {
                const int a = Gxm_MinAlpha(t->format, source.data(), bytes);
                minAlpha = a < minAlpha ? a : minAlpha;
            }
        }
    return minAlpha;
}

// The least alpha of a part of level 0 of the CPU copy (255 for a format
// without alpha).
int MinAlphaOfPart(const GfxTexture *t, const GxmLevelLayout &l, GfxMipRect part)
{
    int least = 255;
    for (int y = part.y0; y < part.y1 && least > 0; y++) {
        const uint8_t *row = t->levels[0][0].data() + (size_t)y * l.sourceRowBytes + (size_t)part.x0 * 4;
        const int a = Gxm_MinAlpha(t->format, row, (size_t)(part.x1 - part.x0) * 4);
        least = a < least ? a : least;
    }
    return least;
}

// The blocks x0, y0 .. x1 - 1, y1 - 1 of a level of a swizzled texture of
// blocks, from the CPU copy into the GPU layout. Returns the least alpha of
// them.
int CopyLevelBlocks(int format, const uint8_t *src, const GxmTextureLayout &layout, const GxmLevelLayout &l,
                    GfxMipRect blocks, uint8_t *dst)
{
    const unsigned width = l.rowBytes / layout.blockBytes;
    int minAlpha = 255;
    for (int y = blocks.y0; y < blocks.y1; y++) {
        const uint8_t *row = src + (size_t)y * l.sourceRowBytes + (size_t)blocks.x0 * layout.blockBytes;
        for (int x = blocks.x0; x < blocks.x1; x++)
            memcpy(dst + (size_t)Gxm_SwizzledBlock((unsigned)x, (unsigned)y, width, l.rows) * layout.blockBytes,
                   row + (size_t)(x - blocks.x0) * layout.blockBytes, layout.blockBytes);
        const int a = Gxm_MinAlpha(format, row, (size_t)(blocks.x1 - blocks.x0) * layout.blockBytes);
        minAlpha = a < minAlpha ? a : minAlpha;
    }
    return minAlpha;
}

uint32_t SamplerKey(int minFilter, int magFilter, int mipFilter, int address)
{
    return (uint32_t)(minFilter & 3) | (uint32_t)(magFilter & 3) << 2 | (uint32_t)(mipFilter & 3) << 4 |
           (uint32_t)(address & 3) << 6;
}

} // namespace

GxmTextures::~GxmTextures()
{
    for (const GxmDeferredFree::Entry &e : deferred.entries)
        Release((GxmTexture *)e.memory);
    for (const GxmDeferredFree::Entry &e : deferredMemory.entries)
        memory.Textures().Free(e.memory);
}

// The texture descriptor of g's layout over its memory.
int InitTexture(GxmTexture &g, const GfxTexture *t)
{
    const SceGxmTextureFormat format = kGxmFormats[t->format];
    if (g.layout.faceBytes != 0)
        return sceGxmTextureInitCube(&g.texture, g.memory, format, t->width, t->height, g.layout.levels);
    if (!g.layout.swizzled)
        return sceGxmTextureInitLinear(&g.texture, g.memory, format, t->width, t->height, g.layout.levels);
    if (Gxm_PowerOfTwo(t->width) == (unsigned)t->width && Gxm_PowerOfTwo(t->height) == (unsigned)t->height)
        return sceGxmTextureInitSwizzled(&g.texture, g.memory, format, t->width, t->height, g.layout.levels);
    return sceGxmTextureInitSwizzledArbitrary(&g.texture, g.memory, format, t->width, t->height,
                                              g.layout.levels);
}

GxmTexture *GxmTextures::Ensure(GfxTexture *t)
{
    if (t->gpu != nullptr) {
        GxmTexture *g = (GxmTexture *)t->gpu;
        return g->failed ? nullptr : g;
    }
    if (t->format < 0 || t->format >= kFormats)
        return nullptr;
    GxmTextureLayout layout;
    // A BC3 texture of one level gets mip levels made here: not a render
    // target, a cube or a texture that changes each frame, at least 8 texels
    // on each side, and a power of two on each side (the levels of a
    // swizzled texture of another size are not known to sit as
    // Gxm_TextureLayout puts them).
    const bool mips = makeMips && t->format == GFX_FORMAT_BC3 && t->mipLevels == 1 &&
                      !(t->flags & (GFX_TEXTURE_RENDER_TARGET | GFX_TEXTURE_CUBE | GFX_TEXTURE_DYNAMIC |
                                    GFX_TEXTURE_STAGING)) &&
                      t->width >= 8 && t->height >= 8 && Gxm_PowerOfTwo(t->width) == (unsigned)t->width &&
                      Gxm_PowerOfTwo(t->height) == (unsigned)t->height;
    const int levels = mips ? Gfx_MipLevelCount(t->width, t->height, t->mipLimit) : t->mipLevels;
    if (!(t->flags & GFX_TEXTURE_CUBE)) {
        layout = Gxm_TextureLayout(t->format, t->width, t->height, levels);
    } else if (t->width != t->height || Gfx_BytesPerPixel(t->format) != 4 || !(t->flags & GFX_TEXTURE_RENDER_TARGET) ||
               !Gxm_CubeLayout(t->width, t->mipLevels, &layout)) {
        // The game's cube maps are the car reflections: square 32-bit render
        // targets of one level.
        if (!loggedCube)
            Sys_Log("gxm tex: a cube map of %dx%d, %d levels, format %d is not on the GPU", t->width, t->height,
                    t->mipLevels, t->format);
        loggedCube = true;
        return nullptr;
    }
    GxmTexture *g = new GxmTexture;
    g->layout = layout;
    g->madeMips = layout.levels > t->mipLevels;
    if (g->madeMips && t->mipLimit > 0)
        g->keptMips.reset(new std::vector<uint8_t>[16]);
    g->memory = memory.Textures().Alloc(g->layout.bytes);
    if (g->memory == nullptr) {
        // Kept as failed: each new try would ask the system for a memory
        // block again, for each draw.
        if (!loggedFull)
            Sys_Log("gxm tex: no room for %dx%d (%u KB)", t->width, t->height, g->layout.bytes / 1024);
        loggedFull = true;
        g->failed = true;
        t->gpu = g;
        return nullptr;
    }
    const int err = InitTexture(*g, t);
    const bool cube = (t->flags & GFX_TEXTURE_CUBE) != 0;
    if (err < 0 ||
        ((t->flags & GFX_TEXTURE_RENDER_TARGET) && !(cube ? CreateCubeTarget(t, *g) : CreateTarget(t, *g)))) {
        // Kept as failed, so that it is not tried (and logged) again each frame.
        if (err < 0)
            Sys_Log("gxm tex: %s %dx%d, %d levels, %s: 0x%08x", kFormatNames[t->format], t->width, t->height,
                    g->layout.levels, cube ? "cube" : g->layout.swizzled ? "swizzled" : "linear", (unsigned)err);
        if (g->target != nullptr)
            sceGxmDestroyRenderTarget(g->target);
        g->target = nullptr;
        memory.Textures().Free(g->depthMemory);
        memory.Textures().Free(g->memory);
        g->depthMemory = g->memory = nullptr;
        g->failed = true;
        t->gpu = g;
        return nullptr;
    }
    count[t->format]++;
    bytes[t->format] += g->layout.bytes;
    changed = true;
    t->gpu = g;
    t->dirty = true;
    return g;
}

bool GxmTextures::CreateTarget(const GfxTexture *t, GxmTexture &g)
{
    const unsigned stride = g.layout.level[0].rowBytes / 4;
    if (!Check("sceGxmColorSurfaceInit",
               sceGxmColorSurfaceInit(&g.color, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB, SCE_GXM_COLOR_SURFACE_LINEAR,
                                      SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, t->width,
                                      t->height, stride, g.memory)))
        return false;
    // The GPU writes the depth: from the texture pool, which it may write
    // (it only reads the static pool).
    const unsigned depthWidth = Align(t->width, kTile);
    g.depthMemory = memory.Textures().Alloc(depthWidth * Align(t->height, kTile) * 4);
    if (g.depthMemory == nullptr ||
        !Check("sceGxmDepthStencilSurfaceInit",
               sceGxmDepthStencilSurfaceInit(&g.depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                             SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, depthWidth, g.depthMemory, nullptr)))
        return false;
    SceGxmRenderTargetParams params{};
    params.width = t->width;
    params.height = t->height;
    params.scenesPerFrame = 1;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    return Check("sceGxmCreateRenderTarget", sceGxmCreateRenderTarget(&params, &g.target));
}

// A cube's render target: one render target and one depth buffer, the size
// of a face, and a swizzled colour surface on each face. A frame draws the six
// faces as six scenes.
bool GxmTextures::CreateCubeTarget(const GfxTexture *t, GxmTexture &g)
{
    g.faces.reset(new SceGxmColorSurface[6]);
    for (int face = 0; face < 6; face++)
        if (!Check("sceGxmColorSurfaceInit(cube face)",
                   sceGxmColorSurfaceInit(&g.faces[face], SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB,
                                          SCE_GXM_COLOR_SURFACE_SWIZZLED, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                          SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, t->width, t->height, t->width,
                                          (uint8_t *)g.memory + (size_t)face * g.layout.faceBytes)))
            return false;
    const unsigned depthWidth = Align(t->width, kTile);
    g.depthMemory = memory.Textures().Alloc(depthWidth * Align(t->height, kTile) * 4);
    if (g.depthMemory == nullptr ||
        !Check("sceGxmDepthStencilSurfaceInit(cube)",
               sceGxmDepthStencilSurfaceInit(&g.depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                             SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, depthWidth, g.depthMemory, nullptr)))
        return false;
    SceGxmRenderTargetParams params{};
    params.width = t->width;
    params.height = t->height;
    params.scenesPerFrame = 6;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    return Check("sceGxmCreateRenderTarget(cube)", sceGxmCreateRenderTarget(&params, &g.target));
}

void GxmTextures::Upload(std::vector<GfxTexture *> &textures, unsigned frame)
{
    for (GfxTexture *t : textures) {
        if (!t->dirty)
            continue;
        t->dirty = false;
        if (t->gpu == nullptr && (t->flags & GFX_TEXTURE_STAGING))
            continue;
        GxmTexture *g = Ensure(t);
        t->dirty = false;
        if (g == nullptr)
            continue;
        // A texture of a linear layout that changed in a part (the dirt
        // stamps of the game on a car): the tiles of the stamps alone.
        if (PartUploadFits(t, g) && UploadPart(t, g, frame))
            continue;
        // A texture that keeps its levels and changed in a part (a texture
        // that is new in the atlas): only the blocks of that part go to the
        // GPU, into the memory that is there. The whole atlas of 2,048
        // texels, into new memory, took 285 ms of a frame of a race. No
        // frame that the GPU still draws reads the blocks of a texture that
        // is new there. A part that is all of the texture (the atlas starts
        // again) takes the whole way.
        const GfxMipRect part = Gfx_MipPartOf(t->changedRect, t->width, t->height, g->layout.levels);
        const bool partOnly = g->keptMips != nullptr && g->uploaded && g->layout.swizzled && g->layout.faceBytes == 0 &&
                              !t->changedRect.Empty() &&
                              !(part.x0 == 0 && part.y0 == 0 && part.x1 == t->width && part.y1 == t->height) &&
                              !t->levels[0][0].empty();
        if (partOnly) {
            const uint64_t start = sceKernelGetProcessTimeWide();
            Gfx_MakeBC3Mips(t->levels[0][0].data(), t->width, t->height, g->layout.levels, t->changedRect,
                            g->keptMips.get());
            mipMicros += sceKernelGetProcessTimeWide() - start;
            mipTextures++;
            t->changedRect = GfxMipRect();
            t->changedTiles.Clear();
            int minAlpha = g->minAlpha;
            for (int m = 0; m < g->layout.levels; m++) {
                const GxmLevelLayout &l = g->layout.level[m];
                const std::vector<uint8_t> &source = m < t->mipLevels ? t->levels[0][m] : g->keptMips[m];
                if (source.size() < (size_t)l.sourceRowBytes * l.sourceRows) {
                    minAlpha = 0;
                    continue;
                }
                const int a = CopyLevelBlocks(t->format, source.data(), g->layout, l,
                                              Gfx_MipPartBlocks(part, t->width, t->height, m),
                                              (uint8_t *)g->memory + l.offset);
                minAlpha = a < minAlpha ? a : minAlpha;
            }
            g->minAlpha = (uint8_t)minAlpha;
            partUploads++;
            continue;
        }
        wholeUploads++;
        g->copies.ChangedWhole();
        if (g->lastDrawn != ~0u && frame - g->lastDrawn < (unsigned)kGxmFrameSlots && g->target == nullptr) {
            void *fresh = memory.Textures().Alloc(g->layout.bytes);
            if (fresh != nullptr) {
                deferredMemory.Add(g->memory, frame);
                g->memory = fresh;
                sceGxmTextureSetData(&g->texture, fresh);
                g->generation++;
            }
        }
        // The mip levels made here (Ensure), from level 0: those of the
        // changed part of a texture that keeps them, else all.
        std::vector<uint8_t> made[16];
        std::vector<uint8_t> *mips = nullptr;
        if (g->madeMips && !t->levels[0][0].empty()) {
            const uint64_t start = sceKernelGetProcessTimeWide();
            mips = g->keptMips ? g->keptMips.get() : made;
            Gfx_MakeBC3Mips(t->levels[0][0].data(), t->width, t->height, g->layout.levels,
                            g->keptMips ? t->changedRect : GfxMipRect(), mips);
            mipMicros += sceKernelGetProcessTimeWide() - start;
            mipTextures++;
        }
        t->changedRect = GfxMipRect();
        t->changedTiles.Clear();
        // The least alpha of the levels goes with the upload: the executor
        // leaves out work where no texel can fail the alpha test. A render
        // target's is not known.
        g->minAlpha = (uint8_t)CopyAllLevels(t, g, mips, (uint8_t *)g->memory);
        g->uploaded = true;
    }
}

bool GxmTextures::UploadPart(GfxTexture *t, GxmTexture *g, unsigned frame)
{
    const GfxTileSet &part = t->changedTiles;
    const GxmLevelLayout &l0 = g->layout.level[0];
    // The memory that is drawn, when no frame on the GPU reads it: the tiles
    // go there. The spares miss them.
    uint8_t *target = nullptr;
    if (g->lastDrawn == GxmTextureCopies::kNotDrawn || frame - g->lastDrawn >= (unsigned)kGxmFrameSlots) {
        target = (uint8_t *)g->memory;
        CopyTiles(t, g, part, target);
        tileTexels += part.Texels();
        g->copies.Changed(part);
    } else {
        // Else a spare that no frame reads, with what it misses.
        int spare = g->copies.Free(frame);
        if (spare < 0) {
            void *fresh = memory.Textures().Alloc(g->layout.bytes);
            if (fresh == nullptr)
                return false;
            if (!g->copies.Add(fresh)) {
                memory.Textures().Free(fresh);
                return false;
            }
            spare = (int)g->copies.spares.size() - 1;
        }
        g->copies.Changed(part);
        GxmTextureCopies::Spare &s = g->copies.spares[(size_t)spare];
        target = (uint8_t *)s.memory;
        if (s.whole) {
            CopyAllLevels(t, g, nullptr, target);
        } else {
            CopyTiles(t, g, s.stale, target);
            tileTexels += s.stale.Texels();
        }
        g->memory = g->copies.Swap(spare, g->memory, g->lastDrawn, part);
        sceGxmTextureSetData(&g->texture, g->memory);
        g->generation++;
        g->lastDrawn = GxmTextureCopies::kNotDrawn;
    }
    // The least alpha stays a lower bound: that of the tiles, and that of the rest.
    if (g->minAlpha > 0)
        part.ForEachRun([&](GfxMipRect r) {
            const int a = MinAlphaOfPart(t, l0, r);
            g->minAlpha = (uint8_t)(a < g->minAlpha ? a : g->minAlpha);
        });
    t->changedRect = GfxMipRect();
    t->changedTiles.Clear();
    partUploads++;
    return true;
}

void GxmTextures::Destroy(GfxTexture *t, unsigned frame)
{
    GxmTexture *g = (GxmTexture *)t->gpu;
    if (g == nullptr)
        return;
    t->gpu = nullptr;
    if (g->failed) {
        delete g;
        return;
    }
    count[t->format]--;
    bytes[t->format] -= g->layout.bytes;
    changed = true;
    deferred.Add(g, frame);
}

void GxmTextures::Collect(unsigned frame)
{
    deferred.Collect(frame, [this](void *g) { Release((GxmTexture *)g); });
    deferredMemory.Collect(frame, [this](void *block) { memory.Textures().Free(block); });
}

void GxmTextures::Release(GxmTexture *g)
{
    if (g->target != nullptr)
        sceGxmDestroyRenderTarget(g->target);
    memory.Textures().Free(g->depthMemory);
    memory.Textures().Free(g->memory);
    for (const GxmTextureCopies::Spare &s : g->copies.spares)
        memory.Textures().Free(s.memory);
    delete g;
}

void GxmTextures::Read(GfxTexture *t)
{
    GxmTexture *g = (GxmTexture *)t->gpu;
    // A cube's faces are swizzled: the game does not lock them.
    if (g == nullptr || g->failed || g->layout.faceBytes != 0 ||
        (t->format != GFX_FORMAT_BGRX8 && t->format != GFX_FORMAT_BGRA8))
        return;
    const GxmLevelLayout &l = g->layout.level[0];
    std::vector<uint8_t> &level = t->levels[0][0];
    if (level.size() < (size_t)l.sourceRowBytes * l.rows)
        return;
    for (unsigned y = 0; y < l.rows; y++)
        memcpy(level.data() + (size_t)y * l.sourceRowBytes, (const uint8_t *)g->memory + (size_t)y * l.rowBytes,
               l.sourceRowBytes);
#ifdef OPENCMR2_DEV
    Sys_Log("gxm tex: read back %dx%d", t->width, t->height);
#endif
}

void GxmTextures::SetSampler(GxmTexture &g, int minFilter, int magFilter, int mipFilter, int address)
{
    const uint32_t key = SamplerKey(minFilter, magFilter, mipFilter, address);
    if (key == g.sampler)
        return;
    g.sampler = key;
    SceGxmTexture *t = &g.texture;
    sceGxmTextureSetMinFilter(t, minFilter == GFX_FILTER_LINEAR ? SCE_GXM_TEXTURE_FILTER_LINEAR
                                                                 : SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(t, magFilter == GFX_FILTER_LINEAR ? SCE_GXM_TEXTURE_FILTER_LINEAR
                                                                 : SCE_GXM_TEXTURE_FILTER_POINT);
    // GXM: ENABLED blends two levels, DISABLED takes the nearest. No mipmap
    // filter reads the first level only.
    sceGxmTextureSetMipFilter(t, mipFilter == GFX_MIPFILTER_LINEAR ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
                                                                   : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    sceGxmTextureSetMipmapCount(t, mipFilter == GFX_MIPFILTER_NONE ? 1 : g.layout.levels);
    // A cube is read by direction: Direct3D ignores its address mode.
    const SceGxmTextureAddrMode mode = address == GFX_ADDRESS_CLAMP || g.layout.faceBytes != 0
                                           ? SCE_GXM_TEXTURE_ADDR_CLAMP
                                       : address == GFX_ADDRESS_MIRROR ? SCE_GXM_TEXTURE_ADDR_MIRROR
                                                                       : SCE_GXM_TEXTURE_ADDR_REPEAT;
    sceGxmTextureSetUAddrMode(t, mode);
    sceGxmTextureSetVAddrMode(t, mode);
}

bool GxmTextures::TakeChanged()
{
    const bool was = changed;
    changed = false;
    return was;
}

void GxmTextures::Log()
{
    char line[256];
    int n = 0;
    for (int f = 0; f < kFormats; f++)
        n += snprintf(line + n, sizeof(line) - n, "%s%s %u %u KB", f ? ", " : "", kFormatNames[f], count[f],
                      bytes[f] / 1024);
    Sys_Log("gxm tex: %s", line);
    if (mipTextures > 0)
        Sys_Log("gxm tex: mip levels made for %u textures in %.1f ms", mipTextures, mipMicros / 1000.0);
    mipTextures = 0;
    mipMicros = 0;
}
