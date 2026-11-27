// The values of axes and actions, shared by the input sources (input_sdl.cpp,
// input_vita.cpp). No SDL: tests/input_math.cpp checks it.
#ifndef OPENCMR2_INPUT_MATH_H
#define OPENCMR2_INPUT_MATH_H

#include <stdint.h>

// How an axis gives an action its value.
enum AxisRange {
    AXIS_NEGATIVE,          // from where the axis rests towards its minimum
    AXIS_POSITIVE,          // from where it rests towards its maximum (a trigger)
    AXIS_RISING,            // the whole axis, released at its minimum (a pedal)
    AXIS_FALLING,           // the whole axis, released at its maximum
};

// Where a joystick's axis rests, snapped to its centre or one of its ends:
// a pedal, a clutch or a throttle lever rests at an end. Without a reading
// yet, nothing is known.
enum AxisRest {
    REST_UNKNOWN,
    REST_CENTRE,
    REST_MIN,
    REST_MAX,
};

namespace InputMath {

// An axis (-32768..32767) as an action value, 0..INPUT_FULL, by its AxisRange.
// An axis half counts from where the axis rests to that end: from the
// centre, as a stick, or the whole axis from the other end, as a pedal. A
// half towards the end the axis rests at gives nothing.
int AxisValue(int16_t raw, int range, AxisRest rest = REST_CENTRE);
// A stick byte of the PS Vita's pad (0..255) as an axis.
constexpr unsigned char kStickRest = 128;
int16_t StickByteAxis(unsigned char raw);
// An action value with the deadzone left out and the saturation the full
// value (both in percent of INPUT_FULL).
int Scale(int value, int deadzone, int saturation);
// Blends the linear response with a cubic one (percent of it), which moves
// less near the centre and reaches the full value at the same end.
int Curve(int value, int percent);
// A thumbstick's gate is round: pushed to its edge on a diagonal, each axis
// reads about 70%, so a thumb that rises or drops a little while pushing to
// the side does not reach full lock. The axis is read as if the gate were
// square: at the edge the axis nearer to the push gives its full value, and
// along the axes and towards the centre nothing changes. other: the stick's
// other axis.
int16_t SquareStick(int16_t axis, int16_t other);

} // namespace InputMath

#endif
