// The GXM device of the renderer on the PS Vita: GXM itself, the context, the
// display buffers, the depth buffer, the back buffer's render target, the
// display queue and the shader patcher. The executor (gfx_gxm.cpp) draws
// with it.
#ifndef OPENCMR2_GXM_DEVICE_H
#define OPENCMR2_GXM_DEVICE_H

#include "render/gxm_memory.h"

#include <psp2/gxm.h>

#include <memory>

class GxmDevice {
public:
    // The display's size; a smaller back buffer is centred in it.
    static constexpr int kDisplayWidth = 960;
    static constexpr int kDisplayHeight = 544;
    // Frames in flight: the CPU records one, the GPU draws one, the display
    // shows one. Rings of per-frame data have one slot for each.
    static constexpr int kFrameSlots = 3;

    GxmDevice() = default;
    GxmDevice(const GxmDevice &) = delete;
    GxmDevice &operator=(const GxmDevice &) = delete;
    ~GxmDevice();

    bool Init();
    // Sets the back buffer's size, at most the display's. Waits for the GPU.
    bool SetMode(int width, int height);
    int Width() const { return width; }
    int Height() const { return height; }

    SceGxmContext *Context() const { return context; }
    SceGxmShaderPatcher *Patcher() const { return patcher; }

    // A scene on the back buffer, with the depth buffer.
    bool BeginBackBufferScene();
    void EndScene();
    // Shows the back buffer: at the next vertical blank with vsync, at once
    // without. Then the next frame slot.
    void Present(bool vsync);
    // 0, 1, 2, ...: the slot of the frame being recorded.
    int FrameSlot() const { return frameSlot; }
    // Waits until the GPU has done all queued work.
    void Finish();

private:
    struct DisplayBuffer {
        GxmBlock memory;
        SceGxmSyncObject *sync = nullptr;
        SceGxmColorSurface surface;     // the back buffer's rectangle of it
    };

    bool CreateContext();
    bool CreateDisplay();
    bool CreatePatcher();
    bool CreateRenderTarget(int w, int h);
    void Shutdown();

    bool initialized = false;
    std::unique_ptr<unsigned char[]> contextHost;
    GxmBlock vdmRing, vertexRing, fragmentRing, fragmentUsseRing;
    SceGxmContext *context = nullptr;
    DisplayBuffer display[kFrameSlots];
    int back = 0, front = 0;
    GxmBlock depthMemory;
    SceGxmDepthStencilSurface depth;
    SceGxmRenderTarget *renderTarget = nullptr;
    GxmBlock patcherBuffer, patcherVertexUsse, patcherFragmentUsse;
    SceGxmShaderPatcher *patcher = nullptr;
    int width = 0, height = 0;
    int frameSlot = 0;
};

#endif
