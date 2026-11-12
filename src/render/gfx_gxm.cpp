// The GXM executor (PS Vita): runs a recorded frame (gfx_core.cpp) on the GXM
// device (gxm_device.cpp), as gfx_gpu.cpp does on SDL_GPU. The fragment
// programs are the precompiled variants of render/shaders/cg (one per
// fragment key and alpha test form), patched per blend pair.

#include "platform/platform_vita.h"
#include "port/sys.h"
#include "render/gfx_executor_shared.h"
#include "render/gfx_internal.h"
#include "render/gxm_device.h"
#include "render/gxm_textures.h"
#include "render/gxm_viewport.h"
#include "render/shader_key.h"
#include "render/shader_variants_gen.h"
#include "render/shaders/cg/uniforms.h"

#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <string.h>

#include <unordered_map>

extern "C" {
extern const SceGxmProgram clear_v_gxp_start;
extern const SceGxmProgram clear_f_gxp_start;
extern const SceGxmProgram fixed3d_v_gxp_start;
extern const SceGxmProgram fixedtl_v_gxp_start;
}

namespace {

using namespace GfxShared;

constexpr int kVariantCount = (int)(sizeof(kGxmFragmentVariants) / sizeof(kGxmFragmentVariants[0]));
constexpr unsigned kIdentityIndices = 65536;    // a non-indexed draw indexes through these
constexpr int kLoggedUnknownKeys = 16;
// The distance fade waits for the backdrop copy (VP-2.6). Until then its range
// starts beyond any eye distance, so no pixel fades. The start and the end
// are different: smoothstep with equal edges divides by zero, and on the
// device each faded pixel then showed the backdrop (a white texture).
constexpr float kNoFadeStart = 1e18f;
constexpr float kNoFadeEnd = 2e18f;
constexpr uint64_t kStatsMicros = 1000000;

SceGxmBlendFactor BlendFactor(int d3d)
{
    switch (d3d) {
    case GFX_BLEND_ZERO: return SCE_GXM_BLEND_FACTOR_ZERO;
    case GFX_BLEND_ONE: return SCE_GXM_BLEND_FACTOR_ONE;
    case GFX_BLEND_SRCCOLOR: return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
    case GFX_BLEND_INVSRCCOLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GFX_BLEND_SRCALPHA: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case GFX_BLEND_INVSRCALPHA: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GFX_BLEND_DESTALPHA: return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case GFX_BLEND_INVDESTALPHA: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GFX_BLEND_DESTCOLOR: return SCE_GXM_BLEND_FACTOR_DST_COLOR;
    case GFX_BLEND_INVDESTCOLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GFX_BLEND_SRCALPHASAT: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return SCE_GXM_BLEND_FACTOR_ONE;
    }
}

SceGxmDepthFunc DepthFunc(int d3d)
{
    switch (d3d) {
    case GFX_CMP_NEVER: return SCE_GXM_DEPTH_FUNC_NEVER;
    case GFX_CMP_LESS: return SCE_GXM_DEPTH_FUNC_LESS;
    case GFX_CMP_EQUAL: return SCE_GXM_DEPTH_FUNC_EQUAL;
    case GFX_CMP_LESSEQUAL: return SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
    case GFX_CMP_GREATER: return SCE_GXM_DEPTH_FUNC_GREATER;
    case GFX_CMP_NOTEQUAL: return SCE_GXM_DEPTH_FUNC_NOT_EQUAL;
    case GFX_CMP_GREATEREQUAL: return SCE_GXM_DEPTH_FUNC_GREATER_EQUAL;
    default: return SCE_GXM_DEPTH_FUNC_ALWAYS;
    }
}

// GXM decides the winding on the target (row 0 at the top), as Direct3D
// does: the same names. Measured on the device: the flipped map culled every
// frontend draw.
SceGxmCullMode CullMode(int d3d)
{
    switch (d3d) {
    case GFX_CULL_CCW: return SCE_GXM_CULL_CCW;
    case GFX_CULL_CW: return SCE_GXM_CULL_CW;
    default: return SCE_GXM_CULL_NONE;
    }
}

// False for a line strip: GXM has none.
bool Primitive(int d3d, SceGxmPrimitiveType *out)
{
    switch (d3d) {
    case GFX_POINTLIST: *out = SCE_GXM_PRIMITIVE_POINTS; return true;
    case GFX_LINELIST: *out = SCE_GXM_PRIMITIVE_LINES; return true;
    case GFX_TRIANGLESTRIP: *out = SCE_GXM_PRIMITIVE_TRIANGLE_STRIP; return true;
    case GFX_TRIANGLELIST: *out = SCE_GXM_PRIMITIVE_TRIANGLES; return true;
    default: return false;
    }
}

bool Check(const char *call, int err)
{
    if (err < 0)
        Sys_Log("gxm: %s: 0x%08x", call, (unsigned)err);
    return err >= 0;
}

// The source a stage's texture coordinate index names (VU_TEX_COORD).
float TexCoordSource(DWORD index)
{
    if ((index & 0xffff0000) == 0x30000)
        return 2.0f;    // camera-space reflection vector
    return (index & 0xffff) == 1 ? 1.0f : 0.0f;
}

// The rows of vu[] in fixed3d_v.cg (shaders/cg/uniforms.h).
void FillVertexUniforms(const GfxDrawState &s, const GfxViewport &viewport, float u[VU_ROWS][4])
{
    memset(u, 0, sizeof(float) * 4 * VU_ROWS);
    Multiply(s.world, s.view, u[VU_WORLD_VIEW]);
    memcpy(u[VU_PROJECTION], &s.projection, 64);
    memcpy(u[VU_TEXTURE_MATRIX0], &s.texture[0], 64);
    memcpy(u[VU_TEXTURE_MATRIX1], &s.texture[1], 64);
    Copy4(s.material.diffuse, u[VU_MATERIAL_DIFFUSE]);
    Copy4(s.material.ambient, u[VU_MATERIAL_AMBIENT]);
    Copy4(s.material.specular, u[VU_MATERIAL_SPECULAR]);
    Copy4(s.material.emissive, u[VU_MATERIAL_EMISSIVE]);
    Color(s.ambient, u[VU_GLOBAL_AMBIENT]);
    u[VU_GLOBAL_AMBIENT][3] = s.material.power;
    for (int i = 0; i < 8; i++) {
        const GfxLight &l = s.lights[i];
        float(*row)[4] = &u[VU_LIGHTS + i * VU_LIGHT_ROWS];
        Copy4(l.diffuse, row[VU_LIGHT_DIFFUSE]);
        Copy4(l.specular, row[VU_LIGHT_SPECULAR]);
        Copy4(l.ambient, row[VU_LIGHT_AMBIENT]);
        TransformPoint(l.position, 1.0f, s.view, row[VU_LIGHT_POSITION]);
        row[VU_LIGHT_POSITION][3] = (float)l.type;
        TransformPoint(l.direction, 0.0f, s.view, row[VU_LIGHT_DIRECTION]);
        row[VU_LIGHT_DIRECTION][3] = l.range > 0.0f ? l.range : 1e30f;
        row[VU_LIGHT_ATTENUATION][0] = l.attenuation0;
        row[VU_LIGHT_ATTENUATION][1] = l.attenuation1;
        row[VU_LIGHT_ATTENUATION][2] = l.attenuation2;
        row[VU_LIGHT_ATTENUATION][3] = l.falloff;
        row[VU_LIGHT_SPOT][0] = cosf(l.theta * 0.5f);
        row[VU_LIGHT_SPOT][1] = cosf(l.phi * 0.5f);
        row[VU_LIGHT_SPOT][2] = s.lightEnabled[i] ? 1.0f : 0.0f;
    }
    u[VU_LIGHT_FLAGS][0] = s.lighting;
    u[VU_LIGHT_FLAGS][1] = s.colorVertex;
    u[VU_LIGHT_FLAGS][2] = s.specular;
    u[VU_LIGHT_FLAGS][3] = s.localViewer;
    u[VU_SOURCES][0] = (float)s.diffuseSource;
    u[VU_SOURCES][1] = (float)s.specularSource;
    u[VU_SOURCES][2] = (float)s.ambientSource;
    u[VU_SOURCES][3] = (float)s.emissiveSource;
    const bool vertexFog = s.fog && s.fogTableMode == GFX_FOG_NONE;
    u[VU_FOG_FLAGS][0] = vertexFog && s.fogVertexMode == GFX_FOG_LINEAR;
    u[VU_FOG_FLAGS][1] = s.normalizeNormals;
    // Direct3D takes the fog factor from the vertex's specular alpha when
    // fog is on with neither vertex nor table fog (the backdrop of a foggy stage).
    u[VU_FOG_FLAGS][2] = vertexFog && s.fogVertexMode == GFX_FOG_NONE;
    u[VU_FOG_PARAMS][0] = s.fogStart;
    u[VU_FOG_PARAMS][1] = s.fogEnd;
    u[VU_TEX_COORD][0] = TexCoordSource(s.stages[0].texCoordIndex);
    u[VU_TEX_COORD][1] = TexCoordSource(s.stages[1].texCoordIndex);
    u[VU_TEX_COORD][2] = s.stages[0].textureTransform != GFX_TTFF_DISABLE;
    u[VU_TEX_COORD][3] = s.stages[1].textureTransform != GFX_TTFF_DISABLE;
    // Direct3D 7's half-pixel offset of the viewport transform.
    u[VU_TARGET][2] = viewport.width ? 1.0f / viewport.width : 0.0f;
    u[VU_TARGET][3] = viewport.height ? -1.0f / viewport.height : 0.0f;
}

// The rows of fu[] in the fixed_f variants (shaders/cg/uniforms.h).
void FillFragmentUniforms(const GfxDrawState &s, int targetW, int targetH, float u[FU_ROWS][4])
{
    memset(u, 0, sizeof(float) * 4 * FU_ROWS);
    for (int i = 0; i < 2; i++) {
        const GfxStageState &stage = s.stages[i];
        const int first = i == 0 ? FU_STAGE0_SOURCE : FU_STAGE1_SOURCE;
        const int args[4] = { stage.colorArg1, stage.colorArg2, stage.alphaArg1, stage.alphaArg2 };
        for (int k = 0; k < 4; k++) {
            const int source = args[k] & 0xf;
            u[first][k] = (float)(source <= GFX_TA_SPECULAR ? source : GFX_TA_CURRENT);
            u[first + 1][k] = (args[k] & GFX_TA_COMPLEMENT) ? 1.0f : 0.0f;
            u[first + 2][k] = (args[k] & GFX_TA_ALPHAREPLICATE) ? 1.0f : 0.0f;
        }
        // The texture drawn, and the original a vector icon also reads.
        const GfxTexture *drawn = Gfx_DrawnTexture(stage.texture);
        float *size = u[i == 0 ? FU_TEX_SIZE0 : FU_TEX_SIZE1];
        if (drawn != nullptr) {
            size[0] = (float)drawn->width;
            size[1] = (float)drawn->height;
            size[2] = (float)stage.texture->width;
            size[3] = (float)stage.texture->height;
        }
    }
    Color(s.textureFactor, u[FU_TEXTURE_FACTOR]);
    u[FU_ALPHA_TEST][0] = (float)s.alphaFunc;
    u[FU_ALPHA_TEST][1] = (float)(s.alphaRef & 0xff);
    Color(s.fogColor, u[FU_FOG_COLOR]);
    u[FU_FOG_PARAMS][0] = s.fogStart;
    u[FU_FOG_PARAMS][1] = s.fogEnd;
    memcpy(u[FU_BUMP_MATRIX], s.stages[0].bumpMat, sizeof(float) * 4);
    u[FU_BUMP_LUMINANCE][0] = s.stages[0].bumpLumScale;
    u[FU_BUMP_LUMINANCE][1] = s.stages[0].bumpLumOffset;
    u[FU_DISTANCE_FADE][0] = kNoFadeStart;
    u[FU_DISTANCE_FADE][1] = kNoFadeEnd;
    u[FU_TARGET][0] = targetW ? 1.0f / targetW : 0.0f;
    u[FU_TARGET][1] = targetH ? 1.0f / targetH : 0.0f;
}

// The copy of a GfxVertexBuffer the GPU draws from, as of a version of it.
struct GxmVertexBuffer {
    void *memory = nullptr;     // from the static pool
    uint32_t version = 0;
};

// A fragment program variant: its patcher id and its fu[] parameter.
struct Variant {
    SceGxmShaderPatcherId id = nullptr;
    const SceGxmProgramParameter *uniforms = nullptr;
    unsigned uniformRows = 0;
};

// What the context has set: a draw sets only what differs. A scene starts
// with nothing known.
struct Bound {
    const SceGxmVertexProgram *vertexProgram = nullptr;
    const SceGxmFragmentProgram *fragmentProgram = nullptr;
    bool viewportSet = false;
    float viewport[6] = {};
    int region[4] = {};
    int depthFunc = -1, depthWrite = -1, cull = -1;
    const GxmTexture *textures[GXM_TEXTURE_UNITS] = {};
    uint32_t samplers[GXM_TEXTURE_UNITS] = {};
};

struct Stats {
    unsigned draws = 0, misses = 0, vertexFills = 0, textureBinds = 0, dropped = 0;
};

struct Executor : GfxExecutor {
    GxmDevice device;
    GxmMemory memory;   // after the device: freed before it
    GxmTextures gpuTextures{ memory };
    bool ready = false;
    bool loggedInit = false;

