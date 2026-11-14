// The recorder (src/render/gfx_core.cpp): what a frame holds for the game's
// draws. A setter with the value that is already set starts no new state. An
// executor that takes joined draws gets a run of sprites or lines as one
// draw, and a small 3D draw in world space: a run of billboards, each with a
// world matrix of its own, has one state. For that executor a new texture
// alone starts no new state: each draw has its textures. Small quads with
// clamped BC3 textures draw from an atlas of those textures, so a run of
// them with different textures is one draw. The parts of a mesh in a vertex
// buffer that the game locks between its draws share one copy of its
// vertices. The draws of a buffer locked before them share one snapshot,
// which the executor copies. A draw outside the view is left out.

#include "port/sys.h"
#include "port/diagnostics.h"
#include "render/gfx_batch_worker.h"
#include "render/gfx_internal.h"
#include "render/gxm_vertex_uniforms.h"

#include <functional>
#include <string>

#include <cstdio>
#include <cstring>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

extern "C" void Sys_GetDesktopSize(int *w, int *h) { *w = 640; *h = 480; }
extern "C" void Sys_Log(const char *, ...) {}
extern "C" int Sys_GetOption(const char *, int fallback) { return fallback; }
// video.cull_draws: the view test is off in the tests unless a test turns it
// on. The recorder reads the settings again at each present.
int s_cullDraws = 0;
int s_alphaBlend = 1, s_alphaTest = 1;     // video.alpha_blend, video.alpha_test
int s_perfStats = 1;                        // video.perf_stats
int s_skyHalf = 1;                          // video.sky_half
int Platform_GetSettingInt(const char *key, int value)
{
    if (strcmp(key, "video.perf_stats") == 0)
        return s_perfStats;
    if (strcmp(key, "video.alpha_blend") == 0)
        return s_alphaBlend;
    if (strcmp(key, "video.alpha_test") == 0)
        return s_alphaTest;
    if (strcmp(key, "video.sky_half") == 0)
        return s_skyHalf;
    return strcmp(key, "video.cull_draws") == 0 ? s_cullDraws : value;
}
const char *Platform_GetSetting(const char *, const char *value) { return value; }

namespace {

bool s_joins;
GfxFrame s_frame;       // the frame of the last present

struct CaptureExecutor : GfxExecutor {
    bool SetVideoMode(int, int, bool) override { return true; }
    void SetFullscreen(bool) override {}
    void Execute(GfxFrame &frame, std::vector<GfxTexture *> &, GfxReadback &, bool, int, bool) override
    {
        s_frame = frame;
    }
    void DestroyTexture(GfxTexture *) override {}
    void ReadTexture(GfxTexture *) override {}
    bool JoinsDraws() const override { return s_joins; }
};

void Sprite(float x)
{
    GfxTLVertex v[4] = {};
    for (int i = 0; i < 4; i++) {
        v[i].sx = x + (float)(i & 1);
        v[i].sy = (float)(i >> 1);
        v[i].rhw = 1.0f;
    }
    Gfx_DrawPrimitiveTL(GFX_TRIANGLESTRIP, v, 4);
}

void Line(float x)
{
    GfxTLVertex v[2] = {};
    v[0].sx = x;
    v[1].sx = x + 1.0f;
    Gfx_DrawPrimitiveTL(GFX_LINELIST, v, 2);
}

unsigned Draws()
{
    unsigned n = 0;
    for (const GfxCommand &c : s_frame.commands)
        n += c.type == GFX_CMD_DRAW;
    return n;
}

const GfxCommand &Draw(unsigned index)
{
    for (const GfxCommand &c : s_frame.commands)
        if (c.type == GFX_CMD_DRAW && index-- == 0)
            return c;
    return s_frame.commands[0];
}

float SpriteX(const GfxCommand &c, unsigned vertex)
{
    GfxTLVertex v;
    memcpy(&v, s_frame.vertices.data() + c.firstVertexByte + vertex * sizeof(GfxTLVertex), sizeof(v));
    return v.sx;
}

GfxVertex Vertex3D(const GfxCommand &c, unsigned vertex)
{
    GfxVertex v;
    memcpy(&v, s_frame.vertices.data() + c.firstVertexByte + vertex * sizeof(GfxVertex), sizeof(v));
    return v;
}

GfxMatrix Identity()
{
    GfxMatrix m{};
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

// A billboard of the game: a quad of two triangles with a world matrix of
// its own (a scale and a place).
void Billboard(float scale, float x, float z)
{
    GfxMatrix world = Identity();
    world._11 = world._22 = world._33 = scale;
    world._41 = x;
    world._43 = z;
    Gfx_SetTransform(GFX_TRANSFORM_WORLD, &world);
    GfxVertex v[4] = {};
    for (int i = 0; i < 4; i++) {
        v[i].x = (float)(i & 1);
        v[i].y = (float)(i >> 1);
        v[i].nz = -1.0f;
        v[i].diffuse = 0xff102030 + (DWORD)i;
        v[i].u0 = (float)(i & 1);
    }
    const WORD quad[6] = { 0, 1, 2, 2, 1, 3 };
    Gfx_DrawIndexedPrimitive(GFX_TRIANGLELIST, v, 4, quad, 6);
}

bool IsIdentity(const GfxMatrix &m)
{
    const GfxMatrix identity = Identity();
    return memcmp(&m, &identity, sizeof(m)) == 0;
}

// A BC3 texture whose blocks all differ, by a seed.
GfxTexture *TreeTexture(int width, int height, unsigned seed)
{
    GfxTexture *t = Gfx_CreateTexture(width, height, GFX_FORMAT_BC3, 1, 0);
    GfxLockedRect locked;
    if (t == NULL || !Gfx_LockTexture(t, 0, 0, &locked))
        return NULL;
    uint8_t *p = (uint8_t *)locked.pixels;
    for (int i = 0; i < width / 4 * (height / 4) * 16; i++) {
        seed = seed * 1664525u + 1013904223u;
        p[i] = (uint8_t)(seed >> 24);
    }
    Gfx_UnlockTexture(t, 0, 0);
    return t;
}

// The texel of a BC3 texture's CPU copy, decoded.
uint32_t Texel(const GfxTexture *t, int x, int y)
{
    uint32_t out[16];
    Gfx_DecodeBlock(GFX_FORMAT_BC3, &t->levels[0][0][((size_t)(y / 4) * (t->width / 4) + x / 4) * 16], out);
    return out[(y % 4) * 4 + x % 4];
}

// The draws of the frame of the last present as text: each draw's state,
// kind, textures, counts, box and reach, and its vertices and indices (not
// their place in the frame's data), to compare two frames.
std::string FrameText()
{
    std::string text;
    char line[320];
    snprintf(line, sizeof(line), "states %zu snapshots %zu\n", s_frame.states.size(), s_frame.snapshots.size());
    text += line;
    for (const GfxCommand &c : s_frame.commands) {
        if (c.type != GFX_CMD_DRAW) {
            snprintf(line, sizeof(line), "command %d\n", (int)c.type);
            text += line;
            continue;
        }
        snprintf(line, sizeof(line), "draw s%u p%d t%p %p n%u i%u b%p v%u snap%d box%d %g %g %g %g %g %g r%g\n",
                 c.state, c.primitive, (void *)c.textures[0], (void *)c.textures[1], c.vertexCount, c.indexCount,
                 (void *)c.buffer, c.buffer != NULL ? c.firstVertexByte : 0u, c.snapshot ? 1 : 0, c.hasBox ? 1 : 0,
                 c.boxLow[0], c.boxLow[1], c.boxLow[2], c.boxHigh[0], c.boxHigh[1], c.boxHigh[2], c.reach);
        text += line;
        const size_t stride = c.transformed ? sizeof(GfxTLVertex) : sizeof(GfxVertex);
        if (c.buffer == NULL && c.firstVertexByte + c.vertexCount * stride <= s_frame.vertices.size())
            text.append((const char *)s_frame.vertices.data() + c.firstVertexByte, c.vertexCount * stride);
        if (c.firstIndex != ~0u && c.firstIndex + c.indexCount <= s_frame.indices.size())
            text.append((const char *)&s_frame.indices[c.firstIndex], c.indexCount * sizeof(uint16_t));
        text += "\n";
    }
    return text;
}

// A mesh that is not small: it keeps its world matrix.
void Mesh()
{
    GfxVertex mesh[18] = {};
    mesh[1].x = 1.0f;
    Gfx_DrawPrimitive(GFX_TRIANGLELIST, mesh, 18);
}

// Whether the vertex uniforms of two recorded states are the same: the rows
// that the GXM executor gives a vertex program (gxm_vertex_uniforms.h).
bool SameVertexRows(const GfxDrawState &a, const GfxDrawState &b)
{
    static float rowsA[VU_ROWS][4], rowsB[VU_ROWS][4];
    Gxm_FillVertexUniforms(a, a.viewport, rowsA);
    Gxm_FillVertexUniforms(b, b.viewport, rowsB);
    return memcmp(rowsA, rowsB, sizeof(rowsA)) == 0;
}

// Draws a mesh, changes the state, draws again. 1: the vertex uniforms of the
// two states differ and their numbers differ. 0: the uniforms and the numbers
// are the same, in two states. -1: something else.
int VersionAfter(const std::function<void()> &change)
{
    Mesh();
    change();
    Mesh();
    Gfx_Present(FALSE);
    if (Draws() != 2 || Draw(0).state == Draw(1).state)
        return -1;
    const GfxDrawState &a = s_frame.states[Draw(0).state], &b = s_frame.states[Draw(1).state];
    const bool same = SameVertexRows(a, b);
    if (same != (a.vertexVersion == b.vertexVersion))
        return -1;
    return same ? 0 : 1;
}

} // namespace

GfxExecutor *Gfx_CreateGpuExecutor() { return new CaptureExecutor; }

int main()
{
    REQUIRE(Gfx_SetVideoMode(640, 480, FALSE));

    // An executor that does not take joined draws gets each draw as the game
    // made it. A setter with the value that is already set starts no state.
    s_joins = false;
    Gfx_SetAlphaBlend(TRUE);
    Sprite(0.0f);
    Gfx_SetAlphaBlend(TRUE);
    Gfx_SetAlphaRef(0);
    Sprite(10.0f);
    Gfx_Present(FALSE);
    REQUIRE(Draws() == 2 && s_frame.states.size() == 1);
    REQUIRE(Draw(0).primitive == GFX_TRIANGLESTRIP && Draw(0).vertexCount == 4 && Draw(0).firstIndex == ~0u);
    REQUIRE(Draw(1).state == Draw(0).state);

    // Joined: three sprites of one state are one list of six triangles, in
    // the game's order, with the corners of each strip's triangles.
    s_joins = true;
    Sprite(0.0f);
    Gfx_SetAlphaBlend(TRUE);
    Sprite(10.0f);
    Sprite(20.0f);
    Gfx_Present(FALSE);
    REQUIRE(Draws() == 1 && s_frame.states.size() == 1);
    {
        const GfxCommand &c = Draw(0);
        REQUIRE(c.primitive == GFX_TRIANGLELIST && c.transformed && c.vertexCount == 12 && c.indexCount == 18);
        const uint16_t expected[18] = { 0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7, 8, 9, 10, 10, 9, 11 };
        REQUIRE(c.firstIndex + 18 <= s_frame.indices.size());
        REQUIRE(memcmp(&s_frame.indices[c.firstIndex], expected, sizeof(expected)) == 0);
        REQUIRE(SpriteX(c, 0) == 0.0f && SpriteX(c, 4) == 10.0f && SpriteX(c, 9) == 21.0f);
    }

    // A new state value, a clear and a render target each end a run.
    GfxTexture *target = Gfx_CreateTexture(64, 64, GFX_FORMAT_BGRA8, 1, GFX_TEXTURE_RENDER_TARGET);
    REQUIRE(target != NULL);
    Sprite(0.0f);
    Gfx_SetAlphaRef(8);
    Sprite(10.0f);
    Sprite(20.0f);
    Gfx_Clear(GFX_CLEAR_ZBUFFER, 0, 1.0f);
    Sprite(30.0f);
    Gfx_SetRenderTarget(target);
    Sprite(40.0f);
    Gfx_SetRenderTarget(NULL);
    Sprite(50.0f);
    Gfx_Present(FALSE);
    REQUIRE(Draws() == 5 && s_frame.states.size() == 2);
    REQUIRE(Draw(0).vertexCount == 4 && Draw(1).vertexCount == 8 && Draw(2).vertexCount == 4);
    REQUIRE(Draw(1).state != Draw(0).state && Draw(2).state == Draw(1).state);
    Gfx_SetAlphaRef(0);

    // The markers of the sky: in the back buffer only, with the settings of
    // the last present, and a run of sprites does not join across them.
    for (int half = 1; half >= 0; half--) {
        s_skyHalf = half;
        Gfx_Present(FALSE);
        Sprite(0.0f);
        Gfx_MarkSky(TRUE);
        Sprite(10.0f);
        Gfx_MarkSky(FALSE);
        Sprite(20.0f);
        Gfx_SetRenderTarget(target);
        Gfx_MarkSky(TRUE);
        Gfx_MarkSky(FALSE);
        Gfx_SetRenderTarget(NULL);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 3);
        const uint8_t flags = (uint8_t)((half ? GFX_SKY_HALF : 0) | GFX_SKY_STENCIL);
        unsigned markers = 0;
        bool begin = true, right = true;
        for (const GfxCommand &c : s_frame.commands) {
            if (c.type != GFX_CMD_SKY)
                continue;
            markers++;
            right = right && c.skyBegin == begin && c.skyFlags == flags;
            begin = false;
        }
        REQUIRE(markers == 2 && right);
    }
    s_skyHalf = 1;

