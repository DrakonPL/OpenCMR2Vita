// The view test of a box (render/gfx_small_draw.h): Gfx_BoxOutsideView tells
// what the view test of the eight corners tells (Gfx_OutsidePlanes: each
// corner outside one plane).

#include "render/gfx_small_draw.h"

#include <cmath>
#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

unsigned s_seed = 99;

float Next()
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / (float)(1u << 24);
}

// The view test of the eight corners, with the least distance of a corner
// to a plane that does not tell the same for each corner: a box that near
// to a plane can round to the other side.
bool CornersOutsideView(const float *m, const float low[3], const float high[3], float *margin)
{
    unsigned all = 63;
    float least = 1e30f;
    for (int i = 0; i < 8; i++) {
        const float x = (i & 1) ? high[0] : low[0], y = (i & 2) ? high[1] : low[1], z = (i & 4) ? high[2] : low[2];
        all &= Gfx_OutsidePlanes(m, x, y, z);
        const float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
        const float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        const float cz = x * m[2] + y * m[6] + z * m[10] + m[14];
        const float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        const float d[6] = { cx + cw, cw - cx, cy + cw, cw - cy, cz, cw - cz };
        for (float v : d)
            least = fabsf(v) < least ? fabsf(v) : least;
    }
    *margin = least;
    return all != 0;
}

// The clip matrix of a camera at (px, py, pz) that looks along the
// direction of the angle (around +y), with a perspective projection.
void ClipMatrix(float px, float py, float pz, float angle, float scaleX, float scaleY, float nearPlane,
                float farPlane, float out[16])
{
    const float c = cosf(angle), s = sinf(angle);
    GfxMatrix world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    // Right (c, 0, -s), up (0, 1, 0), forward (s, 0, c).
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
    float m[16];

    // A camera at the origin that looks along +z: near 1, far 100.
    ClipMatrix(0, 0, 0, 0, 1.0f, 1.0f, 1.0f, 100.0f, m);
    {
        const float inLow[3] = { -1, -1, 10 }, inHigh[3] = { 1, 1, 12 };
        REQUIRE(!Gfx_BoxOutsideView(m, inLow, inHigh));
        const float behindLow[3] = { -1, -1, -12 }, behindHigh[3] = { 1, 1, -10 };
        REQUIRE(Gfx_BoxOutsideView(m, behindLow, behindHigh));
        const float farLow[3] = { -1, -1, 150 }, farHigh[3] = { 1, 1, 160 };
        REQUIRE(Gfx_BoxOutsideView(m, farLow, farHigh));
        const float leftLow[3] = { -40, -1, 10 }, leftHigh[3] = { -30, 1, 12 };
        REQUIRE(Gfx_BoxOutsideView(m, leftLow, leftHigh));
        const float rightLow[3] = { 30, -1, 10 }, rightHigh[3] = { 40, 1, 12 };
        REQUIRE(Gfx_BoxOutsideView(m, rightLow, rightHigh));
        const float belowLow[3] = { -1, -40, 10 }, belowHigh[3] = { 1, -30, 12 };
        REQUIRE(Gfx_BoxOutsideView(m, belowLow, belowHigh));
        const float aboveLow[3] = { -1, 30, 10 }, aboveHigh[3] = { 1, 40, 12 };
        REQUIRE(Gfx_BoxOutsideView(m, aboveLow, aboveHigh));
        // Across a plane: not outside.
        const float acrossLow[3] = { -40, -1, 10 }, acrossHigh[3] = { -5, 1, 12 };
        REQUIRE(!Gfx_BoxOutsideView(m, acrossLow, acrossHigh));
        // Around the camera: not outside.
        const float aroundLow[3] = { -500, -500, -500 }, aroundHigh[3] = { 500, 500, 500 };
        REQUIRE(!Gfx_BoxOutsideView(m, aroundLow, aroundHigh));
    }

    // Many boxes and cameras against the test of the corners.
    int outside = 0, inside = 0;
    float camera[3] = { 0, 0, 0 };
    for (int round = 0; round < 40000; round++) {
        if (round % 50 == 0) {
            camera[0] = (Next() - 0.5f) * 600.0f;
            camera[1] = Next() * 20.0f;
            camera[2] = (Next() - 0.5f) * 600.0f;
            ClipMatrix(camera[0], camera[1], camera[2], Next() * 6.2831853f, 0.6f + Next(), 0.8f + Next() * 1.5f,
                       0.1f + Next(), 100.0f + Next() * 400.0f, m);
        }
        float low[3], high[3];
        for (int k = 0; k < 3; k++) {
            const float centre = camera[k] + (Next() - 0.5f) * (k == 1 ? 60.0f : 500.0f);
            const float half = Next() * Next() * (round % 7 == 0 ? 200.0f : 15.0f);
            low[k] = centre - half;
            high[k] = centre + half;
        }
        float margin;
        const bool expected = CornersOutsideView(m, low, high, &margin);
        if (margin < 0.01f)
            continue;
        REQUIRE(Gfx_BoxOutsideView(m, low, high) == expected);
        outside += expected;
        inside += !expected;
    }
    REQUIRE(outside > 4000 && inside > 4000);

    printf("ok\n");
    return 0;
}
