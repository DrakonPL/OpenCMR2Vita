// The renderer's front end: the gfx.h state machine, CPU copies of textures
// and vertex buffers, and the recording of each frame for the executor.

#ifdef VITA
#include "platform/platform_vita.h"
#else
#include "platform/platform.h"
#endif
#include "port/sys.h"
#include "port/diagnostics.h"
#include "render/gfx_internal.h"
#include "render/gfx_small_draw.h"
#include "render/texture_atlas.h"
#include "render/shader_key.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "render/gfx_batch_worker.h"

static void AddPendingBatchJob();

// The part of the scene that runs now (GfxCommand::part).
static inline uint8_t ScenePartNow()
{
    return Diagnostics::observer != nullptr ? (uint8_t)Diagnostics::observer->Part() : 0;
}
static void SettleBatches(GfxVertexBuffer *buffer, uint32_t first, uint32_t count);

// Why the recorder runs the draws recorded so far in the middle of a frame.
enum {
    FLUSH_ATLAS,            // a texture of the atlas goes or changes
    FLUSH_BUFFER_GONE,      // a vertex buffer that the draws use goes
    FLUSH_BUFFER_LOCK,      // a vertex buffer that the draws use is locked
    FLUSH_TEXTURE_LOCK,     // a texture that the draws use is locked
    FLUSH_REPLACEMENT,      // a texture gets another replacement
    FLUSH_COUNT
};
static void Flush(int cause);

