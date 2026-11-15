// A run of joined small draws and the start of the distance fade
// (Gfx_FadeSideJoins, render/gfx_small_draw.h).

#include "port/gfx.h"
#include "render/gfx_small_draw.h"

#include <cstdio>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

// The runs of a list of reaches, as the recorder and a batch job make them:
// the count of draws in each run, and whether the run reaches the start.
struct Run {
    unsigned draws;
    float reach;
};

static std::vector<Run> Runs(float fadeStart, const std::vector<float> &reaches)
{
    std::vector<Run> runs;
    unsigned nearTail = 0;
    for (float reach : reaches) {
        if (!runs.empty() && Gfx_FadeSideJoins(fadeStart, runs.back().reach, reach, &nearTail)) {
            runs.back().draws++;
            runs.back().reach = reach > runs.back().reach ? reach : runs.back().reach;
            continue;
        }
        runs.push_back({ 1, reach });
        nearTail = 0;
    }
    return runs;
}

int main()
{
    unsigned tail = 0;
    // No fade, or a reach that is not known: the draw joins.
    REQUIRE(Gfx_FadeSideJoins(-1.0f, 50.0f, 500.0f, &tail) && tail == 0);
    REQUIRE(Gfx_FadeSideJoins(100.0f, -1.0f, 500.0f, &tail) && tail == 0);
    REQUIRE(Gfx_FadeSideJoins(100.0f, 50.0f, -1.0f, &tail) && tail == 0);
    for (int i = 0; i < 20; i++)
        REQUIRE(Gfx_FadeSideJoins(100.0f, 150.0f, -1.0f, &tail) && tail == 0);
    // Near joins near, far joins far, far does not join near. At the start
    // is far (the executor draws without the fade below the start only).
    REQUIRE(Gfx_FadeSideJoins(100.0f, 50.0f, 99.9f, &tail) && tail == 0);
    REQUIRE(Gfx_FadeSideJoins(100.0f, 150.0f, 100.0f, &tail) && tail == 0);
    REQUIRE(Gfx_FadeSideJoins(100.0f, 100.0f, 150.0f, &tail) && tail == 0);
    REQUIRE(!Gfx_FadeSideJoins(100.0f, 50.0f, 100.0f, &tail));
    REQUIRE(!Gfx_FadeSideJoins(100.0f, 99.9f, 300.0f, &tail));
    // Near joins far kGfxNearInFadeRun times, and a far draw starts the
    // count again.
    for (unsigned i = 0; i < kGfxNearInFadeRun; i++)
        REQUIRE(Gfx_FadeSideJoins(100.0f, 150.0f, 20.0f, &tail) && tail == i + 1);
    REQUIRE(!Gfx_FadeSideJoins(100.0f, 150.0f, 20.0f, &tail) && tail == kGfxNearInFadeRun);
    REQUIRE(Gfx_FadeSideJoins(100.0f, 150.0f, 120.0f, &tail) && tail == 0);
    REQUIRE(Gfx_FadeSideJoins(100.0f, 150.0f, 20.0f, &tail) && tail == 1);

    // The trees of a frame, from far to near: one run with the fade, a few
    // near trees in it where the two kinds take turns, and one run without.
    {
        std::vector<float> trees;
        for (int i = 0; i < 40; i++)
            trees.push_back(300.0f - (float)i);                     // far
        for (int i = 0; i < 20; i++)
            trees.push_back(i % 2 == 0 ? 99.0f : 101.0f);            // they take turns
        for (int i = 0; i < 200; i++)
            trees.push_back(98.0f - (float)i * 0.4f);               // near
        const std::vector<Run> runs = Runs(100.0f, trees);
        REQUIRE(runs.size() == 2);
        REQUIRE(runs[0].draws == 40 + 20 + kGfxNearInFadeRun && runs[0].reach == 300.0f);
        REQUIRE(runs[1].draws == 200 - kGfxNearInFadeRun && runs[1].reach < 100.0f);
        // No run without the fade has a draw that reaches the start: the
        // rule that keeps the picture.
        unsigned at = 0;
        for (const Run &run : runs) {
            for (unsigned i = 0; i < run.draws; i++)
                REQUIRE(run.reach >= 100.0f || trees[at + i] < 100.0f);
            at += run.draws;
        }
        // Without a fade: one run.
        REQUIRE(Runs(-1.0f, trees).size() == 1);
    }
    // Any order: each draw is in a run, in its order, and a run without the
    // fade has near draws only.
    {
        unsigned seed = 12345;
        for (int round = 0; round < 200; round++) {
            std::vector<float> reaches;
            for (int i = 0; i < 300; i++) {
                seed = seed * 1664525u + 1013904223u;
                reaches.push_back((float)(seed >> 16 & 0xff));
            }
            const std::vector<Run> runs = Runs(128.0f, reaches);
            unsigned at = 0;
            for (const Run &run : runs) {
                float farthest = 0.0f;
                for (unsigned i = 0; i < run.draws; i++)
                    farthest = reaches[at + i] > farthest ? reaches[at + i] : farthest;
                REQUIRE(run.reach == farthest);
                at += run.draws;
            }
            REQUIRE(at == reaches.size());
        }
    }
    printf("ok\n");
    return 0;
}
