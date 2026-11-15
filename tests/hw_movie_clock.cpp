// The time of a movie of the hardware decoder (video/hw_movie_clock.h): the
// sound is taken as it plays, and a frame waits for its time.

#include "video/hw_movie_clock.h"

#include <cstdio>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    // Before the stream plays: sound up to the lead, no more.
    REQUIRE(HwMovie_SoundWanted(0.0, 0.0));
    REQUIRE(HwMovie_SoundWanted(42.0, 0.0));
    REQUIRE(!HwMovie_SoundWanted(64.0, 0.0));
    // While it plays: more sound when the lead is used up.
    REQUIRE(!HwMovie_SoundWanted(1064.0, 1000.0));
    REQUIRE(HwMovie_SoundWanted(1064.0, 1020.0));
    // After a long frame of the game: sound until the lead is there again.
    REQUIRE(HwMovie_SoundWanted(1064.0, 1500.0));

    // A movie of 30 frames in a second, sound in frames of 21.3 ms, a game
    // that asks each millisecond: the sound taken follows the time, never
    // more than the lead and one frame of sound ahead, never behind.
    {
        double queued = 0.0;
        int taken = 0;
        for (int ms = 0; ms <= 10000; ms++) {
            while (HwMovie_SoundWanted(queued, (double)ms)) {
                queued += 21.333;
                taken++;
            }
            REQUIRE(queued - ms >= kHwMovieSoundAheadMs && queued - ms < kHwMovieSoundAheadMs + 21.4);
        }
        // 10 s of sound and the lead: not all the sound of the movie at once.
        REQUIRE(taken >= 470 && taken <= 474);
    }

    // The frames: before the start each one shows.
    REQUIRE(HwMovie_FrameDue(false, 500, 0, 0));
    // After it: a frame at 33 ms shows from 31 ms on (2 ms of grace).
    REQUIRE(!HwMovie_FrameDue(true, 33, 0, 0));
    REQUIRE(!HwMovie_FrameDue(true, 33, 0, 30));
    REQUIRE(HwMovie_FrameDue(true, 33, 0, 31));
    REQUIRE(HwMovie_FrameDue(true, 33, 0, 400));
    // The stamps count from the first frame.
    REQUIRE(!HwMovie_FrameDue(true, 1033, 1000, 20));
    REQUIRE(HwMovie_FrameDue(true, 1033, 1000, 33));
    // A stamp at or before the first one, and one far ahead: no wait.
    REQUIRE(HwMovie_FrameDue(true, 1000, 1000, 0));
    REQUIRE(HwMovie_FrameDue(true, 400, 1000, 0));
    REQUIRE(HwMovie_FrameDue(true, 90000, 0, 100));
    REQUIRE(!HwMovie_FrameDue(true, 2050, 0, 100));

    // A decoder that gives its frames at once (60 in a second on the
    // display): shown by their stamps, 30 in a second.
    {
        int shown = 0;
        uint64_t next = 0;
        for (uint32_t ms = 0; ms < 2000; ms++)
            if (HwMovie_FrameDue(true, next, 0, ms)) {
                shown++;
                next = (uint64_t)(shown * 1000 / 30);
            }
        REQUIRE(shown >= 59 && shown <= 61);
    }

    printf("ok\n");
    return 0;
}
