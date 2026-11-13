// The GXM executor (PS Vita): runs a recorded frame (gfx_core.cpp) on the GXM
// device (gxm_device.cpp), as gfx_gpu.cpp does on SDL_GPU. The fragment
// programs are the precompiled variants of render/shaders/cg (one per
// fragment key and alpha test form), patched per blend pair.

#include "platform/platform_vita.h"
#include "port/sys.h"
#include "render/gfx_executor_shared.h"
#include "render/gfx_internal.h"
#include "render/gxm_device.h"
#include "render/gxm_alpha.h"
#include "render/gxm_bounds.h"
#include "render/gxm_dialog.h"
#include "render/gxm_digits.h"
#include "render/gxm_fragment_uniforms.h"
#include "render/gxm_key_cache.h"
#include "render/gxm_readback.h"
#include "render/gxm_textures.h"
#include "render/gxm_vertex_uniforms.h"
#include "render/gxm_viewport.h"
#include "render/shader_key.h"
#include "render/shader_variants_gen.h"
#include "render/shaders/cg/uniforms.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

#include <stdio.h>
#include <string.h>

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
extern const SceGxmProgram clear_v_gxp_start;
extern const SceGxmProgram clear_f_gxp_start;
extern const SceGxmProgram copy_f_gxp_start;
extern const SceGxmProgram fixed3d_v_gxp_start;
extern const SceGxmProgram fixedtl_v_gxp_start;
}

namespace {

using namespace GfxShared;

constexpr int kVariantCount = (int)(sizeof(kGxmFragmentVariants) / sizeof(kGxmFragmentVariants[0]));
constexpr unsigned kIdentityIndices = 65536;    // a non-indexed draw indexes through these
constexpr size_t kLoggedUnknownKeys = 64;    // different missing variants logged, at most
constexpr uint64_t kFadeKeyBit = 1ull << 32;    // the distance fade of a fragment key (shader_key.h)
// Dev builds time the backdrop copy once in this many frames (it waits for
// the GPU before and after it).
constexpr unsigned kBackdropTimingFrames = 600;
constexpr uint64_t kStatsMicros = 1000000;
constexpr uint64_t kFpsMicros = 500000;     // the frame rate on the screen is that of this long

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

// What the executor finds out about the vertices of a 3D draw from their CPU
// copy: their box, and the least alpha of their two colours.
struct DrawRange {
    GxmBounds box;
    uint8_t diffuseAlpha = 0, specularAlpha = 0;
};

// box, alpha: what to measure (the other part stays as it is).
inline void MeasureRange(const GfxVertex *vertices, unsigned count, bool box, bool alpha, DrawRange &r)
{
    if (box)
        r.box = Gxm_VertexBounds(vertices, sizeof(GfxVertex), count);
    if (alpha) {
        r.diffuseAlpha =
            (uint8_t)Gxm_MinVertexByte(vertices, sizeof(GfxVertex), count, offsetof(GfxVertex, diffuse) + 3);
        r.specularAlpha =
            (uint8_t)Gxm_MinVertexByte(vertices, sizeof(GfxVertex), count, offsetof(GfxVertex, specular) + 3);
    }
}

// The copy of a GfxVertexBuffer the GPU draws from, as of a version of it.
struct GxmVertexBuffer {
    void *memory = nullptr;     // from the static pool
    uint32_t version = 0;
    uint32_t noRoomVersion = 0; // the version the pool had no room for (0: none)
    // The vertex ranges drawn from rangesVersion of the buffer, by first
    // vertex << 32 | count: made at the buffer's first 3D draw. A range
    // that lost its slot to another range is measured again.
    std::unique_ptr<GxmKeyCache<DrawRange, 8>> ranges;
    uint32_t rangesVersion = 0;
    // The snapshot of the Execute call snapshotExecute (GfxFrame::snapshots):
    // the ring offset of its vertex snapshotFirst.
    uint32_t snapshotExecute = 0;
    unsigned snapshotOffset = 0;
    uint32_t snapshotFirst = 0;
};

// A fragment program variant: its patcher id and its fu[] parameter.
struct Variant {
    SceGxmShaderPatcherId id = nullptr;
    const SceGxmProgramParameter *uniforms = nullptr;
    unsigned uniformRows = 0;
};

// The variants of a fragment key and alpha test form: the general one, which
// reads the stage arguments from uniforms, and those made for one arguments
// key (shader_key.h), which are much shorter.
struct VariantSet {
    int general = -1;
    std::vector<std::pair<uint64_t, int>> made;     // arguments key, variant
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
    unsigned generations[GXM_TEXTURE_UNITS] = {};
    // The default uniform buffers of the set programs, as a draw reserved and
    // filled them. They stay for the draws that follow: a draw reserves a
    // buffer again only when its program or its rows change. A new scene
    // starts with none.
    bool vertexUniforms = false, fragmentUniforms = false;
    unsigned vertexVersion = 0, vertexRows = 0;     // a 3D draw: the version and the rows of the vertex uniforms
    float target[2] = {};                           // a 2D draw: the screen size
    float fragment[FU_ROWS][4] = {};                // the rows of the fragment program
    unsigned fragmentVersion = 0;                   // the fill of the executor's rows that these are equal to
};

struct Stats {
    unsigned draws = 0, misses = 0, vertexFills = 0, textureBinds = 0, dropped = 0, depthPasses = 0;
    unsigned fadeDraws = 0, nearDraws = 0;  // draws of a key with the fade: with it, and too near for it
    unsigned depthSkipped = 0;              // blended draws whose alpha test passes for each pixel: no depth pass
    unsigned vertexUploads = 0, fragmentUploads = 0;    // default uniform buffers reserved and filled
    unsigned rangesMeasured = 0, rangeVertices = 0;     // vertex buffer ranges that the cache did not have
    unsigned snapshotBytes = 0;                         // vertices of buffers copied into the ring
    unsigned ownDraws = 0, ownVertices = 0;             // draws of the frame's vertices that were walked for the fade
    // The time of Prepare (the game's thread) and of Execute without the
    // wait of its present (the worker with the threaded executor), and the
    // frames presented.
    unsigned prepareMicros = 0, executeMicros = 0, frames = 0;
    // Texture binds of textures with mip levels made here: with a mip
    // filter, and without one (the game's mipmapping option off).
    unsigned mipReads = 0, mipUnread = 0;
};

// The frame time probe of dev builds. A tap on the front touch screen starts
// it. It runs without vsync. The modes of kProbeCycle take turns, each for
// kProbeBlockFrames frames, kProbeCycles times: each mode gets frames from
// the same parts of the drive, so their times compare (a mode that ran alone
// for some seconds met a scene of its own). The first frames of a turn are
// not counted: the GPU still works on frames of the mode before. Then the log
// gets a line for each mode. The modes split a frame into its parts.
// Before the turns the probe compares the single pass with the two passes
// (see the mode "two passes"): it draws two frames with two passes and two
// with the single pass, waits for the GPU after each, and compares samples
// of the back buffer. With a picture that stands still (the car at rest),
// the count between the two kinds is what the single pass changes. The modes:
//   normal            the frame as the game draws it
//   quarter           the back buffer's draws clipped to about a quarter of
//                     its pixels: the fragment cost goes down with them
//   tile              clipped to one 32x32 tile: almost no fragment cost, the
//                     vertex cost and the CPU cost of the draws stay
//   no 3D             no draw of 3D vertices: the game, the recorder and the
//                     2D draws
//   no depth pass     normal, without the depth pass of blended draws
//   general           normal, with the general variants only: what the
//                     variants made for the arguments keys save
//   fade always       normal, with the fade also in the draws that are too
//                     near for it: what the near test saves
//   depth pass always normal, with the depth pass also for the blended
//                     draws whose alpha test passes for each pixel
//   lookup only       the 3D draws stop before their first GXM call: with
//                     "no 3D" and "tile" it splits the CPU cost of a draw
//                     into the executor's own part and the GXM calls
//   profile           normal, with the time of each step of a 3D draw (and of
//                     an Execute before its draws) summed: a second line
//                     gives them for a frame
//   uniforms always   each pass reserves and fills its uniform buffers, also
//                     where those of the pass before are still right
//   single pass       a blended draw with the alpha test and depth writes
//                     draws once, with the discard and its blending, and has
//                     no depth pass (the default: video.single_pass)
//   two passes        such a draw has a depth pass with the discard and a
//                     blended pass that writes each pixel: what the single
//                     pass saves
// The modes quarter, general, fade always, depth pass always, no depth pass
// and single pass are not in the cycle: their questions are answered.
// Each line also has the CPU time of an Execute up to the present: the cost
// of the draw calls without the wait for the GPU.
enum ProbeMode {
    PROBE_OFF, PROBE_NORMAL, PROBE_QUARTER, PROBE_TILE, PROBE_NO_3D, PROBE_NO_DEPTH_PASS, PROBE_GENERAL,
    PROBE_FADE_ALWAYS, PROBE_DEPTH_PASS_ALWAYS, PROBE_LOOKUP_ONLY, PROBE_PROFILE, PROBE_UNIFORMS_ALWAYS,
    PROBE_SINGLE_PASS, PROBE_TWO_PASSES, PROBE_END
};
const char *const kProbeNames[PROBE_END] = { "off",         "normal",        "quarter", "tile",
                                             "no 3D",       "no depth pass", "general", "fade always",
                                             "depth pass always", "lookup only", "profile", "uniforms always",
                                             "single pass", "two passes" };

// The steps the profile mode times: an Execute before its draws, then the
// steps of a 3D draw in Draw's order.
enum ProfileSection {
    PROFILE_SETUP,      // textures, vertex buffers, the frame's vertices into the ring
    PROFILE_STATE,      // the viewport and the vertex uniforms of the draw's state
    PROFILE_KEYS,       // the fragment key and the arguments key
    PROFILE_RANGE,      // the vertex range and its eye distance
    PROFILE_VARIANT,    // the variant and its fragment program
    PROFILE_DEPTH,      // whether the draw needs a depth pass, and its program
    PROFILE_VERTICES,   // where the vertices and the indices are
    PROFILE_PASSES,     // the GXM calls of the draw's passes
    PROFILE_SECTIONS
};
const int kProbeCycle[] = { PROBE_NORMAL,          PROBE_NO_3D,      PROBE_LOOKUP_ONLY, PROBE_TILE,
                            PROBE_UNIFORMS_ALWAYS, PROBE_TWO_PASSES, PROBE_PROFILE };
constexpr unsigned kProbeCycleModes = sizeof(kProbeCycle) / sizeof(kProbeCycle[0]);
constexpr unsigned kProbeBlockFrames = 8;   // the frames of a mode's turn
constexpr unsigned kProbeSkipFrames = 2;    // at the start of a turn: not counted
constexpr unsigned kProbeCycles = 12;
constexpr int kProbeCheckStep = 4;          // the check samples each 4th pixel of each 4th row
constexpr int kProbeCheckFrames = 4;        // two frames with two passes, two with the single pass
constexpr int kProbeCheckChannel = 12;      // a sample differs with a channel this far off

// The sums of a mode over its counted frames.
struct ProbeSums {
    uint64_t frameMicros = 0, frameMax = 0;
    uint64_t submitMicros = 0;      // in Execute, up to the present
    uint64_t section[PROFILE_SECTIONS] = {};    // the profile mode's sums
    unsigned frames = 0, draws = 0, draws3d = 0;
};

struct Probe {
    int mode = PROBE_OFF;
    unsigned slot = 0, blockFrame = 0, cycle = 0;   // the place in the cycle
    uint64_t lastPresent = 0;
    // The frame that is drawn now.
    uint64_t submitMicros = 0;
    uint64_t section[PROFILE_SECTIONS] = {};
    uint64_t mark = 0;              // the end of the step before
    unsigned draws3d = 0, draws = 0;
    ProbeSums sums[PROBE_END];
    bool touchStarted = false, touchDown = false;
    // The check of the single pass: the frame of the check that is drawn now
    // (1 to kProbeCheckFrames, 0: no check), and the samples that differ
    // between each frame and the frame before it.
    int check = 0;
    unsigned checkSamples = 0, checkDiffer[kProbeCheckFrames] = {};
};

// A back buffer readback the GPU has not finished: a rectangle of a display
// buffer, ready when the notification word reaches value.
struct PendingRead {
    bool active = false;
    int buffer = 0;
    GxmReadbackRect rect;
    uint32_t value = 0;
};

struct Executor : GfxExecutor, GxmDialogPresenter {
    GxmDevice device;
    GxmMemory memory;   // after the device: freed before it
    GxmTextures gpuTextures{ memory };
    bool ready = false;
    bool loggedInit = false;

