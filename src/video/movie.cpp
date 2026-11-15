// Bink 1 playback: decode sequential frames and queue the selected sound track.
#include "port/audio.h"
#include "port/gfx.h"
#include "port/movie.h"
#include "port/sys.h"
#include "video/bink_decoder.h"
#include "video/yuv_convert.h"
#include "video/hw_movie_handle.h"
#include "video/hw_movie_clock.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#ifdef __vita__
#include <psp2/avplayer.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>

#include <malloc.h>
#include <stdlib.h>
#endif

struct HwMovie;

struct Movie {
    SysFile *file = nullptr;
    Bink::Decoder decoder;
    Bink::Frame decoded;
    Bink::Info info;
    DWORD frame = 0, startTicks = 0;
    bool decodedCurrent = false, started = false;
    GfxTexture *texture = nullptr;
    AudioStream *audio = nullptr;
    HwMovie *hw = nullptr;      // the PS Vita's hardware decoder plays it (see HwOpen)
    std::vector<float> pcm;     // the hardware decoder's sound as floats; grows once
};

static bool ReadMovie(void *context, uint32_t offset, void *dst, uint32_t bytes)
{
    SysFile *file = static_cast<SysFile *>(context);
    return offset <= 0x7fffffff && Sys_SeekFile(file, (LONG)offset, SYS_SEEK_SET) == offset &&
           Sys_ReadFile(file, dst, bytes) == bytes;
}

#ifdef OPENCMR2_DEV
// Every 100 frames: the decode with the copy into the texture, average and
// longest in ms, the parts of the decode, and the frame interval of the file.
static void LogDecodeTime(Movie *movie, double ms)
{
    static double total, longest;
    static Bink::Timing parts;
    static unsigned frames;
    const Bink::Timing &t = movie->decoder.GetTiming();
    total += ms;
    longest = ms > longest ? ms : longest;
    parts.audio += t.audio;
    parts.luma += t.luma;
    parts.chroma += t.chroma;
    parts.convert += t.convert;
    if (++frames < 100)
        return;
    Sys_Log("movie: frame %u, decode ms average %.1f max %.1f (audio %.1f, luma %.1f, chroma %.1f, convert %.1f), "
            "interval %.1f", movie->frame + 1, total / frames, longest, parts.audio / frames, parts.luma / frames,
            parts.chroma / frames, parts.convert / frames,
            1000.0 * movie->info.fpsDenominator / movie->info.fpsNumerator);
    total = longest = 0;
    parts = Bink::Timing();
    frames = 0;
}
#endif


#ifdef __vita__
// ---- the PS Vita's hardware decoder ---------------------------------------------
//
// An MP4 file (H.264 and AAC, tools/convert_movies_vita.sh) beside the Bink
// file plays through sceAvPlayer: the decode of a Bink frame took 33 to 40
// ms on the CPU, at 30 frames a second. The CPU converts each frame into the
// texture (video/yuv_convert.h). The decoder keeps the time: Movie_Wait asks
// it for the next frame. [video] movie_hw = 0 plays the Bink files.
// [video] movie_chroma_vu: the order of the chroma bytes of the decoder's
// frames (1: V first).
//
// The decoder's frames are in video memory, and the CPU reads that very
// slowly: the conversion of a frame of 640x480 took 36 ms. The renderer
// draws a frame where it is (Gfx_SetTextureVideoFrame): no conversion and no
// upload. The decoder writes a frame again when the game has taken the next
// one, so the renderer ends its drawing before the game asks for a frame
// (Gfx_FinishDrawing). The decoder moves its picture on with the sound that
// the game takes: the sound is taken as it plays, and a frame waits for its
// time (video/hw_movie_clock.h). [video] movie_gpu = 0 converts on the CPU. A movie
// whose size is not a multiple of 16 is converted on the CPU too: its rows
// in the frame are longer than its width.

struct HwMovie {
    SceAvPlayerHandle handle = 0;
    SceAvPlayerFrameInfo frame{};
    bool frameReady = false;    // a frame that Movie_Wait or the open took
    bool ended = false;
    bool vFirst = true;
    bool gpu = false;           // the renderer draws the frames where they are
    bool gpuDrew = false;       // a frame given to the renderer since its last end of drawing
    bool gpuLogged = false;     // the log has the end of the drawing of the first frame
    // The time of the movie (video/hw_movie_clock.h): the sound that went
    // into the stream, when the stream started to play (below zero: not
    // yet), and the stamp of the first frame.
    double soundQueuedMs = 0.0;
    double soundStart = -1.0;
    uint64_t firstStamp = 0;
    unsigned shown = 0;         // frames given to the renderer
    unsigned width = 0, height = 0;
};

