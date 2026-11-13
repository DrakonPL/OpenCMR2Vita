// A cache of values by a 64-bit key, for the GXM executor (gfx_gxm.cpp): a
// table of slots that grows, with the next slot for a key whose slot is in
// use. A find is a multiply, a mask and a compare for most keys.
// std::unordered_map takes a remainder for each find, and the ARM of the
// PS Vita has no divide instruction: the executor asks for the vertex range
// of each 3D draw. A key stays until Clear: the first cache gave a slot to
// the key that came last, and the draws of a stage then measured hundreds of
// ranges again in each frame. No GXM here: tests/gxm_key_cache.cpp.
#ifndef OPENCMR2_GXM_KEY_CACHE_H
#define OPENCMR2_GXM_KEY_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

// kBits: the cache starts with 1 << kBits slots. At most half of the slots
// are in use: then the table doubles, up to 1 << kMaxBits slots. A full
// table of that size starts again empty.
template <typename T, unsigned kBits> struct GxmKeyCache {
    static constexpr unsigned kMaxBits = 14;
    static constexpr uint64_t kNoKey = ~0ull;   // no value has this key
    struct Slot {
        uint64_t key = kNoKey;
        T value;
    };
    std::vector<Slot> slots;    // none, or a power of two
    unsigned used = 0;

    static uint32_t Hash(uint64_t key)
    {
        uint32_t h = (uint32_t)(key >> 32) * 2654435761u ^ (uint32_t)key * 2246822519u;
        h = (h ^ h >> 15) * 2654435761u;
        return h ^ h >> 16;
    }

    // The value of the key, or nullptr.
    T *Find(uint64_t key)
    {
        if (slots.empty())
            return nullptr;
        const unsigned mask = (unsigned)slots.size() - 1;
        for (unsigned i = Hash(key) & mask;; i = (i + 1) & mask) {
            Slot &s = slots[i];
            if (s.key == key)
                return &s.value;
            if (s.key == kNoKey)
                return nullptr;
        }
    }

    // A slot for the key, for its value. The key is not in the cache.
    T &Put(uint64_t key)
    {
        if ((used + 1) * 2 > slots.size())
            Grow();
        const unsigned mask = (unsigned)slots.size() - 1;
        unsigned i = Hash(key) & mask;
        while (slots[i].key != kNoKey)
            i = (i + 1) & mask;
        slots[i].key = key;
        used++;
        return slots[i].value;
    }

    void Clear()
    {
        for (Slot &s : slots)
            s.key = kNoKey;
        used = 0;
    }

private:
    void Grow()
    {
        if (slots.empty()) {
            slots.resize((size_t)1 << kBits);
            return;
        }
        if (slots.size() >= (size_t)1 << kMaxBits) {
            Clear();
            return;
        }
        std::vector<Slot> old((size_t)slots.size() * 2);
        old.swap(slots);
        used = 0;
        for (const Slot &s : old)
            if (s.key != kNoKey)
                Put(s.key) = s.value;
    }
};

#endif
