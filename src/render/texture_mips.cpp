// The mip levels of a BC3 texture (texture_mips.h).

#include "render/texture_mips.h"

#include "render/gfx_internal.h"

#include <string.h>

#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

namespace {

constexpr int kBlockBytes = 16;     // a BC3 block

inline int Max1(int v)
{
    return v > 1 ? v : 1;
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
    bool whole = changed.Empty();
    for (int m = 1; m < count; m++) {
        const size_t bytes = LevelBytes(Max1(width >> m), Max1(height >> m));
        if (mips[m].size() != bytes) {
            mips[m].assign(bytes, 0);
            whole = true;
        }
    }
    // The part, out to whole blocks of the last level: the levels of a part
    // so aligned are those of the whole texture there.
    GfxMipRect r = changed;
    if (whole) {
        r.x0 = r.y0 = 0;
        r.x1 = width;
        r.y1 = height;
    }
    const int align = 4 << (count - 1);
    r.x0 = r.x0 < 0 ? 0 : r.x0 / align * align;
    r.y0 = r.y0 < 0 ? 0 : r.y0 / align * align;
    r.x1 = (r.x1 + align - 1) / align * align;
    r.y1 = (r.y1 + align - 1) / align * align;
    r.x1 = r.x1 < width ? r.x1 : width;
    r.y1 = r.y1 < height ? r.y1 : height;
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
                stb_compress_dxt_block(&mips[m][((size_t)by * levelRowBlocks + bx) * kBlockBytes], block, 1,
                                       STB_DXT_NORMAL);
            }
        }
    }
}
