// The viewport conversion of the GXM executor (render/gxm_viewport.h): clip
// space corners land on the corners of the rectangle, top row first.

#include "render/gxm_viewport.h"

#include <cmath>
#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

// Where GXM puts a clip-space point (after the divide by w).
void ToWindow(const GxmViewport &v, float x, float y, float z, float out[3])
{
    out[0] = v.xOffset + v.xScale * x;
    out[1] = v.yOffset + v.yScale * y;
    out[2] = v.zOffset + v.zScale * z;
}

} // namespace

int main()
{
    float p[3];

    // The whole 960x544 back buffer: clip (-1, 1) is the top left pixel corner.
    GxmViewport full = Gxm_ViewportFromRect(0, 0, 960, 544, 0, 1);
    ToWindow(full, -1, 1, 0, p);
    REQUIRE(Near(p[0], 0) && Near(p[1], 0) && Near(p[2], 0));
    ToWindow(full, 1, -1, 1, p);
    REQUIRE(Near(p[0], 960) && Near(p[1], 544) && Near(p[2], 1));

    // A part of the target with a depth range (the game's 3D view under the HUD).
    GxmViewport part = Gxm_ViewportFromRect(100, 50, 400, 300, 0.25f, 0.75f);
    ToWindow(part, -1, 1, 0, p);
    REQUIRE(Near(p[0], 100) && Near(p[1], 50) && Near(p[2], 0.25f));
    ToWindow(part, 1, -1, 1, p);
    REQUIRE(Near(p[0], 500) && Near(p[1], 350) && Near(p[2], 0.75f));
    ToWindow(part, 0, 0, 0.5f, p);
    REQUIRE(Near(p[0], 300) && Near(p[1], 200) && Near(p[2], 0.5f));

    printf("gxm_viewport: ok\n");
    return 0;
}
