// AmpParts.h - the building blocks of Breakwater's amp: a 2x oversampler, a triode-like shaper,
// the passive bass/middle/treble tone stack, and a few cheap one-pole filters.
//
// Everything here is allocation free and safe on the audio thread. Coefficients that need
// transcendental functions are computed in set*() calls, which Breakwater makes per block, and
// only when a knob has moved.
//
// SPDX-License-Identifier: MIT

#ifndef BREAKWATER_AMP_PARTS_H
#define BREAKWATER_AMP_PARTS_H

#include "dsp/math.h"

#include <cmath>

namespace bw {

using mpc::dsp::fastTanh;

// ---------------------------------------------------------------------------------------------
// 2x oversampling with a 47-tap halfband FIR (Kaiser window, beta 8: about 80 dB stopband).
// A halfband has every other tap at zero, so the up- and down-samplers each cost 12 multiplies
// per base-rate sample (the taps are symmetric). Total latency: 2K - 1 = 23 base-rate samples.
// ---------------------------------------------------------------------------------------------
constexpr int kHbK = 12;                  // N = 4K - 1 = 47 taps
constexpr int kHbLen = 2 * kHbK;          // history needed, per phase
constexpr int kOversampleLatency = 2 * kHbK - 1;

struct HalfbandTaps
{
    float g[kHbK] = {};                   // g[j] = h[centre +/- (2j + 1)]; h[centre] = 0.5

    void design()
    {
        const int N = 4 * kHbK - 1;
        const int c = 2 * kHbK - 1;
        const double beta = 8.0;
        auto i0 = [](double x) {          // modified Bessel function of the first kind, order 0
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
        for (int j = 0; j < kHbK; ++j) g[j] = (float)(tmp[j] * 0.25 / sum);   // DC gain exactly 1
    }
};

// One base-rate sample in, two oversampled samples out (o0 first, then o1).
struct Upsampler2x
{
    float h[2 * kHbLen] = {};
    int p = 0;

    void reset() { for (float& v : h) v = 0.0f; p = 0; }
    void process(float x, const HalfbandTaps& t, float& o0, float& o1)
    {
        p = (p == 0) ? kHbLen - 1 : p - 1;
        h[p] = h[p + kHbLen] = x;
        const float* s = h + p;           // s[i] = x[n - i]
        float acc = 0.0f;
        for (int j = 0; j < kHbK; ++j) acc += t.g[j] * (s[kHbK - 1 - j] + s[kHbK + j]);
        o0 = 2.0f * acc;
        o1 = s[kHbK - 1];
    }
};

// Two oversampled samples in (the pair the upsampler produced, after processing), one out.
struct Downsampler2x
{
    float e[2 * kHbLen] = {};             // even-phase history
    float o[2 * kHbLen] = {};             // odd-phase history
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
// A triode-like gain stage: a tanh curve (rational approximation, no exp) that saturates at +1 on
// the positive swing (grid conduction) but only at -(1 + asym) on the negative one (cutoff).
// Both halves are tanh, so the curve is smooth and nearly linear around zero: small signals pass
// with unity slope and almost no distortion, and the asymmetry - even harmonics - only appears as
// the signal gets big. That is the "clean when soft, hairs up when you dig in" behaviour. A
// biased-tanh stage would instead make second harmonic in proportion to level, even played softly.
// The DC the asymmetry produces is removed by the DC blocker after every stage.
// ---------------------------------------------------------------------------------------------
struct Tube
{
    float kn = 1.0f, ikn = 1.0f;
    void set(float asym) { kn = 1.0f + asym; ikn = 1.0f / kn; }
    float operator()(float u) const { return u >= 0.0f ? fastTanh(u) : kn * fastTanh(u * ikn); }
};

// ---------------------------------------------------------------------------------------------
// One-pole helpers
// ---------------------------------------------------------------------------------------------
struct OnePoleLP
{
    float a = 1.0f, z = 0.0f;
    void set(float hz, float sr) { a = 1.0f - std::exp(-6.2831853f * mpc::dsp::clamp(hz, 5.0f, 0.45f * sr) / sr); }
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

// ---------------------------------------------------------------------------------------------
// The passive bass / middle / treble tone stack found in most classic guitar amps, after
// D. T. Yeh and J. O. Smith's paper on discretising the guitar amp tone stack (DAFx-06, 2006).
// The circuit's third-order transfer function is written in terms of the component values and the
// three pot positions, then discretised with the bilinear transform. Because it is the circuit and
// not three independent EQ bands, the knobs interact: the middle control changes how much the bass
// and treble can do, and turning the bass changes the depth of the mid scoop.
//
// Runs in double precision (third-order sections with low poles at 88 kHz are touchy in float).
// set() costs about 100 multiplies: call it per block, only when a knob or the voice has moved.
// ---------------------------------------------------------------------------------------------
struct ToneStackParts { double C1, C2, C3, R1, R2, R3, R4; };

struct ToneStack
{
    double b[4] = {1, 0, 0, 0}, a[4] = {1, 0, 0, 0};
    double z[3] = {0, 0, 0};