static void *HwAlloc(void *, uint32_t alignment, uint32_t size)
{
    return memalign(alignment < 64 ? 64 : alignment, size);
}

static void HwFree(void *, void *p)
{
    free(p);
}

// The decoder writes its frames itself: each frame buffer is a memory block
// (video memory, else physically contiguous memory), as other players of
// the PS Vita make them.
struct HwFrameBlock {
    void *base;
    SceUID uid;
    size_t bytes;
    bool onGpu;     // the renderer reads it (Gfx_SetTextureVideoFrame)
};
static HwFrameBlock s_hwFrameBlocks[8];

static void *HwAllocFrame(void *, uint32_t, uint32_t size)
{
    for (HwFrameBlock &b : s_hwFrameBlocks) {
        if (b.base != nullptr)
            continue;
        size_t bytes = (size + 0x3ffff) & ~0x3ffffu;
        SceUID uid = sceKernelAllocMemBlock("movie frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, bytes, nullptr);
        if (uid < 0) {
            bytes = (size + 0xfffff) & ~0xfffffu;
            uid = sceKernelAllocMemBlock("movie frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, bytes,
                                         nullptr);
        }
        if (uid < 0 || sceKernelGetMemBlockBase(uid, &b.base) < 0) {
            if (uid >= 0)
                sceKernelFreeMemBlock(uid);
            b.base = nullptr;
            return nullptr;
        }
        b.uid = uid;
        b.bytes = bytes;
        b.onGpu = false;
        return b.base;
    }
    return nullptr;
}

static void HwFreeFrame(void *, void *p)
{
    for (HwFrameBlock &b : s_hwFrameBlocks)
        if (b.base == p && p != nullptr) {
            sceKernelFreeMemBlock(b.uid);
            b.base = nullptr;
        }
}

// The MP4 file of a game path: name.mp4 for track 0, name_N.mp4 for the
// track N (the language of a country's movie), as a host path.
static bool HwPath(const char *path, int audioTrack, char *host, size_t size)
{
    const char *dot = strrchr(path, '.');
    if (dot == nullptr || strlen(dot) != 4)
        return false;
    char name[512];
    const int base = (int)(dot - path);
    if (audioTrack > 0)
        snprintf(name, sizeof(name), "%.*s_%d.mp4", base, path, audioTrack);
    else
        snprintf(name, sizeof(name), "%.*s.mp4", base, path);
    return Sys_ResolvePath(name, FALSE, host, size) != FALSE;
}

// The sound that the decoder has: into the movie's stream (made at the first
// frame of sound, at its rate).
static void HwPumpAudio(Movie *movie)
{
    HwMovie *hw = movie->hw;
    SceAvPlayerFrameInfo sound{};
    // The sound as it plays: the decoder moves its picture on with the
    // sound taken.
    const double now = Sys_GetTicksPrecise();
    if (movie->started && hw->soundStart < 0.0)
        hw->soundStart = now;
    const double played = hw->soundStart >= 0.0 ? now - hw->soundStart : 0.0;
    while (HwMovie_SoundWanted(hw->soundQueuedMs, played) && sceAvPlayerGetAudioData(hw->handle, &sound)) {
        const unsigned channels = sound.details.audio.channelCount;
        const unsigned samples = sound.details.audio.size / 2;
        if (channels == 0 || samples == 0 || sound.pData == nullptr)
            continue;
        if (sound.details.audio.sampleRate > 0)
            hw->soundQueuedMs += 1000.0 * (double)(samples / channels) / (double)sound.details.audio.sampleRate;
        if (movie->audio == nullptr) {
            movie->audio = Audio_CreateStream(sound.details.audio.sampleRate, (int)channels);
            if (movie->audio == nullptr)
                return;
            if (movie->started)
                Audio_StartStream(movie->audio);
        }
        std::vector<float> &pcm = movie->pcm;
        pcm.resize(samples);
        const int16_t *in = (const int16_t *)sound.pData;
        for (unsigned i = 0; i < samples; i++)
            pcm[i] = in[i] / 32768.0f;
        if (!Audio_QueueStream(movie->audio, pcm.data(), samples)) {
            Sys_Log("movie: cannot queue audio");
            return;
        }
    }
}

// The next frame of the decoder, if its time came: kept for Movie_DecodeFrame.
static bool HwTakeFrame(Movie *movie)
{
    HwMovie *hw = movie->hw;
    if (hw->frameReady)
        return true;
    // The decoder can write the frame before again from here on: the
    // renderer ends the drawing that reads it.
    if (hw->gpuDrew) {
        Gfx_FinishDrawing();
        hw->gpuDrew = false;
        // For a log with [system] log_sync = 1: the first frame is drawn.
        if (!hw->gpuLogged)
            Sys_Log("movie: the renderer has drawn the first frame of the hardware decoder");
        hw->gpuLogged = true;
    }
    if (sceAvPlayerGetVideoData(hw->handle, &hw->frame)) {
        hw->frameReady = true;
        return true;
    }
    if (!sceAvPlayerIsActive(hw->handle))
        hw->ended = true;
    return false;
}

static Movie *HwOpen(const char *path, int audioTrack)
{
    char host[1024];
    if (Sys_GetOption("video.movie_hw", 1) == 0 || !HwPath(path, audioTrack, host, sizeof(host)))
        return nullptr;
    static bool loaded;
    if (!loaded) {
        if (sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) < 0) {
            Sys_Log("movie: the AvPlayer module does not load");
            return nullptr;
        }
        loaded = true;
    }
    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = HwAlloc;
    init.memoryReplacement.deallocate = HwFree;
    init.memoryReplacement.allocateTexture = HwAllocFrame;
    init.memoryReplacement.deallocateTexture = HwFreeFrame;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 2;
    init.autoStart = SCE_TRUE;
    // The handle is an address: below zero as an int (hw_movie_handle.h).
    const SceAvPlayerHandle handle = sceAvPlayerInit(&init);
    if (!HwMovie_HandleIsPlayer(handle)) {
        Sys_Log("movie %s: sceAvPlayerInit: 0x%08x", host, (unsigned)handle);
        return nullptr;
    }
    Movie *movie = new (std::nothrow) Movie;
    HwMovie *hw = movie != nullptr ? new (std::nothrow) HwMovie : nullptr;
    if (hw == nullptr) {
        delete movie;
        sceAvPlayerClose(handle);
        return nullptr;
    }
    movie->hw = hw;
    hw->handle = handle;
    hw->vFirst = Sys_GetOption("video.movie_chroma_vu", 1) != 0;
    hw->gpu = Sys_GetOption("video.movie_gpu", 1) != 0;
    const int added = sceAvPlayerAddSource(handle, host);
    // The size of the frames comes with the first one.
    const DWORD start = Sys_GetTicks();
    while (added >= 0 && !HwTakeFrame(movie) && !hw->ended && Sys_GetTicks() - start < 2000)
        Sys_Sleep(2);
    if (added < 0 || !hw->frameReady) {
        Sys_Log("movie %s: no frame from the hardware decoder (0x%08x)", host, (unsigned)added);
        Movie_Close(movie);
        return nullptr;
    }
    hw->width = hw->frame.details.video.width;
    hw->height = hw->frame.details.video.height;
    hw->firstStamp = hw->frame.timeStamp;
    movie->info.width = hw->width;
    movie->info.height = hw->height;
    movie->info.frames = 1;     // Movie_GetFrameCount follows the decoder
    movie->texture = Gfx_CreateTexture(hw->width, hw->height, GFX_FORMAT_BGRX8, 1, GFX_TEXTURE_DYNAMIC);
    if (movie->texture == nullptr) {
        Movie_Close(movie);
        return nullptr;
    }
    if ((hw->width & 15) != 0 || (hw->height & 15) != 0)
        hw->gpu = false;
    Sys_Log("movie %s: hardware decoder, %ux%u, chroma %s first", host, hw->width, hw->height,
            hw->vFirst ? "V" : "U");
    return movie;
}

