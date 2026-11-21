// KickTracker.h - finds kick drums in a drum track or a full mix.
//
// SPDX-License-Identifier: MIT
//
// How it works
// ------------
//   input -> high-pass 30 Hz -> two low-pass 100 Hz biquads (the "kick band", 24 dB/oct, so a
//         snare's 180-250 Hz body is 20 dB down) -> rectify -> envelope (0.3 ms attack, 40 ms
//         release: long enough that a 45 Hz kick tail does not ripple into fresh onsets)
//
// A hit is an onset in that band: the envelope is above an absolute floor AND more than Ratio
// times the highest it was 10-15 ms ago. Comparing against the recent past rather than a long average means
// a held 808 or a bass note in the same band does not mask the next kick: only a fresh rise counts.
// After an onset the tracker measures the peak for 8 ms (a kick often starts at 150 Hz or more and
// only drops into its body a few ms later), then reports the hit with a velocity
// relative to the loudest recent kick, and ignores the band for 70 ms so one kick is one hit.
// A hit much quieter than recent kicks (below a fraction set by Sens) is not a kick - a snare or tom
// leaking into the band - and is dropped. After a hit the tracker re-arms when the band has fallen
// to 60% of that kick's peak, or 160 ms later, whichever comes first.
//
// So a hit is reported about 9 ms after the kick starts. latencySamples() says exactly how long,
// and the plugin subtracts it when it learns where in the bar the kicks fall.

#pragma once

#include "dsp/biquad.h"
#include "dsp/math.h"

#include <cmath>

class KickTracker
{
public:
    void prepare(double sampleRate)
    {
        sr_ = sampleRate;
        using mpc::dsp::BiquadCoeffs;
        hp_.setCoeffs(BiquadCoeffs::highpass(sampleRate, 30.0f));
        const BiquadCoeffs lp = BiquadCoeffs::lowpass(sampleRate, 100.0f, 0.7071f);
        lp1_.setCoeffs(lp);
        lp2_.setCoeffs(lp);
        attack_  = coeff(0.3f);
        release_ = coeff(40.0f);
        refractory_ = (int)(0.070 * sampleRate);
        rearmAfter_ = (int)(0.160 * sampleRate);
        measureLen_ = (int)(0.008 * sampleRate);
        // The 10 ms history is kept at one entry per kHistDecim samples.
        historyLen_ = mpc::dsp::clamp((int)(0.015 * sampleRate / kHistDecim), 3, kHistMax);
        historyWin_ = mpc::dsp::clamp((int)(0.005 * sampleRate / kHistDecim), 1, historyLen_ - 1);
        reset();
    }

    void reset()
    {
        hp_.reset(); lp1_.reset(); lp2_.reset();
        env_ = 0.0f;
        for (float& h : history_) h = 0.0f;
        histPos_ = 0; histCount_ = 0;
        holdoff_ = 0; measuring_ = 0; peak_ = 0.0f; lastPeak_ = 0.0f; sinceHit_ = 1 << 30; armed_ = true;
        ref_ = 0.05f; velocity_ = 0.0f;
    }

    // 0..1. Higher finds quieter and softer kicks (and, eventually, toms and bass notes).
    void setSensitivity(float s01)
    {
        s01 = mpc::dsp::clamp(s01, 0.0f, 1.0f);
        ratio_ = mpc::dsp::lerp(4.0f, 1.6f, s01);
        floor_ = std::pow(10.0f, mpc::dsp::lerp(-26.0f, -52.0f, s01) * 0.05f);
        relFloor_ = mpc::dsp::lerp(0.6f, 0.15f, s01);
    }

    // Feed one (mono) sample. Returns true on the sample a hit is confirmed.
    bool process(float x)
    {
        const float y = lp2_.process(lp1_.process(hp_.process(x)));
        const float a = std::fabs(y);
        env_ += (a > env_ ? attack_ : release_) * (a - env_);

        // The highest the envelope was 10-15 ms ago (the oldest few entries in the ring).
        float past = 0.0f;
        for (int k = 0; k < historyWin_; ++k)
        {
            const float h = history_[(histPos_ + k) % historyLen_];
            if (h > past) past = h;
        }
        if (++histCount_ >= kHistDecim)
        {
            histCount_ = 0;
            history_[histPos_] = env_;
            histPos_ = (histPos_ + 1) % historyLen_;
        }
        if (sinceHit_ < (1 << 30)) ++sinceHit_;

        if (measuring_ > 0)
        {
            if (env_ > peak_) peak_ = env_;
            if (--measuring_ == 0)
            {
                if (peak_ < relFloor_ * ref_) { holdoff_ = 0; return false; }   // too quiet to be a kick
                ref_ = peak_ > ref_ ? peak_ : ref_ * 0.97f + peak_ * 0.03f;
                velocity_ = mpc::dsp::clamp(peak_ / (ref_ + 1e-9f), 0.25f, 1.0f);
                lastPeak_ = peak_;
                sinceHit_ = 0;
                armed_ = false;
                return true;
            }
            return false;
        }
        if (holdoff_ > 0) { --holdoff_; return false; }
        if (!armed_ && (env_ < 0.6f * lastPeak_ || sinceHit_ > rearmAfter_)) armed_ = true;

        if (armed_ && env_ > floor_ && env_ > ratio_ * past)
        {
            measuring_ = measureLen_ > 0 ? measureLen_ : 1;
            holdoff_ = refractory_;
            peak_ = env_;
        }
        return false;
    }

    float velocity() const { return velocity_; }
    float envelope() const { return env_; }
    // How long after a kick starts the hit is reported (onset rise plus the peak window).
    int latencySamples() const { return measureLen_ + (int)(0.0015 * sr_); }

private:
    float coeff(float ms) const { return 1.0f - std::exp(-1.0f / (ms * 0.001f * (float)sr_)); }

    static constexpr int kHistDecim = 16;
    static constexpr int kHistMax = 64;

    double sr_ = 44100.0;
    mpc::dsp::Biquad hp_, lp1_, lp2_;
    float attack_ = 0.1f, release_ = 0.01f;
    float env_ = 0.0f;
    float history_[kHistMax] = {};
    int historyLen_ = 41, historyWin_ = 13, histPos_ = 0, histCount_ = 0;
    float ratio_ = 2.5f, floor_ = 0.01f;
    int refractory_ = 3000, rearmAfter_ = 7000, measureLen_ = 176;
    float relFloor_ = 0.25f, lastPeak_ = 0.0f;
    int sinceHit_ = 1 << 30;
    bool armed_ = true;
    int holdoff_ = 0, measuring_ = 0;
    float peak_ = 0.0f, ref_ = 0.05f, velocity_ = 0.0f;
};
