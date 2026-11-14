// PORT: the relight of the sectors in view under a budget of vertices in a
// frame ([video] relight_budget). When the light of the scene changes, the
// game lights each sector in view again in one frame: on the PS Vita that
// frame took 50 ms (the writes of the meshes into their vertex buffers, 16
// to 23 ms). The light changes in small steps, so a sector that waits a
// frame or two for its relight shows no difference. A sector that was never
// lit does not wait. No game state here: tests/relight_budget.cpp checks it
// on the PC.
#ifndef OPENCMR2_RELIGHT_BUDGET_H
#define OPENCMR2_RELIGHT_BUDGET_H

// What a sector in view waits for (waits(i)).
enum { RELIGHT_NONE, RELIGHT_LATER_OK, RELIGHT_NOW };

// Goes over the count sectors in view from cursor (in a circle). relight(i)
// relights the sector at i and gives the vertices it lit. A sector whose
// colours wait (RELIGHT_LATER_OK) after budget vertices were lit in this
// frame waits for the next frame. The others are each relit (also for the
// light that the game changes in each frame). Returns the cursor for the
// next frame: the first sector that waits, else 0. budget <= 0: no budget.
template <typename Waits, typename Relight>
unsigned Relight_InBudget(unsigned count, unsigned cursor, int budget, Waits waits, Relight relight)
{
    if (count == 0)
        return 0;
    const unsigned start = cursor % count;
    long lit = 0;
    bool deferred = false;
    unsigned next = 0;
    for (unsigned k = 0; k < count; k++) {
        const unsigned i = (start + k) % count;
        if (budget > 0 && lit >= budget && waits(i) == RELIGHT_LATER_OK) {
            if (!deferred)
                next = i;
            deferred = true;
            continue;
        }
        lit += relight(i);
    }
    return deferred ? next : 0;
}

#endif
