// Process entry point on the PS Vita: sets up the platform and runs the game's
// main loop (CMain::Initialize, which was WinMain's body).

#include "platform/platform_vita.h"
#include "port/types.h"
#include "main.h"

#include <psp2/kernel/processmgr.h>

// The original game ran in 64 MB.
extern "C" {
unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
}

int main(int argc, char **argv)
{
    int code = 1;
    if (Platform_Init(argc, argv)) {
        code = (int)CMain::Initialize("");
        Platform_Shutdown();
    }
    sceKernelExitProcess(code);
    return code;
}