    // Programs.
    SceGxmShaderPatcherId clearVertexId = nullptr, clearFragmentId = nullptr, vertex3dId = nullptr,
                          vertexTLId = nullptr;
    SceGxmVertexProgram *clearVertex = nullptr, *vertex3d = nullptr, *vertexTL = nullptr;
    SceGxmFragmentProgram *clearColor = nullptr, *clearDepthOnly = nullptr;
    const SceGxmProgramParameter *clearDepthParam = nullptr, *clearColorParam = nullptr;
    const SceGxmProgramParameter *vertexUniformsParam = nullptr, *targetParam = nullptr;
    unsigned vertexUniformRows = 0;
    Variant variants[kVariantCount];
    std::unordered_map<uint64_t, int> variantIndex;     // fragment key and alpha test form -> variant
    std::unordered_map<uint32_t, SceGxmFragmentProgram *> fragmentPrograms;
    int loggedUnknownKeys = 0;

    uint16_t *identityIndices = nullptr;
    GxmTexture white;   // for a slot a program reads but the draw has no texture for
    GxmDeferredFree staticDeferred;     // static pool blocks of replaced or destroyed vertex buffers

    // The frame being recorded.
    unsigned presented = 0;     // frames shown: the number of the frame being recorded
    bool frameStarted = false;
    bool backBufferBegun = false;   // its scene was begun this frame
    bool inScene = false;
    const GxmTexture *sceneTarget = nullptr;    // nullptr: the back buffer
    Bound bound;
    // The vertex uniforms last filled: for the state and the target size.
    const GfxDrawState *vertexUniformState = nullptr;
    int vertexUniformW = 0, vertexUniformH = 0;
    float vertexUniforms[VU_ROWS][4];
    bool loggedSplit = false, loggedRing = false, loggedPrimitive = false, loggedStaticFull = false;

