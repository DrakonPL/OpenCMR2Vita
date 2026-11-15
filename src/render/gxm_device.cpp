// The GXM device of the renderer on the PS Vita (gxm_device.h).

#include "render/gxm_device.h"
#include "render/frame_pacing.h"

#include "port/sys.h"
#include "render/gxm_dialog.h"
#include "render/gxm_readback.h"

#include <psp2/common_dialog.h>

#include <psp2/display.h>

#include <stdlib.h>
#include <string.h>

namespace {

constexpr int kDisplayStride = 1024;    // pixels: a multiple of 64, as the display needs
constexpr unsigned kDisplayBytes = kDisplayStride * GxmDevice::kDisplayHeight * 4;
// The back buffer is one scene, or two around the backdrop copy, and more
// when the game reads a render target in the middle of a frame.
constexpr unsigned kScenesPerFrame = 4;
// The patcher's memory: its programs (one for each vertex program, and for
// each fragment variant with each blend pair it draws with).
constexpr unsigned kPatcherBufferBytes = 256 * 1024;
constexpr unsigned kPatcherVertexUsseBytes = 64 * 1024;
constexpr unsigned kPatcherFragmentUsseBytes = 512 * 1024;

const SceGxmMemoryAttribFlags kRead = SCE_GXM_MEMORY_ATTRIB_READ;
const SceGxmMemoryAttribFlags kReadWrite = (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);

// What the display queue hands the callback for a flip.
struct FlipData {
    void *address;
    int vsync;              // the blanks after the frame before (0: at once)
    int width, height;      // the frame buffer size the display gets
};

// The blank count at which the last frame showed (the display queue's
// thread).
unsigned s_shownBlank;

// Runs on the display queue's thread: no log here.
void Flip(const void *callbackData)
{
    const FlipData *flip = (const FlipData *)callbackData;
    SceDisplayFrameBuf frame{};
    frame.size = sizeof(frame);
    frame.base = flip->address;
    frame.pitch = kDisplayStride;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = flip->width;
    frame.height = flip->height;
    if (flip->vsync) {
        // Two blanks for each frame: the frame before stays until then.
        while (Gfx_BlanksBeforeFlip((unsigned)sceDisplayGetVcount(), s_shownBlank, flip->vsync) > 0)
            sceDisplayWaitVblankStart();
        sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
        sceDisplayWaitVblankStart();
        s_shownBlank = (unsigned)sceDisplayGetVcount();
    } else if (sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_IMMEDIATE) < 0) {
        // The display refuses the line timing until it had a vsync flip.
        sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    }
}

void *PatcherAlloc(void *, unsigned int size)
{
    return malloc(size);
}

void PatcherFree(void *, void *memory)
{
    free(memory);
}

bool Check(const char *call, int err)
{
    if (err < 0)
        Sys_Log("gxm: %s: 0x%08x", call, (unsigned)err);
    return err >= 0;
}

unsigned Align(unsigned value, unsigned step)
{
    return (value + step - 1) & ~(step - 1);
}

} // namespace

GxmDevice::~GxmDevice()
{
    Shutdown();
}

bool GxmDevice::Init()
{
    // The frames in flight ([video] frames_in_flight, 2 or 3): the display
    // buffers, the frame ring's slots and the waits before memory is reused.
    g_gxmFrameSlots = Sys_GetOption("video.frames_in_flight", 2) >= 3 ? kGxmMaxFrameSlots : kGxmFrameSlots;
    Sys_Log("gxm: %d frames in flight (%d display buffers)", g_gxmFrameSlots - 1, g_gxmFrameSlots);
    SceGxmInitializeParams params{};
    params.displayQueueMaxPendingCount = g_gxmFrameSlots - 1;
    params.displayQueueCallback = Flip;
    params.displayQueueCallbackDataSize = sizeof(FlipData);
    params.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if (!Check("sceGxmInitialize", sceGxmInitialize(&params)))
        return false;
    initialized = true;
    return CreateContext() && CreateDisplay() && CreatePatcher() && SetMode(kDisplayWidth, kDisplayHeight);
}

bool GxmDevice::CreateContext()
{
    if (!Gxm_AllocGpu(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, kRead, vdmRing) ||
        !Gxm_AllocGpu(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, kRead,
                      vertexRing) ||
        !Gxm_AllocGpu(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, kRead,
                      fragmentRing) ||
        !Gxm_AllocFragmentUsse(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, fragmentUsseRing))
        return false;
    contextHost.reset(new unsigned char[SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE]);
    SceGxmContextParams params{};
    params.hostMem = contextHost.get();
    params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    params.vdmRingBufferMem = vdmRing.data;
    params.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    params.vertexRingBufferMem = vertexRing.data;
    params.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    params.fragmentRingBufferMem = fragmentRing.data;
    params.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferMem = fragmentUsseRing.data;
    params.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferOffset = fragmentUsseRing.usseOffset;
    return Check("sceGxmCreateContext", sceGxmCreateContext(&params, &context));
}

