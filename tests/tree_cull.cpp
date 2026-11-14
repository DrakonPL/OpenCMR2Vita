// The sphere test of a tree against the view (game/TreeCull.h): a tree that
// it leaves out has each vertex outside one plane of the view, as the view
// test of the renderer tells it (render/gfx_small_draw.h). It leaves out
// most of the trees that the renderer leaves out.

#include "render/gfx_small_draw.h"
#include "TreeCull.h"

#include <cmath>
#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

unsigned s_seed = 2718;

float Next()
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / (float)(1u << 24);
}

// The clip matrix of a camera at (px, py, pz) that looks along the
// direction of the angle (around +y), with a perspective projection.
void ClipMatrix(float px, float py, float pz, float angle, float scaleX, float scaleY, float nearPlane,
                float farPlane, float out[16])
{
    const float c = cosf(angle), s = sinf(angle);
    GfxMatrix world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    GfxMatrix view = { c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1 };
    view._41 = -(px * c - pz * s);
    view._42 = -py;
    view._43 = -(px * s + pz * c);
    GfxMatrix projection = {};
    projection._11 = scaleX;
    projection._22 = scaleY;
    projection._33 = farPlane / (farPlane - nearPlane);
    projection._34 = 1.0f;
    projection._43 = -(nearPlane * farPlane) / (farPlane - nearPlane);
    Gfx_ClipMatrixOf(world, view, projection, out);
}

} // namespace

int main()
{
    float clip[16], planes[24];
    TreeCullView view;

    // The radius of a box: its farthest corner from the origin.
    {
        const float low[3] = { -2.0f, 0.0f, -0.5f }, high[3] = { 1.0f, 9.0f, 0.25f };
        REQUIRE(fabsf(TreeCull_BoxRadius(low, high) - sqrtf(4.0f + 81.0f + 0.25f)) < 1e-5f);
    }

    // A camera at the origin that looks along +z: near 1, far 300.
    ClipMatrix(0, 0, 0, 0, 1.0f, 1.3f, 1.0f, 300.0f, clip);
    Gfx_ViewPlanesOf(clip, planes);
    TreeCull_SetView(&view, planes);
    REQUIRE(!TreeCull_Outside(view, 0.0f, 0.0f, 50.0f, 6.0f));      // ahead
    REQUIRE(TreeCull_Outside(view, 0.0f, 0.0f, -20.0f, 6.0f));      // behind
    REQUIRE(!TreeCull_Outside(view, 0.0f, 0.0f, -4.0f, 6.0f));      // behind, but it reaches the view
    REQUIRE(TreeCull_Outside(view, 0.0f, 0.0f, -6.0f, 6.0f));       // behind, 7 from the near plane
    REQUIRE(TreeCull_Outside(view, 200.0f, 0.0f, 50.0f, 6.0f));     // far to the side
    REQUIRE(!TreeCull_Outside(view, 54.0f, 0.0f, 50.0f, 6.0f));     // at the side, it reaches the view
    REQUIRE(TreeCull_Outside(view, 0.0f, 0.0f, 320.0f, 6.0f));      // past the far plane
    REQUIRE(!TreeCull_Outside(view, 0.0f, 0.0f, 304.0f, 6.0f));
    REQUIRE(TreeCull_Outside(view, 0.0f, 120.0f, 50.0f, 6.0f));     // above

    // Trees as the game makes them: a mesh of some vertices near its origin,
    // turned around +y, with a scale for each axis, at a place. Against the
    // view test of the renderer.
    int both = 0, rendererOnly = 0, inside = 0;
    float camera[3] = { 0, 0, 0 };
    for (int round = 0; round < 60000; round++) {
        if (round % 40 == 0) {
            camera[0] = (Next() - 0.5f) * 3000.0f;
            camera[1] = Next() * 30.0f;
            camera[2] = (Next() - 0.5f) * 3000.0f;
            ClipMatrix(camera[0], camera[1], camera[2], Next() * 6.2831853f, 0.6f + Next(), 0.8f + Next() * 1.5f,
                       0.1f + Next(), 100.0f + Next() * 400.0f, clip);
            Gfx_ViewPlanesOf(clip, planes);
            TreeCull_SetView(&view, planes);
        }
        // The mesh: 4 to 12 vertices in a box.
        GfxVertex local[12] = {};
        const int count = 4 + (int)(Next() * 8.99f);
        float low[3] = { 1e30f, 1e30f, 1e30f }, high[3] = { -1e30f, -1e30f, -1e30f };
        for (int i = 0; i < count; i++) {
            const float p[3] = { (Next() - 0.5f) * 8.0f, Next() * 14.0f - 1.0f, (Next() - 0.5f) * 2.0f };
            local[i].x = p[0];
            local[i].y = p[1];
            local[i].z = p[2];
            for (int k = 0; k < 3; k++) {
                low[k] = p[k] < low[k] ? p[k] : low[k];
                high[k] = p[k] > high[k] ? p[k] : high[k];
            }
        }
        const float scale[3] = { 0.5f + Next() * 2.0f, 0.5f + Next() * 2.0f, 0.5f + Next() * 2.0f };
        const float place[3] = { camera[0] + (Next() - 0.5f) * 700.0f, Next() * 10.0f,
                                 camera[2] + (Next() - 0.5f) * 700.0f };
        const float turn = Next() * 6.2831853f, c = cosf(turn), s = sinf(turn);
        GfxMatrix world = { c * scale[0], 0, s * scale[0], 0, 0, scale[1], 0, 0, -s * scale[2], 0, c * scale[2], 0,
                            place[0], place[1], place[2], 1 };
        GfxVertex v[12];
        Gfx_SmallDrawToWorld(world, local, (DWORD)count, v);
        const bool rendererOut = Gfx_PointsOutsideView(clip, v, (DWORD)count);
        float greatest = scale[0] > scale[1] ? scale[0] : scale[1];
        greatest = scale[2] > greatest ? scale[2] : greatest;
        const bool out = TreeCull_Outside(view, place[0], place[1], place[2], TreeCull_BoxRadius(low, high) * greatest);
        // What the sphere leaves out, the renderer leaves out.
        REQUIRE(!out || rendererOut);
        both += out;
        rendererOnly += rendererOut && !out;
        inside += !rendererOut;
    }
    // Most of the trees outside the view, and enough of each kind.
    REQUIRE(both > 15000 && inside > 5000);
    REQUIRE(rendererOnly * 4 < both);

    printf("ok\n");
    return 0;
}
