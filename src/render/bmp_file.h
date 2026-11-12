// The header of an uncompressed 32-bit BMP file, rows from the top: the
// device screenshot of the GXM executor (gfx_gxm.cpp) writes it in front of
// the B, G, R, A bytes of its pixels. No SDL on the Vita, so no SDL_SaveBMP.
// tests/bmp_file.cpp checks it.
#ifndef OPENCMR2_BMP_FILE_H
#define OPENCMR2_BMP_FILE_H

#include <stdint.h>

constexpr int kBmpHeaderBytes = 54;

inline void Bmp_Put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

inline void Bmp_Put32(uint8_t *p, uint32_t v)
{
    Bmp_Put16(p, v);
    Bmp_Put16(p + 2, v >> 16);
}

// The file header and the BITMAPINFOHEADER of a width x height image. The
// height is negative: the rows are stored from the top.
inline void Bmp_Header(int width, int height, uint8_t out[kBmpHeaderBytes])
{
    const uint32_t pixelBytes = (uint32_t)width * (uint32_t)height * 4;
    for (int i = 0; i < kBmpHeaderBytes; i++)
        out[i] = 0;
    out[0] = 'B';
    out[1] = 'M';
    Bmp_Put32(out + 2, kBmpHeaderBytes + pixelBytes);   // file size
    Bmp_Put32(out + 10, kBmpHeaderBytes);               // offset of the pixels
    Bmp_Put32(out + 14, 40);                            // the info header's size
    Bmp_Put32(out + 18, (uint32_t)width);
    Bmp_Put32(out + 22, (uint32_t)-height);
    Bmp_Put16(out + 26, 1);                             // planes
    Bmp_Put16(out + 28, 32);                            // bits per pixel
    Bmp_Put32(out + 34, pixelBytes);                    // BI_RGB (0) at 30, then the image size
    Bmp_Put32(out + 38, 2835);                          // 72 dots per inch
    Bmp_Put32(out + 42, 2835);
}

#endif
