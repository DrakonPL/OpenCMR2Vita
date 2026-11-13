// The seven-segment digits of the frame rate (src/render/gxm_digits.h): each
// digit has its segments, and the rectangles of a number are inside its box.
// No GXM.

#include "render/gxm_digits.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

int Count(unsigned bits)
{
    int n = 0;
    for (; bits != 0; bits >>= 1)
        n += bits & 1;
    return n;
}

// The digit as a picture of 3 columns and 5 rows: which cells its segments
// light. Two digits with one picture would read the same.
unsigned Picture(int digit)
{
    const unsigned s = Gxm_DigitSegments(digit);
    unsigned p = 0;
    if (s & 1)
        p |= 7;                 // top row
    if (s & 2)
        p |= 4 | 4 << 3 | 4 << 6;
    if (s & 4)
        p |= 4 << 6 | 4 << 9 | 4 << 12;
    if (s & 8)
        p |= 7 << 12;           // bottom row
    if (s & 16)
        p |= 1 << 6 | 1 << 9 | 1 << 12;
    if (s & 32)
        p |= 1 | 1 << 3 | 1 << 6;
    if (s & 64)
        p |= 7 << 6;            // middle row
    return p;
}

} // namespace

int main()
{
    // The count of segments of each digit, as on any seven-segment display.
    const int lit[10] = { 6, 2, 5, 5, 4, 5, 6, 3, 7, 6 };
    for (int digit = 0; digit < 10; digit++)
        REQUIRE(Count(Gxm_DigitSegments(digit)) == lit[digit]);
    REQUIRE(Gxm_DigitSegments(-1) == 0 && Gxm_DigitSegments(10) == 0);
    // 1 is the two right segments, 7 adds the top, 4 has no top and no bottom.
    REQUIRE(Gxm_DigitSegments(1) == (2u | 4u));
    REQUIRE(Gxm_DigitSegments(7) == (1u | 2u | 4u));
    REQUIRE((Gxm_DigitSegments(4) & (1u | 8u)) == 0 && (Gxm_DigitSegments(4) & 64u) != 0);
    REQUIRE((Gxm_DigitSegments(0) & 64u) == 0);
    for (int a = 0; a < 10; a++)
        for (int b = a + 1; b < 10; b++)
            REQUIRE(Picture(a) != Picture(b));

    // Each segment is inside the box of its digit, and has an area.
    for (int segment = 0; segment < kGxmDigitSegments; segment++) {
        const GxmDigitRect r = Gxm_SegmentRect(segment, 100, 50);
        REQUIRE(r.width > 0 && r.height > 0);
        REQUIRE(r.x >= 100 && r.x + r.width <= 100 + kGxmDigitWidth);
        REQUIRE(r.y >= 50 && r.y + r.height <= 50 + kGxmDigitHeight);
    }
    // The top is at the top, the bottom at the bottom, the right ones at the right.
    REQUIRE(Gxm_SegmentRect(0, 0, 0).y == 0 && Gxm_SegmentRect(3, 0, 0).y == kGxmDigitHeight - kGxmDigitThick);
    REQUIRE(Gxm_SegmentRect(1, 0, 0).x == kGxmDigitWidth - kGxmDigitThick && Gxm_SegmentRect(5, 0, 0).x == 0);
    REQUIRE(Gxm_SegmentRect(2, 0, 0).y == kGxmDigitHeight / 2 && Gxm_SegmentRect(4, 0, 0).x == 0);

    // A number: its digits from the left, without leading zeros.
    GxmDigitRect rects[kGxmNumberDigits * kGxmDigitSegments];
    REQUIRE(Gxm_NumberRects(0, 0, 0, rects) == 6);
    REQUIRE(Gxm_NumberRects(7, 0, 0, rects) == 3);
    REQUIRE(Gxm_NumberRects(30, 10, 20, rects) == 5 + 6);
    // 3 comes first, at the left. 0 is one digit and a gap to the right.
    for (int i = 0; i < 5; i++)
        REQUIRE(rects[i].x >= 10 && rects[i].x + rects[i].width <= 10 + kGxmDigitWidth && rects[i].y >= 20);
    for (int i = 5; i < 11; i++)
        REQUIRE(rects[i].x >= 10 + kGxmDigitWidth + kGxmDigitGap);
    REQUIRE(Gxm_NumberRects(118, 0, 0, rects) == 2 + 2 + 7);
    // Out of range: 0 and 999.
    REQUIRE(Gxm_NumberRects(-5, 0, 0, rects) == 6);
    REQUIRE(Gxm_NumberRects(5000, 0, 0, rects) == 18);
    for (int i = 0; i < 18; i++)
        REQUIRE(rects[i].x + rects[i].width <= Gxm_NumberWidth(3));
    REQUIRE(Gxm_NumberWidth(1) == kGxmDigitWidth && Gxm_NumberWidth(3) == 3 * kGxmDigitWidth + 2 * kGxmDigitGap);

    printf("gxm_digits: ok\n");
    return 0;
}
