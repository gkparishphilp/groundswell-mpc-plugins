// BassVoice.h - Largemouth's monophonic bass synth.
//
// SPDX-License-Identifier: MIT
//
//   oscillators (by Voice) -> drive (tanh) -> 12 dB low-pass SVF -> amp envelope
//
//   Sub    sine plus a little second harmonic; a short pitch drop on the attack (Punch)
//   808    sine with a long pitch sweep (Punch, up to two octaves) and a decay-only envelope
//   Pluck  saw plus sine, filter envelope (Punch = envelope depth), short sustain
//   Acid   saw into a resonant filter with a snappy filter envelope; loud notes are accented
//   Reese  two saws detuned +-9 cents plus a sine for weight; slow and wide
//
// Glide slides the pitch on legato notes (when Length is 100% the notes overlap). Pitch, filter
// and envelope maths that need pow/tan run every 16 samples, never per sample.

#pragma once

#include "dsp/adsr.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"

#include <cmath>

class BassVoice
{
public:
    enum Kind { kSub = 0, kEight, kPluck, kAcid, kReese, kNumKinds };

    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        saw1_.setSampleRate(sampleRate); saw1_.setWave(mpc::dsp::Wave::Saw);
        saw2_.setSampleRate(sampleRate); saw2_.setWave(mpc::dsp::Wave::Saw);
        sine_.setSampleRate(sampleRate);
        sine2_.setSampleRate(sampleRate);
        svf_.setSampleRate(sampleRate);
        env_.setSampleRate(sampleRate);
        reset();
    }

    void reset()
    {
        svf_.reset(); env_.reset();
        pitch_ = target_ = 40.0f;
        pitchEnv_ = 0.0f; filtEnv_ = 0.0f;
        ctl_ = 0; gate_ = false; velocity_ = 1.0f;
    }

    // Once per block. tone in Hz, reso/drive/punch 0..1, decay and glide in ms.
    void setParams(int kind, float toneHz, float reso, float drive, float decayMs, float punch, float glideMs)
    {
        kind_ = mpc::dsp::clamp(kind, 0, kNumKinds - 1);
        tone_ = mpc::dsp::clamp(toneHz, 30.0f, 12000.0f);
        reso_ = mpc::dsp::clamp(reso, 0.0f, 1.0f);
        drive_ = mpc::dsp::clamp(drive, 0.0f, 1.0f);
        punch_ = mpc::dsp::clamp(punch, 0.0f, 1.0f);
        decayMs = mpc::dsp::clamp(decayMs, 20.0f, 6000.0f);
        glideMs = mpc::dsp::clamp(glideMs, 0.0f, 2000.0f);
        glideCoef_ = glideMs < 1.0f ? 1.0f : 1.0f - std::exp(-(float)kCtl / (glideMs * 0.001f * sr_));
        // Exponential decays per control tick.
        const float pitchTau = kind_ == kEight ? 0.045f : 0.018f;
        pitchDecay_ = std::exp(-(float)kCtl / (pitchTau * sr_));
        const float filtTau = (kind_ == kAcid ? 0.25f : 0.45f) * decayMs * 0.001f;
        filtDecay_ = std::exp(-(float)kCtl / (filtTau * sr_));

        switch (kind_)
        {
            case kSub:   env_.set(2.0f, decayMs, 0.75f, 40.0f); break;
            case kEight: env_.set(1.0f, decayMs, 0.0f, decayMs); break;
            case kPluck: env_.set(1.0f, decayMs * 0.6f, 0.35f, 60.0f); break;
            case kAcid:  env_.set(1.0f, decayMs, 0.85f, 15.0f); break;
            case kReese: env_.set(8.0f, decayMs, 0.8f, 80.0f); break;
            default: break;
        }
        driveGain_ = 1.0f + drive_ * (kind_ == kEight ? 10.0f : 6.0f);
        makeup_ = 1.0f / mpc::dsp::fastTanh(driveGain_ * 0.7f) * 0.7f;
    }

    void noteOn(float midi, float velocity, bool legato)
    {
        target_ = midi;
        velocity_ = mpc::dsp::clamp(velocity, 0.0f, 1.0f);
        const bool slide = legato && gate_ && glideCoef_ < 1.0f && kind_ != kEight;
        if (!slide)
        {
            pitch_ = midi;
            if (kind_ == kSub || kind_ == kEight) { sine_.resetPhase(0.0f); sine2_.resetPhase(0.0f); }
            pitchEnv_ = 1.0f;
            filtEnv_ = 1.0f;
            env_.noteOn();
        }
        else if (kind_ == kAcid && velocity_ > 0.85f)
        {
            filtEnv_ = 1.0f;     // an accented slide still bites
        }
        gate_ = true;
        ctl_ = 0;                // recompute pitch on the next sample
    }

    void noteOff()
    {
        gate_ = false;
        env_.noteOff();
    }

    bool active() const { return env_.isActive(); }
    bool gate() const { return gate_; }

    float process()
    {
        if (!env_.isActive()) return 0.0f;
        if (ctl_ <= 0) { control(); ctl_ = kCtl; }
        --ctl_;

        float x;
        switch (kind_)
        {
            case kSub:   x = sine_.process() + 0.12f * sine2_.process(); break;
            case kEight: x = sine_.process(); break;
            case kPluck: x = 0.55f * saw1_.process() + 0.5f * sine_.process(); break;
            case kAcid:  x = 0.8f * saw1_.process(); break;
            case kReese:
            default:     x = 0.4f * (saw1_.process() + saw2_.process()) + 0.45f * sine_.process(); break;
        }
        x = mpc::dsp::fastTanh(x * driveGain_) * makeup_;
        x = svf_.process(x, mpc::dsp::SVF::Mode::Lowpass);
        const float amp = env_.process() * (0.55f + 0.45f * velocity_);
        return x * amp * kLevel[kind_];
    }

