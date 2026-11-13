// The streamed music where the platform gives it no thread and no read-ahead
// (the PC): the game refills its music from its own thread, and a file reads
// as without Sys_SetFileReadAhead. The PS Vita's are in audio_vita.cpp and
// sys_vita.cpp.

#include "port/audio.h"
#include "port/sys.h"

extern "C" BOOL Audio_StartMusicThread(void (*)(void), DWORD)
{
    return FALSE;
}

extern "C" void Audio_StopMusicThread(void) {}
extern "C" void Audio_LockMusic(void) {}
extern "C" void Audio_UnlockMusic(void) {}

extern "C" void Sys_SetFileReadAhead(SysFile *, DWORD) {}
