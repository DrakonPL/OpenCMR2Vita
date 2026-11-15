// PORT: how far from the eye a part of the ground or of the static objects
// is (Game_DrawMergedGroundParts, Game_DrawObjectRunMerged). The renderer
// draws a draw whose vertices are all nearer than the start of the distance
// fade with the program without the fade: the fade changes no pixel there,
// and it is the largest part of the program. The game put the parts of one
// texture into one draw, the near ones with the far ones, and the renderer
// took the box of all vertices between the first and the last of them: each
// pixel of the ground then had the fade. The parts go into groups by the
// side of the fade's start, and a draw comes with the reach of its parts.
// No game state here: tests/fade_reach.cpp checks it on the PC.
#ifndef OPENCMR2_FADE_REACH_H
#define OPENCMR2_FADE_REACH_H

#include <math.h>

// The eye in the space of the vertices. A distance there times scale is not
// below the distance in eye space.
struct FadeEye {
    float x, y, z;
    float scale;
};

// worldView: the world matrix times the view matrix, 16 numbers, a Direct3D
// row-vector matrix. The eye is the point that the matrix moves to the
// origin. The scale is a bound of how much the matrix makes a distance
// longer: the root of the largest row sum of the products of its axes. For
// a turn with one scale that is the scale. The matrices of the game come
// from fixed-point numbers: their axes are a little off a turn, and the
// bound is then a little above 1. False: the matrix has no inverse. The
// caller then knows no reach.
inline bool FadeEye_Set(FadeEye *e, const float *m)
{
    const float *r0 = m, *r1 = m + 4, *r2 = m + 8, *t = m + 12;
    // The inverse of the three axes, by cofactors.
    const float c00 = r1[1] * r2[2] - r1[2] * r2[1], c01 = r1[2] * r2[0] - r1[0] * r2[2],
                c02 = r1[0] * r2[1] - r1[1] * r2[0];
    const float det = r0[0] * c00 + r0[1] * c01 + r0[2] * c02;
    const float s0 = r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2];
    const float s1 = r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2];
    const float s2 = r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2];
    // A matrix that makes a volume much smaller than its axes are long has
    // no inverse to speak of.
    if (!(fabsf(det) > 1e-6f * sqrtf(s0 * s1 * s2)))
        return false;
    const float inverse[3][3] = {
        { c00 / det, (r0[2] * r2[1] - r0[1] * r2[2]) / det, (r0[1] * r1[2] - r0[2] * r1[1]) / det },
        { c01 / det, (r0[0] * r2[2] - r0[2] * r2[0]) / det, (r0[2] * r1[0] - r0[0] * r1[2]) / det },
        { c02 / det, (r0[1] * r2[0] - r0[0] * r2[1]) / det, (r0[0] * r1[1] - r0[1] * r1[0]) / det },
    };
    e->x = -(t[0] * inverse[0][0] + t[1] * inverse[1][0] + t[2] * inverse[2][0]);
    e->y = -(t[0] * inverse[0][1] + t[1] * inverse[1][1] + t[2] * inverse[2][1]);
    e->z = -(t[0] * inverse[0][2] + t[1] * inverse[1][2] + t[2] * inverse[2][2]);
    const float d01 = fabsf(r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2]);
    const float d02 = fabsf(r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2]);
    const float d12 = fabsf(r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2]);
    const float sum0 = s0 + d01 + d02, sum1 = d01 + s1 + d12, sum2 = d02 + d12 + s2;
    const float largest = sum0 > sum1 ? (sum0 > sum2 ? sum0 : sum2) : (sum1 > sum2 ? sum1 : sum2);
    e->scale = sqrtf(largest);
    return true;
}

// The distance from the eye, in eye space, of the farthest point of a box in
// the space of the vertices: not below its farthest corner, and a little
// more (the rounding of the matrices and of this sum never gives less than
// a vertex has in the vertex program).
inline float FadeEye_BoxReach(const FadeEye &e, const float low[3], const float high[3])
{
    const float eye[3] = { e.x, e.y, e.z };
    float squared = 0.0f;
    for (int k = 0; k < 3; k++) {
        const float a = fabsf(low[k] - eye[k]), b = fabsf(high[k] - eye[k]);
        const float d = a > b ? a : b;
        squared += d * d;
    }
    return sqrtf(squared) * e.scale * 1.0002f + 0.05f;
}

#endif
