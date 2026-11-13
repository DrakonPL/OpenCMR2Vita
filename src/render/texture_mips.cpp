// The mip levels of a BC3 texture (texture_mips.h).

#include "render/texture_mips.h"

#include "render/gfx_internal.h"

#include <string.h>

namespace {

constexpr int kBlockBytes = 16;     // a BC3 block

inline int Max1(int v)
{
    return v > 1 ? v : 1;
}

// A colour of 8-bit channels as 5:6:5, and back as the decoder reads it.
inline uint16_t To565(int r, int g, int b)
{
    return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | (b * 31 + 127) / 255);
}

inline void From565(uint16_t c, int rgb[3])
{
    const int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    rgb[0] = r << 3 | r >> 2;
    rgb[1] = g << 2 | g >> 4;
    rgb[2] = b << 3 | b >> 2;
}

// 16 texels of R, G, B, A bytes as a BC3 block, by a range fit: the alpha
// and the colour each from their least to their most value, each texel at
// the nearest of the values between. Fast enough for each texture of a
// stage at its load on the PS Vita (a quality encoder took some 16 us for
// each block there), and good for levels drawn small. 16 texels of zero
// give a block of zero bytes.
void EncodeBC3Block(const uint8_t *rgba, uint8_t *out)
{
    int aLow = 255, aHigh = 0;
    int low[3] = { 255, 255, 255 }, high[3] = { 0, 0, 0 };
    for (int i = 0; i < 16; i++) {
        const uint8_t *p = rgba + i * 4;
        for (int c = 0; c < 3; c++) {
            low[c] = p[c] < low[c] ? p[c] : low[c];
            high[c] = p[c] > high[c] ? p[c] : high[c];
        }
        aLow = p[3] < aLow ? p[3] : aLow;
        aHigh = p[3] > aHigh ? p[3] : aHigh;
    }
    // Alpha: alpha 0 the most, alpha 1 the least (the mode of 8 values);
    // code 0 is alpha 0, code 1 alpha 1, codes 2 to 7 the values between,
    // from alpha 0 to alpha 1. One alpha: all codes 0.
    out[0] = (uint8_t)aHigh;
    out[1] = (uint8_t)aLow;
    uint64_t alphaBits = 0;
    if (aHigh > aLow) {
        const int range = aHigh - aLow;
        for (int i = 0; i < 16; i++) {
            const int f = ((rgba[i * 4 + 3] - aLow) * 7 + range / 2) / range;     // 0 .. 7 from the least
            const int code = f == 7 ? 0 : f == 0 ? 1 : 8 - f;
            alphaBits |= (uint64_t)code << (3 * i);
        }
    }
    for (int i = 0; i < 6; i++)
        out[2 + i] = (uint8_t)(alphaBits >> (8 * i));
    // Colour: colour 0 the most of each channel, colour 1 the least, so
    // colour 0 >= colour 1 (BC3 has 4 colours always). Index 0 is colour 0,
    // 1 colour 1, 2 two thirds of the way to colour 0, 3 one third.
    const uint16_t c0 = To565(high[0], high[1], high[2]), c1 = To565(low[0], low[1], low[2]);
    uint32_t bits = 0;
    if (c0 != c1) {
        int e0[3], e1[3], d[3];
        From565(c0, e0);
        From565(c1, e1);
        int dd = 0;
        for (int c = 0; c < 3; c++) {
            d[c] = e0[c] - e1[c];
            dd += d[c] * d[c];
        }
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = rgba + i * 4;
            const int t = (p[0] - e1[0]) * d[0] + (p[1] - e1[1]) * d[1] + (p[2] - e1[2]) * d[2];
            const int x = 6 * t;    // 6 x the place from colour 1 (0) to colour 0 (6 dd)
            const uint32_t index = x >= 5 * dd ? 0u : x >= 3 * dd ? 2u : x >= dd ? 3u : 1u;
            bits |= index << (2 * i);
        }
    }
    out[8] = (uint8_t)c0;
    out[9] = (uint8_t)(c0 >> 8);
    out[10] = (uint8_t)c1;
    out[11] = (uint8_t)(c1 >> 8);
    for (int i = 0; i < 4; i++)
        out[12 + i] = (uint8_t)(bits >> (8 * i));
}

// The bytes of a level of a BC3 texture: rows of 4x4 blocks.
size_t LevelBytes(int width, int height)
{
    return (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4) * kBlockBytes;
}

} // namespace

int Gfx_MipLevelCount(int width, int height, int limit)
{
    int count = 1;
    while (width > 1 || height > 1) {
        width = Max1(width / 2);
        height = Max1(height / 2);
        count++;
    }
    return limit > 0 && count > limit ? limit : count;
}

