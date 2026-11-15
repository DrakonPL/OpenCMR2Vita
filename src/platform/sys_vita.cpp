// OpenCMR2 system services on the PS Vita: see port/sys.h. Game data in
// ux0:data/OpenCMR2/, saves, settings and the log in its user/ folder.

#include "platform/log_buffer.h"
#include "platform/platform_vita.h"
#include "platform/read_ahead.h"
#include "platform/sys_common.h"
#include "platform/sys_event_ring.h"
#include "platform/sys_paths.h"
#include "port/diagnostics.h"
#include "port/sys.h"
#include "render/gxm_dialog.h"
#include "render/gxm_memory.h"

#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <string>
#include <vector>

namespace {

const char kDataDir[] = "ux0:data/OpenCMR2/";
const char kUserDir[] = "ux0:data/OpenCMR2/user/";

// Defaults that differ from the PC's. opencmr2.ini overrides them.
// outline_fonts: fitting the outlines to a font takes about 20 s on the
// Vita at every start. 0 draws the original's bitmap fonts.
// vector_icons: the trace of the co-driver arrows takes about 48 s on the
// Vita at the first stage load of every start, that of the menu icons 4 s
// and that of an arcade map 11 s. 0 draws the original's bitmaps.
// movie_workers: the Bink decode uses the two other cores.
// relight_budget: when the light of the scene changes, the sectors in view
// are lit again over some frames, not in one of 50 ms (RelightBudget.h).
const char kVitaDefaults[] =
    "[video]\n"
    "outline_fonts = 0\n"
    "vector_icons = 0\n"
    "movie_workers = 2\n"
    "relight_budget = 10000\n";

SysSettings::Values s_settings;
SysEventRing s_events;
// The file work of all threads, for the line of a long frame (dev builds):
// opens, reads, bytes read, and the time in them.
std::atomic<unsigned> s_fileOpens, s_fileReads, s_fileReadBytes, s_fileMicros;
bool s_resumed;
std::string s_lastMissingFile;  // game path of the last file that could not be read
int s_openFiles;

// ---- host files -----------------------------------------------------------------

// sceIo takes directories without the trailing '/'.
std::string WithoutSlash(const std::string &path)
{
    return !path.empty() && path.back() == '/' ? path.substr(0, path.size() - 1) : path;
}

struct VitaHost : SysPaths::Host {
    void List(const std::string &dir, SysPaths::Entries &entries) override
    {
        SceUID fd = sceIoDopen(WithoutSlash(dir).c_str());
        if (fd < 0)
            return;
        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));
        while (sceIoDread(fd, &entry) > 0) {
            entries.emplace(SysPaths::Lower(entry.d_name), entry.d_name);
            memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(fd);
    }
    void MakeDirectory(const std::string &path) override { sceIoMkdir(WithoutSlash(path).c_str(), 0777); }
    SysPaths::Kind GetKind(const std::string &path, uint64_t *size) override
    {
        SceIoStat stat;
        memset(&stat, 0, sizeof(stat));
        if (sceIoGetstat(WithoutSlash(path).c_str(), &stat) < 0)
            return SysPaths::Kind::None;
        if (size != NULL)
            *size = (uint64_t)stat.st_size;
        return SCE_S_ISDIR(stat.st_mode) ? SysPaths::Kind::Directory
               : SCE_S_ISREG(stat.st_mode) ? SysPaths::Kind::File
                                           : SysPaths::Kind::None;
    }
};

VitaHost s_host;
SysPaths::Cache s_dirs(s_host);

// The whole file, or false. Rejects a failed open or seek before it sizes
// the buffer: an sceIo error code is a negative number.
bool ReadWholeFile(const std::string &path, std::string &out)
{
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0)
        return false;
    const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    bool ok = size >= 0 && size < 64 * 1024 * 1024 && sceIoLseek(fd, 0, SCE_SEEK_SET) == 0;
    if (ok) {
        out.resize((size_t)size);
        size_t done = 0;
        while (done < out.size()) {
            int n = sceIoRead(fd, &out[done], out.size() - done);
            if (n <= 0)
                break;
            done += (size_t)n;
        }
        ok = done == out.size();
    }
    sceIoClose(fd);
    return ok;
}

bool WriteWholeFile(const std::string &path, const std::string &data)
{
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < data.size()) {
        int n = sceIoWrite(fd, data.data() + done, data.size() - done);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    sceIoClose(fd);
    return done == data.size();
}

// ---- log ------------------------------------------------------------------------
// A write to the file takes some 13 ms on the memory card. A dev build wrote
// each line at once, on the thread that logged it: some 5 lines in a second
// of a race, most of them from the game's thread (70 ms of each second, and
// a frame of 100 ms with the lines of each heartbeat). The lines now collect
// in memory (log_buffer.h), and a thread of its own on core 1 writes them to
// the file once per second. A crash loses the lines of its last second:
// [system] log_sync = 1 writes each line at once, as before.

constexpr size_t kLogBufferBytes = 32 * 1024;       // some 20 s of the lines of a race
constexpr SceUInt kLogWriteMicros = 1000000;
constexpr int kLogPriority = 160;                   // below the sound (96, 128), which has core 1 too
constexpr SceSize kLogStackBytes = 16 * 1024;

LogBuffer<kLogBufferBytes> s_logLines;
SceUID s_logFile = -1;
// The renderer's worker thread logs too (render/gfx_threaded.cpp): a line
// goes into the buffer with this mutex held. Made in Platform_Init, before
// that thread starts.
SceUID s_logMutex = -1;
// Held for each write to the file, and taken before s_logMutex: the lines
// go to the file in their order from the writer thread and from FlushLog,
// and the buffer that one thread writes is not taken again. Made with
// s_logMutex.
SceUID s_logIoMutex = -1;
SceUID s_logThread = -1, s_logWake = -1;
volatile bool s_logQuit;
bool s_logSync = true;      // until the writer thread runs

