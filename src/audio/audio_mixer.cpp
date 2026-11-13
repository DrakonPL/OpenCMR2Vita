// The software mixer of port/audio.h (audio_mixer.h). The buffers mix as in
// audio_sdl.cpp: linear interpolation at their frequency, the volume and the
// pan as DirectSound's hundredths of a decibel. A movie stream keeps its
// samples at its own rate and channels; the mixer reads them at the output
// rate with linear interpolation.

#include "audio/audio_mixer.h"
#include "port/audio.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#ifdef VITA
#include <psp2/kernel/threadmgr.h>
#else
#include <mutex>
#endif

namespace {

struct AudioData {
    BYTE *bytes;
    DWORD size;
    DWORD sampleRate;
    int bits;
    int channels;
    int refs;
};

} // namespace

struct AudioBuffer {
    AudioData *data;
    bool playing = false;
    bool loop = false;
    double position = 0.0;      // in frames
    DWORD frequency;
    float leftGain = 1.0f, rightGain = 1.0f;
    LONG volume = 0, pan = 0;
};

struct AudioStream {
    DWORD rate;
    int channels;
    bool playing = false;
    std::vector<float> samples;     // interleaved; those before first are played
    size_t first = 0;               // the first frame not played, in floats
    double position = 0.0;          // in frames after first
};

namespace {

int s_outputRate = 0;
std::vector<AudioBuffer *> s_buffers;
std::vector<AudioStream *> s_streams;

#ifdef VITA
// Made by AudioMixer_Start, before the output's thread starts. Before it no
// other thread mixes: the calls need no lock.
SceUID s_lock = -1;
struct Lock {
    Lock()
    {
        if (s_lock >= 0)
            sceKernelLockMutex(s_lock, 1, NULL);
    }
    ~Lock()
    {
        if (s_lock >= 0)
            sceKernelUnlockMutex(s_lock, 1);
    }
};
#else
std::mutex s_mutex;
struct Lock {
    Lock() { s_mutex.lock(); }
    ~Lock() { s_mutex.unlock(); }
};
#endif

// DirectSound's laws: volume and pan in hundredths of a decibel.
float DecibelGain(LONG hundredths)
{
    if (hundredths <= -10000)
        return 0.0f;
    return powf(10.0f, hundredths / 2000.0f);
}

void UpdateGains(AudioBuffer *b)
{
    const float volume = DecibelGain(b->volume);
    const float left = b->pan > 0 ? DecibelGain(-b->pan) : 1.0f;
    const float right = b->pan < 0 ? DecibelGain(b->pan) : 1.0f;
    b->leftGain = volume * left;
    b->rightGain = volume * right;
}

inline float SampleAt(const AudioData *d, DWORD frame, int channel)
{
    const int ch = channel < d->channels ? channel : 0;
    if (d->bits == 16) {
        const int16_t *p = (const int16_t *)d->bytes;
        return p[frame * d->channels + ch] / 32768.0f;
    }
    return (d->bytes[frame * d->channels + ch] - 128) / 128.0f;
}

void MixBuffer(AudioBuffer *b, float *out, int frames)
{
    AudioData *d = b->data;
    const DWORD frameBytes = (DWORD)(d->bits / 8 * d->channels);
    const DWORD total = frameBytes ? d->size / frameBytes : 0;
    const double step = (double)b->frequency / s_outputRate;

    if (total == 0) {
        b->playing = false;
        return;
    }
    for (int i = 0; i < frames; i++) {
        if (b->position >= total) {
            if (!b->loop) {
                b->playing = false;
                b->position = 0.0;
                return;
            }
            b->position = fmod(b->position, (double)total);
        }
        const DWORD f0 = (DWORD)b->position;
        const DWORD f1 = f0 + 1 < total ? f0 + 1 : (b->loop ? 0 : f0);
        const float t = (float)(b->position - f0);
        const float l = SampleAt(d, f0, 0) * (1.0f - t) + SampleAt(d, f1, 0) * t;
        const float r = SampleAt(d, f0, 1) * (1.0f - t) + SampleAt(d, f1, 1) * t;
        out[i * 2] += l * b->leftGain;
        out[i * 2 + 1] += r * b->rightGain;
        b->position += step;
    }
}

// A movie stream at the output rate. Its last frame is read only with the
// frame after it: a stream that runs short waits for its next samples.
void MixStream(AudioStream *s, float *out, int frames)
{
    const int channels = s->channels;
    const size_t available = (s->samples.size() - s->first) / (size_t)channels;
    const double step = (double)s->rate / s_outputRate;
    const float *p = s->samples.data() + s->first;
    for (int i = 0; i < frames; i++) {
        const size_t f0 = (size_t)s->position;
        if (f0 + 1 >= available)
            break;
        const float t = (float)(s->position - (double)f0);
        const float *a = p + f0 * channels, *b = a + channels;
        const float l = a[0] * (1.0f - t) + b[0] * t;
        const float r = channels == 2 ? a[1] * (1.0f - t) + b[1] * t : l;
        out[i * 2] += l;
        out[i * 2 + 1] += r;
        s->position += step;
    }
    // The frames played go; the vector moves its samples down at times.
    const size_t played = (size_t)s->position;
    s->first += played * channels;
    s->position -= (double)played;
    if (s->first > 65536 && s->first * 2 > s->samples.size()) {
        s->samples.erase(s->samples.begin(), s->samples.begin() + (ptrdiff_t)s->first);
        s->first = 0;
    }
}

} // namespace

