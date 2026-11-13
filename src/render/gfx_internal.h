// Internals of the renderer: the state gfx_core.cpp records and the
// executor that turns a recorded frame into GPU work (gfx_gpu.cpp on
// SDL_GPU, gfx_gl.cpp on OpenGL 3.3, or the null one for headless runs).
#ifndef OPENCMR2_GFX_INTERNAL_H
#define OPENCMR2_GFX_INTERNAL_H

#include "port/gfx.h"
#include "render/gxm_key_cache.h"
#include "render/texture_mips.h"

#include <stdint.h>

#include <memory>
#include <vector>

// ---- textures and buffers --------------------------------------------------------

struct GfxTexture {
    int width, height;
    int format;                 // GFX_FORMAT_*
    int mipLevels;
    DWORD flags;
    GfxTexture *cube;           // a face view: the cube it belongs to
    int face;                   // a face view: its face
    // What an executor reads for each draw is here, before the levels: one
    // cache line for a texture, not two a kilobyte apart.
    bool dirty;                 // CPU copy newer than the GPU's
    bool renderedTo;            // the GPU copy is newer (render targets)
    void *gpu;                  // executor's object
    GfxTexture *replacement;    // drawn instead (Gfx_SetTextureReplacement)
    GfxTexture *faceViews[6];   // a cube: its face views (1..5; 0 is itself)
    // CPU copy of every level of every face, tightly packed rows.
    std::vector<uint8_t> levels[6][16];
    // The mip levels an executor makes of a BC3 texture of one level
    // (texture_mips.h): at most mipLimit (0: down to 1x1), and the part of
    // level 0 that changed since the last upload (empty: all of it). The
    // atlas of the small quads keeps 3 levels: the border of a texture in it
    // is 4 texels.
    int mipLimit = 0;
    GfxMipRect changedRect;
};

// The texture a draw reads for t: its cube, or its replacement.
inline GfxTexture *Gfx_DrawnTexture(GfxTexture *t)
{
    if (t != nullptr && t->cube != nullptr)
        t = t->cube;
    return t != nullptr && t->replacement != nullptr ? t->replacement : t;
}

// A vertex buffer the game fills through Lock/Unlock. Draws from one that
// has not been locked for a while refer to its GPU copy (stage geometry);
// draws from one locked recently copy their vertices (cars, sprites), as the
// game rewrites those during the frame.
// The box of a set of positions.
struct GfxBox {
    float low[3], high[3];
};

struct GfxVertexBuffer {
    std::vector<GfxVertex> vertices;
    uint32_t version = 1;           // bumped by every lock
    uint32_t lastLockFrame = 0;     // presented frame of the last lock
    uint32_t referencedRecord = 0;  // recording that last drew from it
    void *gpu = nullptr;            // executor's copy
    // The recorder's boxes of vertex ranges (first vertex << 32 | count),
    // for the test of a draw against the view. They stay while the
    // positions stay: with the stage lighting the game writes the colours
    // of the ground meshes again in each frame, and not their positions. A
    // lock makes the positions unknown. The first box after it compares a
    // hash of all positions with positionHash.
    std::unique_ptr<GxmKeyCache<GfxBox, 6>> boxes;
    uint64_t positionHash = 0;
    bool positionsUnknown = true;
    // For an executor that takes joined draws: the frame has one copy of
    // the part of a buffer locked recently that its draws use, made by the
    // executor (GfxFrame::snapshots). A buffer that the game locks again
    // after a draw from it in one recording is streaming: each of its draws
    // copies its own vertices into the frame's data.
    bool streaming = false;
    uint32_t snapshotRecord = 0;    // the recording whose frame has a snapshot of it
    uint32_t snapshotIndex = 0;     // that snapshot in GfxFrame::snapshots
};

// Bytes of one level, given its size, for a format.
size_t Gfx_LevelSize(int format, int width, int height, LONG *pitch);
// Bytes per pixel of the CPU copy (0 for compressed formats).
int Gfx_BytesPerPixel(int format);

// ---- recorded state ---------------------------------------------------------------