// Writes the lines so far to the file, on the thread that calls it.
void FlushLog()
{
    if (s_logIoMutex >= 0)
        sceKernelLockMutex(s_logIoMutex, 1, NULL);
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, NULL);
    size_t bytes;
    const char *lines = s_logLines.Take(&bytes);
    const unsigned lost = s_logLines.TakeLost();
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
    if (s_logFile >= 0) {
        if (bytes > 0)
            sceIoWrite(s_logFile, lines, bytes);
        if (lost > 0) {
            char text[64];
            const int length = snprintf(text, sizeof(text), "log: %u lines lost (the buffer was full)\n", lost);
            sceIoWrite(s_logFile, text, (SceSize)length);
        }
    }
    if (s_logIoMutex >= 0)
        sceKernelUnlockMutex(s_logIoMutex, 1);
}

int LogWriterEntry(SceSize, void *)
{
    while (!s_logQuit) {
        SceUInt timeout = kLogWriteMicros;
        sceKernelWaitSema(s_logWake, 1, &timeout);
        FlushLog();
    }
    return 0;
}

// Starts the writer thread. Without it each line is written at once.
void StartLogWriter()
{
    s_logWake = sceKernelCreateSema("opencmr2_log", 0, 0, 1, NULL);
    if (s_logIoMutex < 0 || s_logWake < 0)
        return;
    s_logQuit = false;
    s_logThread = sceKernelCreateThread("log_writer", LogWriterEntry, kLogPriority, kLogStackBytes, 0,
                                        SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (s_logThread < 0 || sceKernelStartThread(s_logThread, 0, NULL) < 0) {
        if (s_logThread >= 0)
            sceKernelDeleteThread(s_logThread);
        s_logThread = -1;
        return;
    }
    s_logSync = false;
}

void StopLogWriter()
{
    if (s_logThread < 0)
        return;
    s_logQuit = true;
    sceKernelSignalSema(s_logWake, 1);
    sceKernelWaitThreadEnd(s_logThread, NULL, NULL);
    sceKernelDeleteThread(s_logThread);
    s_logThread = -1;
    s_logSync = true;
}

// ---- message dialog -------------------------------------------------------------
// A system dialog draws only while the app calls sceCommonDialogUpdate with a
// display buffer before each flip. Before the renderer starts GXM, the dialog
// gets its own GXM display while open. After that it draws through the
// renderer's display (render/gxm_dialog.h): GXM starts once in a process.

constexpr int kDisplayWidth = 960;
constexpr int kDisplayHeight = 544;
constexpr int kDisplayStride = 1024;   // pixels
constexpr int kDisplayBuffers = 2;

struct DialogBuffer {
    GxmBlock memory;
    void *data;
    SceGxmSyncObject *sync;
};

void SetDisplayBuffer(const void *callbackData)
{
    SceDisplayFrameBuf frame{};
    frame.size = sizeof(frame);
    frame.base = *(void *const *)callbackData;
    frame.pitch = kDisplayStride;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = kDisplayWidth;
    frame.height = kDisplayHeight;
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

bool OpenDialogDisplay(DialogBuffer (&buffers)[kDisplayBuffers])
{
    SceGxmInitializeParams params{};
    params.displayQueueMaxPendingCount = 1;
    params.displayQueueCallback = SetDisplayBuffer;
    params.displayQueueCallbackDataSize = sizeof(void *);
    params.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if (sceGxmInitialize(&params) < 0)
        return false;
    const unsigned size = kDisplayStride * kDisplayHeight * 4;
    for (DialogBuffer &b : buffers) {
        Gxm_AllocGpu(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size,
                     (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE), b.memory);
        b.data = b.memory.data;
        if (b.data != nullptr)
            memset(b.data, 0, size);
        sceGxmSyncObjectCreate(&b.sync);
    }
    return true;
}

void CloseDialogDisplay(DialogBuffer (&buffers)[kDisplayBuffers])
{
    sceGxmDisplayQueueFinish();
    for (DialogBuffer &b : buffers) {
        sceGxmSyncObjectDestroy(b.sync);
        Gxm_FreeGpu(b.memory);
    }
    sceGxmTerminate();
}

// Returns the button the player chose, SCE_MSG_DIALOG_BUTTON_ID_INVALID if
// the dialog could not open.
int RunMessageDialog(const char *text, int buttonType)
{
    static bool configured;
    if (!configured) {
        SceAppUtilInitParam init{};
        SceAppUtilBootParam boot{};
        sceAppUtilInit(&init, &boot);
        SceCommonDialogConfigParam config{};
        sceCommonDialogConfigParamInit(&config);
        sceCommonDialogSetConfigParam(&config);
        configured = true;
    }
    DialogBuffer buffers[kDisplayBuffers];
    GxmDialogPresenter *renderer = Gxm_DialogPresenter();
    if (renderer != nullptr)
        renderer->BeginDialog();
    else if (!OpenDialogDisplay(buffers))
        return SCE_MSG_DIALOG_BUTTON_ID_INVALID;

    SceMsgDialogUserMessageParam message{};
    message.buttonType = buttonType;
    message.msg = (const SceChar8 *)text;
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &message;
    int button = SCE_MSG_DIALOG_BUTTON_ID_INVALID;
    if (sceMsgDialogInit(&param) >= 0) {
        int back = 0, front = 0;
        while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
            if (renderer != nullptr) {
                renderer->DialogFrame();
                continue;
            }
            SceCommonDialogUpdateParam update{};
            update.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
            update.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
            update.renderTarget.width = kDisplayWidth;
            update.renderTarget.height = kDisplayHeight;
            update.renderTarget.strideInPixels = kDisplayStride;
            update.renderTarget.colorSurfaceData = buffers[back].data;
            update.displaySyncObject = buffers[back].sync;
            sceCommonDialogUpdate(&update);
            sceGxmDisplayQueueAddEntry(buffers[front].sync, buffers[back].sync, &buffers[back].data);
            front = back;
            back = (back + 1) % kDisplayBuffers;
            sceDisplayWaitVblankStart();
        }
        SceMsgDialogResult result{};
        sceMsgDialogGetResult(&result);
        button = result.buttonId;
        sceMsgDialogTerm();
    }
    if (renderer == nullptr)
        CloseDialogDisplay(buffers);
    return button;
}

// ---- system events --------------------------------------------------------------

void PushFocusEvent(int type)
{
    SysEvent event = {};
    event.type = type;
    s_events.Push(event);
    Sys_Log("system: %s", type == SYS_EVENT_FOCUS_LOST ? "FOCUS_LOST" : "FOCUS_GAINED");
}

void PollSystem()
{
    // Auto-sleep tears down the GXM context under the game: keep the console
    // awake while the game runs. The power button still sleeps it.
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    SceAppMgrSystemEvent event;
    memset(&event, 0, sizeof(event));
    if (sceAppMgrReceiveSystemEvent(&event) < 0 || event.systemEvent != SCE_APPMGR_SYSTEMEVENT_ON_RESUME)
        return;
    // The app is frozen while the LiveArea or sleep is on, so both events
    // come on the return: the game stops and restarts its music and logic.
    PushFocusEvent(SYS_EVENT_FOCUS_LOST);
    PushFocusEvent(SYS_EVENT_FOCUS_GAINED);
    s_resumed = true;
}

#ifdef OPENCMR2_DEV
// The resolver end to end: a mixed-case DOS path read through Sys_OpenFile.
void LogFileProbe()
{
    const char *gamePath = "game\\BIGFILES\\Common.bfl";
    char host[1024] = "";
    Sys_ResolvePath(gamePath, FALSE, host, sizeof(host));
    SysFile *file = Sys_OpenFile(gamePath, SYS_FILE_READ);
    const DWORD size = file != NULL ? Sys_GetFileSize(file) : 0xffffffff;
    BYTE head[4] = {};
    const DWORD read = file != NULL ? Sys_ReadFile(file, head, sizeof(head)) : 0;
    Sys_CloseFile(file);
    Sys_Log("probe: %s -> %s, %u bytes, read %u", gamePath, host, (unsigned)size, (unsigned)read);
}
#endif

} // namespace

