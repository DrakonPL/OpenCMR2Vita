// Sound on the PS Vita (port/audio.h): the software mixer (audio_mixer.cpp)
// mixes blocks of kBlockFrames frames on a thread of its own, which hands
// them to a BGM port of sceAudioOut as 16-bit stereo. The port takes the
// next block while it plays the one before: sceAudioOutOutput waits until
// the port can take it, which paces the thread.

#include "audio/audio_mixer.h"
#include "platform/platform_vita.h"
#include "port/audio.h"
#include "port/sys.h"

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
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
// SysVita_AudioBusyMicros: each written by one thread only.
volatile uint32_t s_mixerBusyMicros, s_musicBusyMicros;

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
    int lastError = 0;
    while (!s_quit) {
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        // Paused (the game is not in front): silence, and the sounds keep
        // their places.
        if (s_paused)
            memset(s_mix, 0, sizeof(s_mix));
        else
            AudioMixer_Mix(s_mix, kBlockFrames);
        int16_t *pcm = s_pcm[block];
        for (int i = 0; i < kBlockFrames * 2; i++)
            pcm[i] = (int16_t)(s_mix[i] * 32767.0f);
        s_mixerBusyMicros += (uint32_t)(sceKernelGetProcessTimeWide() - start);
        // A port that goes bad (after a suspend, say) logs once for each new error.
        const int result = sceAudioOutOutput(s_port, pcm);
        if (result < 0 && result != lastError)
            Sys_Log("audio: sceAudioOutOutput: 0x%08x", (unsigned)result);
        lastError = result < 0 ? result : 0;
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

// ---- the music thread ----
// The game's streamed music refills its buffer here: a 16 KB block of the
// file read and decoded for each part of the buffer that played (some 0.4
// s of music). On the game's thread that was a hitch at times. The thread
// takes the music lock for each refill; the game takes it while it opens,
// starts, pauses or closes the music.

namespace {

constexpr int kMusicPriority = 128;     // below the output (96), above the game (160)
constexpr SceSize kMusicStackBytes = 64 * 1024;     // the refill has a 16 KB block on its stack

SceUID s_musicLock = -1;
SceUID s_musicThread = -1;
volatile bool s_musicQuit;
void (*s_musicRefill)(void);
SceUInt s_musicPeriodMicros;

int MusicEntry(SceSize, void *)
{
    while (!s_musicQuit) {
        sceKernelDelayThread(s_musicPeriodMicros);
        if (s_musicQuit)
            break;
        Audio_LockMusic();
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        s_musicRefill();
        s_musicBusyMicros += (uint32_t)(sceKernelGetProcessTimeWide() - start);
        Audio_UnlockMusic();
    }
    return 0;
}

} // namespace

#ifdef OPENCMR2_DEV
void SysVita_AudioBusyMicros(unsigned *mixer, unsigned *music)
{
    *mixer = s_mixerBusyMicros;
    *music = s_musicBusyMicros;
}
#endif

extern "C" BOOL Audio_StartMusicThread(void (*refill)(void), DWORD periodMs)
{
    if (s_musicThread >= 0)
        return TRUE;
    if (refill == NULL || s_port < 0)
        return FALSE;
    // [audio] music_thread = 0: the game's thread refills, as before.
    if (Platform_GetSettingInt("audio.music_thread", 1) == 0) {
        Sys_Log("audio: the music refills on the game's thread (audio.music_thread = 0)");
        return FALSE;
    }
    if (s_musicLock < 0)
        s_musicLock = sceKernelCreateMutex("audio_music", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    if (s_musicLock < 0) {
        Sys_Log("audio: no music lock (0x%08x): the music refills on the game's thread", (unsigned)s_musicLock);
        return FALSE;
    }
    s_musicRefill = refill;
    s_musicPeriodMicros = (SceUInt)(periodMs > 0 ? periodMs : 20) * 1000;
    s_musicQuit = false;
    s_musicThread = sceKernelCreateThread("audio_music", MusicEntry, kMusicPriority, kMusicStackBytes, 0,
                                          SCE_KERNEL_CPU_MASK_USER_1, NULL);
    const int started = s_musicThread < 0 ? s_musicThread : sceKernelStartThread(s_musicThread, 0, NULL);
    if (started < 0) {
        Sys_Log("audio: no music thread (thread %08x, start %08x): the music refills on the game's thread",
                (unsigned)s_musicThread, (unsigned)started);
        if (s_musicThread >= 0)
            sceKernelDeleteThread(s_musicThread);
        s_musicThread = -1;
        return FALSE;
    }
    Sys_Log("audio: the music refills on a thread of its own, every %u ms", (unsigned)(s_musicPeriodMicros / 1000));
    return TRUE;
}

extern "C" void Audio_StopMusicThread(void)
{
    if (s_musicThread < 0)
        return;
    s_musicQuit = true;
    sceKernelWaitThreadEnd(s_musicThread, NULL, NULL);
    sceKernelDeleteThread(s_musicThread);
    s_musicThread = -1;
}

extern "C" void Audio_LockMusic(void)
{
    if (s_musicLock >= 0)
        sceKernelLockMutex(s_musicLock, 1, NULL);
}

extern "C" void Audio_UnlockMusic(void)
{
    if (s_musicLock >= 0)
        sceKernelUnlockMutex(s_musicLock, 1);
}

extern "C" void Audio_Shutdown(void)
{
    Audio_StopMusicThread();
    if (s_thread >= 0) {
        s_quit = true;
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
        Sys_Log("audio: thread joined");
    }
    if (s_port >= 0) {
        sceAudioOutReleasePort(s_port);
        s_port = -1;
    }
    AudioMixer_Stop();
    // Buffers the game did not release keep their data until exit.
}
