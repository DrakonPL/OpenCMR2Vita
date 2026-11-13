// The key cache of the GXM executor (src/render/gxm_key_cache.h): a value is
// found by its key until the cache is cleared, and never by another key. No
// GXM.

#include "render/gxm_key_cache.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    static GxmKeyCache<int, 6> cache;
    REQUIRE(cache.Find(0) == nullptr && cache.Find(12345) == nullptr);

    // A key is found with its value, and no other key is.
    cache.Put(12345) = 7;
    REQUIRE(cache.slots.size() == 64);
    REQUIRE(cache.Find(12345) != nullptr && *cache.Find(12345) == 7);
    REQUIRE(cache.Find(12346) == nullptr && cache.Find((uint64_t)12345 << 32) == nullptr);
    cache.Put(0) = 9;
    {
        const int *zero = cache.Find(0), *first = cache.Find(12345);
        REQUIRE(zero != nullptr && *zero == 9 && first != nullptr && *first == 7);
    }

    // The keys of vertex ranges (first vertex << 32 | count) spread over the
    // slots: a good spread of 2,000 keys over 64 slots leaves no slot empty.
    bool start[64] = {};
    for (uint64_t first = 0; first < 2000; first += 4)
        for (uint64_t count = 4; count <= 16; count += 4)
            start[cache.Hash(first << 32 | count) & 63] = true;
    for (bool s : start)
        REQUIRE(s);

    // Two keys that start at one slot: both stay, each with its value.
    uint64_t other = 12346;
    while ((cache.Hash(other) & 63) != (cache.Hash(12345) & 63))
        other++;
    cache.Put(other) = 11;
    {
        const int *first = cache.Find(12345), *second = cache.Find(other);
        REQUIRE(first != nullptr && *first == 7 && second != nullptr && *second == 11);
    }
    // A third key of that slot that was not put is not found.
    uint64_t third = other + 1;
    while ((cache.Hash(third) & 63) != (cache.Hash(12345) & 63))
        third++;
    REQUIRE(cache.Find(third) == nullptr);

    // The ranges of a stage's vertex buffer: each of 5,000 keys stays, the
    // table grows, and at most half of it is in use.
    cache.Clear();
    REQUIRE(cache.Find(other) == nullptr && cache.Find(0) == nullptr && cache.used == 0);
    for (int i = 0; i < 5000; i++) {
        const uint64_t key = (uint64_t)i * 12 << 32 | (unsigned)(4 + i % 300);
        REQUIRE(cache.Find(key) == nullptr);
        cache.Put(key) = i;
        REQUIRE(cache.used * 2 <= cache.slots.size());
    }
    REQUIRE(cache.used == 5000 && cache.slots.size() == 16384);
    for (int i = 0; i < 5000; i++) {
        const int *value = cache.Find((uint64_t)i * 12 << 32 | (unsigned)(4 + i % 300));
        REQUIRE(value != nullptr && *value == i);
    }
    REQUIRE(cache.Find((uint64_t)5000 * 12 << 32 | 4) == nullptr);

    // The table has an end: more keys than half of the largest table start
    // the cache again. A key that is found still has its own value.
    for (int i = 5000; i < 9000; i++)
        cache.Put((uint64_t)i * 12 << 32 | 4) = i;
    REQUIRE(cache.slots.size() == 16384 && cache.used <= 8192 && cache.used > 0);
    int found = 0;
    for (int i = 5000; i < 9000; i++) {
        const int *value = cache.Find((uint64_t)i * 12 << 32 | 4);
        if (value != nullptr) {
            REQUIRE(*value == i);
            found++;
        }
    }
    REQUIRE(found == (int)cache.used && cache.Find((uint64_t)12 << 32 | 5) == nullptr);

    // A key that is taken out is not found, and each other key is: the keys
    // behind it in a full run of slots (a small table, half in use) stay.
    static GxmKeyCache<int, 4> small;
    for (int i = 0; i < 8; i++)
        small.Put((uint64_t)i * 7) = i;
    REQUIRE(small.slots.size() == 16 && small.used == 8);
    for (int gone = 0; gone < 8; gone++) {
        REQUIRE(small.Remove((uint64_t)gone * 7) && !small.Remove((uint64_t)gone * 7));
        REQUIRE(small.Find((uint64_t)gone * 7) == nullptr && small.used == 7);
        for (int i = 0; i < 8; i++) {
            const int *value = small.Find((uint64_t)i * 7);
            REQUIRE(i == gone || (value != nullptr && *value == i));
        }
        small.Put((uint64_t)gone * 7) = gone;
    }
    REQUIRE(!small.Remove(999));

    // The keys that a test names go, the others stay (the ranges that a
    // patch of a vertex buffer touches).
    cache.Clear();
    for (int i = 0; i < 1000; i++)
        cache.Put((uint64_t)i << 32 | 4) = i;
    cache.RemoveIf([](uint64_t key) { return (key >> 32) % 3 == 0; });
    REQUIRE(cache.used == 666);
    for (int i = 0; i < 1000; i++) {
        const int *value = cache.Find((uint64_t)i << 32 | 4);
        REQUIRE(i % 3 == 0 ? value == nullptr : (value != nullptr && *value == i));
    }

    printf("gxm_key_cache: ok\n");
    return 0;
}