#ifdef OPENCMR2_DEV
namespace {

// The parts of the frames of a heartbeat period, from the game's hooks: the
// simulation ticks (the game runs more of them in a frame that is long), the
// present, and the whole frame. A present inside a tick is not tick time.
struct FrameParts : Diagnostics::Observer {
    uint64_t tickStart = 0, presentStart = 0, frameStart = 0;
    uint64_t tickMicros = 0, presentMicros = 0, frameMicros = 0, recorderMicros = 0;
    uint64_t batchMicros = 0, batchDraws = 0;   // the Gfx_DrawBatch calls
    unsigned ticks = 0;
    bool inTick = false;
    // The parts of the scene: the time of each, and the part that runs now.
    uint64_t partMicros[Diagnostics::SCENE_PART_COUNT] = {};
    uint64_t partStart = 0;
    int part = Diagnostics::SCENE_PART_NONE;
    // The draws of each part (SCENE_PART_NONE: outside the parts), and the
    // recorder's counts at the start of the part that runs now.
    SysVitaDrawCounts partDraws[Diagnostics::SCENE_PART_COUNT] = {};
    SysVitaDrawCounts partDrawsStart = {};
    void ScenePart(int next) override
    {
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (part != Diagnostics::SCENE_PART_NONE)
            partMicros[part] += now - partStart;
        const SysVitaDrawCounts counts = g_sysVitaDrawCounts;
        partDraws[part].calls += counts.calls - partDrawsStart.calls;
        partDraws[part].commands += counts.commands - partDrawsStart.commands;
        partDraws[part].vertexBytes += counts.vertexBytes - partDrawsStart.vertexBytes;
        partDrawsStart = counts;
        part = next >= 0 && next < Diagnostics::SCENE_PART_COUNT ? next : Diagnostics::SCENE_PART_NONE;
        partStart = now;
    }
    void BeginTick(unsigned char) override
    {
        tickStart = sceKernelGetProcessTimeWide();
        inTick = true;
    }
    void EndTick() override
    {
        tickMicros += sceKernelGetProcessTimeWide() - tickStart;
        ticks++;
        inTick = false;
    }
    void BeginPresent() override
    {
        // A present is no part of the scene.
        ScenePart(Diagnostics::SCENE_PART_NONE);
        presentStart = sceKernelGetProcessTimeWide();
    }
    void EndPresent() override
    {
        const uint64_t spent = sceKernelGetProcessTimeWide() - presentStart;
        presentMicros += spent;
        if (inTick)
            tickStart += spent;
    }
    void BeginFrame() override { frameStart = sceKernelGetProcessTimeWide(); }
    void EndFrame() override { frameMicros += sceKernelGetProcessTimeWide() - frameStart; }
    int Part() const override { return part; }
    void Reset()
    {
        tickMicros = presentMicros = frameMicros = recorderMicros = 0;
        batchMicros = batchDraws = 0;
        ticks = 0;
        memset(partMicros, 0, sizeof(partMicros));
        memset(partDraws, 0, sizeof(partDraws));
    }
};
FrameParts s_frameParts;

// The counts of the frame parts and of the file work at the end of the last
// frame: a long frame's line gives its own share.
struct FrameSnapshot {
    uint64_t tick = 0, recorder = 0;
    unsigned ticks = 0;
    uint64_t parts[Diagnostics::SCENE_PART_COUNT] = {};
    unsigned fileOpens = 0, fileReads = 0, fileBytes = 0, fileMicros = 0;
};

FrameSnapshot SnapshotOf(const FrameParts &p)
{
    FrameSnapshot s;
    s.tick = p.tickMicros;
    s.recorder = p.recorderMicros;
    s.ticks = p.ticks;
    memcpy(s.parts, p.partMicros, sizeof(s.parts));
    s.fileOpens = s_fileOpens.load(std::memory_order_relaxed);
    s.fileReads = s_fileReads.load(std::memory_order_relaxed);
    s.fileBytes = s_fileReadBytes.load(std::memory_order_relaxed);
    s.fileMicros = s_fileMicros.load(std::memory_order_relaxed);
    return s;
}

// A frame longer than this gives a line of its own parts, at most
// kLongFramesPerPeriod in a heartbeat period: the hitches of a race.
constexpr double kLongFrameMs = 50.0;
constexpr unsigned kLongFramesPerPeriod = 4;

void LogLongFrame(double ms, const FrameSnapshot &before, const FrameParts &p, uint64_t presentMicros)
{
    const FrameSnapshot after = SnapshotOf(p);
    // The three parts of the scene that took the most of the frame.
    int top[3] = { -1, -1, -1 };
    uint64_t partsMicros = 0;
    for (int i = 1; i < Diagnostics::SCENE_PART_COUNT; i++) {
        const uint64_t spent = after.parts[i] - before.parts[i];
        partsMicros += spent;
        for (int k = 0; k < 3; k++) {
            if (top[k] < 0 || spent > after.parts[top[k]] - before.parts[top[k]]) {
                for (int m = 2; m > k; m--)
                    top[m] = top[m - 1];
                top[k] = i;
                break;
            }
        }
    }
    char parts[160];
    int used = 0;
    for (int k = 0; k < 3 && top[k] >= 0; k++)
        used += snprintf(parts + used, sizeof(parts) - (size_t)used, "%s%s %.1f", k ? ", " : "",
                         Diagnostics::kScenePartNames[top[k]], (after.parts[top[k]] - before.parts[top[k]]) / 1000.0);
    const double tick = (after.tick - before.tick) / 1000.0, present = presentMicros / 1000.0;
    const double scene = partsMicros / 1000.0;
    Sys_Log("long frame: %.1f ms: ticks %.1f (%u), scene parts %.1f (%s), recorder %.1f, present %.1f, rest %.1f; "
            "files: %u opens, %u reads of %u KB, %.1f ms (all threads)",
            ms, tick, after.ticks - before.ticks, scene, used > 0 ? parts : "none",
            (after.recorder - before.recorder) / 1000.0, present, ms - tick - scene - present,
            after.fileOpens - before.fileOpens, after.fileReads - before.fileReads,
            (after.fileBytes - before.fileBytes) / 1024, (after.fileMicros - before.fileMicros) / 1000.0);
}

// The names of the parts, for the heartbeat.
const char *const *const kPartNames = Diagnostics::kScenePartNames;

// The parts of a heartbeat line, in the order of the line.
const int kSceneParts[] = {
    Diagnostics::SCENE_PART_SETUP, Diagnostics::SCENE_PART_PARTICLES, Diagnostics::SCENE_PART_SKY,
    Diagnostics::SCENE_PART_GROUND, Diagnostics::SCENE_PART_SHADOWS, Diagnostics::SCENE_PART_OBJECTS,
    Diagnostics::SCENE_PART_BILLBOARDS, Diagnostics::SCENE_PART_NODES, Diagnostics::SCENE_PART_LAYERS,
};
const int kViewParts[] = {
    Diagnostics::SCENE_PART_VIEW, Diagnostics::SCENE_PART_CAR_NODES, Diagnostics::SCENE_PART_WHEELS,
    Diagnostics::SCENE_PART_OFF_ROAD, Diagnostics::SCENE_PART_SKID_TRAILS, Diagnostics::SCENE_PART_TYRE_MARKS,
    Diagnostics::SCENE_PART_PREPARE, Diagnostics::SCENE_PART_CAR_SHADOWS, Diagnostics::SCENE_PART_REFLECTIONS,
    Diagnostics::SCENE_PART_HUD,
};
const int kInnerParts[] = {
    Diagnostics::SCENE_PART_SECTOR_CULL, Diagnostics::SCENE_PART_RELIGHT, Diagnostics::SCENE_PART_DEFERRED_SORT,
    Diagnostics::SCENE_PART_GLOW, Diagnostics::SCENE_PART_QUADS, Diagnostics::SCENE_PART_2D,
    Diagnostics::SCENE_PART_VERTEX_WRITES,
};
const int kUpdateParts[] = {
    Diagnostics::SCENE_PART_FRAME_EFFECTS, Diagnostics::SCENE_PART_CAR_UPDATE,
};

// A heartbeat line of the ms of parts in a frame.
template <size_t N>
void LogParts(const char *title, const int (&parts)[N], const FrameParts &p, double perFrame)
{
    char text[512];
    size_t used = 0;
    for (size_t i = 0; i < N && used < sizeof(text); i++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%s %.2f", i ? ", " : "",
                                 kPartNames[parts[i]], p.partMicros[parts[i]] / perFrame);
    Sys_Log("heartbeat: %s ms: %s", title, text);
}

} // namespace