void AudioMixer_Start(int outputRate)
{
#ifdef VITA
    if (s_lock < 0)
        s_lock = sceKernelCreateMutex("audio_mixer", 0, 0, NULL);
#endif
    s_outputRate = outputRate > 0 ? outputRate : 44100;
}

bool AudioMixer_Started(void)
{
    return s_outputRate > 0;
}

int AudioMixer_OutputRate(void)
{
    return s_outputRate;
}

void AudioMixer_Mix(float *out, int frames)
{
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    if (s_outputRate <= 0)
        return;
    {
        Lock lock;
        for (AudioBuffer *b : s_buffers)
            if (b->playing)
                MixBuffer(b, out, frames);
        for (AudioStream *s : s_streams)
            if (s->playing)
                MixStream(s, out, frames);
    }
    for (int i = 0; i < frames * 2; i++)
        out[i] = out[i] > 1.0f ? 1.0f : (out[i] < -1.0f ? -1.0f : out[i]);
}

extern "C" AudioStream *Audio_CreateStream(DWORD sampleRate, int channels)
{
    if (!sampleRate || (channels != 1 && channels != 2))
        return NULL;
    // Boot movies can precede the game's sound initialisation.
    if (!AudioMixer_Started() && !Audio_Init(44100, 2))
        return NULL;
    AudioStream *stream = new AudioStream;
    stream->rate = sampleRate;
    stream->channels = channels;
    Lock lock;
    s_streams.push_back(stream);
    return stream;
}

extern "C" BOOL Audio_QueueStream(AudioStream *stream, const float *samples, DWORD count)
{
    if (!stream || count > 0x7fffffff / sizeof(float))
        return FALSE;
    Lock lock;
    stream->samples.insert(stream->samples.end(), samples, samples + count);
    return TRUE;
}

extern "C" void Audio_StartStream(AudioStream *stream)
{
    if (!stream)
        return;
    Lock lock;
    stream->playing = true;
}

extern "C" void Audio_ReleaseStream(AudioStream *stream)
{
    if (!stream)
        return;
    {
        Lock lock;
        auto it = std::find(s_streams.begin(), s_streams.end(), stream);
        if (it != s_streams.end())
            s_streams.erase(it);
    }
    delete stream;
}

extern "C" AudioBuffer *Audio_CreateBuffer(DWORD sampleRate, int bits, int channels, DWORD bytes)
{
    if ((bits != 8 && bits != 16) || channels < 1 || channels > 2 || bytes == 0)
        return NULL;
    AudioData *d = new AudioData;
    d->bytes = (BYTE *)calloc(1, bytes);
    d->size = bytes;
    d->sampleRate = sampleRate;
    d->bits = bits;
    d->channels = channels;
    d->refs = 1;
    if (bits == 8)
        memset(d->bytes, 0x80, bytes);

    AudioBuffer *b = new AudioBuffer;
    b->data = d;
    b->frequency = sampleRate;
    Lock lock;
    s_buffers.push_back(b);
    return b;
}

