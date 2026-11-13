// The software mixer of the PS Vita's sound (src/audio/audio_mixer.cpp): the
// samples of a buffer at the output rate, DirectSound's volume and pan laws,
// a buffer that ends or loops, 8-bit samples, another frequency, a duplicate
// that shares the samples, a movie stream at another rate, and the clamp.

#include "audio/audio_mixer.h"
#include "port/audio.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

// The output of the test: the mixer at 44100 Hz.
extern "C" BOOL Audio_Init(int, int)
{
    AudioMixer_Start(44100);
    return TRUE;
}

namespace {

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

// A 16-bit buffer of samples (each channel the same value).
AudioBuffer *Buffer16(DWORD rate, int channels, const std::vector<short> &samples)
{
    AudioBuffer *b = Audio_CreateBuffer(rate, 16, channels, (DWORD)(samples.size() * 2));
    void *p1, *p2;
    DWORD n1, n2;
    if (b == NULL || !Audio_LockBuffer(b, 0, (DWORD)(samples.size() * 2), &p1, &n1, &p2, &n2))
        return NULL;
    memcpy(p1, samples.data(), n1);
    Audio_UnlockBuffer(b);
    return b;
}

} // namespace

int main()
{
    REQUIRE(Audio_Init(44100, 2) && AudioMixer_Started() && AudioMixer_OutputRate() == 44100);
    float out[64];

    // A mono buffer at the output rate: each frame its sample on both sides.
    // It does not play until Audio_Play, and stops after its last frame.
    AudioBuffer *half = Buffer16(44100, 1, { 16384, 16384, 16384, 16384 });
    REQUIRE(half != NULL);
    AudioMixer_Mix(out, 4);
    REQUIRE(out[0] == 0.0f && out[7] == 0.0f);
    Audio_Play(half, FALSE);
    AudioMixer_Mix(out, 6);
    REQUIRE(Near(out[0], 0.5f) && Near(out[1], 0.5f) && Near(out[6], 0.5f) && Near(out[7], 0.5f));
    REQUIRE(out[8] == 0.0f && out[11] == 0.0f && !Audio_IsPlaying(half));

    // Looping: after its last frame it goes on from its first.
    AudioBuffer *pulse = Buffer16(44100, 1, { 16384, 0, 0, 0 });
    REQUIRE(pulse != NULL);
    Audio_Play(pulse, TRUE);
    AudioMixer_Mix(out, 6);
    REQUIRE(Near(out[0], 0.5f) && out[2] == 0.0f && out[6] == 0.0f && Near(out[8], 0.5f) && out[10] == 0.0f);
    REQUIRE(Audio_IsPlaying(pulse));
    Audio_ReleaseBuffer(pulse);

    // Volume -20 dB is a gain of 0.1; the pan to the right silences the left
    // side; -100 dB is silence.
    Audio_SetPlayPosition(half, 0);
    Audio_SetVolume(half, -2000);
    Audio_Play(half, TRUE);
    AudioMixer_Mix(out, 2);
    REQUIRE(Near(out[0], 0.05f) && Near(out[1], 0.05f));
    Audio_SetVolume(half, 0);
    Audio_SetPan(half, AUDIO_PAN_RIGHT);
    AudioMixer_Mix(out, 2);
    REQUIRE(out[0] == 0.0f && Near(out[1], 0.5f));
    Audio_SetPan(half, -2000);
    AudioMixer_Mix(out, 1);
    REQUIRE(Near(out[0], 0.5f) && Near(out[1], 0.05f));
    Audio_SetPan(half, AUDIO_PAN_CENTER);
    Audio_SetVolume(half, AUDIO_VOLUME_MIN);
    AudioMixer_Mix(out, 1);
    REQUIRE(out[0] == 0.0f && out[1] == 0.0f);
    Audio_Stop(half);

    // A stereo buffer at half the output rate: each sample twice as long,
    // the frames between two samples interpolated.
    AudioBuffer *ramp = Buffer16(22050, 2, { 0, 0, 16384, -16384, 16384, -16384 });
    REQUIRE(ramp != NULL);
    Audio_Play(ramp, FALSE);
    AudioMixer_Mix(out, 3);
    REQUIRE(out[0] == 0.0f && Near(out[2], 0.25f) && Near(out[3], -0.25f) && Near(out[4], 0.5f));
    Audio_Stop(ramp);

    // An 8-bit buffer: 128 is silence, 192 is a half.
    AudioBuffer *bytes = Audio_CreateBuffer(44100, 8, 1, 2);
    void *p1, *p2;
    DWORD n1, n2;
    REQUIRE(bytes != NULL && Audio_LockBuffer(bytes, 0, 2, &p1, &n1, &p2, &n2) && n1 == 2);
    ((BYTE *)p1)[0] = 128;
    ((BYTE *)p1)[1] = 192;
    Audio_Play(bytes, FALSE);
    AudioMixer_Mix(out, 2);
    REQUIRE(out[0] == 0.0f && Near(out[2], 0.5f));

    // A duplicate plays the same samples at the volume of its source; two
    // together are 1, three are clamped to 1.
    Audio_SetVolume(half, -2000);
    AudioBuffer *quiet = Audio_DuplicateBuffer(half);
    Audio_Play(quiet, FALSE);
    AudioMixer_Mix(out, 1);
    REQUIRE(Near(out[0], 0.05f) && Near(out[1], 0.05f));
    Audio_ReleaseBuffer(quiet);
    Audio_SetVolume(half, 0);
    Audio_SetPlayPosition(half, 0);
    AudioBuffer *copy = Audio_DuplicateBuffer(half);
    REQUIRE(copy != NULL && Audio_GetBufferSize(copy) == 8);
    Audio_Play(half, TRUE);
    Audio_Play(copy, TRUE);
    AudioMixer_Mix(out, 1);
    REQUIRE(Near(out[0], 1.0f) && Near(out[1], 1.0f));
    AudioBuffer *third = Audio_DuplicateBuffer(half);
    Audio_Play(third, TRUE);
    AudioMixer_Mix(out, 1);
    REQUIRE(out[0] == 1.0f && out[1] == 1.0f);
    Audio_ReleaseBuffer(third);
    Audio_ReleaseBuffer(copy);
    Audio_Stop(half);

    // A movie stream of 22050 Hz mono: each sample twice, interpolated. It
    // plays after Audio_StartStream, and waits for more samples where they
    // run short.
    AudioStream *movie = Audio_CreateStream(22050, 1);
    REQUIRE(movie != NULL);
    const float samples[4] = { 0.0f, 0.2f, 0.4f, 0.6f };
    REQUIRE(Audio_QueueStream(movie, samples, 4));
    AudioMixer_Mix(out, 2);
    REQUIRE(out[0] == 0.0f && out[2] == 0.0f);
    Audio_StartStream(movie);
    AudioMixer_Mix(out, 8);
    REQUIRE(out[0] == 0.0f && Near(out[2], 0.1f) && Near(out[3], 0.1f) && Near(out[4], 0.2f) &&
            Near(out[10], 0.5f));
    REQUIRE(out[12] == 0.0f && out[15] == 0.0f);
    const float more[2] = { 0.8f, 1.0f };
    REQUIRE(Audio_QueueStream(movie, more, 2));
    AudioMixer_Mix(out, 2);
    REQUIRE(Near(out[0], 0.6f) && Near(out[2], 0.7f));
    Audio_ReleaseStream(movie);
    AudioMixer_Mix(out, 2);
    REQUIRE(out[0] == 0.0f);

    // MS-ADPCM: a mono block gives its two header samples first, oldest
    // first, then a sample for each nibble.
    struct {
        AudioWaveFormat format;
    } wave = {};
    wave.format.wFormatTag = AUDIO_WAVE_FORMAT_ADPCM;
    wave.format.nChannels = 1;
    wave.format.nBlockAlign = 8;
    const BYTE block[8] = { 0, 16, 0, 100, 0, 50, 0, 0x00 };
    short pcm[8] = {};
    const DWORD written = Audio_DecodeMsAdpcm(&wave.format, block, 8, (BYTE *)pcm, sizeof(pcm));
    REQUIRE(written == 4 * 2 && pcm[0] == 50 && pcm[1] == 100);
    // Nibble 0: the prediction 2 * 100 - 50 (coefficients 512, -256 / 256) ...
    // with coefficient set 0 (256, 0): the prediction is the sample before.
    REQUIRE(pcm[2] == 100 && pcm[3] == 100);

    Audio_ReleaseBuffer(half);
    Audio_ReleaseBuffer(ramp);
    Audio_ReleaseBuffer(bytes);
    printf("audio_mixer: ok\n");
    return 0;
}
