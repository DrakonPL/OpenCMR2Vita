// The time of a movie of the PS Vita's hardware decoder. The decoder moves
// its picture on with the sound that the game takes from it. The game took
// the sound as soon as it was decoded and queued it: the picture then ran
// as fast as the display, 60 frames in a second. The sound is taken as it
// plays, a little ahead. A frame that the decoder gives before its time
// waits for it. tests/hw_movie_clock.cpp checks both on the PC.
#ifndef OPENCMR2_HW_MOVIE_CLOCK_H
#define OPENCMR2_HW_MOVIE_CLOCK_H

#include <stdint.h>

// The sound that the game holds ahead of what has played, in ms: two or
// three frames of the decoder's sound. More would put the picture that far
// ahead of the sound. Less, and a long frame of the game leaves a gap.
constexpr double kHwMovieSoundAheadMs = 50.0;

// Whether the game takes more sound from the decoder: queuedMs of sound
// went into the stream, and the stream has played for playedMs (0 before it
// starts).
inline bool HwMovie_SoundWanted(double queuedMs, double playedMs)
{
    return queuedMs - playedMs < kHwMovieSoundAheadMs;
}

// Whether a frame of the decoder shows now. stampMs: its time, firstStampMs:
// that of the first frame, elapsedMs: the time since the movie started (the
// first frame was shown). Before the start each frame shows. A frame more
// than two seconds ahead has no time of this movie (a movie that starts
// again, a stamp that is no time): it shows.
inline bool HwMovie_FrameDue(bool started, uint64_t stampMs, uint64_t firstStampMs, uint32_t elapsedMs)
{
    if (!started || stampMs <= firstStampMs)
        return true;
    const uint64_t at = stampMs - firstStampMs;
    if (at > (uint64_t)elapsedMs + 2000)
        return true;
    return at <= (uint64_t)elapsedMs + 2;
}

#endif