// The memory block of a frame of the decoder.
static HwFrameBlock *HwBlockOf(const void *frame)
{
    for (HwFrameBlock &b : s_hwFrameBlocks)
        if (b.base != nullptr && (const uint8_t *)frame >= (const uint8_t *)b.base &&
            (const uint8_t *)frame < (const uint8_t *)b.base + b.bytes)
            return &b;
    return nullptr;
}

// The kept frame into the texture: the luma plane, then the chroma at half
// the size, rows of the width rounded up to 16 bytes, the luma plane's
// rows rounded up to 16.
static void HwConvertFrame(Movie *movie)
{
    HwMovie *hw = movie->hw;
    // The renderer draws the frame where it is.
    if (hw->gpu && hw->frame.pData != nullptr) {
        HwFrameBlock *block = HwBlockOf(hw->frame.pData);
        if (block != nullptr && Gfx_SetTextureVideoFrame(movie->texture, hw->frame.pData, hw->vFirst, block->base,
                                                         (DWORD)block->bytes)) {
            if (!block->onGpu)
                Sys_Log("movie: the renderer draws the frames of the hardware decoder (a block of %u KB)",
                        (unsigned)(block->bytes / 1024));
            block->onGpu = true;
            hw->gpuDrew = true;
            hw->frameReady = false;
            movie->decodedCurrent = true;
#ifdef OPENCMR2_DEV
            // The stamp of the frame against the time of the movie.
            if (++hw->shown % 100 == 0)
                Sys_Log("movie: frame %u of the hardware decoder, its time %u ms, shown at %u ms, sound queued %u ms",
                        hw->shown, (unsigned)(hw->frame.timeStamp - hw->firstStamp),
                        movie->started ? (unsigned)(Sys_GetTicks() - movie->startTicks) : 0u,
                        (unsigned)hw->soundQueuedMs);
#endif
            return;
        }
        Sys_Log("movie: the renderer does not draw the frames of the hardware decoder: the CPU converts them");
        hw->gpu = false;
    }
    const size_t stride = (hw->width + 15) & ~15u;
    const size_t rows = (hw->height + 15) & ~15u;
    const uint8_t *luma = hw->frame.pData;
    GfxLockedRect rect;
    if (luma != nullptr && Gfx_LockTexture(movie->texture, 0, 0, &rect)) {
#ifdef OPENCMR2_DEV
        const double start = Sys_GetTicksPrecise();
#endif
        Yuv_SemiPlanarToBgrx(luma, stride, luma + stride * rows, stride, hw->vFirst, hw->width, 0, hw->height,
                             (uint8_t *)rect.pixels, (size_t)rect.pitch);
        Gfx_UnlockTexture(movie->texture, 0, 0);
#ifdef OPENCMR2_DEV
        static double total, longest;
        static unsigned frames;
        const double ms = Sys_GetTicksPrecise() - start;
        total += ms;
        longest = ms > longest ? ms : longest;
        if (++frames == 100) {
            Sys_Log("movie: frame %u, hardware decoder, convert ms average %.1f max %.1f", movie->frame + 1,
                    total / frames, longest);
            total = longest = 0;
            frames = 0;
        }
#endif
    }
    hw->frameReady = false;
    movie->decodedCurrent = true;
}
#endif

