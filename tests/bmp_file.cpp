// The BMP header of the device screenshot (src/render/bmp_file.h): the
// fields a BMP reader takes, for a known size. No SDL.

#include "render/bmp_file.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

uint32_t Get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

} // namespace

int main()
{
    uint8_t h[kBmpHeaderBytes];
    Bmp_Header(960, 544, h);
    REQUIRE(h[0] == 'B' && h[1] == 'M');
    REQUIRE(Get32(h + 2) == 54u + 960u * 544u * 4u);
    REQUIRE(Get32(h + 10) == 54u);
    REQUIRE(Get32(h + 14) == 40u);
    REQUIRE((int32_t)Get32(h + 18) == 960);
    // Rows from the top: a negative height.
    REQUIRE((int32_t)Get32(h + 22) == -544);
    REQUIRE(h[26] == 1 && h[27] == 0);
    REQUIRE(h[28] == 32 && h[29] == 0);
    REQUIRE(Get32(h + 30) == 0u);
    REQUIRE(Get32(h + 34) == 960u * 544u * 4u);

    printf("bmp_file: ok\n");
    return 0;
}
