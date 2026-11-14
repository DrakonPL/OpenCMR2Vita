// The frame rate of video.vsync 2 (render/frame_pacing.h): a frame at every
// second refresh, with a present that waits for one refresh (the PC) and
// with the blank count of the PS Vita's display.

#include "render/frame_pacing.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    const uint64_t period = 16667;      // 60 Hz, in microseconds

    // No wait for the first frame, for a refresh of each frame, and without
    // the refresh.
    GfxFramePacer pacer;
    REQUIRE(pacer.WaitBefore(1000, 2, period) == 0);
    pacer.Presented(100000);
    REQUIRE(pacer.WaitBefore(100500, 1, period) == 0);
    REQUIRE(pacer.WaitBefore(100500, 0, period) == 0);

    // A frame that is ready early waits until half a refresh before the
    // second refresh: the present waits for that refresh.
    REQUIRE(pacer.WaitBefore(105000, 2, period) == 100000 + period + period / 2 - 105000);
    // A frame that is late does not wait.
    REQUIRE(pacer.WaitBefore(100000 + 2 * period, 2, period) == 0);

    // A frame of the game that the pacer runs: at 60 Hz with a frame of
    // 10 ms, each present comes at every second refresh.
    {
        GfxFramePacer p;
        uint64_t now = 0;
        uint64_t shown[8];
        for (int f = 0; f < 8; f++) {
            now += 10000;                           // the frame's work
            now += p.WaitBefore(now, 2, period);    // the pacer
            const uint64_t refresh = (now / period + 1) * period;
            now = refresh;                          // the present waits for the refresh
            shown[f] = refresh;
            p.Presented(now);
        }
        for (int f = 1; f < 8; f++)
            REQUIRE(shown[f] - shown[f - 1] == 2 * period);
    }

    // The PS Vita: one blank between flips waits for none; two wait for one
    // when the frame is ready right after the blank of the frame before.
    REQUIRE(Gfx_BlanksBeforeFlip(100, 100, 1) == 0);
    REQUIRE(Gfx_BlanksBeforeFlip(100, 100, 2) == 1);
    REQUIRE(Gfx_BlanksBeforeFlip(101, 100, 2) == 0);
    REQUIRE(Gfx_BlanksBeforeFlip(105, 100, 2) == 0);
    REQUIRE(Gfx_BlanksBeforeFlip(100, 100, 3) == 2);
    // The counts wrap.
    REQUIRE(Gfx_BlanksBeforeFlip(0xffffffffu, 0xffffffffu, 2) == 1);
    REQUIRE(Gfx_BlanksBeforeFlip(0u, 0xffffffffu, 2) == 0);
    // The first flip: the count of the frame before is long past.
    REQUIRE(Gfx_BlanksBeforeFlip(5000, 0, 2) == 0);

    printf("frame_pacing: ok\n");
    return 0;
}