extern "C" Movie *Movie_Open(const char *path, int audioTrack)
{
#ifdef __vita__
    if (Movie *hwMovie = HwOpen(path, audioTrack))
        return hwMovie;
#endif
    SysFile *file = Sys_OpenFile(path, SYS_FILE_READ);
    if (!file)
        return nullptr;
    Movie *movie = new (std::nothrow) Movie;
    if (!movie) {
        Sys_CloseFile(file);
        return nullptr;
    }
    movie->file = file;
    // Threads beside the game's for the decode (Bink::Decoder::SetWorkers).
    const unsigned wanted = (unsigned)Sys_GetOption("video.movie_workers", 0);
    const unsigned workers = movie->decoder.SetWorkers(wanted);
    if (workers < wanted)
        Sys_Log("movie %s: %u of %u decode workers: %s", path, workers, wanted, movie->decoder.Error().c_str());
    DWORD size = Sys_GetFileSize(file);
    if (size > 0x7fffffff || !movie->decoder.Open({file, size, ReadMovie}, audioTrack)) {
        Sys_Log("movie %s: %s", path, movie->decoder.Error().c_str());
        Movie_Close(movie);
        return nullptr;
    }
    movie->info = movie->decoder.GetInfo();
    movie->texture = Gfx_CreateTexture(movie->info.width, movie->info.height, GFX_FORMAT_BGRX8, 1,
                                       GFX_TEXTURE_DYNAMIC);
    if (!movie->texture) {
        Sys_Log("movie %s: cannot create frame texture", path);
        Movie_Close(movie);
        return nullptr;
    }
    if (movie->info.channels) {
        movie->audio = Audio_CreateStream(movie->info.sampleRate, movie->info.channels);
        if (!movie->audio)
            Sys_Log("movie %s: audio output unavailable", path);
    }
    Sys_Log("movie %s: %ux%u, %u frames at %u/%u fps, %u Hz/%u channels", path,
            movie->info.width, movie->info.height, movie->info.frames,
            movie->info.fpsNumerator, movie->info.fpsDenominator,
            movie->info.sampleRate, movie->info.channels);
    return movie;
}