struct GfxStageState {
    int colorOp = GFX_TOP_MODULATE, colorArg1 = GFX_TA_TEXTURE, colorArg2 = GFX_TA_CURRENT;
    int alphaOp = GFX_TOP_SELECTARG1, alphaArg1 = GFX_TA_TEXTURE, alphaArg2 = GFX_TA_CURRENT;
    int address = GFX_ADDRESS_WRAP;
    int minFilter = GFX_FILTER_POINT, magFilter = GFX_FILTER_POINT, mipFilter = GFX_MIPFILTER_NONE;
    int maxMipLevel = 0;
    DWORD texCoordIndex = 0;
    int textureTransform = GFX_TTFF_DISABLE;
    float bumpMat[4] = { 0, 0, 0, 0 };
    float bumpLumScale = 1.0f, bumpLumOffset = 0.0f;
    GfxTexture *texture = nullptr;
};

// Everything a draw depends on, copied into each recorded draw.
struct GfxDrawState {
    GfxMatrix world, view, projection, texture[2];
    GfxLight lights[8];
    bool lightEnabled[8] = {};
    GfxMaterial material;
    bool lighting = true;
    DWORD ambient = 0;
    bool specular = false;
    bool localViewer = true;
    bool normalizeNormals = false;
    bool colorVertex = true;
    int diffuseSource = GFX_MCS_COLOR1, specularSource = GFX_MCS_COLOR2;
    int ambientSource = GFX_MCS_MATERIAL, emissiveSource = GFX_MCS_MATERIAL;
    bool fog = false;
    DWORD fogColor = 0;
    int fogVertexMode = GFX_FOG_NONE, fogTableMode = GFX_FOG_NONE;
    float fogStart = 0.0f, fogEnd = 1.0f;
    float fadeStart = 0.0f, fadeEnd = 0.0f;   // Gfx_SetDistanceFade (end <= start: off)
    bool depthTest = true, depthWrite = true;
    int depthFunc = GFX_CMP_LESSEQUAL;
    int cull = GFX_CULL_CCW;
    bool alphaBlend = false;
    int srcBlend = GFX_BLEND_ONE, destBlend = GFX_BLEND_ZERO;
    bool alphaTest = false;
    int alphaFunc = GFX_CMP_ALWAYS;
    DWORD alphaRef = 0;
    DWORD textureFactor = 0xffffffff;
    GfxStageState stages[2];
    GfxViewport viewport;
    int screenW = 0, screenH = 0;   // Gfx_SetVirtualScreen (0: the back buffer's size)
    // In a recorded state: a number that goes up when a part that a vertex
    // program reads changes (the transforms, the lights, the material, the
    // fog, the texture coordinate sources, the viewport). Two recorded states
    // with one number have those parts equal: an executor can tell so
    // without a look at them. Most draws of a stage differ from the draw
    // before only in their textures.
    uint32_t vertexVersion = 0;
};

enum GfxCommandType {
    GFX_CMD_DRAW,
    GFX_CMD_CLEAR,
    GFX_CMD_TARGET,
    GFX_CMD_BACKDROP,           // copy the back buffer for the distance fade
    // Small 3D draws of a batch (Gfx_DrawBatch), as the game made them:
    // Gfx_ExpandSmallBatches makes them draws before an executor runs the
    // frame. An executor never gets this command.
    GFX_CMD_SMALL_BATCH
};

struct GfxCommand {
    GfxCommandType type;
    // draw
    int primitive;              // list/strip types only (fans are converted)
    bool transformed;           // GfxTLVertex data
    GfxVertexBuffer *buffer;    // vertices in this buffer, else in the frame's data
    uint32_t firstVertexByte;   // into the buffer or the frame's vertex data,
                                // a multiple of the vertex size
    uint32_t vertexCount;
    uint32_t firstIndex;        // into the frame's indices, or ~0 for none
    uint32_t indexCount;
    uint32_t state;             // index into the frame's states
    // The textures of the draw's two stages. For an executor that takes
    // joined draws, a new texture alone starts no new state: most draws of a
    // stage differ from the draw before only in their textures, and a state
    // is 1.3 KB. There the textures of a draw are these, not those of its
    // state.
    GfxTexture *textures[2];
    // The vertices are in the frame's snapshot of buffer
    // (GfxFrame::snapshots); firstVertexByte is into the buffer.
    bool snapshot;
    // How far the vertices of a 3D draw are, for an executor that takes
    // joined draws: the box of the vertices in their own space (hasBox), and
    // for a run of small draws in world space also the eye distance of its
    // farthest vertex (reach, else below zero). The recorder finds them
    // while it copies the vertices or tests them against the view. The GXM
    // executor then does not walk the vertices for the distance fade: in a
    // frame with the stage lighting, the game writes the colours of the
    // ground meshes again in each frame, and their copies were 80,000
    // vertices.
    bool hasBox;
    float boxLow[3], boxHigh[3];
    float reach;
    // small batch: the items firstItem.. of the frame's smallItems, all with
    // the state (in world space) and textures[1]. cull: they are tested
    // against the view.
    uint32_t firstItem, itemCount;
    bool cull;
    // clear
    DWORD clearFlags;
    DWORD clearColor;
    float clearZ;
    GfxViewport clearViewport;
    int clearScreenW, clearScreenH; // the draw state's screenW, screenH
    // target
    GfxTexture *target;         // NULL: the back buffer
};

