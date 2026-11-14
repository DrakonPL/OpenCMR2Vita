// The frame rate of video.vsync 2: a frame at every second refresh of the
// display (30 frames a second at 60 Hz), steadier than 60 that the GPU
// cannot keep. No platform here: tests/frame_pacing.cpp checks it on the PC.
#ifndef OPENCMR2_FRAME_PACING_H
#define OPENCMR2_FRAME_PACING_H

#include <stdint.h>

// Where the present waits only for the next refresh (SDL_GPU, an OpenGL
// driver without a swap interval of 2): the frame first waits until half a
// refresh before the refresh it shows on, then the present waits for it.
struct GfxFramePacer {
    uint64_t lastPresent = 0;   // microseconds; 0: none yet

    // The microseconds to wait at now before a present that shows a frame
    // interval refreshes of period microseconds after the frame before it.
    uint64_t WaitBefore(uint64_t now, int interval, uint64_t period) const
    {
        if (interval < 2 || lastPresent == 0 || period == 0)
            return 0;
        const uint64_t ready = lastPresent + (uint64_t)(interval - 1) * period + period / 2;
        return now < ready ? ready - now : 0;
    }

    void Presented(uint64_t now) { lastPresent = now; }
};

// The vertical blanks that the display of the PS Vita waits before a flip
// at the blank count now: the frame shows interval blanks after the frame
// before it, which showed at the count shown. A flip shows at the next
// blank. The counts wrap.
inline int Gfx_BlanksBeforeFlip(unsigned now, unsigned shown, int interval)
{
    const int wait = (int)(shown + (unsigned)interval - 1u - now);
    return wait > 0 ? wait : 0;
}

#endif
