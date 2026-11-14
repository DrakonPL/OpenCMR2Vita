// The overlap of the rectangle of a sector and the view triangle
// (game/SectorTriangle.h): the same answer as the test by separating axes
// that it replaces, and a rectangle apart from the box of the triangle has
// no corner in the box.

#include "SectorTriangle.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// The test that SectorTriangle.h replaces: each edge of each shape as an
// axis, each corner of both shapes along it.
bool TouchesBySevenAxes(const int bounds[4][2], const int *tri)
{
    const int64_t margin = 0x100000;
    int64_t minX = bounds[0][0], maxX = bounds[0][0], minZ = bounds[0][1], maxZ = bounds[0][1];
    for (int i = 1; i < 4; i++) {
        minX = bounds[i][0] < minX ? bounds[i][0] : minX;
        maxX = bounds[i][0] > maxX ? bounds[i][0] : maxX;
        minZ = bounds[i][1] < minZ ? bounds[i][1] : minZ;
        maxZ = bounds[i][1] > maxZ ? bounds[i][1] : maxZ;
    }
    minX -= margin;
    maxX += margin;
    minZ -= margin;
    maxZ += margin;
    const int64_t shapes[2][4][2] = {
        { { minX, minZ }, { maxX, minZ }, { maxX, maxZ }, { minX, maxZ } },
        { { tri[0], tri[1] }, { tri[2], tri[3] }, { tri[4], tri[5] }, { tri[4], tri[5] } },
    };
    const int counts[2] = { 4, 3 };

    for (int shape = 0; shape < 2; shape++) {
        for (int e = 0; e < counts[shape]; e++) {
            const int64_t *a = shapes[shape][e], *b = shapes[shape][(e + 1) % counts[shape]];
            int64_t axisX = a[1] - b[1], axisZ = b[0] - a[0];
            int64_t lo[2], hi[2];
            for (int s = 0; s < 2; s++) {
                for (int i = 0; i < counts[s]; i++) {
                    int64_t d = shapes[s][i][0] * axisX + shapes[s][i][1] * axisZ;
                    if (i == 0 || d < lo[s])
                        lo[s] = d;
                    if (i == 0 || d > hi[s])
                        hi[s] = d;
                }
            }
            if (hi[0] < lo[1] || hi[1] < lo[0])
                return false;
        }
    }
    return true;
}

unsigned s_seed = 4711;

// A number from -range to range, in 16.16 fixed point.
int Next(int range)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (int)((s_seed >> 3) % (unsigned)(2 * range + 1)) - range;
}

const int kOne = 0x10000;

} // namespace

int main()
{
    // A view triangle from the origin to (-300, 300) and (300, 300), and
    // sectors of 40 around it.
    const int tri[6] = { 0, 0, -300 * kOne, 300 * kOne, 300 * kOne, 300 * kOne };
    SectorTriangle view;
    SectorTriangle_Set(&view, tri);
    REQUIRE(view.minX == -300 * (int64_t)kOne && view.maxX == 300 * (int64_t)kOne);
    REQUIRE(view.minZ == 0 && view.maxZ == 300 * (int64_t)kOne);

    const auto sector = [](int x, int z, int bounds[4][2]) {
        const int corners[4][2] = { { x - 20, z - 20 }, { x + 20, z - 20 }, { x + 20, z + 20 }, { x - 20, z + 20 } };
        for (int i = 0; i < 4; i++) {
            bounds[i][0] = corners[i][0] * kOne;
            bounds[i][1] = corners[i][1] * kOne;
        }
    };
    int bounds[4][2];

    // In the middle of the triangle.
    sector(0, 150, bounds);
    REQUIRE(!SectorTriangle_BoxApart(view, SectorRectangle_Of(bounds)));
    REQUIRE(SectorTriangle_Touches(view, SectorRectangle_Of(bounds)));
    // Behind the camera, more than the margin away: apart from the box.
    sector(0, -60, bounds);
    REQUIRE(SectorTriangle_BoxApart(view, SectorRectangle_Of(bounds)));
    REQUIRE(!SectorTriangle_Touches(view, SectorRectangle_Of(bounds)));
    // Behind the camera, within the margin of 16.
    sector(0, -30, bounds);
    REQUIRE(!SectorTriangle_BoxApart(view, SectorRectangle_Of(bounds)));
    REQUIRE(SectorTriangle_Touches(view, SectorRectangle_Of(bounds)));
    // In the box of the triangle and outside its side: beside the camera.
    sector(250, 60, bounds);
    REQUIRE(!SectorTriangle_BoxApart(view, SectorRectangle_Of(bounds)));
    REQUIRE(!SectorTriangle_Touches(view, SectorRectangle_Of(bounds)));
    // Outside the side of the triangle, within the margin of it: no corner
    // of the sector in the triangle, no corner of the triangle in the sector.
    sector(150, 100, bounds);
    REQUIRE(SectorTriangle_Touches(view, SectorRectangle_Of(bounds)));
    // Past the far edge.
    sector(0, 340, bounds);
    REQUIRE(SectorTriangle_BoxApart(view, SectorRectangle_Of(bounds)));

    // Many triangles and sectors against the test of the seven axes. A
    // rectangle apart from the box has no corner in the box.
    int touches = 0, apart = 0, inBoxNoTouch = 0;
    for (int round = 0; round < 60000; round++) {
        int t[6];
        const int cx = Next(2000 * kOne), cz = Next(2000 * kOne);
        t[0] = cx;
        t[1] = cz;
        for (int i = 2; i < 6; i += 2) {
            t[i] = cx + Next(400 * kOne);
            t[i + 1] = cz + Next(400 * kOne);
        }
        if (round % 97 == 0) {
            // No area: the three corners in one place, or in a line.
            t[2] = t[4] = cx + (round % 2 ? 0 : 50 * kOne);
            t[3] = t[5] = cz;
        }
        SectorTriangle_Set(&view, t);
        const int x = cx + Next(280 * kOne), z = cz + Next(280 * kOne);
        const int w = 1 + (Next(40 * kOne) + 40 * kOne) / 2, h = 1 + (Next(40 * kOne) + 40 * kOne) / 2;
        const int corners[4][2] = { { x, z + h }, { x + w, z + h }, { x + w, z }, { x, z } };
        const int first = (round / 3) % 4;
        for (int i = 0; i < 4; i++) {
            bounds[i][0] = corners[(i + first) % 4][0];
            bounds[i][1] = corners[(i + first) % 4][1];
        }
        const SectorRectangle r = SectorRectangle_Of(bounds);
        const bool expected = TouchesBySevenAxes(bounds, t);
        REQUIRE(SectorTriangle_Touches(view, r) == expected);
        if (SectorTriangle_BoxApart(view, r)) {
            REQUIRE(!expected);
            for (int i = 0; i < 4; i++) {
                const bool inBox = bounds[i][0] >= view.minX && bounds[i][0] <= view.maxX &&
                                   bounds[i][1] >= view.minZ && bounds[i][1] <= view.maxZ;
                REQUIRE(!inBox);
            }
            apart++;
        } else if (!expected) {
            inBoxNoTouch++;
        }
        touches += expected;
    }
    REQUIRE(touches > 5000 && apart > 5000 && inBoxNoTouch > 1000);

    printf("ok\n");
    return 0;
}