private:
    static constexpr int kCtl = 16;

    void control()
    {
        pitch_ += glideCoef_ * (target_ - pitch_);
        pitchEnv_ *= pitchDecay_;
        filtEnv_ *= filtDecay_;

        float semis = 0.0f;
        if (kind_ == kEight) semis = pitchEnv_ * punch_ * 24.0f;
        else if (kind_ == kSub) semis = pitchEnv_ * punch_ * 12.0f;
        const float hz = 440.0f * std::pow(2.0f, (pitch_ + semis - 69.0f) * (1.0f / 12.0f));
        sine_.setFrequency(hz);
        sine2_.setFrequency(2.0f * hz);
        saw1_.setFrequency(kind_ == kReese ? hz * kDetune : hz);
        saw2_.setFrequency(hz / kDetune);

        float octaves = 0.0f, res = reso_;
        switch (kind_)
        {
            case kPluck: octaves = filtEnv_ * punch_ * 5.0f; break;
            case kAcid:  octaves = filtEnv_ * (0.5f + punch_ * 4.5f) * (0.6f + 0.4f * velocity_);
                         res = 0.35f + 0.6f * reso_; break;
            case kReese: octaves = filtEnv_ * punch_ * 1.5f; break;
            default: break;
        }
        // Key-track a little so high notes are not darker than low ones.
        const float track = std::pow(2.0f, (pitch_ - 40.0f) * (0.5f / 12.0f));
        const float cutoff = mpc::dsp::clamp(tone_ * track * std::pow(2.0f, octaves), 30.0f, 0.45f * sr_);
        svf_.set(cutoff, res * 0.92f);
    }

    static constexpr float kDetune = 1.00521f;   // 2^(9/1200)
    static constexpr float kLevel[kNumKinds] = {0.7f, 1.1f, 1.15f, 0.6f, 0.75f};   // roughly equal loudness

    float sr_ = 44100.0f;
    mpc::dsp::PolyBlepOsc saw1_, saw2_;
    mpc::dsp::WavetableSine sine_, sine2_;
    mpc::dsp::SVF svf_;
    mpc::dsp::ADSR env_;
    int kind_ = kEight;
    float tone_ = 1200.0f, reso_ = 0.2f, drive_ = 0.25f, punch_ = 0.4f;
    float glideCoef_ = 1.0f, pitchDecay_ = 0.9f, filtDecay_ = 0.99f;
    float driveGain_ = 2.0f, makeup_ = 1.0f;
    float pitch_ = 40.0f, target_ = 40.0f, pitchEnv_ = 0.0f, filtEnv_ = 0.0f, velocity_ = 1.0f;
    int ctl_ = 0;
    bool gate_ = false;
};
