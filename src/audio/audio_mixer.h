// The software mixer of the PS Vita's sound (audio_mixer.cpp): the buffers and
// the movie streams of port/audio.h, mixed into one stereo stream of floats,
// with the volume and pan laws of DirectSound, as audio_sdl.cpp mixes them on
// the PC. The output (audio_vita.cpp) calls AudioMixer_Mix from its thread.
// No platform calls besides the lock, so tests/audio_mixer.cpp runs it on
// the PC.
#ifndef OPENCMR2_AUDIO_MIXER_H
#define OPENCMR2_AUDIO_MIXER_H

// The output is open: the mixer mixes at outputRate frames each second. The
// lock between the game's thread and the output's thread is made here.
void AudioMixer_Start(int outputRate);
bool AudioMixer_Started(void);
int AudioMixer_OutputRate(void);

// frames of stereo floats (left, right) into out: each buffer that plays and
// each movie stream that started, clamped to -1..1.
void AudioMixer_Mix(float *out, int frames);

#endif