SysVitaDrawCounts g_sysVitaDrawCounts;

unsigned long long SysVita_Micros(void)
{
    return sceKernelGetProcessTimeWide();
}

void SysVita_RecorderSample(unsigned long long micros)
{
    s_frameParts.recorderMicros += micros * kRecorderSampleEvery;
}

void SysVita_BatchSample(unsigned long long micros, unsigned draws)
{
    s_frameParts.batchMicros += micros;
    s_frameParts.batchDraws += draws;
}

void SysVita_FramePresented(bool stats)
{
    constexpr uint64_t kPeriodMicros = 5000000;
    static uint64_t last, periodStart;
    static unsigned frames, periodFrames;
    static double sumMs, maxMs;
    static bool first = true;
    // The parts of the frames are given only for a period with the stats on
    // from its start.
    static bool partsValid = true;
    static FrameSnapshot lastSnapshot;
    static unsigned longFrames;
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (!stats) {
        Diagnostics::observer = nullptr;
        partsValid = false;
    } else if (Diagnostics::observer == nullptr) {
        // The hooks start again here: no part runs, no present runs.
        s_frameParts.Reset();
        s_frameParts.part = Diagnostics::SCENE_PART_NONE;
        s_frameParts.partDrawsStart = g_sysVitaDrawCounts;
        s_frameParts.presentStart = now;
        s_frameParts.inTick = false;
        Diagnostics::observer = &s_frameParts;
    }
    if (last != 0) {
        const double ms = (double)(now - last) / 1000.0;
        sumMs += ms;
        maxMs = ms > maxMs ? ms : maxMs;
        periodFrames++;
        if (ms > kLongFrameMs && stats && partsValid && longFrames < kLongFramesPerPeriod) {
            longFrames++;
            LogLongFrame(ms, lastSnapshot, s_frameParts, now - s_frameParts.presentStart);
        }
    } else {
        periodStart = now;
    }
    last = now;
    frames++;
    if (now - periodStart < kPeriodMicros) {
        lastSnapshot = SnapshotOf(s_frameParts);
        return;
    }
    longFrames = 0;
    if (first)
        Sys_Log("heartbeat: first after %.1f s", (double)now / 1e6);
    first = false;
    const struct mallinfo heap = mallinfo();
    Sys_Log("heartbeat: %.1f s, frames %u, frame ms avg %.2f max %.2f, heap used %d KB of %d KB, files open %d",
            (double)now / 1e6, frames, periodFrames ? sumMs / periodFrames : 0.0, maxMs, heap.uordblks / 1024,
            heap.arena / 1024, s_openFiles);
    if (!stats)
        Sys_Log("heartbeat: the performance stats are off (video.perf_stats = 0)");
    // The work of the sound threads on core 1, in ms of each second.
    static unsigned lastMixer, lastMusic;
    unsigned mixer, music;
    SysVita_AudioBusyMicros(&mixer, &music);
    const double seconds = (double)(now - periodStart) / 1e6;
    if (stats)
        Sys_Log("heartbeat: core 1 busy ms per s: mixer %.1f, music %.1f", (mixer - lastMixer) / 1000.0 / seconds,
                (music - lastMusic) / 1000.0 / seconds);
    lastMixer = mixer;
    lastMusic = music;
    if (periodFrames > 0 && partsValid) {
        // The scene: the frame without its ticks and its present.
        const FrameParts &p = s_frameParts;
        const double perFrame = 1000.0 * periodFrames;
        const uint64_t known = p.tickMicros + p.presentMicros;
        // The batches' time holds that of their draws that were recorded
        // alone, which the recorder's time also holds.
        Sys_Log("heartbeat: frame parts ms: ticks %.2f (%.2f ticks), scene %.2f (recorder %.2f, batches %.2f for "
                "%.0f draws), present %.2f, other %.2f",
                p.tickMicros / perFrame, (double)p.ticks / periodFrames,
                (p.frameMicros > known ? p.frameMicros - known : 0) / perFrame, p.recorderMicros / perFrame,
                p.batchMicros / perFrame, (double)p.batchDraws / periodFrames, p.presentMicros / perFrame,
                sumMs / periodFrames - p.frameMicros / perFrame);
        uint64_t parts = 0;
        for (int i = 1; i < Diagnostics::SCENE_PART_COUNT; i++)
            parts += p.partMicros[i];
        // An inner part's time is not in the part around it.
        if (parts > 0) {
            LogParts("scene parts", kSceneParts, p, perFrame);
            LogParts("view parts", kViewParts, p, perFrame);
            LogParts("inner parts", kInnerParts, p, perFrame);
            LogParts("update parts", kUpdateParts, p, perFrame);
            // The scene less all its named parts: what no part measures yet.
            const uint64_t scene = p.frameMicros > known ? p.frameMicros - known : 0;
            Sys_Log("heartbeat: unnamed ms: %.2f of the scene %.2f",
                    (scene > parts ? scene - parts : 0) / perFrame, scene / perFrame);
        }
        // The draws of each part in a frame: the calls of the game, the
        // commands they became, and the vertices copied into the frame.
        s_frameParts.ScenePart(s_frameParts.part);
        char text[900];
        size_t used = 0;
        for (int i = 0; i < Diagnostics::SCENE_PART_COUNT; i++) {
            const SysVitaDrawCounts &d = p.partDraws[i];
            if (d.calls == 0 || used >= sizeof(text))
                continue;
            used += (size_t)snprintf(text + used, sizeof(text) - used, "%s %s %.0f>%.0f %.0f KB", used ? "," : "",
                                     kPartNames[i], (double)d.calls / periodFrames, (double)d.commands / periodFrames,
                                     (double)d.vertexBytes / 1024.0 / periodFrames);
        }
        if (used > 0)
            Sys_Log("heartbeat: draws per frame (calls>commands, vertices copied):%s", text);
    }
    s_frameParts.Reset();
    lastSnapshot = SnapshotOf(s_frameParts);
    partsValid = stats;
    periodStart = now;
    periodFrames = 0;
    sumMs = maxMs = 0.0;
}
#endif

