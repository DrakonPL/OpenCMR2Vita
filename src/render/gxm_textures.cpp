// The GPU side of the renderer's textures on the PS Vita (gxm_textures.h).

#include "render/gxm_textures.h"

#include "port/sys.h"

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
    for (unsigned y = 0; y < l.rows; y++) {
        const uint8_t *in = src + (size_t)y * l.sourceRowBytes;
        uint8_t *out = dst + (size_t)y * l.rowBytes;
        if (format == GFX_FORMAT_BGRX8) {
            for (unsigned x = 0; x < l.sourceRowBytes / 4; x++) {
                uint32_t p;
                memcpy(&p, in + x * 4, 4);
                p |= 0xff000000u;
                memcpy(out + x * 4, &p, 4);
            }
        } else if (format == GFX_FORMAT_BUMP_DUDVL) {
            for (unsigned x = 0; x < l.sourceRowBytes / 3; x++) {
                out[x * 4] = in[x * 3];
                out[x * 4 + 1] = in[x * 3 + 1];
                out[x * 4 + 2] = in[x * 3 + 2];
                out[x * 4 + 3] = 0xff;
            }
        } else {
            memcpy(out, in, l.sourceRowBytes);
        }
    }
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
    if (!(t->flags & GFX_TEXTURE_CUBE)) {
        layout = Gxm_TextureLayout(t->format, t->width, t->height, t->mipLevels);
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
        if (g->lastDrawn != ~0u && frame - g->lastDrawn < (unsigned)kGxmFrameSlots && g->target == nullptr) {
            void *fresh = memory.Textures().Alloc(g->layout.bytes);
            if (fresh != nullptr) {
                deferredMemory.Add(g->memory, frame);
                g->memory = fresh;
                sceGxmTextureSetData(&g->texture, fresh);
                g->generation++;
            }
        }
        const int faces = g->layout.faceBytes != 0 ? 6 : 1;
        for (int face = 0; face < faces; face++)
            for (int m = 0; m < g->layout.levels; m++) {
                const GxmLevelLayout &l = g->layout.level[m];
                if (t->levels[face][m].size() < (size_t)l.sourceRowBytes * l.sourceRows)
                    continue;
                CopyLevel(t->format, t->levels[face][m].data(), g->layout, l,
                          (uint8_t *)g->memory + (size_t)face * g->layout.faceBytes + l.offset);
            }
    }
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

void GxmTextures::Log() const
{
    char line[256];
    int n = 0;
    for (int f = 0; f < kFormats; f++)
        n += snprintf(line + n, sizeof(line) - n, "%s%s %u %u KB", f ? ", " : "", kFormatNames[f], count[f],
                      bytes[f] / 1024);
    Sys_Log("gxm tex: %s", line);
}