    // Programs.
    SceGxmShaderPatcherId clearVertexId = nullptr, clearFragmentId = nullptr, copyFragmentId = nullptr,
                          vertex3dId = nullptr, vertexTLId = nullptr;
    SceGxmVertexProgram *clearVertex = nullptr, *vertex3d = nullptr, *vertexTL = nullptr;
    SceGxmFragmentProgram *clearColor = nullptr, *clearDepthOnly = nullptr, *copyProgram = nullptr;
    const SceGxmProgramParameter *clearDepthParam = nullptr, *clearColorParam = nullptr, *copyInvSizeParam = nullptr;
    const SceGxmProgramParameter *vertexUniformsParam = nullptr, *targetParam = nullptr;
    unsigned vertexUniformRows = 0;
    Variant variants[kVariantCount];
    std::unordered_map<uint64_t, VariantSet> variantIndex;  // by fragment key << 2 | alpha test form
    std::unordered_map<uint32_t, SceGxmFragmentProgram *> fragmentPrograms;
    std::unordered_set<uint64_t> loggedUnknownKeys;     // FindVariant's lookups, with the vertex program
    bool programsMade = false;      // the listed triples are made: a new program is a patch in a frame
    size_t loggedUnlisted = 0;
    std::unordered_set<uint64_t> loggedGeneral;     // (fragment key, arguments key) drawn with the general variant

    uint16_t *identityIndices = nullptr;
    GxmTexture white;   // for a slot a program reads but the draw has no texture for
    GxmTexture whiteCube;   // the same for a cube slot
    // The back buffer as it was at GFX_CMD_BACKDROP: the distance fade reads it.
    GxmTexture backdrop;
    int backdropW = 0, backdropH = 0;
    // The back buffer's size divided by this is the backdrop's
    // (video.backdrop_scale, 2 by default, 1 for the full size). The copy
    // cost 2.9 ms of the GPU in each frame at 960x544; the scenery that
    // fades shows a backdrop of half the size, filtered, behind it.
    int backdropScale = 2;
    unsigned nextBackdropTiming = 0;
    // The readback: the notification word the GPU writes after a frame that
    // has one, the last value asked for, and the read not collected yet.
    volatile unsigned int *notificationWord = nullptr;
    uint32_t notificationValue = 0;
    PendingRead pendingRead;
    GxmDeferredFree staticDeferred;     // static pool blocks of replaced or destroyed vertex buffers

    // The frame being recorded.
    unsigned presented = 0;     // frames shown: the number of the frame being recorded
    bool frameStarted = false;
    bool backBufferBegun = false;   // its scene was begun this frame
    bool backdropAhead = false;     // this Execute copies the backdrop: the back buffer scene stores its depth
    bool backdropCopied = false;    // this frame has its backdrop copy
    bool backSceneStoresDepth = false;  // the open back buffer scene stores its depth
    bool depthStored = false;       // the next back buffer scene loads the depth the last one stored
    unsigned backScenes = 0;        // back buffer scenes begun in this frame
    unsigned lastBackScenes = 0;    // and in the frame before
    bool inScene = false;
    const GxmTexture *sceneTarget = nullptr;    // nullptr: the back buffer
    int sceneFace = 0;                          // a cube target: its face
    Bound bound;
    // The vertex uniforms last filled: for the state and the target size.
    // The version goes up when a row changes.
    const GfxDrawState *vertexUniformState = nullptr;
    int vertexUniformW = 0, vertexUniformH = 0;
    bool vertexUniformBackBuffer = false;
    // The fragment uniforms of the state drawn last: filled again when the
    // state, the target or the backdrop copy changes. The draws of one state
    // have their own textures, and the rows of most programs have nothing of
    // a texture. A program that reads the size of its texture gets the rows
    // filled for each pass. The version goes up with each fill.
    const GfxDrawState *fragmentRowsState = nullptr;
    int fragmentRowsW = 0, fragmentRowsH = 0;
    bool fragmentRowsFade = false;
    unsigned fragmentRowsVersion = 1;
    float fragmentRows[FU_ROWS][4];
    // The viewport of the state drawn last, in the target's pixels: the
    // draws of one state on one target have one viewport.
    const GfxDrawState *viewportState = nullptr;
    int viewportW = 0, viewportH = 0;
    bool viewportBackBuffer = false;
    GfxViewport viewportOfState = {};
    unsigned vertexUniformVersion = 1;
    float vertexUniforms[VU_ROWS][4];
    // The variant and the programs of the keys of the draws before. Most
    // draws of a stage have the keys of a draw just before them: they find
    // their programs here, without the maps.
    struct Resolved {
        uint64_t key = ~0ull, args = 0;
        uint32_t pipeline = 0;      // the blending, the vertex kind, the probe's mode
        int variant = -1;
        SceGxmFragmentProgram *fragment = nullptr;      // nullptr: the key cannot draw
        bool depthLooked = false;   // the depth pass of the key is looked up
        int depthVariant = -1;
        SceGxmFragmentProgram *depthProgram = nullptr;
        SceGxmFragmentProgram *single = nullptr;        // the probe's single pass
    };
    Resolved resolvedKeys[4];
    unsigned resolvedNext = 0;

    // The frame rate over the picture (video.show_fps): the presented frames
    // of the last kFpsMicros, as a number at the top left.
    bool showFps = false;
    unsigned fpsFrames = 0;
    uint64_t fpsStart = 0;
    int fpsShown = -1;

    // Whether a blended draw with the alpha test and depth writes draws in
    // one pass, with the discard form and its blending, as Direct3D does it
    // (video.single_pass, 1 by default). Without it such a draw has a depth
    // pass with the discard and a blended pass that writes each pixel: on
    // another SGX543 project a program that can discard drew nothing in a
    // blended pass without depth writes. With depth writes it draws: the
    // device drew 99.7% of the samples of a still picture the same in both
    // ways, and the two passes cost 32 us for each draw.
    bool singlePass = true;
    std::vector<uint32_t> probeShot[2];     // the check's samples of two frames

    // Whether a reserved uniform buffer serves the draws that follow: the
    // device answers it at the first video mode (TestKeptUniforms). A draw
    // that fails with a kept buffer also ends it. Without it each pass
    // reserves its buffers, as GXM always permits.
    bool keepUniforms = false, keepUniformsTested = false;
    bool loggedSplit = false, loggedRing = false, loggedPrimitive = false, loggedStaticFull = false;
    uint32_t executeCount = 0;      // Execute calls (GxmVertexBuffer::snapshotExecute)

    // Textures made or destroyed in this frame; in an earlier one, not logged yet.
    bool texturesChanged = false, texturesLogPending = false;
    Stats stats;
    uint64_t statsStart = 0;
    Probe probe;
    // Set in Prepare, read in Execute: the performance stats are on
    // (Gfx_PerfStatsWanted), and the readback the next present asks for.
    bool statsWanted = true;
    GxmReadbackRect nextRead;

