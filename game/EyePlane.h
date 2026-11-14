// PORT: the plane of the eye of a view, for the triangles that the game
// makes in world space in each frame. The tyre marks are some 1,450
// triangles, and in a race most of them are behind the camera. A triangle
// with each corner behind the eye draws nothing with a perspective
// projection, so the game does not make it. No game state here:
// tests/eye_plane.cpp checks it on the PC.
#ifndef OPENCMR2_EYE_PLANE_H
#define OPENCMR2_EYE_PLANE_H

#include <math.h>

// The w of the clip space of a position in 16.16 fixed point, times 2^44:
// x * this.x + y * this.y + z * this.z + this.w. In integers: the PS Vita
// multiplies two integers faster than it makes a float of each coordinate.
struct EyePlane {
    int x, y, z;
    long long w;
};

// From the view and the projection matrices (16 floats each, in the rows of
// Direct3D: a position is a row vector). The w of the clip space is the
// fourth column of view * projection. Each perspective projection of
// Direct3D has the same fourth column (0, 0, 1, 0), so the plane holds for
// a projection with another field of view or another far plane. A
// projection without perspective gives w = 1: no position is behind. The
// same for a matrix that the integers cannot hold: no position is behind.
inline EyePlane EyePlane_Of(const float *view, const float *projection)
{
    double column[4];
    for (int r = 0; r < 4; r++)
        column[r] = (double)view[r * 4] * projection[3] + (double)view[r * 4 + 1] * projection[7] +
                    (double)view[r * 4 + 2] * projection[11] + (double)view[r * 4 + 3] * projection[15];
    EyePlane e = { 0, 0, 0, 1 };
    if (!(fabs(column[0]) <= 1.5 && fabs(column[1]) <= 1.5 && fabs(column[2]) <= 1.5 && fabs(column[3]) <= 131072.0))
        return e;
    const double scale = 268435456.0; // 2^28
    e.x = (int)llround(column[0] * scale);
    e.y = (int)llround(column[1] * scale);
    e.z = (int)llround(column[2] * scale);
    e.w = llround(column[3] * scale * 65536.0);
    return e;
}

// Whether a position (16.16 fixed point) is behind the eye. The near plane
// of each view is 0.1 or more in front of the eye. The rounding of the
// plane (some 0.0001 for a position 2,000 from the origin, most of it in
// the floats of the view matrix) does not reach a position that the view
// shows.
inline bool EyePlane_Behind(const EyePlane &e, int x, int y, int z)
{
    return (long long)x * e.x + (long long)y * e.y + (long long)z * e.z + e.w < 0;
}

#endif
