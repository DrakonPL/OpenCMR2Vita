// FTOI on 32-bit ARM (the VCVTR way of game/FloatToInt.h) against the llrint
// reference, bit for bit. Not in the CMake build: it needs an ARM compiler.
// Run in the image vc-armtest (Debian, arm-linux-gnueabihf-g++, qemu-arm):
//   arm-linux-gnueabihf-g++ -O2 -std=c++17 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fwrapv \
//     -fno-strict-aliasing -fsigned-char -ffp-contract=off -static -Igame -Isrc -Isrc/port/compat \
//     -include src/port/msvcrt.h tests/ftoi_vcvtr_arm.cpp -o ftoi_arm && qemu-arm ./ftoi_arm
#define CMR2_QIFIST
#include "FloatToInt.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static long long Ref64(double x)
{
    return x >= -9223372036854775808.0 && x < 9223372036854775808.0 ? llrint(x) : (long long)(1ULL << 63);
}

static long long Ref32(double x)
{
    long long r = x > -2147483649.0 && x < 2147483648.0 ? llrint(x) : (long long)1 << 31;
    return r >= -2147483647LL - 1 && r <= 2147483647LL ? r : -2147483647LL - 1;
}

static unsigned long failures;

static void Check(double x)
{
    if (Ftoi_Fist32(x) != Ref32(x) || Ftoi_Fist64(x) != Ref64(x)) {
        if (failures++ < 20)
            printf("FAIL %.17g: 32 %lld/%lld 64 %lld/%lld\n", x, Ftoi_Fist32(x), Ref32(x), Ftoi_Fist64(x), Ref64(x));
    }
}

int main()
{
    std::vector<double> edges = {0.0, -0.0, 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.49999999999999994, 1e9 + 0.5,
                                 2147483647.0, 2147483647.4, 2147483647.5, 2147483647.6, 2147483648.0,
                                 -2147483648.0, -2147483648.4, -2147483648.5, -2147483648.6, -2147483649.0,
                                 4294967295.5, 1e18, -1e18, 9.3e18, -9.3e18, INFINITY, -INFINITY, NAN, -NAN,
                                 1e-300, -1e-300, 5e-324, 65535.5, 65536.5, -65536.5};
    for (double e : edges) {
        Check(e);
        Check(std::nextafter(e, INFINITY));
        Check(std::nextafter(e, -INFINITY));
    }
    for (long long k = -2147483649LL; k <= 2147483649LL; k += 7919) {
        Check((double)k + 0.5);
        Check((double)k - 0.5);
        Check((double)k);
    }
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 20000000; i++) {
        uint64_t bits = rng();
        double x;
        memcpy(&x, &bits, sizeof(x));
        Check(x);
        Check(std::ldexp((double)(int64_t)rng(), -(int)(rng() % 70)));
        float f = (float)std::ldexp((double)(int32_t)rng(), -(int)(rng() % 40));
        Check(f);
    }
    printf("%s: %lu failures\n", failures ? "FAILED" : "passed", failures);
    return failures != 0;
}
