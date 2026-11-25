// PressureParts.h - the small building blocks of Pressure: fast log2/exp2 (so the gain computer
// can run every sample without calling a transcendental function), a 2x halfband oversampler for
// the saturator, an asymmetric soft shaper, and a few one-pole filters.
//
// Everything here is allocation free and safe on the audio thread. Coefficient set*() calls that
// need exp() are made per block, and only when a control has moved.
//
// SPDX-License-Identifier: MIT

#ifndef PRESSURE_PARTS_H
#define PRESSURE_PARTS_H

#include "dsp/math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace pr {

using mpc::dsp::fastTanh;

// ---------------------------------------------------------------------------------------------
// Fast log2 / exp2. Bit tricks split off the exponent; a short polynomial does the mantissa.
// Max error: log2 about 5e-6 (0.00003 dB), exp2 about 2e-7 relative (least-squares fits). Plenty for a gain computer.
// ---------------------------------------------------------------------------------------------
inline float fastLog2(float x)
{
    if (!(x > 1.0e-30f)) x = 1.0e-30f;               // also catches NaN
    uint32_t bits; std::memcpy(&bits, &x, 4);
    const float e = (float)((int)((bits >> 23) & 0xFF) - 127);
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;       // mantissa in [1, 2)
    float m; std::memcpy(&m, &bits, 4);
    // log2(m) on [1, 2): minimax-style polynomial in (m - 1).
    const float t = m - 1.0f;
    const float p = t * (1.442517f + t * (-0.71789728f + t * (0.45688865f + t * (-0.27735291f
                  + t * (0.121902f + t * -0.026061792f)))));
    return e + p;
}

inline float fastExp2(float x)
{
    if (x < -126.0f) return 0.0f;
    if (x > 126.0f) x = 126.0f;
    const float fl = std::floor(x);
    const float f = x - fl;                            // [0, 1)
    // 2^f on [0, 1).
    const float p = 1.0f + f * (0.69315275f + f * (0.24015316f + f * (0.05582829f + f * (0.0089888129f
                  + f * 0.0018767084f))));
    const int32_t ei = (int32_t)fl + 127;
    uint32_t bits = (uint32_t)ei << 23;
    float s; std::memcpy(&s, &bits, 4);
    return s * p;
}

constexpr float kDbPerLog2 = 6.0205999f;             // 20 log10(2)
inline float fastGainToDb(float g) { return kDbPerLog2 * fastLog2(g); }
inline float fastPowerToDb(float p) { return 0.5f * kDbPerLog2 * fastLog2(p); }   // 10 log10(p)
inline float fastDbToGain(float db) { return fastExp2(db * (1.0f / kDbPerLog2)); }

// ---------------------------------------------------------------------------------------------
// 2x oversampling with a 47-tap halfband FIR (Kaiser window, beta 8: about 80 dB stopband), the
// same filter Groundswell's Breakwater uses. Every other tap of a halfband is zero, so the up- and
// down-samplers each cost 12 multiplies per base-rate sample. Latency: 2K - 1 = 23 samples.
// ---------------------------------------------------------------------------------------------
constexpr int kHbK = 12;
constexpr int kHbLen = 2 * kHbK;
constexpr int kOversampleLatency = 2 * kHbK - 1;

struct HalfbandTaps
{
    float g[kHbK] = {};

    void design()
    {
        const int N = 4 * kHbK - 1;
        const int c = 2 * kHbK - 1;
        const double beta = 8.0;
        auto i0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
            return sum;
        };
        double tmp[kHbK], sum = 0.0;
        for (int j = 0; j < kHbK; ++j)
        {
            const int d = 2 * j + 1;
            const double x = 3.14159265358979 * d / 2.0;
            const double r = 2.0 * (double)(c + d) / (double)(N - 1) - 1.0;
            const double w = i0(beta * std::sqrt(std::fmax(0.0, 1.0 - r * r))) / i0(beta);
            tmp[j] = 0.5 * (std::sin(x) / x) * w;
            sum += tmp[j];
        }
        for (int j = 0; j < kHbK; ++j) g[j] = (float)(tmp[j] * 0.25 / sum);
    }
};

struct Upsampler2x
{
    float h[2 * kHbLen] = {};
    int p = 0;
    void reset() { for (float& v : h) v = 0.0f; p = 0; }
    void process(float x, const HalfbandTaps& t, float& o0, float& o1)
    {
        p = (p == 0) ? kHbLen - 1 : p - 1;
        h[p] = h[p + kHbLen] = x;
        const float* s = h + p;
        float acc = 0.0f;
        for (int j = 0; j < kHbK; ++j) acc += t.g[j] * (s[kHbK - 1 - j] + s[kHbK + j]);
        o0 = 2.0f * acc;
        o1 = s[kHbK - 1];
    }
};

struct Downsampler2x
{
    float e[2 * kHbLen] = {};
    float o[2 * kHbLen] = {};
    int p = 0;
    void reset() { for (float& v : e) v = 0.0f; for (float& v : o) v = 0.0f; p = 0; }
    float process(float p0, float p1, const HalfbandTaps& t)
    {
        p = (p == 0) ? kHbLen - 1 : p - 1;
        e[p] = e[p + kHbLen] = p0;
        o[p] = o[p + kHbLen] = p1;
        const float* s = e + p;
        float acc = 0.5f * o[p + kHbK];
        for (int j = 0; j < kHbK; ++j) acc += t.g[j] * (s[kHbK + j] + s[kHbK - 1 - j]);
        return acc;
    }
};

// ---------------------------------------------------------------------------------------------
// Asymmetric soft shaper: tanh (rational approximation) that saturates at +1 on the positive swing
// but at -(1 + asym) on the negative one. Unity slope at zero on both sides, so quiet signals pass
// clean; the asymmetry (even harmonics) only shows as the signal gets big. Pressure moves `asym`
// with the gain reduction, which is how Bite gets dirtier the harder it grabs.
// ---------------------------------------------------------------------------------------------
inline float shape(float u, float kn, float ikn)
{
    return u >= 0.0f ? fastTanh(u) : kn * fastTanh(u * ikn);
}

// ---------------------------------------------------------------------------------------------
// One-pole helpers
// ---------------------------------------------------------------------------------------------
struct OnePoleLP
{
    float a = 1.0f, z = 0.0f;
    void set(float hz, float sr) { a = 1.0f - std::exp(-6.2831853f * mpc::dsp::clamp(hz, 1.0f, 0.45f * sr) / sr); }
    float process(float x) { z += a * (x - z); return z; }
    void reset() { z = 0.0f; }
};

struct OnePoleHP
{
    float r = 0.995f, x1 = 0.0f, y1 = 0.0f;
    void set(float hz, float sr) { r = std::exp(-6.2831853f * mpc::dsp::clamp(hz, 1.0f, 0.45f * sr) / sr); }
    float process(float x) { const float y = x - x1 + r * y1; x1 = x; y1 = mpc::dsp::flushDenormal(y); return y; }
    void reset() { x1 = y1 = 0.0f; }
};

// Delay line for the dry path, so it lines up with the oversampled wet path.
struct ShortDelay
{
    static constexpr int kSize = 32;                  // > kOversampleLatency, power of two
    float b[kSize] = {};
    int w = 0;
    void reset(float fill = 0.0f) { for (float& v : b) v = fill; w = 0; }
    float process(float x, int d)
    {
        b[w] = x;
        const float y = b[(w - d) & (kSize - 1)];
        w = (w + 1) & (kSize - 1);
        return y;
    }
};

} // namespace pr

#endif // PRESSURE_PARTS_H
