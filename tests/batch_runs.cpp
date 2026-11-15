// The order of a run of stage objects for the tree batch
// (game/BatchRuns.h): the objects of one state together and in their order,
// the last object of the run last.

#include "BatchRuns.h"

#include <cstdio>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

unsigned s_seed = 31337;

unsigned Next()
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 8;
}

// How often the state changes from one object of the order to the next.
unsigned Changes(const std::vector<unsigned> &state, const std::vector<unsigned short> &order)
{
    unsigned n = 0;
    for (size_t i = 1; i < order.size(); i++)
        n += state[order[i]] != state[order[i - 1]];
    return n;
}

// Whether the order has each object once, the objects of one state in
// their order, and the last object last.
bool Sound(const std::vector<unsigned> &state, const std::vector<unsigned short> &order)
{
    const size_t count = state.size();
    std::vector<int> seen(count, 0);
    for (size_t i = 0; i < count; i++) {
        if (order[i] >= count || seen[order[i]]++)
            return false;
        for (size_t k = 0; k < i; k++)
            if (state[order[k]] == state[order[i]] && order[k] > order[i])
                return false;
    }
    return count == 0 || order[count - 1] == count - 1;
}

std::vector<unsigned short> OrderOf(const std::vector<unsigned> &state)
{
    std::vector<unsigned short> order(state.size() + 1, 0xabcd);
    BatchRun_Order(state.data(), (unsigned)state.size(), order.data());
    // Nothing after the run.
    if (order.back() != 0xabcd)
        order.assign(order.size(), 0xffff);
    order.pop_back();
    return order;
}

} // namespace

int main()
{
    // No object, one object.
    REQUIRE(OrderOf({}).empty());
    REQUIRE(OrderOf({ 5 }) == std::vector<unsigned short>({ 0 }));

    // Two states that alternate: the state of the last object goes last.
    REQUIRE(OrderOf({ 1, 2, 1, 2, 1, 2 }) == std::vector<unsigned short>({ 0, 2, 4, 1, 3, 5 }));
    REQUIRE(OrderOf({ 1, 2, 1, 2, 1 }) == std::vector<unsigned short>({ 1, 3, 0, 2, 4 }));
    // Three states: by their last objects (state 7 at 1, state 3 at 4,
    // state 9 at 5).
    REQUIRE(OrderOf({ 9, 7, 3, 9, 3, 9 }) == std::vector<unsigned short>({ 1, 2, 4, 0, 3, 5 }));
    // One state: the order of the run.
    REQUIRE(OrderOf({ 4, 4, 4 }) == std::vector<unsigned short>({ 0, 1, 2 }));

    // Random runs of a few states: sound, and as few changes of the state
    // as there are states.
    for (int round = 0; round < 3000; round++) {
        const unsigned count = 1 + Next() % 300;
        const unsigned kinds = 1 + Next() % kBatchRunStates;
        std::vector<unsigned> state(count);
        unsigned distinct = 0;
        // The flags of a mesh state: a value for each kind.
        for (unsigned i = 0; i < count; i++) {
            const unsigned k = Next() % kinds;
            state[i] = 0x40000u * (k & 1) | 0x20u * (k >> 1 & 1) | (k >> 2);
        }
        for (unsigned i = 0; i < count; i++) {
            bool first = true;
            for (unsigned k = 0; k < i; k++)
                first = first && state[k] != state[i];
            distinct += first;
        }
        const std::vector<unsigned short> order = OrderOf(state);
        REQUIRE(Sound(state, order));
        REQUIRE(Changes(state, order) == distinct - 1);
    }

    // More states than the order puts together: the run keeps its order.
    {
        std::vector<unsigned> state;
        for (unsigned i = 0; i < 40; i++)
            state.push_back(i % (kBatchRunStates + 1));
        const std::vector<unsigned short> order = OrderOf(state);
        for (unsigned i = 0; i < 40; i++)
            REQUIRE(order[i] == i);
    }

    printf("ok\n");
    return 0;
}