void Gfx_MakeBC3Mips(const uint8_t *level0, int width, int height, int count, GfxMipRect changed,
                     std::vector<uint8_t> mips[])
{
    if (count < 2 || width <= 0 || height <= 0)
        return;
    bool whole = changed.Empty(), fresh = false;
    for (int m = 1; m < count; m++) {
        const size_t bytes = LevelBytes(Max1(width >> m), Max1(height >> m));
        if (mips[m].size() != bytes) {
            mips[m].assign(bytes, 0);
            fresh = true;
        }
    }
    // The part, out to whole blocks of the last level: the levels of a part
    // so aligned are those of the whole texture there.
    GfxMipRect r = changed;
    const int align = 4 << (count - 1);
    r.x0 = r.x0 < 0 ? 0 : r.x0 / align * align;
    r.y0 = r.y0 < 0 ? 0 : r.y0 / align * align;
    r.x1 = (r.x1 + align - 1) / align * align;
    r.y1 = (r.y1 + align - 1) / align * align;
    r.x1 = r.x1 < width ? r.x1 : width;
    r.y1 = r.y1 < height ? r.y1 : height;
    // New levels are blocks of zero bytes: the levels of a level 0 of zero
    // bytes (EncodeBC3Block). Where level 0 is zero outside the part (an
    // atlas that is new), the part is enough; else all of it is made.
    if (fresh && !whole) {
        const int rowBlocks = (width + 3) / 4, rows = (height + 3) / 4;
        for (int by = 0; by < rows && !whole; by++)
            for (int bx = 0; bx < rowBlocks && !whole; bx++) {
                if (bx * 4 >= r.x0 && bx * 4 < r.x1 && by * 4 >= r.y0 && by * 4 < r.y1)
                    continue;
                const uint8_t *b = level0 + ((size_t)by * rowBlocks + bx) * kBlockBytes;
                for (int i = 0; i < kBlockBytes; i++)
                    whole = whole || b[i] != 0;
            }
    }
    if (whole) {
        r.x0 = r.y0 = 0;
        r.x1 = width;
        r.y1 = height;
    }
    if (r.Empty())
        return;

    // Level 0 of the part as R, G, B, A bytes.
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    std::vector<uint8_t> image((size_t)w * h * 4), half;
    const int rowBlocks = (width + 3) / 4;
    for (int by = r.y0 / 4; by < (r.y1 + 3) / 4; by++) {
        for (int bx = r.x0 / 4; bx < (r.x1 + 3) / 4; bx++) {
            uint32_t texels[16];
            Gfx_DecodeBlock(GFX_FORMAT_BC3, level0 + ((size_t)by * rowBlocks + bx) * kBlockBytes, texels);
            for (int j = 0; j < 4; j++) {
                const int y = by * 4 + j - r.y0;
                if (y < 0 || y >= h)
                    continue;
                for (int i = 0; i < 4; i++) {
                    const int x = bx * 4 + i - r.x0;
                    if (x < 0 || x >= w)
                        continue;
                    const uint32_t p = texels[j * 4 + i];       // 0xAARRGGBB
                    uint8_t *q = &image[((size_t)y * w + x) * 4];
                    q[0] = (uint8_t)(p >> 16);
                    q[1] = (uint8_t)(p >> 8);
                    q[2] = (uint8_t)p;
                    q[3] = (uint8_t)(p >> 24);
                }
            }
        }
    }

    int x0 = r.x0, y0 = r.y0;
    for (int m = 1; m < count; m++) {
        // The next level: each texel the mean of its 2x2 texels (the last
        // one of an odd row or column again where there is no second).
        const int hw = Max1(w / 2), hh = Max1(h / 2);
        half.resize((size_t)hw * hh * 4);
        for (int y = 0; y < hh; y++) {
            const int ya = 2 * y < h ? 2 * y : h - 1, yb = 2 * y + 1 < h ? 2 * y + 1 : h - 1;
            for (int x = 0; x < hw; x++) {
                const int xa = 2 * x < w ? 2 * x : w - 1, xb = 2 * x + 1 < w ? 2 * x + 1 : w - 1;
                const uint8_t *a = &image[((size_t)ya * w + xa) * 4], *b = &image[((size_t)ya * w + xb) * 4];
                const uint8_t *c = &image[((size_t)yb * w + xa) * 4], *d = &image[((size_t)yb * w + xb) * 4];
                uint8_t *q = &half[((size_t)y * hw + x) * 4];
                for (int k = 0; k < 4; k++)
                    q[k] = (uint8_t)((a[k] + b[k] + c[k] + d[k] + 2) / 4);
            }
        }
        image.swap(half);
        w = hw;
        h = hh;
        x0 /= 2;
        y0 /= 2;
        // Its blocks in the part, each of 4x4 texels (the edge texels again
        // past the end of a level smaller than a block).
        const int levelW = Max1(width >> m), levelH = Max1(height >> m);
        const int levelRowBlocks = (levelW + 3) / 4, levelRows = (levelH + 3) / 4;
        for (int by = y0 / 4; by < (y0 + h + 3) / 4 && by < levelRows; by++) {
            for (int bx = x0 / 4; bx < (x0 + w + 3) / 4 && bx < levelRowBlocks; bx++) {
                uint8_t block[16 * 4];
                for (int j = 0; j < 4; j++) {
                    int y = by * 4 + j - y0;
                    y = y < 0 ? 0 : y >= h ? h - 1 : y;
                    for (int i = 0; i < 4; i++) {
                        int x = bx * 4 + i - x0;
                        x = x < 0 ? 0 : x >= w ? w - 1 : x;
                        memcpy(&block[(j * 4 + i) * 4], &image[((size_t)y * w + x) * 4], 4);
                    }
                }
                EncodeBC3Block(block, &mips[m][((size_t)by * levelRowBlocks + bx) * kBlockBytes]);
            }
        }
    }
}
