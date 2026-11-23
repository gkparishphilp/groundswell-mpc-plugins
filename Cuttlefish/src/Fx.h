// Fx.h - Cuttlefish's light built-in effects: chorus, ping-pong delay, small reverb.
//
// SPDX-License-Identifier: MIT
//
// Deliberately cheap (Seaglass, Undertow and friends are the real effects). All buffers are allocated
// in prepare(); process() only does arithmetic.
//
//   Chorus   two modulated delay lines (one per side). Chorus: 1 tap per side, quadrature sine LFOs,
//            7-12 ms. Ensemble: 3 taps per side 120 degrees apart, 10-16 ms. Flange: 0.4-3 ms with
//            feedback. ~25 operations per sample.
//   Delay    stereo ping-pong (the input enters the left line, repeats alternate sides), time from a
//            note division at the song tempo, glides when the time changes, low/high-cut in the loop.
//   Reverb   pre-delay, two input allpass diffusers, then a 4-line feedback delay network with a
//            Householder mix and a one-pole damping filter per line. Size sets the decay (0.3-6 s).

#ifndef CUTTLEFISH_FX_H
#define CUTTLEFISH_FX_H

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "dsp/math.h"

namespace cuttlefish {

inline int pow2AtLeast(int n) { int s = 1; while (s < n) s <<= 1; return s; }

// Linear-interpolated read `d` samples behind write position w (d >= 1).
inline float readFrac(const float* buf, int mask, int w, float d)
{
    const int di = (int)d; const float fr = d - (float)di;
    const float a = buf[(w - di) & mask], b = buf[(w - di - 1) & mask];
    return a + fr * (b - a);
}

// ---------------------------------------------------------------------------------------------------
class Chorus
{
public:
    enum Mode { kChorus = 0, kEnsemble, kFlange };
    void prepare(float sr)
    {
        sr_ = sr; size_ = pow2AtLeast((int)(0.04f * sr) + 8); mask_ = size_ - 1;
        l_.assign((size_t)size_, 0.0f); r_.assign((size_t)size_, 0.0f);
    }
    void reset() { std::fill(l_.begin(), l_.end(), 0.0f); std::fill(r_.begin(), r_.end(), 0.0f); w_ = 0; ph_ = 0.0f; fbL_ = fbR_ = 0.0f; }
    // mix, depth 0..1; rate Hz
    void process(float* L, float* R, int n, float mix, float rateHz, float depth, int mode)
    {
        const float inc = rateHz / sr_;
        float base, dep, fb = 0.0f;
        if (mode == kFlange) { base = 0.0004f * sr_; dep = (0.0004f + 0.0026f * depth) * sr_; fb = 0.6f; }
        else if (mode == kEnsemble) { base = 0.010f * sr_; dep = 0.003f * depth * sr_; }
        else { base = 0.007f * sr_; dep = 0.005f * depth * sr_; }
        const float wetG = mode == kEnsemble ? 0.45f : 0.7f;
        for (int i = 0; i < n; ++i)
        {
            ph_ += inc; if (ph_ >= 1.0f) ph_ -= 1.0f;
            const float inL = L[i], inR = R[i];
            l_[(size_t)w_] = inL + fb * fbL_; r_[(size_t)w_] = inR + fb * fbR_;
            float wl, wr;
            if (mode == kEnsemble)
            {
                const float s0 = mpc::dsp::fastSin01(ph_), s1 = mpc::dsp::fastSin01(ph_ + 0.3333f), s2 = mpc::dsp::fastSin01(ph_ + 0.6667f);
                const float t0 = mpc::dsp::fastSin01(ph_ + 0.1667f), t1 = mpc::dsp::fastSin01(ph_ + 0.5f), t2 = mpc::dsp::fastSin01(ph_ + 0.8333f);
                wl = readFrac(l_.data(), mask_, w_, base + dep * (1.0f + s0)) + readFrac(l_.data(), mask_, w_, base + dep * (1.0f + s1))
                   + readFrac(l_.data(), mask_, w_, base + dep * (1.0f + s2));
                wr = readFrac(r_.data(), mask_, w_, base + dep * (1.0f + t0)) + readFrac(r_.data(), mask_, w_, base + dep * (1.0f + t1))
                   + readFrac(r_.data(), mask_, w_, base + dep * (1.0f + t2));
            }
            else
            {
                const float sl = mpc::dsp::fastSin01(ph_), sr = mpc::dsp::fastSin01(ph_ + 0.25f);
                wl = readFrac(l_.data(), mask_, w_, base + 0.5f * dep * (1.0f + sl));
                wr = readFrac(r_.data(), mask_, w_, base + 0.5f * dep * (1.0f + sr));
            }
            fbL_ = wl; fbR_ = wr;
            w_ = (w_ + 1) & mask_;
            L[i] = inL * (1.0f - 0.35f * mix) + wl * wetG * mix;
            R[i] = inR * (1.0f - 0.35f * mix) + wr * wetG * mix;
        }
    }
private:
    std::vector<float> l_, r_;
    float sr_ = 44100.0f, ph_ = 0.0f, fbL_ = 0.0f, fbR_ = 0.0f;
    int size_ = 0, mask_ = 0, w_ = 0;
};

// ---------------------------------------------------------------------------------------------------
class PingPongDelay
{
public:
    void prepare(float sr)
    {
        sr_ = sr; size_ = pow2AtLeast((int)(3.1f * sr) + 8); mask_ = size_ - 1;
        l_.assign((size_t)size_, 0.0f); r_.assign((size_t)size_, 0.0f);
    }
    void reset() { std::fill(l_.begin(), l_.end(), 0.0f); std::fill(r_.begin(), r_.end(), 0.0f); w_ = 0; lpL_ = lpR_ = hpL_ = hpR_ = 0.0f; d_ = -1.0f; }
    // timeSec: delay time; fb 0..1; tone 0..1 (dark..bright); mix 0..1
    void process(float* L, float* R, int n, float mix, float timeSec, float fb, float tone)
    {
        const float target = mpc::dsp::clamp(timeSec * sr_, 16.0f, (float)(size_ - 4));
        if (d_ < 0.0f) d_ = target;
        const float lpC = 1.0f - std::exp(-6.2831853f * (1200.0f * std::pow(10.0f, tone)) / sr_);   // 1.2k..12k
        const float hpC = 1.0f - std::exp(-6.2831853f * 90.0f / sr_);
        const float glide = 1.0f - std::exp(-1.0f / (0.08f * sr_));
        fb = mpc::dsp::clamp(fb, 0.0f, 0.92f);
        for (int i = 0; i < n; ++i)
        {
            d_ += glide * (target - d_);
            const float yl = readFrac(l_.data(), mask_, w_, d_), yr = readFrac(r_.data(), mask_, w_, d_);
            // tone in the loop: low pass, then a gentle high pass
            lpL_ += lpC * (yl - lpL_); lpR_ += lpC * (yr - lpR_);
            hpL_ += hpC * (lpL_ - hpL_); hpR_ += hpC * (lpR_ - hpR_);
            const float fl = lpL_ - hpL_, fr = lpR_ - hpR_;
            const float in = 0.5f * (L[i] + R[i]);
            l_[(size_t)w_] = in + fb * fr;
            r_[(size_t)w_] = fb * fl;
            w_ = (w_ + 1) & mask_;
            L[i] += mix * yl;
            R[i] += mix * yr;
        }
    }
private:
    std::vector<float> l_, r_;
    float sr_ = 44100.0f, lpL_ = 0, lpR_ = 0, hpL_ = 0, hpR_ = 0, d_ = -1.0f;
    int size_ = 0, mask_ = 0, w_ = 0;
};

// ---------------------------------------------------------------------------------------------------
class SmallReverb
{
public:
    void prepare(float sr)
    {
        sr_ = sr;
        const float s = sr / 44100.0f;
        static const int lens[4] = {1087, 1283, 1447, 1663};
        static const int ap[2] = {225, 556};
        for (int i = 0; i < 4; ++i) { len_[i] = (int)(lens[i] * s); line_[i].assign((size_t)pow2AtLeast(len_[i] + 2), 0.0f); }
        for (int i = 0; i < 2; ++i) { apLen_[i] = (int)(ap[i] * s); ap_[i].assign((size_t)pow2AtLeast(apLen_[i] + 2), 0.0f); }
        preSize_ = pow2AtLeast((int)(0.11f * sr) + 4); pre_.assign((size_t)preSize_, 0.0f);
        lastSize_ = -1.0f; lastTone_ = -1.0f;
    }
    void reset()
    {
        for (auto& l : line_) std::fill(l.begin(), l.end(), 0.0f);
        for (auto& a : ap_) std::fill(a.begin(), a.end(), 0.0f);
        std::fill(pre_.begin(), pre_.end(), 0.0f);
        for (int i = 0; i < 4; ++i) { pos_[i] = 0; damp_[i] = 0.0f; }
        apPos_[0] = apPos_[1] = 0; prePos_ = 0;
    }
    // size, tone 0..1; preMs 0..100; mix 0..1
    void process(float* L, float* R, int n, float mix, float size, float tone, float preMs)
    {
        if (size != lastSize_)
        {
            const float rt = 0.3f + 5.7f * size * size;
            for (int i = 0; i < 4; ++i) g_[i] = std::pow(10.0f, -3.0f * (float)len_[i] / (rt * sr_));
            lastSize_ = size;
        }
        if (tone != lastTone_)
        {
            dampC_ = 1.0f - std::exp(-6.2831853f * (1800.0f * std::pow(7.0f, tone)) / sr_);   // 1.8k..12.6k
            lastTone_ = tone;
        }
        const int pd = mpc::dsp::clamp((int)(preMs * 0.001f * sr_), 0, preSize_ - 2);
        const int pm = preSize_ - 1;
        for (int i = 0; i < n; ++i)
        {
            pre_[(size_t)prePos_] = 0.5f * (L[i] + R[i]);
            float x = pre_[(size_t)((prePos_ - pd) & pm)];
            prePos_ = (prePos_ + 1) & pm;
            for (int a = 0; a < 2; ++a)
            {
                std::vector<float>& b = ap_[a]; const int m = (int)b.size() - 1;
                const float d = b[(size_t)((apPos_[a] - apLen_[a]) & m)];
                const float v = x + 0.6f * d;
                b[(size_t)apPos_[a]] = v; apPos_[a] = (apPos_[a] + 1) & m;
                x = d - 0.6f * v;
            }
            float o[4];
            for (int k = 0; k < 4; ++k)
            {
                std::vector<float>& b = line_[k]; const int m = (int)b.size() - 1;
                float y = b[(size_t)((pos_[k] - len_[k]) & m)];
                damp_[k] += dampC_ * (y - damp_[k]);
                o[k] = damp_[k] * g_[k];
            }
            const float h = 0.5f * (o[0] + o[1] + o[2] + o[3]);
            static const float sgn[4] = {1.0f, -1.0f, 1.0f, -1.0f};
            for (int k = 0; k < 4; ++k)
            {
                std::vector<float>& b = line_[k]; const int m = (int)b.size() - 1;
                b[(size_t)pos_[k]] = (o[k] - h) + sgn[k] * x;
                pos_[k] = (pos_[k] + 1) & m;
            }
            const float wl = o[0] + 0.6f * o[2] - 0.3f * o[1];
            const float wr = o[1] + 0.6f * o[3] - 0.3f * o[0];
            L[i] = L[i] * (1.0f - 0.25f * mix) + 0.6f * mix * wl;
            R[i] = R[i] * (1.0f - 0.25f * mix) + 0.6f * mix * wr;
        }
    }
private:
    std::vector<float> line_[4], ap_[2], pre_;
    int len_[4] = {}, pos_[4] = {}, apLen_[2] = {}, apPos_[2] = {}, preSize_ = 0, prePos_ = 0;
    float g_[4] = {}, damp_[4] = {}, dampC_ = 0.5f, sr_ = 44100.0f, lastSize_ = -1.0f, lastTone_ = -1.0f;
};

} // namespace cuttlefish

#endif
