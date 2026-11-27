// Internal interface between the src/ subsystems on the PS Vita: platform.h
// without SDL types.
#ifndef OPENCMR2_PLATFORM_VITA_H
#define OPENCMR2_PLATFORM_VITA_H

#include "port/sys.h"

// Sets up the directories and settings. Returns false (after telling the
// user) when the game data cannot be found.
bool Platform_Init(int argc, char **argv);
void Platform_Shutdown(void);

const char *Platform_GetDataDir(void);
const char *Platform_GetUserDir(void);

// Writes a whole file through a temporary renamed over it, so it is never
// left truncated.
bool Platform_SaveFile(const char *path, const void *data, size_t size);

// OpenCMR2 settings (opencmr2.ini in the user directory): "section.key".
const char *Platform_GetSetting(const char *key, const char *fallback);
int Platform_GetSettingInt(const char *key, int fallback);

// Queues an event for Sys_PollEvent (key presses, typed text). Main thread only.
void SysVita_PushEvent(const SysEvent &event);
// True once after the app came back from the LiveArea or from sleep: the
// display queue must be idle before the next draw.
bool Platform_TakeResumed(void);
// One more for each setting that Sys_SetOption changes (a menu choice). The
// renderer reads its settings again when the number changes. Main thread only.
unsigned SysVita_SettingsSerial(void);

#ifdef OPENCMR2_DEV
// Called once per presented frame: every 5 seconds a heartbeat line in the log.
// The line splits a frame into the simulation ticks, the scene (the game's
// draw code with the recorder) and the present (the executor with its wait).
// stats: the performance stats are on (video.perf_stats). Off, the game's
// hooks of the frame parts stop, and the line gives the frame time only.
void SysVita_FramePresented(bool stats);
// The time of one recorded draw, from one draw in kRecorderSampleEvery: the
// heartbeat gives the recorder's part of the scene from them.
constexpr unsigned kRecorderSampleEvery = 16;
unsigned long long SysVita_Micros(void);
void SysVita_RecorderSample(unsigned long long micros);
// The time of one Gfx_DrawBatch call of the game, with its draws.
void SysVita_BatchSample(unsigned long long micros, unsigned draws);
// The us the sound threads of core 1 worked since the start (audio_vita.cpp):
// the mixer without its wait for the port, and the music refills. They wrap
// at 2^32 us: a difference of two reads is right.
void SysVita_AudioBusyMicros(unsigned *mixer, unsigned *music);
// What the recorder got and made since the start: the draw calls of the
// game, the draw commands they became, and the bytes of vertices copied into
// the frames. The heartbeat gives them for each part of the scene.
struct SysVitaDrawCounts {
    unsigned calls, commands, vertexBytes;
};
extern SysVitaDrawCounts g_sysVitaDrawCounts;
#endif

#endif