namespace {

GfxExecutor *s_executor;
bool s_threaded;        // s_executor runs its frames on a worker thread
GfxDrawState s_state;
bool s_stateDirty = true;
// A part of the state that a vertex program reads changed since the state
// recorded last (GfxDrawState::vertexVersion), and the number of that state.
bool s_vertexDirty = true;
uint32_t s_vertexVersion;
GfxFrame s_frame;
GfxReadback s_readback;
std::vector<GfxTexture *> s_textures;     // every live texture (not face views)
int s_width = 640, s_height = 480;
GfxTexture *s_target;
std::vector<std::pair<GfxDrawState, GfxTexture *>> s_savedStates;
std::vector<std::pair<int, int>> s_modes;
uint32_t s_presentedFrames = 1;     // frames presented (vertex buffer ages)
uint32_t s_record = 1;              // recordings run (presented or flushed)

// Frames a vertex buffer stays out of locks before draws refer to it.
const uint32_t kStaticAfterFrames = 4;

GfxMatrix Identity()
{
    GfxMatrix m;
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

void ResetState()
{
    s_state = GfxDrawState();
    s_state.world = s_state.view = s_state.projection = Identity();
    s_state.texture[0] = s_state.texture[1] = Identity();
    memset(s_state.lights, 0, sizeof(s_state.lights));
    memset(&s_state.material, 0, sizeof(s_state.material));
    s_state.stages[1].colorOp = GFX_TOP_DISABLE;
    s_state.stages[1].alphaOp = GFX_TOP_DISABLE;
    s_state.stages[1].texCoordIndex = 1;
    s_state.viewport.x = s_state.viewport.y = 0;
    s_state.viewport.width = s_width;
    s_state.viewport.height = s_height;
    s_state.viewport.minZ = 0.0f;
    s_state.viewport.maxZ = 1.0f;
    s_stateDirty = true;
    s_vertexDirty = true;
}

inline void Touch()
{
    s_stateDirty = true;
    s_vertexDirty = true;
}

const GfxMatrix &RecordedWorldOf(bool worldSpace);
bool StateIsRecorded(bool worldSpace);

// Sets a part of the state. A value that is already set leaves the state as
// it is recorded: the next draw then has the state of the draw before, and
// the executor does not set it up again.
template <typename T> inline void Set(T &field, const T &value)
{
    if (memcmp(&field, &value, sizeof(T)) != 0) {
        field = value;
        Touch();
    }
}

// Sets a part of the state that no vertex program reads: the textures, the
// stage operations, the blending, the tests of a pixel.
// tests/gfx_record.cpp checks the choice against the vertex uniforms.
template <typename T> inline void SetPixels(T &field, const T &value)
{
    if (memcmp(&field, &value, sizeof(T)) != 0) {
        field = value;
        s_stateDirty = true;
    }
}

// The vertices of a draw from a vertex buffer that is not drawn by reference
// (the game locks it each frame): the range that was copied into the frame
// last. A draw from inside that range takes the copy.
struct CopiedRange {
    const GfxVertexBuffer *buffer = NULL;
    uint32_t version = 0, record = 0;
    DWORD first = 0, count = 0;
    uint32_t offset = 0;            // into the frame's vertex data
    float low[3] = {}, high[3] = {};    // the box of its positions
};
CopiedRange s_copied;

const DWORD kJoinedVertices = kGfxJoinedVertices;
const DWORD kSmallDrawVertices = kGfxSmallDrawVertices;

// The vertices of a small 3D draw in world space, for an executor that takes
// joined draws (gfx_small_draw.h). The game draws each trackside billboard as
// two triangles with a world matrix of its own, some 300 in a frame: in
// world space they have one state, and the executor sets up no matrix for
// each. False (the draw stays as it is) for a world matrix that is the
// identity or not affine.
bool SmallDrawToWorld(const GfxVertex *in, DWORD count, GfxVertex *out)
{
    if (!Gfx_SmallDrawWorld(s_state.world, RecordedWorldOf(true)))
        return false;
    Gfx_SmallDrawToWorld(s_state.world, in, count, out);
    return true;
}
// A strip of at most this many vertices is recorded as a list, which joins.
const DWORD kJoinedStrip = 6;

// ---- the counts for the logs (Gfx_RecorderCountsText) ----

// Why a small draw in world space did not join the draw before.
enum {
    NO_JOIN_STATE,          // the state changed
    NO_JOIN_PRIMITIVE,      // not a list
    NO_JOIN_COMMAND,        // the draw before is of another kind
    NO_JOIN_TEXTURE,        // another texture
    NO_JOIN_SIZE,           // too many vertices together
    NO_JOIN_DATA,           // the draw before does not end the frame's data
    NO_JOIN_COUNT
};
const char *const kNoJoinNames[NO_JOIN_COUNT] = { "state", "primitive", "command", "texture", "size", "data" };

// Why a small draw in world space did not draw from the atlas.
enum {
    NO_ATLAS_OFF,           // video.quad_atlas = 0
    NO_ATLAS_TEXTURE,       // no texture
    NO_ATLAS_STAGE1,        // a second texture
    NO_ATLAS_ADDRESS,       // not clamped
    NO_ATLAS_SOURCE,        // the coordinates are not those of the vertex
    NO_ATLAS_RANGE,         // coordinates outside the texture
    NO_ATLAS_FORMAT,        // not a plain BC3 texture of one level
    NO_ATLAS_LARGE,         // too large
    NO_ATLAS_ROOM,          // the atlas is full
    NO_ATLAS_COUNT
};
const char *const kNoAtlasNames[NO_ATLAS_COUNT] = { "off", "no texture", "stage 1", "address", "source", "range",
                                                    "format", "large", "room" };

// The parts of the state that changed for a small draw that did not join
// for its state. "none": the game set a value and set it back.
enum {
    CHANGED_NONE, CHANGED_FIRST, CHANGED_WORLD, CHANGED_VIEW, CHANGED_TEXTURE_MATRIX, CHANGED_LIGHTS,
    CHANGED_MATERIAL, CHANGED_LIGHTING, CHANGED_FOG, CHANGED_FADE, CHANGED_DEPTH, CHANGED_CULL, CHANGED_BLEND,
    CHANGED_ALPHA_TEST, CHANGED_FACTOR, CHANGED_STAGE0, CHANGED_STAGE1, CHANGED_VIEWPORT, CHANGED_COUNT
};
const char *const kChangedNames[CHANGED_COUNT] = { "none", "first", "world", "view", "texture matrix", "lights",
                                                   "material", "lighting", "fog", "fade", "depth", "cull", "blend",
                                                   "alpha test", "factor", "stage 0", "stage 1", "viewport" };

const char *const kFlushNames[FLUSH_COUNT] = { "atlas", "buffer gone", "buffer lock", "texture lock",
                                               "replacement" };

struct RecorderCounts {
    uint32_t states, small, joined, atlas, culled, snapshots, batched;
    uint32_t noJoin[NO_JOIN_COUNT], noAtlas[NO_ATLAS_COUNT], changed[CHANGED_COUNT], flushes[FLUSH_COUNT];
};
RecorderCounts s_counts;
bool s_countsWanted;    // a log reads the counts
int s_noJoin;           // why JoinDraw returned false
int s_noAtlas;          // why AtlasSmallDraw returned NULL

inline bool NoJoin(int reason)
{
    s_noJoin = reason;
    return false;
}


// ---- the atlas of the small quads ----
//
// The game draws each trackside tree as a quad with a world matrix and a
// texture of its own, sorted from far to near: some 300 draws in a frame of
// a wood, with 17 textures in a stage. In world space they have one state
// (SmallDrawToWorld). With their textures in one atlas they also have one
// texture: a run of them is one draw, in the game's order. The atlas is a
// BC3 texture that the recorder fills from the textures' own blocks, each
// with a border (texture_atlas.h), at a texture's first draw.

// The atlas starts with kAtlasSize texels on each side. With the border of
// each texture, that is room for nine textures of 256 x 256. An atlas that
// is full starts again with kAtlasLargeSize texels on each side in the next
// frame (room for 49 of them): the executor loads the whole atlas for each
// new texture in it, so the small atlas is first.
constexpr int kAtlasSize = 1024;
constexpr int kAtlasLargeSize = 2048;
constexpr int kAtlasTextureSize = 256;  // the largest texture that goes in

struct AtlasEntry {
    GfxTexture *texture;
    int x, y;       // the texel of the atlas where the texture starts
};
GfxTexture *s_atlas;
int s_atlasSize = kAtlasSize;   // texels on each side of the atlas, or of the next atlas
bool s_atlasFull;               // a texture found no room: the large atlas starts with the next frame
int s_atlasWanted = -1;     // video.quad_atlas (1 by default); -1: not read yet
AtlasPacker s_atlasPacker;
std::vector<AtlasEntry> s_atlasEntries;
uint32_t s_atlasRecord;     // the recording that last drew from the atlas

// The place of a texture in the atlas, or NULL where it does not go in: not
// a plain BC3 texture of one level, too large, or no room. A search of the
// places, and a new place for a texture that has none.
const AtlasEntry *FindAtlasEntry(GfxTexture *t)
{
    for (const AtlasEntry &e : s_atlasEntries)
        if (e.texture == t)
            return &e;
    s_noAtlas = NO_ATLAS_FORMAT;
    if (t->format != GFX_FORMAT_BC3 || t->mipLevels != 1 || t->flags != 0 || t->cube != NULL ||
        t->replacement != NULL)
        return NULL;
    s_noAtlas = NO_ATLAS_LARGE;
    if (t->width % 4 != 0 || t->height % 4 != 0 || t->width > kAtlasTextureSize || t->height > kAtlasTextureSize)
        return NULL;
    s_noAtlas = NO_ATLAS_ROOM;
    if (s_atlas == NULL) {
        s_atlas = Gfx_CreateTexture(s_atlasSize, s_atlasSize, GFX_FORMAT_BC3, 1, 0);
        if (s_atlas == NULL)
            return NULL;
        s_atlas->mipLimit = 3;
        s_atlasPacker.Reset(s_atlasSize / 4, s_atlasSize / 4);
    }
    const int w = t->width / 4, h = t->height / 4;
    int x, y;
    if (!s_atlasPacker.Place(w + 2, h + 2, &x, &y)) {
        s_atlasFull = s_atlasSize < kAtlasLargeSize;
        return NULL;
    }
    Atlas_CopyBC3Tile(t->levels[0][0].data(), w, h, s_atlas->levels[0][0].data(), s_atlasSize / 4, x, y);
    // The part of the atlas that changed: an executor makes the mip levels
    // of that part again (GfxTexture::changedRect). An executor without the
    // levels of the atlas yet makes all of them.
    GfxMipRect &c = s_atlas->changedRect;
    GfxMipRect tile;
    tile.x0 = x * 4;
    tile.y0 = y * 4;
    tile.x1 = (x + w + 2) * 4;
    tile.y1 = (y + h + 2) * 4;
    if (c.Empty()) {
        c = tile;
    } else {
        c.x0 = tile.x0 < c.x0 ? tile.x0 : c.x0;
        c.y0 = tile.y0 < c.y0 ? tile.y0 : c.y0;
        c.x1 = tile.x1 > c.x1 ? tile.x1 : c.x1;
        c.y1 = tile.y1 > c.y1 ? tile.y1 : c.y1;
    }
    s_atlas->dirty = true;
    s_atlasEntries.push_back({ t, (x + 1) * 4, (y + 1) * 4 });
    return &s_atlasEntries.back();
}

// Whether the state gives a small draw in world space the atlas: one clamped
// texture, with the coordinates of the vertex.
bool AtlasStateFits()
{
    if (s_atlasWanted < 0)
        s_atlasWanted = Platform_GetSettingInt("video.quad_atlas", 1) != 0 ? 1 : 0;
    s_noAtlas = NO_ATLAS_OFF;
    if (s_atlasWanted == 0)
        return false;
    const GfxStageState &stage = s_state.stages[0];
    s_noAtlas = NO_ATLAS_TEXTURE;
    if (stage.texture == NULL)
        return false;
    // A second stage that is off reads no texture.
    s_noAtlas = NO_ATLAS_STAGE1;
    if (s_state.stages[1].texture != NULL && s_state.stages[1].colorOp != GFX_TOP_DISABLE)
        return false;
    s_noAtlas = NO_ATLAS_ADDRESS;
    if (stage.address != GFX_ADDRESS_CLAMP)
        return false;
    s_noAtlas = NO_ATLAS_SOURCE;
    return stage.texCoordIndex == 0 && stage.textureTransform == GFX_TTFF_DISABLE;
}

// The places of the atlas as of this number: a texture keeps the answer of
// FindAtlasEntry while the number stays (GfxTexture::atlasSerial). It
// changes when the places go.
uint32_t s_atlasSerial = 1;

// FindAtlasEntry, with the answer that the texture keeps: the search for
// each small draw of a batch was a quarter of the time of its recording.
const AtlasEntry *AtlasEntryOf(GfxTexture *t)
{
    if (t->atlasSerial == s_atlasSerial) {
        if (t->atlasIndex >= 0)
            return &s_atlasEntries[(size_t)t->atlasIndex];
        s_noAtlas = t->atlasReason;
        return NULL;
    }
    const AtlasEntry *e = FindAtlasEntry(t);
    t->atlasSerial = s_atlasSerial;
    t->atlasIndex = e != NULL ? (int)(e - s_atlasEntries.data()) : -1;
    t->atlasReason = s_noAtlas;
    return e;
}

// The place of the texture of the state in the atlas, or NULL.
const AtlasEntry *AtlasEntryOfState()
{
    const AtlasEntry *e = AtlasEntryOf(s_state.stages[0].texture);
    if (e != NULL)
        s_atlasRecord = s_record;
    return e;
}

// The place in the atlas of the texture of a small draw in world space (stage
// 0 of the state), or NULL where the draw stays with its texture. A draw goes
// to the atlas with one clamped texture and coordinates inside it: the atlas
// then gives each pixel what the texture gave.
const AtlasEntry *AtlasPlaceOf(const GfxVertex *vertices, DWORD count)
{
    if (!AtlasStateFits())
        return NULL;
    s_noAtlas = NO_ATLAS_RANGE;
    if (!Gfx_AtlasCoordinatesFit(vertices, count))
        return NULL;
    return AtlasEntryOfState();
}

// The place for a draw of a small batch, whose vertices the recorder does
// not read: the place of its texture, for the case that its coordinates are
// inside the texture (Gfx_RunBatchJob looks at them).
const AtlasEntry *AtlasPlaceOfBatchDraw()
{
    return AtlasStateFits() ? AtlasEntryOfState() : NULL;
}

// The atlas for a small draw in world space, with the draw's texture
// coordinates moved into it: NULL where the draw stays with its texture.
GfxTexture *AtlasSmallDraw(GfxVertex *vertices, DWORD count)
{
    const AtlasEntry *e = AtlasPlaceOf(vertices, count);
    if (e == NULL)
        return NULL;
    Gfx_AtlasRemap(vertices, count, e->x, e->y, e->texture->width, e->texture->height, 1.0f / (float)s_atlasSize);
    return s_atlas;
}

// A texture goes, or its texels change: the atlas starts again (the textures
// of a stage go together). Draws recorded with the atlas run first.
void AtlasForget(const GfxTexture *t)
{
    bool known = t == s_atlas;
    for (const AtlasEntry &e : s_atlasEntries)
        known = known || e.texture == t;
    if (!known)
        return;
    if (s_atlasRecord == s_record)
        Flush(FLUSH_ATLAS);
    s_atlasEntries.clear();
    s_atlasSerial++;
    s_atlasPacker.Reset(s_atlasSize / 4, s_atlasSize / 4);
    if (t == s_atlas) {
        s_atlas = NULL;
    } else if (s_atlas != NULL) {
        // The textures that come now take the places of those that were
        // there: all of the atlas changes (an executor that writes a changed
        // part into the memory that the GPU reads takes new memory then).
        s_atlas->changedRect.x0 = s_atlas->changedRect.y0 = 0;
        s_atlas->changedRect.x1 = s_atlas->width;
        s_atlas->changedRect.y1 = s_atlas->height;
    }
}

// After a frame: the atlas that was full goes, and the next draw from an
// atlas starts the large one. No recorded draw has the atlas then.
void AtlasGrow()
{
    if (!s_atlasFull)
        return;
    s_atlasFull = false;
    s_atlasSize = kAtlasLargeSize;
    if (s_atlas != NULL)
        Gfx_DestroyTexture(s_atlas);
}

// The eye distance of the farthest vertex of a small draw in world space
// (its world matrix is the identity: the view matrix gives the eye space).
// The executor draws a run of small draws with the distance fade only where
// a vertex of the run reaches the start of the fade. A run joins quads on
// both sides of the start: the game sorts the trackside objects by their
// distance on the ground from the car, not by that from the eye, and at the
// start of the fade the quads of the two sides came in turns, each turn a
// draw (some 280 draws in a frame of a wood).
float SmallDrawReach(const GfxVertex *vertices, DWORD count)
{
    return Gfx_SmallDrawReach(s_state.view, vertices, count);
}

// The box of the positions of count vertices (count is at least 1).
inline void VertexBox(const GfxVertex *vertices, DWORD count, float low[3], float high[3])
{
    Gfx_VertexBox(vertices, count, low, high);
}

// ---- the test of a draw against the view ----
//
// The game culls its scenery by sectors: each object of a sector that
// touches a wide wedge in front of the camera is drawn. Half of the trees
// and a quarter of the ground parts and objects of a frame are then outside
// the view (PC runs of the stages). A draw whose vertices are all outside
// one plane of the view draws no pixel: the recorder leaves it out, and the
// executor does not set it up. The state that the game set for it stays.

// video.cull_draws and video.buffer_snapshots: -2 not read yet, -1 not set
// (then on with an executor that takes joined draws). Read again at each
// present.
int s_cullSetting = -2, s_snapshotSetting = -2, s_mergeGroundSetting = -2;
// video.range_locks: 0 makes a lock of a part of a vertex buffer a lock of
// the whole buffer (Gfx_LockVertexBufferRange), as it is for an executor
// that takes no joined draws.
int s_rangeLockSetting = 1;
// video.alpha_blend and video.alpha_test: 0 draws each 3D draw without
// blending, or without the alpha test, for a test of what they cost on the
// GPU. The transparent objects (the trees, the fences, the shadows, the
// particles) then are opaque. The 2D draws (the HUD, the menus) keep both.
// The screen options of the graphics menu set them.
int s_alphaBlendSetting = 1, s_alphaTestSetting = 1;
// video.perf_stats: 0 stops the performance stats of a development build,
// for a measurement without their cost: the counts of the recorder, the time
// of the recorder and of the parts of a frame, the executor's lines in the
// log and its time of the backdrop copy (which waits for the GPU). The
// heartbeat keeps its frame time line. The screen options of the graphics
// menu set it.
int s_perfStatsSetting = 1;

// What the recorded state of the draw that is recorded now leaves out:
// OVERRIDE_BLEND, OVERRIDE_TEST. A 3D draw and a 2D draw with one state of
// the game get two recorded states.
enum { OVERRIDE_BLEND = 1, OVERRIDE_TEST = 2 };
int s_drawOverride, s_recordedOverride;

void ReadDrawSettings()
{
    s_cullSetting = Platform_GetSettingInt("video.cull_draws", -1);
    s_snapshotSetting = Platform_GetSettingInt("video.buffer_snapshots", -1);
    s_mergeGroundSetting = Platform_GetSettingInt("video.merge_ground", -1);
    s_rangeLockSetting = Platform_GetSettingInt("video.range_locks", 1);
    s_alphaBlendSetting = Platform_GetSettingInt("video.alpha_blend", 1);
    s_alphaTestSetting = Platform_GetSettingInt("video.alpha_test", 1);
    s_perfStatsSetting = Platform_GetSettingInt("video.perf_stats", 1);
    // A log asks for the counts again after the stats are on again.
    if (s_perfStatsSetting == 0)
        s_countsWanted = false;
}

// The override of a draw (s_drawOverride).
int DrawOverride(bool transformed)
{
    if (s_cullSetting == -2)
        ReadDrawSettings();
    if (transformed)
        return 0;
    return (s_alphaBlendSetting == 0 ? OVERRIDE_BLEND : 0) | (s_alphaTestSetting == 0 ? OVERRIDE_TEST : 0);
}

bool CullWanted()
{
    if (s_cullSetting == -2)
        ReadDrawSettings();
    return s_cullSetting < 0 ? s_executor->JoinsDraws() : s_cullSetting != 0;
}

bool SnapshotsWanted()
{
    if (s_snapshotSetting == -2)
        ReadDrawSettings();
    return s_executor->JoinsDraws() && s_snapshotSetting != 0;
}

// Whether a lock of a part of a vertex buffer is such a lock
// (gfx_range_locks.h): the executor writes the patches of a frame.
bool RangeLocksWanted()
{
    if (s_cullSetting == -2)
        ReadDrawSettings();
    return s_executor != NULL && s_executor->JoinsDraws() && s_rangeLockSetting != 0;
}

// The matrix from a draw's own space to the clip space (world, view and
// projection), kept for the matrices it was made of. Slot 0: the world
// matrix of the state; slot 1: the identity (draws in world space).
struct ClipTransform {
    GfxMatrix world, view, projection;
    float m[16];
    bool valid = false;
};
ClipTransform s_clip[2];

const float *ClipMatrix(const GfxMatrix &world, int slot)
{
    ClipTransform &t = s_clip[slot];
    if (t.valid && memcmp(&t.world, &world, sizeof(GfxMatrix)) == 0 &&
        memcmp(&t.view, &s_state.view, sizeof(GfxMatrix)) == 0 &&
        memcmp(&t.projection, &s_state.projection, sizeof(GfxMatrix)) == 0)
        return t.m;
    t.world = world;
    t.view = s_state.view;
    t.projection = s_state.projection;
    Gfx_ClipMatrixOf(world, s_state.view, s_state.projection, t.m);
    t.valid = true;
    return t.m;
}

// The view test of points and of a box (gfx_small_draw.h).
inline bool PointsOutsideView(const float *m, const GfxVertex *vertices, DWORD count)
{
    return Gfx_PointsOutsideView(m, vertices, count);
}

bool BoxOutsideView(const float *m, const GfxBox &b)
{
    unsigned all = 63;
    for (int i = 0; i < 8 && all != 0; i++)
        all &= Gfx_OutsidePlanes(m, (i & 1) ? b.high[0] : b.low[0], (i & 2) ? b.high[1] : b.low[1],
                                 (i & 4) ? b.high[2] : b.low[2]);
    return all != 0;
}

// A hash of the positions of a buffer: the boxes of its ranges stay while
// it is the same.
uint64_t PositionHash(const GfxVertexBuffer *b)
{
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (const GfxVertex &v : b->vertices) {
        uint32_t w[3];
        memcpy(w, &v.x, sizeof(w));
        h = (h ^ w[0]) * 0x100000001b3ull;
        h = (h ^ w[1]) * 0x100000001b3ull;
        h = (h ^ w[2]) * 0x100000001b3ull;
    }
    return h;
}

// The box of the positions of a range of a buffer. A streaming buffer
// changes between its draws: its range is measured each time.
GfxBox BufferRangeBox(GfxVertexBuffer *b, DWORD first, DWORD count)
{
    GfxBox box;
    if (b->streaming) {
        VertexBox(&b->vertices[first], count, box.low, box.high);
        return box;
    }
    if (b->positionsUnknown) {
        const uint64_t hash = PositionHash(b);
        if (b->boxes != NULL && hash != b->positionHash)
            b->boxes->Clear();
        b->positionHash = hash;
        b->positionsUnknown = false;
    }
    if (b->boxes == NULL)
        b->boxes.reset(new GxmKeyCache<GfxRangeBox, 6>());
    const uint64_t key = (uint64_t)first << 32 | count;
    GfxRangeBox *known = b->boxes->Find(key);
    // A range lock that touched the range after the box was measured: the
    // game can have written other positions.
    if (known != NULL && b->lockStamps.Latest(first, count) <= known->serial)
        return known->box;
    VertexBox(&b->vertices[first], count, box.low, box.high);
    if (known == NULL)
        known = &b->boxes->Put(key);
    known->box = box;
    known->serial = b->lockSerial;
    return box;
}

// Adds a draw of vertices in the frame's data to the draw before, where the
// executor takes joined draws (GfxExecutor::JoinsDraws) and both are lists of
// one primitive with one state, one after the other in the frame's data. The
// game draws each sprite and each line with a call of its own: a run of them
// becomes one draw. The triangles keep their order.
bool JoinDraw(int primitive, bool transformed, const void *vertices, DWORD vertexCount, const WORD *indices,
              DWORD indexCount, bool worldSpace, GfxTexture *const textures[2], float reach)
{
    if (!StateIsRecorded(worldSpace) || s_frame.commands.empty())
        return NoJoin(NO_JOIN_STATE);
    if (primitive != GFX_TRIANGLELIST && primitive != GFX_LINELIST)
        return NoJoin(NO_JOIN_PRIMITIVE);
    GfxCommand &last = s_frame.commands.back();
    const size_t stride = transformed ? sizeof(GfxTLVertex) : sizeof(GfxVertex);
    const DWORD corners = primitive == GFX_LINELIST ? 2 : 3;
    if (last.type != GFX_CMD_DRAW || last.primitive != primitive || last.transformed != transformed ||
        last.buffer != NULL || last.state != s_frame.states.size() - 1)
        return NoJoin(NO_JOIN_COMMAND);
    if (last.textures[0] != textures[0] || last.textures[1] != textures[1])
        return NoJoin(NO_JOIN_TEXTURE);
    if ((last.firstIndex != ~0u) != (indices != NULL))
        return NoJoin(NO_JOIN_COMMAND);
    if (last.vertexCount + vertexCount > kJoinedVertices)
        return NoJoin(NO_JOIN_SIZE);
    // The draw before ends the frame's data, in whole primitives.
    if ((size_t)last.firstVertexByte + (size_t)last.vertexCount * stride != s_frame.vertices.size())
        return NoJoin(NO_JOIN_DATA);
    if (indices != NULL) {
        if ((size_t)last.firstIndex + last.indexCount != s_frame.indices.size() || last.indexCount % corners != 0)
            return NoJoin(NO_JOIN_DATA);
    } else if (last.vertexCount % corners != 0) {
        return NoJoin(NO_JOIN_DATA);
    }
    s_frame.vertices.insert(s_frame.vertices.end(), (const uint8_t *)vertices,
                            (const uint8_t *)vertices + vertexCount * stride);
#if defined(VITA) && defined(OPENCMR2_DEV)
    g_sysVitaDrawCounts.vertexBytes += vertexCount * stride;
#endif
    if (indices != NULL) {
        for (DWORD i = 0; i < indexCount; i++)
            s_frame.indices.push_back((uint16_t)(indices[i] + last.vertexCount));
        last.indexCount += indexCount;
    }
    last.vertexCount += vertexCount;
    // The box and the reach of the run. A draw that is not a small draw in
    // world space has no reach: the run then has its box only.
    if (!transformed && last.hasBox) {
        float low[3], high[3];
        VertexBox((const GfxVertex *)vertices, vertexCount, low, high);
        for (int k = 0; k < 3; k++) {
            last.boxLow[k] = low[k] < last.boxLow[k] ? low[k] : last.boxLow[k];
            last.boxHigh[k] = high[k] > last.boxHigh[k] ? high[k] : last.boxHigh[k];
        }
        last.reach = worldSpace && last.reach >= 0.0f ? (reach > last.reach ? reach : last.reach) : -1.0f;
    }
    return true;
}

// The world matrix of the state recorded last. A change of the world matrix
// alone does not make the state dirty: RecordState compares the matrix. A
// small draw is recorded in world space, with the identity as its world
// matrix (SmallDrawToWorld): a run of them with different world matrices has
// one state.
GfxMatrix s_recordedWorld;

const GfxMatrix kIdentityMatrix = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };

// worldSpace: the draw's vertices are in world space.
const GfxMatrix &RecordedWorldOf(bool worldSpace)
{
    return worldSpace ? kIdentityMatrix : s_state.world;
}

bool StateIsRecorded(bool worldSpace)
{
    return !s_stateDirty && !s_frame.states.empty() && s_recordedOverride == s_drawOverride &&
           memcmp(&RecordedWorldOf(worldSpace), &s_recordedWorld, sizeof(GfxMatrix)) == 0;
}

uint32_t RecordState(bool worldSpace = false)
{
    if (!StateIsRecorded(worldSpace)) {
        const GfxMatrix &world = RecordedWorldOf(worldSpace);
        if (s_vertexDirty || memcmp(&world, &s_recordedWorld, sizeof(world)) != 0)
            s_vertexVersion++;
        s_counts.states++;
        s_frame.states.push_back(s_state);
        s_frame.states.back().world = world;
        s_frame.states.back().vertexVersion = s_vertexVersion;
        // The test of the transparent objects (video.alpha_blend and
        // video.alpha_test).
        if (s_drawOverride & OVERRIDE_BLEND)
            s_frame.states.back().alphaBlend = false;
        if (s_drawOverride & OVERRIDE_TEST)
            s_frame.states.back().alphaTest = false;
        s_recordedOverride = s_drawOverride;
        s_recordedWorld = world;
        s_stateDirty = false;
        s_vertexDirty = false;
    }
    return (uint32_t)s_frame.states.size() - 1;
}

// Counts the parts of the state that differ from the state recorded last,
// for a small draw in world space that did not join for its state.
void CountChangedParts()
{
    uint32_t *n = s_counts.changed;
    if (s_frame.states.empty()) {
        n[CHANGED_FIRST]++;
        return;
    }
    const GfxDrawState &a = s_state, &b = s_frame.states.back();
    bool any = false;
#define PART(index, same) do { if (!(same)) { n[index]++; any = true; } } while (0)
#define SAME(field) (memcmp(&a.field, &b.field, sizeof(a.field)) == 0)
    PART(CHANGED_WORLD, memcmp(&kIdentityMatrix, &s_recordedWorld, sizeof(GfxMatrix)) == 0);
    PART(CHANGED_VIEW, SAME(view) && SAME(projection));
    PART(CHANGED_TEXTURE_MATRIX, SAME(texture));
    PART(CHANGED_LIGHTS, SAME(lights) && SAME(lightEnabled));
    PART(CHANGED_MATERIAL, SAME(material));
    PART(CHANGED_LIGHTING, a.lighting == b.lighting && a.ambient == b.ambient && a.specular == b.specular &&
                               a.localViewer == b.localViewer && a.normalizeNormals == b.normalizeNormals &&
                               a.colorVertex == b.colorVertex && a.diffuseSource == b.diffuseSource &&
                               a.specularSource == b.specularSource && a.ambientSource == b.ambientSource &&
                               a.emissiveSource == b.emissiveSource);
    PART(CHANGED_FOG, a.fog == b.fog && a.fogColor == b.fogColor && a.fogVertexMode == b.fogVertexMode &&
                          a.fogTableMode == b.fogTableMode && SAME(fogStart) && SAME(fogEnd));
    PART(CHANGED_FADE, SAME(fadeStart) && SAME(fadeEnd));
    PART(CHANGED_DEPTH, a.depthTest == b.depthTest && a.depthWrite == b.depthWrite && a.depthFunc == b.depthFunc);
    PART(CHANGED_CULL, a.cull == b.cull);
    PART(CHANGED_BLEND, a.alphaBlend == b.alphaBlend && a.srcBlend == b.srcBlend && a.destBlend == b.destBlend);
    PART(CHANGED_ALPHA_TEST, a.alphaTest == b.alphaTest && a.alphaFunc == b.alphaFunc && a.alphaRef == b.alphaRef);
    PART(CHANGED_FACTOR, a.textureFactor == b.textureFactor);
    for (int i = 0; i < 2; i++) {
        GfxStageState mine = a.stages[i];
        mine.texture = b.stages[i].texture;
        PART(i == 0 ? CHANGED_STAGE0 : CHANGED_STAGE1, memcmp(&mine, &b.stages[i], sizeof(mine)) == 0);
    }
    PART(CHANGED_VIEWPORT, SAME(viewport) && a.screenW == b.screenW && a.screenH == b.screenH);
#undef SAME
#undef PART
    if (!any)
        n[CHANGED_NONE]++;
}

// Vertices at a multiple of their size, so that the executor can draw them
// from their first vertex.
uint32_t AppendVertices(const void *data, size_t bytes, size_t stride)
{
    size_t offset = (s_frame.vertices.size() + stride - 1) / stride * stride;
    s_frame.vertices.resize(offset);
    s_frame.vertices.insert(s_frame.vertices.end(), (const uint8_t *)data, (const uint8_t *)data + bytes);
    return (uint32_t)offset;
}

// OPENCMR2_GFX_TRACE=file: one line per draw (primitive, counts, a hash of the
// vertices, indices and bound texture sizes, the calling game function, and
// k= the fragment key, p= the pipeline key), "F n" per presented frame with
// the frame's sizes, and "T" with the live textures and vertex buffers when
// they change. Builds for different targets must write the same lines;
// diffing them finds drawing that differs (tools: addr2line on caller).
// tools/trace_stats.py sums a trace up.
static FILE *TraceFile()
{
    static FILE *file = getenv("OPENCMR2_GFX_TRACE") ? fopen(getenv("OPENCMR2_GFX_TRACE"), "w") : NULL;
    return file;
}
static const void *s_traceCaller;
static unsigned s_traceFrame;

// Sizes of the frame being traced, summed over its flushes.
struct TraceFrameSizes {
    size_t vertexBytes, indices, commands, states, buffers, bufferBytes;
};
static TraceFrameSizes s_traceSizes;

static void TraceAddFrameSizes(const GfxFrame &frame)
{
    s_traceSizes.vertexBytes += frame.vertices.size();
    s_traceSizes.indices += frame.indices.size();
    s_traceSizes.commands += frame.commands.size();
    s_traceSizes.states += frame.states.size();
    s_traceSizes.buffers += frame.buffers.size();
    for (const GfxVertexBuffer *buffer : frame.buffers)
        s_traceSizes.bufferBytes += buffer->vertices.size() * sizeof(GfxVertex);
}

// Live textures per format (GFX_FORMAT_*), count and bytes, and vertex buffers.
static size_t s_traceTextureCount[5], s_traceTextureBytes[5];
static size_t s_traceVertexBuffers, s_traceVertexBufferBytes;
static bool s_traceLiveChanged;

static size_t TextureBytes(const GfxTexture *t)
{
    size_t bytes = 0;
    for (const auto &face : t->levels)
        for (const auto &level : face)
            bytes += level.size();
    return bytes;
}

static void TraceTexture(const GfxTexture *t, int sign)
{
    if (TraceFile() == NULL || t->format < 0 || t->format >= 5)
        return;
    s_traceTextureCount[t->format] += sign;
    s_traceTextureBytes[t->format] += sign * TextureBytes(t);
    s_traceLiveChanged = true;
}

static void TraceVertexBuffer(const GfxVertexBuffer *buffer, int sign)
{
    if (TraceFile() == NULL)
        return;
    s_traceVertexBuffers += sign;
    s_traceVertexBufferBytes += sign * buffer->vertices.size() * sizeof(GfxVertex);
    s_traceLiveChanged = true;
}

static uint32_t TraceHash(uint32_t h, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

void RecordNewDraw(int primitive, bool transformed, const void *vertices, DWORD vertexCount, const WORD *indices,
                   DWORD indexCount, GfxVertexBuffer *buffer, DWORD firstVertex, bool worldSpace,
                   GfxTexture *const textures[2], float reach, const GfxBox *knownBox);

void RecordDrawNow(int primitive, bool transformed, const void *vertices, DWORD vertexCount, const WORD *indices,
                   DWORD indexCount, GfxVertexBuffer *buffer, DWORD firstVertex);

// Records a draw; fans become lists (SDL_GPU has no fans). With a buffer,
// the vertices are firstVertex.. of that buffer.
void RecordDraw(int primitive, bool transformed, const void *vertices, DWORD vertexCount, const WORD *indices,
                DWORD indexCount, GfxVertexBuffer *buffer = NULL, DWORD firstVertex = 0)
{
#if defined(VITA) && defined(OPENCMR2_DEV)
    g_sysVitaDrawCounts.calls++;
    // The heartbeat's recorder time, from one draw in kRecorderSampleEvery.
    static unsigned count;
    if (s_perfStatsSetting != 0 && ++count % kRecorderSampleEvery == 0) {
        const unsigned long long start = SysVita_Micros();
        RecordDrawNow(primitive, transformed, vertices, vertexCount, indices, indexCount, buffer, firstVertex);
        SysVita_RecorderSample(SysVita_Micros() - start);
        return;
    }
#endif
    RecordDrawNow(primitive, transformed, vertices, vertexCount, indices, indexCount, buffer, firstVertex);
}

void RecordDrawNow(int primitive, bool transformed, const void *vertices, DWORD vertexCount, const WORD *indices,
                   DWORD indexCount, GfxVertexBuffer *buffer, DWORD firstVertex)
{
    if (FILE *trace = TraceFile()) {
        uint32_t h = TraceHash(2166136261u, vertices, vertexCount * (transformed ? sizeof(GfxTLVertex) : sizeof(GfxVertex)));
        if (indices != NULL)
            h = TraceHash(h, indices, indexCount * sizeof(WORD));
        for (int k = 0; k < 2; k++) {
            const GfxTexture *t = s_state.stages[k].texture;
            int size[2] = {t ? t->width : 0, t ? t->height : 0};
            h = TraceHash(h, size, sizeof(size));
        }
        const int drawn = primitive == GFX_TRIANGLEFAN ? GFX_TRIANGLELIST : primitive;
        fprintf(trace, "%d %d %u %u %08x %p k=%09llx p=%06x a=%011llx\n", primitive, transformed ? 1 : 0, (unsigned)vertexCount,
                (unsigned)indexCount, h, s_traceCaller,
                (unsigned long long)Gfx_FragmentKey(s_state, !transformed && s_target == NULL),
                (unsigned)Gfx_PipelineKey(drawn, transformed, s_state),
                (unsigned long long)Gfx_FragmentArgsKey(s_state));
        static int dumpFrame = getenv("OPENCMR2_GFX_TRACE_FRAME") ? atoi(getenv("OPENCMR2_GFX_TRACE_FRAME")) : -1;
        if ((int)s_traceFrame == dumpFrame && transformed)
            for (DWORD v = 0; v < vertexCount; v++) {
                const GfxTLVertex *t = (const GfxTLVertex *)vertices + v;
                fprintf(trace, "  v %g %g %g %g c=%08x s=%08x uv=%g,%g tex=%dx%d\n", t->sx, t->sy, t->sz, t->rhw, t->color, t->specular, t->tu, t->tv,
                        s_state.stages[0].texture ? s_state.stages[0].texture->width : 0, s_state.stages[0].texture ? s_state.stages[0].texture->height : 0);
            }
    }
    if (vertexCount == 0 || s_executor == NULL)
        return;
    std::vector<uint16_t> fan;
    if (primitive == GFX_TRIANGLEFAN) {
        DWORD n = indices ? indexCount : vertexCount;
        for (DWORD i = 1; i + 1 < n; i++) {
            fan.push_back(indices ? indices[0] : 0);
            fan.push_back(indices ? indices[i] : (uint16_t)i);
            fan.push_back(indices ? indices[i + 1] : (uint16_t)(i + 1));
        }
        primitive = GFX_TRIANGLELIST;
        indices = fan.data();
        indexCount = (DWORD)fan.size();
        if (indexCount == 0)
            return;
    }
    s_drawOverride = DrawOverride(transformed);
    const bool joins = s_executor->JoinsDraws();
    // A small 3D draw in world space, from the frame's data, with the atlas
    // as its texture where its texture is in the atlas.
    GfxVertex small[kSmallDrawVertices];
    GfxTexture *textures[2] = { s_state.stages[0].texture, s_state.stages[1].texture };
    bool worldSpace = false;
    float reach = -1.0f;
    // The test against the view: a draw outside it is left out.
    const bool cull = !transformed && CullWanted();
    GfxBox box;
    const GfxBox *knownBox = NULL;
    if (joins && !transformed && vertexCount <= kSmallDrawVertices &&
        SmallDrawToWorld((const GfxVertex *)vertices, vertexCount, small)) {
        vertices = small;
        buffer = NULL;
        firstVertex = 0;
        worldSpace = true;
        if (cull && PointsOutsideView(ClipMatrix(kIdentityMatrix, 1), small, vertexCount)) {
            s_counts.culled++;
            return;
        }
        reach = SmallDrawReach(small, vertexCount);
        GfxTexture *atlas = AtlasSmallDraw(small, vertexCount);
        if (atlas != NULL)
            textures[0] = atlas;
        if (s_countsWanted) {
            s_counts.small++;
            if (atlas != NULL)
                s_counts.atlas++;
            else
                s_counts.noAtlas[s_noAtlas]++;
        }
    } else if (cull) {
        if (buffer != NULL)
            box = BufferRangeBox(buffer, firstVertex, vertexCount);
        else
            VertexBox((const GfxVertex *)vertices, vertexCount, box.low, box.high);
        if (BoxOutsideView(ClipMatrix(s_state.world, 0), box)) {
            s_counts.culled++;
            return;
        }
        knownBox = &box;
    }
    // A short strip (a sprite) as a list: lists join. The odd triangles of a
    // strip have their first two corners in the other order.
    WORD strip[(kJoinedStrip - 2) * 3];
    if (buffer == NULL && joins) {
        if (primitive == GFX_TRIANGLESTRIP && indices == NULL && vertexCount >= 3 && vertexCount <= kJoinedStrip) {
            indexCount = 0;
            for (DWORD i = 0; i + 2 < vertexCount; i++) {
                strip[indexCount++] = (WORD)(i + (i & 1));
                strip[indexCount++] = (WORD)(i + 1 - (i & 1));
                strip[indexCount++] = (WORD)(i + 2);
            }
            primitive = GFX_TRIANGLELIST;
            indices = strip;
        }
        if (JoinDraw(primitive, transformed, vertices, vertexCount, indices, indexCount, worldSpace, textures,
                     reach)) {
            if (worldSpace)
                s_counts.joined++;
            return;
        }
        if (worldSpace && s_countsWanted) {
            s_counts.noJoin[s_noJoin]++;
            if (s_noJoin == NO_JOIN_STATE)
                CountChangedParts();
        }
    }
    RecordNewDraw(primitive, transformed, vertices, vertexCount, indices, indexCount, buffer, firstVertex, worldSpace,
                  textures, reach, knownBox);
}

// Records a draw as a command of its own. worldSpace: its vertices are in
// world space (SmallDrawToWorld). textures: those of its two stages.
// knownBox: the box of its vertices, where the view test found it.
void RecordNewDraw(int primitive, bool transformed, const void *vertices, DWORD vertexCount, const WORD *indices,
                   DWORD indexCount, GfxVertexBuffer *buffer, DWORD firstVertex, bool worldSpace,
                   GfxTexture *const textures[2], float reach, const GfxBox *knownBox)
{
    GfxCommand cmd = {};
    cmd.part = ScenePartNow();
    cmd.reach = -1.0f;
    // The box of the vertices of a 3D draw that the executor gets in the
    // frame's data (GfxCommand::hasBox).
    const bool box = !transformed && s_executor->JoinsDraws();
    cmd.type = GFX_CMD_DRAW;
    cmd.primitive = primitive;
    cmd.transformed = transformed;
    cmd.vertexCount = vertexCount;
    const bool rested = buffer != NULL && s_presentedFrames - buffer->lastLockFrame >= kStaticAfterFrames;
    if (rested && !buffer->rangeLocks.Recent(firstVertex, vertexCount, s_presentedFrames)) {
        cmd.buffer = buffer;
        cmd.firstVertexByte = firstVertex * (uint32_t)sizeof(GfxVertex);
        if (buffer->referencedRecord != s_record) {
            buffer->referencedRecord = s_record;
            s_frame.buffers.push_back(buffer);
            // The parts that rested go into the GPU copy before the draws
            // of this recording (gfx_range_locks.h).
            buffer->rangeLocks.TakeRested(s_presentedFrames, (uint32_t)buffer->vertices.size(),
                                          [buffer](uint32_t first, uint32_t end) {
                                              s_frame.patches.push_back({ buffer, first, end });
                                              buffer->patchRecord = s_record;
                                          });
        }
        if (box && knownBox != NULL) {
            cmd.hasBox = true;
            memcpy(cmd.boxLow, knownBox->low, sizeof(cmd.boxLow));
            memcpy(cmd.boxHigh, knownBox->high, sizeof(cmd.boxHigh));
        }
    } else if (buffer != NULL && !buffer->streaming && SnapshotsWanted()) {
        // A buffer locked recently, or a part of it (a block of
        // gfx_range_locks.h), before its draws: the executor copies
        // the part that the frame draws once. The recorder copied each draw
        // range, and the executor copied the frame again: with the stage
        // lighting 3.6 MB twice in a frame.
        cmd.buffer = buffer;
        cmd.snapshot = true;
        cmd.firstVertexByte = firstVertex * (uint32_t)sizeof(GfxVertex);
        if (buffer->snapshotRecord != s_record) {
            buffer->snapshotRecord = s_record;
            buffer->snapshotIndex = (uint32_t)s_frame.snapshots.size();
            s_frame.snapshots.push_back({ buffer, firstVertex, firstVertex + vertexCount });
        } else {
            GfxSnapshot &snapshot = s_frame.snapshots[buffer->snapshotIndex];
            snapshot.first = firstVertex < snapshot.first ? firstVertex : snapshot.first;
            snapshot.end = firstVertex + vertexCount > snapshot.end ? firstVertex + vertexCount : snapshot.end;
        }
        if (box) {
            const GfxBox b = knownBox != NULL ? *knownBox : BufferRangeBox(buffer, firstVertex, vertexCount);
            cmd.hasBox = true;
            memcpy(cmd.boxLow, b.low, sizeof(cmd.boxLow));
            memcpy(cmd.boxHigh, b.high, sizeof(cmd.boxHigh));
        }
        s_counts.snapshots++;
    } else if (buffer != NULL && s_copied.buffer == buffer && s_copied.version == buffer->version &&
               s_copied.record == s_record && firstVertex >= s_copied.first &&
               firstVertex + vertexCount <= s_copied.first + s_copied.count) {
        // The game draws a mesh in parts, each with the vertex range of the
        // mesh: one copy serves them, and its box.
        cmd.firstVertexByte = s_copied.offset + (firstVertex - s_copied.first) * (uint32_t)sizeof(GfxVertex);
        if (box) {
            cmd.hasBox = true;
            memcpy(cmd.boxLow, s_copied.low, sizeof(cmd.boxLow));
            memcpy(cmd.boxHigh, s_copied.high, sizeof(cmd.boxHigh));
        }
    } else {
        size_t stride = transformed ? sizeof(GfxTLVertex) : sizeof(GfxVertex);
        cmd.firstVertexByte = AppendVertices(vertices, vertexCount * stride, stride);
        if (box) {
            cmd.hasBox = true;
            if (knownBox != NULL) {
                memcpy(cmd.boxLow, knownBox->low, sizeof(cmd.boxLow));
                memcpy(cmd.boxHigh, knownBox->high, sizeof(cmd.boxHigh));
            } else {
                VertexBox((const GfxVertex *)vertices, vertexCount, cmd.boxLow, cmd.boxHigh);
            }
            cmd.reach = worldSpace ? reach : -1.0f;
        }
        if (buffer != NULL) {
            s_copied.buffer = buffer;
            s_copied.version = buffer->version;
            s_copied.record = s_record;
            s_copied.first = firstVertex;
            s_copied.count = vertexCount;
            s_copied.offset = cmd.firstVertexByte;
            memcpy(s_copied.low, cmd.boxLow, sizeof(s_copied.low));
            memcpy(s_copied.high, cmd.boxHigh, sizeof(s_copied.high));
        }
    }
    if (indices != NULL) {
        cmd.firstIndex = (uint32_t)s_frame.indices.size();
        cmd.indexCount = indexCount;
        s_frame.indices.insert(s_frame.indices.end(), indices, indices + indexCount);
    } else {
        cmd.firstIndex = ~0u;
        cmd.indexCount = 0;
    }
    cmd.state = RecordState(worldSpace);
    cmd.textures[0] = textures[0];
    cmd.textures[1] = textures[1];
    s_frame.commands.push_back(cmd);
#if defined(VITA) && defined(OPENCMR2_DEV)
    g_sysVitaDrawCounts.commands++;
    if (cmd.buffer == NULL)
        g_sysVitaDrawCounts.vertexBytes += vertexCount * (transformed ? sizeof(GfxTLVertex) : sizeof(GfxVertex));
#endif
}

GfxStageState *Stage(int stage)
{
    return stage >= 0 && stage < 2 ? &s_state.stages[stage] : NULL;
}

GfxTexture *Root(GfxTexture *t)
{
    return t != NULL && t->cube != NULL ? t->cube : t;
}

// The desktop size, landscape: the game always runs that way (on Android the
// display can still report its natural portrait size).
void LandscapeDesktopSize(int *width, int *height)
{
    Sys_GetDesktopSize(width, height);
    if (*height > *width)
        std::swap(*width, *height);
}

// Resolutions at the display's aspect ratio, from 540 lines up to the display
// itself, and the original 4:3 ones up to 1024x768. At most 10 (the game's
// list), smallest first.
void BuildModes()
{
    static const int classic[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 } };
    static const int heights[] = { 540, 720, 900, 1080, 1440, 2160 };
    int desktopW, desktopH;

    s_modes.clear();
    LandscapeDesktopSize(&desktopW, &desktopH);
    auto add = [&](int w, int h) {
        if (w <= desktopW && h <= desktopH &&
            std::find(s_modes.begin(), s_modes.end(), std::make_pair(w, h)) == s_modes.end())
            s_modes.push_back({ w, h });
    };
    for (const auto &c : classic)
        add(c[0], c[1]);
#ifdef VITA
    // The sizes that the display of the PS Vita scales to the whole screen
    // (gxm_device.h): fewer pixels for the GPU, the picture of the screen's
    // shape.
    add(640, 368);
    add(720, 408);
#endif
    for (int h : heights)
        add((int)((long long)h * desktopW / desktopH) & ~1, h);
    add(desktopW, desktopH);
    std::sort(s_modes.begin(), s_modes.end());
    while (s_modes.size() > 10)
        s_modes.erase(s_modes.begin() + 1);
}

} // namespace

