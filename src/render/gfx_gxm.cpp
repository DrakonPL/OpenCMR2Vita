// The GXM executor (PS Vita): runs a recorded frame (gfx_core.cpp) on the
// GXM device (gxm_device.cpp). For now it shows a probe: a clear colour that
// changes each second, and draws nothing of the frame.

#include "platform/platform_vita.h"
#include "port/sys.h"
#include "render/gfx_internal.h"
#include "render/gxm_device.h"

#include <psp2/kernel/processmgr.h>

extern "C" {
extern const SceGxmProgram clear_v_gxp_start;
extern const SceGxmProgram clear_f_gxp_start;
}

namespace {

struct Executor : GfxExecutor {
    GxmDevice device;
    GxmMemory memory;   // after the device: freed before it
    bool ready = false;
    bool loggedInit = false;
    unsigned presented = 0;
    SceGxmShaderPatcherId clearVertexId = nullptr, clearFragmentId = nullptr;
    SceGxmVertexProgram *clearVertex = nullptr;
    SceGxmFragmentProgram *clearFragment = nullptr;
    const SceGxmProgramParameter *clearDepthParam = nullptr, *clearColorParam = nullptr;
    uint16_t *clearIndices = nullptr;

    ~Executor() override
    {
        if (!ready)
            return;
        device.Finish();
        sceGxmShaderPatcherReleaseFragmentProgram(device.Patcher(), clearFragment);
        sceGxmShaderPatcherReleaseVertexProgram(device.Patcher(), clearVertex);
        sceGxmShaderPatcherUnregisterProgram(device.Patcher(), clearFragmentId);
        sceGxmShaderPatcherUnregisterProgram(device.Patcher(), clearVertexId);
        memory.Static().Free(clearIndices);
    }

    static bool Check(const char *call, int err)
    {
        if (err < 0)
            Sys_Log("gxm: %s: 0x%08x", call, (unsigned)err);
        return err >= 0;
    }

    bool CreateClearProgram()
    {
        SceGxmShaderPatcher *patcher = device.Patcher();
        if (!Check("sceGxmProgramCheck(clear_v)", sceGxmProgramCheck(&clear_v_gxp_start)) ||
            !Check("sceGxmProgramCheck(clear_f)", sceGxmProgramCheck(&clear_f_gxp_start)) ||
            !Check("RegisterProgram(clear_v)",
                   sceGxmShaderPatcherRegisterProgram(patcher, &clear_v_gxp_start, &clearVertexId)) ||
            !Check("RegisterProgram(clear_f)",
                   sceGxmShaderPatcherRegisterProgram(patcher, &clear_f_gxp_start, &clearFragmentId)) ||
            !Check("CreateVertexProgram(clear_v)",
                   sceGxmShaderPatcherCreateVertexProgram(patcher, clearVertexId, nullptr, 0, nullptr, 0,
                                                          &clearVertex)) ||
            !Check("CreateFragmentProgram(clear_f)",
                   sceGxmShaderPatcherCreateFragmentProgram(patcher, clearFragmentId,
                                                            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                            SCE_GXM_MULTISAMPLE_NONE, nullptr, &clear_v_gxp_start,
                                                            &clearFragment)))
            return false;
        clearDepthParam = sceGxmProgramFindParameterByName(&clear_v_gxp_start, "clearDepth");
        clearColorParam = sceGxmProgramFindParameterByName(&clear_f_gxp_start, "clearColor");
        if (clearDepthParam == nullptr || clearColorParam == nullptr) {
            Sys_Log("gxm: the clear programs lack clearDepth or clearColor");
            return false;
        }
        clearIndices = (uint16_t *)memory.Static().Alloc(3 * sizeof(uint16_t));
        if (clearIndices == nullptr)
            return false;
        clearIndices[0] = 0;
        clearIndices[1] = 1;
        clearIndices[2] = 2;
        return true;
    }

    bool SetVideoMode(int width, int height, bool) override
    {
        if (!ready)
            ready = device.Init() && memory.Init() && CreateClearProgram();
        return ready && device.SetMode(width, height);
    }

    void SetFullscreen(bool) override {}

    // The probe: the whole back buffer in one colour, a new one each second.
    void DrawProbe()
    {
        static const float kColors[4][4] = {
            { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }
        };
        const float *color = kColors[(sceKernelGetProcessTimeWide() / 1000000) % 4];
        SceGxmContext *context = device.Context();
        const float w = (float)device.Width(), h = (float)device.Height();
        sceGxmSetViewport(context, w * 0.5f, w * 0.5f, h * 0.5f, -h * 0.5f, 0.0f, 1.0f);
        sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_ENABLED);
        sceGxmSetVertexProgram(context, clearVertex);
        sceGxmSetFragmentProgram(context, clearFragment);
        void *uniforms;
        const float depth[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, clearDepthParam, 0, 4, depth);
        sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, clearColorParam, 0, 4, color);
        Check("sceGxmDraw", sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                                       clearIndices, 3));
    }

    void Execute(GfxFrame &, std::vector<GfxTexture *> &textures, GfxReadback &readback, bool present, bool vsync,
                 bool) override
    {
        for (GfxTexture *t : textures)
            t->dirty = false;
        if (!present || !ready)
            return;
        if (readback.requested) {
            readback.pixels.assign((size_t)readback.width * readback.height, 0xff000000);
            readback.readyWidth = readback.width;
            readback.readyHeight = readback.height;
            readback.ready = true;
            readback.requested = false;
        }
        if (!loggedInit) {
            Sys_Log("gxm: init ok %dx%d vsync %d, %d memblocks", device.Width(), device.Height(), vsync ? 1 : 0,
                    Gxm_LiveBlocks());
            loggedInit = true;
        }
        // Back from the LiveArea or from sleep: the GPU finishes what it had
        // before it gets more.
        if (Platform_TakeResumed())
            device.Finish();
        if (!device.BeginBackBufferScene())
            return;
        DrawProbe();
        device.EndScene();
#ifdef OPENCMR2_DEV
        if (presented < 6)
            Sys_Log("gxm: frame %u slot %d", presented, device.FrameSlot());
#endif
        device.Present(vsync);
        presented++;
    }

    void DestroyTexture(GfxTexture *) override {}
    void ReadTexture(GfxTexture *) override {}
};

} // namespace

GfxExecutor *Gfx_CreateGxmExecutor(void)
{
    return new Executor;
}
