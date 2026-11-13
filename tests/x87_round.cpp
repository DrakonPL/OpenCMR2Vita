// The x87 rounding of game/X87Single.h: the fast X87s_Round gives the bits of
// the general one for each double and each sign of the rest. The physics and
// the recorded runs of the game depend on these bits.

#include "X87Single.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

uint64_t s_seed = 0x9e3779b97f4a7c15ull;

uint64_t Random()
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 7;
    s_seed ^= s_seed << 17;
    return s_seed;
}

uint64_t Bits(double v)
{
    uint64_t b;
    memcpy(&b, &v, sizeof(b));
    return b;
}

double FromBits(uint64_t b)
{
    double v;
    memcpy(&v, &b, sizeof(v));
    return v;
}

unsigned s_checked;

// The product as it was before the fast ways: the exact product of Dekker,
// then the general rounding.
double ReferenceMul(double a, double b)
{
    double hi, lo;
    X87s_TwoProduct(a, b, &hi, &lo);
    return X87s_RoundGeneral(hi, lo);
}

bool SameProduct(double a, double b)
{
    const double fast = X87S_MUL(a, b), reference = ReferenceMul(a, b);
    s_checked++;
    if (Bits(fast) != Bits(reference) && !(fast != fast && reference != reference)) {
        fprintf(stderr, "%016llx * %016llx: %016llx, reference %016llx\n", (unsigned long long)Bits(a),
                (unsigned long long)Bits(b), (unsigned long long)Bits(fast), (unsigned long long)Bits(reference));
        return false;
    }
    return true;
}