bool GxmDevice::CreateDisplay()
{
    for (int i = 0; i < g_gxmFrameSlots; i++) {
        DisplayBuffer &d = display[i];
        if (!Gxm_AllocGpu(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, kDisplayBytes, kReadWrite, d.memory) ||
            !Check("sceGxmSyncObjectCreate", sceGxmSyncObjectCreate(&d.sync)))
            return false;
        memset(d.memory.data, 0, kDisplayBytes);
    }
    // The display refuses a flip without vsync until it had one with: show
    // the first (black) buffer so.
    FlipData first = { display[front].memory.data, 1, kDisplayWidth, kDisplayHeight };
    Flip(&first);
    sceDisplayWaitSetFrameBuf();
    back = 1;
    // The largest back buffer's depth; smaller ones use the start of it.
    const unsigned samples = Align(kDisplayWidth, SCE_GXM_TILE_SIZEX) * Align(kDisplayHeight, SCE_GXM_TILE_SIZEY);
    return Gxm_AllocGpu(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, samples * 4, kReadWrite, depthMemory);
}

bool GxmDevice::CreatePatcher()
{
    if (!Gxm_AllocGpu(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, kPatcherBufferBytes, kReadWrite, patcherBuffer) ||
        !Gxm_AllocVertexUsse(kPatcherVertexUsseBytes, patcherVertexUsse) ||
        !Gxm_AllocFragmentUsse(kPatcherFragmentUsseBytes, patcherFragmentUsse))
        return false;
    SceGxmShaderPatcherParams params{};
    params.hostAllocCallback = PatcherAlloc;
    params.hostFreeCallback = PatcherFree;
    params.bufferMem = patcherBuffer.data;
    params.bufferMemSize = patcherBuffer.size;
    params.vertexUsseMem = patcherVertexUsse.data;
    params.vertexUsseMemSize = patcherVertexUsse.size;
    params.vertexUsseOffset = patcherVertexUsse.usseOffset;
    params.fragmentUsseMem = patcherFragmentUsse.data;
    params.fragmentUsseMemSize = patcherFragmentUsse.size;
    params.fragmentUsseOffset = patcherFragmentUsse.usseOffset;
    return Check("sceGxmShaderPatcherCreate", sceGxmShaderPatcherCreate(&params, &patcher));
}

bool GxmDevice::CreateRenderTarget(int w, int h)
{
    SceGxmRenderTargetParams params{};
    params.width = w;
    params.height = h;
    params.scenesPerFrame = kScenesPerFrame;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    return Check("sceGxmCreateRenderTarget", sceGxmCreateRenderTarget(&params, &renderTarget));
}

bool GxmDevice::SetMode(int w, int h)
{
    if (w <= 0 || h <= 0 || w > kDisplayWidth || h > kDisplayHeight) {
        Sys_Log("gxm: mode %dx%d is larger than the display", w, h);
        return false;
    }
    if (renderTarget != nullptr && w == width && h == height)
        return true;
    Finish();
    if (renderTarget != nullptr) {
        sceGxmDestroyRenderTarget(renderTarget);
        renderTarget = nullptr;
    }
    if (!CreateRenderTarget(w, h))
        return false;
    // A smaller back buffer is the centre of the display, with a black
    // border. A size that the display scales starts at the buffer's first
    // pixel and fills the screen.
    const bool scaled = DisplayScales(w, h);
    const int x = scaled ? 0 : (kDisplayWidth - w) / 2, y = scaled ? 0 : (kDisplayHeight - h) / 2;
    // The buffers in use only: the others of the array have no memory.
    for (int i = 0; i < g_gxmFrameSlots; i++) {
        DisplayBuffer &d = display[i];
        memset(d.memory.data, 0, kDisplayBytes);
        unsigned char *origin = (unsigned char *)d.memory.data + ((size_t)y * kDisplayStride + x) * 4;
        d.origin = origin;
        if (!Check("sceGxmColorSurfaceInit",
                   sceGxmColorSurfaceInit(&d.surface, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                                          SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h,
                                          kDisplayStride, origin)))
            return false;
    }
    if (!Check("sceGxmDepthStencilSurfaceInit",
               sceGxmDepthStencilSurfaceInit(&depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                             SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, Align(w, SCE_GXM_TILE_SIZEX),
                                             depthMemory.data, nullptr)))
        return false;
    // A scene that does not load the depth starts with stencil 0: the
    // stencil of the near ground before the sky (gfx_sky_plan.h) needs it.
    sceGxmDepthStencilSurfaceSetBackgroundStencil(&depth, 0);
    // And depth 1, which the executor counts on to leave out a depth clear.
    sceGxmDepthStencilSurfaceSetBackgroundDepth(&depth, 1.0f);
    width = w;
    height = h;
    displayWidth = scaled ? w : kDisplayWidth;
    displayHeight = scaled ? h : kDisplayHeight;
    if (scaled)
        Sys_Log("gxm: mode %dx%d, scaled by the display to %dx%d", w, h, kDisplayWidth, kDisplayHeight);
    else
        Sys_Log("gxm: mode %dx%d in the %dx%d display", w, h, kDisplayWidth, kDisplayHeight);
    return true;
}