// ---- platform -------------------------------------------------------------------

bool Platform_Init(int, char **)
{
    if (s_logMutex < 0)
        s_logMutex = sceKernelCreateMutex("opencmr2_log", 0, 0, NULL);
    if (s_logIoMutex < 0)
        s_logIoMutex = sceKernelCreateMutex("opencmr2_log_io", 0, 0, NULL);
    s_settings.clear();
    SysSettings::Parse(kVitaDefaults, s_settings);
    s_dirs.Clear();
    s_host.MakeDirectory(kUserDir);
    s_logFile = sceIoOpen((std::string(kUserDir) + "opencmr2.log").c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    std::string text;
    if (ReadWholeFile(std::string(kUserDir) + "opencmr2.ini", text))
        SysSettings::Parse(text, s_settings);

    const std::string missing = s_dirs.MissingGameData(kDataDir);
    if (!missing.empty()) {
        const std::string message = std::string("Game data not found in ") + kDataDir +
                                    "\nCopy the Colin McRae Rally 2.0 files there. Missing:" + missing;
        Sys_Log("%s", message.c_str());
        Sys_MessageBox("OpenCMR2", message.c_str(), SYS_MESSAGEBOX_OK);
        Platform_Shutdown();
        return false;
    }
    Sys_Log("game data: %s", kDataDir);
    Sys_Log("user directory: %s", kUserDir);
    if (Platform_GetSettingInt("system.log_sync", 0) == 0)
        StartLogWriter();
    Sys_Log(s_logSync ? "log: each line is written at once" : "log: a thread writes the lines once per second");
    // The game's thread (this one) on core 0. The renderer's worker has core
    // 2 and the sound has core 1. A thread with no core of its own runs where
    // the system puts it: on the worker's core, the worker waits for it.
    // [system] pin_game_thread = 0 leaves the game's thread on any core.
    if (Platform_GetSettingInt("system.pin_game_thread", 1) != 0) {
        const int result = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
        Sys_Log("game thread: on core 0 (the mask before: 0x%08x)", (unsigned)result);
    } else {
        Sys_Log("game thread: on any core (system.pin_game_thread = 0)");
    }
#ifdef OPENCMR2_DEV
    LogFileProbe();
    // The app's memory budget, after the heap and the program are taken out.
    SceKernelFreeMemorySizeInfo memory;
    memory.size = sizeof(memory);
    if (sceKernelGetFreeMemorySize(&memory) >= 0)
        Sys_Log("memory free: main %d KB, CDRAM %d KB, physically contiguous %d KB", memory.size_user / 1024,
                memory.size_cdram / 1024, memory.size_phycont / 1024);
#endif
    return true;
}

void Platform_Shutdown(void)
{
    StopLogWriter();
    FlushLog();
    if (s_logFile >= 0) {
        sceIoClose(s_logFile);
        s_logFile = -1;
    }
}

const char *Platform_GetDataDir(void) { return kDataDir; }
const char *Platform_GetUserDir(void) { return kUserDir; }

const char *Platform_GetSetting(const char *key, const char *fallback)
{
    auto it = s_settings.find(SysPaths::Lower(key));
    return it != s_settings.end() ? it->second.c_str() : fallback;
}

int Platform_GetSettingInt(const char *key, int fallback)
{
    const char *value = Platform_GetSetting(key, NULL);
    return value != NULL ? (int)strtol(value, NULL, 0) : fallback;
}

// ---- time -----------------------------------------------------------------------

extern "C" DWORD Sys_GetTicks(void)
{
    return (DWORD)(sceKernelGetProcessTimeWide() / 1000);
}

extern "C" double Sys_GetTicksPrecise(void)
{
    return (double)sceKernelGetProcessTimeWide() / 1000.0;
}

extern "C" unsigned int Sys_GetUnixTime(void)
{
    return (unsigned int)time(NULL);
}

extern "C" void Sys_Sleep(DWORD ms)
{
    sceKernelDelayThread(ms * 1000);
}

// ---- events and window ---------------------------------------------------------

void SysVita_PushEvent(const SysEvent &event)
{
    s_events.Push(event);
}

bool Platform_TakeResumed(void)
{
    const bool resumed = s_resumed;
    s_resumed = false;
    return resumed;
}

extern "C" int Sys_PollEvent(SysEvent *event)
{
    PollSystem();
    return s_events.Pop(event) ? 1 : 0;
}

extern "C" int Sys_WaitEvent(SysEvent *event)
{
    while (!Sys_PollEvent(event))
        Sys_Sleep(10);
    return 1;
}

extern "C" void Sys_RequestTextInput(const char *, const char *, int) {}
extern "C" void Sys_OfferTextInput(const char *, const char *, int) {}

extern "C" BOOL Sys_CreateWindow(const char *, BOOL) { return TRUE; }
extern "C" void Sys_DestroyWindow(void) {}
extern "C" void Sys_ShowCursor(BOOL) {}

extern "C" void Sys_GetDesktopSize(int *width, int *height)
{
    *width = kDisplayWidth;
    *height = kDisplayHeight;
}

// ---- message boxes and log ------------------------------------------------------

extern "C" int Sys_MessageBox(const char *title, const char *text, int buttons)
{
    Sys_Log("message box: %s: %s", title, text);
    FlushLog();
    if (buttons == SYS_MESSAGEBOX_RETRY_CANCEL) {
        return RunMessageDialog(text, SCE_MSG_DIALOG_BUTTON_TYPE_YESNO) == SCE_MSG_DIALOG_BUTTON_ID_YES
                   ? SYS_MESSAGEBOX_RESULT_RETRY
                   : SYS_MESSAGEBOX_RESULT_CANCEL;
    }
    RunMessageDialog(text, SCE_MSG_DIALOG_BUTTON_TYPE_OK);
    return SYS_MESSAGEBOX_RESULT_OK;
}

extern "C" void Sys_Log(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
#ifdef OPENCMR2_DEV
    // Seconds since the start on each line.
    char stamped[sizeof(line) + 16];
    snprintf(stamped, sizeof(stamped), "[%8.2f] %s", (double)sceKernelGetProcessTimeWide() / 1e6, line);
    const char *text = stamped;
#else
    const char *text = line;
#endif
    sceClibPrintf("%s\n", text);
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, NULL);
    const bool room = s_logLines.Append(text);
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
    // The writer thread has the file (the log section above). Without it
    // the line goes to the file at once. A buffer that is full wakes it.
    if (s_logSync) {
        FlushLog();
    } else if (!room) {
        sceKernelSignalSema(s_logWake, 1);
    }
}