// ---- formats -------------------------------------------------------------------------

int Gfx_BytesPerPixel(int format)
{
    switch (format) {
    case GFX_FORMAT_BGRX8:
    case GFX_FORMAT_BGRA8:
        return 4;
    case GFX_FORMAT_BUMP_DUDVL:
        return 3;
    default:
        return 0;
    }
}

size_t Gfx_LevelSize(int format, int width, int height, LONG *pitch)
{
    if (format == GFX_FORMAT_BC1 || format == GFX_FORMAT_BC3) {
        int blockBytes = format == GFX_FORMAT_BC1 ? 8 : 16;
        int bw = (width + 3) / 4, bh = (height + 3) / 4;
        if (pitch)
            *pitch = bw * blockBytes;
        return (size_t)bw * bh * blockBytes;
    }
    int bpp = Gfx_BytesPerPixel(format);
    if (pitch)
        *pitch = width * bpp;
    return (size_t)width * height * bpp;
}

extern "C" void Gfx_GetPixelFormat(int format, GfxPixelFormat *out)
{
    memset(out, 0, sizeof(*out));
    switch (format) {
    case GFX_FORMAT_BGRX8:
        out->flags = GFX_PF_RGB;
        out->bitsPerPixel = 32;
        out->redMask = 0x00ff0000;
        out->greenMask = 0x0000ff00;
        out->blueMask = 0x000000ff;
        break;
    case GFX_FORMAT_BGRA8:
        out->flags = GFX_PF_RGB | GFX_PF_ALPHA;
        out->bitsPerPixel = 32;
        out->redMask = 0x00ff0000;
        out->greenMask = 0x0000ff00;
        out->blueMask = 0x000000ff;
        out->alphaMask = 0xff000000;
        break;
    case GFX_FORMAT_BC1:
        out->flags = GFX_PF_FOURCC;
        out->fourCC = MAKEFOURCC('D', 'X', 'T', '1');
        break;
    case GFX_FORMAT_BC3:
        out->flags = GFX_PF_FOURCC;
        out->fourCC = MAKEFOURCC('D', 'X', 'T', '5');
        break;
    case GFX_FORMAT_BUMP_DUDVL:
        out->flags = GFX_PF_BUMP;
        out->bitsPerPixel = 24;
        out->redMask = 0x0000ff;
        out->greenMask = 0x00ff00;
        out->blueMask = 0xff0000;
        break;
    }
}

