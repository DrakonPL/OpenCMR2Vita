// OpenCMR2 system services on the PS Vita (port/sys.h). The clock, the log,
// the message box and the game data check work. Events, files and settings
// are stubs that fail or do nothing.

#include "platform/platform_vita.h"
#include "port/sys.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
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

namespace {

const char kDataDir[] = "ux0:data/OpenCMR2/";

// ---- message dialog ------------------------------------------------------------
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

bool IsFile(const std::string &path)
{
    SceIoStat stat{};
    return sceIoGetstat(path.c_str(), &stat) >= 0 && SCE_S_ISREG(stat.st_mode) && stat.st_size > 0;
}

// The archives that the PC check (MissingGameData in sys.cpp) also looks for.
std::string MissingGameData(const std::string &dir)
{
    std::string missing;
    for (const char *file : {"Game/Bigfiles/common.bfl", "FrontEnd/common.bfl", "Sounds/surface.bfl"}) {
        if (!IsFile(dir + file))
            missing += std::string("\n- ") + file;
    }
    return missing;
}

} // namespace

// ---- platform -------------------------------------------------------------------

bool Platform_Init(int, char **)
{
    const std::string missing = MissingGameData(kDataDir);
    if (!missing.empty()) {
        const std::string message = std::string("Game data not found in ") + kDataDir +
                                    "\nCopy the Colin McRae Rally 2.0 files there. Missing:" + missing;
        Sys_Log("%s", message.c_str());
        Sys_MessageBox("OpenCMR2", message.c_str(), SYS_MESSAGEBOX_OK);
        return false;
    }
    Sys_Log("game data: %s", kDataDir);
    return true;
}

void Platform_Shutdown(void) {}

const char *Platform_GetDataDir(void) { return kDataDir; }
const char *Platform_GetUserDir(void) { return kDataDir; }
const char *Platform_GetSetting(const char *, const char *fallback) { return fallback; }
int Platform_GetSettingInt(const char *, int fallback) { return fallback; }

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

extern "C" int Sys_TakeSteps(SysStepClock *, int, int)
{
    return 1;
}

// ---- events and window ---------------------------------------------------------

extern "C" int Sys_PollEvent(SysEvent *) { return 0; }

extern "C" int Sys_WaitEvent(SysEvent *)
{
    Sys_Sleep(16);
    return 0;
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
}

extern "C" void Sys_Fatal(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    Sys_Log("fatal: %s", line);
    RunMessageDialog(line, SCE_MSG_DIALOG_BUTTON_TYPE_OK);
    Sys_Exit(1);
}

extern "C" void Sys_Exit(int code)
{
    sceKernelExitProcess(code);
}

extern "C" const char *Sys_GetCommandLine(void) { return ""; }

// ---- files ----------------------------------------------------------------------

extern "C" SysFile *Sys_OpenFile(const char *, int) { return NULL; }
extern "C" const char *Sys_GetLastMissingFile(void) { return ""; }
extern "C" void Sys_CloseFile(SysFile *) {}
extern "C" DWORD Sys_ReadFile(SysFile *, void *, DWORD) { return 0; }
extern "C" DWORD Sys_WriteFile(SysFile *, const void *, DWORD) { return 0; }
extern "C" DWORD Sys_SeekFile(SysFile *, LONG, int) { return 0xffffffff; }
extern "C" DWORD Sys_GetFileSize(SysFile *) { return 0xffffffff; }
extern "C" void Sys_FlushFile(SysFile *) {}
extern "C" BOOL Sys_FileExists(const char *) { return FALSE; }
extern "C" BOOL Sys_DeleteFile(const char *) { return FALSE; }
extern "C" BOOL Sys_ResolvePath(const char *, BOOL, char *, size_t) { return FALSE; }
extern "C" void Sys_ListFiles(const char *, const char *, SysListCallback, void *) {}

// ---- settings -------------------------------------------------------------------

extern "C" const char *Sys_GetInstallSetting(const char *) { return ""; }
extern "C" int Sys_GetOption(const char *, int fallback) { return fallback; }
extern "C" const char *Sys_GetOptionText(const char *, const char *fallback) { return fallback; }
extern "C" BOOL Sys_SetOption(const char *, int) { return FALSE; }