extern "C" void Sys_Fatal(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    Sys_Log("fatal: %s", line);
    FlushLog();
    RunMessageDialog(line, SCE_MSG_DIALOG_BUTTON_TYPE_OK);
    Sys_Exit(1);
}

extern "C" void Sys_Exit(int code)
{
    Platform_Shutdown();
    sceKernelExitProcess(code);
}

extern "C" const char *Sys_GetCommandLine(void) { return ""; }

// ---- files ----------------------------------------------------------------------

// A read of a block at an offset of a file, the whole block or to the end
// of the file: the byte count, or a negative sceIo error.
int ReadBlock(SceUID fd, uint64_t offset, void *buffer, uint32_t size)
{
    if (sceIoLseek(fd, (SceOff)offset, SCE_SEEK_SET) != (SceOff)offset)
        return -1;
    uint32_t done = 0;
    while (done < size) {
        const int n = sceIoRead(fd, (uint8_t *)buffer + done, size - done);
        if (n < 0)
            return n;
        if (n == 0)
            break;
        done += (uint32_t)n;
    }
    return (int)done;
}

// The thread of the read-aheads: it reads the block a read-ahead asked for
// while that one's reader goes on. The SDK has no declared asynchronous
// read with a documented end (sceIoReadAsync, sceIoComplete): this thread
// makes the read asynchronous with sceIoRead. One read at a time: a second
// read-ahead's Start waits until the first one's Wait. The thread that
// collects a read (Wait) can be another than the one that began it (Start):
// the music thread reads ahead, the game's thread seeks or closes the file.
// So the reader is held by a semaphore, which has no owner: a mutex must be
// unlocked by the thread that locked it.
struct FileReader {
    SceUID free = -1, start = -1, done = -1, thread = -1;
    bool tried = false;
    // The read the thread does now.
    SceUID fd = -1;
    uint64_t offset = 0;
    void *buffer = nullptr;
    uint32_t size = 0;
    int result = 0;

