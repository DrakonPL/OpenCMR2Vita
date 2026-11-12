// How far from the eye the vertices of a draw can be. The GXM executor
// (gfx_gxm.cpp) draws without the distance fade where no vertex reaches the
// fade's start: the fade then changes no pixel, and its part of the fragment
// program is the largest. No GXM: tests/gxm_bounds.cpp checks it.
#ifndef OPENCMR2_GXM_BOUNDS_H
#define OPENCMR2_GXM_BOUNDS_H

#include <math.h>
#include <stddef.h>
#include <string.h>

// The box around the positions of a vertex range, in their own space.
struct GxmBounds {
    float low[3] = {};
    float high[3] = {};
};

// The box of count positions: three floats at the start of each vertex of
// stride bytes. count is at least 1.
inline GxmBounds Gxm_VertexBounds(const void *vertices, size_t stride, unsigned count)
{
    GxmBounds b;
    memcpy(b.low, vertices, sizeof(b.low));
    memcpy(b.high, vertices, sizeof(b.high));
    const unsigned char *p = (const unsigned char *)vertices;
    for (unsigned i = 1; i < count; i++) {
        float v[3];
        memcpy(v, p + (size_t)i * stride, sizeof(v));
        for (int k = 0; k < 3; k++) {
            b.low[k] = v[k] < b.low[k] ? v[k] : b.low[k];
            b.high[k] = v[k] > b.high[k] ? v[k] : b.high[k];
        }
    }
    return b;
}

// The largest distance from the eye of a point of the box, after worldView
// (a Direct3D row-vector matrix, its rows 0 to 3: the eye space of
// fixed3d_v.cg). The distance is largest at a corner of the box, so no vertex
// in the box is farther.
inline float Gxm_FarthestEyeDistance(const GxmBounds &b, const float worldView[4][4])
{
    float farthest = 0.0f;
    for (int corner = 0; corner < 8; corner++) {
        const float x = (corner & 1) ? b.high[0] : b.low[0];
        const float y = (corner & 2) ? b.high[1] : b.low[1];
        const float z = (corner & 4) ? b.high[2] : b.low[2];
        float squared = 0.0f;
        for (int k = 0; k < 3; k++) {
            const float e = x * worldView[0][k] + y * worldView[1][k] + z * worldView[2][k] + worldView[3][k];
            squared += e * e;
        }
        farthest = squared > farthest ? squared : farthest;
    }
    return sqrtf(farthest);
}

#endif