// ---- renderer choice -------------------------------------------------------------------

namespace {

enum Backend { BACKEND_NONE, BACKEND_NULL, BACKEND_GPU, BACKEND_GL, BACKEND_GXM };

// SDL_GPU (Vulkan, Direct3D 12, Metal) where the system has a hardware
// device for it, otherwise OpenGL 3.3, for GPUs older than Vulkan; [video] graphics_api
// (auto, sdl_gpu, opengl) chooses one. Decided once, before the window is
// made: OpenGL needs its window to be made for it.
Backend ChooseBackend()
{
    static Backend backend = BACKEND_NONE;
    if (backend != BACKEND_NONE)
        return backend;
#ifdef VITA
    const char *headless = getenv("OPENCMR2_HEADLESS");
#else
    const char *headless = SDL_getenv("OPENCMR2_HEADLESS");
#endif
    if ((headless != NULL && *headless != '0') || Platform_GetSettingInt("video.headless", 0))
        return backend = BACKEND_NULL;
#ifdef VITA
    return backend = BACKEND_GXM;
#else
    const char *wanted = Platform_GetSetting("video.graphics_api", "auto");
#ifdef OPENCMR2_RENDERER_GL
    if (SDL_strcasecmp(wanted, "opengl") == 0)
        backend = BACKEND_GL;
    else if (SDL_strcasecmp(wanted, "sdl_gpu") == 0)
        backend = BACKEND_GPU;
    else if (Gfx_GpuIsAvailable())
        backend = BACKEND_GPU;
    else {
        Sys_Log("gfx: no hardware Vulkan, Direct3D 12 or Metal device for SDL_GPU: OpenGL 3.3");
        backend = BACKEND_GL;
    }
#else
    if (SDL_strcasecmp(wanted, "opengl") == 0)
        Sys_Log("gfx: this build has no OpenGL renderer");
    backend = BACKEND_GPU;
#endif
    return backend;
#endif
}

} // namespace

