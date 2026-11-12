// The values of axes and actions (input_math.h).

#include "input/input_math.h"

#include "port/input.h"

#include <math.h>

#include <algorithm>

namespace InputMath {

int AxisValue(int16_t raw, int range)
{
    switch (range) {
    case AXIS_NEGATIVE: return raw < 0 ? (int)(-(long long)raw * INPUT_FULL / 32768) : 0;
    case AXIS_POSITIVE: return raw > 0 ? (int)((long long)raw * INPUT_FULL / 32767) : 0;
    case AXIS_RISING: return (int)(((long long)raw + 32768) * INPUT_FULL / 65535);
    default: return (int)((32767 - (long long)raw) * INPUT_FULL / 65535);
    }
}

int16_t StickByteAxis(unsigned char raw)
{
    return (int16_t)std::clamp((raw - kStickRest) * 256, -32768, 32767);
}

int Scale(int value, int deadzone, int saturation)
{
    const int low = deadzone * INPUT_FULL / 100;
    const int high = saturation * INPUT_FULL / 100;
    if (value <= low)
        return 0;
    if (value >= high || high <= low)
        return INPUT_FULL;
    return (int)((long long)(value - low) * INPUT_FULL / (high - low));
}

int Curve(int value, int percent)
{
    long long v = value;
    long long cubic = v * v / INPUT_FULL * v / INPUT_FULL;
    return (int)((v * (100 - percent) + cubic * percent) / 100);
}

int16_t SquareStick(int16_t axis, int16_t other)
{
    double x = axis / 32767.0;
    double y = other / 32767.0;
    double longest = std::max(fabs(x), fabs(y));
    if (longest <= 0.0)
        return 0;
    double value = std::clamp(x * sqrt(x * x + y * y) / longest, -1.0, 1.0);
    return (int16_t)lround(value * 32767.0);
}

} // namespace InputMath