// The vertices first..end-1 of a vertex buffer, which the executor copies
// once for the frame's draws from it (GfxVertexBuffer::snapshotRecord).
struct GfxSnapshot {
    GfxVertexBuffer *buffer;
    uint32_t first, end;
};

// A small 3D draw of a batch (GFX_CMD_SMALL_BATCH), as the game made it: its
// vertices in their own space, its world matrix, and the texture of stage 0.
// A draw from the atlas has its place in the atlas (atlasScale is 1 / the
// size of the atlas, 0 for a draw with its own texture).
struct GfxSmallItem {
    GfxMatrix world;
    GfxTexture *texture;
    int atlasX, atlasY, textureWidth, textureHeight;
    float atlasScale;
    uint32_t firstVertex, vertexCount;      // into GfxFrame::smallVertices
    uint32_t firstIndex, indexCount;        // into GfxFrame::smallIndices
};

struct GfxFrame {
    std::vector<uint8_t> vertices;
    std::vector<uint16_t> indices;
    std::vector<GfxDrawState> states;
    std::vector<GfxCommand> commands;
    std::vector<GfxVertexBuffer *> buffers;     // drawn from, each once
    std::vector<GfxSnapshot> snapshots;         // each buffer once
    // The small batches: their draws, vertices and indices, and the textures
    // the draws read (each once), for the executor to make ready before the
    // batches are draws.
    std::vector<GfxSmallItem> smallItems;
    std::vector<GfxVertex> smallVertices;
    std::vector<uint16_t> smallIndices;
    std::vector<GfxTexture *> smallTextures;
    void Reset()
    {
        vertices.clear();
        indices.clear();
        states.clear();
        commands.clear();
        buffers.clear();
        snapshots.clear();
        smallItems.clear();
        smallVertices.clear();
        smallIndices.clear();
        smallTextures.clear();
    }
};

// Makes the small batches of a frame (GFX_CMD_SMALL_BATCH) draws of the
// frame's vertex data, in their place among the commands: the vertices in
// world space, the draws outside the view left out, the coordinates of the
// atlas, and the draws of one texture joined, as the recorder does it for a
// small draw made alone (gfx_small_draw.h). Reads only the frame.
void Gfx_ExpandSmallBatches(GfxFrame &frame);

struct GfxReadback {
    bool requested = false;
    int x = 0, y = 0, width = 0, height = 0;
    bool ready = false;
    int readyWidth = 0, readyHeight = 0;
    std::vector<uint32_t> pixels;   // 0xAARRGGBB
};

// ---- executor -----------------------------------------------------------------------

// The times of the threaded executor, in us (GfxExecutor::NoteWorkerTimes).
struct GfxWorkerTimes {
    // The waits of the game's thread for the worker: before the Execute of a
    // present, before an Execute in the middle of a frame (the recorder ran
    // the draws so far: a lock of what they use), and in other calls.
    unsigned presentWait = 0, flushWait = 0, otherWait = 0;
    unsigned expand = 0;    // the worker's expand of the small batches of the last Execute
};

