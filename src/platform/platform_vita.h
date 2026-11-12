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

// OpenCMR2 settings (opencmr2.ini in the user directory): "section.key".
const char *Platform_GetSetting(const char *key, const char *fallback);
int Platform_GetSettingInt(const char *key, int fallback);

// Queues an event for Sys_PollEvent (key presses, typed text). Main thread only.
void SysVita_PushEvent(const SysEvent &event);
// True once after the app came back from the LiveArea or from sleep: the
// display queue must be idle before the next draw.
bool Platform_TakeResumed(void);

#endif