extern "C" unsigned long long Gfx_PrepareWindow(void)
{
#ifdef OPENCMR2_RENDERER_GL
    if (ChooseBackend() == BACKEND_GL)
        return Gfx_PrepareGlWindow();
#endif
    return 0;
}

extern "C" const char *Gfx_GetRendererName(void)
{
    return ChooseBackend() == BACKEND_GL ? "OpenCMR2 OpenGL" : "OpenCMR2 SDL_GPU";
}

// ---- device and frame ----------------------------------------------------------------

extern "C" BOOL Gfx_SetVideoMode(int width, int height, BOOL fullscreen)
{
    if (s_executor == NULL) {
        switch (ChooseBackend()) {
        case BACKEND_NULL:
            s_executor = Gfx_CreateNullExecutor();
            break;
#ifdef OPENCMR2_RENDERER_GL
        case BACKEND_GL:
            s_executor = Gfx_CreateGlExecutor();
            break;
#endif
#ifdef VITA
        case BACKEND_GXM:
            s_executor = Gfx_CreateGxmExecutor();
            break;
#else
        default:
            s_executor = Gfx_CreateGpuExecutor();
            break;
#endif
        }
        // The frames on a worker thread where the executor permits it (the
        // GXM one: the OpenGL and SDL_GPU ones draw from the main thread).
        const bool gxm = ChooseBackend() == BACKEND_GXM;
        if (Platform_GetSettingInt("video.threaded_render", gxm ? 1 : 0) != 0) {
            GfxExecutor *inner = s_executor;
            s_executor = Gfx_CreateThreadedExecutor(inner);
            s_threaded = s_executor != inner;
            if (s_threaded)
                Sys_Log("gfx: the frames run on a worker thread (video.threaded_render)");
        }
        // The small batches on a thread of their own, with the frames on a
        // worker (gfx_batch_worker.h). [video] batch_thread = 0: on the
        // game's thread, when the game gives a batch.
        if (s_threaded && Platform_GetSettingInt("video.batch_thread", 1) != 0 && Gfx_StartBatchThread())
            Sys_Log("gfx: the small batches run on a thread of their own (video.batch_thread)");
    }
    s_width = width;
    s_height = height;
    if (!s_executor->SetVideoMode(width, height, fullscreen != 0)) {
        Sys_Log("gfx: could not set %dx%d", width, height);
        return FALSE;
    }
    ResetState();
    return TRUE;
}

extern "C" void Gfx_SetFullscreen(BOOL fullscreen)
{
    if (s_executor != NULL)
        s_executor->SetFullscreen(fullscreen != 0);
}

extern "C" void Gfx_Shutdown(void)
{
    while (!s_textures.empty())
        Gfx_DestroyTexture(s_textures.back());
    delete s_executor;
    s_executor = NULL;
    s_threaded = false;
    Gfx_StopBatchThread();
}

extern "C" int Gfx_GetDisplayModeCount(void)
{
    if (s_modes.empty())
        BuildModes();
    return (int)s_modes.size();
}

extern "C" void Gfx_GetDefaultDisplayMode(int *width, int *height)
{
    // The display's own resolution: the menus, the HUD and the text are
    // scaled to the screen's height, so they stay the same size at any mode,
    // and the 3D picture is as sharp as the display.
    LandscapeDesktopSize(width, height);
}

extern "C" void Gfx_GetDisplayMode(int index, int *width, int *height)
{
    if (s_modes.empty())
        BuildModes();
    *width = s_modes[index].first;
    *height = s_modes[index].second;
}

extern "C" void Gfx_BeginScene(void)
{
}

extern "C" void Gfx_EndScene(void)
{
}

// Runs the frame recorded so far on the executor. The draws of the small
// batches go in their place first: here, or on the worker of the threaded
// executor. That one takes the frame's contents; the caller resets what it
// leaves.
static void Submit(bool present, bool vsync, bool stretch)
{
    if (s_executor == NULL)
        return;
    AddPendingBatchJob();
    s_frame.batchSequence = Gfx_BatchJobsAdded();
    if (!s_threaded) {
        Gfx_WaitForBatchJobs(s_frame.batchSequence, 0);
        Gfx_SpliceSmallBatches(s_frame);
    }
    s_executor->Prepare(s_frame, s_textures, s_readback, present);
    s_executor->Execute(s_frame, s_textures, s_readback, present, vsync, stretch);
}

void Gfx_WaitForExecutor(void)
{
    if (s_executor != NULL)
        s_executor->Wait();
}

static void Present(BOOL vsync, bool stretch)
{
    if (FILE *trace = TraceFile()) {
        TraceAddFrameSizes(s_frame);
        const TraceFrameSizes &f = s_traceSizes;
        fprintf(trace, "F %u v=%zu i=%zu c=%zu s=%zu b=%zu/%zu\n", s_traceFrame++, f.vertexBytes, f.indices, f.commands,
                f.states, f.buffers, f.bufferBytes);
        if (s_executor != NULL && s_executor->JoinsDraws()) {
            char counts[512];
            Gfx_RecorderCountsText(counts, sizeof(counts));
            fprintf(trace, "R %s\n", counts);
        }
        s_traceSizes = TraceFrameSizes();
        if (s_traceLiveChanged) {
            fprintf(trace, "T");
            for (int format = 0; format < 5; format++)
                fprintf(trace, " %d:%zu/%zu", format, s_traceTextureCount[format], s_traceTextureBytes[format]);
            fprintf(trace, " vb:%zu/%zu\n", s_traceVertexBuffers, s_traceVertexBufferBytes);
            s_traceLiveChanged = false;
        }
    }
    if (Diagnostics::observer)
        Diagnostics::observer->BeginPresent();
    Submit(true, vsync != 0, stretch);
    s_frame.Reset();
    s_record++;
    s_presentedFrames++;
    AtlasGrow();
    ReadDrawSettings();
#if defined(VITA) && defined(OPENCMR2_DEV)
    SysVita_FramePresented(s_perfStatsSetting != 0);
#endif
    s_stateDirty = true;
    // A frame always starts on the back buffer.
    s_target = NULL;
    if (Diagnostics::observer)
        Diagnostics::observer->EndPresent();
}

extern "C" void Gfx_Present(BOOL vsync) { Present(vsync, false); }
extern "C" void Gfx_PresentStretched(BOOL vsync) { Present(vsync, true); }

// Runs what is recorded so far without ending the frame.
void Gfx_RecorderCountsText(char *text, size_t size)
{
    const RecorderCounts &n = s_counts;
    size_t used = 0;
    const auto add = [&](const char *format, unsigned value, const char *name) {
        if (used < size)
            used += (size_t)snprintf(text + used, size - used, format, value, name);
    };
    const auto list = [&](const char *title, const uint32_t *values, const char *const *names, int count) {
        bool first = true;
        for (int i = 0; i < count; i++) {
            if (values[i] == 0)
                continue;
            if (first && used < size)
                used += (size_t)snprintf(text + used, size - used, "; %s:", title);
            first = false;
            add(" %u %s", values[i], names[i]);
        }
    };
    add("states %u%s", n.states, "");
    add(", small draws %u%s", n.small, "");
    add(" (%u joined%s", n.joined, "");
    add(", %u from the atlas)%s", n.atlas, "");
    add(", culled %u%s", n.culled, "");
    add(", snapshot draws %u%s", n.snapshots, "");
    add(", batched %u%s", n.batched, "");
    list("no join", n.noJoin, kNoJoinNames, NO_JOIN_COUNT);
    list("state parts", n.changed, kChangedNames, CHANGED_COUNT);
    list("no atlas", n.noAtlas, kNoAtlasNames, NO_ATLAS_COUNT);
    list("flushes", n.flushes, kFlushNames, FLUSH_COUNT);
    s_counts = RecorderCounts();
    s_countsWanted = s_perfStatsSetting != 0;
}

bool Gfx_PerfStatsWanted(void)
{
    if (s_cullSetting == -2)
        ReadDrawSettings();
    return s_perfStatsSetting != 0;
}

static void Flush(int cause)
{
    s_counts.flushes[cause]++;
    if (TraceFile() != NULL)
        TraceAddFrameSizes(s_frame);
    Submit(false, false, false);
    s_frame.Reset();
    s_record++;
    s_stateDirty = true;
    if (s_target != NULL)
        Gfx_SetRenderTarget(s_target);
}

extern "C" void Gfx_Clear(DWORD flags, DWORD color, float z)
{
    GfxCommand cmd = {};
    cmd.type = GFX_CMD_CLEAR;
    cmd.clearFlags = flags;
    cmd.clearColor = color;
    cmd.clearZ = z;
    cmd.clearViewport = s_state.viewport;
    cmd.clearScreenW = s_state.screenW;
    cmd.clearScreenH = s_state.screenH;
    s_frame.commands.push_back(cmd);
}

extern "C" void Gfx_SetViewport(const GfxViewport *viewport)
{
    Set(s_state.viewport, *viewport);
}

extern "C" void Gfx_SetVirtualScreen(int width, int height)
{
    if (width <= 0 || height <= 0)
        width = height = 0;
    if (s_state.screenW == width && s_state.screenH == height)
        return;
    s_state.screenW = width;
    s_state.screenH = height;
    Touch();
}

extern "C" void Gfx_GetViewport(GfxViewport *viewport)
{
    *viewport = s_state.viewport;
}

extern "C" void Gfx_PushState(void)
{
    s_savedStates.push_back({s_state, s_target});
}

extern "C" void Gfx_PopState(void)
{
    if (s_savedStates.empty())
        return;
    const auto saved = s_savedStates.back();
    s_savedStates.pop_back();
    s_state = saved.first;
    Touch();
    if (s_target != saved.second)
        Gfx_SetRenderTarget(saved.second);
}

extern "C" void Gfx_SetRenderTarget(GfxTexture *target)
{
    GfxCommand cmd = {};
    cmd.type = GFX_CMD_TARGET;
    cmd.target = target;
    s_frame.commands.push_back(cmd);
    s_target = target;
    if (target != NULL)
        Root(target)->renderedTo = true;
}

extern "C" void Gfx_CaptureBackdrop(void)
{
    if (s_target != NULL)
        return;
    GfxCommand cmd = {};
    cmd.type = GFX_CMD_BACKDROP;
    s_frame.commands.push_back(cmd);
}

extern "C" void Gfx_SetDistanceFade(float start, float end)
{
    SetPixels(s_state.fadeStart, start);
    SetPixels(s_state.fadeEnd, end);
}

// ---- fixed-function state --------------------------------------------------------------

extern "C" void Gfx_SetTransform(int type, const GfxMatrix *matrix)
{
    switch (type) {
    case GFX_TRANSFORM_WORLD: s_state.world = *matrix; break;   // RecordState compares it
    case GFX_TRANSFORM_VIEW: Set(s_state.view, *matrix); break;
    case GFX_TRANSFORM_PROJECTION: Set(s_state.projection, *matrix); break;
    case GFX_TRANSFORM_TEXTURE0: Set(s_state.texture[0], *matrix); break;
    case GFX_TRANSFORM_TEXTURE1: Set(s_state.texture[1], *matrix); break;
    }
}

extern "C" void Gfx_GetTransform(int type, GfxMatrix *matrix)
{
    switch (type) {
    case GFX_TRANSFORM_WORLD: *matrix = s_state.world; break;
    case GFX_TRANSFORM_VIEW: *matrix = s_state.view; break;
    case GFX_TRANSFORM_PROJECTION: *matrix = s_state.projection; break;
    case GFX_TRANSFORM_TEXTURE0: *matrix = s_state.texture[0]; break;
    case GFX_TRANSFORM_TEXTURE1: *matrix = s_state.texture[1]; break;
    default: *matrix = Identity(); break;
    }
}

extern "C" void Gfx_SetLight(int index, const GfxLight *light)
{
    if (index >= 0 && index < 8)
        Set(s_state.lights[index], *light);
}

extern "C" void Gfx_GetLight(int index, GfxLight *light)
{
    if (index >= 0 && index < 8)
        *light = s_state.lights[index];
}

extern "C" void Gfx_EnableLight(int index, BOOL enable)
{
    if (index >= 0 && index < 8)
        Set<bool>(s_state.lightEnabled[index], enable != 0);
}

extern "C" void Gfx_SetMaterial(const GfxMaterial *material)
{
    Set(s_state.material, *material);
}

#define SETTER(name, type, field)       \
    extern "C" void name(type value)    \
    {                                   \
        Set<decltype(s_state.field)>(s_state.field, value); \
    }

SETTER(Gfx_SetAmbient, DWORD, ambient)
SETTER(Gfx_SetDiffuseMaterialSource, int, diffuseSource)
SETTER(Gfx_SetSpecularMaterialSource, int, specularSource)
SETTER(Gfx_SetAmbientMaterialSource, int, ambientSource)
SETTER(Gfx_SetEmissiveMaterialSource, int, emissiveSource)
SETTER(Gfx_SetFogVertexMode, int, fogVertexMode)
SETTER(Gfx_SetFogTableMode, int, fogTableMode)
SETTER(Gfx_SetFogStart, float, fogStart)
SETTER(Gfx_SetFogEnd, float, fogEnd)

#define PIXEL_SETTER(name, type, field) \
    extern "C" void name(type value)    \
    {                                   \
        SetPixels<decltype(s_state.field)>(s_state.field, value); \
    }

PIXEL_SETTER(Gfx_SetFogColor, DWORD, fogColor)
PIXEL_SETTER(Gfx_SetDepthFunc, int, depthFunc)
PIXEL_SETTER(Gfx_SetCullMode, int, cull)
PIXEL_SETTER(Gfx_SetSrcBlend, int, srcBlend)
PIXEL_SETTER(Gfx_SetDestBlend, int, destBlend)
PIXEL_SETTER(Gfx_SetAlphaFunc, int, alphaFunc)
PIXEL_SETTER(Gfx_SetAlphaRef, DWORD, alphaRef)
PIXEL_SETTER(Gfx_SetTextureFactor, DWORD, textureFactor)

#define BOOL_SETTER(name, field)        \
    extern "C" void name(BOOL value)    \
    {                                   \
        Set<bool>(s_state.field, value != 0); \
    }

