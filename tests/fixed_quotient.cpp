// The quotient of the fixed-point helpers by a division of doubles
// (game/FixedPoint.h, FixQuotientByDouble): the quotient of the integers,
// or "no" where the caller divides the integers.

#include <cstdio>
#include <cstdint>

#include "FixedPoint.h"

unsigned short g_sqrtTable[4096];
int g_sinTable[4096];
int g_tanTable[4096];
unsigned short g_atanTable[512];

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

unsigned s_seed = 777;

unsigned Next()
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed;
}

// A number of 32 bits: all the bits from two steps of the generator, at a
// size that differs from call to call.
int NextInt()
{
    const unsigned bits = (Next() >> 16) | (Next() & 0xffff0000u);
    return (int)bits >> (Next() >> 27);
}

// Whether FixQuotientByDouble tells what the division of the integers
// tells for num / den. A quotient outside an int, and den = 0: "no".
bool Same(long long num, int den)
{
    int q = 12345;
    const bool ok = FixQuotientByDouble((double)num, den, &q);
    if (den == 0)
        return !ok;
    const long long expected = num / den;
    if (expected > 2147483647LL || expected < -2147483647LL)
        return !ok || (long long)q == expected;
    return ok && (long long)q == expected;
}

} // namespace

int main()
{
    const int edges[] = { 1, -1, 2, -2, 3, -3, 7, 10, 0x7fff, 0x8000, 0xffff, 0x10000, -0x10000, 0x10001, 0x12345,
                          -0x12345, 0x40000000, -0x40000000, 0x7fffffff, -0x7fffffff, (int)0x80000000 };

    // FixDiv: a * 65536 / b.
    REQUIRE(Same(0x30000LL * 65536, 0x20000));
    REQUIRE(Same(-0x30000LL * 65536, 0x20000));
    REQUIRE(Same(1LL * 65536, 3));
    REQUIRE(Same(-1LL * 65536, 3));
    REQUIRE(Same(0, 5));
    REQUIRE(Same(65536, 0));
    for (int a : edges)
        for (int b : edges)
            REQUIRE(Same((long long)a * 65536, b));
    for (int i = 0; i < 4000000; i++)
        REQUIRE(Same((long long)NextInt() * 65536, NextInt()));

    // A quotient one below a whole number by the least part: q * b - 1.
    for (int i = 0; i < 2000000; i++) {
        const int b = NextInt();
        const long long q = NextInt();
        if (b == 0)
            continue;
        const long long num = q * b + (b > 0 ? -1 : 1) * (q >= 0 ? 1 : -1);
        if (num < (1LL << 51) && num > -(1LL << 51)) {
            REQUIRE(Same(num, b));
            REQUIRE(Same(q * b, b));
        }
    }

    // FixVecScaleRecip: 2^32 / len.
    for (int len = -70000; len <= 70000; len++)
        REQUIRE(Same(1LL << 32, len));
    for (int len : edges)
        REQUIRE(Same(1LL << 32, len));
    for (int i = 0; i < 1000000; i++)
        REQUIRE(Same(1LL << 32, NextInt()));

    // The helpers give what the integers give, with either division.
    for (int i = 0; i < 1000000; i++) {
        const int a = NextInt(), b = NextInt();
        if (b == 0)
            continue;
        int q;
        if (FixQuotientByDouble((double)a * 65536.0, b, &q))
            REQUIRE(q == FixDiv(a, b));
    }

    printf("ok\n");
    return 0;
}