    // Textures made or destroyed in this frame; in an earlier one, not logged yet.
    bool texturesChanged = false, texturesLogPending = false;
    Stats stats;
    uint64_t statsStart = 0;

    ~Executor() override
    {
        if (!ready)
            return;
        device.Finish();
        SceGxmShaderPatcher *patcher = device.Patcher();
        for (auto &p : fragmentPrograms)
            if (p.second != nullptr)
                sceGxmShaderPatcherReleaseFragmentProgram(patcher, p.second);
        for (SceGxmFragmentProgram *p : { clearColor, clearDepthOnly })
            if (p != nullptr)
                sceGxmShaderPatcherReleaseFragmentProgram(patcher, p);
        for (SceGxmVertexProgram *p : { clearVertex, vertex3d, vertexTL })
            if (p != nullptr)
                sceGxmShaderPatcherReleaseVertexProgram(patcher, p);
        for (Variant &v : variants)
            if (v.id != nullptr)
                sceGxmShaderPatcherUnregisterProgram(patcher, v.id);
        for (SceGxmShaderPatcherId id : { clearVertexId, clearFragmentId, vertex3dId, vertexTLId })
            if (id != nullptr)
                sceGxmShaderPatcherUnregisterProgram(patcher, id);
        for (const GxmDeferredFree::Entry &e : staticDeferred.entries)
            memory.Static().Free(e.memory);
        memory.Static().Free(identityIndices);
        memory.Textures().Free(white.memory);
    }

    // ---- set up ----

