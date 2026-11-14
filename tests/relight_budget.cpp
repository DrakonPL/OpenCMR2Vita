// The relight of the sectors in view under a budget (game/RelightBudget.h):
// the sectors that wait go to the next frame, in turn, and each sector that
// waits is relit within a few frames.

#include "RelightBudget.h"

#include <cstdio>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    // Ten sectors in view, each 1,000 vertices, all lit with the old light.
    const unsigned count = 10;
    std::vector<int> waiting(count, RELIGHT_LATER_OK);
    std::vector<int> calls(count, 0);
    const auto waits = [&](unsigned i) { return waiting[i]; };
    const auto relight = [&](unsigned i) {
        calls[i]++;
        const int lit = waiting[i] != RELIGHT_NONE ? 1000 : 0;
        waiting[i] = RELIGHT_NONE;
        return lit;
    };

    // No budget: all in one frame.
    unsigned cursor = Relight_InBudget(count, 0, 0, waits, relight);
    REQUIRE(cursor == 0);
    for (unsigned i = 0; i < count; i++)
        REQUIRE(waiting[i] == RELIGHT_NONE && calls[i] == 1);

    // A budget of 3,000: three sectors in a frame (at 3,000 the next one
    // waits), the rest in the next frames from where the frame stopped.
    for (unsigned i = 0; i < count; i++) {
        waiting[i] = RELIGHT_LATER_OK;
        calls[i] = 0;
    }
    cursor = Relight_InBudget(count, 0, 3000, waits, relight);
    REQUIRE(cursor == 3);
    REQUIRE(waiting[0] == RELIGHT_NONE && waiting[2] == RELIGHT_NONE && waiting[3] == RELIGHT_LATER_OK);
    // The sectors that wait are not called in this frame.
    for (unsigned i = 3; i < count; i++)
        REQUIRE(calls[i] == 0);
    cursor = Relight_InBudget(count, cursor, 3000, waits, relight);
    REQUIRE(cursor == 6 && waiting[5] == RELIGHT_NONE && waiting[6] == RELIGHT_LATER_OK);
    // Relit in the first frame, and called again for the game's light.
    REQUIRE(calls[0] == 2 && calls[1] == 2 && calls[2] == 2);
    cursor = Relight_InBudget(count, cursor, 3000, waits, relight);
    REQUIRE(cursor == 9);
    cursor = Relight_InBudget(count, cursor, 3000, waits, relight);
    REQUIRE(cursor == 0);
    for (unsigned i = 0; i < count; i++)
        REQUIRE(waiting[i] == RELIGHT_NONE);

    // A sector that was never lit does not wait, after the budget too.
    for (unsigned i = 0; i < count; i++)
        waiting[i] = RELIGHT_LATER_OK;
    waiting[8] = RELIGHT_NOW;
    cursor = Relight_InBudget(count, 0, 3000, waits, relight);
    REQUIRE(waiting[8] == RELIGHT_NONE && waiting[7] == RELIGHT_LATER_OK && cursor == 3);

    // A cursor past the end of a shorter list starts in it.
    REQUIRE(Relight_InBudget(0, 5, 3000, waits, relight) == 0);
    for (unsigned i = 0; i < count; i++)
        waiting[i] = RELIGHT_LATER_OK;
    cursor = Relight_InBudget(4, 6, 1000, waits, relight);
    REQUIRE(waiting[2] == RELIGHT_NONE && waiting[3] == RELIGHT_LATER_OK && cursor == 3);

    printf("relight_budget: ok\n");
    return 0;
}