    static int Entry(SceSize, void *data)
    {
        FileReader *self = *(FileReader **)data;
        for (;;) {
            sceKernelWaitSema(self->start, 1, NULL);
            self->result = ReadBlock(self->fd, self->offset, self->buffer, self->size);
            sceKernelSignalSema(self->done, 1);
        }
        return 0;
    }

    // Made at the first read-ahead. False: the read-aheads read in Start.
    bool Ready()
    {
        if (tried)
            return thread >= 0;
        tried = true;
        free = sceKernelCreateSema("file_reader_free", 0, 1, 1, NULL);
        start = sceKernelCreateSema("file_reader_start", 0, 0, 1, NULL);
        done = sceKernelCreateSema("file_reader_done", 0, 0, 1, NULL);
        if (free >= 0 && start >= 0 && done >= 0)
            thread = sceKernelCreateThread("file_reader", Entry, 128, 16 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
        FileReader *self = this;
        const int started = thread < 0 ? thread : sceKernelStartThread(thread, sizeof(self), &self);
        if (started < 0) {
            Sys_Log("file: no reader thread (0x%08x): a read-ahead reads in its reader's thread", (unsigned)started);
            if (thread >= 0)
                sceKernelDeleteThread(thread);
            thread = -1;
        }
        return thread >= 0;
    }
};
FileReader s_fileReader;

// The file of a read-ahead (platform/read_ahead.h): Start hands the read to
// the reader thread and returns, Wait waits for its end.
struct VitaReadIo : ReadAheadIo {
    SceUID fd = -1;
    int syncResult = 0;     // the read of Start without the thread
    bool onThread = false;

    bool Start(uint64_t offset, void *buffer, uint32_t size) override
    {
        FileReader &r = s_fileReader;
        onThread = r.Ready();
        if (!onThread) {
            syncResult = ReadBlock(fd, offset, buffer, size);
            return true;
        }
        // Held until Wait: the thread has one read.
        sceKernelWaitSema(r.free, 1, NULL);
        r.fd = fd;
        r.offset = offset;
        r.buffer = buffer;
        r.size = size;
        sceKernelSignalSema(r.start, 1);
        return true;
    }

