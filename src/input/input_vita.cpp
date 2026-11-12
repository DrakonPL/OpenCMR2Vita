// Input on the PS Vita (port/input.h). Stubs: no device, no action, no effect.

#include "port/input.h"

void Input_Update(void) {}
void Input_SetLocalPlayers(int) {}
void Input_SetTextEntryOpen(BOOL) {}
int Input_GetAction(int, int) { return 0; }
BOOL Input_IsAnalogSteering(int) { return FALSE; }
BOOL Input_IsAnalogPedals(int) { return FALSE; }
BOOL Input_IsSmoothSteering(int) { return FALSE; }
BOOL Input_DevicesChanged(void) { return FALSE; }
InputDevice *Input_GetFeedbackDevice(int) { return NULL; }
void Input_SetRumble(int, int, int) {}
void Input_ImportKeyboardKeys(int, const BYTE[9]) {}
void Input_SaveBindings(void) {}

int Input_GetProfileCount(void) { return 0; }
int Input_GetProfileKind(int) { return INPUT_PROFILE_GAMEPAD; }
int Input_GetLastUsedProfile(void) { return -1; }
const char *Input_GetProfileName(int) { return ""; }

void Input_GetBindingText(int, int, char *text, int size)
{
    if (size > 0)
        text[0] = '\0';
}

int Input_GetSetting(int, int) { return 0; }
void Input_SetSetting(int, int, int) {}
void Input_RestoreDefaults(int) {}
void Input_BeginCapture(int, int) {}
int Input_UpdateCapture(void) { return -1; }
void Input_ReloadBindings(void) {}

InputDevice *Input_OpenKeyboard(void) { return NULL; }
InputDevice *Input_OpenMouse(void) { return NULL; }
void Input_CloseDevice(InputDevice *) {}
void Input_SetMouseExclusive(InputDevice *, BOOL) {}
BOOL Input_ReadKeyboard(InputDevice *, BYTE[256]) { return FALSE; }
BOOL Input_ReadMouse(InputDevice *, InputMouseState *) { return FALSE; }
BOOL Input_IsKeyDown(int) { return FALSE; }

void Input_GetKeyRepeat(DWORD *delay, DWORD *interval)
{
    *delay = 500;
    *interval = 33;
}

const char *Input_GetKeyName(int) { return ""; }

InputEffect *Input_CreateEffect(InputDevice *, const InputEffectParams *) { return NULL; }
void Input_UpdateEffect(InputEffect *, const InputEffectParams *) {}
void Input_StartEffect(InputEffect *) {}
BOOL Input_IsEffectPlaying(InputEffect *) { return FALSE; }
void Input_DestroyEffect(InputEffect *) {}
void Input_StopAllEffects(InputDevice *) {}
void Input_SetAutocenter(InputDevice *, BOOL) {}
