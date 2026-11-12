// Sound on the PS Vita (port/audio.h). Stubs: the device does not open and no
// buffer or stream is made.

#include "port/audio.h"

BOOL Audio_Init(int, int) { return FALSE; }
void Audio_Shutdown(void) {}
void Audio_SetPaused(BOOL) {}

AudioBuffer *Audio_CreateBuffer(DWORD, int, int, DWORD) { return NULL; }
AudioBuffer *Audio_DuplicateBuffer(AudioBuffer *) { return NULL; }
void Audio_ReleaseBuffer(AudioBuffer *) {}
DWORD Audio_GetBufferSize(AudioBuffer *) { return 0; }

void Audio_GetBufferFormat(AudioBuffer *, DWORD *sampleRate, int *bits, int *channels)
{
    *sampleRate = 0;
    *bits = 0;
    *channels = 0;
}

BOOL Audio_LockBuffer(AudioBuffer *, DWORD, DWORD, void **, DWORD *, void **, DWORD *) { return FALSE; }
void Audio_UnlockBuffer(AudioBuffer *) {}
void Audio_Play(AudioBuffer *, BOOL) {}
void Audio_Stop(AudioBuffer *) {}
BOOL Audio_IsPlaying(AudioBuffer *) { return FALSE; }
DWORD Audio_GetPlayPosition(AudioBuffer *) { return 0; }
void Audio_SetPlayPosition(AudioBuffer *, DWORD) {}
void Audio_SetVolume(AudioBuffer *, LONG) {}
void Audio_SetPan(AudioBuffer *, LONG) {}
void Audio_SetFrequency(AudioBuffer *, DWORD) {}

AudioStream *Audio_CreateStream(DWORD, int) { return NULL; }
BOOL Audio_QueueStream(AudioStream *, const float *, DWORD) { return FALSE; }
void Audio_StartStream(AudioStream *) {}
void Audio_ReleaseStream(AudioStream *) {}

DWORD Audio_DecodeMsAdpcm(const AudioWaveFormat *, const BYTE *, DWORD, BYTE *, DWORD) { return 0; }
