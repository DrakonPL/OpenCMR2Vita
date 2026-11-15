// The rows of game/MidpointTexGen.h against the formula of
// Graphics_ProjectMeshVertexMidpoints (game/Graphics.cpp), which wrote the
// second texture coordinates of a car mesh on the CPU: the renderer's
// dot(float4(m, 1), row) gives the same values within float rounding.

#include "MidpointTexGen.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static float Random(float low, float high)
{
    return low + (high - low) * (float)(rand() / (double)RAND_MAX);
}

int main()
{
    for (int trial = 0; trial < 20000; trial++) {
        GfxMatrix t;
        float *m = &t._11;
        for (int i = 0; i < 16; i++)
            m[i] = Random(-2.0f, 2.0f);
        float rows[8];
        MidpointTexGen_Rows(&t, rows);
        const float position[3] = { Random(-3.0f, 3.0f), Random(-3.0f, 3.0f), Random(-3.0f, 3.0f) };
        const float normal[3] = { Random(-1.0f, 1.0f), Random(-1.0f, 1.0f), Random(-1.0f, 1.0f) };

        // Graphics_ProjectMeshVertexMidpoints, as the game writes it.
        const float x = (normal[0] + position[0]) * 0.5f;
        const float y = (normal[1] + position[1]) * 0.5f;
        const float z = (normal[2] + position[2]) * 0.5f;
        const float u = (z * t._31 + y * t._21 + x * t._11 + 1.0f) * 0.5f;
        const float v = (1.0f - (z * t._32 + y * t._22 + x * t._12)) * 0.5f;

        // The vertex program: dot(float4(m, 1), row).
        const float gu = x * rows[0] + y * rows[1] + z * rows[2] + rows[3];
        const float gv = x * rows[4] + y * rows[5] + z * rows[6] + rows[7];
        REQUIRE(std::fabs(gu - u) <= 1e-5f * (1.0f + std::fabs(u)));
        REQUIRE(std::fabs(gv - v) <= 1e-5f * (1.0f + std::fabs(v)));
    }
    printf("midpoint_tex_gen: the rows give the coordinates of the game\n");
    return 0;
}