bool GxmDevice::BeginBackBufferScene(bool loadDepth, bool storeDepth)
{
    DisplayBuffer &d = display[back];
    // The colour surface is always loaded and stored. The depth is not,
    // unless a split scene asks for it: it costs a read or a write of the
    // whole depth buffer.
    sceGxmDepthStencilSurfaceSetForceLoadMode(&depth, loadDepth ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED
                                                                : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
    sceGxmDepthStencilSurfaceSetForceStoreMode(&depth, storeDepth ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
                                                                  : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
    return Check("sceGxmBeginScene",
                 sceGxmBeginScene(context, 0, renderTarget, nullptr, nullptr, d.sync, &d.surface, &depth));
}

void GxmDevice::EndScene(const SceGxmNotification *fragment)
{
    Check("sceGxmEndScene", sceGxmEndScene(context, nullptr, fragment));
}

bool GxmDevice::BackBufferTexture(SceGxmTexture *texture) const
{
    return Check("sceGxmTextureInitLinearStrided(back buffer)",
                 sceGxmTextureInitLinearStrided(texture, display[back].origin, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                                                width, height, kDisplayStride * 4));
}

void GxmDevice::ReadPixels(int buffer, int x, int y, int w, int h, uint32_t *pixels) const
{
    const unsigned char *origin = display[buffer].origin;
    for (int row = 0; row < h; row++)
        Gxm_PackReadbackRow((const uint32_t *)(origin + ((size_t)(y + row) * kDisplayStride + x) * 4),
                            pixels + (size_t)row * w, w);
}

void GxmDevice::Present(int vsync)
{
    QueueFlip(vsync, displayWidth, displayHeight);
}

void GxmDevice::QueueFlip(int vsync, int w, int h)
{
    FlipData flip = { display[back].memory.data, vsync, w, h };
    Check("sceGxmDisplayQueueAddEntry", sceGxmDisplayQueueAddEntry(display[front].sync, display[back].sync, &flip));
    front = back;
    back = (back + 1) % g_gxmFrameSlots;
    frameSlot = (frameSlot + 1) % g_gxmFrameSlots;
}

void GxmDevice::Finish()
{
    if (context != nullptr)
        sceGxmFinish(context);
}

void GxmDevice::DialogFrame()
{
    // The flips before are done: the buffer is not on the display.
    sceGxmDisplayQueueFinish();
    DisplayBuffer &d = display[back];
    memset(d.memory.data, 0, kDisplayBytes);
    SceCommonDialogUpdateParam update{};
    update.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
    update.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
    update.renderTarget.width = kDisplayWidth;
    update.renderTarget.height = kDisplayHeight;
    update.renderTarget.strideInPixels = kDisplayStride;
    update.renderTarget.colorSurfaceData = d.memory.data;
    update.displaySyncObject = d.sync;
    sceCommonDialogUpdate(&update);
    // A dialog is drawn at the screen's size, also in a scaled mode.
    QueueFlip(true, kDisplayWidth, kDisplayHeight);
}

namespace {
GxmDialogPresenter *s_dialogPresenter;
}

GxmDialogPresenter *Gxm_DialogPresenter(void)
{
    return s_dialogPresenter;
}

void Gxm_SetDialogPresenter(GxmDialogPresenter *presenter)
{
    s_dialogPresenter = presenter;
}

void GxmDevice::Shutdown()
{
    if (!initialized)
        return;
    Finish();
    sceGxmDisplayQueueFinish();
    if (patcher != nullptr)
        sceGxmShaderPatcherDestroy(patcher);
    Gxm_FreeFragmentUsse(patcherFragmentUsse);
    Gxm_FreeVertexUsse(patcherVertexUsse);
    Gxm_FreeGpu(patcherBuffer);
    if (renderTarget != nullptr)
        sceGxmDestroyRenderTarget(renderTarget);
    Gxm_FreeGpu(depthMemory);
    for (DisplayBuffer &d : display) {
        if (d.sync != nullptr)
            sceGxmSyncObjectDestroy(d.sync);
        Gxm_FreeGpu(d.memory);
    }
    if (context != nullptr)
        sceGxmDestroyContext(context);
    Gxm_FreeFragmentUsse(fragmentUsseRing);
    Gxm_FreeGpu(fragmentRing);
    Gxm_FreeGpu(vertexRing);
    Gxm_FreeGpu(vdmRing);
    sceGxmTerminate();
    initialized = false;
}