// The fast rounding of hi against the general one, with no rest and a rest
// of each sign. NaN payloads aside, the bits are the same.
bool Same(double hi)
{
    const double rests[3] = { 0.0, 1e-300, -1e-300 };
    for (double lo : rests) {
        const double fast = X87s_Round(hi, lo), general = X87s_RoundGeneral(hi, lo);
        s_checked++;
        if (Bits(fast) != Bits(general) && !(fast != fast && general != general)) {
            fprintf(stderr, "hi %016llx lo %g: %016llx, general %016llx\n", (unsigned long long)Bits(hi), lo,
                    (unsigned long long)Bits(fast), (unsigned long long)Bits(general));
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    // Each double: random bits, of each exponent.
    for (int i = 0; i < 2000000; i++)
        REQUIRE(Same(FromBits(Random())));
    // Doubles in the range of the numbers of the game.
    for (int i = 0; i < 2000000; i++) {
        const double v = (double)(int64_t)(Random() >> 20) / 65536.0 * ((Random() & 1) ? 1.0 : -1.0);
        REQUIRE(Same(v) && Same(v * 1.0000001) && Same(1.0 / (v + 0.5)));
    }
    // The halfway cases: a double at the middle of two neighbour floats, and
    // the doubles next to it, of each sign and through the exponents. There
    // the rest decides, or the even neighbour.
    for (int i = 0; i < 1000000; i++) {
        const uint64_t mantissa = (Random() & 0x7fffffull) << 29;       // 23 bits that stay
        const uint64_t exponent = (uint64_t)(1 + Random() % 2046) << 52;
        const uint64_t sign = (Random() & 1) << 63;
        const uint64_t middle = sign | exponent | mantissa | 0x10000000ull;
        REQUIRE(Same(FromBits(middle)) && Same(FromBits(middle + 1)) && Same(FromBits(middle - 1)));
        REQUIRE(Same(FromBits(sign | exponent | mantissa)) && Same(FromBits(sign | exponent | mantissa | 0x1fffffffull)));
    }
    // The mantissa that a step fills: the carry goes into the exponent.
    for (uint64_t exponent = 1; exponent < 2047; exponent += 7) {
        const uint64_t full = exponent << 52 | 0xfffffffffffffull;
        REQUIRE(Same(FromBits(full)) && Same(FromBits(full | 1ull << 63)));
        REQUIRE(Same(FromBits(exponent << 52 | 0xfffffff0000000ull)));
        REQUIRE(Same(FromBits(exponent << 52 | 0xfffffe0000000ull | 0x10000000ull)));
    }
    // Zeros, subnormals, infinities and NaNs take the general way.
    const double special[] = { 0.0, -0.0, FromBits(1), FromBits(0x000fffffffffffffull), FromBits(0x7ff0000000000000ull),
                               FromBits(0xfff0000000000000ull), FromBits(0x7ff8000000000000ull) };
    for (double v : special)
        REQUIRE(Same(v));

    // Known values. A product that fits 24 bits stays. The middle of 1 and
    // its next float goes by the rest, and to the even 1 without one.
    REQUIRE(X87S_MUL(3.0f, 0.5f) == 1.5);
    const double next = (double)nextafterf(1.0f, 2.0f), middle = (1.0 + next) * 0.5;
    REQUIRE(X87s_Round(middle, 0.0) == 1.0 && X87s_Round(middle, 1e-30) == next && X87s_Round(middle, -1e-30) == 1.0);
    REQUIRE(X87s_Round(-middle, 0.0) == -1.0 && X87s_Round(-middle, -1e-30) == -next && X87s_Round(-middle, 1e-30) == -1.0);
    // The operations with it: a product, a quotient and a sum of two floats
    // are the float product, quotient and sum (the x87 at 24 bits is a float
    // unit there).
    for (int i = 0; i < 1000000; i++) {
        const float a = (float)(int32_t)Random() / 65536.0f, b = (float)(int32_t)Random() / 4096.0f + 0.25f;
        REQUIRE(X87S_MUL(a, b) == (double)(a * b));
        REQUIRE(X87S_DIV(a, b) == (double)(a / b));
        REQUIRE(X87S_ADD(a, b) == (double)(a + b));
    }

    // The product, against the exact product of Dekker with the general
    // rounding: floats, integers with a float, doubles with long mantissas,
    // zeros, and numbers with few mantissa bits (the short way).
    for (int i = 0; i < 2000000; i++) {
        const float a = (float)(int32_t)Random() / 65536.0f, b = (float)(int32_t)Random() / 4096.0f + 0.25f;
        const double longA = (double)(int64_t)(Random() >> 12) / 1048576.0, longB = 1.0 / ((double)a + 3.0);
        const int32_t whole = (int32_t)Random();
        REQUIRE(SameProduct(a, b) && SameProduct(whole, 1.0f / 65536.0f) && SameProduct(whole, b));
        REQUIRE(SameProduct(longA, longB) && SameProduct(longA, b) && SameProduct(a, longB));
        REQUIRE(SameProduct((double)(Random() & 0xffff), (double)(Random() & 0xffffff) * 0.125));
    }
    for (double a : special)
        for (double b : { 0.0, -0.0, 1.5, -2.0, 1e300, 1e-300, 3.0000000001 })
            REQUIRE(SameProduct(a, b) && SameProduct(b, a));
    // A zero of each sign times a number (the short way of the zero): any
    // bits, with an infinity and a NaN among them, and the power of two of
    // the game's 16.16 numbers.
    for (int i = 0; i < 500000; i++) {
        const double any = FromBits(Random());
        REQUIRE(SameProduct(0.0, any) && SameProduct(-0.0, any) && SameProduct(any, 0.0) && SameProduct(any, -0.0));
    }
    REQUIRE(SameProduct(0.0, FromBits(0x7ff0000000000000ull)) && SameProduct(FromBits(0xfff0000000000000ull), -0.0));
    REQUIRE(SameProduct(0.0, 1.0 / 65536.0) && SameProduct(-0.0, 1.0 / 65536.0) && SameProduct(0.0, -1.0 / 65536.0));
    REQUIRE(Bits(X87S_MUL((int32_t)0, 1.0f / 65536.0f)) == 0 && Bits(X87S_MUL(-0.0, 1.0f / 65536.0f)) == 1ull << 63);
    // The short product of a whole number and 1/65536 (X87S_MUL_FIX16)
    // against the product: any whole number, and those at the edges.
    {
        const double scale = 1.0 / 65536.0;
        const int32_t edges[] = { 0, 1, -1, 0x10000, -0x10000, 0x7fffffff, (int32_t)0x80000000, 0x1000001, 0x1000003,
                                  -0x1000001, 0x7fffff80, 0x7fffffc0 };
        for (int32_t whole : edges)
            REQUIRE(Bits(X87S_MUL_FIX16(whole, scale)) == Bits(X87S_MUL(whole, scale)));
        for (int i = 0; i < 2000000; i++) {
            const int32_t whole = (int32_t)Random(), small = whole >> (Random() % 32);
            REQUIRE(Bits(X87S_MUL_FIX16(whole, scale)) == Bits(X87S_MUL(whole, scale)));
            REQUIRE(Bits(X87S_MUL_FIX16(small, scale)) == Bits(X87S_MUL(small, scale)));
            REQUIRE(Bits(X87S_MUL(whole, scale)) == Bits(ReferenceMul(whole, scale)));
        }
    }
    // A number times a power of two, through the exponents: inside the
    // range of the short way, at its edges and beyond them.
    for (int i = 0; i < 2000000; i++) {
        const double any = FromBits(Random());
        const double longA = (double)(int64_t)(Random() >> 12) / 1048576.0;
        const double power = ldexp((Random() & 1) ? 1.0 : -1.0, (int)(Random() % 160) - 80);
        const double scaled = ldexp(longA, (int)(Random() % 160) - 80);
        REQUIRE(SameProduct(longA, power) && SameProduct(scaled, power) && SameProduct(any, power));
        REQUIRE(SameProduct(longA, 1.0 / 65536.0) && SameProduct((int32_t)Random(), 1.0 / 65536.0));
        REQUIRE(SameProduct(power, longA));
    }
    for (int e = -70; e <= 70; e++)
        for (int f = -70; f <= 70; f += 3) {
            const double a = ldexp(1.0000001192092896, e), b = ldexp(1.0, f);
            REQUIRE(SameProduct(a, b) && SameProduct(-a, b) && SameProduct(ldexp(1.9999999, e), -b));
        }
    REQUIRE(SameProduct(1e200, 1e200) && SameProduct(1e-200, 1e-200) && SameProduct(-1e200, 3.0));
    // The edge of the short way: two whole numbers of 27 bits whose product
    // has 54 bits, with a 1, 28 zeros and a 1 at its end. The double product
    // loses the last 1 and is the middle of two floats. Only the rest tells
    // that the exact product is above the middle.
    {
        unsigned found = 0, above = 0;
        while (found < 2000) {
            const uint64_t a = (Random() & 0x3ffffff) | 0x4000001;      // odd, 27 bits
            // The inverse of a modulo 2^30 (Newton's steps double its bits).
            uint64_t inverse = a;
            for (int i = 0; i < 5; i++)
                inverse *= 2 - a * inverse;
            const uint64_t b = (0x20000001ull * inverse) & 0x3fffffff;
            if (b < 0x4000000 || b >= 0x8000000 || (a * b) >> 53 == 0)
                continue;
            REQUIRE(((a * b) & 0x3fffffff) == 0x20000001);
            REQUIRE(X87s_MantissaBits(Bits((double)a)) + X87s_MantissaBits(Bits((double)b)) == 54);
            REQUIRE(SameProduct((double)a, (double)b) && SameProduct(-(double)a, (double)b));
            // The exact product is above the middle: the result is the float above.
            const double rounded = X87S_MUL((double)a, (double)b);
            REQUIRE(rounded == (double)(((a * b) >> 30) + 1) * 1073741824.0);
            above += (((a * b) >> 30) & 1) == 0;    // the even neighbour is the float below
            found++;
        }
        REQUIRE(above > 500);
    }
    REQUIRE(X87s_MantissaBits(Bits(1.0)) == 1 && X87s_MantissaBits(Bits(3.0)) == 2);
    REQUIRE(X87s_MantissaBits(Bits(1.0 + 1.0 / 4503599627370496.0)) == 53);

    printf("x87_round: ok, %u cases\n", s_checked);
    return 0;
}
