// Input on the PS Vita (port/input.h): the built-in pad is player 1's
// controller, bound in the [vita] section of controls.ini (bindings.cpp).
// The menus read the menu actions, as with a gamepad on Android: no key
// events. No keyboard, mouse or rumble.

#include "input/input_internal.h"
#include "input/input_math.h"
#include "input/vita_controls.h"
#include "platform/platform.h"
#include "port/input.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include <string.h>

#include <algorithm>
#include <string>

namespace {

// The pad's button of each VitaButton.
const unsigned kButtonBits[VITA_BUTTON_COUNT] = {
    SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER,
    SCE_CTRL_START, SCE_CTRL_SELECT, SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT,
};

// The two channels whose kind of input (axis or button) the game asks about.
enum Channel { CHANNEL_STEERING, CHANNEL_PEDALS, CHANNEL_COUNT };

bool s_ready;
SceCtrlData s_pad;
int s_values[INPUT_ACTION_COUNT];
bool s_analog[CHANNEL_COUNT] = { true, false };
bool s_textEntryOpen;
bool s_used;    // an input was given: the controls page opens on the pad's profile

Profile &VitaProfile()
{
    Bindings_Get();
    return *Bindings_Find("vita");
}

// A stick axis as an SDL axis (-32768..32767).
int16_t StickAxis(int axis)
{
    const unsigned char raw[VITA_AXIS_COUNT] = { s_pad.lx, s_pad.ly, s_pad.rx, s_pad.ry };
    return InputMath::StickByteAxis(raw[axis]);
}

// No button down and the sticks at rest: the pad when it cannot be read.
void ReleasePad()
{
    memset(&s_pad, 0, sizeof(s_pad));
    s_pad.lx = s_pad.ly = s_pad.rx = s_pad.ry = InputMath::kStickRest;
}

int SourceValue(const Profile &profile, const Source &source)
{
    if (source.kind == SOURCE_VITA_BUTTON)
        return (s_pad.buttons & kButtonBits[source.index]) != 0 ? INPUT_FULL : 0;
    if (source.kind != SOURCE_VITA_AXIS)
        return 0;
    const int other = source.index ^ 1;     // lx with ly, rx with ry
    const int16_t value = InputMath::SquareStick(StickAxis(source.index), StickAxis(other));
    return InputMath::Scale(InputMath::AxisValue(value, source.direction), profile.deadzone, profile.saturation);
}

// The values of the actions, and whether the steering and the pedals came
// from a stick or from buttons, as the SDL path decides it (input_sdl.cpp).
void UpdateActions(const Profile &profile)
{
    bool analog[CHANNEL_COUNT] = {}, digital[CHANNEL_COUNT] = {};
    memset(s_values, 0, sizeof(s_values));
    for (int action = 0; action < INPUT_ACTION_COUNT; action++) {
        for (const Source &source : profile.actions[action]) {
            int value = SourceValue(profile, source);
            if (action <= INPUT_STEER_RIGHT && IsAnalog(source) && profile.steeringCurve != 0)
                value = InputMath::Curve(value, profile.steeringCurve);
            s_values[action] = std::max(s_values[action], value);
            s_used = s_used || value > INPUT_FULL / 4;
            if (action > INPUT_BRAKE)
                continue;
            const int channel = action <= INPUT_STEER_RIGHT ? CHANNEL_STEERING : CHANNEL_PEDALS;
            if (IsAnalog(source) && value > INPUT_FULL / 4)
                analog[channel] = true;
            else if (!IsAnalog(source) && value != 0)
                digital[channel] = true;
        }
    }
    for (int channel = 0; channel < CHANNEL_COUNT; channel++)
        if (analog[channel] != digital[channel])
            s_analog[channel] = analog[channel];
}

} // namespace

// ---- players and actions -------------------------------------------------------

extern "C" void Input_Update(void)
{
    if (!s_ready) {
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
        s_ready = true;
    }
    // An error, or no sample yet.
    if (sceCtrlPeekBufferPositive(0, &s_pad, 1) < 1)
        ReleasePad();
    UpdateActions(VitaProfile());
}

// The pad is player 1's. Two local players are accepted. Player 2 has no
// device: the console has one pad.
extern "C" void Input_SetLocalPlayers(int) {}

extern "C" void Input_SetTextEntryOpen(BOOL open)
{
    s_textEntryOpen = open != FALSE;
}

extern "C" int Input_GetAction(int player, int action)
{
    if (s_textEntryOpen || player != 0 || action < 0 || action >= INPUT_ACTION_COUNT)
        return 0;
    return s_values[action];
}

extern "C" BOOL Input_IsAnalogSteering(int player)
{
    return player == 0 && s_analog[CHANNEL_STEERING];
}

extern "C" BOOL Input_IsAnalogPedals(int player)
{
    return player == 0 && s_analog[CHANNEL_PEDALS];
}

// A stick is short: the game eases the wheels towards it (input.h).
extern "C" BOOL Input_IsSmoothSteering(int player)
{
    return Input_IsAnalogSteering(player);
}

extern "C" BOOL Input_DevicesChanged(void) { return FALSE; }
extern "C" InputDevice *Input_GetFeedbackDevice(int) { return NULL; }
extern "C" void Input_SetRumble(int, int, int) {}
extern "C" void Input_ImportKeyboardKeys(int, const BYTE[9]) {}