    int Wait() override
    {
        if (!onThread)
            return syncResult;
        FileReader &r = s_fileReader;
        sceKernelWaitSema(r.done, 1, NULL);
        const int result = r.result;
        sceKernelSignalSema(r.free, 1);
        return result;
    }
};

struct SysFile {
    SceUID fd;
    // Sys_SetFileReadAhead: the reads and seeks go through it.
    ReadAhead *ahead = nullptr;
    VitaReadIo io;
};

extern "C" BOOL Sys_ResolvePath(const char *path, BOOL forWriting, char *out, size_t outSize)
{
    std::string host;
    if (forWriting) {
        std::vector<std::string> parts = SysPaths::SplitGamePath(path);
        if (parts.empty())
            return FALSE;
        host = s_dirs.PathForWriting(kUserDir, parts);
    } else {
        host = s_dirs.ResolveForReading(kUserDir, kDataDir, path);
        if (host.empty()) {
            s_lastMissingFile = path;
            return FALSE;
        }
    }
    if (host.size() + 1 > outSize)
        return FALSE;
    memcpy(out, host.c_str(), host.size() + 1);
    return TRUE;
}

extern "C" const char *Sys_GetLastMissingFile(void)
{
    return s_lastMissingFile.c_str();
}

extern "C" SysFile *Sys_OpenFile(const char *path, int mode)
{
    const uint64_t started = sceKernelGetProcessTimeWide();
    struct Counted {
        uint64_t started;
        ~Counted()
        {
            s_fileOpens.fetch_add(1, std::memory_order_relaxed);
            s_fileMicros.fetch_add((unsigned)(sceKernelGetProcessTimeWide() - started), std::memory_order_relaxed);
        }
    } counted{ started };
    char host[1024];
    SceUID fd = -1;
    if (Sys_ResolvePath(path, mode == SYS_FILE_WRITE, host, sizeof(host))) {
        fd = mode == SYS_FILE_WRITE ? sceIoOpen(host, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777)
                                    : sceIoOpen(host, SCE_O_RDONLY, 0);
    }
    if (fd < 0) {
        if (mode != SYS_FILE_WRITE)
            s_lastMissingFile = path;
        return NULL;
    }
    SysFile *file = new SysFile;
    file->fd = fd;
    s_openFiles++;
    return file;
}

extern "C" void Sys_CloseFile(SysFile *file)
{
    if (file != NULL) {
        if (file->ahead != nullptr) {
            file->ahead->Close(file->io);
            delete file->ahead;
        }
        sceIoClose(file->fd);
        delete file;
        s_openFiles--;
    }
}

extern "C" void Sys_SetFileReadAhead(SysFile *file, DWORD blockBytes)
{
    if (file == NULL || file->ahead != nullptr || blockBytes == 0)
        return;
    // [audio] read_ahead = 0: the file reads as without it.
    if (Platform_GetSettingInt("audio.read_ahead", 1) == 0)
        return;
    const SceOff position = sceIoLseek(file->fd, 0, SCE_SEEK_CUR);
    if (position < 0)
        return;
    file->io.fd = file->fd;
    file->ahead = new ReadAhead(blockBytes);
    file->ahead->Seek(file->io, (uint64_t)position);
}

extern "C" DWORD Sys_ReadFile(SysFile *file, void *buffer, DWORD size)
{
    const uint64_t started = sceKernelGetProcessTimeWide();
    DWORD done = 0;
    if (file->ahead != nullptr) {
        done = file->ahead->Read(file->io, buffer, size);
    } else {
        while (done < size) {
            int n = sceIoRead(file->fd, (BYTE *)buffer + done, size - done);
            if (n <= 0)
                break;
            done += (DWORD)n;
        }
    }
    s_fileReads.fetch_add(1, std::memory_order_relaxed);
    s_fileReadBytes.fetch_add(done, std::memory_order_relaxed);
    s_fileMicros.fetch_add((unsigned)(sceKernelGetProcessTimeWide() - started), std::memory_order_relaxed);
    return done;
}

extern "C" DWORD Sys_WriteFile(SysFile *file, const void *buffer, DWORD size)
{
    DWORD done = 0;
    while (done < size) {
        int n = sceIoWrite(file->fd, (const BYTE *)buffer + done, size - done);
        if (n <= 0)
            break;
        done += (DWORD)n;
    }
    return done;
}

extern "C" DWORD Sys_SeekFile(SysFile *file, LONG offset, int origin)
{
    if (file->ahead != nullptr) {
        // The position is the read-ahead's: the file's own moves with its reads.
        file->ahead->Finish(file->io);
        SceOff base = 0;
        if (origin == SYS_SEEK_CUR)
            base = (SceOff)file->ahead->Position();
        else if (origin == SYS_SEEK_END)
            base = sceIoLseek(file->fd, 0, SCE_SEEK_END);
        const SceOff position = base < 0 ? -1 : base + offset;
        if (position < 0 || position > 0xfffffffe)
            return 0xffffffff;
        file->ahead->Seek(file->io, (uint64_t)position);
        return (DWORD)position;
    }
    int whence = origin == SYS_SEEK_CUR ? SCE_SEEK_CUR : origin == SYS_SEEK_END ? SCE_SEEK_END : SCE_SEEK_SET;
    SceOff position = sceIoLseek(file->fd, offset, whence);
    return position < 0 || position > 0xfffffffe ? 0xffffffff : (DWORD)position;
}

extern "C" DWORD Sys_GetFileSize(SysFile *file)
{
    if (file->ahead != nullptr) {
        file->ahead->Finish(file->io);
        const SceOff size = sceIoLseek(file->fd, 0, SCE_SEEK_END);
        return size < 0 || size > 0xfffffffe ? 0xffffffff : (DWORD)size;
    }
    SceOff position = sceIoLseek(file->fd, 0, SCE_SEEK_CUR);
    SceOff size = position < 0 ? -1 : sceIoLseek(file->fd, 0, SCE_SEEK_END);
    if (position >= 0)
        sceIoLseek(file->fd, position, SCE_SEEK_SET);
    return size < 0 || size > 0xfffffffe ? 0xffffffff : (DWORD)size;
}

extern "C" void Sys_FlushFile(SysFile *file)
{
    sceIoSyncByFd(file->fd, 0);
}

extern "C" BOOL Sys_FileExists(const char *path)
{
    return !s_dirs.ResolveForReading(kUserDir, kDataDir, path).empty();
}

extern "C" BOOL Sys_DeleteFile(const char *path)
{
    // Only files in the user directory can be deleted.
    std::string host = s_dirs.FindUnder(kUserDir, SysPaths::SplitGamePath(path));
    if (host.empty() || sceIoRemove(host.c_str()) < 0)
        return FALSE;
    s_dirs.Clear();
    return TRUE;
}

extern "C" void Sys_ListFiles(const char *directory, const char *pattern, SysListCallback callback, void *context)
{
    for (const std::string &name : s_dirs.ListFiles(kDataDir, kUserDir, directory, pattern))
        if (!callback(name.c_str(), context))
            break;
}

// ---- settings -------------------------------------------------------------------

extern "C" const char *Sys_GetInstallSetting(const char *name)
{
    const std::string key = SysPaths::Lower(name);
    if (key == "game_hdpath" || key == "game_cdpath")
        return ".";
    if (key == "install_version")
        return "Full";
    if (key == "language")
        return Platform_GetSetting("install.language", "English");
    if (key == "sku_type")
        return s_dirs.DataSku(kDataDir, Platform_GetSetting("install.sku", "EUROPE"));
    return "";
}

extern "C" int Sys_GetOption(const char *name, int fallback)
{
    return Platform_GetSettingInt(name, fallback);
}

extern "C" const char *Sys_GetOptionText(const char *name, const char *fallback)
{
    return Platform_GetSetting(name, fallback);
}

extern "C" BOOL Sys_SetOption(const char *name, int value)
{
    const std::string key = SysPaths::Lower(name);
    const size_t dot = key.find('.');
    if (dot == std::string::npos)
        return FALSE;
    const std::string text = std::to_string(value);
    s_settings[key] = text;
    const std::string file = std::string(kUserDir) + "opencmr2.ini";
    std::string contents;
    ReadWholeFile(file, contents);
    const std::string temporary = file + ".tmp";
    if (!WriteWholeFile(temporary, SysSettings::Edit(contents, key.substr(0, dot), key.substr(dot + 1), text))) {
        Sys_Log("could not save %s = %s", name, text.c_str());
        return FALSE;
    }
    sceIoRemove(file.c_str());
    if (sceIoRename(temporary.c_str(), file.c_str()) < 0) {
        Sys_Log("could not save %s = %s", name, text.c_str());
        return FALSE;
    }
    return TRUE;
}
