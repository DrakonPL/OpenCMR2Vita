// Sound on the PS Vita (port/audio.h): the software mixer (audio_mixer.cpp)
// mixes blocks of kBlockFrames frames on a thread of its own, which hands
// them to a BGM port of sceAudioOut as 16-bit stereo. The port takes the
// next block while it plays the one before: sceAudioOutOutput waits until
// the port can take it, which paces the thread.

#include "audio/audio_mixer.h"
#include "port/audio.h"
#include "port/sys.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <string.h>

namespace {

// 512 frames: 11.6 ms at 44100 Hz, so the game's sounds start within some
// 25 ms (the block that plays and the one after it).
constexpr int kBlockFrames = 512;
constexpr int kPriority = 96;               // above the game's threads (160)
constexpr SceSize kStackBytes = 64 * 1024;

int s_port = -1;
SceUID s_thread = -1;
volatile bool s_quit;
volatile bool s_paused;
float s_mix[kBlockFrames * 2];
int16_t s_pcm[2][kBlockFrames * 2];

// The rates a BGM port plays; another rate opens at 48000 Hz.
int PortRate(int rate)
{
    static const int kRates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    for (int r : kRates)
        if (r == rate)
            return rate;
    return 48000;
}

int Entry(SceSize, void *)
{
    int block = 0;
    while (!s_quit) {
        // Paused (the game is not in front): silence, and the sounds keep
        // their places.
        if (s_paused)
            memset(s_mix, 0, sizeof(s_mix));
        else
            AudioMixer_Mix(s_mix, kBlockFrames);
        int16_t *pcm = s_pcm[block];
        for (int i = 0; i < kBlockFrames * 2; i++)
            pcm[i] = (int16_t)(s_mix[i] * 32767.0f);
        sceAudioOutOutput(s_port, pcm);
        block ^= 1;
    }
    // The last block plays to its end.
    sceAudioOutOutput(s_port, NULL);
    return 0;
}

} // namespace

extern "C" BOOL Audio_Init(int sampleRate, int)
{
    if (s_port >= 0)
        return TRUE;
    const int rate = PortRate(sampleRate > 0 ? sampleRate : 44100);
    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, kBlockFrames, rate, SCE_AUDIO_OUT_MODE_STEREO);
    if (s_port < 0) {
        Sys_Log("audio: sceAudioOutOpenPort(%d Hz): 0x%08x", rate, (unsigned)s_port);
        s_port = -1;
        return FALSE;
    }
    int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s_port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                         volume);
    AudioMixer_Start(rate);
    s_quit = false;
    // Core 1: core 0 runs the game, core 2 the renderer's worker.
    s_thread = sceKernelCreateThread("audio_out", Entry, kPriority, kStackBytes, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    const int started = s_thread < 0 ? s_thread : sceKernelStartThread(s_thread, 0, NULL);
    if (started < 0) {
        Sys_Log("audio: no output thread (thread %08x, start %08x)", (unsigned)s_thread, (unsigned)started);
        if (s_thread >= 0)
            sceKernelDeleteThread(s_thread);
        s_thread = -1;
        sceAudioOutReleasePort(s_port);
        s_port = -1;
        return FALSE;
    }
    Sys_Log("audio: %d Hz stereo, blocks of %d frames (asked for %d Hz)", rate, kBlockFrames, sampleRate);
    return TRUE;
}

extern "C" void Audio_SetPaused(BOOL paused)
{
    s_paused = paused != 0;
}

extern "C" void Audio_Shutdown(void)
{
    if (s_thread >= 0) {
        s_quit = true;
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_port >= 0) {
        sceAudioOutReleasePort(s_port);
        s_port = -1;
    }
    // Buffers the game did not release keep their data until exit.
}