    ~Executor() override
    {
        if (!ready)
            return;
        Gxm_SetDialogPresenter(nullptr);
        device.Finish();
        SceGxmShaderPatcher *patcher = device.Patcher();
        for (auto &p : fragmentPrograms)
            if (p.second != nullptr)
                sceGxmShaderPatcherReleaseFragmentProgram(patcher, p.second);
        for (SceGxmFragmentProgram *p : { clearColor, clearDepthOnly, copyProgram })
            if (p != nullptr)
                sceGxmShaderPatcherReleaseFragmentProgram(patcher, p);
        for (SceGxmVertexProgram *p : { clearVertex, vertex3d, vertexTL })
            if (p != nullptr)
                sceGxmShaderPatcherReleaseVertexProgram(patcher, p);
        for (Variant &v : variants)
            if (v.id != nullptr)
                sceGxmShaderPatcherUnregisterProgram(patcher, v.id);
        for (SceGxmShaderPatcherId id : { clearVertexId, clearFragmentId, copyFragmentId, vertex3dId, vertexTLId })
            if (id != nullptr)
                sceGxmShaderPatcherUnregisterProgram(patcher, id);
        for (const GxmDeferredFree::Entry &e : staticDeferred.entries)
            memory.Static().Free(e.memory);
        memory.Static().Free(identityIndices);
        memory.Textures().Free(white.memory);
        memory.Textures().Free(whiteCube.memory);
        ReleaseBackdrop();
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
            !Register(&copy_f_gxp_start, "copy_f", &copyFragmentId) ||
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
            !Check("CreateFragmentProgram(copy_f)",
                   sceGxmShaderPatcherCreateFragmentProgram(patcher, copyFragmentId,
                                                            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                            SCE_GXM_MULTISAMPLE_NONE, nullptr, &clear_v_gxp_start,
                                                            &copyProgram)) ||
            !VertexProgram(vertex3dId, &fixed3d_v_gxp_start, false, &vertex3d) ||
            !VertexProgram(vertexTLId, &fixedtl_v_gxp_start, true, &vertexTL))
            return false;
        clearDepthParam = sceGxmProgramFindParameterByName(&clear_v_gxp_start, "clearDepth");
        clearColorParam = sceGxmProgramFindParameterByName(&clear_f_gxp_start, "clearColor");
        copyInvSizeParam = sceGxmProgramFindParameterByName(&copy_f_gxp_start, "invSize");
        vertexUniformsParam = sceGxmProgramFindParameterByName(&fixed3d_v_gxp_start, "vu");
        targetParam = sceGxmProgramFindParameterByName(&fixedtl_v_gxp_start, "target");
        if (clearDepthParam == nullptr || clearColorParam == nullptr || copyInvSizeParam == nullptr ||
            vertexUniformsParam == nullptr || targetParam == nullptr) {
            Sys_Log("gxm: a program lacks clearDepth, clearColor, invSize, vu or target");
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
            VariantSet &set = variantIndex[v.key << 2 | (uint64_t)v.alphaTest];
            if (v.args == GXM_ARGS_GENERAL)
                set.general = i;
            else
                set.made.push_back({ v.args, i });
        }
        // The pipelines the game draws with (variants.txt), made now: a
        // patch costs a few ms. Each triple with the general variant of its
        // key and with the variants of the key's arguments keys. A blended
        // 3D triple with the alpha test also gets the programs of its
        // discard form: with its blending for the single pass, or without
        // colour for the depth pass.
        for (const GxmPipelineTriple &t : kGxmPipelineTriples) {
            const GxmFragmentVariant &v = kGxmFragmentVariants[t.variant];
            const bool blend = t.srcBlend != 0;
            if (blend && v.alphaTest == 1)
                Sys_Log("gxm: %s discards and draws with blending", v.name);
            const VariantSet &set = variantIndex[v.key << 2 | (uint64_t)v.alphaTest];
            FragmentProgram(t.variant, blend, t.srcBlend, t.destBlend, t.transformed);
            for (const auto &made : set.made)
                FragmentProgram(made.second, blend, t.srcBlend, t.destBlend, t.transformed);
            if (!blend || v.alphaTest != 2 || t.transformed)
                continue;
            const VariantSet &discard = variantIndex[v.key << 2 | 1];
            auto make = [&](int variant) {
                if (singlePass)
                    FragmentProgram(variant, true, t.srcBlend, t.destBlend, false);
                else
                    FragmentProgram(variant, false, 0, 0, false, true);
            };
            if (discard.general >= 0)
                make(discard.general);
            for (const auto &made : discard.made)
                make(made.second);
        }
        Sys_Log("gxm: %d fragment variants, %d pipelines made", kVariantCount, (int)fragmentPrograms.size());
        stats.misses = 0;
        programsMade = true;
        return true;
    }

    bool CreateBuffers()
    {
        identityIndices = (uint16_t *)memory.Static().Alloc(kIdentityIndices * sizeof(uint16_t));
        white.layout = Gxm_TextureLayout(GFX_FORMAT_BGRA8, 8, 8, 1);
        white.memory = memory.Textures().Alloc(white.layout.bytes);
        Gxm_CubeLayout(kGxmMinCubeSize, 1, &whiteCube.layout);
        whiteCube.memory = memory.Textures().Alloc(whiteCube.layout.bytes);
        if (identityIndices == nullptr || white.memory == nullptr || whiteCube.memory == nullptr)
            return false;
        memset(whiteCube.memory, 0xff, whiteCube.layout.bytes);
        if (!Check("sceGxmTextureInitCube(white)",
                   sceGxmTextureInitCube(&whiteCube.texture, whiteCube.memory, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB,
                                         kGxmMinCubeSize, kGxmMinCubeSize, 1)))
            return false;
        for (unsigned i = 0; i < kIdentityIndices; i++)
            identityIndices[i] = (uint16_t)i;
        notificationWord = sceGxmGetNotificationRegion();
        *notificationWord = 0;
        memset(white.memory, 0xff, white.layout.bytes);
        return Check("sceGxmTextureInitLinear(white)",
                     sceGxmTextureInitLinear(&white.texture, white.memory, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB, 8, 8,
                                             1));
    }

    bool SetVideoMode(int width, int height, bool) override
    {
        if (!ready) {
            singlePass = Platform_GetSettingInt("video.single_pass", 1) != 0;
            // The mip levels of the BC3 textures (video.mipmaps, 1 by
            // default): the game's graphics menu chooses how a draw reads
            // them (its mipmapping option), as for the textures with levels.
            gpuTextures.SetMipmaps(Platform_GetSettingInt("video.mipmaps", 1) != 0);
            ready = device.Init() && memory.Init() && CreatePrograms() && CreateBuffers();
            // GXM runs from here: a message box draws through this display.
            if (ready)
                Gxm_SetDialogPresenter(this);
        }
        if (!ready || !device.SetMode(width, height))
            return false;
        if (!keepUniformsTested) {
            keepUniformsTested = true;
            TestKeptUniforms();
        }
        // Dev builds show the frame rate unless the settings say no.
#ifdef OPENCMR2_DEV
        showFps = Platform_GetSettingInt("video.show_fps", 1) != 0;
#else
        showFps = Platform_GetSettingInt("video.show_fps", 0) != 0;
#endif
        // Without it the scenery does not fade, and the game still runs.
        const int scale = Platform_GetSettingInt("video.backdrop_scale", 2);
        backdropScale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
        CreateBackdrop();
        return true;
    }

