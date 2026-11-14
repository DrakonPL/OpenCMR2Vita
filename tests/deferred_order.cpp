// The order of a list by its keys (game/DeferredOrder.h): the least key
// first, the items of one key in their order, as std::stable_sort gives it.

#include "DeferredOrder.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

unsigned s_seed = 2024;

unsigned Next()
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 4;
}

// Whether DeferredOrder_Sort gives the order of std::stable_sort.
bool SameAsStableSort(const std::vector<unsigned int> &keys)
{
    const unsigned count = (unsigned)keys.size();
    std::vector<unsigned short> order(count + 1, 0xabcd), scratch(count + 1, 0xabcd);
    DeferredOrder_Sort(keys.data(), count, order.data(), scratch.data());
    std::vector<unsigned short> expected(count);
    for (unsigned i = 0; i < count; i++)
        expected[i] = (unsigned short)i;
    std::stable_sort(expected.begin(), expected.end(),
                     [&](unsigned short a, unsigned short b) { return keys[a] < keys[b]; });
    for (unsigned i = 0; i < count; i++)
        if (order[i] != expected[i])
            return false;
    // Nothing after the list.
    return order[count] == 0xabcd && scratch[count] == 0xabcd;
}

} // namespace

int main()
{
    // No item, one item.
    REQUIRE(SameAsStableSort({}));
    REQUIRE(SameAsStableSort({ 7 }));

    // Keys that differ in each digit, in one digit only, and in none.
    REQUIRE(SameAsStableSort({ 0x04030201u, 0x01020304u, 0xffffffffu, 0u, 0x80000000u, 0x7fffffffu }));
    REQUIRE(SameAsStableSort({ 0x00000300u, 0x00000100u, 0x00000200u, 0x00000100u }));
    REQUIRE(SameAsStableSort({ 0x12000000u, 0x11000000u, 0x13000000u }));
    REQUIRE(SameAsStableSort({ 5, 5, 5, 5, 5 }));

    // The distances of a frame: 16.16 numbers below 600, each key the same
    // in its top digit, some of them the same key.
    for (int round = 0; round < 200; round++) {
        const unsigned count = 1 + Next() % 700;
        const unsigned step = 1 + (unsigned)(round % 5) * 4096u;
        std::vector<unsigned int> keys(count);
        for (unsigned i = 0; i < count; i++)
            keys[i] = 0x7fffffffu - (Next() % (600u << 16)) / step * step;
        REQUIRE(SameAsStableSort(keys));
    }

    // Keys over all 32 bits, and a few values many times.
    for (int round = 0; round < 50; round++) {
        const unsigned count = 2 + Next() % 4094;
        std::vector<unsigned int> keys(count);
        for (unsigned i = 0; i < count; i++)
            keys[i] = round % 2 ? Next() * 16u + Next() % 16u : (Next() % 7) * 0x01010101u;
        REQUIRE(SameAsStableSort(keys));
    }

    printf("ok\n");
    return 0;
}
