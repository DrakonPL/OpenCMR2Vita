// The recorder (src/render/gfx_core.cpp): what a frame holds for the game's
// draws. A setter with the value that is already set starts no new state. An
// executor that takes joined draws gets a run of sprites or lines as one
// draw, and a small 3D draw in world space: a run of billboards, each with a
// world matrix of its own, has one state. For that executor a new texture
// alone starts no new state: each draw has its textures. The parts of a mesh
// in a vertex buffer that is not drawn by reference share one copy of its
// vertices.

#include "port/sys.h"
#include "render/gfx_internal.h"
#include "render/gxm_vertex_uniforms.h"

#include <functional>

#include <cstdio>
#include <cstring>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

extern "C" void Sys_GetDesktopSize(int *w, int *h) { *w = 640; *h = 480; }
extern "C" void Sys_Log(const char *, ...) {}
extern "C" int Sys_GetOption(const char *, int fallback) { return fallback; }
int Platform_GetSettingInt(const char *, int value) { return value; }
const char *Platform_GetSetting(const char *, const char *value) { return value; }

namespace {

bool s_joins;
GfxFrame s_frame;       // the frame of the last present

struct CaptureExecutor : GfxExecutor {
    bool SetVideoMode(int, int, bool) override { return true; }
    void SetFullscreen(bool) override {}
    void Execute(GfxFrame &frame, std::vector<GfxTexture *> &, GfxReadback &, bool, bool, bool) override
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

    // A vertex buffer the game has just locked is not drawn by reference:
    // its vertices are copied into the frame. The parts of a mesh, each drawn
    // with a range inside the range of the part before, share one copy.
    GfxVertexBuffer *buffer = Gfx_CreateVertexBuffer(16);
    GfxVertex *locked = Gfx_LockVertexBuffer(buffer);
    for (int i = 0; i < 16; i++)
        locked[i].x = (float)i;
    Gfx_UnlockVertexBuffer(buffer);
    const WORD triangle[3] = { 0, 1, 2 };
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

    printf("gfx_record: ok\n");
    return 0;
}
