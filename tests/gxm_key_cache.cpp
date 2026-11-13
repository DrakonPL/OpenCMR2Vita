// The key cache of the GXM executor (src/render/gxm_key_cache.h): a value is
// found by its key until another key takes its slot, and never by another
// key. No GXM.

#include "render/gxm_key_cache.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    static GxmKeyCache<int, 6> cache;
    REQUIRE(cache.kSlots == 64);
    REQUIRE(cache.Find(0) == nullptr && cache.Find(12345) == nullptr);

    // A key is found with its value, and no other key is.
    cache.Put(12345) = 7;
    REQUIRE(cache.Find(12345) != nullptr && *cache.Find(12345) == 7);
    REQUIRE(cache.Find(12346) == nullptr && cache.Find((uint64_t)12345 << 32) == nullptr);
    cache.Put(0) = 9;
    {
        const int *zero = cache.Find(0), *first = cache.Find(12345);
        REQUIRE(zero != nullptr && *zero == 9 && first != nullptr && *first == 7);
    }

    // The keys of vertex ranges (first vertex << 32 | count): each slot is in
    // range, and the keys spread over the slots. A good spread of 2,000 keys
    // over 64 slots leaves no slot empty.
    bool used[64] = {};
    for (uint64_t first = 0; first < 2000; first += 4) {
        for (uint64_t count = 4; count <= 16; count += 4) {
            const unsigned slot = cache.SlotOf(first << 32 | count);
            REQUIRE(slot < 64);
            used[slot] = true;
        }
    }
    for (bool u : used)
        REQUIRE(u);

    // Two keys of one slot: the later key has the slot, the earlier is gone.
    uint64_t other = 12346;
    while (cache.SlotOf(other) != cache.SlotOf(12345))
        other++;
    cache.Put(other) = 11;
    REQUIRE(cache.Find(12345) == nullptr && cache.Find(other) != nullptr && *cache.Find(other) == 11);

    // Each key that is put is found until its slot is taken: of 40 keys in
    // 64 slots, each found value is the key's own.
    cache.Clear();
    REQUIRE(cache.Find(other) == nullptr && cache.Find(0) == nullptr);
    for (int i = 0; i < 40; i++)
        cache.Put((uint64_t)i * 977 << 32 | 12) = i;
    int found = 0;
    for (int i = 0; i < 40; i++) {
        const int *value = cache.Find((uint64_t)i * 977 << 32 | 12);
        if (value != nullptr) {
            REQUIRE(*value == i);
            found++;
        }
    }
    REQUIRE(found >= 25);

    printf("gxm_key_cache: ok\n");
    return 0;
}