// ---- controls pages: one profile, the pad ---------------------------------------

namespace {

Profile *EditableProfile(int index)
{
    return index == 0 ? &VitaProfile() : NULL;
}

} // namespace

extern "C" int Input_GetProfileCount(void) { return 1; }
// A joystick's kind: the page shows the name below, and no rumble.
extern "C" int Input_GetProfileKind(int) { return INPUT_PROFILE_JOYSTICK; }
extern "C" int Input_GetLastUsedProfile(void) { return s_used ? 0 : -1; }
extern "C" const char *Input_GetProfileName(int) { return "PS Vita"; }

extern "C" void Input_GetBindingText(int index, int action, char *text, int size)
{
    std::string names;
    Profile *profile = EditableProfile(index);
    if (profile != NULL && action >= 0 && action < INPUT_ACTION_COUNT)
        for (const Source &source : profile->actions[action])
            names += std::string(names.empty() ? "" : " / ") + VitaControls_Name(source);
    if (size > 0) {
        strncpy(text, names.c_str(), size - 1);
        text[size - 1] = '\0';
    }
}

extern "C" int Input_GetSetting(int index, int setting)
{
    Profile *profile = EditableProfile(index);
    if (profile == NULL)
        return 0;
    switch (setting) {
    case INPUT_SETTING_DEADZONE: return profile->deadzone;
    case INPUT_SETTING_CURVE: return profile->steeringCurve;
    default: return profile->rumble;
    }
}

extern "C" void Input_SetSetting(int index, int setting, int value)
{
    Profile *profile = EditableProfile(index);
    if (profile == NULL || Input_GetSetting(index, setting) == value)
        return;
    switch (setting) {
    case INPUT_SETTING_DEADZONE: profile->deadzone = std::clamp(value, 0, 99); profile->chosen |= SETTING_DEADZONE; break;
    case INPUT_SETTING_CURVE: profile->steeringCurve = std::clamp(value, 0, 100); profile->chosen |= SETTING_CURVE; break;
    default: profile->rumble = std::clamp(value, 0, 100); profile->chosen |= SETTING_RUMBLE; break;
    }
    Bindings_MarkChanged();
}

extern "C" void Input_RestoreDefaults(int index)
{
    Profile *profile = EditableProfile(index);
    if (profile == NULL)
        return;
    Bindings_SetDefaults(*profile, NULL);
    Bindings_MarkChanged();
}

namespace {

VitaCapture s_capture;

VitaPad PadNow()
{
    VitaPad pad;
    for (int button = 0; button < VITA_BUTTON_COUNT; button++)
        pad.buttons[button] = (s_pad.buttons & kButtonBits[button]) != 0;
    for (int axis = 0; axis < VITA_AXIS_COUNT; axis++)
        pad.axes[axis] = StickAxis(axis);
    return pad;
}

uint32_t NowMs()
{
    return (uint32_t)(sceKernelGetProcessTimeWide() / 1000);
}

} // namespace

// Every button binds, Circle too: the capture gives up after ten seconds only.
extern "C" void Input_BeginCapture(int index, int action)
{
    if (EditableProfile(index) != NULL && action >= 0 && action < INPUT_ACTION_COUNT)
        s_capture.Begin(action, PadNow(), NowMs());
}

extern "C" int Input_UpdateCapture(void)
{
    Source source;
    const int result = s_capture.Update(PadNow(), NowMs(), &source);
    if (result == 1)
        Bindings_Assign(VitaProfile(), s_capture.Action(), source);
    return result;
}

extern "C" void Input_ReloadBindings(void)
{
    Bindings_Load();
}

// ---- raw keyboard and mouse: none ------------------------------------------------

extern "C" InputDevice *Input_OpenKeyboard(void) { return NULL; }
extern "C" InputDevice *Input_OpenMouse(void) { return NULL; }
extern "C" void Input_CloseDevice(InputDevice *) {}
extern "C" void Input_SetMouseExclusive(InputDevice *, BOOL) {}
extern "C" BOOL Input_ReadKeyboard(InputDevice *, BYTE[256]) { return FALSE; }
extern "C" BOOL Input_ReadMouse(InputDevice *, InputMouseState *) { return FALSE; }
extern "C" BOOL Input_IsKeyDown(int) { return FALSE; }

extern "C" void Input_GetKeyRepeat(DWORD *delay, DWORD *interval)
{
    *delay = (DWORD)Platform_GetSettingInt("input.key_repeat_delay", 500);
    *interval = (DWORD)Platform_GetSettingInt("input.key_repeat_interval", 33);
}

extern "C" const char *Input_GetKeyName(int) { return ""; }

// ---- force feedback: none -----------------------------------------------------------

extern "C" InputEffect *Input_CreateEffect(InputDevice *, const InputEffectParams *) { return NULL; }
extern "C" void Input_UpdateEffect(InputEffect *, const InputEffectParams *) {}
extern "C" void Input_StartEffect(InputEffect *) {}
extern "C" BOOL Input_IsEffectPlaying(InputEffect *) { return FALSE; }
extern "C" void Input_DestroyEffect(InputEffect *) {}
extern "C" void Input_StopAllEffects(InputDevice *) {}
extern "C" void Input_SetAutocenter(InputDevice *, BOOL) {}
