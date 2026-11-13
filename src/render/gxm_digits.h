// A number as seven-segment digits made of rectangles, for the frame rate
// that the GXM executor draws over the picture (gfx_gxm.cpp). The executor
// has no font: it fills each rectangle with its clear program. No GXM here:
// tests/gxm_digits.cpp checks the digits.
#ifndef OPENCMR2_GXM_DIGITS_H
#define OPENCMR2_GXM_DIGITS_H

struct GxmDigitRect {
    int x, y, width, height;
};

// The size of a digit, the thickness of a segment, the gap to the next digit.
constexpr int kGxmDigitWidth = 16, kGxmDigitHeight = 28, kGxmDigitThick = 4, kGxmDigitGap = 6;
constexpr int kGxmDigitSegments = 7;
constexpr int kGxmNumberDigits = 3;     // a number has at most this many digits

// The segments of a digit, bit 0 to bit 6: top, top right, bottom right,
// bottom, bottom left, top left, middle.
inline unsigned Gxm_DigitSegments(int digit)
{
    static const unsigned char segments[10] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f };
    return digit >= 0 && digit <= 9 ? segments[digit] : 0;
}

// The rectangle of a segment of a digit whose top left corner is at x, y.
inline GxmDigitRect Gxm_SegmentRect(int segment, int x, int y)
{
    const int w = kGxmDigitWidth, h = kGxmDigitHeight, t = kGxmDigitThick, half = h / 2;
    switch (segment) {
    case 0: return { x, y, w, t };
    case 1: return { x + w - t, y, t, half };
    case 2: return { x + w - t, y + half, t, half };
    case 3: return { x, y + h - t, w, t };
    case 4: return { x, y + half, t, half };
    case 5: return { x, y, t, half };
    default: return { x, y + half - t / 2, w, t };
    }
}

// The rectangles of a number (0 to 999; more shows as 999) whose first digit
// has its top left corner at x, y. Returns their count: at most
// kGxmNumberDigits * kGxmDigitSegments. No leading zeros.
inline int Gxm_NumberRects(int value, int x, int y, GxmDigitRect *out)
{
    if (value < 0)
        value = 0;
    if (value > 999)
        value = 999;
    int digits[kGxmNumberDigits];
    int count = 0;
    do {
        digits[count++] = value % 10;
        value /= 10;
    } while (value > 0 && count < kGxmNumberDigits);
    int rects = 0;
    for (int i = count - 1; i >= 0; i--) {
        const unsigned segments = Gxm_DigitSegments(digits[i]);
        for (int segment = 0; segment < kGxmDigitSegments; segment++)
            if (segments >> segment & 1)
                out[rects++] = Gxm_SegmentRect(segment, x, y);
        x += kGxmDigitWidth + kGxmDigitGap;
    }
    return rects;
}

// The width of a number of that many digits.
inline int Gxm_NumberWidth(int digits)
{
    return digits * kGxmDigitWidth + (digits - 1) * kGxmDigitGap;
}

#endif
