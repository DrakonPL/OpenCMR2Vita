// The wait of GfxFramePacer (frame_pacing.h) before a present of the SDL
// executors, at the refresh rate of the window's display.
#ifndef OPENCMR2_SDL_FRAME_PACING_H
#define OPENCMR2_SDL_FRAME_PACING_H

#include "render/frame_pacing.h"

#include <SDL3/SDL.h>

// Waits before a present that shows a frame interval refreshes after the
// frame before it. The caller calls pacer.Presented after the present.
inline void Gfx_PaceFrame(const GfxFramePacer &pacer, int interval, SDL_Window *window)
{
    float hz = 60.0f;
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    if (mode != NULL && mode->refresh_rate > 1.0f)
        hz = mode->refresh_rate;
    const uint64_t period = (uint64_t)(1000000.0f / hz);
    const uint64_t wait = pacer.WaitBefore(SDL_GetTicksNS() / 1000, interval, period);
    if (wait > 0)
        SDL_DelayPrecise(wait * 1000);
}

#endif
