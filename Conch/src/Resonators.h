// Resonators.h - one bank of up to 16 tuned resonators, in one of three materials.
//
// SPDX-License-Identifier: MIT
//
// Materials
// ---------
//   String  a sympathetic string per note: a comb filter (delay = one period) with a one-pole
//           low-pass in the loop (Bright), the Karplus-Strong loop. One delay line rings every
//           harmonic of its note.
//   Pipe    a stopped pipe per note: the same loop, half a period long, with the feedback inverted.
//           That rings only the odd harmonics (1, 3, 5...), the hollow, clarinet-like colour of a
//           shell held to the ear.
//   Bell    three modes per note at 1 : 2.76 : 5.40 (a free bar, the gamelan / glockenspiel ratios),
//           each a coupled-form ("magic circle") two-pole resonator. Bright lifts the upper modes
//           and makes them ring longer. Three resonators per note, still cheap.
//
// The fractional part of a string's delay is read with a 4-point Lagrange interpolator, which keeps
// high notes in tune and lets Tune and Shimmer move the pitch without clicks. The loop's low-pass
// delays the signal a little too; that delay is measured at the note's frequency and taken off the
// line length, so a dark string is as in tune as a bright one.
//
// A bank is tuned once, when it starts, and never retuned to new notes: a chord change starts a fresh
// bank and lets this one ring out (Releasing: no new input, decay shortened to the ring-out time).
// Tune and Shimmer are small glides of the notes it already has.
//
// Cost per sample and note: String/Pipe ~12 multiplies, Bell ~15. Coefficients (pow, exp, atan2,
// cos, sin) are only computed when a bank starts or a knob moves; Shimmer and Tune use a cheap
// rotation update once per 32-sample sub-block.

#pragma once

#include "dsp/math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace conch {

enum Material { kString = 0, kBell, kPipe, kNumMaterials };

constexpr int   kMaxPitches = 16;
constexpr int   kModes = 3;
constexpr int   kSub = 32;                 // sub-block: coefficients move at most this often
constexpr float kTwoPi = 6.283185307f;
constexpr float kLn1000 = 6.907755279f;    // T60: amplitude down by 1000
constexpr float kExcitePow = 0.3f;         // input gain = (1 - g^2)^this; see Bank::computeTargets
constexpr float kBellExcitePow = 0.5f;     // the same for a bell mode (g per sample, not per cycle)

static const float kBellRatio[kModes] = {1.0f, 2.76f, 5.40f};

// Block-rate settings every bank follows.
struct Shared
{
    float decay = 2.5f;      // seconds to -60 dB (the note's fundamental)
    float bright = 0.55f;    // 0..1
    float width = 0.6f;      // 0..1
    float tune = 1.0f;       // frequency ratio from the Tune knob
    float shimmer = 0.0f;    // peak relative frequency drift
};

class Bank
{
public:
    enum State { kIdle = 0, kActive, kReleasing, kKilling };