extern "C" AudioBuffer *Audio_DuplicateBuffer(AudioBuffer *source)
{
    if (source == NULL)
        return NULL;
    Lock lock;
    AudioBuffer *b = new AudioBuffer;
    b->data = source->data;
    b->data->refs++;
    b->frequency = source->frequency;
    b->volume = source->volume;
    b->pan = source->pan;
    UpdateGains(b);
    s_buffers.push_back(b);
    return b;
}

extern "C" void Audio_ReleaseBuffer(AudioBuffer *buffer)
{
    if (buffer == NULL)
        return;
    Lock lock;
    for (size_t i = 0; i < s_buffers.size(); i++) {
        if (s_buffers[i] == buffer) {
            s_buffers.erase(s_buffers.begin() + i);
            break;
        }
    }
    if (--buffer->data->refs == 0) {
        free(buffer->data->bytes);
        delete buffer->data;
    }
    delete buffer;
}

extern "C" DWORD Audio_GetBufferSize(AudioBuffer *buffer)
{
    return buffer ? buffer->data->size : 0;
}

extern "C" void Audio_GetBufferFormat(AudioBuffer *buffer, DWORD *sampleRate, int *bits, int *channels)
{
    *sampleRate = buffer->data->sampleRate;
    *bits = buffer->data->bits;
    *channels = buffer->data->channels;
}

extern "C" BOOL Audio_LockBuffer(AudioBuffer *buffer, DWORD offset, DWORD bytes, void **p1, DWORD *n1, void **p2,
                                 DWORD *n2)
{
    if (buffer == NULL)
        return FALSE;
    AudioData *d = buffer->data;
    offset %= d->size;
    if (bytes > d->size)
        bytes = d->size;
    *p1 = d->bytes + offset;
    if (offset + bytes <= d->size) {
        *n1 = bytes;
        *p2 = NULL;
        *n2 = 0;
    } else {
        *n1 = d->size - offset;
        *p2 = d->bytes;
        *n2 = bytes - *n1;
    }
    return TRUE;
}

extern "C" void Audio_UnlockBuffer(AudioBuffer *)
{
    // The mixer reads the data in place, as DirectSound's software buffers.
}

extern "C" void Audio_Play(AudioBuffer *buffer, BOOL loop)
{
    if (buffer == NULL)
        return;
    Lock lock;
    buffer->loop = loop != 0;
    buffer->playing = true;
}

extern "C" void Audio_Stop(AudioBuffer *buffer)
{
    if (buffer == NULL)
        return;
    Lock lock;
    buffer->playing = false;
}

extern "C" BOOL Audio_IsPlaying(AudioBuffer *buffer)
{
    if (buffer == NULL)
        return FALSE;
    Lock lock;
    return buffer->playing;
}

extern "C" DWORD Audio_GetPlayPosition(AudioBuffer *buffer)
{
    if (buffer == NULL)
        return 0;
    Lock lock;
    return (DWORD)buffer->position * (DWORD)(buffer->data->bits / 8 * buffer->data->channels);
}

extern "C" void Audio_SetPlayPosition(AudioBuffer *buffer, DWORD position)
{
    if (buffer == NULL)
        return;
    Lock lock;
    buffer->position = (double)(position / (DWORD)(buffer->data->bits / 8 * buffer->data->channels));
}

extern "C" void Audio_SetVolume(AudioBuffer *buffer, LONG volume)
{
    if (buffer == NULL)
        return;
    Lock lock;
    buffer->volume = volume < AUDIO_VOLUME_MIN ? AUDIO_VOLUME_MIN : (volume > 0 ? 0 : volume);
    UpdateGains(buffer);
}

