// The recorder (src/render/gfx_core.cpp): what a frame holds for the game's
// draws. A setter with the value that is already set starts no new state. An
// executor that takes joined draws gets a run of sprites or lines as one
// draw, and a small 3D draw in world space: a run of billboards, each with a
// world matrix of its own, has one state. The parts of a mesh in a vertex
// buffer that is not drawn by reference share one copy of its vertices.

#include "port/sys.h"
#include "render/gfx_internal.h"

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
    // Another texture starts a new draw and a new state, in world space also.
    // A draw that is not small keeps its world matrix, and so does the small
    // draw after it: each has the state of its vertices.
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
        REQUIRE(Draws() == 4 && s_frame.states.size() == 4);
        REQUIRE(IsIdentity(s_frame.states[Draw(0).state].world) && IsIdentity(s_frame.states[Draw(1).state].world));
        REQUIRE(s_frame.states[Draw(1).state].stages[0].texture == leaves);
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