extern "C" void Movie_Close(Movie *movie)
{
    if (!movie)
        return;
    Audio_ReleaseStream(movie->audio);
#ifdef __vita__
    // The renderer ends the drawing of the decoder's frames, and reads
    // their memory no longer, before the decoder frees it.
    if (movie->hw != nullptr)
        Gfx_FinishDrawing();
#endif
    Gfx_DestroyTexture(movie->texture);
#ifdef __vita__
    if (movie->hw != nullptr) {
        for (HwFrameBlock &b : s_hwFrameBlocks)
            if (b.base != nullptr && b.onGpu) {
                Gfx_ReleaseVideoMemory(b.base);
                b.onGpu = false;
            }
        sceAvPlayerStop(movie->hw->handle);
        sceAvPlayerClose(movie->hw->handle);
        delete movie->hw;
    }
#endif
    if (movie->file != nullptr)
        Sys_CloseFile(movie->file);
    delete movie;
}

extern "C" DWORD Movie_GetWidth(Movie *movie) { return movie->info.width; }
extern "C" DWORD Movie_GetHeight(Movie *movie) { return movie->info.height; }
extern "C" DWORD Movie_GetFrameNumber(Movie *movie) { return movie->frame + 1; }
extern "C" DWORD Movie_GetFrameCount(Movie *movie)
{
#ifdef __vita__
    // The decoder's movie goes on while it has frames.
    if (movie->hw != nullptr)
        return movie->hw->ended ? movie->frame : movie->frame + 1;
#endif
    return movie->info.frames;
}

extern "C" void Movie_DecodeFrame(Movie *movie)
{
#ifdef __vita__
    if (movie->hw != nullptr) {
        HwPumpAudio(movie);
        if (!movie->decodedCurrent && HwTakeFrame(movie))
            HwConvertFrame(movie);
        return;
    }
#endif
    if (movie->decodedCurrent || movie->frame >= movie->info.frames)
        return;
#ifdef OPENCMR2_DEV
    const double decodeStart = Sys_GetTicksPrecise();
#endif
    if (!movie->decoder.Decode(movie->decoded)) {
        Sys_Log("movie frame %u: %s", movie->frame + 1, movie->decoder.Error().c_str());
        Audio_ReleaseStream(movie->audio);
        movie->audio = nullptr;
        movie->frame = movie->info.frames;
        movie->started = false;
        return;
    }
    movie->decodedCurrent = true;
    if (movie->audio && !movie->decoded.audio.empty() &&
        !Audio_QueueStream(movie->audio, movie->decoded.audio.data(), movie->decoded.audio.size())) {
        Sys_Log("movie: cannot queue audio");
        Audio_ReleaseStream(movie->audio);
        movie->audio = nullptr;
    }
    GfxLockedRect rect;
    if (Gfx_LockTexture(movie->texture, 0, 0, &rect)) {
        for (DWORD y = 0; y < movie->info.height; ++y)
            memcpy((BYTE *)rect.pixels + y * rect.pitch,
                   movie->decoded.bgrx.data() + size_t(y) * movie->info.width * 4, movie->info.width * 4);
        Gfx_UnlockTexture(movie->texture, 0, 0);
    }
#ifdef OPENCMR2_DEV
    LogDecodeTime(movie, Sys_GetTicksPrecise() - decodeStart);
#endif
}

