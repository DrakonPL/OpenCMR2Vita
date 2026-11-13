// OpenCMR2 system services on the PS Vita: see port/sys.h. Game data in
// ux0:data/OpenCMR2/, saves, settings and the log in its user/ folder.

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
const char kVitaDefaults[] =
    "[video]\n"
    "outline_fonts = 0\n"
    "vector_icons = 0\n"
    "movie_workers = 2\n";

SysSettings::Values s_settings;
SysEventRing s_events;
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
// A file write per line costs tens of ms on the Vita: lines collect in a
// buffer that goes to the file once per second, when full, and at exit.

constexpr size_t kLogBufferBytes = 4096;
constexpr uint64_t kLogFlushMicros = 1000000;

char s_logBuffer[kLogBufferBytes];
size_t s_logUsed;
uint64_t s_logLastFlush;
SceUID s_logFile = -1;
// The renderer's worker thread logs too (render/gfx_threaded.cpp): a line
// goes into the buffer with this mutex held. Made in Platform_Init, before
// that thread starts.
SceUID s_logMutex = -1;

void FlushLog()
{
    if (s_logFile >= 0 && s_logUsed > 0)
        sceIoWrite(s_logFile, s_logBuffer, s_logUsed);
    s_logUsed = 0;
    s_logLastFlush = sceKernelGetProcessTimeWide();
}

void AppendLog(const char *line)
{
    const size_t length = strlen(line);
    if (s_logUsed + length + 1 > kLogBufferBytes)
        FlushLog();
    if (length + 1 <= kLogBufferBytes) {
        memcpy(s_logBuffer + s_logUsed, line, length);
        s_logUsed += length;
        s_logBuffer[s_logUsed++] = '\n';
    }
    if (sceKernelGetProcessTimeWide() - s_logLastFlush >= kLogFlushMicros)
        FlushLog();
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
    void Reset()
    {
        tickMicros = presentMicros = frameMicros = recorderMicros = 0;
        ticks = 0;
        memset(partMicros, 0, sizeof(partMicros));
        memset(partDraws, 0, sizeof(partDraws));
    }
};
FrameParts s_frameParts;

// The names of the parts, for the heartbeat.
const char *const kPartNames[Diagnostics::SCENE_PART_COUNT] = {
    "other", "setup", "particles", "sky", "ground", "shadows", "objects", "billboards",
    "nodes", "layers", "view", "prepare", "car shadows", "reflections", "hud"
};

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
    } else {
        periodStart = now;
    }
    last = now;
    frames++;
    if (now - periodStart < kPeriodMicros)
        return;
    if (first)
        Sys_Log("heartbeat: first after %.1f s", (double)now / 1e6);
    first = false;
    const struct mallinfo heap = mallinfo();
    Sys_Log("heartbeat: %.1f s, frames %u, frame ms avg %.2f max %.2f, heap used %d KB of %d KB, files open %d",
            (double)now / 1e6, frames, periodFrames ? sumMs / periodFrames : 0.0, maxMs, heap.uordblks / 1024,
            heap.arena / 1024, s_openFiles);
    if (!stats)
        Sys_Log("heartbeat: the performance stats are off (video.perf_stats = 0)");
    if (periodFrames > 0 && partsValid) {
        // The scene: the frame without its ticks and its present.
        const FrameParts &p = s_frameParts;
        const double perFrame = 1000.0 * periodFrames;
        const uint64_t known = p.tickMicros + p.presentMicros;
        Sys_Log("heartbeat: frame parts ms: ticks %.2f (%.2f ticks), scene %.2f (recorder %.2f), present %.2f, "
                "other %.2f",
                p.tickMicros / perFrame, (double)p.ticks / periodFrames,
                (p.frameMicros > known ? p.frameMicros - known : 0) / perFrame, p.recorderMicros / perFrame,
                p.presentMicros / perFrame, sumMs / periodFrames - p.frameMicros / perFrame);
        uint64_t parts = 0;
        for (int i = 1; i < Diagnostics::SCENE_PART_COUNT; i++)
            parts += p.partMicros[i];
        if (parts > 0)
            Sys_Log("heartbeat: scene parts ms: setup %.2f, particles %.2f, sky %.2f, ground %.2f, shadows %.2f, "
                    "objects %.2f, billboards %.2f, nodes %.2f, layers %.2f",
                    p.partMicros[Diagnostics::SCENE_PART_SETUP] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_PARTICLES] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_SKY] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_GROUND] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_SHADOWS] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_OBJECTS] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_BILLBOARDS] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_NODES] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_LAYERS] / perFrame);
        if (parts > 0)
            Sys_Log("heartbeat: view parts ms: view %.2f, prepare %.2f, car shadows %.2f, reflections %.2f, hud %.2f",
                    p.partMicros[Diagnostics::SCENE_PART_VIEW] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_PREPARE] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_CAR_SHADOWS] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_REFLECTIONS] / perFrame,
                    p.partMicros[Diagnostics::SCENE_PART_HUD] / perFrame);
        // The draws of each part in a frame: the calls of the game, the
        // commands they became, and the vertices copied into the frame.
        s_frameParts.ScenePart(s_frameParts.part);
        char text[512];
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
    // Seconds since the start on each line, and each line on the card at once:
    // a crash or a close from the LiveArea keeps every line.
    char stamped[sizeof(line) + 16];
    snprintf(stamped, sizeof(stamped), "[%8.2f] %s", (double)sceKernelGetProcessTimeWide() / 1e6, line);
    sceClibPrintf("%s\n", stamped);
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, NULL);
    AppendLog(stamped);
    FlushLog();
#else
    sceClibPrintf("%s\n", line);
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, NULL);
    AppendLog(line);
#endif
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
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
// read-ahead's Start waits until the first one's Wait.
struct FileReader {
    SceUID lock = -1, start = -1, done = -1, thread = -1;
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
        lock = sceKernelCreateMutex("file_reader", 0, 0, NULL);
        start = sceKernelCreateSema("file_reader_start", 0, 0, 1, NULL);
        done = sceKernelCreateSema("file_reader_done", 0, 0, 1, NULL);
        if (lock >= 0 && start >= 0 && done >= 0)
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
        sceKernelLockMutex(r.lock, 1, NULL);
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
        sceKernelUnlockMutex(r.lock, 1);
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
    const SceOff position = sceIoLseek(file->fd, 0, SCE_SEEK_CUR);
    if (position < 0)
        return;
    file->io.fd = file->fd;
    file->ahead = new ReadAhead(blockBytes);
    file->ahead->Seek(file->io, (uint64_t)position);
}

extern "C" DWORD Sys_ReadFile(SysFile *file, void *buffer, DWORD size)
{
    if (file->ahead != nullptr)
        return file->ahead->Read(file->io, buffer, size);
    DWORD done = 0;
    while (done < size) {
        int n = sceIoRead(file->fd, (BYTE *)buffer + done, size - done);
        if (n <= 0)
            break;
        done += (DWORD)n;
    }
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
