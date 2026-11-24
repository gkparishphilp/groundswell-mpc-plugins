// UrchinParts.h - the building blocks of Urchin: a 2x halfband oversampler, antiderivative
// anti-aliasing (ADAA) for every memoryless shaper, the shapers themselves (piecewise polynomials,
// no transcendental functions), and a few one-pole filters.
//
// ADAA in one paragraph: instead of y[n] = f(x[n]), a first-order ADAA stage outputs the average of
// f over the straight line from x[n-1] to x[n]:  y = (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1]),
// where F is the antiderivative of f. A hard corner in f becomes a smooth ramp in time, which
// removes most of the aliasing a corner would fold back into the audio band. It costs half a
// sample of delay (a quarter of a base-rate sample here, since it runs at 2x) and a tiny high-cut.
// Urchin's shaper parameters move every sample (bias, sag, sputter), so both F(x[n]) and F(x[n-1])
// are evaluated with today's parameters; that keeps the difference exact when the curve moves.
// The arithmetic is in double: F differences of nearby points would lose too much in float.
//
// Everything here is allocation free and safe on the audio thread.
//
// SPDX-License-Identifier: MIT

#ifndef URCHIN_PARTS_H
#define URCHIN_PARTS_H

#include "dsp/math.h"

#include <cmath>
#include <cstdint>

namespace ur {

// ---------------------------------------------------------------------------------------------
// 2x oversampling with a 47-tap halfband FIR (Kaiser window, beta 8: about 80 dB stopband): the
// same filter as Groundswell's Breakwater and Pressure. Every other tap of a halfband is zero, so
// the up- and down-samplers each cost 12 multiplies per base-rate sample. Latency 2K - 1 = 23.
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
// The shapers. Each has f (the curve) and F (its antiderivative, F(0) = 0).
// ---------------------------------------------------------------------------------------------

// Knee clipper, for v >= 0 (callers mirror it): linear up to 1 - k, a quadratic knee, flat at 1
// from 1 + k. k = 1 is a soft overdrive curve (the knee starts at zero); k -> 0 is a hard clip.
// prep() precomputes the constants, so f and F need no divisions.
struct Knee
{
    double k = 0.5, a = 0.5, i4k = 0.5, i12k = 1.0 / 6.0, top = 0.0;
    void prep(double kk)
    {
        k = kk < 0.02 ? 0.02 : kk;
        a = 1.0 - k; i4k = 1.0 / (4.0 * k); i12k = 1.0 / (12.0 * k);
        top = 0.5 * a * a + 2.0 * k - (2.0 / 3.0) * k * k;
    }
    double f(double v) const
    {
        if (v <= a) return v;
        if (v >= 1.0 + k) return 1.0;
        const double d = v - a;
        return v - d * d * i4k;
    }
    double A(double v) const                           // antiderivative of f
    {
        if (v <= a) return 0.5 * v * v;
        if (v >= 1.0 + k) return top + (v - (1.0 + k));
        const double d = v - a;
        return 0.5 * a * a + a * d + 0.5 * d * d - d * d * d * i12k;
    }
};

// "DzClip": the workhorse. Bias offset b, then a dead zone of half-width dz (the crossover gap of a
// starved transistor: small signals vanish, decays sputter), then an asymmetric knee clip that
// saturates at +P and -N. Optionally rectified afterwards: the negative side is scaled by -w
// (w = 0 half-wave, w = 1 full-wave), which is how the octave fuzz folds its first stage.
// Call set() after changing P, N or k; b, dz and w can change freely.
struct DzClip
{
    double b = 0.0, dz = 0.0, P = 1.0, N = 1.0, iP = 1.0, iN = 1.0;
    bool rect = false;
    double w = 1.0;
    Knee kn;

    void set(double p, double n, double k) { P = p; N = n; iP = 1.0 / p; iN = 1.0 / n; kn.prep(k); }
    double dead(double u) const
    {
        const double v = u + b;
        return v > dz ? v - dz : (v < -dz ? v + dz : 0.0);
    }
    double f(double u) const
    {
        const double x = dead(u);
        if (x >= 0.0) return P * kn.f(x * iP);
        const double y = -N * kn.f(-x * iN);
        return rect ? -w * y : y;
    }
    double F(double u) const
    {
        const double x = dead(u);
        if (x >= 0.0) return P * P * kn.A(x * iP);
        const double y = N * N * kn.A(-x * iN);
        return rect ? -w * y : y;
    }
};

// Rectifier with folds: r = u (u >= 0) or -w u (u < 0), so w = 0 is half-wave and w = 1 full-wave;
// then a triangle fold of period 2 (r folds back down at 1, up again at 2, ...), shifted by phi.
// Rectifying is scale-free, so the drive shows as the number of folds: below 1 it is a plain
// rectifier, above it the hard fold corners add grit. Output 0..1 (DC is blocked afterwards).
struct RectTri
{
    double w = 1.0, phi = 0.0;

