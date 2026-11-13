#pragma once

#include <algorithm>
#include <cmath>

namespace esa {

// What happens to a chain sample on its way to the speaker: volume, then upstream's
// soft boost (about 2x when quiet, never past full scale), then a hard clamp to full
// scale. One definition, used by the speaker path and by anything that shows its
// level, so the two cannot disagree. Real-time safe.
inline float shapeOutput(float x, float gainLinear, bool boost)
{
    x *= gainLinear;
    if (boost) x = std::tanh(x * 2.0f);
    return std::clamp(x, -1.0f, 1.0f);
}

// Volume in dB as the gain shapeOutput() takes, worked out the one way for the speaker path
// and the output meter alike.
inline float outputGain(double db)
{
    return float(std::pow(10.0, db / 20.0));
}

} // namespace esa
