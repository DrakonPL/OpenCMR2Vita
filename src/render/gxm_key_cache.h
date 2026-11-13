// A small cache of values by a 64-bit key, for the GXM executor
// (gfx_gxm.cpp): a slot for each hash, and a key that meets a slot in use
// takes it. A find is a multiply, a shift and a compare. std::unordered_map
// takes a remainder for each find, and the ARM of the PS Vita has no divide
// instruction: the executor asks for the vertex range of each 3D draw. No
// GXM here: tests/gxm_key_cache.cpp.
#ifndef OPENCMR2_GXM_KEY_CACHE_H
#define OPENCMR2_GXM_KEY_CACHE_H

#include <stdint.h>

// kBits: the cache has 1 << kBits slots.
template <typename T, unsigned kBits> struct GxmKeyCache {
    static constexpr unsigned kSlots = 1u << kBits;
    static constexpr uint64_t kNoKey = ~0ull;   // no value has this key
    struct Slot {
        uint64_t key = kNoKey;
        T value;
    };
    Slot slots[kSlots];

    static unsigned SlotOf(uint64_t key)
    {
        const uint32_t h = (uint32_t)(key >> 32) * 2654435761u ^ (uint32_t)key * 2246822519u;
        return (h ^ h >> 15) * 2654435761u >> (32 - kBits);
    }

    // The value of the key, or nullptr.
    T *Find(uint64_t key)
    {
        Slot &s = slots[SlotOf(key)];
        return s.key == key ? &s.value : nullptr;
    }

    // The slot of the key, for its value: the key that had the slot is gone.
    T &Put(uint64_t key)
    {
        Slot &s = slots[SlotOf(key)];
        s.key = key;
        return s.value;
    }

    void Clear()
    {
        for (Slot &s : slots)
            s.key = kNoKey;
    }
};

#endif