    // Lines join without indices. A line and a sprite do not join.
    Line(0.0f);
    Line(5.0f);
    Line(9.0f);
    Sprite(0.0f);
    Line(1.0f);
    Gfx_Present(FALSE);
    REQUIRE(Draws() == 3);
    REQUIRE(Draw(0).primitive == GFX_LINELIST && Draw(0).vertexCount == 6 && Draw(0).firstIndex == ~0u);
    REQUIRE(Draw(1).primitive == GFX_TRIANGLELIST && Draw(1).vertexCount == 4);
    REQUIRE(Draw(2).primitive == GFX_LINELIST && Draw(2).vertexCount == 2);

    // A joined draw has at most 4096 vertices: its indices are 16 bits wide.
    for (int i = 0; i < 1025; i++)
        Sprite((float)i);
    Gfx_Present(FALSE);
    REQUIRE(Draws() == 2 && Draw(0).vertexCount == 4096 && Draw(1).vertexCount == 4);
    REQUIRE(s_frame.indices[Draw(0).firstIndex + Draw(0).indexCount - 1] == 4095);
    REQUIRE(s_frame.indices[Draw(1).firstIndex] == 0 && SpriteX(Draw(1), 0) == 1024.0f);

    // A 2D draw and a 3D draw do not join: their vertices are different.
    // Triangles and lines without indices do not join.
    {
        GfxVertex v[3] = {};
        GfxTLVertex t[6] = {};
        Gfx_DrawPrimitive(GFX_TRIANGLELIST, v, 3);
        Gfx_DrawPrimitive(GFX_TRIANGLELIST, v, 3);
        Gfx_DrawPrimitiveTL(GFX_TRIANGLELIST, t, 6);
        Line(0.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 3 && Draw(0).vertexCount == 6 && !Draw(0).transformed && Draw(1).transformed);
        REQUIRE(Draw(1).primitive == GFX_TRIANGLELIST && Draw(1).vertexCount == 6 && Draw(2).vertexCount == 2);
    }

    // Small 3D draws, each with a world matrix of its own: an executor that
    // does not take joined draws gets them as the game made them.
    s_joins = false;
    Billboard(2.0f, 10.0f, 5.0f);
    Billboard(3.0f, 20.0f, 6.0f);
    Gfx_Present(FALSE);
    REQUIRE(Draws() == 2 && s_frame.states.size() == 2);
    REQUIRE(s_frame.states[Draw(1).state].world._41 == 20.0f && Vertex3D(Draw(1), 1).x == 1.0f);
    REQUIRE(!Draw(0).hasBox && Draw(0).reach < 0.0f);
    // Joined: they are in world space, with one state and one draw. The
    // positions go through the whole matrix, the normals through its 3x3 part.
    s_joins = true;
    Billboard(2.0f, 10.0f, 5.0f);
    Billboard(3.0f, 20.0f, 6.0f);
    Gfx_Present(FALSE);
    REQUIRE(Draws() == 1 && s_frame.states.size() == 1);
    {
        const GfxCommand &c = Draw(0);
        REQUIRE(IsIdentity(s_frame.states[c.state].world));
        REQUIRE(!c.transformed && c.buffer == NULL && c.vertexCount == 8 && c.indexCount == 12);
        REQUIRE(s_frame.indices[c.firstIndex + 6] == 4 && s_frame.indices[c.firstIndex + 11] == 7);
        const GfxVertex a = Vertex3D(c, 3), b = Vertex3D(c, 5);
        REQUIRE(a.x == 12.0f && a.y == 2.0f && a.z == 5.0f && a.nz == -2.0f && a.nx == 0.0f);
        REQUIRE(b.x == 23.0f && b.y == 0.0f && b.z == 6.0f && b.nz == -3.0f);
        REQUIRE(a.diffuse == 0xff102033 && a.u0 == 1.0f);
        // The box of the run, and the eye distance of its farthest vertex
        // (the view is the identity here).
        REQUIRE(c.hasBox && c.boxLow[0] == 10.0f && c.boxHigh[0] == 23.0f && c.boxLow[1] == 0.0f);
        REQUIRE(c.boxHigh[1] == 3.0f && c.boxLow[2] == 5.0f && c.boxHigh[2] == 6.0f);
        REQUIRE(c.reach == sqrtf(23.0f * 23.0f + 3.0f * 3.0f + 6.0f * 6.0f));
    }
    // Another texture starts a new draw, in the same state: the draw has its
    // textures. A draw that is not small keeps its world matrix, and so does
    // the small draw after it: each has the state of its vertices.
    {
        GfxTexture *leaves = Gfx_CreateTexture(8, 8, GFX_FORMAT_BGRA8, 1, 0);
        Billboard(2.0f, 10.0f, 5.0f);
        Gfx_SetTexture(0, leaves);
        Billboard(3.0f, 20.0f, 6.0f);
        GfxVertex mesh[18] = {};
        mesh[1].x = 1.0f;
        Gfx_DrawPrimitive(GFX_TRIANGLELIST, mesh, 18);
        Billboard(3.0f, 20.0f, 6.0f);
        Billboard(4.0f, 30.0f, 7.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 4 && s_frame.states.size() == 3);
        REQUIRE(IsIdentity(s_frame.states[Draw(0).state].world) && Draw(1).state == Draw(0).state);
        REQUIRE(Draw(0).textures[0] == NULL && Draw(1).textures[0] == leaves && Draw(1).textures[1] == NULL);
        REQUIRE(Draw(2).textures[0] == leaves && Draw(3).textures[0] == leaves);
        REQUIRE(s_frame.states[Draw(2).state].world._41 == 20.0f && Draw(2).vertexCount == 18);
        REQUIRE(Vertex3D(Draw(2), 1).x == 1.0f);
        // A draw that is not small has the box of its own space, and no reach.
        REQUIRE(Draw(2).hasBox && Draw(2).reach < 0.0f && Draw(2).boxLow[0] == 0.0f && Draw(2).boxHigh[0] == 1.0f);
        REQUIRE(Draw(2).boxHigh[1] == 0.0f && Draw(3).reach > 0.0f);
        REQUIRE(IsIdentity(s_frame.states[Draw(3).state].world) && Draw(3).vertexCount == 8);
        REQUIRE(Vertex3D(Draw(3), 5).x == 34.0f);
        Gfx_SetTexture(0, NULL);
        Gfx_DestroyTexture(leaves);
    }
    // A world matrix that is not affine stays with its draw.
    {
        GfxMatrix world = Identity();
        world._34 = 0.5f;
        Gfx_SetTransform(GFX_TRANSFORM_WORLD, &world);
        GfxVertex v[3] = {};
        v[1].x = 1.0f;
        Gfx_DrawPrimitive(GFX_TRIANGLELIST, v, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && s_frame.states[Draw(0).state].world._34 == 0.5f && Vertex3D(Draw(0), 1).x == 1.0f);
        world = Identity();
        Gfx_SetTransform(GFX_TRANSFORM_WORLD, &world);
    }

