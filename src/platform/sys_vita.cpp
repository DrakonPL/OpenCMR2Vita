// OpenCMR2 system services on the PS Vita: see port/sys.h. Game data in
// ux0:data/OpenCMR2/, saves, settings and the log in its user/ folder.

#include "platform/platform_vita.h"
#include "platform/sys_common.h"
#include "platform/sys_event_ring.h"
#include "platform/sys_paths.h"
#include "port/sys.h"

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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

namespace {

const char kDataDir[] = "ux0:data/OpenCMR2/";
const char kUserDir[] = "ux0:data/OpenCMR2/user/";

SysSettings::Values s_settings;
SysEventRing s_events;
bool s_resumed;
std::string s_lastMissingFile;  // game path of the last file that could not be read

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
// display buffer before each flip, so it gets its own GXM display while open.

constexpr int kDisplayWidth = 960;
constexpr int kDisplayHeight = 544;
constexpr int kDisplayStride = 1024;   // pixels
constexpr int kDisplayBuffers = 2;
constexpr unsigned kMemblockAlign = 256 * 1024;

struct DialogBuffer {
    SceUID uid;
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
    const unsigned size = (kDisplayStride * kDisplayHeight * 4 + kMemblockAlign - 1) & ~(kMemblockAlign - 1);
    for (DialogBuffer &b : buffers) {
        b.uid = sceKernelAllocMemBlock("dialog_display", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
        sceKernelGetMemBlockBase(b.uid, &b.data);
        sceGxmMapMemory(b.data, size, (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE));
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
        sceGxmUnmapMemory(b.data);
        sceKernelFreeMemBlock(b.uid);
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
    if (!OpenDialogDisplay(buffers))
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

// ---- platform -------------------------------------------------------------------

bool Platform_Init(int, char **)
{
    s_settings.clear();
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
    sceClibPrintf("%s\n", line);
    AppendLog(line);
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

struct SysFile {
    SceUID fd;
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
    return file;
}

extern "C" void Sys_CloseFile(SysFile *file)
{
    if (file != NULL) {
        sceIoClose(file->fd);
        delete file;
    }
}

extern "C" DWORD Sys_ReadFile(SysFile *file, void *buffer, DWORD size)
{
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
    int whence = origin == SYS_SEEK_CUR ? SCE_SEEK_CUR : origin == SYS_SEEK_END ? SCE_SEEK_END : SCE_SEEK_SET;
    SceOff position = sceIoLseek(file->fd, offset, whence);
    return position < 0 || position > 0xfffffffe ? 0xffffffff : (DWORD)position;
}

extern "C" DWORD Sys_GetFileSize(SysFile *file)
{
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
