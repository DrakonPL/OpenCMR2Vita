// How far from the eye a box of vertices is (game/FadeReach.h): never less
// than its farthest corner after the matrix, and for the matrix of a view
// no more than a little above it.

#include "FadeReach.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static double Random(double low, double high)
{
    return low + (high - low) * (rand() / (double)RAND_MAX);
}

// A turn about the three axes, one scale and a move: 16 numbers, row vectors.
static void Matrix(double a, double b, double c, double scale, const double move[3], float out[16])
{
    const double ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b), cc = cos(c), sc = sin(c);
    const double r[3][3] = {
        { cb * cc, cb * sc, -sb },
        { sa * sb * cc - ca * sc, sa * sb * sc + ca * cc, sa * cb },
        { ca * sb * cc + sa * sc, ca * sb * sc - sa * cc, ca * cb },
    };
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++)
            out[i * 4 + k] = (float)(r[i][k] * scale);
        out[i * 4 + 3] = 0.0f;
        out[12 + i] = (float)move[i];
    }
    out[15] = 1.0f;
}

// The farthest corner of the box after the matrix, as the vertex program
// has it (eye = position * worldView).
static double Farthest(const float low[3], const float high[3], const float m[16])
{
    double farthest = 0.0;
    for (int corner = 0; corner < 8; corner++) {
        const double p[3] = { corner & 1 ? high[0] : low[0], corner & 2 ? high[1] : low[1],
                              corner & 4 ? high[2] : low[2] };
        double squared = 0.0;
        for (int k = 0; k < 3; k++) {
            const double eye = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + m[12 + k];
            squared += eye * eye;
        }
        farthest = squared > farthest ? squared : farthest;
    }
    return sqrt(farthest);
}

int main()
{
    srand(7);
    // The identity: the eye is at the origin.
    {
        const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        FadeEye e;
        REQUIRE(FadeEye_Set(&e, identity));
        REQUIRE(e.x == 0.0f && e.y == 0.0f && e.z == 0.0f && e.scale == 1.0f);
        const float low[3] = { -1.0f, 2.0f, 3.0f }, high[3] = { 4.0f, 3.0f, 12.0f };
        const float reach = FadeEye_BoxReach(e, low, high);
        REQUIRE(reach >= 13.0f && reach < 13.06f);    // the corner (4, 3, 12)
        // A box around the eye: its farthest corner, not its nearest side.
        const float around[3] = { -30.0f, -1.0f, -1.0f }, to[3] = { 2.0f, 1.0f, 40.0f };
        const float far = FadeEye_BoxReach(e, around, to);
        REQUIRE(far >= 50.0f && far < 50.1f);
    }
    // Views of a stage: a turn, a move of some thousands of units, with and
    // without one scale. Boxes near and far.
    for (int n = 0; n < 20000; n++) {
        const double move[3] = { Random(-6000, 6000), Random(-300, 300), Random(-6000, 6000) };
        const double scale = n % 3 == 0 ? Random(0.25, 4.0) : 1.0;
        float m[16];
        Matrix(Random(-3.2, 3.2), Random(-3.2, 3.2), Random(-3.2, 3.2), scale, move, m);
        FadeEye e;
        REQUIRE(FadeEye_Set(&e, m));
        // Around the eye: the point that the matrix moves to the origin.
        float low[3], high[3];
        const double size = n % 2 == 0 ? 600.0 : 40.0;
        const float eye[3] = { e.x, e.y, e.z };
        for (int k = 0; k < 3; k++) {
            const double a = eye[k] + Random(-size, size) / scale, b = eye[k] + Random(-size, size) / scale;
            low[k] = (float)(a < b ? a : b);
            high[k] = (float)(a < b ? b : a);
        }
        const double exact = Farthest(low, high, m);
        const float reach = FadeEye_BoxReach(e, low, high);
        REQUIRE(reach >= exact);
        REQUIRE(reach <= exact * 1.0005 + 0.06);
    }
    // The matrices of the game, from fixed-point numbers: each number a
    // little off (1 / 65,536), so the axes are not those of a turn. The
    // reach is still not below the farthest corner, and close to it.
    for (int n = 0; n < 20000; n++) {
        const double move[3] = { Random(-6000, 6000), Random(-300, 300), Random(-6000, 6000) };
        float m[16];
        Matrix(Random(-3.2, 3.2), Random(-3.2, 3.2), Random(-3.2, 3.2), 1.0, move, m);
        for (int i = 0; i < 12; i++)
            if (i % 4 != 3)
                m[i] = (float)(floor(m[i] * 65536.0) / 65536.0);
        FadeEye e;
        REQUIRE(FadeEye_Set(&e, m));
        float low[3], high[3];
        const float eye[3] = { e.x, e.y, e.z };
        for (int k = 0; k < 3; k++) {
            const double a = eye[k] + Random(-600, 600), b = eye[k] + Random(-600, 600);
            low[k] = (float)(a < b ? a : b);
            high[k] = (float)(a < b ? b : a);
        }
        const double exact = Farthest(low, high, m);
        const float reach = FadeEye_BoxReach(e, low, high);
        REQUIRE(reach >= exact);
        REQUIRE(reach <= exact * 1.001 + 0.06);
    }
    // A matrix that does not keep the ratio of distances (an axis longer,
    // an axis toward another): the reach is a bound, never below a corner.
    for (int n = 0; n < 20000; n++) {
        const double move[3] = { Random(-2000, 2000), Random(-300, 300), Random(-2000, 2000) };
        float m[16];
        Matrix(Random(-3.2, 3.2), Random(-3.2, 3.2), Random(-3.2, 3.2), 1.0, move, m);
        const float stretch = (float)Random(0.5, 3.0), shear = (float)Random(-0.8, 0.8);
        for (int k = 0; k < 3; k++) {
            m[4 + k] *= stretch;
            m[8 + k] += shear * m[k];
        }
        FadeEye e;
        REQUIRE(FadeEye_Set(&e, m));
        float low[3], high[3];
        const float eye[3] = { e.x, e.y, e.z };
        for (int k = 0; k < 3; k++) {
            const double a = eye[k] + Random(-300, 300), b = eye[k] + Random(-300, 300);
            low[k] = (float)(a < b ? a : b);
            high[k] = (float)(a < b ? b : a);
        }
        REQUIRE(FadeEye_BoxReach(e, low, high) >= Farthest(low, high, m));
    }
    // A matrix with no inverse: no eye.
    {
        const double move[3] = { 10.0, 20.0, 30.0 };
        float m[16], flat[16];
        FadeEye e;
        Matrix(0.3, 0.7, -1.1, 1.0, move, m);
        REQUIRE(FadeEye_Set(&e, m));
        for (int i = 0; i < 16; i++)
            flat[i] = m[i];
        for (int k = 0; k < 3; k++)
            flat[8 + k] = m[k] + m[4 + k];      // the third axis in the plane of the others
        REQUIRE(!FadeEye_Set(&e, flat));
        for (int k = 0; k < 3; k++)
            flat[k] = flat[4 + k] = flat[8 + k] = 0.0f;
        REQUIRE(!FadeEye_Set(&e, flat));
    }
    printf("ok\n");
    return 0;
}
