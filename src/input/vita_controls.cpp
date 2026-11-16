#include "input/vita_controls.h"

#include <stdlib.h>

namespace {

constexpr int kHalfTravel = 16384;
constexpr uint32_t kGiveUpMs = 10000;

const char *const kButtonNames[VITA_BUTTON_COUNT] = {
    "Cross", "Circle", "Square", "Triangle", "L", "R", "Start", "Select", "Up", "Down", "Left", "Right",
};
// Each stick axis: its negative half, then its positive one.
const char *const kAxisNames[VITA_AXIS_COUNT][2] = {
    { "Left Stick Left", "Left Stick Right" }, { "Left Stick Up", "Left Stick Down" },
    { "Right Stick Left", "Right Stick Right" }, { "Right Stick Up", "Right Stick Down" },
};

} // namespace

const char *VitaControls_Name(const Source &source)
{
    if (source.kind == SOURCE_VITA_BUTTON && source.index >= 0 && source.index < VITA_BUTTON_COUNT)
        return kButtonNames[source.index];
    if (source.kind == SOURCE_VITA_AXIS && source.index >= 0 && source.index < VITA_AXIS_COUNT)
        return kAxisNames[source.index][source.direction == AXIS_POSITIVE ? 1 : 0];
    return "";
}

bool VitaCapture_FindInput(const VitaPad &pad, const int16_t base[VITA_AXIS_COUNT], Source *out)
{
    for (int button = 0; button < VITA_BUTTON_COUNT; button++) {
        if (pad.buttons[button]) {
            *out = { SOURCE_VITA_BUTTON, button, 0 };
            return true;
        }
    }
    for (int axis = 0; axis < VITA_AXIS_COUNT; axis++) {
        const int value = pad.axes[axis];
        if (abs(value - base[axis]) < kHalfTravel)
            continue;
        *out = { SOURCE_VITA_AXIS, axis, value > base[axis] ? AXIS_POSITIVE : AXIS_NEGATIVE };
        return true;
    }
    return false;
}

void VitaCapture::Begin(int action, const VitaPad &pad, uint32_t nowMs)
{
    _active = true;
    _released = false;
    _action = action;
    _startMs = nowMs;
    for (int axis = 0; axis < VITA_AXIS_COUNT; axis++)
        _base[axis] = pad.axes[axis];
}

int VitaCapture::Update(const VitaPad &pad, uint32_t nowMs, Source *out)
{
    if (!_active)
        return -1;
    if (nowMs - _startMs > kGiveUpMs) {
        _active = false;
        return -1;
    }
    Source found;
    const bool any = VitaCapture_FindInput(pad, _base, &found);
    if (!_released) {
        _released = !any;
        return 0;
    }
    if (!any)
        return 0;
    _active = false;
    *out = found;
    return 1;
}
