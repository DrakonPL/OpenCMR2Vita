// The axis and action values of the input sources (src/input/input_math.h),
// on known inputs. No SDL, no device.

#include "input/input_math.h"
#include "port/input.h"

#include <cstdio>
#include <cstdlib>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

bool Near(int a, int b, int tolerance)
{
    return abs(a - b) <= tolerance;
}

} // namespace

int main()
{
    using namespace InputMath;

    // Axis halves: the far end is the full value, the other half nothing.
    REQUIRE(AxisValue(-32768, AXIS_NEGATIVE) == INPUT_FULL && AxisValue(32767, AXIS_NEGATIVE) == 0);
    REQUIRE(AxisValue(32767, AXIS_POSITIVE) == INPUT_FULL && AxisValue(-32768, AXIS_POSITIVE) == 0);
    REQUIRE(Near(AxisValue(16384, AXIS_POSITIVE), INPUT_FULL / 2, 2));
    // Pedals: the whole axis, released at one end.
    REQUIRE(AxisValue(-32768, AXIS_RISING) == 0 && AxisValue(32767, AXIS_RISING) == INPUT_FULL);
    REQUIRE(AxisValue(32767, AXIS_FALLING) == 0 && AxisValue(-32768, AXIS_FALLING) == INPUT_FULL);

    // A stick byte: at rest nothing, the ends the full axis.
    REQUIRE(StickByteAxis(kStickRest) == 0);
    REQUIRE(StickByteAxis(0) == -32768 && StickByteAxis(255) == 32512);
    REQUIRE(AxisValue(StickByteAxis(kStickRest), AXIS_NEGATIVE) == 0 && AxisValue(StickByteAxis(0), AXIS_NEGATIVE) == INPUT_FULL);

    // Deadzone 10 and saturation 95 (the Vita's): nothing inside 10%, full
    // from 95%, linear between.
    REQUIRE(Scale(INPUT_FULL / 10, 10, 95) == 0);
    REQUIRE(Scale(INPUT_FULL * 95 / 100, 10, 95) == INPUT_FULL);
    REQUIRE(Near(Scale(INPUT_FULL * 525 / 1000, 10, 95), INPUT_FULL / 2, 2));
    REQUIRE(Scale(INPUT_FULL / 2, 0, 100) == INPUT_FULL / 2);
    // A saturation at or under the deadzone gives full at once.
    REQUIRE(Scale(INPUT_FULL / 2, 40, 30) == INPUT_FULL);

    // The curve: linear at 0, cubic at 100, the ends unchanged.
    REQUIRE(Curve(INPUT_FULL / 2, 0) == INPUT_FULL / 2);
    REQUIRE(Curve(INPUT_FULL / 2, 100) == INPUT_FULL / 8);
    REQUIRE(Curve(INPUT_FULL, 30) == INPUT_FULL && Curve(0, 30) == 0);
    REQUIRE(Curve(INPUT_FULL / 2, 30) > INPUT_FULL / 8 && Curve(INPUT_FULL / 2, 30) < INPUT_FULL / 2);

    // The square gate: along an axis nothing changes; at the round gate's
    // edge on a diagonal (each axis about 70%) the axis reads full.
    REQUIRE(SquareStick(20000, 0) == 20000 && SquareStick(-20000, 0) == -20000);
    REQUIRE(SquareStick(0, 30000) == 0);
    REQUIRE(Near(SquareStick(23170, 23170), 32767, 2));
    REQUIRE(Near(SquareStick(-23170, 23170), -32767, 2));
    // Towards the centre a diagonal grows by the same factor, no more.
    REQUIRE(Near(SquareStick(8000, 8000), 11314, 2));

    printf("input_math: ok\n");
    return 0;
}