    bool Register(const SceGxmProgram *program, const char *name, SceGxmShaderPatcherId *id)
    {
        char call[96];
        snprintf(call, sizeof(call), "sceGxmProgramCheck(%s)", name);
        if (!Check(call, sceGxmProgramCheck(program)))
            return false;
        snprintf(call, sizeof(call), "RegisterProgram(%s)", name);
        return Check(call, sceGxmShaderPatcherRegisterProgram(device.Patcher(), program, id));
    }

    // A vertex program over the frame's vertex layout: the attributes by
    // name, one stream of the vertex size.
    bool VertexProgram(SceGxmShaderPatcherId id, const SceGxmProgram *program, bool transformed,
                       SceGxmVertexProgram **out)
    {
        struct Attribute {
            const char *name;
            SceGxmAttributeFormat format;
            uint8_t components;
            uint16_t offset;
        };
        static const Attribute k3d[] = {
            { "aPosition", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 0 }, { "aNormal", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 12 },
            { "aDiffuse", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 24 }, { "aSpecular", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 28 },
            { "aTex0", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 32 },    { "aTex1", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 40 },
        };
        static const Attribute kTL[] = {
            { "aPosition", SCE_GXM_ATTRIBUTE_FORMAT_F32, 4, 0 },
            { "aDiffuse", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 16 },
            { "aSpecular", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 20 },
            { "aTex", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 24 },
        };
        const Attribute *list = transformed ? kTL : k3d;
        const int count = transformed ? 4 : 6;
        SceGxmVertexAttribute attributes[6];
        int used = 0;
        for (int i = 0; i < count; i++) {
            // The compiler drops an attribute the program does not read.
            const SceGxmProgramParameter *p = sceGxmProgramFindParameterByName(program, list[i].name);
            if (p == nullptr)
                continue;
            SceGxmVertexAttribute &a = attributes[used++];
            a.streamIndex = 0;
            a.offset = list[i].offset;
            a.format = list[i].format;
            a.componentCount = list[i].components;
            a.regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(p);
        }
        SceGxmVertexStream stream;
        stream.stride = transformed ? sizeof(GfxTLVertex) : sizeof(GfxVertex);
        stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
        return Check("CreateVertexProgram",
                     sceGxmShaderPatcherCreateVertexProgram(device.Patcher(), id, attributes, used, &stream, 1, out));
    }

    bool CreatePrograms()
    {
        SceGxmShaderPatcher *patcher = device.Patcher();
        if (!Register(&clear_v_gxp_start, "clear_v", &clearVertexId) ||
            !Register(&clear_f_gxp_start, "clear_f", &clearFragmentId) ||
            !Register(&fixed3d_v_gxp_start, "fixed3d_v", &vertex3dId) ||
            !Register(&fixedtl_v_gxp_start, "fixedtl_v", &vertexTLId))
            return false;
        SceGxmBlendInfo noColor{};
        noColor.colorMask = SCE_GXM_COLOR_MASK_NONE;
        noColor.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
        noColor.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
        if (!Check("CreateVertexProgram(clear_v)",
                   sceGxmShaderPatcherCreateVertexProgram(patcher, clearVertexId, nullptr, 0, nullptr, 0,
                                                          &clearVertex)) ||
            !Check("CreateFragmentProgram(clear_f)",
                   sceGxmShaderPatcherCreateFragmentProgram(patcher, clearFragmentId,
                                                            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                            SCE_GXM_MULTISAMPLE_NONE, nullptr, &clear_v_gxp_start,
                                                            &clearColor)) ||
            !Check("CreateFragmentProgram(clear_f, no colour)",
                   sceGxmShaderPatcherCreateFragmentProgram(patcher, clearFragmentId,
                                                            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                            SCE_GXM_MULTISAMPLE_NONE, &noColor, &clear_v_gxp_start,
                                                            &clearDepthOnly)) ||
            !VertexProgram(vertex3dId, &fixed3d_v_gxp_start, false, &vertex3d) ||
            !VertexProgram(vertexTLId, &fixedtl_v_gxp_start, true, &vertexTL))
            return false;
        clearDepthParam = sceGxmProgramFindParameterByName(&clear_v_gxp_start, "clearDepth");
        clearColorParam = sceGxmProgramFindParameterByName(&clear_f_gxp_start, "clearColor");
        vertexUniformsParam = sceGxmProgramFindParameterByName(&fixed3d_v_gxp_start, "vu");
        targetParam = sceGxmProgramFindParameterByName(&fixedtl_v_gxp_start, "target");
        if (clearDepthParam == nullptr || clearColorParam == nullptr || vertexUniformsParam == nullptr ||
            targetParam == nullptr) {
            Sys_Log("gxm: a program lacks clearDepth, clearColor, vu or target");
            return false;
        }
        vertexUniformRows = std::min<unsigned>(VU_ROWS, sceGxmProgramParameterGetArraySize(vertexUniformsParam));
        for (int i = 0; i < kVariantCount; i++) {
            const GxmFragmentVariant &v = kGxmFragmentVariants[i];
            const SceGxmProgram *program = (const SceGxmProgram *)v.gxp;
            if (!Register(program, v.name, &variants[i].id))
                return false;
            // A variant that reads no uniform row has no fu[].
            variants[i].uniforms = sceGxmProgramFindParameterByName(program, "fu");
            if (variants[i].uniforms != nullptr)
                variants[i].uniformRows =
                    std::min<unsigned>(FU_ROWS, sceGxmProgramParameterGetArraySize(variants[i].uniforms));
            variantIndex[v.key << 2 | (uint64_t)v.alphaTest] = i;
        }
        // The pipelines the game draws with (variants.txt), made now: a
        // patch costs a few ms. A program that can discard never blends.
        for (const GxmPipelineTriple &t : kGxmPipelineTriples) {
            if (t.srcBlend != 0 && kGxmFragmentVariants[t.variant].alphaTest == 1)
                Sys_Log("gxm: %s discards and draws with blending", kGxmFragmentVariants[t.variant].name);
            FragmentProgram(t.variant, t.srcBlend != 0, t.srcBlend, t.destBlend, t.transformed);
        }
        Sys_Log("gxm: %d fragment variants, %d pipelines made", kVariantCount, (int)fragmentPrograms.size());
        stats.misses = 0;
        return true;
    }

