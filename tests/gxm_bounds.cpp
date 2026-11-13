// The eye distance bound of a draw's vertices (src/render/gxm_bounds.h): the
// box of known positions, and its farthest corner after a matrix. No GXM.

#include "render/gxm_bounds.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

// A vertex as the renderer has it: the position first, then other data.
struct Vertex {
    float position[3];
    float other[9];
};

bool Near(float a, float b)
{
    return fabsf(a - b) <= 1e-3f * (1.0f + fabsf(b));
}

float EyeDistance(const float p[3], const float m[4][4])
{
    float squared = 0.0f;
    for (int k = 0; k < 3; k++) {
        const float e = p[0] * m[0][k] + p[1] * m[1][k] + p[2] * m[2][k] + m[3][k];
        squared += e * e;
    }
    return sqrtf(squared);
}

} // namespace

int main()
{
    Vertex vertices[4] = {};
    const float positions[4][3] = { { 1, 2, 3 }, { -4, 0, 5 }, { 2, -6, 1 }, { 0, 1, -2 } };
    for (int i = 0; i < 4; i++)
        memcpy(vertices[i].position, positions[i], sizeof(positions[i]));

    // The box of the positions, by the stride of the vertex.
    const GxmBounds b = Gxm_VertexBounds(vertices, sizeof(Vertex), 4);
    REQUIRE(b.low[0] == -4 && b.low[1] == -6 && b.low[2] == -2);
    REQUIRE(b.high[0] == 2 && b.high[1] == 2 && b.high[2] == 5);
    // One vertex: a point.
    const GxmBounds point = Gxm_VertexBounds(&vertices[1], sizeof(Vertex), 1);
    REQUIRE(point.low[0] == -4 && point.high[0] == -4 && point.low[2] == 5 && point.high[2] == 5);

    // The eye at the origin, no transform: the farthest corner of the box.
    const float identity[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    REQUIRE(Near(Gxm_EyeDistanceBound(b, identity), sqrtf(16 + 36 + 25)));
    REQUIRE(Near(Gxm_EyeDistanceBound(point, identity), sqrtf(16 + 0 + 25)));
    // A translation (row 3) moves the box away from the eye.
    const float away[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 100, 1 } };
    REQUIRE(Near(Gxm_EyeDistanceBound(point, away), sqrtf(16 + 105 * 105)));

    // No vertex is farther than the bound: a rotation about y with a scale of
    // 2 and a translation.
    const float c = cosf(0.7f) * 2.0f, s = sinf(0.7f) * 2.0f;
    const float turned[4][4] = { { c, 0, -s, 0 }, { 0, 2, 0, 0 }, { s, 0, c, 0 }, { 3, -7, 40, 1 } };
    const float bound = Gxm_EyeDistanceBound(b, turned);
    float farthest = 0.0f;
    for (int i = 0; i < 4; i++) {
        const float d = EyeDistance(positions[i], turned);
        REQUIRE(d <= bound * 1.0001f);
        farthest = d > farthest ? d : farthest;
    }
    // The bound is not below the farthest corner of the box, and not far
    // above it: at most the box's half diagonal after the matrix's scale.
    float cornerMax = 0.0f;
    for (int k = 0; k < 8; k++) {
        const float p[3] = { (k & 1) ? b.high[0] : b.low[0], (k & 2) ? b.high[1] : b.low[1],
                             (k & 4) ? b.high[2] : b.low[2] };
        const float d = EyeDistance(p, turned);
        cornerMax = d > cornerMax ? d : cornerMax;
    }
    REQUIRE(bound >= farthest && bound >= cornerMax * 0.9999f);
    const float halfDiagonal = 0.5f * sqrtf(6 * 6 + 8 * 8 + 7 * 7) * 2.0f;
    REQUIRE(bound <= cornerMax + halfDiagonal);

    printf("gxm_bounds: ok\n");
    return 0;
}