    // t, m, l: treble, middle, bass pot positions 0..1 (already through their tapers).
    void set(const ToneStackParts& p, double t, double m, double l, double fs)
    {
        const double C1 = p.C1, C2 = p.C2, C3 = p.C3, R1 = p.R1, R2 = p.R2, R3 = p.R3, R4 = p.R4;
        const double b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
        const double b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4)
                        - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                        + m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                        + l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4)
                        + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
                        + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
        const double b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4)
                        - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
                        + m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
                        + t * C1 * C2 * C3 * R1 * R3 * R4 - t * m * C1 * C2 * C3 * R1 * R3 * R4
                        + t * l * C1 * C2 * C3 * R1 * R2 * R4;
        const double a0 = 1.0;
        const double a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
        const double a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                        + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
                        - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                        + l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4)
                        + (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3
                           + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
        const double a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4)
                        - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
                        + m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4)
                        + l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;

        // Bilinear transform, s = c (1 - z^-1) / (1 + z^-1), c = 2 fs.
        const double c = 2.0 * fs, c2 = c * c, c3 = c2 * c;
        const double B0 = -b1 * c - b2 * c2 - b3 * c3;
        const double B1 = -b1 * c + b2 * c2 + 3.0 * b3 * c3;
        const double B2 =  b1 * c + b2 * c2 - 3.0 * b3 * c3;
        const double B3 =  b1 * c - b2 * c2 + b3 * c3;
        const double A0 = -a0 - a1 * c - a2 * c2 - a3 * c3;
        const double A1 = -3.0 * a0 - a1 * c + a2 * c2 + 3.0 * a3 * c3;
        const double A2 = -3.0 * a0 + a1 * c + a2 * c2 - 3.0 * a3 * c3;
        const double A3 = -a0 + a1 * c - a2 * c2 + a3 * c3;
        b[0] = B0 / A0; b[1] = B1 / A0; b[2] = B2 / A0; b[3] = B3 / A0;
        a[0] = 1.0;     a[1] = A1 / A0; a[2] = A2 / A0; a[3] = A3 / A0;
    }

    // Transposed direct form II.
    float process(float xf)
    {
        const double x = xf;
        const double y = b[0] * x + z[0];
        z[0] = b[1] * x - a[1] * y + z[1];
        z[1] = b[2] * x - a[2] * y + z[2];
        z[2] = b[3] * x - a[3] * y;
        return (float)y;
    }
    void reset() { z[0] = z[1] = z[2] = 0.0; }

    // Magnitude at hz (for tests): evaluates the digital filter on the unit circle.
    double magnitude(double hz, double fs) const
    {
        const double w = 6.283185307179586 * hz / fs;
        double nr = 0, ni = 0, dr = 0, di = 0;
        for (int k = 0; k < 4; ++k)
        {
            nr += b[k] * std::cos(-w * k); ni += b[k] * std::sin(-w * k);
            dr += a[k] * std::cos(-w * k); di += a[k] * std::sin(-w * k);
        }
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

// Audio-taper ("A") pot: about 15% of the resistance at half rotation.
inline double audioTaper(double x)
{
    const double k = 3.47;
    return (std::exp(k * x) - 1.0) / (std::exp(k) - 1.0);
}

} // namespace bw

#endif // BREAKWATER_AMP_PARTS_H