    bool CreateBuffers()
    {
        identityIndices = (uint16_t *)memory.Static().Alloc(kIdentityIndices * sizeof(uint16_t));
        white.layout = Gxm_TextureLayout(GFX_FORMAT_BGRA8, 8, 8, 1);
        white.memory = memory.Textures().Alloc(white.layout.bytes);
        if (identityIndices == nullptr || white.memory == nullptr)
            return false;
        for (unsigned i = 0; i < kIdentityIndices; i++)
            identityIndices[i] = (uint16_t)i;
        memset(white.memory, 0xff, white.layout.bytes);
        return Check("sceGxmTextureInitLinear(white)",
                     sceGxmTextureInitLinear(&white.texture, white.memory, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB, 8, 8,
                                             1));
    }

    bool SetVideoMode(int width, int height, bool) override
    {
        if (!ready)
            ready = device.Init() && memory.Init() && CreatePrograms() && CreateBuffers();
        return ready && device.SetMode(width, height);
    }

    void SetFullscreen(bool) override {}

    // ---- programs ----

    SceGxmFragmentProgram *FragmentProgram(int variant, bool blend, int srcBlend, int destBlend, bool transformed)
    {
        const uint32_t key = (uint32_t)variant | (uint32_t)(blend ? srcBlend & 15 : 0) << 8 |
                             (uint32_t)(blend ? destBlend & 15 : 0) << 12 | (blend ? 1u : 0u) << 16 |
                             (transformed ? 1u : 0u) << 17;
        auto it = fragmentPrograms.find(key);
        if (it != fragmentPrograms.end())
            return it->second;
        stats.misses++;
        SceGxmBlendInfo info{};
        info.colorMask = SCE_GXM_COLOR_MASK_ALL;
        info.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
        info.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
        info.colorSrc = info.alphaSrc = BlendFactor(srcBlend);
        info.colorDst = info.alphaDst = BlendFactor(destBlend);
        SceGxmFragmentProgram *program = nullptr;
        Check(kGxmFragmentVariants[variant].name,
              sceGxmShaderPatcherCreateFragmentProgram(
                  device.Patcher(), variants[variant].id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                  SCE_GXM_MULTISAMPLE_NONE, blend ? &info : nullptr,
                  transformed ? &fixedtl_v_gxp_start : &fixed3d_v_gxp_start, &program));
        fragmentPrograms[key] = program;
        return program;
    }

    // The variant of a fragment key drawn with or without blending, or -1.
    int FindVariant(uint64_t key, bool blend)
    {
        const uint64_t form = (key & 1ull << 28) == 0 ? 0 : blend ? 2 : 1;
        auto it = variantIndex.find(key << 2 | form);
        if (it != variantIndex.end())
            return it->second;
        if (loggedUnknownKeys < kLoggedUnknownKeys) {
            Sys_Log("gxm: no variant for fragment key %09llx with blending %d: add it to variants.txt",
                    (unsigned long long)key, blend ? 1 : 0);
            loggedUnknownKeys++;
        }
        return -1;
    }

    // ---- state ----

    void SetPrograms(const SceGxmVertexProgram *v, const SceGxmFragmentProgram *f)
    {
        SceGxmContext *context = device.Context();
        if (v != bound.vertexProgram) {
            sceGxmSetVertexProgram(context, v);
            bound.vertexProgram = v;
        }
        if (f != bound.fragmentProgram) {
            sceGxmSetFragmentProgram(context, f);
            bound.fragmentProgram = f;
        }
    }

    // The viewport (x, y, w, h in target pixels, row 0 at the top) and the
    // region clip (the scissor; GXM clips at a coarser step than a pixel).
    void SetViewport(float x, float y, float w, float h, float minZ, float maxZ, const GfxViewport &scissor)
    {
        SceGxmContext *context = device.Context();
        const float viewport[6] = { x, y, w, h, minZ, maxZ };
        if (!bound.viewportSet || memcmp(viewport, bound.viewport, sizeof(viewport)) != 0) {
            const GxmViewport v = Gxm_ViewportFromRect(x, y, w, h, minZ, maxZ);
            sceGxmSetViewport(context, v.xOffset, v.xScale, v.yOffset, v.yScale, v.zOffset, v.zScale);
            memcpy(bound.viewport, viewport, sizeof(viewport));
        }
        const int region[4] = { (int)scissor.x, (int)scissor.y, (int)(scissor.x + scissor.width) - 1,
                                (int)(scissor.y + scissor.height) - 1 };
        if (!bound.viewportSet || memcmp(region, bound.region, sizeof(region)) != 0) {
            sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, region[0], region[1], region[2], region[3]);
            memcpy(bound.region, region, sizeof(region));
        }
        bound.viewportSet = true;
    }