BOOL_SETTER(Gfx_SetLighting, lighting)
BOOL_SETTER(Gfx_SetSpecular, specular)
BOOL_SETTER(Gfx_SetLocalViewer, localViewer)
BOOL_SETTER(Gfx_SetNormalizeNormals, normalizeNormals)
BOOL_SETTER(Gfx_SetColorVertex, colorVertex)
BOOL_SETTER(Gfx_SetFog, fog)

#define PIXEL_BOOL_SETTER(name, field)  \
    extern "C" void name(BOOL value)    \
    {                                   \
        SetPixels<bool>(s_state.field, value != 0); \
    }

PIXEL_BOOL_SETTER(Gfx_SetDepthTest, depthTest)
PIXEL_BOOL_SETTER(Gfx_SetDepthWrite, depthWrite)
PIXEL_BOOL_SETTER(Gfx_SetAlphaBlend, alphaBlend)
PIXEL_BOOL_SETTER(Gfx_SetAlphaTest, alphaTest)

#define STAGE_SETTER(name, type, field)             \
    extern "C" void name(int stage, type value)     \
    {                                               \
        GfxStageState *s = Stage(stage);            \
        if (s != NULL)                              \
            Set<decltype(s->field)>(s->field, value); \
    }

STAGE_SETTER(Gfx_SetStageTexCoordIndex, DWORD, texCoordIndex)
STAGE_SETTER(Gfx_SetStageTextureTransform, int, textureTransform)

#define PIXEL_STAGE_SETTER(name, type, field)       \
    extern "C" void name(int stage, type value)     \
    {                                               \
        GfxStageState *s = Stage(stage);            \
        if (s != NULL)                              \
            SetPixels<decltype(s->field)>(s->field, value); \
    }


// For an executor that takes joined draws, a new texture alone starts no new
// state: each draw has its textures (GfxCommand::textures).
extern "C" void Gfx_SetTexture(int stage, GfxTexture *texture)
{
    GfxStageState *s = Stage(stage);
    if (s == NULL)
        return;
    if (s_executor != NULL && s_executor->JoinsDraws())
        s->texture = texture;
    else
        SetPixels(s->texture, texture);
}

PIXEL_STAGE_SETTER(Gfx_SetStageColorOp, int, colorOp)
PIXEL_STAGE_SETTER(Gfx_SetStageColorArg1, int, colorArg1)
PIXEL_STAGE_SETTER(Gfx_SetStageColorArg2, int, colorArg2)
PIXEL_STAGE_SETTER(Gfx_SetStageAlphaOp, int, alphaOp)
PIXEL_STAGE_SETTER(Gfx_SetStageAlphaArg1, int, alphaArg1)
PIXEL_STAGE_SETTER(Gfx_SetStageAlphaArg2, int, alphaArg2)
PIXEL_STAGE_SETTER(Gfx_SetStageAddress, int, address)
PIXEL_STAGE_SETTER(Gfx_SetStageMinFilter, int, minFilter)
PIXEL_STAGE_SETTER(Gfx_SetStageMagFilter, int, magFilter)
PIXEL_STAGE_SETTER(Gfx_SetStageMipFilter, int, mipFilter)
PIXEL_STAGE_SETTER(Gfx_SetStageMaxMipLevel, int, maxMipLevel)
PIXEL_STAGE_SETTER(Gfx_SetStageBumpEnvLScale, float, bumpLumScale)
PIXEL_STAGE_SETTER(Gfx_SetStageBumpEnvLOffset, float, bumpLumOffset)

extern "C" void Gfx_SetStageBumpEnvMat(int stage, int index, float value)
{
    GfxStageState *s = Stage(stage);
    if (s != NULL && index >= 0 && index < 4)
        SetPixels(s->bumpMat[index], value);
}

// ---- geometry ----------------------------------------------------------------------

extern "C" GfxVertexBuffer *Gfx_CreateVertexBuffer(DWORD vertexCount)
{
    GfxVertexBuffer *buffer = new GfxVertexBuffer;
    buffer->vertices.resize(vertexCount);
    memset(buffer->vertices.data(), 0, vertexCount * sizeof(GfxVertex));
    TraceVertexBuffer(buffer, 1);
    return buffer;
}

extern "C" void Gfx_DestroyVertexBuffer(GfxVertexBuffer *buffer)
{
    if (buffer == NULL)
        return;
    // The batch jobs that read its vertices are done first. Draws recorded
    // this frame may still use it.
    SettleBatches(buffer, 0, (uint32_t)buffer->vertices.size());
    if (buffer->referencedRecord == s_record || buffer->snapshotRecord == s_record)
        Flush(FLUSH_BUFFER_GONE);
    if (s_executor != NULL)
        s_executor->DestroyVertexBuffer(buffer);
    if (s_copied.buffer == buffer)
        s_copied = CopiedRange();
    TraceVertexBuffer(buffer, -1);
    delete buffer;
}

// Before the game writes count vertices of a buffer from first: the batch
// jobs that read them are done (GfxSmallItem::buffer). The blocks of the
// buffer have the numbers of those jobs. Most are done already: they run
// while the game records.
static void SettleBatches(GfxVertexBuffer *buffer, uint32_t first, uint32_t count)
{
    const uint32_t latest = buffer->batchBlocks.Latest(first, count);
    if (latest == 0 || (int32_t)(Gfx_BatchJobsDone() - latest) >= 0)
        return;
    AddPendingBatchJob();
    Gfx_WaitForBatchJobs(latest, 0);
}

extern "C" GfxVertex *Gfx_LockVertexBuffer(GfxVertexBuffer *buffer)
{
    if (buffer == NULL)
        return NULL;
    SettleBatches(buffer, 0, (uint32_t)buffer->vertices.size());
    // Draws recorded this frame refer to the contents before this lock: run
    // them first. It is rare: buffers drawn by reference are rarely locked.
    // A buffer that the frame has a snapshot of is streaming from now on:
    // the game changes it between its draws.
    if (buffer->snapshotRecord == s_record)
        buffer->streaming = true;
    if (buffer->referencedRecord == s_record || buffer->snapshotRecord == s_record)
        Flush(FLUSH_BUFFER_LOCK);
    buffer->version++;
    buffer->lastLockFrame = s_presentedFrames;
    buffer->positionsUnknown = true;
    return buffer->vertices.data();
}

// Whether a lock of count vertices of a buffer from first is a range lock
// (gfx_range_locks.h). Only a buffer whose draws refer to its GPU copy has
// parts: a buffer that was locked whole a short time ago has a snapshot or
// copies.
static bool RangeLockPossible(const GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount)
{
    return RangeLocksWanted() && !buffer->streaming && vertexCount != 0 &&
           firstVertex + vertexCount <= buffer->vertices.size() &&
           s_presentedFrames - buffer->lastLockFrame >= kStaticAfterFrames;
}

// Before the game writes count vertices of a buffer from first, as a range
// lock: what refers to the vertices as they are runs first, and their blocks
// are recent.
static void BeginRangeWrite(GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount)
{
    SettleBatches(buffer, firstVertex, vertexCount);
    // The draws recorded so far that refer to the GPU copy keep its
    // contents: the copy changes only with a patch. A patch of this frame
    // for these vertices would take what the game writes now: the draws
    // recorded so far run first.
    if (buffer->patchRecord == s_record) {
        bool patched = false;
        for (const GfxPatch &p : s_frame.patches)
            patched = patched || (p.buffer == buffer && p.first < firstVertex + vertexCount && p.end > firstVertex);
        if (patched)
            Flush(FLUSH_BUFFER_LOCK);
    }
    // The frame has a snapshot of these vertices, which the executor takes
    // when the frame runs: the draws recorded so far run first.
    if (buffer->snapshotRecord == s_record) {
        const GfxSnapshot &snapshot = s_frame.snapshots[buffer->snapshotIndex];
        if (snapshot.first < firstVertex + vertexCount && snapshot.end > firstVertex)
            Flush(FLUSH_BUFFER_LOCK);
    }
    if (s_copied.buffer == buffer)
        s_copied = CopiedRange();
    buffer->rangeLocks.Lock(firstVertex, vertexCount, s_presentedFrames);
}

extern "C" GfxVertex *Gfx_LockVertexBufferRange(GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount)
{
    if (buffer == NULL)
        return NULL;
    if (!RangeLockPossible(buffer, firstVertex, vertexCount))
        return Gfx_LockVertexBuffer(buffer);
    BeginRangeWrite(buffer, firstVertex, vertexCount);
    // The caller can write any position: the boxes of these vertices are
    // measured again.
    buffer->lockStamps.Mark(firstVertex, vertexCount, ++buffer->lockSerial);
    return buffer->vertices.data();
}

extern "C" void Gfx_WriteVertexBufferRange(GfxVertexBuffer *buffer, DWORD firstVertex, const GfxVertex *vertices,
                                           DWORD vertexCount)
{
    if (buffer == NULL || vertices == NULL || vertexCount == 0 ||
        firstVertex + vertexCount > buffer->vertices.size())
        return;
    const size_t bytes = (size_t)vertexCount * sizeof(GfxVertex);
    if (!RangeLockPossible(buffer, firstVertex, vertexCount)) {
        memcpy(Gfx_LockVertexBuffer(buffer) + firstVertex, vertices, bytes);
        return;
    }
    GfxVertex *target = &buffer->vertices[firstVertex];
    // The vertices that the buffer has: nothing changes, and no draw of
    // them is a draw of changed vertices.
    if (memcmp(target, vertices, bytes) == 0)
        return;
    BeginRangeWrite(buffer, firstVertex, vertexCount);
    // The boxes of these vertices stay while their positions stay (the
    // stage lighting writes colours: a box measured again for each draw of
    // each mesh in the view took 1 to 2 ms of a frame of a wood).
    bool moved = false;
    for (DWORD i = 0; i < vertexCount && !moved; i++)
        moved = memcmp(&target[i].x, &vertices[i].x, 3 * sizeof(float)) != 0;
    memcpy(target, vertices, bytes);
    if (moved)
        buffer->lockStamps.Mark(firstVertex, vertexCount, ++buffer->lockSerial);
}

extern "C" void Gfx_UnlockVertexBuffer(GfxVertexBuffer *)
{
}

extern "C" BOOL Gfx_IsRangeVisible(GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount)
{
    if (buffer == NULL || vertexCount == 0 || firstVertex + vertexCount > buffer->vertices.size())
        return FALSE;
    if (s_executor == NULL || !CullWanted())
        return TRUE;
    return BoxOutsideView(ClipMatrix(s_state.world, 0), BufferRangeBox(buffer, firstVertex, vertexCount)) ? FALSE : TRUE;
}

extern "C" BOOL Gfx_MergesGroundDraws(void)
{
    if (s_executor == NULL)
        return FALSE;
    if (s_mergeGroundSetting == -2)
        ReadDrawSettings();
    return (s_mergeGroundSetting < 0 ? s_executor->JoinsDraws() : s_mergeGroundSetting != 0) ? TRUE : FALSE;
}

extern "C" void Gfx_DrawPrimitiveVB(int primitive, GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount)
{
    s_traceCaller = __builtin_return_address(0);
    if (buffer == NULL || firstVertex + vertexCount > buffer->vertices.size())
        return;
    RecordDraw(primitive, false, buffer->vertices.data() + firstVertex, vertexCount, NULL, 0, buffer, firstVertex);
}

extern "C" void Gfx_DrawIndexedPrimitiveVB(int primitive, GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount,
                                           const WORD *indices, DWORD indexCount)
{
    s_traceCaller = __builtin_return_address(0);
    if (buffer == NULL || firstVertex + vertexCount > buffer->vertices.size())
        return;
    RecordDraw(primitive, false, buffer->vertices.data() + firstVertex, vertexCount, indices, indexCount, buffer,
               firstVertex);
}

namespace {

// The small draws of the frame that no batch job has yet: those from
// s_pendingFirst of the frame's batches, of the batch command
// s_pendingPlaceholder.
uint32_t s_pendingPlaceholder, s_pendingFirst;
bool s_pending;

// The batches of the frame, with room for count more draws and their
// indices. The jobs read the draws where they are: before the draws move to
// more room, the jobs are done.
GfxBatchData &BatchRoom(size_t draws, size_t indices)
{
    constexpr size_t kDraws = 4096, kIndices = 4096 * 12;   // a frame of a wood has some 1,300 draws
    if (s_frame.batch == nullptr) {
        s_frame.batch = std::make_shared<GfxBatchData>();
        s_frame.batch->items.reserve(kDraws);
        s_frame.batch->indices.reserve(kIndices);
    }
    GfxBatchData &data = *s_frame.batch;
    if (data.items.size() + draws > data.items.capacity() || data.indices.size() + indices > data.indices.capacity()) {
        Gfx_WaitForBatchJobs(Gfx_BatchJobsAdded(), 0);
        data.items.reserve((data.items.size() + draws) * 2);
        data.indices.reserve((data.indices.size() + indices) * 2);
    }
    return data;
}

} // namespace

// Gives the small draws that no batch job has yet to a job: at the end of a
// batch command, of a call of the game, and of the frame.
static void AddPendingBatchJob()
{
    if (!s_pending)
        return;
    s_pending = false;
    GfxBatchData &data = *s_frame.batch;
    const GfxCommand &batch = s_frame.commands[s_pendingPlaceholder];
    const GfxDrawState &state = s_frame.states[batch.state];
    GfxBatchJob job = {};
    job.data = &data;
    job.items = data.items.data() + s_pendingFirst;
    job.itemCount = (uint32_t)data.items.size() - s_pendingFirst;
    job.indices = data.indices.data();
    job.indexCount = (uint32_t)data.indices.size();
    job.placeholder = s_pendingPlaceholder;
    job.state = batch.state;
    job.texture1 = batch.textures[1];
    job.cull = batch.cull;
    job.part = batch.part;
    job.view = state.view;
    job.projection = state.projection;
    Gfx_AddBatchJob(job);
}

