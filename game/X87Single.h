#ifndef _X87SINGLE_H
#define _X87SINGLE_H

// x87 arithmetic at single precision. Direct3D 7 sets the precision control
// of the x87 to 24 bits (control word 0x007f) when the device is created,
// unless the application asks it to preserve the FPU state (DDSCL_FPUPRESERVE),
// which the game does not. From then on every x87 add, subtract, multiply,
// divide and square root rounds its result to a 24-bit mantissa, whatever the
// declared types; loads of double constants and the results of FSIN, FPTAN and
// FPATAN keep their full precision, and the exponent keeps its 15-bit range.
// The fixed-point math tables are computed this way after the device is
// created, and the CPU runs of the super special stages were recorded with
// them: they replay their controls open loop, so with tables computed at
// double precision the opponents leave their recorded lines and get stuck.
//
// X87S_ADD/SUB/MUL/DIV(a, b) and X87S_SQRT(x) mark these operations, and
// X87S_FILD(float, i) (an explicit cast) or X87S_LOAD(i) (an implicit
// conversion) an integer converted to float as one of their operands, which
// the x87 loads exactly (the conversion is not stored). MSVC6 compiles them as
// the plain operators and casts, so the code is the original's. Other compilers
// round the exact result once to a 24-bit mantissa and return it as a double,
// as the x87 does; the exact result is reconstructed with error-free
// transformations, without fused multiply-add.
#if defined(_MSC_VER) && _MSC_VER < 1300

// The operands are written as in the original expression, so these expand
// to its exact tokens: MSVC6 does not reassociate floating-point operations
// across parentheses, and an extra pair changes its code.
#define X87S_ADD(a, b) a + b
#define X87S_SUB(a, b) a - b
#define X87S_MUL(a, b) a * b
#define X87S_MUL_FIX16(a, b) a * b
#define X87S_DIV(a, b) a / b
#define X87S_SQRT(x) sqrt(x)
#define X87S_FILD(type, x) (type)x
#define X87S_LOAD(x) x

#else

#include <math.h>
#include <stdint.h>
#include <string.h>

// Rounds an exact result to a 24-bit mantissa, nearest even. hi is the double
// nearest to it and lo has the sign of the rest (exact result - hi). The
// general way, through frexp and ldexp: for a zero, a subnormal, an infinity
// and a NaN, and the reference of tests/x87_round.cpp.
static inline double X87s_RoundGeneral(double hi, double lo)
{
    int exponent;
    double m, rounded, other;
    float r;

    if (hi == 0.0 || !isfinite(hi))
        return hi;
    // Scale into [0.5, 1): the x87 exponent range does not overflow a float.
    m = frexp(hi, &exponent);
    r = (float)m;
    rounded = r;
    if (rounded != m && lo != 0.0) {
        // When hi is halfway between two floats, the rest decides.
        other = nextafterf(r, m > rounded ? 2.0f : -2.0f);
        if (fabs(m - rounded) == fabs(other - m))
            rounded = lo > 0.0 ? fmax(rounded, other) : fmin(rounded, other);
    }
    return ldexp(rounded, exponent);
}

// PORT: the same result from the bits of hi, without a call into the maths
// library. The game also computes each vertex it draws with these
// operations, some ten thousand in a frame, and frexp, nextafterf and ldexp
// were most of their time on the PS Vita. A normal double has 52 mantissa
// bits below its leading one, and 23 of them stay. The 29 below are cut, and
// the result goes one step up when they are above the half, or at the half
// with the rest on the far side of hi (no rest: to the even neighbour). A
// step that fills the mantissa carries into the exponent, as it must.
// tests/x87_round.cpp compares it with X87s_RoundGeneral.
static inline double X87s_Round(double hi, double lo)
{
    uint64_t bits;
    memcpy(&bits, &hi, sizeof(bits));
    const unsigned exponent = (unsigned)(bits >> 52) & 0x7ff;
    // An exact result in the range of a float (most results: a product of
    // two floats, a scaled integer): the conversion to float is this
    // rounding, to the nearest and to the even one at the middle.
    if (lo == 0.0 && exponent - (1023 - 120) <= 240)
        return (double)(float)hi;
    if (exponent == 0 || exponent == 0x7ff)
        return X87s_RoundGeneral(hi, lo);
    const uint64_t cut = bits & 0x1fffffffull, half = 0x10000000ull, step = 0x20000000ull;
    bits &= ~0x1fffffffull;
    if (cut > half || (cut == half && (lo != 0.0 ? (lo > 0.0) == (hi > 0.0) : (bits & step) != 0)))
        bits += step;
    memcpy(&hi, &bits, sizeof(hi));
    return hi;
}