    static double tri(double s) { const double m = s - 2.0 * std::floor(s * 0.5); return m <= 1.0 ? m : 2.0 - m; }
    static double T(double s)                          // integral of tri from 0 to s
    {
        const double fl = std::floor(s * 0.5);
        const double m = s - 2.0 * fl;
        const double part = m <= 1.0 ? 0.5 * m * m : 0.5 + 2.0 * (m - 1.0) - 0.5 * (m * m - 1.0);
        return fl + part;
    }
    double f(double u) const { return u >= 0.0 ? tri(u + phi) : tri(phi - w * u); }
    double F(double u) const
    {
        if (u >= 0.0) return T(u + phi) - T(phi);
        if (w < 1.0e-6) return u * tri(phi);
        return -(T(phi - w * u) - T(phi)) / w;
    }
};

// West Coast style folder: a smooth sine-like fold, g(tri(x)) with tri a triangle wave of period 4
// (tri(x) = x near zero) and g(t) = 1.5 t - 0.5 t^3. Its antiderivative is periodic and bounded,
// which keeps ADAA well conditioned however hard it is driven.
struct Folder
{
    double off = 0.0;                                 // symmetry offset (even harmonics)

    static double g(double t) { return 1.5 * t - 0.5 * t * t * t; }
    static double G(double t) { const double t2 = t * t; return 0.75 * t2 - 0.125 * t2 * t2; }
    double f(double x) const
    {
        const double p = x + off + 1.0;
        const double m = p - 4.0 * std::floor(p * 0.25);
        return m < 2.0 ? g(m - 1.0) : g(3.0 - m);
    }
    double F(double x) const
    {
        const double p = x + off + 1.0;
        const double m = p - 4.0 * std::floor(p * 0.25);
        return m < 2.0 ? G(m - 1.0) : 1.25 - G(3.0 - m);
    }
};

// One ADAA stage around any of the shapers above. F(x[n-1]) is cached and only re-evaluated when the
// shaper's parameters have changed (the caller bumps `ver` whenever it changes them).
struct Adaa
{
    double x1 = 0.0, F1 = 0.0;
    uint32_t v1 = 0xFFFFFFFFu;
    void reset() { x1 = 0.0; F1 = 0.0; v1 = 0xFFFFFFFFu; }
    template <class S> float process(const S& s, double x, uint32_t ver)
    {
        if (ver != v1) { F1 = s.F(x1); v1 = ver; }
        const double Fx = s.F(x);
        const double dx = x - x1;
        const double y = std::fabs(dx) > 1.0e-5 ? (Fx - F1) / dx : s.f(0.5 * (x + x1));
        x1 = x; F1 = Fx;
        return (float)y;
    }
};

// ---------------------------------------------------------------------------------------------
// One-pole helpers (coefficients from exp(), so set them per block only)
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
    float process(float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
    void reset() { x1 = y1 = 0.0f; }
    void flush() { y1 = mpc::dsp::flushDenormal(y1); }
};

// Short delay, so the dry and clean-low paths line up with the oversampled wet path.
struct ShortDelay
{
    static constexpr int kSize = 32;                  // > kOversampleLatency, power of two
    float b[kSize] = {};
    int w = 0;
    void reset() { for (float& v : b) v = 0.0f; w = 0; }
    float process(float x, int d)
    {
        b[w] = x;
        const float y = b[(w - d) & (kSize - 1)];
        w = (w + 1) & (kSize - 1);
        return y;
    }
};

// Cheap white noise (an LCG), for the torn cone's paper rattle.
struct Lcg
{
    uint32_t s = 0x1234567u;
    float next() { s = s * 1664525u + 1013904223u; return (float)(int32_t)s * (1.0f / 2147483648.0f); }
};

} // namespace ur

#endif // URCHIN_PARTS_H
