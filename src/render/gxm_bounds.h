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

// An upper bound of the distance from the eye of a point of the box, after
// worldView (a Direct3D row-vector matrix, its rows 0 to 3: the eye space of
// fixed3d_v.cg). It takes the box's centre in eye space and, on each eye axis,
// the reach of the box's half sides from it. That is the farthest corner of
// the eye-space box around the turned box: exact for a matrix that does not
// turn the box, a little above the farthest vertex otherwise, never below it.
// It costs a quarter of the eight corners, for each 3D draw.
inline float Gxm_EyeDistanceBound(const GxmBounds &b, const float worldView[4][4])
{
    float centre[3], half[3], squared = 0.0f;
    for (int k = 0; k < 3; k++) {
        centre[k] = (b.low[k] + b.high[k]) * 0.5f;
        half[k] = (b.high[k] - b.low[k]) * 0.5f;
    }
    for (int k = 0; k < 3; k++) {
        const float eye = centre[0] * worldView[0][k] + centre[1] * worldView[1][k] + centre[2] * worldView[2][k] +
                          worldView[3][k];
        const float reach = half[0] * fabsf(worldView[0][k]) + half[1] * fabsf(worldView[1][k]) +
                            half[2] * fabsf(worldView[2][k]);
        const float far = fabsf(eye) + reach;
        squared += far * far;
    }
    return sqrtf(squared);
}

// The distance from the eye of the farthest of count vertices (positions as
// for Gxm_VertexBounds) after worldView. Exact, at the cost of each vertex:
// for the frame's own vertices, which no box is kept of. A run of joined
// quads is as far as its farthest quad, not as the box around all of them.
inline float Gxm_FarthestEyeDistance(const void *vertices, size_t stride, unsigned count, const float worldView[4][4])
{
    const unsigned char *p = (const unsigned char *)vertices;
    float farthest = 0.0f;
    for (unsigned i = 0; i < count; i++) {
        float v[3];
        memcpy(v, p + (size_t)i * stride, sizeof(v));
        float squared = 0.0f;
        for (int k = 0; k < 3; k++) {
            const float eye = v[0] * worldView[0][k] + v[1] * worldView[1][k] + v[2] * worldView[2][k] + worldView[3][k];
            squared += eye * eye;
        }
        farthest = squared > farthest ? squared : farthest;
    }
    return sqrtf(farthest);
}

#endif
