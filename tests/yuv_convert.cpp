// The frames of the movie decoder into BGRX (video/yuv_convert.h): the
// colours of the Bink decoder's arithmetic, the chroma of each pixel pair,
// and both orders of the chroma bytes.

#include "video/yuv_convert.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    // Known colours (BT.601, limited range): black, white, and a red.
    uint8_t px[4];
    YuvConvert::Pixel(16, 128, 128, px);
    REQUIRE(px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 255);
    YuvConvert::Pixel(235, 128, 128, px);
    REQUIRE(px[0] == 255 && px[1] == 255 && px[2] == 255);
    YuvConvert::Pixel(81, 90, 240, px);         // BT.601 red: about 255, 0, 0
    REQUIRE(px[2] >= 250 && px[1] <= 3 && px[0] <= 3);
    // Below black clamps to black, above white to white.
    YuvConvert::Pixel(0, 128, 128, px);
    REQUIRE(px[0] == 0 && px[2] == 0);
    YuvConvert::Pixel(255, 255, 255, px);
    REQUIRE(px[0] == 255 && px[2] == 255);

    // A frame of 36 x 4 (36: the 16-pixel steps of NEON and a rest), with
    // strides wider than the rows and a pitch wider than the pixels.
    const unsigned w = 36, h = 4;
    const size_t lumaStride = 48, chromaStride = 48, pitch = 160;
    std::vector<uint8_t> luma(lumaStride * h), uv(chromaStride * h / 2), vu(uv.size());
    for (size_t i = 0; i < luma.size(); i++)
        luma[i] = (uint8_t)(16 + (i * 7) % 220);
    for (size_t i = 0; i < uv.size(); i += 2) {
        uv[i] = (uint8_t)(40 + (i * 3) % 180);      // U
        uv[i + 1] = (uint8_t)(200 - (i * 5) % 150); // V
        vu[i] = uv[i + 1];
        vu[i + 1] = uv[i];
    }
    std::vector<uint8_t> a(pitch * h, 0xcd), b(pitch * h, 0xcd);
    Yuv_SemiPlanarToBgrx(luma.data(), lumaStride, uv.data(), chromaStride, false, w, 0, h, a.data(), pitch);
    Yuv_SemiPlanarToBgrx(luma.data(), lumaStride, vu.data(), chromaStride, true, w, 0, h, b.data(), pitch);
    // The two orders give one picture.
    REQUIRE(a == b);
    // Each pixel: its luma, the chroma of its pair of columns and rows.
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            const uint8_t *pair = &uv[(y / 2) * chromaStride + (x / 2) * 2];
            YuvConvert::Pixel(luma[y * lumaStride + x], pair[0], pair[1], px);
            REQUIRE(memcmp(&a[y * pitch + x * 4], px, 4) == 0);
        }
    // Past the pixels of a row: as it was.
    REQUIRE(a[w * 4] == 0xcd && a[pitch - 1] == 0xcd);

    // A part of the rows: the others stay.
    std::vector<uint8_t> c(pitch * h, 0);
    Yuv_SemiPlanarToBgrx(luma.data(), lumaStride, uv.data(), chromaStride, false, w, 2, 4, c.data(), pitch);
    REQUIRE(c[0] == 0 && memcmp(&c[2 * pitch], &a[2 * pitch], w * 4) == 0);

    printf("yuv_convert: ok\n");
    return 0;
}