    // The textures of a draw. An executor that does not take joined draws
    // gets a state for each new texture, as before. Each draw has its
    // textures in its command also.
    {
        GfxTexture *road = Gfx_CreateTexture(8, 8, GFX_FORMAT_BGRA8, 1, 0);
        GfxTexture *grass = Gfx_CreateTexture(8, 8, GFX_FORMAT_BGRA8, 1, 0);
        s_joins = false;
        Gfx_SetTexture(0, road);
        Mesh();
        Gfx_SetTexture(0, grass);
        Gfx_SetTexture(1, road);
        Mesh();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && s_frame.states.size() == 2);
        REQUIRE(s_frame.states[Draw(1).state].stages[0].texture == grass);
        REQUIRE(Draw(0).textures[0] == road && Draw(1).textures[0] == grass && Draw(1).textures[1] == road);
        // Joined: one state for the three meshes. A change of another part
        // still starts a state.
        s_joins = true;
        Gfx_SetTexture(1, NULL);
        Gfx_SetTexture(0, road);
        Mesh();
        Gfx_SetTexture(0, grass);
        Mesh();
        Gfx_SetTexture(1, road);
        Mesh();
        Gfx_SetAlphaRef(0x21);
        Mesh();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 4 && s_frame.states.size() == 2);
        REQUIRE(Draw(0).state == Draw(1).state && Draw(1).state == Draw(2).state && Draw(3).state != Draw(2).state);
        REQUIRE(Draw(0).textures[0] == road && Draw(1).textures[0] == grass && Draw(1).textures[1] == NULL);
        REQUIRE(Draw(2).textures[0] == grass && Draw(2).textures[1] == road);
        // Two sprites with different textures do not join.
        Gfx_SetTexture(1, NULL);
        Sprite(0.0f);
        Gfx_SetTexture(0, road);
        Sprite(10.0f);
        Sprite(20.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && s_frame.states.size() == 1 && Draw(1).vertexCount == 8);
        // A texture that only a draw's command names is still in use: the
        // frame runs before the texture goes.
        Mesh();
        Gfx_SetTexture(0, grass);
        Mesh();
        Gfx_SetTexture(0, NULL);
        s_frame.commands.clear();
        Gfx_DestroyTexture(grass);
        REQUIRE(Draws() == 2 && Draw(1).textures[0] == grass);
        Gfx_Present(FALSE);
        Gfx_DestroyTexture(road);
        Gfx_SetAlphaRef(0);
    }

    // The atlas of the small quads. Two trees with two clamped BC3 textures
    // are one draw from the atlas, in the game's order, with their texture
    // coordinates in the places of their textures.
    {
        GfxTexture *oak = TreeTexture(128, 128, 1), *pine = TreeTexture(64, 64, 2);
        REQUIRE(oak != NULL && pine != NULL);
        s_joins = true;
        Gfx_SetStageAddress(0, GFX_ADDRESS_CLAMP);
        Gfx_SetTexture(1, NULL);
        Gfx_SetTexture(0, oak);
        Billboard(2.0f, 10.0f, 5.0f);
        Gfx_SetTexture(0, pine);
        Billboard(3.0f, 20.0f, 6.0f);
        Gfx_SetTexture(0, oak);
        Billboard(4.0f, 30.0f, 7.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && s_frame.states.size() == 1 && Draw(0).vertexCount == 12 && Draw(0).indexCount == 18);
        GfxTexture *atlas = Draw(0).textures[0];
        REQUIRE(atlas != NULL && atlas != oak && atlas != pine && atlas->format == GFX_FORMAT_BC3);
        REQUIRE(atlas->width == 1024 && atlas->height == 1024 && atlas->mipLevels == 1 && atlas->dirty);
        // The oak is the first tile: its texels start one block in, after
        // its border. The pine is next to it, after both borders.
        const float size = 1024.0f;
        const GfxVertex a0 = Vertex3D(Draw(0), 0), a3 = Vertex3D(Draw(0), 3);
        REQUIRE(a0.u0 == 4.0f / size && a0.v0 == 4.0f / size);
        REQUIRE(a3.u0 == (4.0f + 128.0f) / size && a3.x == 12.0f);
        const GfxVertex p0 = Vertex3D(Draw(0), 4), p1 = Vertex3D(Draw(0), 5);
        REQUIRE(p0.u0 == (136.0f + 4.0f) / size && p1.u0 == (136.0f + 4.0f + 64.0f) / size && p0.v0 == 4.0f / size);
        const GfxVertex b0 = Vertex3D(Draw(0), 8);
        REQUIRE(b0.u0 == a0.u0 && b0.x == 30.0f);
        // The atlas has the textures' texels at those places, and the edge
        // texel again beside each edge.
        for (int y = 0; y < 128; y += 9)
            for (int x = 0; x < 128; x += 7)
                REQUIRE(Texel(atlas, 4 + x, 4 + y) == Texel(oak, x, y));
        for (int y = 0; y < 64; y += 5)
            for (int x = 0; x < 64; x += 3)
                REQUIRE(Texel(atlas, 140 + x, 4 + y) == Texel(pine, x, y));
        for (int i = 0; i < 128; i += 5) {
            REQUIRE(Texel(atlas, 3, 4 + i) == Texel(oak, 0, i) && Texel(atlas, 132, 4 + i) == Texel(oak, 127, i));
            REQUIRE(Texel(atlas, 4 + i, 3) == Texel(oak, i, 0) && Texel(atlas, 4 + i, 132) == Texel(oak, i, 127));
        }
        REQUIRE(Texel(atlas, 3, 3) == Texel(oak, 0, 0) && Texel(atlas, 139, 4) == Texel(pine, 0, 0));

        // A quad stays with its texture where the atlas would not give the
        // same pixels: a texture that repeats, coordinates outside the
        // texture, a second texture, a texture that is not BC3.
        GfxTexture *plain = Gfx_CreateTexture(64, 64, GFX_FORMAT_BGRA8, 1, 0);
        Gfx_SetTexture(0, oak);
        Gfx_SetStageAddress(0, GFX_ADDRESS_WRAP);
        Billboard(2.0f, 10.0f, 5.0f);
        Gfx_SetStageAddress(0, GFX_ADDRESS_CLAMP);
        Gfx_SetTexture(1, pine);
        Gfx_SetStageColorOp(1, GFX_TOP_MODULATE);
        Billboard(2.0f, 10.0f, 5.0f);
        Gfx_SetStageColorOp(1, GFX_TOP_DISABLE);
        Gfx_SetTexture(1, NULL);
        Gfx_SetTexture(0, plain);
        Billboard(2.0f, 10.0f, 5.0f);
        Gfx_SetTexture(0, oak);
        {
            GfxMatrix world = Identity();
            world._41 = 50.0f;
            Gfx_SetTransform(GFX_TRANSFORM_WORLD, &world);
            GfxVertex v[4] = {};
            v[1].x = 1.0f;
            v[1].u0 = 2.0f;
            v[2].y = 1.0f;
            const WORD quad[6] = { 0, 1, 2, 2, 1, 3 };
            Gfx_DrawIndexedPrimitive(GFX_TRIANGLELIST, v, 4, quad, 6);
        }
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 4);
        REQUIRE(Draw(0).textures[0] == oak && Draw(1).textures[0] == oak && Draw(1).textures[1] == pine);
        REQUIRE(Draw(2).textures[0] == plain && Draw(3).textures[0] == oak && Vertex3D(Draw(3), 1).u0 == 2.0f);

        // The distance fade: a run of quads joins on both sides of its
        // start, and has the eye distance of its farthest vertex. The
        // executor draws a run with the fade only where that reaches the
        // start.
        Gfx_SetDistanceFade(100.0f, 200.0f);
        Billboard(2.0f, 10.0f, 150.0f);
        Billboard(2.0f, 10.0f, 30.0f);
        Billboard(2.0f, 10.0f, 120.0f);
        Billboard(2.0f, 10.0f, 20.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).vertexCount == 16 && Draw(0).textures[0] == atlas);
        REQUIRE(Draw(0).reach == sqrtf(12.0f * 12.0f + 2.0f * 2.0f + 150.0f * 150.0f));
        REQUIRE(Draw(0).hasBox && Draw(0).boxLow[2] == 20.0f && Draw(0).boxHigh[2] == 150.0f);
        // A run of quads before the start reaches less far than the start.
        Billboard(2.0f, 10.0f, 60.0f);
        Billboard(2.0f, 10.0f, 80.0f);
        Billboard(2.0f, 10.0f, 30.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).vertexCount == 12);
        REQUIRE(Draw(0).reach == sqrtf(12.0f * 12.0f + 2.0f * 2.0f + 80.0f * 80.0f) && Draw(0).reach < 100.0f);
        // A draw that is not small, in world space, joins the run: the run
        // then has its box and no reach.
        {
            Gfx_SetTexture(0, NULL);
            Billboard(2.0f, 10.0f, 60.0f);
            GfxMatrix world = Identity();
            Gfx_SetTransform(GFX_TRANSFORM_WORLD, &world);
            GfxVertex mesh[18] = {};
            WORD list[18];
            for (int i = 0; i < 18; i++) {
                mesh[i].z = 300.0f;
                list[i] = (WORD)i;
            }
            Gfx_DrawIndexedPrimitive(GFX_TRIANGLELIST, mesh, 18, list, 18);
            Gfx_Present(FALSE);
            REQUIRE(Draws() == 1 && Draw(0).vertexCount == 22 && Draw(0).reach < 0.0f);
            REQUIRE(Draw(0).hasBox && Draw(0).boxLow[2] == 60.0f && Draw(0).boxHigh[2] == 300.0f);
            Gfx_SetTexture(0, oak);
        }
        Gfx_SetDistanceFade(0.0f, 0.0f);

        // A second stage that is off reads no texture: its texture does not
        // keep a quad out of the atlas.
        Gfx_SetTexture(1, pine);
        Billboard(2.0f, 10.0f, 5.0f);
        Billboard(3.0f, 20.0f, 6.0f);
        Gfx_SetTexture(1, NULL);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).vertexCount == 8 && Draw(0).textures[0] == atlas);

        // A texture that changes or goes: the atlas starts again, after the
        // draws that are recorded with it. The next draw has the new texels.
        Billboard(2.0f, 10.0f, 5.0f);
        s_frame.commands.clear();
        GfxLockedRect locked;
        REQUIRE(Gfx_LockTexture(pine, 0, 0, &locked));
        REQUIRE(Draws() == 1 && Draw(0).textures[0] == atlas);
        ((uint8_t *)locked.pixels)[0] ^= 0x55;
        ((uint8_t *)locked.pixels)[1] ^= 0xaa;
        Gfx_UnlockTexture(pine, 0, 0);
        Gfx_SetTexture(0, pine);
        Billboard(2.0f, 10.0f, 5.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).textures[0] == atlas && Vertex3D(Draw(0), 0).u0 == 4.0f / size);
        REQUIRE(Texel(atlas, 4, 4) == Texel(pine, 0, 0) && Texel(atlas, 10, 9) == Texel(pine, 6, 5));
        Gfx_SetTexture(0, NULL);
        Gfx_DestroyTexture(pine);
        Gfx_SetTexture(0, oak);
        Billboard(2.0f, 10.0f, 5.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).textures[0] == atlas && Vertex3D(Draw(0), 0).u0 == 4.0f / size);
        REQUIRE(Texel(atlas, 4, 4) == Texel(oak, 0, 0));
        // The atlas has room for nine textures of 256 x 256 with their
        // borders, beside the oak. The tenth finds no room and stays with
        // its texture for that frame. The next frame starts a larger atlas,
        // with room for all of them.
        {
            GfxTexture *trees[10];
            for (int i = 0; i < 10; i++) {
                trees[i] = TreeTexture(256, 256, 10 + (unsigned)i);
                REQUIRE(trees[i] != NULL);
            }
            const auto forest = [&] {
                for (int i = 0; i < 10; i++) {
                    Gfx_SetTexture(0, trees[i]);
                    Billboard(2.0f, 10.0f + (float)i, 5.0f);
                }
            };
            forest();
            Gfx_Present(FALSE);
            REQUIRE(Draws() == 2 && Draw(0).vertexCount == 36 && Draw(1).vertexCount == 4);
            REQUIRE(Draw(1).textures[0] == trees[9] && Vertex3D(Draw(1), 1).u0 == 1.0f);
            forest();
            Gfx_SetTexture(0, oak);
            Billboard(2.0f, 30.0f, 5.0f);
            Gfx_Present(FALSE);
            REQUIRE(Draws() == 1 && Draw(0).vertexCount == 44);
            const GfxTexture *large = Draw(0).textures[0];
            REQUIRE(large != NULL && large->width == 2048 && large->height == 2048 && large->format == GFX_FORMAT_BC3);
            // Seven of them in a row of the large atlas: the tenth is the
            // third of the second row.
            const float largeSize = 2048.0f;
            REQUIRE(Vertex3D(Draw(0), 0).u0 == 4.0f / largeSize && Vertex3D(Draw(0), 1).u0 == 260.0f / largeSize);
            REQUIRE(Vertex3D(Draw(0), 36).u0 == (528.0f + 4.0f) / largeSize);
            REQUIRE(Vertex3D(Draw(0), 37).u0 == (528.0f + 260.0f) / largeSize);
            for (int y = 0; y < 256; y += 17)
                for (int x = 0; x < 256; x += 13) {
                    REQUIRE(Texel(large, 4 + x, 4 + y) == Texel(trees[0], x, y));
                    REQUIRE(Texel(large, 532 + x, 268 + y) == Texel(trees[9], x, y));
                }
            // The large atlas stays: the next frame draws from it.
            forest();
            Gfx_Present(FALSE);
            REQUIRE(Draws() == 1 && Draw(0).vertexCount == 40 && Draw(0).textures[0] == large);
            // The textures of the next stage have the room of the large
            // atlas also.
            Gfx_SetTexture(0, NULL);
            for (int i = 0; i < 10; i++) {
                Gfx_DestroyTexture(trees[i]);
                trees[i] = TreeTexture(256, 256, 30 + (unsigned)i);
                REQUIRE(trees[i] != NULL);
            }
            forest();
            Gfx_Present(FALSE);
            REQUIRE(Draws() == 1 && Draw(0).vertexCount == 40 && Draw(0).textures[0] == large);
            Gfx_SetTexture(0, NULL);
            for (GfxTexture *tree : trees)
                Gfx_DestroyTexture(tree);
            Gfx_SetTexture(0, oak);
        }
        // An executor that does not take joined draws has no atlas.
        s_joins = false;
        Billboard(2.0f, 10.0f, 5.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).textures[0] == oak && Vertex3D(Draw(0), 1).u0 == 1.0f);
        Gfx_SetTexture(0, NULL);
        Gfx_DestroyTexture(oak);
        Gfx_DestroyTexture(plain);
        Gfx_SetStageAddress(0, GFX_ADDRESS_WRAP);
        GfxMatrix world = Identity();
        Gfx_SetTransform(GFX_TRANSFORM_WORLD, &world);
    }

    // The number of a recorded state for the vertex program: it changes with
    // each part that the vertex uniforms come from, and with no other part.
    // The GXM executor does not look at a state whose number it has.
    {
        s_joins = false;
        Gfx_SetLighting(TRUE);
        Gfx_SetFog(TRUE);
        GfxMatrix m = Identity();
        m._42 = 3.0f;
        GfxLight light{};
        light.type = 3;
        light.diffuse.r = 0.5f;
        light.direction.z = 1.0f;
        GfxMaterial material{};
        material.diffuse.g = 0.25f;
        material.power = 4.0f;
        GfxViewport viewport{};
        viewport.width = 320;
        viewport.height = 240;
        viewport.maxZ = 1.0f;
        GfxTexture *road = Gfx_CreateTexture(8, 8, GFX_FORMAT_BGRA8, 1, 0);
        const std::function<void()> vertexParts[] = {
            [&] { Gfx_SetTransform(GFX_TRANSFORM_WORLD, &m); },
            [&] { m._41 = 2.0f; Gfx_SetTransform(GFX_TRANSFORM_VIEW, &m); },
            [&] { m._43 = 5.0f; Gfx_SetTransform(GFX_TRANSFORM_PROJECTION, &m); },
            [&] { Gfx_SetTransform(GFX_TRANSFORM_TEXTURE0, &m); },
            [&] { Gfx_SetTransform(GFX_TRANSFORM_TEXTURE1, &m); },
            [&] { Gfx_EnableLight(1, TRUE); },
            [&] { Gfx_SetLight(1, &light); },
            [&] { Gfx_SetMaterial(&material); },
            [] { Gfx_SetAmbient(0xff203040); },
            [] { Gfx_SetSpecular(TRUE); },
            [] { Gfx_SetLocalViewer(FALSE); },
            [] { Gfx_SetNormalizeNormals(TRUE); },
            [] { Gfx_SetColorVertex(FALSE); },
            [] { Gfx_SetDiffuseMaterialSource(GFX_MCS_MATERIAL); },
            [] { Gfx_SetSpecularMaterialSource(GFX_MCS_MATERIAL); },
            [] { Gfx_SetAmbientMaterialSource(GFX_MCS_COLOR1); },
            [] { Gfx_SetEmissiveMaterialSource(GFX_MCS_COLOR1); },
            [] { Gfx_SetFogVertexMode(GFX_FOG_LINEAR); },
            [] { Gfx_SetFog(FALSE); },
            [] { Gfx_SetFog(TRUE); },
            [] { Gfx_SetFogTableMode(GFX_FOG_LINEAR); },
            [] { Gfx_SetFogStart(10.0f); },
            [] { Gfx_SetFogEnd(90.0f); },
            [] { Gfx_SetStageTexCoordIndex(0, 1); },
            [] { Gfx_SetStageTextureTransform(1, GFX_TTFF_DISABLE + 2); },
            [&] { Gfx_SetViewport(&viewport); },
            [] { Gfx_SetLighting(FALSE); },
        };
        int index = 0;
        for (const auto &change : vertexParts) {
            if (VersionAfter(change) != 1) {
                fprintf(stderr, "vertex part %d\n", index);
                REQUIRE(false);
            }
            index++;
        }
        const std::function<void()> pixelParts[] = {
            [&] { Gfx_SetTexture(0, road); },
            [&] { Gfx_SetTexture(1, road); },
            [] { Gfx_SetStageColorOp(1, GFX_TOP_MODULATE); },
            [] { Gfx_SetStageColorArg1(0, GFX_TA_DIFFUSE); },
            [] { Gfx_SetStageColorArg2(0, GFX_TA_TEXTURE); },
            [] { Gfx_SetStageAlphaOp(1, GFX_TOP_MODULATE); },
            [] { Gfx_SetStageAlphaArg1(0, GFX_TA_DIFFUSE); },
            [] { Gfx_SetStageAlphaArg2(0, GFX_TA_TEXTURE); },
            [] { Gfx_SetStageAddress(0, GFX_ADDRESS_CLAMP); },
            [] { Gfx_SetStageMinFilter(0, GFX_FILTER_LINEAR); },
            [] { Gfx_SetStageMagFilter(0, GFX_FILTER_LINEAR); },
            [] { Gfx_SetStageMipFilter(0, GFX_MIPFILTER_LINEAR); },
            [] { Gfx_SetStageMaxMipLevel(0, 2); },
            [] { Gfx_SetStageBumpEnvLScale(0, 0.5f); },
            [] { Gfx_SetStageBumpEnvLOffset(0, 0.25f); },
            [] { Gfx_SetStageBumpEnvMat(0, 1, 0.75f); },
            [] { Gfx_SetAlphaBlend(FALSE); },
            [] { Gfx_SetSrcBlend(GFX_BLEND_SRCALPHA); },
            [] { Gfx_SetDestBlend(GFX_BLEND_INVSRCALPHA); },
            [] { Gfx_SetAlphaTest(TRUE); },
            [] { Gfx_SetAlphaFunc(GFX_CMP_GREATEREQUAL); },
            [] { Gfx_SetAlphaRef(0x55); },
            [] { Gfx_SetTextureFactor(0x80808080); },
            [] { Gfx_SetDepthTest(FALSE); },
            [] { Gfx_SetDepthWrite(FALSE); },
            [] { Gfx_SetDepthFunc(GFX_CMP_ALWAYS); },
            [] { Gfx_SetCullMode(GFX_CULL_NONE); },
            [] { Gfx_SetFogColor(0xff112233); },
            [] { Gfx_SetDistanceFade(100.0f, 200.0f); },
        };
        index = 0;
        for (const auto &change : pixelParts) {
            if (VersionAfter(change) != 0) {
                fprintf(stderr, "pixel part %d\n", index);
                REQUIRE(false);
            }
            index++;
        }
        // The number stays over a present, and over a state that comes back.
        Mesh();
        Gfx_Present(FALSE);
        const uint32_t version = s_frame.states[Draw(0).state].vertexVersion;
        Mesh();
        Gfx_SetAlphaRef(0x56);
        Mesh();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && s_frame.states.size() == 2);
        REQUIRE(s_frame.states[0].vertexVersion == version && s_frame.states[1].vertexVersion == version);
        // A small draw in world space, then a mesh: the world matrix of the
        // recorded states differs, and so do their numbers.
        s_joins = true;
        Billboard(2.0f, 10.0f, 5.0f);
        Mesh();
        Billboard(3.0f, 20.0f, 6.0f);
        Billboard(4.0f, 30.0f, 7.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 3 && s_frame.states.size() == 3);
        REQUIRE(s_frame.states[0].vertexVersion != s_frame.states[1].vertexVersion);
        REQUIRE(s_frame.states[1].vertexVersion != s_frame.states[2].vertexVersion);
        REQUIRE(!SameVertexRows(s_frame.states[0], s_frame.states[1]));
        // Back to the state the tests after this start from.
        Gfx_SetTexture(0, NULL);
        Gfx_SetTexture(1, NULL);
        Gfx_DestroyTexture(road);
        m = Identity();
        Gfx_SetTransform(GFX_TRANSFORM_WORLD, &m);
        Gfx_SetAlphaRef(0);
    }

    // A vertex buffer the game has just locked is not drawn by reference.
    // For an executor that takes joined draws the frame has a snapshot of
    // the part of it that the draws use: the executor copies it once. The
    // draws refer to the buffer, each with the box of its range.
    s_joins = true;
    const WORD triangle[3] = { 0, 1, 2 };
    {
        GfxVertexBuffer *mesh = Gfx_CreateVertexBuffer(16);
        GfxVertex *v = Gfx_LockVertexBuffer(mesh);
        for (int i = 0; i < 16; i++)
            v[i].x = (float)i;
        Gfx_UnlockVertexBuffer(mesh);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, mesh, 2, 12, triangle, 3);
        Gfx_SetAlphaRef(4);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, mesh, 6, 8, triangle, 3);
        Gfx_SetAlphaRef(6);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, mesh, 0, 4, triangle, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 3 && s_frame.vertices.empty() && s_frame.buffers.empty());
        REQUIRE(s_frame.snapshots.size() == 1 && s_frame.snapshots[0].buffer == mesh);
        REQUIRE(s_frame.snapshots[0].first == 0 && s_frame.snapshots[0].end == 14);
        for (unsigned i = 0; i < 3; i++)
            REQUIRE(Draw(i).snapshot && Draw(i).buffer == mesh && Draw(i).hasBox && Draw(i).reach < 0.0f);
        REQUIRE(Draw(0).firstVertexByte == 2 * sizeof(GfxVertex) && Draw(1).firstVertexByte == 6 * sizeof(GfxVertex));
        REQUIRE(Draw(0).boxLow[0] == 2.0f && Draw(0).boxHigh[0] == 13.0f);
        REQUIRE(Draw(1).boxLow[0] == 6.0f && Draw(1).boxHigh[0] == 13.0f);
        REQUIRE(Draw(2).boxLow[0] == 0.0f && Draw(2).boxHigh[0] == 3.0f);
        // A lock after a draw of the frame runs the draws first: the
        // executor copies the snapshot before the game changes the buffer.
        // The buffer is streaming from then on: each draw copies its range.
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, mesh, 0, 4, triangle, 3);
        v = Gfx_LockVertexBuffer(mesh);
        REQUIRE(Draws() == 1 && Draw(0).snapshot && s_frame.snapshots.size() == 1 && mesh->streaming);
        v[0].x = 50.0f;
        Gfx_UnlockVertexBuffer(mesh);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, mesh, 0, 4, triangle, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && !Draw(0).snapshot && Draw(0).buffer == NULL && s_frame.snapshots.empty());
        REQUIRE(Vertex3D(Draw(0), 0).x == 50.0f && Draw(0).boxHigh[0] == 50.0f);
        Gfx_DestroyVertexBuffer(mesh);
        // An executor that does not take joined draws gets no snapshot.
        s_joins = false;
        mesh = Gfx_CreateVertexBuffer(16);
        Gfx_LockVertexBuffer(mesh);
        Gfx_UnlockVertexBuffer(mesh);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, mesh, 0, 4, triangle, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && !Draw(0).snapshot && Draw(0).buffer == NULL && s_frame.snapshots.empty());
        Gfx_DestroyVertexBuffer(mesh);
        s_joins = true;
    }

    // The blocks of a vertex buffer that a range lock touched
    // (gfx_range_locks.h): recent for two frames, then a patch, once.
    {
        GfxRangeLocks locks;
        std::vector<std::pair<uint32_t, uint32_t>> patches;
        const auto take = [&](uint32_t frame) {
            patches.clear();
            locks.TakeRested(frame, 200, [&](uint32_t first, uint32_t end) { patches.push_back({ first, end }); });
        };
        REQUIRE(!locks.Any() && !locks.Recent(0, 200, 5));
        locks.Lock(40, 30, 5);      // vertices 40 to 69: blocks 1 and 2
        locks.Lock(190, 10, 5);     // blocks 5 and 6; the buffer ends in block 6
        REQUIRE(locks.Any());
        REQUIRE(!locks.Recent(0, 32, 5) && locks.Recent(0, 33, 5) && locks.Recent(95, 1, 5) && !locks.Recent(96, 64, 5));
        REQUIRE(locks.Recent(60, 4, 6) && !locks.Recent(60, 4, 7));
        take(6);
        REQUIRE(patches.empty() && locks.Any());
        locks.Lock(64, 1, 6);       // block 2 again, a frame later
        take(7);
        REQUIRE(patches.size() == 2 && patches[0] == std::make_pair(32u, 64u) && patches[1] == std::make_pair(160u, 200u));
        REQUIRE(locks.Recent(64, 1, 7) && locks.Any());
        take(8);
        REQUIRE(patches.size() == 1 && patches[0] == std::make_pair(64u, 96u) && !locks.Any());
        take(9);
        REQUIRE(patches.empty());
        // The ranges that touch a patch.
        const GfxPatch some[2] = { { NULL, 32, 64 }, { NULL, 192, 200 } };
        REQUIRE(!Gfx_TouchesPatch(some, 2, 0, 32) && Gfx_TouchesPatch(some, 2, 0, 33) && Gfx_TouchesPatch(some, 2, 63, 1));
        REQUIRE(!Gfx_TouchesPatch(some, 2, 64, 128) && Gfx_TouchesPatch(some, 2, 64, 129) &&
                Gfx_TouchesPatch(some, 2, 199, 50) && !Gfx_TouchesPatch(some, 2, 200, 50) &&
                !Gfx_TouchesPatch(some, 0, 0, 500));
    }

    // The game changes one mesh of a vertex buffer of many
    // (Gfx_LockVertexBufferRange): for two frames the draws of that mesh
    // take its vertices from the frame's snapshot of them, and the draws of
    // the other meshes refer to the buffer. Then the frame has a patch of
    // the blocks of the mesh, and its draws refer to the buffer again. The
    // buffer's version stays.
    {
        s_cullDraws = 1;
        Gfx_Present(FALSE);
        GfxMatrix view, projection;
        Gfx_GetTransform(GFX_TRANSFORM_VIEW, &view);
        Gfx_GetTransform(GFX_TRANSFORM_PROJECTION, &projection);
        const GfxMatrix identity = Identity();
        Gfx_SetTransform(GFX_TRANSFORM_VIEW, &identity);
        Gfx_SetTransform(GFX_TRANSFORM_PROJECTION, &identity);
        GfxVertexBuffer *shared = Gfx_CreateVertexBuffer(200);
        GfxVertex *v = Gfx_LockVertexBuffer(shared);
        for (int i = 0; i < 200; i++) {
            v[i].x = (float)i * 0.001f;
            v[i].z = 0.5f;
        }
        Gfx_UnlockVertexBuffer(shared);
        for (int i = 0; i < 4; i++)
            Gfx_Present(FALSE);
        const uint32_t version = shared->version;
        const auto drawBoth = [&] {
            Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, shared, 0, 40, triangle, 3);      // mesh A
            Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, shared, 100, 40, triangle, 3);    // mesh B
        };
        drawBoth();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && Draw(0).buffer == shared && Draw(1).buffer == shared && s_frame.patches.empty());
        REQUIRE(Draw(0).hasBox && Draw(0).boxHigh[0] == 39.0f * 0.001f);
        // The colours of mesh A change.
        v = Gfx_LockVertexBufferRange(shared, 0, 40);
        REQUIRE(v == shared->vertices.data());
        v[5].diffuse = 7;
        Gfx_UnlockVertexBuffer(shared);
        for (int frame = 0; frame < 2; frame++) {
            drawBoth();
            Gfx_Present(FALSE);
            REQUIRE(Draws() == 2 && Draw(0).snapshot && Draw(0).buffer == shared);
            REQUIRE(!Draw(1).snapshot && Draw(1).buffer == shared && s_frame.vertices.empty());
            REQUIRE(s_frame.snapshots.size() == 1 && s_frame.snapshots[0].buffer == shared);
            REQUIRE(s_frame.snapshots[0].first == 0 && s_frame.snapshots[0].end == 40);
            REQUIRE(shared->vertices[5].diffuse == 7 && s_frame.patches.empty());
            REQUIRE(s_frame.buffers.size() == 1 && shared->version == version);
        }
        drawBoth();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && !Draw(0).snapshot && Draw(0).buffer == shared && Draw(1).buffer == shared);
        REQUIRE(s_frame.snapshots.empty() && s_frame.vertices.empty());
        REQUIRE(s_frame.patches.size() == 1 && s_frame.patches[0].buffer == shared);
        REQUIRE(s_frame.patches[0].first == 0 && s_frame.patches[0].end == 64);
        drawBoth();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && Draw(0).buffer == shared && s_frame.patches.empty() && shared->version == version);

        // A position of mesh A changes: the box of its draws is that of the
        // new positions (a box is measured again after a lock of its
        // vertices). The box of mesh B stays.
        v = Gfx_LockVertexBufferRange(shared, 0, 40);
        v[3].x = 0.9f;
        Gfx_UnlockVertexBuffer(shared);
        drawBoth();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && Draw(0).snapshot && Draw(0).hasBox && Draw(0).boxHigh[0] == 0.9f);
        REQUIRE(Draw(1).hasBox && Draw(1).boxHigh[0] == 139.0f * 0.001f);

        // The game changes the mesh after a draw of the frame that has its
        // patch: the draws so far run first, with the patch of what the
        // buffer had. The draws after the lock copy the new vertices.
        Gfx_Present(FALSE);
        drawBoth();
        v = Gfx_LockVertexBufferRange(shared, 10, 4);
        REQUIRE(Draws() == 2 && Draw(0).buffer == shared && s_frame.patches.size() == 1);
        v[10].diffuse = 9;
        Gfx_UnlockVertexBuffer(shared);
        drawBoth();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && Draw(0).snapshot && shared->vertices[10].diffuse == 9 && s_frame.patches.empty());
        // A change of a mesh with no patch and no snapshot in the frame runs
        // no draw: the draws so far keep the GPU copy, which does not
        // change yet.
        drawBoth();
        v = Gfx_LockVertexBufferRange(shared, 100, 40);
        Gfx_UnlockVertexBuffer(shared);
        drawBoth();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 4 && Draw(0).snapshot && !Draw(1).snapshot && Draw(1).buffer == shared && Draw(3).snapshot);
        REQUIRE(s_frame.snapshots.size() == 1 && s_frame.snapshots[0].first == 0 && s_frame.snapshots[0].end == 140);
        // A change of vertices that the frame has a snapshot of: the draws
        // so far run first, with the snapshot of what the buffer had.
        drawBoth();
        s_frame = GfxFrame();
        v = Gfx_LockVertexBufferRange(shared, 120, 4);
        REQUIRE(Draws() == 2 && Draw(1).snapshot && s_frame.snapshots.size() == 1);
        Gfx_UnlockVertexBuffer(shared);
        Gfx_Present(FALSE);

        // The game gives the vertices of a mesh to the renderer
        // (Gfx_WriteVertexBufferRange). Vertices that the buffer has already
        // change nothing: the draws refer to the buffer.
        for (int i = 0; i < 3; i++) {
            drawBoth();
            Gfx_Present(FALSE);
        }
        REQUIRE(Draws() == 2 && !Draw(0).snapshot && !Draw(1).snapshot);
        std::vector<GfxVertex> meshA(shared->vertices.begin(), shared->vertices.begin() + 40);
        const uint32_t serial = shared->lockSerial;
        Gfx_WriteVertexBufferRange(shared, 0, meshA.data(), 40);
        drawBoth();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && !Draw(0).snapshot && s_frame.snapshots.empty() && shared->lockSerial == serial);
        // New colours: the draws of the mesh take the snapshot, and its box
        // stays (no range lock number: the positions are the same).
        meshA[7].diffuse = 0x11223344;
        Gfx_WriteVertexBufferRange(shared, 0, meshA.data(), 40);
        REQUIRE(shared->vertices[7].diffuse == 0x11223344 && shared->lockSerial == serial && shared->version == version);
        drawBoth();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && Draw(0).snapshot && !Draw(1).snapshot && Draw(0).boxHigh[0] == 0.9f);
        // A new position: the box is measured again.
        meshA[3].x = 0.7f;
        Gfx_WriteVertexBufferRange(shared, 0, meshA.data(), 40);
        REQUIRE(shared->lockSerial == serial + 1);
        drawBoth();
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && Draw(0).snapshot && Draw(0).boxHigh[0] == 0.7f);

        // A buffer that was locked whole a short time ago has no parts, and
        // an executor that takes no joined draws has none: a lock of all.
        Gfx_LockVertexBuffer(shared);
        Gfx_UnlockVertexBuffer(shared);
        const uint32_t whole = shared->version;
        Gfx_LockVertexBufferRange(shared, 0, 40);
        Gfx_UnlockVertexBuffer(shared);
        REQUIRE(shared->version == whole + 1);
        for (int i = 0; i < 4; i++)
            Gfx_Present(FALSE);
        s_joins = false;
        Gfx_LockVertexBufferRange(shared, 0, 40);
        Gfx_UnlockVertexBuffer(shared);
        REQUIRE(shared->version == whole + 2);
        meshA[9].diffuse = 5;
        Gfx_WriteVertexBufferRange(shared, 0, meshA.data(), 40);
        REQUIRE(shared->version == whole + 3 && shared->vertices[9].diffuse == 5);
        s_joins = true;
        Gfx_DestroyVertexBuffer(shared);
        Gfx_SetTransform(GFX_TRANSFORM_VIEW, &view);
        Gfx_SetTransform(GFX_TRANSFORM_PROJECTION, &projection);
        s_cullDraws = 0;
        Gfx_Present(FALSE);
    }

    // Each draw has the part of the scene that recorded it (GfxCommand::part,
    // for the probe): a draw alone, a draw whose vertices come in parts, and
    // the draws of a batch.
    {
        struct PartObserver : Diagnostics::Observer {
            int part = Diagnostics::SCENE_PART_NONE;
            int Part() const override { return part; }
        } parts;
        s_joins = true;
        Gfx_Present(FALSE);
        Diagnostics::observer = &parts;
        parts.part = Diagnostics::SCENE_PART_LAYERS;
        Sprite(1.0f);
        parts.part = Diagnostics::SCENE_PART_QUADS;
        GfxVertex tri[3] = {};
        tri[1].x = 1.0f;
        tri[2].y = 1.0f;
        Gfx_BeginPrimitive(GFX_TRIANGLELIST);
        Gfx_AddPrimitiveVertices(tri, 3);
        Gfx_EndPrimitive();
        parts.part = Diagnostics::SCENE_PART_BILLBOARDS;
        GfxVertexBuffer *quadBuffer = Gfx_CreateVertexBuffer(4);
        GfxVertex *q = Gfx_LockVertexBuffer(quadBuffer);
        for (int i = 0; i < 4; i++) {
            q[i].x = (float)(i & 1);
            q[i].y = (float)(i >> 1);
        }
        Gfx_UnlockVertexBuffer(quadBuffer);
        const WORD quadIndices[6] = { 0, 1, 2, 2, 1, 3 };
        GfxMatrix world = Identity();
        world._41 = 0.5f;
        const GfxBatchDraw one = { &world, NULL, quadBuffer, 0, 4, quadIndices, 6 };
        Gfx_DrawBatch(&one, 1);
        parts.part = Diagnostics::SCENE_PART_NONE;
        Gfx_Present(FALSE);
        Diagnostics::observer = nullptr;
        REQUIRE(Draws() == 3);
        REQUIRE(Draw(0).part == Diagnostics::SCENE_PART_LAYERS && Draw(1).part == Diagnostics::SCENE_PART_QUADS);
        REQUIRE(Draw(2).part == Diagnostics::SCENE_PART_BILLBOARDS);
        const GfxMatrix identity = Identity();
        Gfx_SetTransform(GFX_TRANSFORM_WORLD, &identity);
        Gfx_DestroyVertexBuffer(quadBuffer);
        Gfx_Present(FALSE);
    }

    // A draw whose vertices come in parts (Gfx_BeginPrimitive) is the draw
    // of the same vertices from a vertex buffer that the game locks between
    // its draws: the same commands, states and vertices, with and without
    // joined draws and the view test. A draw outside the view leaves no
    // vertex in the frame, and no vertex makes no draw.
    {
        s_joins = true;
        GfxVertexBuffer *stream = Gfx_CreateVertexBuffer(12);
        Gfx_LockVertexBuffer(stream);
        Gfx_UnlockVertexBuffer(stream);
        Gfx_DrawPrimitiveVB(GFX_TRIANGLELIST, stream, 0, 3);
        Gfx_LockVertexBuffer(stream);
        Gfx_UnlockVertexBuffer(stream);
        REQUIRE(stream->streaming);
        Gfx_Present(FALSE);
        GfxMatrix view, projection;
        Gfx_GetTransform(GFX_TRANSFORM_VIEW, &view);
        Gfx_GetTransform(GFX_TRANSFORM_PROJECTION, &projection);
        const GfxMatrix identity = Identity();
        Gfx_SetTransform(GFX_TRANSFORM_VIEW, &identity);
        Gfx_SetTransform(GFX_TRANSFORM_PROJECTION, &identity);
        GfxVertex quads[18] = {};
        for (int i = 0; i < 18; i++) {
            quads[i].x = (i >= 12 ? 100.0f : 0.0f) + 0.01f * (float)i;
            quads[i].y = (float)(i & 1) * 0.25f;
            quads[i].z = 0.5f;
            quads[i].diffuse = 0xff000000u + (DWORD)i;
        }
        const auto fromBuffer = [&](int first, int count) {
            GfxVertex *v = Gfx_LockVertexBuffer(stream);
            memcpy(v, quads + first, (size_t)count * sizeof(GfxVertex));
            Gfx_UnlockVertexBuffer(stream);
            Gfx_DrawPrimitiveVB(GFX_TRIANGLELIST, stream, 0, (DWORD)count);
        };
        for (int mode = 0; mode < 4; mode++) {
            s_joins = (mode & 1) != 0;
            s_cullDraws = mode >> 1;
            Gfx_Present(FALSE);
            // A sprite first: its 2D vertices end at no multiple of the size
            // of a 3D vertex.
            Sprite(1.0f);
            fromBuffer(0, 6);
            Gfx_SetAlphaRef(4);
            fromBuffer(6, 6);
            fromBuffer(12, 6);      // outside the view
            Gfx_SetAlphaRef(0);
            Gfx_Present(FALSE);
            const std::string made = FrameText();
            const size_t madeBytes = s_frame.vertices.size();
            Sprite(1.0f);
            Gfx_BeginPrimitive(GFX_TRIANGLELIST);
            Gfx_AddPrimitiveVertices(quads, 3);
            Gfx_AddPrimitiveVertices(quads + 3, 3);
            Gfx_EndPrimitive();
            Gfx_SetAlphaRef(4);
            Gfx_BeginPrimitive(GFX_TRIANGLELIST);
            Gfx_AddPrimitiveVertices(quads + 6, 6);
            Gfx_EndPrimitive();
            Gfx_BeginPrimitive(GFX_TRIANGLELIST);
            Gfx_AddPrimitiveVertices(quads + 12, 6);
            Gfx_EndPrimitive();
            Gfx_BeginPrimitive(GFX_TRIANGLELIST);
            Gfx_EndPrimitive();
            Gfx_SetAlphaRef(0);
            Gfx_Present(FALSE);
            REQUIRE(FrameText() == made && s_frame.vertices.size() == madeBytes);
            REQUIRE(Draws() == (s_cullDraws ? 3u : 4u) && Draw(2).vertexCount == 6);
            REQUIRE(Vertex3D(Draw(1), 5).diffuse == 0xff000005u && Vertex3D(Draw(2), 0).diffuse == 0xff000006u);
            REQUIRE(Draw(1).firstVertexByte % sizeof(GfxVertex) == 0 && Draw(1).firstVertexByte != 0);
        }
        s_joins = true;
        s_cullDraws = 0;
        Gfx_SetTransform(GFX_TRANSFORM_VIEW, &view);
        Gfx_SetTransform(GFX_TRANSFORM_PROJECTION, &projection);
        Gfx_Present(FALSE);
        Gfx_DestroyVertexBuffer(stream);
    }

    // A streaming vertex buffer (the game locks it between its draws): its
    // vertices are copied into the frame. The parts of a mesh, each drawn
    // with a range inside the range of the part before, share one copy.
    GfxVertexBuffer *buffer = Gfx_CreateVertexBuffer(16);
    GfxVertex *locked = Gfx_LockVertexBuffer(buffer);
    Gfx_UnlockVertexBuffer(buffer);
    Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, buffer, 0, 4, triangle, 3);
    locked = Gfx_LockVertexBuffer(buffer);
    REQUIRE(buffer->streaming);
    Gfx_Present(FALSE);
    for (int i = 0; i < 16; i++)
        locked[i].x = (float)i;
    Gfx_UnlockVertexBuffer(buffer);
    Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, buffer, 2, 12, triangle, 3);
    Gfx_SetAlphaRef(4);
    Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, buffer, 6, 8, triangle, 3);
    Gfx_SetAlphaRef(6);
    Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, buffer, 2, 12, triangle, 3);
    // A range that starts before the copy gets a copy of its own.
    Gfx_SetAlphaRef(8);
    Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, buffer, 0, 4, triangle, 3);
    Gfx_Present(FALSE);
    REQUIRE(Draws() == 4);
    REQUIRE(s_frame.vertices.size() == (12 + 4) * sizeof(GfxVertex));
    REQUIRE(Draw(0).buffer == NULL && Draw(0).firstVertexByte == 0);
    REQUIRE(Draw(1).firstVertexByte == 4 * sizeof(GfxVertex) && Draw(1).vertexCount == 8);
    REQUIRE(Draw(2).firstVertexByte == 0);
    REQUIRE(Draw(3).firstVertexByte == 12 * sizeof(GfxVertex));
    // Each draw has the box of its copy: the box of the parts that share a
    // copy is that of the whole copy.
    REQUIRE(Draw(0).hasBox && Draw(0).boxLow[0] == 2.0f && Draw(0).boxHigh[0] == 13.0f && Draw(0).reach < 0.0f);
    REQUIRE(Draw(1).hasBox && Draw(1).boxLow[0] == 2.0f && Draw(1).boxHigh[0] == 13.0f);
    REQUIRE(Draw(2).hasBox && Draw(2).boxLow[0] == 2.0f && Draw(2).boxHigh[0] == 13.0f);
    REQUIRE(Draw(3).hasBox && Draw(3).boxLow[0] == 0.0f && Draw(3).boxHigh[0] == 3.0f);
    {
        GfxVertex v;
        memcpy(&v, s_frame.vertices.data() + Draw(1).firstVertexByte, sizeof(v));
        REQUIRE(v.x == 6.0f);
    }
    // A lock makes the copy old: the next draw copies the new vertices.
    Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, buffer, 0, 4, triangle, 3);
    locked = Gfx_LockVertexBuffer(buffer);
    locked[0].x = 100.0f;
    Gfx_UnlockVertexBuffer(buffer);
    Gfx_SetAlphaRef(10);
    Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, buffer, 0, 4, triangle, 3);
    Gfx_Present(FALSE);
    REQUIRE(Draws() == 2 && Draw(1).firstVertexByte == 4 * sizeof(GfxVertex));
    {
        GfxVertex v;
        memcpy(&v, s_frame.vertices.data() + Draw(1).firstVertexByte, sizeof(v));
        REQUIRE(v.x == 100.0f);
    }
    Gfx_DestroyVertexBuffer(buffer);

    // The view test. The view and the projection are the identity here: the
    // clip space is -1 <= x, y <= 1, 0 <= z <= 1. A draw whose vertices are
    // all outside one of these planes is left out; a draw with a vertex
    // inside, or across the view, stays.
    {
        s_cullDraws = 1;
        Gfx_Present(FALSE);
        GfxMatrix m = Identity();
        Gfx_SetTransform(GFX_TRANSFORM_WORLD, &m);
        Gfx_SetTransform(GFX_TRANSFORM_VIEW, &m);
        Gfx_SetTransform(GFX_TRANSFORM_PROJECTION, &m);
        const auto mesh = [](float x0, float x1, float z) {
            GfxVertex v[18] = {};
            WORD list[18];
            for (int i = 0; i < 18; i++) {
                v[i].x = (i & 1) ? x1 : x0;
                v[i].y = (i & 2) ? 0.5f : -0.5f;
                v[i].z = z;
                list[i] = (WORD)i;
            }
            Gfx_DrawIndexedPrimitive(GFX_TRIANGLELIST, v, 18, list, 18);
        };
        mesh(2.0f, 3.0f, 0.5f);         // right of the view
        mesh(-3.0f, -2.0f, 0.5f);       // left
        mesh(-0.5f, 0.5f, -1.0f);       // behind the eye
        mesh(-0.5f, 0.5f, 2.0f);        // beyond the far plane
        mesh(0.5f, 1.5f, 0.5f);         // across the right edge: stays
        mesh(-0.2f, 0.2f, 0.5f);        // inside: stays
        Gfx_Present(FALSE);
        // The two that stay join: one draw.
        REQUIRE(Draws() == 1 && Draw(0).vertexCount == 36);
        REQUIRE(Vertex3D(Draw(0), 1).x == 1.5f && Vertex3D(Draw(0), 18 + 1).x == 0.2f);
        // A vertex left of the view and one right of it: the draw crosses
        // the view, though no vertex is inside.
        {
            GfxVertex v[18] = {};
            WORD list[18];
            for (int i = 0; i < 18; i++) {
                v[i].x = (i & 1) ? 5.0f : -5.0f;
                v[i].z = 0.5f;
                list[i] = (WORD)i;
            }
            Gfx_DrawIndexedPrimitive(GFX_TRIANGLELIST, v, 18, list, 18);
            Gfx_Present(FALSE);
            REQUIRE(Draws() == 1);
        }
        // Small draws in world space: each against the view.
        s_joins = true;
        Billboard(0.2f, 0.1f, 0.5f);
        Billboard(0.2f, 3.0f, 0.5f);
        Billboard(0.2f, -0.1f, 0.6f);
        Billboard(0.2f, 0.0f, 1.5f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).vertexCount == 8 && Vertex3D(Draw(0), 4).x == -0.1f);
        // A quad across the view, no corner inside: it stays.
        Billboard(10.0f, -5.0f, 0.5f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1);
        // The test follows the view: moved 10 to the left, the view does
        // not have the quad; moved back, it has it.
        {
            GfxMatrix moved = Identity();
            moved._41 = -10.0f;
            Gfx_SetTransform(GFX_TRANSFORM_VIEW, &moved);
            Billboard(0.2f, 0.1f, 0.5f);
            mesh(-0.2f, 0.2f, 0.5f);
            Gfx_Present(FALSE);
            REQUIRE(Draws() == 0);
            Gfx_SetTransform(GFX_TRANSFORM_VIEW, &m);
            Billboard(0.2f, 0.1f, 0.5f);
            Gfx_SetTransform(GFX_TRANSFORM_WORLD, &m);
            mesh(-0.2f, 0.2f, 0.5f);
            Gfx_Present(FALSE);
            REQUIRE(Draws() == 1 && Draw(0).vertexCount == 4 + 18);
        }
        // A range of a buffer, with the box of its range. A lock that keeps
        // the positions keeps the boxes. A lock that moves them gives new
        // boxes.
        Gfx_SetTransform(GFX_TRANSFORM_WORLD, &m);
        GfxVertexBuffer *ground = Gfx_CreateVertexBuffer(16);
        GfxVertex *v = Gfx_LockVertexBuffer(ground);
        for (int i = 0; i < 16; i++) {
            v[i].x = (float)i * 0.25f;
            v[i].z = 0.5f;
        }
        Gfx_UnlockVertexBuffer(ground);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, ground, 0, 4, triangle, 3);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, ground, 8, 4, triangle, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).firstVertexByte == 0 && Draw(0).boxHigh[0] == 0.75f);
        const uint64_t far = (uint64_t)8 << 32 | 4;
        REQUIRE(ground->boxes != NULL && ground->boxes->Find(far) != NULL);
        v = Gfx_LockVertexBuffer(ground);
        for (int i = 0; i < 16; i++)
            v[i].diffuse = 0xff808080;
        Gfx_UnlockVertexBuffer(ground);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, ground, 0, 4, triangle, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && ground->boxes->Find(far) != NULL);
        v = Gfx_LockVertexBuffer(ground);
        for (int i = 8; i < 12; i++)
            v[i].x = 0.1f;
        Gfx_UnlockVertexBuffer(ground);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, ground, 8, 4, triangle, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).firstVertexByte == 8 * sizeof(GfxVertex) && Draw(0).boxHigh[0] == 0.1f);
        // The game asks for a range before it draws it (the merged ground).
        v = Gfx_LockVertexBuffer(ground);
        for (int i = 12; i < 16; i++)
            v[i].x = 5.0f;
        Gfx_UnlockVertexBuffer(ground);
        REQUIRE(Gfx_IsRangeVisible(ground, 0, 4) && Gfx_IsRangeVisible(ground, 8, 4));
        REQUIRE(!Gfx_IsRangeVisible(ground, 12, 4) && Gfx_IsRangeVisible(ground, 8, 8));
        REQUIRE(!Gfx_IsRangeVisible(ground, 12, 5) && !Gfx_IsRangeVisible(NULL, 0, 4));
        s_cullDraws = 0;
        Gfx_Present(FALSE);
        REQUIRE(Gfx_IsRangeVisible(ground, 12, 4));
        s_cullDraws = 1;
        Gfx_Present(FALSE);
        Gfx_DestroyVertexBuffer(ground);
        // The game merges its ground draws for an executor that takes
        // joined draws.
        REQUIRE(Gfx_MergesGroundDraws());
        s_joins = false;
        REQUIRE(!Gfx_MergesGroundDraws());
        s_joins = true;
        // Transformed draws are not tested: they are in pixels.
        Sprite(5000.0f);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1);
        s_cullDraws = 0;
        Gfx_Present(FALSE);
    }

    // The tests of the transparent objects (video.alpha_blend = 0,
    // video.alpha_test = 0): each 3D draw is recorded without blending, or
    // without the alpha test. The 2D draws keep both, with the same state of
    // the game: a 3D and a 2D draw get two recorded states.
    {
        GfxMatrix m = Identity();
        Gfx_SetTransform(GFX_TRANSFORM_WORLD, &m);
        GfxVertex v[3] = {};
        v[1].x = 1.0f;
        Gfx_SetAlphaBlend(TRUE);
        Gfx_SetAlphaTest(TRUE);
        Gfx_SetAlphaFunc(GFX_CMP_GREATEREQUAL);
        Gfx_SetAlphaRef(1);
        const auto frame = [&] {
            Gfx_DrawPrimitive(GFX_TRIANGLELIST, v, 3);
            Sprite(10.0f);
            Gfx_DrawPrimitive(GFX_TRIANGLELIST, v, 3);
            Gfx_Present(FALSE);
        };
        frame();
        REQUIRE(Draws() == 3 && s_frame.states.size() == 1);
        REQUIRE(s_frame.states[0].alphaBlend && s_frame.states[0].alphaTest);
        s_alphaBlend = 0;
        Gfx_Present(FALSE);
        frame();
        REQUIRE(Draws() == 3 && s_frame.states.size() == 3);
        REQUIRE(!s_frame.states[Draw(0).state].alphaBlend && s_frame.states[Draw(0).state].alphaTest);
        REQUIRE(s_frame.states[Draw(1).state].alphaBlend && s_frame.states[Draw(1).state].alphaTest);
        REQUIRE(!s_frame.states[Draw(2).state].alphaBlend && Draw(1).transformed && !Draw(2).transformed);
        s_alphaBlend = 1;
        s_alphaTest = 0;
        Gfx_Present(FALSE);
        frame();
        REQUIRE(Draws() == 3 && s_frame.states[Draw(0).state].alphaBlend && !s_frame.states[Draw(0).state].alphaTest);
        REQUIRE(s_frame.states[Draw(1).state].alphaTest && !s_frame.states[Draw(2).state].alphaTest);
        // Both, and two 3D draws in a row share their recorded state.
        s_alphaBlend = 0;
        Gfx_Present(FALSE);
        Gfx_DrawPrimitive(GFX_TRIANGLELIST, v, 3);
        v[0].z = 0.5f;
        Gfx_DrawPrimitive(GFX_TRIANGLELIST, v, 3);
        Gfx_Present(FALSE);
        REQUIRE(s_frame.states.size() == 1 && !s_frame.states[0].alphaBlend && !s_frame.states[0].alphaTest);
        s_alphaBlend = 1;
        s_alphaTest = 1;
        Gfx_Present(FALSE);
        frame();
        REQUIRE(Draws() == 3 && s_frame.states.size() == 1 && s_frame.states[0].alphaBlend);
        Gfx_SetAlphaBlend(FALSE);
        Gfx_SetAlphaTest(FALSE);
        Gfx_SetAlphaFunc(GFX_CMP_ALWAYS);
        Gfx_SetAlphaRef(0);
    }

    // A batch (Gfx_DrawBatch) gives the frame the draws that the same draws
    // made alone give: the small ones in world space, from the atlas where
    // their texture goes in, joined, those outside the view left out, and a
    // large one as it is (here a snapshot of its buffer). The world matrix
    // after the batch is that of its last draw. With an executor that does
    // not take joined draws, too.
    {
        GfxTexture *oak = TreeTexture(64, 64, 7), *pine = TreeTexture(32, 32, 8);
        GfxTexture *bark = Gfx_CreateTexture(16, 16, GFX_FORMAT_BGRA8, 1, 0);
        GfxVertexBuffer *buffer = Gfx_CreateVertexBuffer(40);
        REQUIRE(oak != NULL && pine != NULL && bark != NULL && buffer != NULL);
        GfxVertex *b = Gfx_LockVertexBuffer(buffer);
        for (int i = 0; i < 40; i++) {
            b[i].x = (float)(i & 1) * 0.5f + (i >= 8 ? 0.01f * (float)i : 0.0f);
            b[i].y = (float)((i >> 1) & 1) * 0.5f;
            b[i].nz = -1.0f;
            b[i].diffuse = 0xff203040u + (DWORD)i;
            b[i].u0 = (float)(i & 1);
            b[i].v0 = (float)((i >> 1) & 1);
        }
        Gfx_UnlockVertexBuffer(buffer);
        const WORD quad[6] = { 0, 1, 2, 2, 1, 3 };
        WORD mesh[30];
        for (int i = 0; i < 30; i++)
            mesh[i] = (WORD)i;
        GfxMatrix m[6];
        for (int i = 0; i < 6; i++) {
            m[i] = Identity();
            m[i]._11 = m[i]._22 = m[i]._33 = 0.5f + 0.1f * (float)i;
            m[i]._41 = -0.9f + 0.25f * (float)i;
            m[i]._43 = 0.25f + 0.05f * (float)i;
        }
        m[4]._41 = 100.0f;      // outside the view
        const GfxBatchDraw draws[6] = {
            { &m[0], oak, buffer, 0, 4, quad, 6 },  { &m[1], pine, buffer, 4, 4, quad, 6 },
            { &m[2], bark, buffer, 0, 4, quad, 6 }, { &m[3], oak, buffer, 8, 32, mesh, 30 },
            { &m[4], oak, buffer, 0, 4, quad, 6 },  { &m[5], pine, buffer, 4, 4, quad, 6 },
        };
        const GfxMatrix identity = Identity();
        const auto start = [&] {
            Gfx_SetTransform(GFX_TRANSFORM_VIEW, &identity);
            Gfx_SetTransform(GFX_TRANSFORM_PROJECTION, &identity);
            Gfx_SetStageAddress(0, GFX_ADDRESS_CLAMP);
            Gfx_SetStageTexCoordIndex(0, 0);
            Gfx_SetStageTextureTransform(0, GFX_TTFF_DISABLE);
            Gfx_SetTexture(1, NULL);
            Gfx_SetStageColorOp(1, GFX_TOP_DISABLE);
            Gfx_SetAlphaBlend(FALSE);
            Gfx_SetAlphaTest(FALSE);
        };
        const auto alone = [&] {
            for (const GfxBatchDraw &d : draws) {
                Gfx_SetTransform(GFX_TRANSFORM_WORLD, d.world);
                Gfx_SetTexture(0, d.texture);
                Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, d.buffer, d.firstVertex, d.vertexCount, d.indices,
                                           d.indexCount);
            }
        };
        GfxTexture *atlasTexture = NULL;
        for (int joins = 1; joins >= 0; joins--) {
            s_joins = joins != 0;
            s_cullDraws = 1;
            Gfx_Present(FALSE);
            start();
            alone();
            Gfx_Present(FALSE);
            const std::string made = FrameText();
            start();
            Gfx_DrawBatch(draws, 6);
            GfxMatrix after;
            Gfx_GetTransform(GFX_TRANSFORM_WORLD, &after);
            REQUIRE(memcmp(&after, &m[5], sizeof(after)) == 0);
            Gfx_Present(FALSE);
            REQUIRE(FrameText() == made);
            if (joins) {
                // Oak and pine from the atlas joined, the bark alone, the
                // snapshot of the mesh, the last pine; the tree outside the
                // view is left out.
                REQUIRE(Draws() == 4 && Draw(0).vertexCount == 8 && Draw(1).textures[0] == bark);
                REQUIRE(Draw(2).snapshot && Draw(3).vertexCount == 4 && Draw(0).textures[0] == Draw(3).textures[0]);
                REQUIRE(Draw(0).textures[0] != oak && Draw(0).reach > 0.0f && Draw(0).hasBox);
                // The atlas keeps 3 mip levels, and tells the part that
                // changed (the executor's levels of it).
                REQUIRE(Draw(0).textures[0]->mipLimit == 3 && !Draw(0).textures[0]->changedRect.Empty());
                atlasTexture = Draw(0).textures[0];
            } else {
                // Each draw alone; the box of the tree outside the view is
                // outside it.
                REQUIRE(Draws() == 5);
            }
        }
        // A texture that only a draw of a batch reads (the state of the batch
        // has the texture of its first draw) is drawn before it goes: the
        // frame so far runs first.
        s_joins = true;
        start();
        const GfxBatchDraw two[2] = { draws[0], draws[2] };
        Gfx_DrawBatch(two, 2);
        s_frame = GfxFrame();
        Gfx_DestroyTexture(bark);
        REQUIRE(Draws() == 2 && Draw(1).textures[0] == bark);
        Gfx_Present(FALSE);

        // The small draws of a batch refer to the vertices of the buffer:
        // the recorder does not copy them. A batch job makes the draws of
        // them (here when the game gives the batch). The game writes such
        // vertices again: the draws of the batch before have the vertices as
        // they were, and no draw runs for the lock.
        const auto worldX = [&](float x) { return x * m[0]._11 + m[0]._41; };
        start();
        Gfx_DrawBatch(two, 1);      // the oak quad: vertices 0 to 3
        s_frame = GfxFrame();
        GfxVertex *w = Gfx_LockVertexBufferRange(buffer, 0, 4);
        REQUIRE(Draws() == 0);
        w[1].x = 0.4f;
        Gfx_UnlockVertexBuffer(buffer);
        Gfx_DrawBatch(two, 1);
        Gfx_Present(FALSE);
        // Both quads from the atlas, joined: the first as it was.
        REQUIRE(Draws() == 1 && Draw(0).vertexCount == 8);
        REQUIRE(Vertex3D(Draw(0), 1).x == worldX(0.5f) && Vertex3D(Draw(0), 5).x == worldX(0.4f));
        // The same with the batch jobs on a thread of their own: the lock
        // waits for the job that reads the vertices. 4,000 draws in one job,
        // so that it runs for some time.
        {
            REQUIRE(Gfx_StartBatchThread());
            std::vector<GfxBatchDraw> many(4000, two[0]);
            start();
            Gfx_DrawBatch(many.data(), (DWORD)many.size());
            w = Gfx_LockVertexBufferRange(buffer, 0, 4);
            w[1].x = 0.5f;
            Gfx_UnlockVertexBuffer(buffer);
            Gfx_Present(FALSE);
            unsigned vertices = 0;
            for (unsigned d = 0; d < Draws(); d++) {
                for (unsigned v = 1; v < Draw(d).vertexCount; v += 4)
                    REQUIRE(Vertex3D(Draw(d), v).x == worldX(0.4f));
                vertices += Draw(d).vertexCount;
            }
            REQUIRE(vertices == 16000);
            // A frame of batches is the frame of the same draws made alone,
            // with the thread. (Some frames first: the vertices that the
            // lock wrote are at rest then, in both frames.)
            for (int i = 0; i < 3; i++)
                Gfx_Present(FALSE);
            // A sprite first: the frame has vertices and indices of its own
            // before those of the batches.
            const auto sprite = [&] {
                Gfx_SetTransform(GFX_TRANSFORM_WORLD, &identity);
                Gfx_SetTexture(0, NULL);
                Sprite(1.0f);
            };
            // (Without the bark: that texture is gone.)
            const GfxBatchDraw live[5] = { draws[0], draws[1], draws[3], draws[4], draws[5] };
            sprite();
            start();
            for (const GfxBatchDraw &d : live) {
                Gfx_SetTransform(GFX_TRANSFORM_WORLD, d.world);
                Gfx_SetTexture(0, d.texture);
                Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, d.buffer, d.firstVertex, d.vertexCount, d.indices,
                                           d.indexCount);
            }
            Gfx_Present(FALSE);
            const std::string made = FrameText();
            sprite();
            start();
            Gfx_DrawBatch(live, 5);
            Gfx_Present(FALSE);
            REQUIRE(FrameText() == made && Draw(1).firstVertexByte != 0 && Draw(1).firstIndex != 0);
            Gfx_StopBatchThread();
        }
        // A small draw with texture coordinates outside its texture keeps
        // its texture, in a batch as alone: the batch job finds it when it
        // reads the vertices.
        w = Gfx_LockVertexBuffer(buffer);
        w[1].u0 = 2.0f;
        Gfx_UnlockVertexBuffer(buffer);
        start();
        Gfx_SetTransform(GFX_TRANSFORM_WORLD, two[0].world);
        Gfx_SetTexture(0, oak);
        Gfx_DrawIndexedPrimitiveVB(GFX_TRIANGLELIST, buffer, 0, 4, quad, 6);
        Gfx_Present(FALSE);
        const std::string outside = FrameText();
        REQUIRE(Draws() == 1 && Draw(0).textures[0] == oak);
        start();
        Gfx_DrawBatch(two, 1);
        Gfx_Present(FALSE);
        REQUIRE(FrameText() == outside && Draws() == 1 && Draw(0).textures[0] == oak);
        // A buffer that goes: the draws of its batch are made already.
        Gfx_DrawBatch(two, 1);
        s_cullDraws = 0;
        Gfx_DestroyVertexBuffer(buffer);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).vertexCount == 4);
        Gfx_Present(FALSE);
        // A texture of the atlas goes: the atlas starts again, and all of it
        // is the part that changed (the textures that come take the places
        // of those that were there).
        atlasTexture->changedRect = GfxMipRect();
        Gfx_DestroyTexture(oak);
        REQUIRE(atlasTexture->changedRect.x0 == 0 && atlasTexture->changedRect.y0 == 0 &&
                atlasTexture->changedRect.x1 == atlasTexture->width &&
                atlasTexture->changedRect.y1 == atlasTexture->height);
        Gfx_DestroyTexture(pine);
        Gfx_SetStageAddress(0, GFX_ADDRESS_WRAP);
    }

    // The mip levels of a BC3 texture (render/texture_mips.cpp): each level
    // a 2x2 box of the level before. In a texture of blocks of grey and
    // black columns, a texel of level 1 is the mean of its 2x2 texels. A
    // part that changes (a texture new in the atlas) gives, made again
    // alone, the levels of the whole texture made again.
    {
        REQUIRE(Gfx_MipLevelCount(64, 32, 0) == 7 && Gfx_MipLevelCount(256, 256, 3) == 3);
        REQUIRE(Gfx_MipLevelCount(1, 1, 0) == 1 && Gfx_MipLevelCount(8, 8, 0) == 4);
        const int w = 64, h = 32;
        std::vector<uint8_t> level0((size_t)(w / 4) * (h / 4) * 16);
        for (int by = 0; by < h / 4; by++)
            for (int bx = 0; bx < w / 4; bx++) {
                uint8_t *b = &level0[((size_t)by * (w / 4) + bx) * 16];
                const int v = 16 + (bx + by) % 16;
                const uint16_t c = (uint16_t)(v << 11 | (v * 2) << 5 | v);
                memset(b, 0, 16);
                b[0] = b[1] = 255;      // alpha 255
                b[8] = (uint8_t)c;      // colour 0 the grey, colour 1 black
                b[9] = (uint8_t)(c >> 8);
                memset(b + 12, 0x44, 4);    // the columns: colour 0, 1, 0, 1
            }
        std::vector<uint8_t> mips[16];
        Gfx_MakeBC3Mips(level0.data(), w, h, 7, GfxMipRect(), mips);
        REQUIRE(mips[0].empty() && mips[1].size() == 8 * 4 * 16 && mips[5].size() == 16 && mips[6].size() == 16);
        int worst = 0;
        for (int y = 0; y < h / 2; y++)
            for (int x = 0; x < w / 2; x++) {
                uint32_t a[16], b[16];
                Gfx_DecodeBlock(GFX_FORMAT_BC3, &mips[1][((size_t)(y / 4) * (w / 8) + x / 4) * 16], a);
                Gfx_DecodeBlock(GFX_FORMAT_BC3, &level0[((size_t)(2 * y / 4) * (w / 4) + 2 * x / 4) * 16], b);
                const uint32_t p = a[(y % 4) * 4 + x % 4];
                const int tx = 2 * x % 4, ty = 2 * y % 4;
                const uint32_t q[4] = { b[ty * 4 + tx], b[ty * 4 + tx + 1], b[(ty + 1) * 4 + tx],
                                        b[(ty + 1) * 4 + tx + 1] };
                REQUIRE(p >> 24 == 255);
                for (int s = 0; s < 24; s += 8) {
                    const int mean = (int)((q[0] >> s & 255) + (q[1] >> s & 255) + (q[2] >> s & 255) +
                                           (q[3] >> s & 255) + 2) / 4;
                    const int d = (int)(p >> s & 255) - mean;
                    worst = d > worst ? d : -d > worst ? -d : worst;
                }
            }
        REQUIRE(worst <= 10);
        // A red texture stays red: red and blue keep their places.
        std::vector<uint8_t> red((size_t)4 * 4 * 16);
        for (size_t i = 0; i < red.size(); i += 16) {
            memset(&red[i], 0, 16);
            red[i] = red[i + 1] = 255;
            red[i + 9] = red[i + 11] = 0xf8;    // 0xf800: red 31, green 0, blue 0
        }
        std::vector<uint8_t> redMips[16];
        Gfx_MakeBC3Mips(red.data(), 16, 16, 2, GfxMipRect(), redMips);
        uint32_t texels[16];
        Gfx_DecodeBlock(GFX_FORMAT_BC3, redMips[1].data(), texels);
        REQUIRE((texels[5] >> 16 & 255) >= 240 && (texels[5] & 255) <= 8 && (texels[5] >> 8 & 255) <= 8);

        const int size = 256;
        std::vector<uint8_t> atlas((size_t)(size / 4) * (size / 4) * 16);
        unsigned seed = 5;
        for (uint8_t &byte : atlas) {
            seed = seed * 1664525u + 1013904223u;
            byte = (uint8_t)(seed >> 24);
        }
        std::vector<uint8_t> kept[16], whole[16];
        Gfx_MakeBC3Mips(atlas.data(), size, size, 3, GfxMipRect(), kept);
        const std::vector<uint8_t> before = kept[1];
        // What the GPU has, for the test of the part that changes below.
        std::vector<uint8_t> onGpu[3] = { atlas, kept[1], kept[2] };
        GfxMipRect tile;
        tile.x0 = 40;
        tile.y0 = 36;
        tile.x1 = 72;
        tile.y1 = 104;
        for (int by = tile.y0 / 4; by < tile.y1 / 4; by++)
            for (int bx = tile.x0 / 4; bx < tile.x1 / 4; bx++)
                for (int i = 0; i < 16; i++) {
                    seed = seed * 1664525u + 1013904223u;
                    atlas[((size_t)by * (size / 4) + bx) * 16 + i] = (uint8_t)(seed >> 24);
                }
        Gfx_MakeBC3Mips(atlas.data(), size, size, 3, tile, kept);
        Gfx_MakeBC3Mips(atlas.data(), size, size, 3, GfxMipRect(), whole);
        REQUIRE(kept[1] == whole[1] && kept[2] == whole[2] && kept[1] != before);
        // An executor writes only the blocks of the part into the levels
        // that it has (Gfx_MipPartBlocks): each level is then the new one.
        {
            const GfxMipRect part = Gfx_MipPartOf(tile, size, size, 3);
            REQUIRE(part.x0 == 32 && part.y0 == 32 && part.x1 == 80 && part.y1 == 112);
            const std::vector<uint8_t> *now[3] = { &atlas, &kept[1], &kept[2] };
            for (int m = 0; m < 3; m++) {
                const GfxMipRect blocks = Gfx_MipPartBlocks(part, size, size, m);
                const int rowBlocks = (size >> m) / 4;
                REQUIRE(blocks.x0 == 8 >> m && blocks.y0 == 8 >> m && blocks.x1 == 20 >> m && blocks.y1 == 28 >> m);
                REQUIRE(onGpu[m] != *now[m]);
                for (int by = blocks.y0; by < blocks.y1; by++)
                    memcpy(&onGpu[m][((size_t)by * rowBlocks + blocks.x0) * 16],
                           &(*now[m])[((size_t)by * rowBlocks + blocks.x0) * 16], (size_t)(blocks.x1 - blocks.x0) * 16);
                REQUIRE(onGpu[m] == *now[m]);
            }
            // All of a texture, and a level smaller than a block.
            const GfxMipRect all = Gfx_MipPartOf(GfxMipRect(), 64, 32, 7);
            REQUIRE(all.x0 == 0 && all.y0 == 0 && all.x1 == 64 && all.y1 == 32);
            const GfxMipRect last = Gfx_MipPartBlocks(all, 64, 32, 6), first = Gfx_MipPartBlocks(all, 64, 32, 0);
            REQUIRE(last.x0 == 0 && last.y0 == 0 && last.x1 == 1 && last.y1 == 1);
            REQUIRE(first.x1 == 16 && first.y1 == 8);
        }
        // New levels for a part: of a texture that is not zero outside the
        // part, all of it is made; of an atlas that is new (zero but for a
        // tile), the tile is enough. Both are the levels of the whole.
        std::vector<uint8_t> fresh[16];
        Gfx_MakeBC3Mips(atlas.data(), size, size, 3, tile, fresh);
        REQUIRE(fresh[1] == whole[1] && fresh[2] == whole[2]);
        std::vector<uint8_t> blank(atlas.size(), 0);
        for (int by = tile.y0 / 4; by < tile.y1 / 4; by++)
            memcpy(&blank[((size_t)by * (size / 4) + tile.x0 / 4) * 16],
                   &atlas[((size_t)by * (size / 4) + tile.x0 / 4) * 16], (size_t)(tile.x1 - tile.x0) / 4 * 16);
        std::vector<uint8_t> part[16], all[16];
        Gfx_MakeBC3Mips(blank.data(), size, size, 3, tile, part);
        Gfx_MakeBC3Mips(blank.data(), size, size, 3, GfxMipRect(), all);
        REQUIRE(part[1] == all[1] && part[2] == all[2] && part[1] != std::vector<uint8_t>(part[1].size(), 0));
    }

    // The performance stats (video.perf_stats = 0): the recorder counts no
    // small draw for the log. On again, it counts after the next log line.
    {
        char text[512];
        s_joins = true;
        const auto small = [&] {
            Billboard(2.0f, 10.0f, 5.0f);
            Gfx_Present(FALSE);
            Gfx_RecorderCountsText(text, sizeof(text));
            return strstr(text, "small draws 1 ") != NULL;
        };
        Gfx_RecorderCountsText(text, sizeof(text));
        REQUIRE(Gfx_PerfStatsWanted() && small());
        s_perfStats = 0;
        Gfx_Present(FALSE);
        REQUIRE(!Gfx_PerfStatsWanted() && !small());
        s_perfStats = 1;
        Gfx_Present(FALSE);
        REQUIRE(Gfx_PerfStatsWanted() && !small() && small());
    }

    // The least alpha of a buffer's colours goes with each draw of it
    // without blending and with the alpha test, as of the submit. The
    // executor then reads no GPU copy. It only goes down. A buffer drawn
    // from its GPU copy rested since its last lock (kStaticAfterFrames).
    {
        s_joins = false;
        const auto rest = [] {
            for (int i = 0; i < 5; i++)
                Gfx_Present(FALSE);
        };
        GfxVertexBuffer *mesh = Gfx_CreateVertexBuffer(8);
        GfxVertex *v = Gfx_LockVertexBuffer(mesh);
        for (int i = 0; i < 8; i++) {
            v[i].diffuse = 0xff808080;
            v[i].specular = 0xf0000000;
        }
        Gfx_UnlockVertexBuffer(mesh);
        rest();
        Gfx_SetAlphaBlend(FALSE);
        Gfx_SetAlphaTest(TRUE);
        Gfx_DrawPrimitiveVB(GFX_TRIANGLELIST, mesh, 0, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).buffer == mesh && Draw(0).vertexAlphaKnown);
        REQUIRE(Draw(0).vertexAlpha[0] == 0xff && Draw(0).vertexAlpha[1] == 0xf0);
        // A lock that lowers a vertex outside the draw: the buffer's least
        // alpha at the next submit.
        v = Gfx_LockVertexBuffer(mesh);
        v[5].diffuse = 0x40808080;
        Gfx_UnlockVertexBuffer(mesh);
        rest();
        Gfx_DrawPrimitiveVB(GFX_TRIANGLELIST, mesh, 0, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).buffer == mesh && Draw(0).vertexAlpha[0] == 0x40);
        // Written back: the least alpha stays.
        v = Gfx_LockVertexBuffer(mesh);
        v[5].diffuse = 0xff808080;
        Gfx_UnlockVertexBuffer(mesh);
        rest();
        Gfx_DrawPrimitiveVB(GFX_TRIANGLELIST, mesh, 0, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).buffer == mesh && Draw(0).vertexAlpha[0] == 0x40);
        // A blended draw, and one without the test: none.
        Gfx_SetAlphaBlend(TRUE);
        Gfx_DrawPrimitiveVB(GFX_TRIANGLELIST, mesh, 0, 3);
        Gfx_SetAlphaBlend(FALSE);
        Gfx_SetAlphaTest(FALSE);
        Gfx_DrawPrimitiveVB(GFX_TRIANGLELIST, mesh, 3, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 2 && !Draw(0).vertexAlphaKnown && !Draw(1).vertexAlphaKnown);
        // A buffer that the game never wrote: alpha 0.
        GfxVertexBuffer *fresh = Gfx_CreateVertexBuffer(4);
        rest();
        Gfx_SetAlphaTest(TRUE);
        Gfx_DrawPrimitiveVB(GFX_TRIANGLELIST, fresh, 0, 3);
        Gfx_Present(FALSE);
        REQUIRE(Draws() == 1 && Draw(0).buffer == fresh && Draw(0).vertexAlphaKnown && Draw(0).vertexAlpha[0] == 0);
        Gfx_SetAlphaTest(FALSE);
        Gfx_DestroyVertexBuffer(fresh);
        Gfx_DestroyVertexBuffer(mesh);
    }

    printf("gfx_record: ok\n");
    return 0;
}