namespace {

// Adds a small draw of a batch to the frame (Gfx_DrawBatch), with the world
// matrix and the stage 0 texture of the state. The batch command at the end
// of the frame's commands takes it while the state it recorded is the state;
// else a new batch command starts. The atlas place is found here: the atlas
// belongs to the recorder. The vertices stay in the buffer
// (GfxSmallItem::buffer): the recorder does not read them.
void RecordSmallItem(GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount, const WORD *indices,
                     DWORD indexCount)
{
#if defined(VITA) && defined(OPENCMR2_DEV)
    g_sysVitaDrawCounts.calls++;
#endif
    s_drawOverride = DrawOverride(false);
    const bool open = !s_frame.commands.empty() && s_frame.commands.back().type == GFX_CMD_SMALL_BATCH &&
                      StateIsRecorded(true) && s_frame.commands.back().state == s_frame.states.size() - 1 &&
                      s_frame.commands.back().textures[1] == s_state.stages[1].texture;
    if (!open) {
        // The draws of the batch before go to their job.
        AddPendingBatchJob();
        GfxCommand cmd = {};
        cmd.part = ScenePartNow();
        cmd.type = GFX_CMD_SMALL_BATCH;
        cmd.reach = -1.0f;
        cmd.state = RecordState(true);
        cmd.textures[1] = s_state.stages[1].texture;
        cmd.cull = CullWanted();
        cmd.firstItem = s_frame.batch != nullptr ? (uint32_t)s_frame.batch->items.size() : 0;
        s_frame.commands.push_back(cmd);
#if defined(VITA) && defined(OPENCMR2_DEV)
        g_sysVitaDrawCounts.commands++;
#endif
    }
    GfxBatchData &data = BatchRoom(1, indexCount);
    if (!s_pending) {
        s_pending = true;
        s_pendingPlaceholder = (uint32_t)s_frame.commands.size() - 1;
        s_pendingFirst = (uint32_t)data.items.size();
    }
    data.items.emplace_back();
    GfxSmallItem &item = data.items.back();
    item.world = s_state.world;
    item.buffer = buffer;
    item.texture = s_state.stages[0].texture;
    item.atlas = NULL;
    item.atlasScale = 0.0f;
    if (const AtlasEntry *e = AtlasPlaceOfBatchDraw()) {
        item.atlas = s_atlas;
        item.atlasX = e->x;
        item.atlasY = e->y;
        item.textureWidth = e->texture->width;
        item.textureHeight = e->texture->height;
        item.atlasScale = 1.0f / (float)s_atlasSize;
    }
    item.firstVertex = firstVertex;
    item.vertexCount = vertexCount;
    item.firstIndex = (uint32_t)data.indices.size();
    item.indexCount = indexCount;
    data.indices.insert(data.indices.end(), indices, indices + indexCount);
    s_frame.commands.back().itemCount++;
    // The number of the job that reads the vertices: the next one.
    buffer->batchBlocks.Mark(firstVertex, vertexCount, Gfx_BatchJobsAdded() + 1);
    // The executor makes ready each texture that the draw can have.
    // (GfxFrame::smallTextures: each texture once in a recording. A search
    // of the list for each draw was a third of the time of this function.)
    for (GfxTexture *t : { item.texture, item.atlas })
        if (t != NULL && t->batchRecord != s_record) {
            t->batchRecord = s_record;
            s_frame.smallTextures.push_back(t);
        }
    s_counts.batched++;
}

void DrawBatchNow(const GfxBatchDraw *draws, DWORD count, const void *caller)
{
    // The small draws go into the frame as a batch where the executor takes
    // joined draws. Otherwise, and with the trace (a line for each draw),
    // each draw is recorded as the game makes it alone.
    const bool batch = s_executor != NULL && s_executor->JoinsDraws() && TraceFile() == NULL;
    for (DWORD i = 0; i < count; i++) {
        const GfxBatchDraw &d = draws[i];
        if (d.world != NULL)
            Gfx_SetTransform(GFX_TRANSFORM_WORLD, d.world);
        Gfx_SetTexture(0, d.texture);
        GfxVertexBuffer *buffer = d.buffer;
        if (buffer == NULL || d.firstVertex + d.vertexCount > buffer->vertices.size())
            continue;
        if (batch && d.vertexCount > 0 && d.vertexCount <= kSmallDrawVertices && d.indices != NULL &&
            d.indexCount > 0 && Gfx_SmallDrawWorld(s_state.world, kIdentityMatrix)) {
            RecordSmallItem(buffer, d.firstVertex, d.vertexCount, d.indices, d.indexCount);
            continue;
        }
        s_traceCaller = caller;
        RecordDraw(GFX_TRIANGLELIST, false, buffer->vertices.data() + d.firstVertex, d.vertexCount, d.indices,
                   d.indexCount, buffer, d.firstVertex);
    }
    // The small draws of this call go to their job now: it runs while the
    // game makes the next ones.
    AddPendingBatchJob();
}

} // namespace

extern "C" void Gfx_DrawBatch(const GfxBatchDraw *draws, DWORD count)
{
    const void *caller = __builtin_return_address(0);
#if defined(VITA) && defined(OPENCMR2_DEV)
    // The heartbeat's batch time: a few tens of calls in a frame.
    if (s_perfStatsSetting != 0) {
        const unsigned long long start = SysVita_Micros();
        DrawBatchNow(draws, count, caller);
        SysVita_BatchSample(SysVita_Micros() - start, (unsigned)count);
        return;
    }
#endif
    DrawBatchNow(draws, count, caller);
}

// ---- a draw whose vertices the game gives in parts (Gfx_BeginPrimitive) ----
//
// The game copied the triangles of a 3D layer into a vertex buffer and drew
// that: the recorder then copied them into the frame (the tyre marks: 200 KB
// in a frame, twice on the game's thread). For an executor that takes joined
// draws the parts go into the frame's vertex data at once, and the end makes
// a draw of them where they are. Otherwise the parts collect here, and the
// end records the draw as any other.

namespace {

int s_openPrimitive = -1;               // of Gfx_BeginPrimitive; -1: none
bool s_openInFrame;                     // its vertices are in the frame's data, from s_openOffset
uint32_t s_openOffset;
std::vector<GfxVertex> s_openVertices;  // its vertices, when they are not in the frame

// The draw of the open primitive from its vertices in the frame's data: as
// RecordDrawNow records a draw of a vertex buffer that the game locks
// between its draws (a copy of its vertices, with their box), without the
// copy.
void RecordDrawInFrame(int primitive, uint32_t offset, DWORD vertexCount)
{
#if defined(VITA) && defined(OPENCMR2_DEV)
    g_sysVitaDrawCounts.calls++;
#endif
    const GfxVertex *vertices = (const GfxVertex *)(s_frame.vertices.data() + offset);
    s_drawOverride = DrawOverride(false);
    GfxCommand cmd = {};
    cmd.part = ScenePartNow();
    cmd.type = GFX_CMD_DRAW;
    cmd.primitive = primitive;
    cmd.transformed = false;
    cmd.vertexCount = vertexCount;
    cmd.firstVertexByte = offset;
    cmd.hasBox = true;
    cmd.reach = -1.0f;
    VertexBox(vertices, vertexCount, cmd.boxLow, cmd.boxHigh);
    // The test against the view: a draw outside it is left out, and its
    // vertices with it.
    if (CullWanted()) {
        GfxBox box;
        memcpy(box.low, cmd.boxLow, sizeof(box.low));
        memcpy(box.high, cmd.boxHigh, sizeof(box.high));
        if (BoxOutsideView(ClipMatrix(s_state.world, 0), box)) {
            s_counts.culled++;
            s_frame.vertices.resize(offset);
            return;
        }
    }
    cmd.firstIndex = ~0u;
    cmd.indexCount = 0;
    cmd.state = RecordState(false);
    cmd.textures[0] = s_state.stages[0].texture;
    cmd.textures[1] = s_state.stages[1].texture;
    s_frame.commands.push_back(cmd);
#if defined(VITA) && defined(OPENCMR2_DEV)
    g_sysVitaDrawCounts.commands++;
    g_sysVitaDrawCounts.vertexBytes += vertexCount * sizeof(GfxVertex);
#endif
}

} // namespace

extern "C" void Gfx_BeginPrimitive(int primitive)
{
    s_openPrimitive = primitive;
    // In the frame: lists and strips, which are drawn as they are. A fan
    // becomes a list, and the trace has a line for each draw: the other way.
    s_openInFrame = s_executor != NULL && s_executor->JoinsDraws() && TraceFile() == NULL &&
                    primitive != GFX_TRIANGLEFAN;
    if (s_openInFrame)
        s_openOffset = AppendVertices(NULL, 0, sizeof(GfxVertex));
    else
        s_openVertices.clear();
}

extern "C" void Gfx_AddPrimitiveVertices(const GfxVertex *vertices, DWORD vertexCount)
{
    if (s_openPrimitive < 0 || vertices == NULL || vertexCount == 0)
        return;
    if (s_openInFrame)
        s_frame.vertices.insert(s_frame.vertices.end(), (const uint8_t *)vertices,
                                (const uint8_t *)(vertices + vertexCount));
    else
        s_openVertices.insert(s_openVertices.end(), vertices, vertices + vertexCount);
}

extern "C" void Gfx_EndPrimitive(void)
{
    const int primitive = s_openPrimitive;
    s_openPrimitive = -1;
    if (primitive < 0)
        return;
    if (!s_openInFrame) {
        s_traceCaller = __builtin_return_address(0);
        if (!s_openVertices.empty())
            RecordDraw(primitive, false, s_openVertices.data(), (DWORD)s_openVertices.size(), NULL, 0);
        return;
    }
    const size_t bytes = s_frame.vertices.size() - s_openOffset;
    if (s_executor == NULL || bytes < sizeof(GfxVertex)) {
        s_frame.vertices.resize(s_openOffset);
        return;
    }
    RecordDrawInFrame(primitive, s_openOffset, (DWORD)(bytes / sizeof(GfxVertex)));
}

extern "C" void Gfx_DrawPrimitive(int primitive, const GfxVertex *vertices, DWORD vertexCount)
{
    s_traceCaller = __builtin_return_address(0);
    RecordDraw(primitive, false, vertices, vertexCount, NULL, 0);
}

extern "C" void Gfx_DrawIndexedPrimitive(int primitive, const GfxVertex *vertices, DWORD vertexCount, const WORD *indices,
                                         DWORD indexCount)
{
    s_traceCaller = __builtin_return_address(0);
    RecordDraw(primitive, false, vertices, vertexCount, indices, indexCount);
}

extern "C" void Gfx_DrawPrimitiveTL(int primitive, const GfxTLVertex *vertices, DWORD vertexCount)
{
    s_traceCaller = __builtin_return_address(0);
    RecordDraw(primitive, true, vertices, vertexCount, NULL, 0);
}

extern "C" void Gfx_DrawIndexedPrimitiveTL(int primitive, const GfxTLVertex *vertices, DWORD vertexCount,
                                           const WORD *indices, DWORD indexCount)
{
    s_traceCaller = __builtin_return_address(0);
    RecordDraw(primitive, true, vertices, vertexCount, indices, indexCount);
}

// ---- textures -------------------------------------------------------------------------

extern "C" GfxTexture *Gfx_CreateTexture(int width, int height, int format, int mipLevels, DWORD flags)
{
    if (width <= 0 || height <= 0)
        return NULL;
    // Compressed and bump textures keep one level: the original's mip
    // generation (a DirectDraw stretch blit) only worked on plain RGB.
    if (format != GFX_FORMAT_BGRX8 && format != GFX_FORMAT_BGRA8)
        mipLevels = 1;
    if (flags & (GFX_TEXTURE_RENDER_TARGET | GFX_TEXTURE_CUBE))
        mipLevels = 1;
    if (mipLevels < 1)
        mipLevels = 1;
    if (mipLevels > 16)
        mipLevels = 16;

    GfxTexture *t = new GfxTexture();
    t->width = width;
    t->height = height;
    t->format = format;
    t->mipLevels = mipLevels;
    t->flags = flags;
    t->cube = NULL;
    t->face = 0;
    t->dirty = true;
    t->renderedTo = false;
    t->gpu = NULL;
    t->replacement = NULL;
    int faces = (flags & GFX_TEXTURE_CUBE) ? 6 : 1;
    for (int f = 0; f < faces; f++) {
        for (int m = 0; m < mipLevels; m++) {
            int w = std::max(1, width >> m), h = std::max(1, height >> m);
            t->levels[f][m].assign(Gfx_LevelSize(format, w, h, NULL), 0);
        }
    }
    for (int f = 0; f < 6; f++)
        t->faceViews[f] = NULL;
    if (flags & GFX_TEXTURE_CUBE) {
        t->faceViews[0] = t;
        for (int f = 1; f < 6; f++) {
            GfxTexture *v = new GfxTexture();
            v->width = width;
            v->height = height;
            v->format = format;
            v->mipLevels = 1;
            v->flags = flags;
            v->cube = t;
            v->face = f;
            v->gpu = NULL;
            t->faceViews[f] = v;
        }
    }
    s_textures.push_back(t);
    TraceTexture(t, 1);
    return t;
}

extern "C" void Gfx_DestroyTexture(GfxTexture *texture)
{
    if (texture == NULL || texture->cube != NULL)
        return;     // face views go with their cube
    AtlasForget(texture);
    for (int i = 0; i < 2; i++)
        if (Root(s_state.stages[i].texture) == texture)
            s_state.stages[i].texture = NULL;
    // Draws recorded this frame may still use it: flush them first.
    {
        auto uses = [texture](GfxTexture *t) {
            return Root(t) == texture || (Root(t) != NULL && Root(t)->replacement == texture);
        };
        bool used = false;
        for (const GfxDrawState &state : s_frame.states)
            used = used || uses(state.stages[0].texture) || uses(state.stages[1].texture);
        for (const GfxCommand &command : s_frame.commands)
            used = used || ((command.type == GFX_CMD_DRAW || command.type == GFX_CMD_SMALL_BATCH) &&
                            (uses(command.textures[0]) || uses(command.textures[1])));
        for (GfxTexture *t : s_frame.smallTextures)
            used = used || uses(t);
        if (used)
            Flush(FLUSH_TEXTURE_LOCK);
    }
    // Shutdown may visit a replacement before its owner. Detach it so the
    // owner cannot destroy it a second time (also applies to font fields).
    for (GfxTexture *owner : s_textures)
        if (owner->replacement == texture)
            owner->replacement = NULL;
    Gfx_DestroyTexture(texture->replacement);
    if (s_executor != NULL)
        s_executor->DestroyTexture(texture);
    s_textures.erase(std::remove(s_textures.begin(), s_textures.end(), texture), s_textures.end());
    TraceTexture(texture, -1);
    for (int f = 1; f < 6; f++)
        delete texture->faceViews[f];
    delete texture;
}

