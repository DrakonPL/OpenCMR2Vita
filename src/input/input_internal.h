// Shared by the input sources (input_sdl.cpp, input_vita.cpp) and the
// bindings (bindings.cpp).
#ifndef OPENCMR2_INPUT_INTERNAL_H
#define OPENCMR2_INPUT_INTERNAL_H

#include "input/input_math.h"
#include "port/input.h"

#ifdef VITA
struct SDL_Joystick;
#else
#include <SDL3/SDL.h>
#endif

#include <deque>
#include <string>
#include <vector>

// One input an action is bound to.
enum SourceKind {
    SOURCE_KEY,             // index: SDL_Scancode
    SOURCE_BUTTON,          // index: SDL_GamepadButton
    SOURCE_AXIS,            // index: SDL_GamepadAxis
    SOURCE_JOY_BUTTON,      // index: joystick button
    SOURCE_JOY_AXIS,        // index: joystick axis
    SOURCE_JOY_HAT,         // index: hat, direction: SDL_HAT_UP/RIGHT/DOWN/LEFT
    SOURCE_VITA_BUTTON,     // index: VitaButton
    SOURCE_VITA_AXIS,       // index: VitaAxis, direction: AXIS_NEGATIVE or AXIS_POSITIVE
};

// The PS Vita's buttons and stick axes, as the [vita] section names them
// (kVitaButtonNames, kVitaAxisNames).
enum VitaButton {
    VITA_CROSS, VITA_CIRCLE, VITA_SQUARE, VITA_TRIANGLE, VITA_L, VITA_R, VITA_START, VITA_SELECT,
    VITA_UP, VITA_DOWN, VITA_LEFT, VITA_RIGHT, VITA_BUTTON_COUNT
};
enum VitaAxis { VITA_LX, VITA_LY, VITA_RX, VITA_RY, VITA_AXIS_COUNT };
extern const char *const kVitaButtonNames[VITA_BUTTON_COUNT];
extern const char *const kVitaAxisNames[VITA_AXIS_COUNT];

#ifndef VITA
// Where a joystick's axis rests (AxisRest, input_math.h).
AxisRest Joystick_AxisRest(SDL_Joystick *joystick, int axis);
#endif

struct Source {
    SourceKind kind;
    int index;
    int direction;          // AxisRange, or the hat direction
};

// Bindings of one kind of device: the keyboard players, gamepads in SDL's
// layout, and joysticks and wheels by model.
struct Profile {
    std::string section;            // "keyboard", "keyboard2", "gamepad", "joystick", "device <GUID>"
    std::string name;               // the device's name, for a device section
    std::vector<Source> actions[INPUT_ACTION_COUNT];
    int deadzone = 2;               // percent of the axis left out around its rest
    int saturation = 100;           // percent of the axis that gives the full value
    int player = 0;                 // 1 or 2; 0 follows the connection order
    int rumble = 100;               // percent of the gamepad's rumble
    int steeringCurve = 0;          // percent of cubic response of the steering axes
    int force = 100;                // percent of the game's force feedback (the original's at 100)
    bool enabled = true;            // a device section: false leaves the device out
    unsigned chosen = 0;            // SETTING_* bits set by the user (written to controls.ini)
    // The keys' actions held at each player's previous update (keyboard
    // profiles), for telling when one is pressed.
    mutable bool keyActive[INPUT_PLAYERS][INPUT_ACTION_COUNT] = {};
};

// Settings of a profile; those not chosen follow the defaults of its kind.
enum {
    SETTING_DEADZONE = 1,
    SETTING_SATURATION = 2,
    SETTING_CURVE = 4,
    SETTING_RUMBLE = 8,
    SETTING_FORCE = 16,
};

bool IsAnalog(const Source &source);
std::string FormatSource(const Profile &profile, const Source &source);

// controls.ini: defaults, reading and writing.
std::deque<Profile> &Bindings_Get(void);
Profile *Bindings_Find(const std::string &section);
// The profile of a joystick or wheel: SDL's layout of a known gamepad, else
// its device section, made from where its axes rest when it has none
// (*created set).
Profile *Bindings_ForJoystick(SDL_Joystick *joystick, bool gamepad, bool *created);
void Bindings_Load(void);
// Writes the driving bindings of a profile to the log.
void Bindings_LogProfile(const Profile &profile, const char *what);
void Bindings_MarkChanged(void);
bool SameSource(const Source &a, const Source &b);
// Binds the input to the action. An input bound to another driving action
// trades places with the action's own.
void Bindings_Assign(Profile &profile, int action, const Source &source);
// The defaults of a profile, from the device for a joystick's.
void Bindings_SetDefaults(Profile &profile, SDL_Joystick *joystick);
// The defaults of the settings not chosen: for a device section, from the
// device (NULL: as the joystick defaults).
void Bindings_DefaultSettings(Profile &profile, SDL_Joystick *joystick);
// Player 1's default keys: the keys Android's touch controls press, which
// keep working whatever the keyboard is bound to.
const Profile &Bindings_TouchProfile(void);

#endif