    // Finds out whether a reserved default uniform buffer serves the draws
    // that follow it. vitaGL draws that way. Notes from other GXM work say
    // that a buffer lasts one draw. Squares of the clear programs in a scene
    // of their own answer it, read back from the back buffer:
    //   A  both buffers reserved and filled: green
    //   B  no buffer reserved: green only if both buffers stayed
    //   C  after a draw with another fragment program, only the fragment
    //      buffer reserved: red only if the vertex buffer stayed
    // Each square has the depth test "equal" on a depth of 0.5 from the
    // vertex buffer, so a vertex buffer with other values draws nothing.
    void TestKeptUniforms()
    {
        keepUniforms = false;
        const int size = 64;
        if (device.Width() < size * 4 || device.Height() < size || !device.BeginBackBufferScene())
            return;
        bound = Bound();
        SceGxmContext *context = device.Context();
        bool drawn = true;
        const float depth[4] = { 0.5f, 0.0f, 0.0f, 0.0f };
        float black[4], green[4], red[4];
        Color(0xff000000, black);
        Color(0xff00ff00, green);
        Color(0xffff0000, red);
        // z, rgba: nullptr keeps the buffer of the draw before.
        auto square = [&](int column, SceGxmFragmentProgram *program, SceGxmDepthFunc func, const float *z,
                          const float *rgba) {
            GfxViewport v{};
            v.x = (DWORD)(column * size);
            v.width = v.height = (DWORD)size;
            v.maxZ = 1.0f;
            SetPrograms(clearVertex, program);
            SetViewport((float)v.x, 0.0f, (float)size, (float)size, 0.0f, 1.0f, v);
            SetDepthAndCull(func, true, SCE_GXM_CULL_NONE);
            void *buffer;
            if (z != nullptr) {
                sceGxmReserveVertexDefaultUniformBuffer(context, &buffer);
                sceGxmSetUniformDataF(buffer, clearDepthParam, 0, 4, z);
            }
            if (rgba != nullptr) {
                sceGxmReserveFragmentDefaultUniformBuffer(context, &buffer);
                sceGxmSetUniformDataF(buffer, clearColorParam, 0, 4, rgba);
            }
            drawn = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, identityIndices,
                               4) >= 0 && drawn;
        };
        // The ground of the squares: black, at the depth.
        for (int column = 0; column < 4; column++)
            square(column, clearColor, SCE_GXM_DEPTH_FUNC_ALWAYS, depth, black);
        square(0, clearColor, SCE_GXM_DEPTH_FUNC_EQUAL, depth, green);
        square(1, clearColor, SCE_GXM_DEPTH_FUNC_EQUAL, nullptr, nullptr);
        square(3, clearDepthOnly, SCE_GXM_DEPTH_FUNC_ALWAYS, nullptr, black);
        square(2, clearColor, SCE_GXM_DEPTH_FUNC_EQUAL, nullptr, red);
        device.EndScene();
        device.Finish();
        bound = Bound();
        uint32_t pixel[3] = {};
        for (int column = 0; column < 3; column++)
            device.ReadPixels(device.BackBuffer(), column * size + size / 2, size / 2, 1, 1, &pixel[column]);
        const uint32_t a = pixel[0] & 0xffffff, b = pixel[1] & 0xffffff, c = pixel[2] & 0xffffff;
        keepUniforms = drawn && a == 0x00ff00 && b == 0x00ff00 && c == 0xff0000;
        Sys_Log("gxm: a reserved uniform buffer serves the draws that follow: %s (squares %06x %06x %06x, draws %s)",
                keepUniforms ? "yes" : "no", (unsigned)a, (unsigned)b, (unsigned)c, drawn ? "ok" : "failed");
    }

    // The backdrop target, from the texture pool: the size of the back buffer
    // divided by backdropScale. SetMode waited for the GPU before a size
    // change.
    void CreateBackdrop()
    {
        const int w = (device.Width() + backdropScale - 1) / backdropScale;
        const int h = (device.Height() + backdropScale - 1) / backdropScale;
        if (backdrop.target != nullptr && w == backdropW && h == backdropH)
            return;
        ReleaseBackdrop();
        backdrop.layout = Gxm_TextureLayout(GFX_FORMAT_BGRA8, w, h, 1);
        backdrop.memory = memory.Textures().Alloc(backdrop.layout.bytes);
        if (backdrop.memory == nullptr) {
            Sys_Log("gxm: no room for the backdrop (%u KB): the scenery does not fade", backdrop.layout.bytes / 1024);
            return;
        }
        const unsigned rowBytes = backdrop.layout.level[0].rowBytes;
        SceGxmRenderTargetParams params{};
        params.width = w;
        params.height = h;
        params.scenesPerFrame = 1;
        params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
        params.driverMemBlock = -1;
        if (!Check("sceGxmColorSurfaceInit(backdrop)",
                   sceGxmColorSurfaceInit(&backdrop.color, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                                          SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h,
                                          rowBytes / 4, backdrop.memory)) ||
            !Check("sceGxmTextureInitLinearStrided(backdrop)",
                   sceGxmTextureInitLinearStrided(&backdrop.texture, backdrop.memory,
                                                  SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, w, h, rowBytes)) ||
            !Check("sceGxmCreateRenderTarget(backdrop)", sceGxmCreateRenderTarget(&params, &backdrop.target))) {
            ReleaseBackdrop();
            return;
        }
        // Read pixel for pixel at the size of the back buffer (copy_f and the
        // fade both sample texel centres); smaller, between its texels.
        const int filter = backdropScale > 1 ? GFX_FILTER_LINEAR : GFX_FILTER_POINT;
        gpuTextures.SetSampler(backdrop, filter, filter, GFX_MIPFILTER_NONE, GFX_ADDRESS_CLAMP);
        backdropW = w;
        backdropH = h;
    }

    void ReleaseBackdrop()
    {
        if (backdrop.target != nullptr)
            sceGxmDestroyRenderTarget(backdrop.target);
        backdrop.target = nullptr;
        memory.Textures().Free(backdrop.memory);
        backdrop.memory = nullptr;
        backdrop.sampler = ~0u;
        backdropW = backdropH = 0;
    }

    void SetFullscreen(bool) override {}

    // ---- programs ----

    // depthOnly: the program writes no colour (the depth pass of a blended
    // draw with the alpha test). Then blend is false.
    SceGxmFragmentProgram *FragmentProgram(int variant, bool blend, int srcBlend, int destBlend, bool transformed,
                                           bool depthOnly = false)
    {
        const uint32_t key = (uint32_t)variant | (uint32_t)(blend ? srcBlend & 15 : 0) << 16 |
                             (uint32_t)(blend ? destBlend & 15 : 0) << 20 | (blend ? 1u : 0u) << 24 |
                             (transformed ? 1u : 0u) << 25 | (depthOnly ? 1u : 0u) << 26;
        auto it = fragmentPrograms.find(key);
        if (it != fragmentPrograms.end())
            return it->second;
        stats.misses++;
        const GxmFragmentVariant &v = kGxmFragmentVariants[variant];
        // A patch in the middle of a frame costs a few ms: variants.txt lists
        // the triples that are made at the start.
        if (programsMade && probe.mode == PROBE_OFF && loggedUnlisted < kLoggedUnknownKeys) {
            char pair[8] = "off";
            if (blend)
                snprintf(pair, sizeof(pair), "%d/%d", srcBlend & 15, destBlend & 15);
            Sys_Log("gxm: a program was made in a frame: add \"triple %09llx %s %s\" to variants.txt",
                    (unsigned long long)v.key, pair, transformed ? "tl" : "3d");
            loggedUnlisted++;
        }
        // Form 2 gives colour and alpha 0 where the alpha test fails. Only
        // some destination factors then leave the pixel as it was.
        if (blend && v.alphaTest == 2 && !Gxm_Form2Keeps(destBlend & 15))
            Sys_Log("gxm: %s with blend %d/%d: the pixels that fail the alpha test are not left as they were",
                    v.name, srcBlend & 15, destBlend & 15);
        SceGxmBlendInfo info{};
        info.colorMask = depthOnly ? SCE_GXM_COLOR_MASK_NONE : SCE_GXM_COLOR_MASK_ALL;
        info.colorFunc = depthOnly ? SCE_GXM_BLEND_FUNC_NONE : SCE_GXM_BLEND_FUNC_ADD;
        info.alphaFunc = info.colorFunc;
        info.colorSrc = info.alphaSrc = BlendFactor(srcBlend);
        info.colorDst = info.alphaDst = BlendFactor(destBlend);
        SceGxmFragmentProgram *program = nullptr;
        Check(v.name, sceGxmShaderPatcherCreateFragmentProgram(
                          device.Patcher(), variants[variant].id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                          SCE_GXM_MULTISAMPLE_NONE, blend || depthOnly ? &info : nullptr,
                          transformed ? &fixedtl_v_gxp_start : &fixed3d_v_gxp_start, &program));
        fragmentPrograms[key] = program;
        return program;
    }

    // The variant of a fragment key drawn with or without blending, or -1.
    // A missing one is logged once for each blend pair and vertex program,
    // as its triple line in variants.txt.
    // The variant of a key, form and arguments key: the one made for the
    // arguments, else the general one, else -1.
    int LookupVariant(uint64_t key, uint64_t form, uint64_t args)
    {
        auto it = variantIndex.find(key << 2 | form);
        if (it == variantIndex.end())
            return -1;
        if (probe.mode != PROBE_GENERAL)
            for (const auto &made : it->second.made)
                if (made.first == args)
                    return made.second;
        // The general variant is several times slower for each pixel.
        if (programsMade && probe.mode == PROBE_OFF && loggedGeneral.size() < kLoggedUnknownKeys &&
            loggedGeneral.insert(key << 24 ^ args).second)
            Sys_Log("gxm: the general variant draws: add \"args %09llx %011llx\" to variants.txt",
                    (unsigned long long)key, (unsigned long long)args);
        return it->second.general;
    }

    int FindVariant(uint64_t key, uint64_t args, const GfxDrawState &s, bool transformed)
    {
        const bool blend = s.alphaBlend;
        const uint64_t form = (key & 1ull << 28) == 0 ? 0 : blend ? 2 : 1;
        const int variant = LookupVariant(key, form, args);
        if (variant >= 0)
            return variant;
        const unsigned src = blend ? s.srcBlend & 15 : 0, dst = blend ? s.destBlend & 15 : 0;
        const uint64_t logged = key << 10 | src << 6 | dst << 2 | (blend ? 2u : 0u) | (transformed ? 1u : 0u);
        if (loggedUnknownKeys.size() < kLoggedUnknownKeys && loggedUnknownKeys.insert(logged).second) {
            char pair[8] = "off";
            if (blend)
                snprintf(pair, sizeof(pair), "%u/%u", src, dst);
            Sys_Log("gxm: no variant: add \"triple %09llx %s %s\" to variants.txt", (unsigned long long)key, pair,
                    transformed ? "tl" : "3d");
        }
        return -1;
    }

    // The variant and the fragment program of a draw's keys, or nullptr
    // where there is none.
    Resolved *Resolve(uint64_t key, uint64_t args, const GfxDrawState &s, bool transformed)
    {
        const uint32_t pipeline = (s.alphaBlend ? 1u | (uint32_t)(s.srcBlend & 15) << 1 | (uint32_t)(s.destBlend & 15) << 5
                                                : 0u) |
                                  (transformed ? 1u << 9 : 0u) | (uint32_t)probe.mode << 10 |
                                  (singlePass ? 1u << 20 : 0u);
        for (Resolved &r : resolvedKeys)
            if (r.key == key && r.args == args && r.pipeline == pipeline)
                return r.fragment != nullptr ? &r : nullptr;
        Resolved &r = resolvedKeys[resolvedNext++ % (sizeof(resolvedKeys) / sizeof(resolvedKeys[0]))];
        r = Resolved();
        r.key = key;
        r.args = args;
        r.pipeline = pipeline;
        r.variant = FindVariant(key, args, s, transformed);
        if (r.variant >= 0)
            r.fragment = FragmentProgram(r.variant, s.alphaBlend, s.srcBlend, s.destBlend, transformed);
        return r.fragment != nullptr ? &r : nullptr;
    }

    // ---- state ----

    void SetPrograms(const SceGxmVertexProgram *v, const SceGxmFragmentProgram *f)
    {
        SceGxmContext *context = device.Context();
        if (v != bound.vertexProgram) {
            sceGxmSetVertexProgram(context, v);
            bound.vertexProgram = v;
            bound.vertexUniforms = false;
        }
        if (f != bound.fragmentProgram) {
            sceGxmSetFragmentProgram(context, f);
            bound.fragmentProgram = f;
            bound.fragmentUniforms = false;
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
        int region[4] = { (int)scissor.x, (int)scissor.y, (int)(scissor.x + scissor.width) - 1,
                          (int)(scissor.y + scissor.height) - 1 };
        // The probe clips the back buffer's draws (240..719 x 136..407 is a
        // quarter of 960x544).
        if (sceneTarget == nullptr && probe.mode == PROBE_QUARTER) {
            const int quarter[4] = { device.Width() / 4, device.Height() / 4, device.Width() * 3 / 4 - 1,
                                     device.Height() * 3 / 4 - 1 };
            memcpy(region, quarter, sizeof(region));
        } else if (sceneTarget == nullptr && probe.mode == PROBE_TILE) {
            const int tile[4] = { 0, 0, 31, 31 };
            memcpy(region, tile, sizeof(region));
        }
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
    // change or a move to other memory sets it again.
    void BindTexture(int unit, GxmTexture *g)
    {
        if (g == nullptr)
            g = &white;
        g->lastDrawn = presented;
        if (bound.textures[unit] == g && bound.samplers[unit] == g->sampler && bound.generations[unit] == g->generation)
            return;
        sceGxmSetFragmentTexture(device.Context(), unit, &g->texture);
        bound.textures[unit] = g;
        bound.samplers[unit] = g->sampler;
        bound.generations[unit] = g->generation;
        stats.textureBinds++;
    }

    // The textures of a draw, by the variant's units, with the sampler state
    // of gfx_gpu.cpp's TextureBindings.
    void BindTextures(const GfxDrawState &s, const GxmFragmentVariant &v)
    {
        for (int unit = 0; unit < GXM_TEXTURE_UNITS && v.units[unit] != GXM_SLOT_NONE; unit++) {
            const int slot = v.units[unit];
            if (slot == GXM_SLOT_BACKDROP) {
                BindTexture(unit, backdropCopied ? &backdrop : nullptr);
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
                // The mip levels made here: read where the stage has a mip
                // filter (the game's mipmapping option), else not.
                if (g->madeMips) {
                    if (mipFilter != GFX_MIPFILTER_NONE)
                        stats.mipReads++;
                    else
                        stats.mipUnread++;
                }
            }
            if (g == nullptr && (slot == GXM_SLOT_CUBE0 || slot == GXM_SLOT_CUBE1))
                g = &whiteCube;
            BindTexture(unit, g);
        }
    }

    // ---- scenes ----

    // fragment: the back buffer scene's notification (the readback).
    void EndScene(const SceGxmNotification *fragment = nullptr)
    {
        if (!inScene)
            return;
        if (sceneTarget == nullptr) {
            device.EndScene(fragment);
            depthStored = backSceneStoresDepth;
        } else {
            Check("sceGxmEndScene", sceGxmEndScene(device.Context(), nullptr, nullptr));
        }
        inScene = false;
    }

    // face: the face of a cube target (0 otherwise).
    bool BeginScene(const GxmTexture *target, int face = 0)
    {
        if (inScene && sceneTarget == target && sceneFace == face)
            return true;
        EndScene();
        bool ok;
        if (target == nullptr) {
            // A scene stores its depth when another back buffer scene comes
            // after it, and that one loads it. The executor knows that around
            // the backdrop copy of this Execute. For the other splits (an
            // Execute before the one with the copy, a render target or a
            // texture read in the middle of a frame) it takes the frame
            // before: a scene stores where that frame had one more scene.
            if (backBufferBegun && !depthStored && !loggedSplit) {
                Sys_Log("gxm: the back buffer's scene was split in a frame, its depth not kept");
                loggedSplit = true;
            }
            backSceneStoresDepth = (backdropAhead && !backdropCopied) || backScenes + 1 < lastBackScenes;
            ok = device.BeginBackBufferScene(depthStored, backSceneStoresDepth);
            depthStored = false;
            backScenes += ok ? 1 : 0;
            backBufferBegun = backBufferBegun || ok;
        } else {
            const SceGxmColorSurface *color = target->faces != nullptr ? &target->faces[face] : &target->color;
            ok = Check("sceGxmBeginScene(target)", sceGxmBeginScene(device.Context(), 0, target->target, nullptr,
                                                                    nullptr, nullptr, color, &target->depth));
        }
        inScene = ok;
        sceneTarget = target;
        sceneFace = face;
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
            if ((g->memory != nullptr && g->version == b->version) || bytes == 0 || g->noRoomVersion == b->version)
                continue;
            // Frames still on the GPU may draw from the old copy.
            if (g->memory != nullptr)
                staticDeferred.Add(g->memory, presented);
            g->memory = memory.Static().Alloc(bytes);
            if (g->memory == nullptr) {
                if (!loggedStaticFull)
                    Sys_Log("gxm: no room for a vertex buffer of %u KB", bytes / 1024);
                loggedStaticFull = true;
                g->noRoomVersion = b->version;
                continue;
            }
            memcpy(g->memory, b->vertices.data(), bytes);
            g->version = b->version;
        }
    }

    // Each draw costs CPU time here: a run of the game's sprites or lines
    // comes as one draw.
    bool JoinsDraws() const override { return true; }

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
        // The textures of a draw are in its command (GfxCommand::textures).
        GfxTexture *last[2] = {};
        for (const GfxCommand &c : frame.commands) {
            if (c.type == GFX_CMD_TARGET && c.target != nullptr)
                gpuTextures.Ensure(c.target->cube != nullptr ? c.target->cube : c.target);
            if (c.type != GFX_CMD_DRAW)
                continue;
            for (int i = 0; i < 2; i++) {
                if (c.textures[i] == last[i])
                    continue;
                last[i] = c.textures[i];
                GfxTexture *t = Gfx_DrawnTexture(c.textures[i]);
                if (t == nullptr)
                    continue;
                gpuTextures.Ensure(t);
                // A vector icon also reads its original.
                if (t->flags & GFX_TEXTURE_VECTOR_ICON)
                    gpuTextures.Ensure(c.textures[i]);
            }
        }
        // The textures of the small batches, which become draws later.
        for (GfxTexture *t : frame.smallTextures)
            if (GfxTexture *drawn = Gfx_DrawnTexture(t))
                gpuTextures.Ensure(drawn);
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
        backdropCopied = false;
        depthStored = false;
        lastBackScenes = backScenes;
        backScenes = 0;
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
        bound.vertexUniforms = bound.fragmentUniforms = false;
        Check("sceGxmDraw(clear)", sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16,
                                              identityIndices, 4));
    }

    // The frame rate, in the back buffer's scene of a present: yellow
    // seven-segment digits on black, each segment a clear of its rectangle
    // (the executor has no font). The check of the probe compares frames:
    // none is drawn then.
    void DrawFps()
    {
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (fpsStart == 0)
            fpsStart = now;
        fpsFrames++;
        if (now - fpsStart >= kFpsMicros) {
            const uint64_t span = now - fpsStart;
            fpsShown = (int)((fpsFrames * 1000000ull + span / 2) / span);
            fpsFrames = 0;
            fpsStart = now;
        }
        if (!showFps || fpsShown < 0 || probe.check != 0)
            return;
        const int x = 12, y = 12, pad = 6;
        GfxCommand c = {};
        c.type = GFX_CMD_CLEAR;
        c.clearFlags = GFX_CLEAR_TARGET;
        auto fill = [&](const GxmDigitRect &r, DWORD color) {
            c.clearColor = color;
            c.clearViewport.x = (DWORD)r.x;
            c.clearViewport.y = (DWORD)r.y;
            c.clearViewport.width = (DWORD)r.width;
            c.clearViewport.height = (DWORD)r.height;
            c.clearViewport.minZ = 0.0f;
            c.clearViewport.maxZ = 1.0f;
            Clear(c, true, device.Width(), device.Height());
        };
        const int digits = fpsShown >= 100 ? 3 : fpsShown >= 10 ? 2 : 1;
        fill({ x - pad, y - pad, Gxm_NumberWidth(digits) + 2 * pad, kGxmDigitHeight + 2 * pad }, 0xff000000);
        GxmDigitRect rects[kGxmNumberDigits * kGxmDigitSegments];
        const int count = Gxm_NumberRects(fpsShown, x, y, rects);
        for (int i = 0; i < count; i++)
            fill(rects[i], 0xffffe000);
    }

    // The vertices of a 3D draw, measured from their CPU copy, or nullptr
    // where they cannot be read. The range of a vertex buffer is measured
    // once and kept while the buffer does not change. That of the frame's
    // vertices is measured for each draw, into scratch: only its box or its
    // alpha, as the draw needs.
    const DrawRange *RangeOf(const GfxCommand &c, const GfxFrame &frame, bool box, bool alpha, DrawRange &scratch)
    {
        if (c.transformed || c.vertexCount == 0)
            return nullptr;
        if (c.buffer == nullptr) {
            if ((size_t)c.firstVertexByte + (size_t)c.vertexCount * sizeof(GfxVertex) > frame.vertices.size())
                return nullptr;
            MeasureRange((const GfxVertex *)(frame.vertices.data() + c.firstVertexByte), c.vertexCount, box, alpha,
                         scratch);
            return &scratch;
        }
        // The vertices as the GPU draws them: the game may change the CPU
        // copy of the buffer while the worker of the threaded executor runs
        // the frame. The vertex count of a buffer does not change.
        GxmVertexBuffer *g = (GxmVertexBuffer *)c.buffer->gpu;
        const size_t first = c.firstVertexByte / sizeof(GfxVertex);
        if (g == nullptr || first + c.vertexCount > c.buffer->vertices.size())
            return nullptr;
        if (c.snapshot) {
            // The frame's snapshot, which starts at its first vertex.
            if (g->snapshotExecute != executeCount || g->snapshotOffset == GxmFrameRing::kFull ||
                first < g->snapshotFirst)
                return nullptr;
            const GfxVertex *copy = (const GfxVertex *)memory.FrameRing().At(g->snapshotOffset);
            MeasureRange(copy + (first - g->snapshotFirst), c.vertexCount, box, alpha, scratch);
            return &scratch;
        }
        if (g->memory == nullptr)
            return nullptr;
        if (g->ranges == nullptr) {
            g->ranges.reset(new GxmKeyCache<DrawRange, 8>());
            g->rangesVersion = g->version;
        } else if (g->rangesVersion != g->version) {
            g->ranges->Clear();
            g->rangesVersion = g->version;
        }
        const uint64_t key = (uint64_t)first << 32 | c.vertexCount;
        if (DrawRange *range = g->ranges->Find(key))
            return range;
        DrawRange &range = g->ranges->Put(key);
        MeasureRange((const GfxVertex *)g->memory + first, c.vertexCount, true, true, range);
        stats.rangesMeasured++;
        stats.rangeVertices += c.vertexCount;
        return &range;
    }

    // The least alpha (0..1) the texture read of a stage gives: 1 with no
    // texture, 0 where it is not known (a texture that is not a plain 2D one,
    // or not on the GPU yet).
    static float StageTextureAlpha(const GfxStageState &stage)
    {
        if (stage.texture == nullptr)
            return 1.0f;
        if (Gfx_TextureKind(stage) != 1)
            return 0.0f;
        const GxmTexture *g = (const GxmTexture *)Gfx_DrawnTexture(stage.texture)->gpu;
        return g != nullptr && !g->failed ? (float)g->minAlpha / 255.0f : 0.0f;
    }

    // Whether the alpha test of a 3D draw passes for each of its pixels: the
    // least alpha its stages can give is at least the test's threshold. Then
    // the test changes nothing.
    static bool AlphaTestAlwaysPasses(const GfxDrawState &s, const DrawRange &range)
    {
        if (Gfx_AlphaCompare(s) != GFX_ALPHA_COMPARE_AT_LEAST)
            return false;
        // The alpha of the diffuse colour, as fixed3d_v.cg gives it.
        float diffuse = (float)range.diffuseAlpha / 255.0f;
        if (s.lighting) {
            if (s.colorVertex && s.diffuseSource == 2)
                diffuse = (float)range.specularAlpha / 255.0f;
            else if (!s.colorVertex || s.diffuseSource != 1)
                diffuse = s.material.diffuse.a < 1.0f ? s.material.diffuse.a : 1.0f;
        }
        const float bound = Gfx_AlphaLowerBound(s, diffuse, StageTextureAlpha(s.stages[0]), StageTextureAlpha(s.stages[1]));
        // A margin for the rounding of the interpolated colour.
        return bound >= Gfx_AlphaThreshold(s) + 1.0f / 1024.0f;
    }

    // vertexOffset, indexOffset: the Execute's data in the ring (kFull: none).
    void Draw(const GfxCommand &c, GfxFrame &frame, unsigned vertexOffset, unsigned indexOffset,
              bool backBuffer, int targetW, int targetH)
    {
        SceGxmPrimitiveType primitive;
        if (!Primitive(c.primitive, &primitive)) {
            if (!loggedPrimitive)
                Sys_Log("gxm: primitive %d is not drawn", c.primitive);
            loggedPrimitive = true;
            return;
        }
        if (probe.mode == PROBE_NO_3D && !c.transformed)
            return;
        // The profile mode sums the time of each step of a 3D draw.
        const bool profiled = probe.mode == PROBE_PROFILE && !c.transformed;
        auto step = [&](int section) {
            if (!profiled)
                return;
            const uint64_t now = sceKernelGetProcessTimeWide();
            probe.section[section] += now - probe.mark;
            probe.mark = now;
        };
        if (profiled) {
            probe.mark = sceKernelGetProcessTimeWide();
            probe.draws3d++;
        }
        // The state of the draw, with the textures of the draw: the recorder
        // starts no new state for a new texture alone.
        GfxDrawState &drawState = frame.states[c.state];
        drawState.stages[0].texture = c.textures[0];
        drawState.stages[1].texture = c.textures[1];
        const GfxDrawState &s = drawState;
        if (viewportState != &s || viewportW != targetW || viewportH != targetH || viewportBackBuffer != backBuffer) {
            viewportOfState = ToTarget(s.viewport, s.screenW, s.screenH, backBuffer, targetW, targetH);
            viewportState = &s;
            viewportW = targetW;
            viewportH = targetH;
            viewportBackBuffer = backBuffer;
        }
        const GfxViewport &viewport = viewportOfState;
        // The vertex uniforms of the state: all rows at first, then only the
        // rows that differ from the 3D draw before.
        if (!c.transformed && (vertexUniformState != &s || vertexUniformW != targetW || vertexUniformH != targetH ||
                               vertexUniformBackBuffer != backBuffer)) {
            bool changed = true;
            if (vertexUniformState == nullptr || vertexUniformW != targetW || vertexUniformH != targetH ||
                vertexUniformBackBuffer != backBuffer)
                Gxm_FillVertexUniforms(s, viewport, vertexUniforms);
            else if (vertexUniformState->vertexVersion == s.vertexVersion)
                changed = false;    // the recorder says so: no look at the 1.3 KB of each state
            else
                changed = Gxm_UpdateVertexUniforms(*vertexUniformState, s, viewport, vertexUniforms);
            vertexUniformState = &s;
            vertexUniformW = targetW;
            vertexUniformH = targetH;
            vertexUniformBackBuffer = backBuffer;
            if (changed) {
                vertexUniformVersion++;
                stats.vertexFills++;
            }
        }
        step(PROFILE_STATE);
        uint64_t key = Gfx_FragmentKey(s, backBuffer && !c.transformed);
        const uint64_t args = Gfx_FragmentArgsKey(s);
        step(PROFILE_KEYS);
        int variant = -1;
        Resolved *resolved = nullptr;
        // The fade changes no pixel of a draw whose vertices are all nearer
        // than its start: such a draw takes the variant of its key without
        // the fade, which is much shorter.
        // The range of the draw's vertices: its box for the fade, its alpha
        // for the depth pass.
        const bool fadeKey = (key & kFadeKeyBit) != 0 && probe.mode != PROBE_FADE_ALWAYS;
        const bool depthCandidate = s.alphaBlend && s.alphaTest && s.depthTest && s.depthWrite &&
                                    probe.mode != PROBE_NO_DEPTH_PASS;
        // The single pass draws the same pixels with or without a discard
        // that never fails: there the least alpha of the draw is not asked.
        const bool single = probe.mode != PROBE_TWO_PASSES && (singlePass || probe.mode == PROBE_SINGLE_PASS);
        const bool needAlpha = depthCandidate && !single && probe.mode != PROBE_DEPTH_PASS_ALWAYS;
        DrawRange scratch;
        // The frame's own vertices come with their box, or with the reach of
        // a run of small draws (GfxCommand::hasBox). Without them, their
        // farthest vertex is measured.
        const bool ownVertices = c.buffer == nullptr;
        const bool ownKnown = c.hasBox;
        const DrawRange *range = (fadeKey && !ownKnown) || needAlpha
                                     ? RangeOf(c, frame, fadeKey && !ownVertices, needAlpha, scratch)
                                     : nullptr;
        if ((key & kFadeKeyBit) != 0) {
            float reach = 0.0f;
            bool known = false;
            if (fadeKey && ownKnown) {
                if (c.reach >= 0.0f) {
                    reach = c.reach;
                } else {
                    GxmBounds box;
                    memcpy(box.low, c.boxLow, sizeof(box.low));
                    memcpy(box.high, c.boxHigh, sizeof(box.high));
                    reach = Gxm_EyeDistanceBound(box, &vertexUniforms[VU_WORLD_VIEW]);
                }
                known = true;
            } else if (fadeKey && range != nullptr) {
                if (ownVertices) {
                    stats.ownDraws++;
                    stats.ownVertices += c.vertexCount;
                }
                reach = ownVertices ? Gxm_FarthestEyeDistance(frame.vertices.data() + c.firstVertexByte,
                                                              sizeof(GfxVertex), c.vertexCount,
                                                              &vertexUniforms[VU_WORLD_VIEW])
                                    : Gxm_EyeDistanceBound(range->box, &vertexUniforms[VU_WORLD_VIEW]);
                known = true;
            }
            const bool tooNear = known && reach < s.fadeStart;
            step(PROFILE_RANGE);
            if (tooNear)
                resolved = Resolve(key & ~kFadeKeyBit, args, s, c.transformed);
            if (resolved != nullptr) {
                key &= ~kFadeKeyBit;
                stats.nearDraws++;
            } else {
                stats.fadeDraws++;
            }
        }
        if (resolved == nullptr)
            resolved = Resolve(key, args, s, c.transformed);
        if (resolved == nullptr)
            return;
        variant = resolved->variant;
        SceGxmFragmentProgram *fragment = resolved->fragment;
        step(PROFILE_VARIANT);
        // A blended draw with the alpha test cannot discard (the SGX543), so
        // its program writes each pixel. Its depth must be written only where
        // the alpha test passes, as in Direct3D. Thus a pass without colour
        // and with the discard writes the depth first, and the blended pass
        // writes none. A draw whose alpha test passes for each pixel needs no
        // depth pass: its blended pass writes the same depth.
        int depthVariant = -1;
        SceGxmFragmentProgram *depthProgram = nullptr;
        if (depthCandidate && kGxmFragmentVariants[variant].alphaTest == 2) {
            if (needAlpha && range != nullptr && AlphaTestAlwaysPasses(s, *range)) {
                stats.depthSkipped++;
            } else {
                if (!resolved->depthLooked) {
                    resolved->depthLooked = true;
                    resolved->depthVariant = LookupVariant(key, 1, args);
                    if (resolved->depthVariant >= 0 && single)
                        resolved->single = FragmentProgram(resolved->depthVariant, true, s.srcBlend, s.destBlend,
                                                           c.transformed);
                    if (resolved->depthVariant >= 0 && resolved->single == nullptr)
                        resolved->depthProgram = FragmentProgram(resolved->depthVariant, false, 0, 0, c.transformed,
                                                                 true);
                }
                if (resolved->single != nullptr) {
                    // The single pass: the discard form with the draw's blending.
                    variant = resolved->depthVariant;
                    fragment = resolved->single;
                } else {
                    depthVariant = resolved->depthVariant;
                    depthProgram = resolved->depthProgram;
                }
            }
        }
        step(PROFILE_DEPTH);
        // Where the vertices and the indices are.
        const unsigned char *vertices = nullptr;
        if (c.snapshot) {
            // The snapshot starts at its first vertex: the buffer's vertex 0
            // would be before it.
            const GxmVertexBuffer *g = (const GxmVertexBuffer *)c.buffer->gpu;
            if (g != nullptr && g->snapshotExecute == executeCount && g->snapshotOffset != GxmFrameRing::kFull &&
                c.firstVertexByte >= g->snapshotFirst * sizeof(GfxVertex))
                vertices = memory.FrameRing().At(g->snapshotOffset) - g->snapshotFirst * sizeof(GfxVertex);
        } else if (c.buffer != nullptr) {
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
        if (vertices == nullptr || count == 0 || (c.firstIndex == ~0u && count > kIdentityIndices)) {
            stats.dropped++;
            return;
        }

        step(PROFILE_VERTICES);
        if (probe.mode == PROBE_LOOKUP_ONLY && !c.transformed)
            return;
        const bool scaled = backBuffer && s.screenW > 0 && s.screenH > 0;
        if (c.transformed)
            SetViewport(0.0f, 0.0f, (float)targetW, (float)targetH, 0.0f, 1.0f, viewport);
        else
            SetViewport((float)viewport.x, (float)viewport.y, (float)viewport.width, (float)viewport.height,
                        viewport.minZ, viewport.maxZ, viewport);
        SceGxmContext *context = device.Context();

        // One pass of the draw with a variant's program.
        auto pass = [&](int index, SceGxmFragmentProgram *program, SceGxmDepthFunc func, bool depthWrite) {
            SetPrograms(c.transformed ? vertexTL : vertex3d, program);
            SetDepthAndCull(func, depthWrite, CullMode(s.cull));
            // A default uniform buffer stays reserved and filled for the
            // draws that follow, while its program is set. Thus a pass
            // reserves and fills a buffer only when the program or a row is
            // new: most draws differ from the draw before only in their
            // textures.
            const bool always = probe.mode == PROBE_UNIFORMS_ALWAYS || !keepUniforms;
            const unsigned uploads = stats.vertexUploads + stats.fragmentUploads;
            void *buffer;
            if (c.transformed) {
                // Pre-transformed vertices are in pixels of the game's screen.
                const float target[4] = { (float)(scaled ? s.screenW : targetW),
                                          (float)(scaled ? s.screenH : targetH), 0.0f, 0.0f };
                if (always || !bound.vertexUniforms || bound.target[0] != target[0] || bound.target[1] != target[1]) {
                    sceGxmReserveVertexDefaultUniformBuffer(context, &buffer);
                    sceGxmSetUniformDataF(buffer, targetParam, 0, 4, target);
                    bound.vertexUniforms = true;
                    bound.target[0] = target[0];
                    bound.target[1] = target[1];
                    stats.vertexUploads++;
                }
            } else {
                // A draw without lighting reads only the rows before the lights.
                const unsigned rows = s.lighting ? vertexUniformRows : std::min<unsigned>(vertexUniformRows, VU_UNLIT_ROWS);
                if (always || !bound.vertexUniforms || bound.vertexVersion != vertexUniformVersion ||
                    bound.vertexRows < rows) {
                    sceGxmReserveVertexDefaultUniformBuffer(context, &buffer);
                    sceGxmSetUniformDataF(buffer, vertexUniformsParam, 0, rows * 4, vertexUniforms[0]);
                    bound.vertexUniforms = true;
                    bound.vertexVersion = vertexUniformVersion;
                    bound.vertexRows = rows;
                    stats.vertexUploads++;
                }
            }
            const Variant &v = variants[index];
            if (v.uniforms != nullptr) {
                const bool sizes = Gxm_KeyReadsTextureSize(kGxmFragmentVariants[index].key);
                if (sizes || fragmentRowsState != &s || fragmentRowsW != targetW || fragmentRowsH != targetH ||
                    fragmentRowsFade != backdropCopied) {
                    Gxm_FillFragmentUniforms(s, targetW, targetH, backdropCopied, fragmentRows);
                    fragmentRowsState = sizes ? nullptr : &s;
                    fragmentRowsW = targetW;
                    fragmentRowsH = targetH;
                    fragmentRowsFade = backdropCopied;
                    fragmentRowsVersion++;
                }
                const size_t bytes = std::min<size_t>(v.uniformRows, FU_ROWS) * sizeof(fragmentRows[0]);
                // The buffer of the program has the rows of this fill, or
                // rows that compare equal: then it stays.
                if (always || !bound.fragmentUniforms ||
                    (bound.fragmentVersion != fragmentRowsVersion && memcmp(bound.fragment, fragmentRows, bytes) != 0)) {
                    sceGxmReserveFragmentDefaultUniformBuffer(context, &buffer);
                    sceGxmSetUniformDataF(buffer, v.uniforms, 0, v.uniformRows * 4, fragmentRows[0]);
                    bound.fragmentUniforms = true;
                    memcpy(bound.fragment, fragmentRows, bytes);
                    stats.fragmentUploads++;
                }
                bound.fragmentVersion = fragmentRowsVersion;
            }
            BindTextures(s, kGxmFragmentVariants[index]);
            sceGxmSetVertexStream(context, 0, vertices + c.firstVertexByte);
            const bool kept = stats.vertexUploads + stats.fragmentUploads - uploads < (v.uniforms != nullptr ? 2u : 1u);
            if (!Check("sceGxmDraw", sceGxmDraw(context, primitive, SCE_GXM_INDEX_FORMAT_U16, indices, count)) &&
                kept && keepUniforms) {
                Sys_Log("gxm: a draw with a kept uniform buffer failed: each pass reserves its buffers from here");
                keepUniforms = false;
                bound.vertexUniforms = bound.fragmentUniforms = false;
            }
            stats.draws++;
            probe.draws++;
        };
        SceGxmDepthFunc func = s.depthTest ? DepthFunc(s.depthFunc) : SCE_GXM_DEPTH_FUNC_ALWAYS;
        bool depthWrite = s.depthTest && s.depthWrite;
        if (depthProgram != nullptr) {
            pass(depthVariant, depthProgram, func, true);
            stats.depthPasses++;
            // The blended pass meets the depth its depth pass wrote.
            if (func == SCE_GXM_DEPTH_FUNC_LESS)
                func = SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
            depthWrite = false;
        }
        pass(variant, fragment, func, depthWrite);
        step(PROFILE_PASSES);
    }

    // ---- a system dialog (GxmDialogPresenter) ----

    void BeginDialog() override
    {
        // The worker of the threaded executor finishes its frame first.
        Gfx_WaitForExecutor();
        EndScene();
        device.Finish();
    }

    void DialogFrame() override
    {
        device.DialogFrame();
    }

    // ---- the backdrop and the readback ----

    // Whether the frame's back buffer commands copy the backdrop.
    static bool HasBackdrop(const GfxFrame &frame)
    {
        bool backBuffer = true;
        for (const GfxCommand &c : frame.commands) {
            if (c.type == GFX_CMD_TARGET)
                backBuffer = c.target == nullptr;
            else if (c.type == GFX_CMD_BACKDROP && backBuffer)
                return true;
        }
        return false;
    }

    // GFX_CMD_BACKDROP: the back buffer as drawn so far into the backdrop.
    // GXM cannot copy a surface inside a scene: the back buffer's scene ends
    // (it stores its colour, and its depth for the scene after), a scene on
    // the backdrop draws the back buffer through copy_f, and the next draw
    // begins the back buffer's scene again, which loads both.
    void CopyBackdrop()
    {
        if (backdrop.target == nullptr || backdropCopied || !BeginScene(nullptr))
            return;
        EndScene();
        SceGxmTexture source;
        if (!device.BackBufferTexture(&source))
            return;
        // A smaller backdrop: each of its pixels reads the middle of its 2x2
        // pixels of the back buffer, which the linear filter averages.
        const SceGxmTextureFilter filter =
            backdropScale > 1 ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
        sceGxmTextureSetMinFilter(&source, filter);
        sceGxmTextureSetMagFilter(&source, filter);
        bool timed = false;
        uint64_t start = 0;
#ifdef OPENCMR2_DEV
        // The time waits for the GPU twice: not with the stats off.
        if (presented >= nextBackdropTiming && statsWanted) {
            nextBackdropTiming = presented + kBackdropTimingFrames;
            timed = true;
            device.Finish();
            start = sceKernelGetProcessTimeWide();
        }
#endif
        SceGxmContext *context = device.Context();
        if (!Check("sceGxmBeginScene(backdrop)", sceGxmBeginScene(context, 0, backdrop.target, nullptr, nullptr,
                                                                  nullptr, &backdrop.color, nullptr)))
            return;
        bound = Bound();
        GfxViewport whole = {};
        whole.width = (DWORD)backdropW;
        whole.height = (DWORD)backdropH;
        whole.maxZ = 1.0f;
        SetPrograms(clearVertex, copyProgram);
        SetViewport(0.0f, 0.0f, (float)backdropW, (float)backdropH, 0.0f, 1.0f, whole);
        SetDepthAndCull(SCE_GXM_DEPTH_FUNC_ALWAYS, false, SCE_GXM_CULL_NONE);
        void *buffer;
        const float z[4] = {};
        sceGxmReserveVertexDefaultUniformBuffer(context, &buffer);
        sceGxmSetUniformDataF(buffer, clearDepthParam, 0, 4, z);
        const float invSize[4] = { 1.0f / backdropW, 1.0f / backdropH, 0.0f, 0.0f };
        sceGxmReserveFragmentDefaultUniformBuffer(context, &buffer);
        sceGxmSetUniformDataF(buffer, copyInvSizeParam, 0, 4, invSize);
        sceGxmSetFragmentTexture(context, 0, &source);
        Check("sceGxmDraw(backdrop)", sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
                                                 SCE_GXM_INDEX_FORMAT_U16, identityIndices, 4));
        Check("sceGxmEndScene(backdrop)", sceGxmEndScene(context, nullptr, nullptr));
        bound = Bound();
        backdropCopied = true;
        if (timed) {
            device.Finish();
            Sys_Log("gxm: backdrop copy %.2f ms on the GPU (%dx%d)",
                    (sceKernelGetProcessTimeWide() - start) / 1000.0, backdropW, backdropH);
        }
    }

    // Takes the pending readback when the GPU has done its frame. wait: the
    // display buffer is about to be drawn again, so it is taken now.
    void CollectRead(GfxReadback &readback, bool wait)
    {
        if (!pendingRead.active)
            return;
        if ((int32_t)(*notificationWord - pendingRead.value) < 0) {
            if (!wait)
                return;
            SceGxmNotification n;
            n.address = notificationWord;
            n.value = pendingRead.value;
            sceGxmNotificationWait(&n);
        }
        const GxmReadbackRect &r = pendingRead.rect;
        readback.pixels.resize((size_t)r.width * r.height);
        device.ReadPixels(pendingRead.buffer, r.x, r.y, r.width, r.height, readback.pixels.data());
        readback.readyWidth = r.width;
        readback.readyHeight = r.height;
        readback.ready = true;
        pendingRead.active = false;
    }

    void CollectReadback(GfxReadback &readback) override
    {
        if (ready)
            CollectRead(readback, false);
    }

    // ---- frame execution ----

    // What reads the recorder's objects: the textures and the vertex buffers
    // go to the GPU, the snapshots of the buffers into the ring, and the
    // readback is taken and asked for. With the threaded executor this runs
    // on the game's thread while no Execute runs, and Execute reads only the
    // frame (gfx_threaded.cpp).
    void Prepare(GfxFrame &frame, std::vector<GfxTexture *> &textures, GfxReadback &readback, bool present) override
    {
        if (!ready) {
            for (GfxTexture *t : textures)
                t->dirty = false;
            return;
        }
        const uint64_t entered = sceKernelGetProcessTimeWide();
        // The lines of the frames before: no Execute changes the counts now.
        if (present) {
            LogTextures();
            LogStats();
        }
        statsWanted = Gfx_PerfStatsWanted();
        if (!frameStarted)
            StartFrame();
        // The read of a display buffer is taken before it is drawn again,
        // else when the GPU has done it.
        CollectRead(readback, pendingRead.active && pendingRead.buffer == device.BackBuffer());
        CollectRead(readback, false);
        EnsureTextures(frame);
        gpuTextures.Upload(textures, presented);
        texturesChanged = gpuTextures.TakeChanged() || texturesChanged;
        UploadVertexBuffers(frame);
        // The part of each buffer locked recently that the frame draws, once.
        executeCount++;
        GxmFrameRing &ring = memory.FrameRing();
        for (const GfxSnapshot &snapshot : frame.snapshots) {
            GfxVertexBuffer *b = snapshot.buffer;
            GxmVertexBuffer *g = (GxmVertexBuffer *)b->gpu;
            if (g == nullptr)
                b->gpu = g = new GxmVertexBuffer;
            const unsigned bytes = (snapshot.end - snapshot.first) * (unsigned)sizeof(GfxVertex);
            g->snapshotExecute = executeCount;
            g->snapshotFirst = snapshot.first;
            g->snapshotOffset = snapshot.end <= b->vertices.size() ? ring.Alloc(bytes, 16) : GxmFrameRing::kFull;
            if (g->snapshotOffset != GxmFrameRing::kFull) {
                memcpy(ring.At(g->snapshotOffset), &b->vertices[snapshot.first], bytes);
                stats.snapshotBytes += bytes;
            } else if (!loggedRing) {
                Sys_Log("gxm: a frame is larger than its ring slot: its draws are dropped");
                loggedRing = true;
            }
        }
        // The readback of this frame: a rectangle of its display buffer, read
        // when the GPU has done it (the lens flare test of a later frame
        // takes it, as on the PC). A read that the GPU has not done yet
        // stays, and the game gets its pixels a frame later: a wait for it
        // here would stop the game until the GPU has drawn the frame before.
        nextRead = GxmReadbackRect();
        if (present && readback.requested) {
            if (!pendingRead.active)
                nextRead = Gxm_ClampReadback(readback.x, readback.y, readback.width, readback.height, device.Width(),
                                             device.Height());
            readback.requested = false;
        }
        stats.prepareMicros += (unsigned)(sceKernelGetProcessTimeWide() - entered);
    }

    // The frame's commands, and with present the frame on the display. Reads
    // only the frame and the executor's own objects (see Prepare).
    void Execute(GfxFrame &frame, std::vector<GfxTexture *> &, GfxReadback &, bool present, bool vsync,
                 bool) override
    {
        if (!ready)
            return;
        const uint64_t entered = sceKernelGetProcessTimeWide();
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
        backdropAhead = HasBackdrop(frame);
        if (probe.mode == PROBE_PROFILE)
            probe.section[PROFILE_SETUP] += sceKernelGetProcessTimeWide() - entered;
        for (int phase = 0; phase < 2; phase++) {
            GfxTexture *target = nullptr;
            const GxmTexture *targetTexture = nullptr;
            int targetFace = 0;
            for (const GfxCommand &c : frame.commands) {
                if (c.type == GFX_CMD_TARGET) {
                    target = c.target;
                    targetTexture = nullptr;
                    targetFace = target != nullptr && target->cube != nullptr ? target->face : 0;
                    if (target != nullptr)
                        targetTexture = (const GxmTexture *)(target->cube != nullptr ? target->cube : target)->gpu;
                    continue;
                }
                if ((target != nullptr) != (phase == 0))
                    continue;
                if (c.type == GFX_CMD_BACKDROP) {
                    if (target == nullptr)
                        CopyBackdrop();
                    continue;
                }
                // A target with no GPU surface is not drawn.
                if (target != nullptr && (targetTexture == nullptr || targetTexture->target == nullptr))
                    continue;
                if (!BeginScene(targetTexture, targetFace))
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
        viewportState = nullptr;
        fragmentRowsState = nullptr;
        probe.submitMicros += sceKernelGetProcessTimeWide() - entered;
        if (!present) {
            stats.executeMicros += (unsigned)(sceKernelGetProcessTimeWide() - entered);
            return;
        }
        const GxmReadbackRect read = nextRead;
        if (!loggedInit) {
            Sys_Log("gxm: init ok %dx%d vsync %d, %d memblocks", device.Width(), device.Height(), vsync ? 1 : 0,
                    Gxm_LiveBlocks());
            loggedInit = true;
        }
        if (!BeginScene(nullptr))
            return;
        DrawFps();
        SceGxmNotification notification;
        const SceGxmNotification *fragment = nullptr;
        if (read.width > 0 && read.height > 0) {
            notification.address = notificationWord;
            notification.value = ++notificationValue;
            fragment = &notification;
            pendingRead.active = true;
            pendingRead.buffer = device.BackBuffer();
            pendingRead.rect = read;
            pendingRead.value = notification.value;
        }
        EndScene(fragment);
        backdropAhead = false;
        ProbeCheckFrame();
        // The time of the frame's commands, without the wait of the present.
        stats.executeMicros += (unsigned)(sceKernelGetProcessTimeWide() - entered);
        stats.frames++;
        device.Present(vsync && probe.mode == PROBE_OFF);
        ProbeTick();
        presented++;
        frameStarted = false;
    }

    // The frame time probe, after each present (dev builds).
    // A frame of the single pass check: waits for the GPU, takes samples of
    // the back buffer, and counts those that differ from the frame before.
    void ProbeCheckFrame()
    {
#ifdef OPENCMR2_DEV
        if (probe.check == 0)
            return;
        device.Finish();
        const int w = device.Width(), h = device.Height();
        std::vector<uint32_t> &shot = probeShot[probe.check & 1];
        const std::vector<uint32_t> &before = probeShot[(probe.check + 1) & 1];
        std::vector<uint32_t> row((size_t)w);
        shot.clear();
        for (int y = kProbeCheckStep / 2; y < h; y += kProbeCheckStep) {
            device.ReadPixels(device.BackBuffer(), 0, y, w, 1, row.data());
            for (int x = kProbeCheckStep / 2; x < w; x += kProbeCheckStep)
                shot.push_back(row[(size_t)x]);
        }
        probe.checkSamples = (unsigned)shot.size();
        unsigned differ = 0;
        if (probe.check > 1 && before.size() == shot.size())
            for (size_t i = 0; i < shot.size(); i++) {
                const uint32_t a = shot[i], b = before[i];
                bool off = false;
                for (int shift = 0; shift < 24; shift += 8) {
                    const int d = (int)(a >> shift & 0xff) - (int)(b >> shift & 0xff);
                    off = off || d > kProbeCheckChannel || d < -kProbeCheckChannel;
                }
                differ += off ? 1 : 0;
            }
        probe.checkDiffer[probe.check - 1] = differ;
#endif
    }

    // The end of the comparison of the single pass with the two passes.
    void ProbeCheckResult()
    {
#ifdef OPENCMR2_DEV
        Sys_Log("gxm probe: single pass check: of %u samples differ %u between two frames with two passes, %u "
                "between a frame with two passes and one with the single pass, %u between two with the single pass",
                probe.checkSamples, probe.checkDiffer[1], probe.checkDiffer[2], probe.checkDiffer[3]);
#endif
    }

    void ProbeTick()
    {
#ifdef OPENCMR2_DEV
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (probe.mode == PROBE_OFF) {
            if (!probe.touchStarted) {
                sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
                probe.touchStarted = true;
            }
            SceTouchData touch{};
            const bool down = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 1 && touch.reportNum > 0;
            const bool tapped = down && !probe.touchDown;
            probe.touchDown = down;
            if (!tapped)
                return;
            const bool touchStarted = probe.touchStarted;
            probe = Probe();
            probe.touchStarted = touchStarted;
            probe.touchDown = true;
            probe.mode = PROBE_TWO_PASSES;
            probe.check = 1;
            probe.lastPresent = now;
            Sys_Log("gxm probe: start, the single pass check, then %u modes take turns of %u frames, %u times, "
                    "no vsync",
                    kProbeCycleModes, kProbeBlockFrames, kProbeCycles);
            return;
        }
        if (probe.check != 0) {
            // The check's frames: two with two passes, then two with the single pass.
            probe.lastPresent = now;
            probe.submitMicros = 0;
            probe.draws = probe.draws3d = 0;
            memset(probe.section, 0, sizeof(probe.section));
            if (probe.check < kProbeCheckFrames) {
                probe.check++;
                probe.mode = probe.check > kProbeCheckFrames / 2 ? PROBE_SINGLE_PASS : PROBE_TWO_PASSES;
                return;
            }
            probe.check = 0;
            ProbeCheckResult();
            probe.mode = kProbeCycle[0];
            return;
        }
        const uint64_t frame = now - probe.lastPresent;
        probe.lastPresent = now;
        if (probe.blockFrame >= kProbeSkipFrames) {
            ProbeSums &sums = probe.sums[probe.mode];
            sums.frameMicros += frame;
            sums.frameMax = frame > sums.frameMax ? frame : sums.frameMax;
            sums.submitMicros += probe.submitMicros;
            sums.frames++;
            sums.draws += probe.draws;
            sums.draws3d += probe.draws3d;
            for (int i = 0; i < PROFILE_SECTIONS; i++)
                sums.section[i] += probe.section[i];
        }
        probe.submitMicros = 0;
        probe.draws = probe.draws3d = 0;
        memset(probe.section, 0, sizeof(probe.section));
        if (++probe.blockFrame < kProbeBlockFrames)
            return;
        probe.blockFrame = 0;
        if (++probe.slot == kProbeCycleModes) {
            probe.slot = 0;
            probe.cycle++;
        }
        if (probe.cycle < kProbeCycles) {
            probe.mode = kProbeCycle[probe.slot];
            return;
        }
        for (int mode : kProbeCycle) {
            const ProbeSums &sums = probe.sums[mode];
            const unsigned frames = sums.frames > 0 ? sums.frames : 1;
            Sys_Log("gxm probe: %s: frame ms avg %.2f max %.2f over %u frames, %u draws per frame, draw calls %.2f ms",
                    kProbeNames[mode], sums.frameMicros / 1000.0 / frames, sums.frameMax / 1000.0, sums.frames,
                    sums.draws / frames, sums.submitMicros / 1000.0 / frames);
            if (mode != PROBE_PROFILE)
                continue;
            // What a time read costs: the steps of each draw have 7 of them.
            const uint64_t t0 = sceKernelGetProcessTimeWide();
            uint64_t t1 = t0;
            for (int i = 0; i < 256; i++)
                t1 = sceKernelGetProcessTimeWide();
            const double perFrame = 1000.0 * frames;
            Sys_Log("gxm probe: profile ms per frame: setup %.2f, state %.2f, keys %.2f, range %.2f, variant %.2f, "
                    "depth %.2f, vertices %.2f, passes %.2f (%u 3D draws per frame, a time read %.2f us)",
                    sums.section[PROFILE_SETUP] / perFrame, sums.section[PROFILE_STATE] / perFrame,
                    sums.section[PROFILE_KEYS] / perFrame, sums.section[PROFILE_RANGE] / perFrame,
                    sums.section[PROFILE_VARIANT] / perFrame, sums.section[PROFILE_DEPTH] / perFrame,
                    sums.section[PROFILE_VERTICES] / perFrame, sums.section[PROFILE_PASSES] / perFrame,
                    sums.draws3d / frames, (double)(t1 - t0) / 256.0);
        }
        probe.mode = PROBE_OFF;
        Sys_Log("gxm probe: done");
#endif
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
        // With the stats off, no line: a period starts when they are on again.
        if (!Gfx_PerfStatsWanted()) {
            statsStart = 0;
            stats = Stats();
            return;
        }
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (statsStart == 0)
            statsStart = now;
        if (now - statsStart < kStatsMicros)
            return;
        Sys_Log("gxm: per s draws %u (depth passes %u, not needed %u, fade %u, near %u), patcher misses %u, "
                "vertex fills %u, uniform uploads %u vertex %u fragment, texture binds %u, dropped %u",
                stats.draws, stats.depthPasses, stats.depthSkipped, stats.fadeDraws, stats.nearDraws, stats.misses,
                stats.vertexFills, stats.vertexUploads, stats.fragmentUploads, stats.textureBinds, stats.dropped);
        // The ranges measured: those of vertex buffers that the cache did
        // not have, and the frame's own vertices of each draw with the fade.
        char recorder[512];
        Gfx_RecorderCountsText(recorder, sizeof(recorder));
        Sys_Log("gxm: per s ranges measured %u with %u vertices, own vertices walked %u in %u draws, snapshots %u KB; "
                "recorder: %s",
                stats.rangesMeasured, stats.rangeVertices, stats.ownVertices, stats.ownDraws, stats.snapshotBytes / 1024,
                recorder);
        // Prepare runs on the game's thread; Execute on the worker with the
        // threaded executor.
        const double frames = stats.frames > 0 ? (double)stats.frames : 1.0;
        Sys_Log("gxm: per frame ms: prepare %.2f, execute %.2f (without the wait of the present); per s binds of "
                "made mip levels: %u read, %u not read (no mip filter)",
                stats.prepareMicros / 1000.0 / frames, stats.executeMicros / 1000.0 / frames, stats.mipReads,
                stats.mipUnread);
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
