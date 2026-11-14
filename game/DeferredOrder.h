// PORT: the order of a list by a key of 32 bits, the least key first, as a
// radix sort in four passes of 8 bits. It keeps the items of one key in
// their order (a stable sort). For the deferred stage objects of a frame
// (Game_OrderDeferredDraws), some hundreds of them: the passes of 11 bits
// that it had cleared and added up 2,048 counts three times, which cost
// more than the items did. A pass in which each key has the same digit
// changes nothing and is left out (the top digits of the distances). No
// game state here: tests/deferred_order.cpp checks it on the PC.
#ifndef OPENCMR2_DEFERRED_ORDER_H
#define OPENCMR2_DEFERRED_ORDER_H

#include <string.h>

// order[0..count) gets the indices of keys[0..count), the least key first.
// scratch has room for count indices. count is at most 65,536.
inline void DeferredOrder_Sort(const unsigned int *keys, unsigned int count, unsigned short *order,
                               unsigned short *scratch)
{
    unsigned short *from = order;
    unsigned short *to = scratch;
    unsigned int i;

    for (i = 0; i < count; i++)
        order[i] = (unsigned short)i;
    if (count < 2)
        return;
    for (int shift = 0; shift < 32; shift += 8) {
        unsigned int counts[256];
        unsigned int start = 0;

        memset(counts, 0, sizeof(counts));
        for (i = 0; i < count; i++)
            counts[(keys[i] >> shift) & 255]++;
        if (counts[(keys[0] >> shift) & 255] == count)
            continue;
        for (i = 0; i < 256; i++) {
            const unsigned int n = counts[i];
            counts[i] = start;
            start += n;
        }
        for (i = 0; i < count; i++)
            to[counts[(keys[from[i]] >> shift) & 255]++] = from[i];
        unsigned short *swap = from;
        from = to;
        to = swap;
    }
    if (from != order)
        memcpy(order, from, count * sizeof(order[0]));
}

#endif