extern "C" void Audio_SetPan(AudioBuffer *buffer, LONG pan)
{
    if (buffer == NULL)
        return;
    Lock lock;
    buffer->pan = pan < AUDIO_PAN_LEFT ? AUDIO_PAN_LEFT : (pan > AUDIO_PAN_RIGHT ? AUDIO_PAN_RIGHT : pan);
    UpdateGains(buffer);
}

extern "C" void Audio_SetFrequency(AudioBuffer *buffer, DWORD frequency)
{
    if (buffer == NULL)
        return;
    Lock lock;
    buffer->frequency = frequency == 0 ? buffer->data->sampleRate : frequency;
}

// ---- MS-ADPCM ---------------------------------------------------------------------

extern "C" DWORD Audio_DecodeMsAdpcm(const AudioWaveFormat *format, const BYTE *src, DWORD srcBytes, BYTE *dst,
                                     DWORD dstBytes)
{
    static const int adaptation[16] = { 230, 230, 230, 230, 307, 409, 512, 614,
                                        768, 614, 512, 409, 307, 230, 230, 230 };
    static const short standardCoefs[7][2] = { { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 },
                                               { 240, 0 }, { 460, -208 }, { 392, -232 } };
    short coefs[32][2];
    int coefCount = 7;
    const int channels = format->nChannels;
    const DWORD blockAlign = format->nBlockAlign;
    DWORD written = 0;

    memcpy(coefs, standardCoefs, sizeof(standardCoefs));
    if (format->cbSize >= 4) {
        const BYTE *extra = (const BYTE *)format + sizeof(AudioWaveFormat);
        const WORD count = (WORD)(extra[2] | extra[3] << 8);
        if (count >= 7 && count <= 32 && format->cbSize >= 4 + count * 4) {
            coefCount = count;
            for (int i = 0; i < count; i++) {
                coefs[i][0] = (short)(extra[4 + i * 4] | extra[5 + i * 4] << 8);
                coefs[i][1] = (short)(extra[6 + i * 4] | extra[7 + i * 4] << 8);
            }
        }
    }
    if (channels < 1 || channels > 2 || blockAlign < (DWORD)(7 * channels))
        return 0;

    int16_t *out = (int16_t *)dst;
    const DWORD outMax = dstBytes / 2;
    for (DWORD offset = 0; offset + blockAlign <= srcBytes; offset += blockAlign) {
        const BYTE *p = src + offset;
        int predictor[2], delta[2], s1[2], s2[2];
        for (int c = 0; c < channels; c++)
            predictor[c] = p[c] < coefCount ? p[c] : 0;
        p += channels;
        for (int c = 0; c < channels; c++, p += 2)
            delta[c] = (int16_t)(p[0] | p[1] << 8);
        for (int c = 0; c < channels; c++, p += 2)
            s1[c] = (int16_t)(p[0] | p[1] << 8);
        for (int c = 0; c < channels; c++, p += 2)
            s2[c] = (int16_t)(p[0] | p[1] << 8);
        // The header's two samples come out oldest first.
        for (int c = 0; c < channels && written < outMax; c++)
            out[written++] = (int16_t)s2[c];
        for (int c = 0; c < channels && written < outMax; c++)
            out[written++] = (int16_t)s1[c];
        const BYTE *end = src + offset + blockAlign;
        int channel = 0;
        for (; p < end && written < outMax; p++) {
            for (int half = 0; half < 2 && written < outMax; half++) {
                const int nibble = half == 0 ? p[0] >> 4 : p[0] & 0xf;
                const int signedNibble = nibble >= 8 ? nibble - 16 : nibble;
                const int c = channel;
                const int prediction = (s1[c] * coefs[predictor[c]][0] + s2[c] * coefs[predictor[c]][1]) / 256;
                int sample = prediction + signedNibble * delta[c];
                sample = sample > 32767 ? 32767 : (sample < -32768 ? -32768 : sample);
                out[written++] = (int16_t)sample;
                s2[c] = s1[c];
                s1[c] = sample;
                delta[c] = delta[c] * adaptation[nibble] / 256;
                if (delta[c] < 16)
                    delta[c] = 16;
                channel = (channel + 1) % channels;
            }
        }
    }
    return written * 2;
}