struct GfxExecutor {
    virtual ~GfxExecutor() {}
    virtual bool SetVideoMode(int width, int height, bool fullscreen) = 0;
    virtual void SetFullscreen(bool fullscreen) = 0;
    // Called before each Execute, on the recorder's thread, while no Execute
    // runs: the part of the work that reads the recorder's objects (the
    // textures, the vertex buffers, the readback). With the threaded
    // executor (Gfx_CreateThreadedExecutor) Execute runs on a worker thread
    // and reads only the frame.
    virtual void Prepare(GfxFrame &, std::vector<GfxTexture *> &, GfxReadback &, bool) {}
    // Runs the recorded commands; with present, shows the back buffer and
    // ends the frame (otherwise the next commands continue it).
    virtual void Execute(GfxFrame &frame, std::vector<GfxTexture *> &textures, GfxReadback &readback, bool present,
                         bool vsync, bool stretch = false) = 0;
    // Returns when no Execute runs (the threaded executor's worker is idle).
    virtual void Wait() {}
    // For the logs, from the threaded executor before each Prepare: what
    // happened since the call before.
    virtual void NoteWorkerTimes(const GfxWorkerTimes &) {}
    virtual void DestroyTexture(GfxTexture *texture) = 0;
    virtual void DestroyVertexBuffer(GfxVertexBuffer *) {}
    // Whether the recorder joins consecutive draws of one state from the
    // frame's vertex data into one command (a run of sprites or lines),
    // records small 3D draws in world space, starts no new state for a new
    // texture alone (GfxCommand::textures), and draws small quads from an
    // atlas of their textures.
    virtual bool JoinsDraws() const { return false; }
    // Brings a render target's pixels back into its CPU copy (for locks).
    virtual void ReadTexture(GfxTexture *texture) = 0;
    // Completes the back buffer read the last presented frame started, when
    // the game asks for its pixels (Gfx_ReadBackBuffer).
    virtual void CollectReadback(GfxReadback &) {}
};

// A text for the logs: what the recorder did with the small 3D draws since
// the last call (how many joined the draw before, how many drew from the
// atlas, and why the others did not). The recorder counts from the first
// call on. Only an executor that takes joined draws has such draws.
void Gfx_RecorderCountsText(char *text, size_t size);

// Whether the performance stats of a development build are on
// (video.perf_stats, read at each present). Off, the recorder counts nothing
// for the logs.
bool Gfx_PerfStatsWanted(void);

// An executor that runs inner's Execute on a worker thread while the game
// records the next frame (gfx_threaded.cpp). It owns inner. Without a worker
// thread it gives inner back. [video] threaded_render (1 on the PS Vita, 0
// elsewhere: the OpenGL and SDL_GPU executors draw from the main thread).
GfxExecutor *Gfx_CreateThreadedExecutor(GfxExecutor *inner);
// Returns when the executor runs no frame: for code that uses the GPU
// outside the executor (a system dialog). From the worker it returns at once.
void Gfx_WaitForExecutor(void);

GfxExecutor *Gfx_CreateGpuExecutor(void);
GfxExecutor *Gfx_CreateNullExecutor(void);
#ifdef VITA
GfxExecutor *Gfx_CreateGxmExecutor(void);
#endif
// Whether SDL_GPU has a hardware device on this system (gfx_gpu.cpp).
bool Gfx_GpuIsAvailable(void);
#ifdef OPENCMR2_RENDERER_GL
GfxExecutor *Gfx_CreateGlExecutor(void);
// Sets up an OpenGL 3.3 context's attributes; returns the window flags it
// needs (gfx_gl.cpp).
unsigned long long Gfx_PrepareGlWindow(void);
#endif

// Shared by the executors (gfx_core.cpp).
struct SDL_Window;
// Fullscreen on the desktop resolution, or a centred window of the back
// buffer's size times video.window_scale.
void Gfx_SetWindowMode(SDL_Window *window, bool fullscreen, int width, int height);
// OPENCMR2_FRAME_HASH=file: one line per presented frame, its number, size
// and a hash of the back buffer's pixels, to compare renderers and renderer
// changes; OPENCMR2_FRAME_DUMP=dir also saves every OPENCMR2_FRAME_DUMP_EVERY'th
// frame (default 600) as a BMP. Reading the pixels waits for the GPU: not for
// timing.
bool Gfx_FrameHashWanted(void);
void Gfx_HashFrame(const uint8_t *bgra, int width, int height);

// Decodes a BC1/BC3 4x4 block into 16 BGRA pixels.
void Gfx_DecodeBlock(int format, const uint8_t *block, uint32_t out[16]);

#endif