extern "C" void Gfx_SetTextureReplacement(GfxTexture *texture, GfxTexture *replacement)
{
    texture = Root(texture);
    if (texture == NULL || texture->replacement == replacement)
        return;
    // Draws recorded so far read the previous one. They have run (the worker
    // of the threaded executor reads the replacement too).
    Flush(FLUSH_REPLACEMENT);
    Gfx_WaitForExecutor();
    Gfx_DestroyTexture(texture->replacement);
    texture->replacement = replacement;
    // A texture with a replacement does not go into the atlas.
    texture->atlasSerial = 0;
}

extern "C" int Gfx_GetDistanceFieldScale(GfxTexture *texture)
{
    GfxTexture *drawn = Gfx_DrawnTexture(texture);
    if (drawn == NULL || !(drawn->flags & (GFX_TEXTURE_DISTANCE_FIELD | GFX_TEXTURE_VECTOR_ICON)) ||
        Root(texture)->width <= 0)
        return 0;
    return drawn->width / Root(texture)->width;
}

extern "C" void Gfx_VectorizeMenuDot(GfxTexture *texture)
{
    texture = Root(texture);
    if (texture == NULL || texture->replacement != NULL || texture->width != texture->height ||
        (texture->width != 16 && texture->width != 32))
        return;
    // Keep the original coverage for draws at native size and below. At
    // larger sizes the shader draws the same circle from its geometry.
    GfxTexture *dot = Gfx_CreateTexture(texture->width, texture->height, texture->format, 1,
                                       GFX_TEXTURE_MENU_DOT);
    if (dot == NULL)
        return;
    Gfx_CopyTexture(dot, texture);
    Gfx_SetTextureReplacement(texture, dot);
}

extern "C" float Gfx_GetSpriteTexelInset(GfxTexture *texture)
{
    GfxTexture *drawn = Gfx_DrawnTexture(texture);
    if (drawn != NULL && (drawn->flags & GFX_TEXTURE_MENU_DOT))
        return 0.0f;
    int field = Gfx_GetDistanceFieldScale(texture);
    return field > 0 ? 0.5f / field : 0.5f;
}

extern "C" int Gfx_GetTextureMipCount(GfxTexture *texture)
{
    return texture != NULL ? Root(texture)->mipLevels : 0;
}

extern "C" GfxTexture *Gfx_GetCubeFace(GfxTexture *cube, int face)
{
    if (cube == NULL || face < 0 || face > 5)
        return NULL;
    cube = Root(cube);
    return cube->faceViews[face] != NULL ? cube->faceViews[face] : cube;
}

extern "C" BOOL Gfx_LockTexture(GfxTexture *texture, int face, int mip, GfxLockedRect *out)
{
    if (texture == NULL)
        return FALSE;
    if (texture->cube != NULL) {
        face = texture->face;
        texture = texture->cube;
    }
    if (mip < 0 || mip >= texture->mipLevels || face < 0 || face > 5 || texture->levels[face][mip].empty())
        return FALSE;
    // The atlas has a copy of its texels.
    if (texture != s_atlas)
        AtlasForget(texture);
    if (texture->renderedTo && s_executor != NULL) {
        s_executor->ReadTexture(texture);
        texture->renderedTo = false;
    }
    int w = std::max(1, texture->width >> mip), h = std::max(1, texture->height >> mip);
    memset(out, 0, sizeof(*out));
    out->pixels = texture->levels[face][mip].data();
    Gfx_LevelSize(texture->format, w, h, &out->pitch);
    out->width = w;
    out->height = h;
    Gfx_GetPixelFormat(texture->format, &out->format);
    return TRUE;
}

extern "C" void Gfx_UnlockTexture(GfxTexture *texture, int, int)
{
    if (texture != NULL)
        Root(texture)->dirty = true;
}

namespace {

// The top level of a texture as BGRA pixels.
std::vector<uint32_t> ToBGRA(GfxTexture *t)
{
    std::vector<uint32_t> out((size_t)t->width * t->height);
    const uint8_t *src = t->levels[0][0].data();
    switch (t->format) {
    case GFX_FORMAT_BGRX8:
        for (size_t i = 0; i < out.size(); i++)
            out[i] = ((const uint32_t *)src)[i] | 0xff000000;
        break;
    case GFX_FORMAT_BGRA8:
        memcpy(out.data(), src, out.size() * 4);
        break;
    case GFX_FORMAT_BUMP_DUDVL:
        for (size_t i = 0; i < out.size(); i++)
            out[i] = src[i * 3] | src[i * 3 + 1] << 8 | src[i * 3 + 2] << 16 | 0xff000000;
        break;
    case GFX_FORMAT_BC1:
    case GFX_FORMAT_BC3: {
        int blockBytes = t->format == GFX_FORMAT_BC1 ? 8 : 16;
        int bw = (t->width + 3) / 4, bh = (t->height + 3) / 4;
        uint32_t block[16];
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                Gfx_DecodeBlock(t->format, src + (by * bw + bx) * blockBytes, block);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        int px = bx * 4 + x, py = by * 4 + y;
                        if (px < t->width && py < t->height)
                            out[(size_t)py * t->width + px] = block[y * 4 + x];
                    }
            }
        }
        break;
    }
    }
    return out;
}

} // namespace

extern "C" void Gfx_CopyTexture(GfxTexture *dst, GfxTexture *src)
{
    dst = Root(dst);
    src = Root(src);
    if (dst == NULL || src == NULL)
        return;
    if (src->renderedTo && s_executor != NULL) {
        s_executor->ReadTexture(src);
        src->renderedTo = false;
    }
    if (dst->format == src->format && dst->width == src->width && dst->height == src->height) {
        dst->levels[0][0] = src->levels[0][0];
        dst->dirty = true;
        return;
    }
    if (dst->format == GFX_FORMAT_BC1 || dst->format == GFX_FORMAT_BC3) {
        Sys_Log("gfx: cannot convert a texture to a compressed format");
        return;
    }
    std::vector<uint32_t> pixels = ToBGRA(src);
    uint8_t *out = dst->levels[0][0].data();
    for (int y = 0; y < dst->height; y++) {
        int sy = y * src->height / dst->height;
        for (int x = 0; x < dst->width; x++) {
            int sx = x * src->width / dst->width;
            uint32_t p = pixels[(size_t)sy * src->width + sx];
            size_t i = (size_t)y * dst->width + x;
            if (dst->format == GFX_FORMAT_BUMP_DUDVL) {
                out[i * 3] = (uint8_t)p;
                out[i * 3 + 1] = (uint8_t)(p >> 8);
                out[i * 3 + 2] = (uint8_t)(p >> 16);
            } else {
                ((uint32_t *)out)[i] = p;
            }
        }
    }
    dst->dirty = true;
}

extern "C" void Gfx_GenerateMipmaps(GfxTexture *texture)
{
    texture = Root(texture);
    if (texture == NULL || (texture->format != GFX_FORMAT_BGRX8 && texture->format != GFX_FORMAT_BGRA8))
        return;
    for (int m = 1; m < texture->mipLevels; m++) {
        int sw = std::max(1, texture->width >> (m - 1)), sh = std::max(1, texture->height >> (m - 1));
        int dw = std::max(1, texture->width >> m), dh = std::max(1, texture->height >> m);
        const uint8_t *src = texture->levels[0][m - 1].data();
        uint8_t *dst = texture->levels[0][m].data();
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                int sum[4] = { 0, 0, 0, 0 };
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int sx = std::min(sw - 1, x * 2 + dx), sy = std::min(sh - 1, y * 2 + dy);
                        const uint8_t *p = src + ((size_t)sy * sw + sx) * 4;
                        for (int c = 0; c < 4; c++)
                            sum[c] += p[c];
                    }
                uint8_t *q = dst + ((size_t)y * dw + x) * 4;
                for (int c = 0; c < 4; c++)
                    q[c] = (uint8_t)((sum[c] + 2) / 4);
            }
        }
    }
    texture->dirty = true;
}

extern "C" BOOL Gfx_ReadBackBuffer(int x, int y, int width, int height, DWORD *pixels)
{
    if (s_executor != NULL)
        s_executor->CollectReadback(s_readback);
    s_readback.requested = true;
    s_readback.x = x;
    s_readback.y = y;
    s_readback.width = width;
    s_readback.height = height;
    if (!s_readback.ready || s_readback.readyWidth != width || s_readback.readyHeight != height)
        return FALSE;
    memcpy(pixels, s_readback.pixels.data(), (size_t)width * height * 4);
    return TRUE;
}

// ---- BC decoding -----------------------------------------------------------------------

namespace {

uint32_t Rgb565(uint16_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return b | g << 8 | r << 16;
}

uint32_t Mix(uint32_t a, uint32_t b, int wa, int wb, int div)
{
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) {
        uint32_t ca = (a >> s) & 0xff, cb = (b >> s) & 0xff;
        out |= ((ca * wa + cb * wb) / div) << s;
    }
    return out;
}

} // namespace

void Gfx_DecodeBlock(int format, const uint8_t *block, uint32_t out[16])
{
    const uint8_t *colour = format == GFX_FORMAT_BC3 ? block + 8 : block;
    uint16_t c0 = (uint16_t)(colour[0] | colour[1] << 8), c1 = (uint16_t)(colour[2] | colour[3] << 8);
    uint32_t palette[4];
    palette[0] = Rgb565(c0) | 0xff000000;
    palette[1] = Rgb565(c1) | 0xff000000;
    if (c0 > c1 || format == GFX_FORMAT_BC3) {
        palette[2] = Mix(palette[0], palette[1], 2, 1, 3) | 0xff000000;
        palette[3] = Mix(palette[0], palette[1], 1, 2, 3) | 0xff000000;
    } else {
        palette[2] = Mix(palette[0], palette[1], 1, 1, 2) | 0xff000000;
        palette[3] = 0;     // transparent black
    }
    uint32_t bits = colour[4] | colour[5] << 8 | colour[6] << 16 | (uint32_t)colour[7] << 24;
    for (int i = 0; i < 16; i++)
        out[i] = palette[(bits >> (i * 2)) & 3];

    if (format == GFX_FORMAT_BC3) {
        uint8_t a0 = block[0], a1 = block[1];
        uint8_t alphas[8];
        alphas[0] = a0;
        alphas[1] = a1;
        if (a0 > a1) {
            for (int i = 1; i < 7; i++)
                alphas[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        } else {
            for (int i = 1; i < 5; i++)
                alphas[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
            alphas[6] = 0;
            alphas[7] = 255;
        }
        uint64_t abits = 0;
        for (int i = 0; i < 6; i++)
            abits |= (uint64_t)block[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++)
            out[i] = (out[i] & 0x00ffffff) | (uint32_t)alphas[(abits >> (3 * i)) & 7] << 24;
    }
}

// ---- the null executor (headless runs) ---------------------------------------------------

namespace {

struct NullExecutor : GfxExecutor {
    bool SetVideoMode(int, int, bool) override { return true; }
    void SetFullscreen(bool) override {}
    void Execute(GfxFrame &, std::vector<GfxTexture *> &textures, GfxReadback &readback, bool present, bool,
                 bool) override
    {
        for (GfxTexture *t : textures)
            t->dirty = false;
        if (present && readback.requested) {
            readback.pixels.assign((size_t)readback.width * readback.height, 0xff000000);
            readback.readyWidth = readback.width;
            readback.readyHeight = readback.height;
            readback.ready = true;
            readback.requested = false;
        }
    }
    void DestroyTexture(GfxTexture *) override {}
    void ReadTexture(GfxTexture *) override {}
    // OPENCMR2_NULL_JOINS=1: the recorder works as for the GXM executor (the
    // joined draws, the batches, the range locks), for a run of a stage on
    // the PC without a picture.
    bool JoinsDraws() const override { return joins; }
    bool joins = getenv("OPENCMR2_NULL_JOINS") != NULL;
};

} // namespace

GfxExecutor *Gfx_CreateNullExecutor(void)
{
    return new NullExecutor;
}

// ---- shared by the executors -----------------------------------------------------------

#ifndef VITA
void Gfx_SetWindowMode(SDL_Window *window, bool fullscreen, int width, int height)
{
    if (window == NULL)
        return;
    bool wasFullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(window, fullscreen);
    if (fullscreen)
        return;
    // Leaving fullscreen restores the earlier window size; the new size
    // only applies once the window is out of fullscreen.
    if (wasFullscreen)
        SDL_SyncWindow(window);
    int scale = Platform_GetSettingInt("video.window_scale", 1);
    if (scale < 1)
        scale = 1;
    SDL_SetWindowSize(window, width * scale, height * scale);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}
#endif

bool Gfx_FrameHashWanted(void)
{
    return getenv("OPENCMR2_FRAME_HASH") != NULL;
}

void Gfx_HashFrame(const uint8_t *bgra, int width, int height)
{
    static FILE *file = getenv("OPENCMR2_FRAME_HASH") ? fopen(getenv("OPENCMR2_FRAME_HASH"), "w") : NULL;
    static const char *dump = getenv("OPENCMR2_FRAME_DUMP");
    static int every = getenv("OPENCMR2_FRAME_DUMP_EVERY") ? atoi(getenv("OPENCMR2_FRAME_DUMP_EVERY")) : 600;
    static unsigned frame;
    if (file == NULL)
        return;
    const size_t bytes = (size_t)width * height * 4;
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < bytes; i++)
        h = (h ^ bgra[i]) * 1099511628211ull;
    fprintf(file, "%u %dx%d %016llx\n", frame, width, height, (unsigned long long)h);
#ifndef VITA
    if (dump != NULL && every > 0 && frame % every == 0) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame-%06u.bmp", dump, frame);
        SDL_Surface *s = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB8888, (void *)bgra, width * 4);
        if (s != NULL) {
            SDL_SaveBMP(s, path);
            SDL_DestroySurface(s);
        }
    }
#endif
    frame++;
}
