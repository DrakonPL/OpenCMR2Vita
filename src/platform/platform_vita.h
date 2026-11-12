// Internal interface between the src/ subsystems on the PS Vita: platform.h
// without SDL types.
#ifndef OPENCMR2_PLATFORM_VITA_H
#define OPENCMR2_PLATFORM_VITA_H

// Sets up the directories and settings. Returns false (after telling the
// user) when the game data cannot be found.
bool Platform_Init(int argc, char **argv);
void Platform_Shutdown(void);

const char *Platform_GetDataDir(void);
const char *Platform_GetUserDir(void);

// OpenCMR2 settings (opencmr2.ini in the user directory): "section.key".
const char *Platform_GetSetting(const char *key, const char *fallback);
int Platform_GetSettingInt(const char *key, int fallback);

#endif