    // `mem` holds kMaxPitches lines of `lineLen` floats (a power of two).
    void attach(float* mem, int lineLen) { mem_ = mem; lineLen_ = lineLen; mask_ = lineLen - 1; }

    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        inStep_ = 1.0f / (0.005f * sr_);          // input fades in/out over 5 ms
        freezeStep_ = 1.0f / (0.15f * sr_);       // Freeze lets the input go over 150 ms
        killStep_ = 1.0f / (0.015f * sr_);        // a bank needed for a new chord fades out in 15 ms
        reset();
    }

    void reset()
    {
        state_ = kIdle;
        n_ = 0;
        inGain_ = inTarget_ = 0.0f;
        outGain_ = 1.0f;
        peak_ = 0.0f;
    }

    State state() const { return state_; }
    int material() const { return material_; }
    float peak() const { return peak_; }

    // Tune the bank to `notes` (MIDI, A440 grid) and start listening to the input.
    void start(const int* notes, int n, int material, float trim, const Shared& sh)
    {
        material_ = material;
        n_ = n < 0 ? 0 : (n > kMaxPitches ? kMaxPitches : n);
        trim_ = trim;
        state_ = kActive;
        frozen_ = false;
        inGain_ = 0.0f;
        inTarget_ = 1.0f;
        outGain_ = 1.0f;
        peak_ = 0.0f;
        silentSubs_ = 0;
        tuneNow_ = sh.tune;
        int maxPeriod = 1;
        for (int i = 0; i < n_; ++i)
        {
            const float f = 440.0f * std::pow(2.0f, (float)(notes[i] - 69) / 12.0f);
            freq_[i] = f;
            period_[i] = (material_ == kPipe ? 0.5f : 1.0f) * sr_ / f;
            // Each note's shimmer drifts at its own slow rate and phase.
            lfoPhase_[i] = (float)((i * 7919) % 1000) * 0.001f;
            lfoRate_[i] = 0.07f + 0.031f * (float)((i * 5) % 8);
            z_[i] = 0.0f;
            for (int k = 0; k < kModes; ++k) { s1_[i][k] = 0.0f; s2_[i][k] = 0.0f; }
            const float w = kTwoPi * f / sr_;
            for (int k = 0; k < kModes; ++k)
            {
                const float wk = w * kBellRatio[k];
                modeW_[i][k] = wk;
                modeOn_[i][k] = wk < kTwoPi * 0.45f && f * kBellRatio[k] < 18000.0f;
                c0_[i][k] = std::cos(wk);
                s0_[i][k] = std::sin(wk);
            }
            if ((int)period_[i] > maxPeriod) maxPeriod = (int)period_[i];
        }
        if (material_ != kBell)
        {
            // Clear just the part of each line this bank can read (a little over one period).
            const int clearLen = mpc::clamp((int)((float)maxPeriod * 1.1f) + 8, 8, lineLen_);
            for (int i = 0; i < n_; ++i)
            {
                float* line = mem_ + (size_t)i * (size_t)lineLen_;
                std::memset(line, 0, sizeof(float) * (size_t)clearLen);
                if (clearLen < lineLen_)   // the tail end, where reads wrap to
                    std::memset(line + lineLen_ - clearLen, 0, sizeof(float) * (size_t)clearLen);
            }
        }
        wpos_ = 0;
        silentLimit_ = maxPeriod / kSub + 8;
        cached_ = false;
        computeTargets(sh);
        snap();
    }

    // Stop listening and ring out over `ringT60` seconds (or the Decay, if shorter).
    void release(float ringT60)
    {
        if (state_ != kActive) return;
        state_ = kReleasing;
        inTarget_ = 0.0f;
        frozen_ = false;
        ringT60_ = ringT60;
        cached_ = false;
    }

    // Fade out fast: the bank is needed for a new chord.
    void kill() { if (state_ != kIdle) { state_ = kKilling; inTarget_ = 0.0f; } }

    void setFrozen(bool f)
    {
        if (state_ != kActive || f == frozen_) return;
        frozen_ = f;
        inTarget_ = f ? 0.0f : 1.0f;
        cached_ = false;
    }

    // Once per sub-block, before process(): follow the knobs, move shimmer and tune.
    void update(const Shared& sh, float subSeconds)
    {
        if (!cached_ || sh.decay != cDecay_ || sh.bright != cBright_ || sh.width != cWidth_)
            computeTargets(sh);

        // Tune glides at most ~0.9 cent per sub-block, so a knob turn never jumps the delay.
        const float maxStep = 0.0005f * tuneNow_;
        tuneNow_ += mpc::clamp(sh.tune - tuneNow_, -maxStep, maxStep);

        const float k = 0.25f;
        for (int i = 0; i < n_; ++i)
        {
            lfoPhase_[i] += lfoRate_[i] * subSeconds;
            if (lfoPhase_[i] >= 1.0f) lfoPhase_[i] -= 1.0f;
            const float ratio = tuneNow_ * (1.0f + sh.shimmer * mpc::dsp::fastSin01(lfoPhase_[i]));
            pl_[i] += k * (plT_[i] - pl_[i]);
            pr_[i] += k * (prT_[i] - pr_[i]);

            if (material_ == kBell)
            {
                for (int m = 0; m < kModes; ++m)
                {
                    r_[i][m] += k * (rT_[i][m] - r_[i][m]);
                    mg_[i][m] += k * (mgT_[i][m] - mg_[i][m]);
                    mnorm_[i][m] += k * (mnormT_[i][m] - mnorm_[i][m]);
                    // Rotate the base angle by phi = w * (ratio - 1): second-order update, then
                    // renormalise so the pole radius stays exactly r.
                    const float phi = modeW_[i][m] * (ratio - 1.0f);
                    const float a = 1.0f - 0.5f * phi * phi;
                    const float c = c0_[i][m] * a - s0_[i][m] * phi;
                    const float s = s0_[i][m] * a + c0_[i][m] * phi;
                    const float rr = r_[i][m] * (1.0f - 0.125f * phi * phi * phi * phi);
                    rc_[i][m] = rr * c;
                    rs_[i][m] = rr * s;
                }
            }
            else
            {
                g_[i] += k * (gT_[i] - g_[i]);
                norm_[i] += k * (normT_[i] - norm_[i]);
                const float target = mpc::clamp(period_[i] / ratio - lpDelay_[i], 2.0f, (float)(lineLen_ - 4));
                const float step = 0.0005f * delay_[i] + 0.002f;
                delay_[i] += mpc::clamp(target - delay_[i], -step, step);
                setLagrange(i);
            }
        }
    }

    // Add this bank's output for `n` samples (n <= kSub) of excitation `exc` into outL/outR.
    void process(const float* exc, int n, float* outL, float* outR)
    {
        if (state_ == kIdle) return;

        // This bank's share of the excitation: a ramp toward its input target.
        float ex[kSub];
        const float step = frozen_ ? freezeStep_ : inStep_;
        for (int s = 0; s < n; ++s)
        {
            if (inGain_ < inTarget_) { inGain_ += step; if (inGain_ > inTarget_) inGain_ = inTarget_; }
            else if (inGain_ > inTarget_) { inGain_ -= step; if (inGain_ < inTarget_) inGain_ = inTarget_; }
            ex[s] = exc[s] * inGain_;
        }

        float bl[kSub], br[kSub];
        for (int s = 0; s < n; ++s) { bl[s] = 0.0f; br[s] = 0.0f; }

        if (material_ == kBell) processBell(ex, n, bl, br);
        else processComb(ex, n, bl, br, material_ == kPipe ? -1.0f : 1.0f);
        wpos_ = (wpos_ + n) & mask_;

        float pk = 0.0f;
        for (int s = 0; s < n; ++s)
        {
            if (state_ == kKilling)
            {
                outGain_ -= killStep_;
                if (outGain_ < 0.0f) outGain_ = 0.0f;
            }
            const float g = outGain_ * trim_;
            const float l = bl[s] * g, r = br[s] * g;
            outL[s] += l;
            outR[s] += r;
            const float a = std::fabs(l) > std::fabs(r) ? std::fabs(l) : std::fabs(r);
            if (a > pk) pk = a;
        }
        peak_ = pk;

        if (state_ == kKilling && outGain_ <= 0.0f) state_ = kIdle;
        else if (state_ == kReleasing)
        {
            // Silent for longer than its longest period: nothing left in the loop.
            silentSubs_ = pk < 2e-6f ? silentSubs_ + 1 : 0;
            if (silentSubs_ > silentLimit_) state_ = kIdle;
        }
    }