    void SetDepthAndCull(SceGxmDepthFunc func, bool write, SceGxmCullMode cull)
    {
        SceGxmContext *context = device.Context();
        if ((int)func != bound.depthFunc) {
            sceGxmSetFrontDepthFunc(context, func);
            sceGxmSetBackDepthFunc(context, func);
            bound.depthFunc = (int)func;
        }
        if ((int)write != bound.depthWrite) {
            const SceGxmDepthWriteMode mode = write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
            sceGxmSetFrontDepthWriteEnable(context, mode);
            sceGxmSetBackDepthWriteEnable(context, mode);
            bound.depthWrite = (int)write;
        }
        if ((int)cull != bound.cull) {
            sceGxmSetCullMode(context, cull);
            bound.cull = (int)cull;
        }
    }

    // The context copies the texture's state when it is set: a sampler
    // change sets it again.
    void BindTexture(int unit, GxmTexture *g)
    {
        if (g == nullptr)
            g = &white;
        g->lastDrawn = presented;
        if (bound.textures[unit] == g && bound.samplers[unit] == g->sampler)
            return;
        sceGxmSetFragmentTexture(device.Context(), unit, &g->texture);
        bound.textures[unit] = g;
        bound.samplers[unit] = g->sampler;
        stats.textureBinds++;
    }

    // The textures of a draw, by the variant's units, with the sampler state
    // of gfx_gpu.cpp's TextureBindings.
    void BindTextures(const GfxDrawState &s, const GxmFragmentVariant &v)
    {
        for (int unit = 0; unit < GXM_TEXTURE_UNITS && v.units[unit] != GXM_SLOT_NONE; unit++) {
            const int slot = v.units[unit];
            if (slot == GXM_SLOT_BACKDROP) {
                BindTexture(unit, nullptr);     // VP-2.6
                continue;
            }
            const int i = (slot == GXM_SLOT_TEX1 || slot == GXM_SLOT_CUBE1 || slot == GXM_SLOT_NATIVE1) ? 1 : 0;
            const GfxStageState &stage = s.stages[i];
            const bool native = slot == GXM_SLOT_NATIVE0 || slot == GXM_SLOT_NATIVE1;
            GfxTexture *t = native ? stage.texture : Gfx_DrawnTexture(stage.texture);
            GxmTexture *g = t != nullptr ? gpuTextures.Ensure(t) : nullptr;
            if (g != nullptr) {
                const int kind = Gfx_TextureKind(stage);
                int minFilter = stage.minFilter, magFilter = stage.magFilter, mipFilter = stage.mipFilter;
                if (Gfx_ScaledPoint(stage) || (!native && (kind == 5 || kind == 6)))
                    minFilter = magFilter = GFX_FILTER_LINEAR;
                // A distance field is read between its texels, and from its mip
                // levels where it is drawn smaller.
                if (!native && kind == 4) {
                    minFilter = magFilter = GFX_FILTER_LINEAR;
                    mipFilter = GFX_MIPFILTER_LINEAR;
                }
                gpuTextures.SetSampler(*g, minFilter, magFilter, mipFilter, stage.address);
            }
            BindTexture(unit, g);
        }
    }

    // ---- scenes ----

    void EndScene()
    {
        if (!inScene)
            return;
        if (sceneTarget == nullptr)
            device.EndScene();
        else
            Check("sceGxmEndScene", sceGxmEndScene(device.Context(), nullptr, nullptr));
        inScene = false;
    }

    bool BeginScene(const GxmTexture *target)
    {
        if (inScene && sceneTarget == target)
            return true;
        EndScene();
        bool ok;
        if (target == nullptr) {
            // The back buffer's depth is not kept across a second scene.
            if (backBufferBegun && !loggedSplit) {
                Sys_Log("gxm: the back buffer's scene was split in a frame");
                loggedSplit = true;
            }
            ok = device.BeginBackBufferScene();
            backBufferBegun = backBufferBegun || ok;
        } else {
            ok = Check("sceGxmBeginScene(target)", sceGxmBeginScene(device.Context(), 0, target->target, nullptr,
                                                                    nullptr, nullptr, &target->color, &target->depth));
        }
        inScene = ok;
        sceneTarget = target;
        bound = Bound();
        return ok;
    }

    // ---- frame data ----

    void UploadVertexBuffers(const GfxFrame &frame)
    {
        for (GfxVertexBuffer *b : frame.buffers) {
            GxmVertexBuffer *g = (GxmVertexBuffer *)b->gpu;
            if (g == nullptr)
                b->gpu = g = new GxmVertexBuffer;
            const uint32_t bytes = (uint32_t)(b->vertices.size() * sizeof(GfxVertex));
            if ((g->memory != nullptr && g->version == b->version) || bytes == 0)
                continue;
            // Frames still on the GPU may draw from the old copy.
            if (g->memory != nullptr)
                staticDeferred.Add(g->memory, presented);
            g->memory = memory.Static().Alloc(bytes);
            if (g->memory == nullptr) {
                if (!loggedStaticFull)
                    Sys_Log("gxm: no room for a vertex buffer of %u KB", bytes / 1024);
                loggedStaticFull = true;
                continue;
            }
            memcpy(g->memory, b->vertices.data(), bytes);
            g->version = b->version;
        }
    }

    void DestroyVertexBuffer(GfxVertexBuffer *b) override
    {
        GxmVertexBuffer *g = (GxmVertexBuffer *)b->gpu;
        if (g == nullptr)
            return;
        if (g->memory != nullptr)
            staticDeferred.Add(g->memory, presented);
        delete g;
        b->gpu = nullptr;
    }

    // The textures the frame draws with or into are on the GPU before the upload.
    void EnsureTextures(const GfxFrame &frame)
    {
        for (const GfxDrawState &s : frame.states)
            for (const GfxStageState &stage : s.stages) {
                GfxTexture *t = Gfx_DrawnTexture(stage.texture);
                if (t == nullptr)
                    continue;
                gpuTextures.Ensure(t);
                // A vector icon also reads its original.
                if (t->flags & GFX_TEXTURE_VECTOR_ICON)
                    gpuTextures.Ensure(stage.texture);
            }
        for (const GfxCommand &c : frame.commands)
            if (c.type == GFX_CMD_TARGET && c.target != nullptr)
                gpuTextures.Ensure(c.target->cube != nullptr ? c.target->cube : c.target);
    }

