// TapeParts.h - small, cheap building blocks for Low Tide (no allocation, no transcendental per sample).
//
// SPDX-License-Identifier: MIT

#ifndef LOWTIDE_TAPEPARTS_H
#define LOWTIDE_TAPEPARTS_H

#include "dsp/biquad.h"

#include <cmath>
#include <cstdint>

namespace lowtide {

constexpr float kTwoPi = 6.28318530718f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline float smooth01(float x)
{
    x = clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// tanh-like soft clipper (Pade 3/3), exact unity slope at 0, flat at |x| >= 3.
inline float softClip(float x)
{
    x = clampf(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// sin(2*pi*ph) for any ph, parabolic approximation (error < 0.1 %), a few multiplies.
inline float fastSin01(float ph)
{
    ph -= std::floor(ph);
    const float t = 2.0f * ph - 1.0f;                       // -1..1 is -pi..pi, and sin(pi(t+1)) = -sin(pi t)
    float y = 4.0f * t * (1.0f - std::fabs(t));
    y = 0.225f * (y * std::fabs(y) - y) + y;
    return -y;
}

// Integer hash to [0,1): the same input always gives the same value (the Drift tide shape uses bar numbers).
inline float hash01(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

// xorshift32 with visible state, so the accident Pattern lock can save and restore it.
struct Rng
{
    uint32_t s = 0x2545F491u;
    float next() { uint32_t x = s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s = x; return (float)(x >> 8) * (1.0f / 16777216.0f); }
};

// One-pole low pass with a directly computed coefficient.
struct OnePole
{
    float a = 1.0f, z = 0.0f;
    void setHz(float hz, double sr)
    {
        const float x = kTwoPi * hz / (float)sr;
        a = 1.0f - std::exp(-(x < 0.0f ? 0.0f : x));
        if (a > 1.0f) a = 1.0f;
    }
    float lp(float x) { z += a * (x - z); return z; }
    float hp(float x) { z += a * (x - z); return x - z; }
};

// The old-sampler resample: input filter -> sample and hold at a lowered rate -> N-bit -> output filter.
// The rate is the converter's base rate divided by the record pitch factor: recording a source sped up
// and playing it back slowed down means fewer samples per musical event, so the grit follows the pitch.
struct CrunchChannel
{
    mpc::dsp::Biquad in1, in2, out1, out2;
    float held = 0.0f;
    void reset() { in1.reset(); in2.reset(); out1.reset(); out2.reset(); held = 0.0f; }
};

} // namespace lowtide

#endif // LOWTIDE_TAPEPARTS_H