private:
    void computeTargets(const Shared& sh)
    {
        cached_ = true;
        cDecay_ = sh.decay; cBright_ = sh.bright; cWidth_ = sh.width;

        const bool freeze = frozen_ && state_ == kActive;
        float t60 = mpc::clamp(sh.decay, 0.05f, 60.0f);
        if (state_ == kReleasing || state_ == kKilling) t60 = t60 < ringT60_ ? t60 : ringT60_;
        const float b = mpc::clamp(sh.bright, 0.0f, 1.0f);
        // Pan: alternate left and right, wider as the notes go up (the lowest stay near the middle).
        for (int i = 0; i < n_; ++i)
        {
            float pos = 0.0f;
            if (n_ > 1) pos = ((i & 1) ? 1.0f : -1.0f) * (0.35f + 0.65f * (float)i / (float)(n_ - 1));
            mpc::dsp::equalPowerPan(sh.width * pos, plT_[i], prT_[i]);
        }

        // The excitation is scaled by (1 - g^2)^0.3, g being the per-cycle (or per-sample) decay.
        // At ^0.5 every resonator would put out the same energy whatever its decay; at ^0 a long
        // decay would boom. ^0.3 lets longer decays ring somewhat louder without running away.
        // A bell mode is one narrow peak with g per sample: it gets ^0.5, or a kick drum sweeping
        // through a low mode's frequency would make it boom far louder than a string.
        if (material_ == kBell)
        {
            for (int i = 0; i < n_; ++i)
                for (int m = 0; m < kModes; ++m)
                {
                    if (!modeOn_[i][m]) { rT_[i][m] = 0.0f; mgT_[i][m] = 0.0f; mnormT_[i][m] = 0.0f; continue; }
                    // Upper modes die faster, more so when dark.
                    const float tk = t60 * std::pow(kBellRatio[m], -(0.4f + 1.0f * (1.0f - b)));
                    const float rNorm = std::exp(-kLn1000 / (tk * sr_));
                    float r = rNorm;
                    if (freeze) r = std::exp(-kLn1000 / (300.0f * sr_));
                    rT_[i][m] = r < 0.9999995f ? r : 0.9999995f;
                    mgT_[i][m] = m == 0 ? 1.0f : (m == 1 ? 0.18f + 0.5f * b : 0.06f + 0.36f * b);
                    mnormT_[i][m] = std::pow(1.0f - rNorm * rNorm, kBellExcitePow);
                }
        }
        else
        {
            // Loop low-pass y = (1-d) x + d y1. Bright 100% = almost no damping. Each note's d is
            // capped so the filter costs its fundamental at most 0.05% per trip round the loop:
            // otherwise a dark high note (hundreds of trips a second) would die in a blink whatever
            // the Decay. So Bright darkens low notes more than high ones, as on a real string.
            float dBright = 0.02f + 0.83f * std::pow(1.0f - b, 1.5f);
            if (freeze) dBright *= 0.3f;               // a frozen string keeps its overtones
            for (int i = 0; i < n_; ++i)
            {
                const float w = kTwoPi * freq_[i] / sr_;
                const float ratioMax = 0.0316f / w;     // |H(w0)| >= 0.9995
                const float dMax = ratioMax / (1.0f + ratioMax);
                const float d = dBright < dMax ? dBright : dMax;
                d_[i] = d;
                const float sw = std::sin(w), cw = std::cos(w);
                lpDelay_[i] = std::atan2(d * sw, 1.0f - d * cw) / w;
                const float lpMag = (1.0f - d) / std::sqrt(1.0f - 2.0f * d * cw + d * d);
                // Per-cycle gain so the fundamental reaches -60 dB in t60, after the low-pass's own loss.
                const float gDecay = std::pow(10.0f, -3.0f * period_[i] / (t60 * sr_));
                float g = gDecay / lpMag;
                if (freeze) g = 0.99997f;
                gT_[i] = g < 0.99997f ? g : 0.99997f;  // never 1: the loop's DC gain must stay below 1
                normT_[i] = std::pow(1.0f - gDecay * gDecay, kExcitePow);
            }
        }
    }

    // Jump straight to the targets (a fresh bank has nothing ringing to smooth).
    void snap()
    {
        for (int i = 0; i < n_; ++i)
        {
            pl_[i] = plT_[i]; pr_[i] = prT_[i];
            g_[i] = gT_[i]; norm_[i] = normT_[i];
            delay_[i] = mpc::clamp(period_[i] / tuneNow_ - lpDelay_[i], 2.0f, (float)(lineLen_ - 4));
            setLagrange(i);
            for (int m = 0; m < kModes; ++m)
            {
                r_[i][m] = rT_[i][m]; mg_[i][m] = mgT_[i][m]; mnorm_[i][m] = mnormT_[i][m];
                rc_[i][m] = r_[i][m] * c0_[i][m];
                rs_[i][m] = r_[i][m] * s0_[i][m];
            }
        }
    }

    void setLagrange(int i)
    {
        const float D = delay_[i];
        const int id = (int)D;
        const float f = D - (float)id;
        idel_[i] = id;
        // 4-point Lagrange through delays id-1, id, id+1, id+2, evaluated at id + f.
        h_[i][0] = -f * (f - 1.0f) * (f - 2.0f) * (1.0f / 6.0f);
        h_[i][1] = (f + 1.0f) * (f - 1.0f) * (f - 2.0f) * 0.5f;
        h_[i][2] = -(f + 1.0f) * f * (f - 2.0f) * 0.5f;
        h_[i][3] = (f + 1.0f) * f * (f - 1.0f) * (1.0f / 6.0f);
    }

    void processComb(const float* ex, int n, float* bl, float* br, float sign)
    {
        for (int i = 0; i < n_; ++i)
        {
            float* line = mem_ + (size_t)i * (size_t)lineLen_;
            const int id = idel_[i];
            const float h0 = h_[i][0], h1 = h_[i][1], h2 = h_[i][2], h3 = h_[i][3];
            const float d = d_[i], fb = sign * g_[i], nm = norm_[i];
            const float pl = pl_[i], pr = pr_[i];
            float z = z_[i];
            int w = wpos_;
            for (int s = 0; s < n; ++s)
            {
                const int r0 = w - id;
                const float y = h0 * line[(r0 + 1) & mask_] + h1 * line[r0 & mask_]
                              + h2 * line[(r0 - 1) & mask_] + h3 * line[(r0 - 2) & mask_];
                z = y + d * (z - y);
                line[w] = fb * z + ex[s] * nm;
                w = (w + 1) & mask_;
                bl[s] += y * pl;
                br[s] += y * pr;
            }
            z_[i] = z;
        }
    }

    void processBell(const float* ex, int n, float* bl, float* br)
    {
        float acc[kSub];
        for (int i = 0; i < n_; ++i)
        {
            for (int s = 0; s < n; ++s) acc[s] = 0.0f;
            for (int m = 0; m < kModes; ++m)
            {
                if (!modeOn_[i][m]) continue;
                const float rc = rc_[i][m], rs = rs_[i][m], nm = mnorm_[i][m], g = mg_[i][m];
                float a = s1_[i][m], b = s2_[i][m];
                for (int s = 0; s < n; ++s)
                {
                    const float a1 = rc * a - rs * b + ex[s] * nm;
                    b = rs * a + rc * b;
                    a = a1;
                    acc[s] += b * g;
                }
                s1_[i][m] = a; s2_[i][m] = b;
            }
            const float pl = pl_[i], pr = pr_[i];
            for (int s = 0; s < n; ++s) { bl[s] += acc[s] * pl; br[s] += acc[s] * pr; }
        }
    }

    float sr_ = 44100.0f;
    float* mem_ = nullptr;
    int lineLen_ = 4096, mask_ = 4095, wpos_ = 0;

    State state_ = kIdle;
    int material_ = kString, n_ = 0;
    bool frozen_ = false, cached_ = false;
    float trim_ = 1.0f, ringT60_ = 1.0f;
    float inGain_ = 0.0f, inTarget_ = 0.0f, outGain_ = 1.0f;
    float inStep_ = 0.0f, freezeStep_ = 0.0f, killStep_ = 0.0f;
    float peak_ = 0.0f;
    int silentSubs_ = 0, silentLimit_ = 16;
    float tuneNow_ = 1.0f;
    float cDecay_ = -1.0f, cBright_ = -1.0f, cWidth_ = -1.0f;

    // per note
    float freq_[kMaxPitches] = {}, period_[kMaxPitches] = {};
    float lfoPhase_[kMaxPitches] = {}, lfoRate_[kMaxPitches] = {};
    float pl_[kMaxPitches] = {}, pr_[kMaxPitches] = {}, plT_[kMaxPitches] = {}, prT_[kMaxPitches] = {};
    // String / Pipe
    float g_[kMaxPitches] = {}, gT_[kMaxPitches] = {}, norm_[kMaxPitches] = {}, normT_[kMaxPitches] = {};
    float d_[kMaxPitches] = {}, z_[kMaxPitches] = {}, lpDelay_[kMaxPitches] = {}, delay_[kMaxPitches] = {};
    float h_[kMaxPitches][4] = {};
    int idel_[kMaxPitches] = {};
    // Bell
    bool modeOn_[kMaxPitches][kModes] = {};
    float modeW_[kMaxPitches][kModes] = {}, c0_[kMaxPitches][kModes] = {}, s0_[kMaxPitches][kModes] = {};
    float r_[kMaxPitches][kModes] = {}, rT_[kMaxPitches][kModes] = {};
    float rc_[kMaxPitches][kModes] = {}, rs_[kMaxPitches][kModes] = {};
    float mg_[kMaxPitches][kModes] = {}, mgT_[kMaxPitches][kModes] = {};
    float mnorm_[kMaxPitches][kModes] = {}, mnormT_[kMaxPitches][kModes] = {};
    float s1_[kMaxPitches][kModes] = {}, s2_[kMaxPitches][kModes] = {};
};

} // namespace conch