// Dekker's exact product: a * b == *pHi + *pLo.
static inline void X87s_TwoProduct(double a, double b, double *pHi, double *pLo)
{
    const double split = 134217729.0; // 2^27 + 1
    double c, aHi, aLo, bHi, bLo;

    *pHi = a * b;
    c = split * a;
    aHi = c - (c - a);
    aLo = a - aHi;
    c = split * b;
    bHi = c - (c - b);
    bLo = b - bHi;
    *pLo = ((aHi * bHi - *pHi) + aHi * bLo + aLo * bHi) + aLo * bLo;
}

static inline double X87s_Add(double a, double b)
{
    double s = a + b;
    double bb = s - a;
    return X87s_Round(s, (a - (s - bb)) + (b - bb));
}

// The count of mantissa bits of a normal double, from its leading one to its
// last one.
static inline int X87s_MantissaBits(uint64_t bits)
{
    return 53 - __builtin_ctzll((bits & 0xfffffffffffffull) | 0x10000000000000ull);
}

// PORT: most products of the game are of two floats, or of an integer and a
// float: their mantissas have 53 bits or fewer together, so the double
// product is exact and has no rest. Only a product of longer mantissas needs
// the exact product of Dekker (17 operations). x, y: the bits of a, b.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, unused))
#endif
static double X87s_MulGeneral(double a, double b, uint64_t x, uint64_t y)
{
    double hi, lo;
    const unsigned ex = (unsigned)(x >> 52) & 0x7ff, ey = (unsigned)(y >> 52) & 0x7ff;

    // PORT: a zero times a finite number is a zero with the sign of the
    // product, exact. Half of the texture coordinates of the 2D layers and
    // of the tyre marks are zeros, and many numbers of a matrix: each took
    // the exact product of Dekker.
    if (((x << 1) == 0 && ey != 0x7ff) || ((y << 1) == 0 && ex != 0x7ff))
        return a * b;
    // Two normal numbers (an exponent field of 0 is a zero or a subnormal)
    // with a normal product: a product below the normal range loses bits.
    if (ex != 0 && ey != 0 && X87s_MantissaBits(x) + X87s_MantissaBits(y) <= 53) {
        uint64_t z;
        hi = a * b;
        memcpy(&z, &hi, sizeof(z));
        if ((z >> 52 & 0x7ff) != 0 && (z >> 52 & 0x7ff) != 0x7ff)
            return X87s_Round(hi, 0.0);
    }
    X87s_TwoProduct(a, b, &hi, &lo);
    return X87s_Round(hi, lo);
}

// PORT: a number times a power of two (the game's 16.16 numbers times
// 1/65536, some ten thousand in a frame, for each vertex of its 2D layers
// and each matrix it gives the renderer): the product is the number with
// another exponent, so it is exact, and the conversion to float rounds it.
// Both exponents are in a range that keeps the product a normal float. This
// part is in each place that multiplies: the rest is one function.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline double X87s_Mul(double a, double b)
{
    uint64_t x, y;

    memcpy(&x, &a, sizeof(x));
    memcpy(&y, &b, sizeof(y));
    if ((y & 0xfffffffffffffull) == 0 && ((unsigned)(y >> 52) & 0x7ff) - (1023 - 60) <= 120 &&
        ((unsigned)(x >> 52) & 0x7ff) - (1023 - 60) <= 120)
        return (double)(float)(a * b);
    return X87s_MulGeneral(a, b, x, y);
}

static inline double X87s_Div(double a, double b)
{
    double q = a / b;
    double hi, lo;

    // The rest of the quotient has the sign of (a - q * b) / b.
    X87s_TwoProduct(q, b, &hi, &lo);
    return X87s_Round(q, ((a - hi) - lo) * (b < 0.0 ? -1.0 : 1.0));
}

static inline double X87s_Sqrt(double x)
{
    double s = sqrt(x);
    double hi, lo;

    X87s_TwoProduct(s, s, &hi, &lo);
    return X87s_Round(s, (x - hi) - lo);
}

#define X87S_ADD(a, b) X87s_Add((double)(a), (double)(b))
#define X87S_SUB(a, b) X87s_Add((double)(a), -(double)(b))
#define X87S_MUL(a, b) X87s_Mul((double)(a), (double)(b))
// PORT: X87S_MUL for a whole number of 32 bits (a: a 16.16 number) and b the
// double 1/65536 (CGraphics::m_oneOver65536). The product is exact for each
// such number, and the conversion to float rounds it as the x87 does: no
// look at the bits of the operands. For a place that makes many of them (a
// triangle of a 3D layer has 15). tests/x87_round.cpp compares the two.
#define X87S_MUL_FIX16(a, b) ((void)sizeof(b), (double)(float)((double)(int)(a) * (1.0 / 65536.0)))
#define X87S_DIV(a, b) X87s_Div((double)(a), (double)(b))
#define X87S_SQRT(x) X87s_Sqrt((double)(x))
#define X87S_FILD(type, x) ((double)(x))
#define X87S_LOAD(x) ((double)(x))

#endif

#endif
