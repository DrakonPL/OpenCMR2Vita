// PORT: whether a tree of a stage is outside the view, from a sphere around
// it. The game queued each tree of the sectors in view, made its matrix,
// sorted it and gave it to the renderer, and the renderer then left out the
// trees outside the view: half of them in a forest. A tree that turns to
// the camera stays within a sphere around its place: the farthest corner of
// its mesh from the mesh's origin, times its greatest scale. A sphere that
// is wholly on the outer side of one plane of the view holds no vertex that
// draws. No game state here: tests/tree_cull.cpp checks it on the PC
// against the view test of the renderer.
#ifndef OPENCMR2_TREE_CULL_H
#define OPENCMR2_TREE_CULL_H

#include <math.h>

// The six planes of a view, four numbers (a, b, c, d) each: a point is
// outside a plane when a * x + b * y + c * z + d is below zero. length: the
// length of (a, b, c).
struct TreeCullView {
    float plane[6][4];
    float length[6];
};

inline void TreeCull_SetView(TreeCullView *view, const float planes[24])
{
    for (int i = 0; i < 6; i++) {
        for (int k = 0; k < 4; k++)
            view->plane[i][k] = planes[i * 4 + k];
        view->length[i] = sqrtf(planes[i * 4] * planes[i * 4] + planes[i * 4 + 1] * planes[i * 4 + 1] +
                                planes[i * 4 + 2] * planes[i * 4 + 2]);
    }
}

// Whether the sphere is wholly on the outer side of one plane. The radius
// counts 1% and 0.01 more, for the rounding of the matrix of the tree and
// of the vertices: a tree that this leaves out has no vertex in the view.
inline bool TreeCull_Outside(const TreeCullView &view, float x, float y, float z, float radius)
{
    const float reach = radius * 1.01f + 0.01f;
    for (int i = 0; i < 6; i++) {
        const float *p = view.plane[i];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < -reach * view.length[i])
            return true;
    }
    return false;
}

// The distance of the farthest corner of a box from the origin.
inline float TreeCull_BoxRadius(const float low[3], const float high[3])
{
    float sum = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float a = fabsf(low[i]), b = fabsf(high[i]);
        const float m = a > b ? a : b;
        sum += m * m;
    }
    return sqrtf(sum);
}

#endif