extern "C" void Movie_NextFrame(Movie *movie)
{
#ifdef __vita__
    if (movie->hw != nullptr) {
        ++movie->frame;
        movie->decodedCurrent = false;
        return;
    }
#endif
    if (movie->frame < movie->info.frames)
        ++movie->frame;
    movie->decodedCurrent = false;
}

extern "C" BOOL Movie_Wait(Movie *movie)
{
#ifdef __vita__
    // The decoder's time: wait until it gives the next frame or ends.
    if (movie->hw != nullptr) {
        HwPumpAudio(movie);
        if (movie->hw->ended)
            return FALSE;
        if (!HwTakeFrame(movie))
            return movie->hw->ended ? FALSE : TRUE;
        // The frame is here: it waits for its time.
        return HwMovie_FrameDue(movie->started, movie->hw->frame.timeStamp, movie->hw->firstStamp,
                                movie->started ? Sys_GetTicks() - movie->startTicks : 0)
                   ? FALSE
                   : TRUE;
    }
#endif
    if (!movie->started)
        return FALSE;
    DWORD due = (DWORD)(uint64_t(movie->frame) * 1000 * movie->info.fpsDenominator / movie->info.fpsNumerator);
    return Sys_GetTicks() - movie->startTicks < due;
}

extern "C" void Movie_Present(Movie *movie, int x, int y, int width, int height)
{
    // The game caches its render-state setters. Leave their actual values
    // intact so the next menu can still blend its fonts and sprites.
    Gfx_PushState();
    // Keep the movie's proportions within the rectangle, with black bars,
    // unless the settings ask to fill the screen.
    const bool stretch = Sys_GetOption("video.stretch_movies", 0) != 0;
    if (!stretch && movie->info.width && movie->info.height && width > 0 && height > 0) {
        const int64_t fitted = int64_t(height) * movie->info.width / movie->info.height;
        if (fitted <= width) {
            x += (width - int(fitted)) / 2;
            width = int(fitted);
        } else {
            const int fittedHeight = int(int64_t(width) * movie->info.height / movie->info.width);
            y += (height - fittedHeight) / 2;
            height = fittedHeight;
        }
    }
    GfxTLVertex v[4];
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + width), y1 = (float)(y + height);

    memset(v, 0, sizeof(v));
    for (int i = 0; i < 4; i++) {
        v[i].rhw = 1.0f;
        v[i].color = 0xffffffff;
    }
    v[0].sx = x0; v[0].sy = y0; v[0].tu = 0.0f; v[0].tv = 0.0f;
    v[1].sx = x1; v[1].sy = y0; v[1].tu = 1.0f; v[1].tv = 0.0f;
    v[2].sx = x0; v[2].sy = y1; v[2].tu = 0.0f; v[2].tv = 1.0f;
    v[3].sx = x1; v[3].sy = y1; v[3].tu = 1.0f; v[3].tv = 1.0f;

    Gfx_Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER, 0xff000000, 1.0f);
    Gfx_BeginScene();
    Gfx_SetDepthTest(FALSE);
    Gfx_SetAlphaBlend(FALSE);
    Gfx_SetAlphaTest(FALSE);
    Gfx_SetCullMode(GFX_CULL_NONE);
    Gfx_SetTexture(0, movie->texture);
    Gfx_SetStageColorOp(0, GFX_TOP_SELECTARG1);
    Gfx_SetStageColorArg1(0, GFX_TA_TEXTURE);
    Gfx_SetStageAlphaOp(0, GFX_TOP_SELECTARG1);
    Gfx_SetStageAlphaArg1(0, GFX_TA_TEXTURE);
    Gfx_SetStageColorOp(1, GFX_TOP_DISABLE);
    Gfx_SetStageAlphaOp(1, GFX_TOP_DISABLE);
    Gfx_SetStageMinFilter(0, GFX_FILTER_LINEAR);
    Gfx_SetStageMagFilter(0, GFX_FILTER_LINEAR);
    Gfx_SetStageAddress(0, GFX_ADDRESS_CLAMP);
    Gfx_DrawPrimitiveTL(GFX_TRIANGLESTRIP, v, 4);
    Gfx_EndScene();
    if (stretch)
        Gfx_PresentStretched(TRUE);
    else
        Gfx_Present(TRUE);
    Gfx_PopState();
    if (!movie->started && movie->decodedCurrent) {
        movie->startTicks = Sys_GetTicks();
        movie->started = true;
        Audio_StartStream(movie->audio);
    }
}
