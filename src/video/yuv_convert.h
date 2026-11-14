// The frames of the PS Vita's movie decoder (sceAvPlayer: a plane of luma,
// then one of the two chroma channels interleaved at half the size) into
// the BGRX pixels of the movie's texture. The arithmetic of the Bink
// decoder's ConvertRows (BT.601, limited range): a movie converted to H.264
// from its Bink file shows its colours. No platform here: tests/yuv_convert.cpp
// checks it on the PC.
#ifndef OPENCMR2_YUV_CONVERT_H
#define OPENCMR2_YUV_CONVERT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace YuvConvert {

inline uint8_t Clamp(int value)
{
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

// One pixel, as ConvertRows of the Bink decoder.
inline void Pixel(int luma8, int u8, int v8, uint8_t *dst)
{
    const int luma = 298 * (luma8 - 16 > 0 ? luma8 - 16 : 0) + 128;
    const int u = u8 - 128, v = v8 - 128;
    dst[0] = Clamp((luma + 516 * u) >> 8);
    dst[1] = Clamp((luma - 100 * u - 208 * v) >> 8);
    dst[2] = Clamp((luma + 409 * v) >> 8);
    dst[3] = 255;
}

} // namespace YuvConvert

// The rows y0 .. y1 - 1 of a frame of width pixels. luma: lumaStride bytes a
// row. chroma: chromaStride bytes a row of the half-height plane, a pair of
// bytes for each two pixels, V first with vFirst (NV21), else U first
// (NV12). bgrx: pitch bytes a row.
inline void Yuv_SemiPlanarToBgrx(const uint8_t *luma, size_t lumaStride, const uint8_t *chroma, size_t chromaStride,
                                 bool vFirst, unsigned width, unsigned y0, unsigned y1, uint8_t *bgrx, size_t pitch)
{
    for (unsigned y = y0; y < y1; ++y) {
        const uint8_t *lumaRow = luma + (size_t)y * lumaStride;
        const uint8_t *chromaRow = chroma + (size_t)(y / 2) * chromaStride;
        uint8_t *dst = bgrx + (size_t)y * pitch;
        unsigned x = 0;
#ifdef __ARM_NEON
        // 16 pixels at a time, as the Bink decoder's NEON rows.
        for (; x + 16 <= width; x += 16, dst += 64) {
            const uint16x8_t luma16[2] = { vqsubq_u16(vmovl_u8(vld1_u8(lumaRow + x)), vdupq_n_u16(16)),
                                           vqsubq_u16(vmovl_u8(vld1_u8(lumaRow + x + 8)), vdupq_n_u16(16)) };
            // 8 pairs of chroma: each pair for two pixels.
            const uint8x8x2_t pairs = vld2_u8(chromaRow + x);
            const uint8x8_t uHalf = vFirst ? pairs.val[1] : pairs.val[0];
            const uint8x8_t vHalf = vFirst ? pairs.val[0] : pairs.val[1];
            const uint8x8x2_t u8 = vzip_u8(uHalf, uHalf);
            const uint8x8x2_t v8 = vzip_u8(vHalf, vHalf);
            uint8x8_t b[2], g[2], r[2];
            for (int half = 0; half < 2; ++half) {
                const int16x8_t u = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u8.val[half])), vdupq_n_s16(128));
                const int16x8_t v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v8.val[half])), vdupq_n_s16(128));
                uint16x4_t bq[2], gq[2], rq[2];
                for (int quarter = 0; quarter < 2; ++quarter) {
                    const uint16x4_t l16 = quarter ? vget_high_u16(luma16[half]) : vget_low_u16(luma16[half]);
                    const int32x4_t lum = vmlaq_n_s32(vdupq_n_s32(128), vreinterpretq_s32_u32(vmovl_u16(l16)), 298);
                    const int32x4_t uq = vmovl_s16(quarter ? vget_high_s16(u) : vget_low_s16(u));
                    const int32x4_t vq = vmovl_s16(quarter ? vget_high_s16(v) : vget_low_s16(v));
                    bq[quarter] = vqmovun_s32(vshrq_n_s32(vmlaq_n_s32(lum, uq, 516), 8));
                    gq[quarter] = vqmovun_s32(vshrq_n_s32(vmlaq_n_s32(vmlaq_n_s32(lum, uq, -100), vq, -208), 8));
                    rq[quarter] = vqmovun_s32(vshrq_n_s32(vmlaq_n_s32(lum, vq, 409), 8));
                }
                b[half] = vqmovn_u16(vcombine_u16(bq[0], bq[1]));
                g[half] = vqmovn_u16(vcombine_u16(gq[0], gq[1]));
                r[half] = vqmovn_u16(vcombine_u16(rq[0], rq[1]));
            }
            uint8x16x4_t out;
            out.val[0] = vcombine_u8(b[0], b[1]);
            out.val[1] = vcombine_u8(g[0], g[1]);
            out.val[2] = vcombine_u8(r[0], r[1]);
            out.val[3] = vdupq_n_u8(255);
            vst4q_u8(dst, out);
        }
#endif
        for (; x < width; ++x, dst += 4) {
            const uint8_t *pair = chromaRow + (x / 2) * 2;
            const int u = vFirst ? pair[1] : pair[0], v = vFirst ? pair[0] : pair[1];
            YuvConvert::Pixel(lumaRow[x], u, v, dst);
        }
    }
}

#endif
