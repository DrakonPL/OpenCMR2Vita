// The arithmetic of the small 3D draws in world space, for the recorder
// (gfx_core.cpp) and for the batches of small draws (gfx_small_batch.cpp).
// Both use these functions, so a draw of a batch gets the same vertices, the
// same view test and the same distance as the same draw made alone.
#ifndef OPENCMR2_GFX_SMALL_DRAW_H
#define OPENCMR2_GFX_SMALL_DRAW_H

#include "render/gfx_internal.h"

#include <math.h>
#include <string.h>

// A draw that joins the draw before has at most this many vertices with it
// (the indices are 16 bits wide).
constexpr DWORD kGfxJoinedVertices = 4096;
// A 3D draw of at most this many vertices is recorded in world space.
constexpr DWORD kGfxSmallDrawVertices = 16;

// Whether a world matrix takes a small draw into world space: affine, and
// not the identity (identity: the draw is in world space already).
inline bool Gfx_SmallDrawWorld(const GfxMatrix &m, const GfxMatrix &identity)
{
    if (m._14 != 0.0f || m._24 != 0.0f || m._34 != 0.0f || m._44 != 1.0f)
        return false;
    return memcmp(&m, &identity, sizeof(m)) != 0;
}

// The vertices through an affine world matrix: positions through all of it,
// normals through its upper 3x3 part (as fixed3d_v.cg does it, with the view
// matrix after).
inline void Gfx_SmallDrawToWorld(const GfxMatrix &m, const GfxVertex *in, DWORD count, GfxVertex *out)
{
    for (DWORD i = 0; i < count; i++) {
        const GfxVertex &v = in[i];
        GfxVertex &o = out[i];
        o = v;
        o.x = v.x * m._11 + v.y * m._21 + v.z * m._31 + m._41;
        o.y = v.x * m._12 + v.y * m._22 + v.z * m._32 + m._42;
        o.z = v.x * m._13 + v.y * m._23 + v.z * m._33 + m._43;
        o.nx = v.nx * m._11 + v.ny * m._21 + v.nz * m._31;
        o.ny = v.nx * m._12 + v.ny * m._22 + v.nz * m._32;
        o.nz = v.nx * m._13 + v.ny * m._23 + v.nz * m._33;
    }
}

// The matrix from a draw's own space to the clip space: world, view and
// projection, in this order.
inline void Gfx_ClipMatrixOf(const GfxMatrix &world, const GfxMatrix &view, const GfxMatrix &projection,
                             float out[16])
{
    float wv[16];
    const float *a = &world._11, *v = &view._11, *p = &projection._11;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            wv[r * 4 + k] = a[r * 4] * v[k] + a[r * 4 + 1] * v[4 + k] + a[r * 4 + 2] * v[8 + k] + a[r * 4 + 3] * v[12 + k];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            out[r * 4 + k] = wv[r * 4] * p[k] + wv[r * 4 + 1] * p[4 + k] + wv[r * 4 + 2] * p[8 + k] + wv[r * 4 + 3] * p[12 + k];
}

// The planes of the view a point is outside of, in the clip space of
// Direct3D: -w <= x <= w, -w <= y <= w, 0 <= z <= w. A draw whose points are
// all outside one plane draws nothing.
inline unsigned Gfx_OutsidePlanes(const float *m, float x, float y, float z)
{
    const float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
    const float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
    const float cz = x * m[2] + y * m[6] + z * m[10] + m[14];
    const float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
    return (cx < -cw ? 1u : 0u) | (cx > cw ? 2u : 0u) | (cy < -cw ? 4u : 0u) | (cy > cw ? 8u : 0u) |
           (cz < 0.0f ? 16u : 0u) | (cz > cw ? 32u : 0u);
}

inline bool Gfx_PointsOutsideView(const float *m, const GfxVertex *vertices, DWORD count)
{
    unsigned all = 63;
    for (DWORD i = 0; i < count && all != 0; i++)
        all &= Gfx_OutsidePlanes(m, vertices[i].x, vertices[i].y, vertices[i].z);
    return all != 0;
}

// The eye distance of the farthest vertex of a small draw in world space
// (its world matrix is the identity: the view matrix gives the eye space).
inline float Gfx_SmallDrawReach(const GfxMatrix &v, const GfxVertex *vertices, DWORD count)
{
    float farthest = 0.0f;
    for (DWORD i = 0; i < count; i++) {
        const GfxVertex &p = vertices[i];
        const float x = p.x * v._11 + p.y * v._21 + p.z * v._31 + v._41;
        const float y = p.x * v._12 + p.y * v._22 + p.z * v._32 + v._42;
        const float z = p.x * v._13 + p.y * v._23 + p.z * v._33 + v._43;
        const float squared = x * x + y * y + z * z;
        farthest = squared > farthest ? squared : farthest;
    }
    return sqrtf(farthest);
}

// The box of the positions of count vertices (count is at least 1).
inline void Gfx_VertexBox(const GfxVertex *vertices, DWORD count, float low[3], float high[3])
{
    // In locals: the outputs can alias the vertices, and the compiler kept
    // them in memory for each vertex (the tyre marks: 4,400 vertices in a
    // frame).
    float lx = vertices[0].x, ly = vertices[0].y, lz = vertices[0].z;
    float hx = lx, hy = ly, hz = lz;
    for (DWORD i = 1; i < count; i++) {
        const GfxVertex &p = vertices[i];
        lx = p.x < lx ? p.x : lx;
        hx = p.x > hx ? p.x : hx;
        ly = p.y < ly ? p.y : ly;
        hy = p.y > hy ? p.y : hy;
        lz = p.z < lz ? p.z : lz;
        hz = p.z > hz ? p.z : hz;
    }
    low[0] = lx;
    low[1] = ly;
    low[2] = lz;
    high[0] = hx;
    high[1] = hy;
    high[2] = hz;
}

// Whether the texture coordinates of a draw are inside its texture: the
// atlas then gives each pixel what the texture gave.
inline bool Gfx_AtlasCoordinatesFit(const GfxVertex *vertices, DWORD count)
{
    for (DWORD i = 0; i < count; i++)
        if (!(vertices[i].u0 >= 0.0f && vertices[i].u0 <= 1.0f && vertices[i].v0 >= 0.0f && vertices[i].v0 <= 1.0f))
            return false;
    return true;
}

// The texture coordinates of a draw moved into the place of its texture in
// the atlas: the texel x, y of the atlas where the texture starts, the size
// of the texture, and 1 / the size of the atlas.
inline void Gfx_AtlasRemap(GfxVertex *vertices, DWORD count, int x, int y, int width, int height, float scale)
{
    for (DWORD i = 0; i < count; i++) {
        vertices[i].u0 = ((float)x + vertices[i].u0 * (float)width) * scale;
        vertices[i].v0 = ((float)y + vertices[i].v0 * (float)height) * scale;
    }
}

#endif
