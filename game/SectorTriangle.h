// PORT: whether the ground rectangle of a sector and the view triangle
// overlap, for the walk over the sector grid of a frame
// (Sector_CullGridAroundViewNode). The test by separating axes took seven
// axes with seven corners on each for each sector of the walk, in integers
// of 64 bits: 1.5 to 1.8 ms of a frame on the PS Vita. The triangle is the
// same for each sector, so its part is made once (SectorTriangle_Set). The
// axes of the rectangle are x and z: a compare of two boxes. Most sectors of
// the walk are apart from the box of the triangle. The result for each
// sector is the same. No game state here: tests/sector_triangle.cpp checks
// it on the PC against the test it replaces.
#ifndef OPENCMR2_SECTOR_TRIANGLE_H
#define OPENCMR2_SECTOR_TRIANGLE_H

#include <stdint.h>

// The margin around the rectangle of a sector, for the trees and the signs
// near its edge: 16 in 16.16 fixed point.
#define SECTOR_TRIANGLE_MARGIN 0x100000

struct SectorTriangle {
    int64_t minX, maxX, minZ, maxZ;     // the box of the triangle
    int64_t axisX[3], axisZ[3];         // the normal of each edge
    int64_t low[3], high[3];            // the triangle along each normal
};

// From the corners of the triangle: x and z of each, 16.16 fixed point.
inline void SectorTriangle_Set(SectorTriangle *t, const int *tri)
{
    t->minX = t->maxX = tri[0];
    t->minZ = t->maxZ = tri[1];
    for (int i = 1; i < 3; i++) {
        const int64_t x = tri[i * 2], z = tri[i * 2 + 1];
        t->minX = x < t->minX ? x : t->minX;
        t->maxX = x > t->maxX ? x : t->maxX;
        t->minZ = z < t->minZ ? z : t->minZ;
        t->maxZ = z > t->maxZ ? z : t->maxZ;
    }
    for (int e = 0; e < 3; e++) {
        const int n = e == 2 ? 0 : e + 1;
        const int64_t ax = (int64_t)tri[e * 2 + 1] - tri[n * 2 + 1];
        const int64_t az = (int64_t)tri[n * 2] - tri[e * 2];
        t->axisX[e] = ax;
        t->axisZ[e] = az;
        for (int i = 0; i < 3; i++) {
            const int64_t d = tri[i * 2] * ax + tri[i * 2 + 1] * az;
            if (i == 0 || d < t->low[e])
                t->low[e] = d;
            if (i == 0 || d > t->high[e])
                t->high[e] = d;
        }
    }
}

// The rectangle of a sector with the margin: the box of its four corners.
struct SectorRectangle {
    int64_t minX, maxX, minZ, maxZ;
};

inline SectorRectangle SectorRectangle_Of(const int bounds[4][2])
{
    SectorRectangle r = { bounds[0][0], bounds[0][0], bounds[0][1], bounds[0][1] };
    for (int i = 1; i < 4; i++) {
        r.minX = bounds[i][0] < r.minX ? bounds[i][0] : r.minX;
        r.maxX = bounds[i][0] > r.maxX ? bounds[i][0] : r.maxX;
        r.minZ = bounds[i][1] < r.minZ ? bounds[i][1] : r.minZ;
        r.maxZ = bounds[i][1] > r.maxZ ? bounds[i][1] : r.maxZ;
    }
    r.minX -= SECTOR_TRIANGLE_MARGIN;
    r.maxX += SECTOR_TRIANGLE_MARGIN;
    r.minZ -= SECTOR_TRIANGLE_MARGIN;
    r.maxZ += SECTOR_TRIANGLE_MARGIN;
    return r;
}

// Whether the rectangle is apart from the box of the triangle. No corner of
// the sector is then in the triangle, and the two do not overlap.
inline bool SectorTriangle_BoxApart(const SectorTriangle &t, const SectorRectangle &r)
{
    return r.maxX < t.minX || t.maxX < r.minX || r.maxZ < t.minZ || t.maxZ < r.minZ;
}

// Whether the rectangle and the triangle overlap: neither is wholly on one
// side of an edge of the other. Along a normal of the triangle, the least
// and the greatest corner of the rectangle come from the signs of the normal.
inline bool SectorTriangle_Touches(const SectorTriangle &t, const SectorRectangle &r)
{
    if (SectorTriangle_BoxApart(t, r))
        return false;
    for (int e = 0; e < 3; e++) {
        const int64_t ax = t.axisX[e], az = t.axisZ[e];
        const int64_t low = (ax >= 0 ? r.minX : r.maxX) * ax + (az >= 0 ? r.minZ : r.maxZ) * az;
        const int64_t high = (ax >= 0 ? r.maxX : r.minX) * ax + (az >= 0 ? r.maxZ : r.minZ) * az;
        if (high < t.low[e] || t.high[e] < low)
            return false;
    }
    return true;
}

#endif