    void StartFrame()
    {
        // Back from the LiveArea or from sleep: the GPU finishes what it had
        // before it gets more.
        if (Platform_TakeResumed())
            device.Finish();
        gpuTextures.Collect(presented);
        staticDeferred.Collect(presented, [this](void *block) { memory.Static().Free(block); });
        memory.FrameRing().BeginFrame(device.FrameSlot());
        backBufferBegun = false;
        frameStarted = true;
    }

    // ---- drawing ----

    // GXM has no clear: a quad over the cleared rectangle.
    void Clear(const GfxCommand &c, bool backBuffer, int targetW, int targetH)
    {
        const bool color = (c.clearFlags & GFX_CLEAR_TARGET) != 0;
        const bool depth = (c.clearFlags & GFX_CLEAR_ZBUFFER) != 0;
        const GfxViewport v = ToTarget(c.clearViewport, c.clearScreenW, c.clearScreenH, backBuffer, targetW, targetH);
        if ((!color && !depth) || v.width == 0 || v.height == 0)
            return;
        SetPrograms(clearVertex, color ? clearColor : clearDepthOnly);
        SetViewport((float)v.x, (float)v.y, (float)v.width, (float)v.height, 0.0f, 1.0f, v);
        SetDepthAndCull(SCE_GXM_DEPTH_FUNC_ALWAYS, depth, SCE_GXM_CULL_NONE);
        SceGxmContext *context = device.Context();
        void *buffer;
        const float z[4] = { c.clearZ, 0.0f, 0.0f, 0.0f };
        sceGxmReserveVertexDefaultUniformBuffer(context, &buffer);
        sceGxmSetUniformDataF(buffer, clearDepthParam, 0, 4, z);
        float rgba[4];
        Color(c.clearColor, rgba);
        sceGxmReserveFragmentDefaultUniformBuffer(context, &buffer);
        sceGxmSetUniformDataF(buffer, clearColorParam, 0, 4, rgba);
        Check("sceGxmDraw(clear)", sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16,
                                              identityIndices, 4));
    }

    // vertexOffset, indexOffset: the Execute's data in the ring (kFull: none).
    void Draw(const GfxCommand &c, const GfxFrame &frame, unsigned vertexOffset, unsigned indexOffset,
              bool backBuffer, int targetW, int targetH)
    {
        SceGxmPrimitiveType primitive;
        if (!Primitive(c.primitive, &primitive)) {
            if (!loggedPrimitive)
                Sys_Log("gxm: primitive %d is not drawn", c.primitive);
            loggedPrimitive = true;
            return;
        }
        const GfxDrawState &s = frame.states[c.state];
        const int variant = FindVariant(Gfx_FragmentKey(s, backBuffer && !c.transformed), s.alphaBlend);
        if (variant < 0)
            return;
        SceGxmFragmentProgram *fragment = FragmentProgram(variant, s.alphaBlend, s.srcBlend, s.destBlend,
                                                          c.transformed);
        if (fragment == nullptr)
            return;
        // Where the vertices and the indices are.
        const unsigned char *vertices = nullptr;
        if (c.buffer != nullptr) {
            const GxmVertexBuffer *g = (const GxmVertexBuffer *)c.buffer->gpu;
            if (g != nullptr)
                vertices = (const unsigned char *)g->memory;
        } else if (vertexOffset != GxmFrameRing::kFull) {
            vertices = memory.FrameRing().At(vertexOffset);
        }
        const uint16_t *indices = identityIndices;
        unsigned count = c.vertexCount;
        if (c.firstIndex != ~0u) {
            if (indexOffset == GxmFrameRing::kFull)
                vertices = nullptr;
            else
                indices = (const uint16_t *)memory.FrameRing().At(indexOffset) + c.firstIndex;
            count = c.indexCount;
        }
        if (vertices == nullptr || count == 0 || count > kIdentityIndices) {
            stats.dropped++;
            return;
        }

        SetPrograms(c.transformed ? vertexTL : vertex3d, fragment);
        const bool scaled = backBuffer && s.screenW > 0 && s.screenH > 0;
        const GfxViewport viewport = ToTarget(s.viewport, s.screenW, s.screenH, backBuffer, targetW, targetH);
        if (c.transformed)
            SetViewport(0.0f, 0.0f, (float)targetW, (float)targetH, 0.0f, 1.0f, viewport);
        else
            SetViewport((float)viewport.x, (float)viewport.y, (float)viewport.width, (float)viewport.height,
                        viewport.minZ, viewport.maxZ, viewport);
        SetDepthAndCull(s.depthTest ? DepthFunc(s.depthFunc) : SCE_GXM_DEPTH_FUNC_ALWAYS, s.depthTest && s.depthWrite,
                        CullMode(s.cull));

        // The default uniform buffers last one draw: reserved and filled each
        // time, after the programs are set.
        SceGxmContext *context = device.Context();
        void *buffer;
        sceGxmReserveVertexDefaultUniformBuffer(context, &buffer);
        if (c.transformed) {
            // Pre-transformed vertices are in pixels of the game's screen.
            const float target[4] = { (float)(scaled ? s.screenW : targetW), (float)(scaled ? s.screenH : targetH),
                                      0.0f, 0.0f };
            sceGxmSetUniformDataF(buffer, targetParam, 0, 4, target);
        } else {
            if (vertexUniformState == nullptr || vertexUniformW != targetW || vertexUniformH != targetH ||
                (vertexUniformState != &s && !SameVertexUniforms(*vertexUniformState, s))) {
                FillVertexUniforms(s, viewport, vertexUniforms);
                vertexUniformState = &s;
                vertexUniformW = targetW;
                vertexUniformH = targetH;
                stats.vertexFills++;
            }
            sceGxmSetUniformDataF(buffer, vertexUniformsParam, 0, vertexUniformRows * 4, vertexUniforms[0]);
        }
        const Variant &v = variants[variant];
        if (v.uniforms != nullptr) {
            float fragmentUniforms[FU_ROWS][4];
            FillFragmentUniforms(s, targetW, targetH, fragmentUniforms);
            sceGxmReserveFragmentDefaultUniformBuffer(context, &buffer);
            sceGxmSetUniformDataF(buffer, v.uniforms, 0, v.uniformRows * 4, fragmentUniforms[0]);
        }
        BindTextures(s, kGxmFragmentVariants[variant]);
        sceGxmSetVertexStream(context, 0, vertices + c.firstVertexByte);
        Check("sceGxmDraw", sceGxmDraw(context, primitive, SCE_GXM_INDEX_FORMAT_U16, indices, count));
        stats.draws++;
    }

    // ---- frame execution ----

    void Execute(GfxFrame &frame, std::vector<GfxTexture *> &textures, GfxReadback &readback, bool present,
                 bool vsync, bool) override
    {
        if (!ready) {
            for (GfxTexture *t : textures)
                t->dirty = false;
            return;
        }
        if (!frameStarted)
            StartFrame();
        EnsureTextures(frame);
        gpuTextures.Upload(textures, presented);
        texturesChanged = gpuTextures.TakeChanged() || texturesChanged;
        UploadVertexBuffers(frame);

        // The frame's vertices and indices, after those of its earlier Execute calls.
        GxmFrameRing &ring = memory.FrameRing();
        const unsigned vertexOffset = ring.Alloc((unsigned)frame.vertices.size(), 16);
        const unsigned indexOffset = ring.Alloc((unsigned)(frame.indices.size() * 2), 4);
        if (vertexOffset != GxmFrameRing::kFull)
            memcpy(ring.At(vertexOffset), frame.vertices.data(), frame.vertices.size());
        if (indexOffset != GxmFrameRing::kFull)
            memcpy(ring.At(indexOffset), frame.indices.data(), frame.indices.size() * 2);
        if ((vertexOffset == GxmFrameRing::kFull || indexOffset == GxmFrameRing::kFull) && !loggedRing) {
            Sys_Log("gxm: a frame is larger than its ring slot: its draws are dropped");
            loggedRing = true;
        }

        // The drawing into textures runs first, then the back buffer's, as in
        // gfx_gpu.cpp: no scene on another target splits the back buffer's.
        for (int phase = 0; phase < 2; phase++) {
            GfxTexture *target = nullptr;
            const GxmTexture *targetTexture = nullptr;
            for (const GfxCommand &c : frame.commands) {
                if (c.type == GFX_CMD_TARGET) {
                    target = c.target;
                    targetTexture = nullptr;
                    if (target != nullptr)
                        targetTexture = (const GxmTexture *)(target->cube != nullptr ? target->cube : target)->gpu;
                    continue;
                }
                if ((target != nullptr) != (phase == 0))
                    continue;
                // The backdrop copy for the distance fade: VP-2.6.
                if (c.type == GFX_CMD_BACKDROP)
                    continue;
                // A target with no GPU surface (a cube face) is not drawn.
                if (target != nullptr && (targetTexture == nullptr || targetTexture->target == nullptr))
                    continue;
                if (!BeginScene(targetTexture))
                    continue;
                const int w = target != nullptr ? target->width : device.Width();
                const int h = target != nullptr ? target->height : device.Height();
                if (c.type == GFX_CMD_CLEAR)
                    Clear(c, target == nullptr, w, h);
                else
                    Draw(c, frame, vertexOffset, indexOffset, target == nullptr, w, h);
            }
            if (phase == 0 && inScene && sceneTarget != nullptr)
                EndScene();
        }
        // The frame's states go with this Execute.
        vertexUniformState = nullptr;
        if (!present)
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
        if (!BeginScene(nullptr))
            return;
        EndScene();
        device.Present(vsync);
        presented++;
        frameStarted = false;
        LogTextures();
        LogStats();
    }

    // One "gxm tex" and one "gxm mem" line when the textures stop changing
    // (the end of a load), not one per frame of the load.
    void LogTextures()
    {
        if (texturesChanged) {
            texturesLogPending = true;
        } else if (texturesLogPending) {
            gpuTextures.Log();
            memory.Log();
            texturesLogPending = false;
        }
        texturesChanged = false;
    }

    void LogStats()
    {
#ifdef OPENCMR2_DEV
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (statsStart == 0)
            statsStart = now;
        if (now - statsStart < kStatsMicros)
            return;
        Sys_Log("gxm: per s draws %u, patcher misses %u, vertex fills %u, texture binds %u, dropped %u", stats.draws,
                stats.misses, stats.vertexFills, stats.textureBinds, stats.dropped);
        stats = Stats();
        statsStart = now;
#endif
    }

    void DestroyTexture(GfxTexture *t) override
    {
        gpuTextures.Destroy(t, presented);
    }

    void ReadTexture(GfxTexture *t) override
    {
        // The frame's open scene draws first.
        EndScene();
        device.Finish();
        gpuTextures.Read(t);
    }
};

} // namespace

GfxExecutor *Gfx_CreateGxmExecutor(void)
{
    return new Executor;
}
