// Seaglass - a lush reverb you can freeze, for the MPC, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// A stereo hall-style reverb with three tricks:
//   FREEZE   holds whatever is in the tail as an endless pad. The loop is crossfaded (over "Fade")
//            into a lossless state: feedback at exactly 1, damping out, input and shimmer muted.
//            Switching it off crossfades back to the normal decay, so the pad lets go naturally.
//   SHIMMER  a pitch shifter in the feedback loop: the tail is shifted up an octave (or a fifth,
//            two octaves, or down an octave) and fed back in, so every pass rises and sparkles.
//   SWELL    ducks the tail while you play and lets it bloom back in the gaps ("Rise" seconds,
//            S-shaped), the way Tidepools waits for you to stop.
//
// SIGNAL FLOW
//
//   in L/R -+-------------------------------------------------------------- dry ---+
//           |                                                                      |
//           +-> Low Cut (one-pole HP) -> Pre-Delay -> (+ shimmer feed) -> 4 input   |
//               allpass diffusers per side -> x input gain x (1 - freeze) ->        |
//               8-line feedback delay network (Hadamard mix, per-line damping       |
//               "Tone", per-line gain from "Decay", 4 lines modulated by "Motion")  |
//                 -> wet L/R -> Width (M/S) -> x swell duck --------------- wet ----+-> Mix -> Output
//                      |                                                               -> soft ceiling
//                      +-> shimmer path: 2 low-passes + high-pass -> pitch shifter (two
//                          crossfaded grains per side) -> x Shimmer x limiter -> back into the loop
//
// The FDN is Abyss's reverb (same delay lengths, Hadamard mix, input scaled so a held sound is
// about equally loud at any decay), with allpass interpolation on the modulated lines instead of
// linear (linear interpolation is a low-pass, which would slowly drain a frozen loop) and input
// diffusion added in front (so drum hits smear into a wash instead of a flutter of echoes).
//
// CPU: every coefficient (exp, pow, log) is computed once per block; the per-sample work is
// table-free arithmetic: about 8 delay reads, 4 allpass interpolations, a 24-add Hadamard
// transform, 8 one-pole filters and 8 diffuser allpasses, plus the shimmer path (skipped
// entirely while Shimmer is at 0).

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/smoother.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace mpc::dsp;

namespace seaglass {

// Reverb delay lengths in samples at 44.1 kHz (35..93 ms, mutually prime), from Abyss.
static const int kFdnLen[8] = {1559, 1907, 2243, 2467, 2851, 3307, 3637, 4111};
// The four lines whose length is slowly modulated ("Motion"), with their rates (Hz) and phases.
static const int kModLine[4] = {0, 2, 5, 7};
static const float kModRate[4] = {0.13f, 0.21f, 0.17f, 0.29f};
static const float kModPhase[4] = {0.0f, 0.37f, 0.61f, 0.83f};
constexpr float kModMaxDepth = 14.0f;    // samples at 44.1 kHz, peak

// Input diffusers: allpass lengths (samples at 44.1 kHz) and coefficients, per side.
static const int kDiffLenL[4] = {210, 159, 562, 410};
static const int kDiffLenR[4] = {227, 168, 547, 397};
static const float kDiffCoef[4] = {0.75f, 0.75f, 0.625f, 0.625f};

// Shimmer intervals. Internal names carry the kIv prefix so they can never collide with the
// parameter enum inside the class (Abyss 1.0 shipped two dead knobs because of exactly that).
enum IntervalChoice { kIvOctave = 0, kIvFifth, kIvTwoOct, kIvOctDown };
static const char* const kIntervalNames[] = {"Octave", "Fifth", "2 Oct", "Oct Down"};
static const float kIntervalRatio[] = {2.0f, 1.5f, 4.0f, 0.5f};
constexpr float kGrainSec = 0.07f;       // pitch shifter window (70 ms)

constexpr float kWetTrim = 1.0f;         // wet level so a held sound's tail is about as loud as the input
constexpr float kShimCeiling = 0.3f;     // the shimmer feed backs off when the wet RMS passes this
constexpr float kShimMaxFeed = 1.1f;     // shimmer feed at 100 %

inline float smooth01(float x)
{
    const float t = clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Output ceiling: untouched below 0.8 (-1.9 dBFS), then bends smoothly toward 0.98 (-0.2 dBFS).
inline float ceiling(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.8f) return x;
    const float y = 0.8f + 0.18f * fastTanh((a - 0.8f) * (1.0f / 0.18f));
    return x < 0.0f ? -y : y;
}

// A delay buffer with a power-of-two size.
struct Ring
{
    std::vector<float> buf;
    int mask = 0;
    void allocate(int minLen)
    {
        int size = 16;
        while (size < minLen) size <<= 1;
        buf.assign((size_t)size, 0.0f);
        mask = size - 1;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
};

// Schroeder allpass diffuser.
struct Diffuser
{
    Ring r;
    int len = 1;
    float g = 0.7f;
    int pos = 0;
    float process(float x)
    {
        const float d = r.buf[(size_t)((pos - len) & r.mask)];
        const float v = x - g * d;
        r.buf[(size_t)(pos & r.mask)] = v;
        ++pos;
        return d + g * v;
    }
};

// Pitch shifter: two read taps sweep through a short delay at (1 - ratio) samples per sample,
// half a window apart, each faded in and out by a triangle so the jump back is silent.
struct Shifter
{
    Ring r;
    int pos = 0;
    float phase = 0.0f;   // 0..1 across the window
    float read(float d) const
    {
        const int di = (int)d;
        const float fr = d - (float)di;
        const float s0 = r.buf[(size_t)((pos - di) & r.mask)];
        const float s1 = r.buf[(size_t)((pos - di - 1) & r.mask)];
        return s0 + fr * (s1 - s0);
    }
    float process(float x, float rate, float window)
    {
        r.buf[(size_t)(pos & r.mask)] = x;
        phase += rate;
        if (phase >= 1.0f) phase -= 1.0f;
        else if (phase < 0.0f) phase += 1.0f;
        float p2 = phase + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        const float wa = 1.0f - std::fabs(2.0f * phase - 1.0f);
        const float wb = 1.0f - std::fabs(2.0f * p2 - 1.0f);
        const float y = wa * read(phase * window) + wb * read(p2 * window);
        ++pos;
        return y;
    }
};

} // namespace seaglass

using namespace seaglass;

class Seaglass : public mpc::Plugin
{
public:
    // Parameter order and ids are permanent once users have projects: append new ones at the end.
    enum Params
    {
        kMix = 0,
        kDecay, kPreDelay, kTone, kLowCut, kMotion, kWidth, kOutput,
        kFreeze, kFade,
        kShimmer, kInterval,
        kSwell, kRise, kSense,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Seaglass";
        i.vendor = "Groundswell";
        i.uniqueId = MPC_FOURCC('G', 'v', 'X', 'f');   // == "uid" in plugin.json. Never change.
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.tailSamples = 0;   // unknown: Decay goes to 30 s and Freeze is endless
        return i;
    }

    Seaglass() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        std::vector<std::string> intervals;
        for (const char* s : kIntervalNames) intervals.push_back(s);

        // Dry / wet balance (equal power). Parameter 0: the host nudges it on load, which is harmless.
        addParam(floatParam ("mix",      "Mix", 0.0f, 100.0f, 35.0f, "%", 0));
        // Reverb time (RT60): how long the tail takes to fall by 60 dB.
        addParam(logParam   ("decay",    "Decay", 0.5f, 30.0f, 5.0f, "", 1));        // text from formatValue
        // Gap before the reverb starts.
        addParam(floatParam ("predelay", "Pre-Delay", 0.0f, 250.0f, 20.0f, "", 0));  // text from formatValue
        // High-frequency damping inside the tail: lower is darker.
        addParam(logParam   ("tone",     "Tone", 1000.0f, 16000.0f, 7000.0f, "", 0)); // text from formatValue
        // High-pass on the way into the reverb, so the low end stays clean.
        addParam(logParam   ("lowcut",   "Low Cut", 20.0f, 1000.0f, 120.0f, "", 0)); // text from formatValue
        // Slow movement of four delay lines: a gentle chorus in the tail.
        addParam(floatParam ("motion",   "Motion", 0.0f, 100.0f, 40.0f, "%", 0));
        // Stereo width of the tail (0 = mono, 100 = full).
        addParam(floatParam ("width",    "Width", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(floatParam ("output",   "Output", -24.0f, 6.0f, 0.0f, "dB", 1));
        // Hold the tail as an endless pad; Fade is how long it takes to settle in and to let go.
        addParam(boolParam  ("freeze",   "Freeze", false));
        addParam(logParam   ("fade",     "Fade", 20.0f, 4000.0f, 250.0f, "", 0));    // text from formatValue
        // Pitch-shifted feedback: how strongly the tail climbs, and by what interval.
        addParam(floatParam ("shimmer",  "Shimmer", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("interval", "Interval", intervals, kIvOctave));
        // Swell: how far the tail ducks while you play, how long it takes to bloom back, and the
        // input level that counts as playing.
        addParam(floatParam ("swell",    "Swell", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("rise",     "Rise", 0.1f, 8.0f, 1.5f, "", 2));          // text from formatValue
        addParam(floatParam ("sense",    "Sense", -60.0f, -12.0f, -36.0f, "dB", 0));

        if (numParams() != kNumParams) mpc::log("Seaglass: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Seaglass", {});
        program("Frosted Shore", {{kMix, 30}, {kDecay, 2.8f}, {kPreDelay, 30}, {kTone, 9500}, {kMotion, 30}, {kLowCut, 180}});
        program("Wave-Worn", {{kMix, 45}, {kDecay, 12}, {kTone, 2600}, {kLowCut, 70}, {kMotion, 60}, {kPreDelay, 40}});
        program("Tumbled Light", {{kMix, 45}, {kDecay, 10}, {kTone, 8000}, {kShimmer, 55}, {kInterval, kIvOctave}, {kMotion, 45}});
        program("Moon Pool", {{kMix, 50}, {kDecay, 14}, {kTone, 6500}, {kShimmer, 45}, {kInterval, kIvFifth}, {kMotion, 55}, {kPreDelay, 60}});
        program("Kelp Shadow", {{kMix, 40}, {kDecay, 9}, {kTone, 3800}, {kShimmer, 40}, {kInterval, kIvOctDown}, {kLowCut, 60}, {kWidth, 85}});
        program("Cobalt Bloom", {{kMix, 50}, {kDecay, 8}, {kSwell, 85}, {kRise, 2.5f}, {kShimmer, 20}, {kTone, 7500}});
        program("Ghost Tide", {{kMix, 60}, {kDecay, 18}, {kSwell, 100}, {kRise, 4.0f}, {kShimmer, 30}, {kTone, 5500}, {kMotion, 55}});
        program("Endless Shoal", {{kMix, 60}, {kDecay, 20}, {kFade, 1200}, {kTone, 6000}, {kMotion, 45}, {kPreDelay, 10}});
    }

    // ----- lifecycle ------------------------------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        scale_ = sr_ / 44100.0f;
        for (int k = 0; k < 8; ++k)
        {
            fdnLen_[k] = (int)std::lround((float)kFdnLen[k] * scale_);
            line_[k].allocate(fdnLen_[k] + 8);
        }
        for (int j = 0; j < 4; ++j)
        {
            diffL_[j].len = std::max(1, (int)std::lround((float)kDiffLenL[j] * scale_));
            diffR_[j].len = std::max(1, (int)std::lround((float)kDiffLenR[j] * scale_));
            diffL_[j].g = diffR_[j].g = kDiffCoef[j];
            diffL_[j].r.allocate(diffL_[j].len + 2);
            diffR_[j].r.allocate(diffR_[j].len + 2);
        }
        preL_.allocate((int)(0.26f * sr_) + 8);
        preR_.allocate((int)(0.26f * sr_) + 8);
        window_ = kGrainSec * sr_;
        shiftL_.r.allocate((int)window_ + 8);
        shiftR_.r.allocate((int)window_ + 8);
        envAtt_ = 1.0f - std::exp(-1.0f / (0.003f * sr_));
        envRel_ = 1.0f - std::exp(-1.0f / (0.12f * sr_));
        reset();
    }

    void reset() override
    {
        for (int k = 0; k < 8; ++k) { line_[k].clear(); lp_[k] = 0.0f; }
        for (int j = 0; j < 4; ++j) apY_[j] = 0.0f;
        for (int j = 0; j < 4; ++j) { diffL_[j].r.clear(); diffR_[j].r.clear(); diffL_[j].pos = diffR_[j].pos = 0; }
        preL_.clear(); preR_.clear();
        shiftL_.r.clear(); shiftR_.r.clear();
        shiftL_.pos = shiftR_.pos = 0;
        shiftL_.phase = 0.0f; shiftR_.phase = 0.25f;
        fdnPos_ = 0; prePos_ = 0;
        hpL_ = hpR_ = 0.0f;
        shLp1L_ = shLp2L_ = shHpL_ = shLp1R_ = shLp2R_ = shHpR_ = 0.0f;
        shOutL_ = shOutR_ = 0.0f;
        env_ = 0.0f;
        wetRms_ = 0.0f;
        for (int j = 0; j < 4; ++j) modPhase_[j] = kModPhase[j];

        // Snap every smoothed value to its target, so a reset does not glide.
        decaySm_ = get(kDecay);
        computeLoop(decaySm_, get(kTone), gTarget_, dTarget_, inGTarget_);
        for (int k = 0; k < 8; ++k) g_[k] = gTarget_[k];
        d_ = dTarget_; inG_ = inGTarget_;
        modDepth_ = modDepthTarget();
        for (int j = 0; j < 4; ++j) modNow_[j] = modOffset(j, modPhase_[j], modDepth_);
        preDelay_ = preDelayTarget();
        freeze_ = getBool(kFreeze) ? 1.0f : 0.0f;
        swellState_ = 0.0f;
        duck_.snap(1.0f);
        shimGain_.snap(0.0f);
        float dg, wg; mixGains(get(kMix) * 0.01f, dg, wg);
        dry_.snap(dg); wet_.snap(wg);
        out_.snap(dbToGain(get(kOutput)));
        width_.snap(get(kWidth) * 0.01f);
    }

    // ----- audio ----------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (n <= 0 || ctx.numOutputs < 1) return;
        const float* inL = (ctx.inputs && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* inR = (ctx.inputs && ctx.numInputs > 1) ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const float fn = (float)n;
        const float invN = 1.0f / fn;

        // ---- per-block control: everything with exp/pow/log happens here ----
        // Decay glides (in about 80 ms) so turning it never steps the loop gain.
        const float decayT = get(kDecay);
        decaySm_ += (decayT - decaySm_) * std::min(1.0f, fn / (0.08f * sr_));
        computeLoop(decaySm_, get(kTone), gTarget_, dTarget_, inGTarget_);
        float gStep[8];
        for (int k = 0; k < 8; ++k) gStep[k] = (gTarget_[k] - g_[k]) * invN;
        const float dStep = (dTarget_ - d_) * invN;
        const float inGStep = (inGTarget_ - inG_) * invN;
        const float hpC = 1.0f - std::exp(-kTwoPi * clamp(get(kLowCut), 10.0f, 0.2f * sr_) / sr_);

        // Delay modulation: the four modulated lengths glide linearly through the block.
        modDepth_ += (modDepthTarget() - modDepth_) * std::min(1.0f, fn / (0.3f * sr_));
        float modStep[4];
        for (int j = 0; j < 4; ++j)
        {
            modPhase_[j] = wrapPhase(modPhase_[j] + kModRate[j] * fn / sr_);
            modStep[j] = (modOffset(j, modPhase_[j], modDepth_) - modNow_[j]) * invN;
        }

        // Pre-delay glides too (a short pitch bend while you turn it, never a jump).
        const float preStart = preDelay_;
        preDelay_ += (preDelayTarget() - preDelay_) * std::min(1.0f, fn / (0.05f * sr_));
        const float preStep = (preDelay_ - preStart) * invN;
        float pre = preStart;

        // Freeze: a linear ramp over Fade, read through an S-curve.
        const float freezeTarget = getBool(kFreeze) ? 1.0f : 0.0f;
        const float freezeStep = 1.0f / (std::max(0.02f, get(kFade) * 0.001f) * sr_);

        // Shimmer: interval, filters, and a feed level that backs off when the tail gets loud.
        const int iv = clamp(getInt(kInterval), 0, 3);
        const float ratio = kIntervalRatio[iv];
        const float shRate = (1.0f - ratio) / window_;
        const float shLpHz = ratio > 1.0f ? std::min(get(kTone), 12000.0f / ratio) : get(kTone);
        const float shLpC = 1.0f - std::exp(-kTwoPi * clamp(shLpHz, 200.0f, 0.4f * sr_) / sr_);
        const float shHpC = 1.0f - std::exp(-kTwoPi * clamp(std::max(get(kLowCut), 60.0f), 10.0f, 0.2f * sr_) / sr_);
        const float limit = wetRms_ > kShimCeiling ? kShimCeiling / wetRms_ : 1.0f;
        shimGain_.setTarget(get(kShimmer) * 0.01f * kShimMaxFeed * limit, n);
        const bool shimOn = shimGain_.value() > 0.0f || shimGain_.target() > 0.0f;

        // Swell: read the input level, then duck fast or bloom back over Rise.
        const float levelDb = 20.0f * std::log10(env_ + 1e-9f);
        const float sense = get(kSense);
        const float playing = smooth01((levelDb - (sense - 6.0f)) / 12.0f);
        const float swellTarget = get(kSwell) * 0.01f * playing;
        if (swellTarget > swellState_) swellState_ = std::min(swellTarget, swellState_ + fn / (0.06f * sr_));
        else swellState_ = std::max(swellTarget, swellState_ - fn / (std::max(0.1f, get(kRise)) * sr_));
        duck_.setTarget(1.0f - smooth01(swellState_), n);

        float dg, wg; mixGains(get(kMix) * 0.01f, dg, wg);
        dry_.setTarget(dg, n);
        wet_.setTarget(wg * kWetTrim, n);
        out_.setTarget(dbToGain(get(kOutput)), n);
        width_.setTarget(get(kWidth) * 0.01f, n);

        Ring& pl = preL_; Ring& pr = preR_;
        const int preMask = pl.mask;
        double wetSq = 0.0;
        float env = env_;

        for (int i = 0; i < n; ++i)
        {
            // Read the input of this frame first (outputs may share the buffers).
            const float xl = inL ? inL[i] : 0.0f;
            const float xr = inR ? inR[i] : xl;

            // Envelope follower for Swell (fast attack, 120 ms release).
            const float a = std::max(std::fabs(xl), std::fabs(xr));
            env += (a > env ? envAtt_ : envRel_) * (a - env);

            // Low cut, then pre-delay (linear interpolation, gliding delay).
            hpL_ += hpC * (xl - hpL_);
            hpR_ += hpC * (xr - hpR_);
            pl.buf[(size_t)(prePos_ & preMask)] = xl - hpL_;
            pr.buf[(size_t)(prePos_ & preMask)] = xr - hpR_;
            pre += preStep;
            const int pi = (int)pre;
            const float pf = pre - (float)pi;
            const float pl0 = pl.buf[(size_t)((prePos_ - pi) & preMask)], pl1 = pl.buf[(size_t)((prePos_ - pi - 1) & preMask)];
            const float pr0 = pr.buf[(size_t)((prePos_ - pi) & preMask)], pr1 = pr.buf[(size_t)((prePos_ - pi - 1) & preMask)];
            float sl = pl0 + pf * (pl1 - pl0);
            float sr = pr0 + pf * (pr1 - pr0);
            ++prePos_;

            // Freeze ramp.
            if (freeze_ < freezeTarget) freeze_ = std::min(freezeTarget, freeze_ + freezeStep);
            else if (freeze_ > freezeTarget) freeze_ = std::max(freezeTarget, freeze_ - freezeStep);
            const float fz = smooth01(freeze_);
            const bool frozen = fz >= 1.0f;

            // Shimmer feed (computed from the previous frame's tail) joins the input.
            const float sg = shimGain_.next();
            if (shimOn)
            {
                sl += fastTanh(shOutL_ * sg);
                sr += fastTanh(shOutR_ * sg);
            }

            // Input diffusion.
            for (int j = 0; j < 4; ++j) { sl = diffL_[j].process(sl); sr = diffR_[j].process(sr); }
            inG_ += inGStep;
            const float inMul = inG_ * (1.0f - fz);
            const float il = sl * inMul, ir = sr * inMul;

            // 8-line FDN: read (allpass-interpolated on the modulated lines), tap the output, damp,
            // apply the loop gain, Hadamard mix, inject, write.
            float y[8];
            for (int k = 0; k < 8; ++k) y[k] = line_[k].buf[(size_t)((fdnPos_ - fdnLen_[k]) & line_[k].mask)];
            for (int j = 0; j < 4; ++j)
            {
                const int k = kModLine[j];
                modNow_[j] += modStep[j];
                const float dly = (float)fdnLen_[k] - modNow_[j];
                const int m = (int)(dly - 0.5f);
                const float delta = dly - (float)m;            // 0.5 .. 1.5: keeps the allpass well behaved
                const float c = (1.0f - delta) / (1.0f + delta);
                const Ring& rb = line_[k];
                const float x0 = rb.buf[(size_t)((fdnPos_ - m) & rb.mask)];
                const float x1 = rb.buf[(size_t)((fdnPos_ - m - 1) & rb.mask)];
                const float v = c * (x0 - apY_[j]) + x1;
                apY_[j] = v;
                y[k] = v;
            }
            const float wl = 0.5f * (y[0] - y[2] + y[4] - y[6]);
            const float wr = 0.5f * (y[1] - y[3] + y[5] - y[7]);

            d_ += dStep;
            if (frozen)
            {
                for (int k = 0; k < 8; ++k) { g_[k] += gStep[k]; lp_[k] = y[k]; }
            }
            else
            {
                const float d = d_ + fz * (1.0f - d_);
                for (int k = 0; k < 8; ++k)
                {
                    g_[k] += gStep[k];
                    lp_[k] += d * (y[k] - lp_[k]);
                    y[k] = lp_[k] * (g_[k] + fz * (1.0f - g_[k]));
                }
            }
            // Fast Walsh-Hadamard transform, normalised by 1/sqrt(8): lossless.
            for (int h = 1; h < 8; h <<= 1)
                for (int s = 0; s < 8; s += h << 1)
                    for (int b = s; b < s + h; ++b)
                    {
                        const float x0 = y[b], x1 = y[b + h];
                        y[b] = x0 + x1; y[b + h] = x0 - x1;
                    }
            constexpr float kNorm = 0.35355339f;
            for (int k = 0; k < 8; ++k)
            {
                float v = y[k] * kNorm + ((k & 1) ? ((k & 2) ? -ir : ir) : ((k & 2) ? -il : il));
                v = clamp(v, -8.0f, 8.0f);   // safety only: never reached in normal use
                line_[k].buf[(size_t)(fdnPos_ & line_[k].mask)] = v;
            }
            ++fdnPos_;

            // Shimmer path: filter the tail and shift it; it re-enters on the next frame.
            if (shimOn)
            {
                shLp1L_ += shLpC * (wl - shLp1L_);     shLp1R_ += shLpC * (wr - shLp1R_);
                shLp2L_ += shLpC * (shLp1L_ - shLp2L_); shLp2R_ += shLpC * (shLp1R_ - shLp2R_);
                shHpL_ += shHpC * (shLp2L_ - shHpL_);   shHpR_ += shHpC * (shLp2R_ - shHpR_);
                shOutL_ = shiftL_.process(shLp2L_ - shHpL_, shRate, window_);
                shOutR_ = shiftR_.process(shLp2R_ - shHpR_, shRate, window_);
            }

            // Wet: width, swell duck, mix, output, ceiling.
            const float w = width_.next();
            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * w;
            const float duck = duck_.next();
            const float wetG = wet_.next() * duck;
            const float el = (mid + side) * wetG, er = (mid - side) * wetG;
            wetSq += (double)(wl * wl + wr * wr);
            const float dryG = dry_.next(), og = out_.next();
            outL[i] = ceiling((xl * dryG + el) * og);
            if (outR) outR[i] = ceiling((xr * dryG + er) * og);
        }
        env_ = env;
        fdnPos_ &= 0x3FFFFFFF;
        prePos_ &= 0x3FFFFFFF;
        for (int j = 0; j < 4; ++j) { diffL_[j].pos &= 0x3FFFFFFF; diffR_[j].pos &= 0x3FFFFFFF; }
        shiftL_.pos &= 0x3FFFFFFF; shiftR_.pos &= 0x3FFFFFFF;

        // Tail level for the shimmer limiter (about 50 ms smoothing).
        const float rms = std::sqrt((float)(wetSq / (2.0 * fn)));
        wetRms_ += (rms - wetRms_) * (1.0f - std::exp(-fn / (0.05f * sr_)));

        // Belt and braces: if anything ever went non-finite, start the reverb again from silence.
        if (!std::isfinite(wetRms_) || !std::isfinite(lp_[0]) || !std::isfinite(shOutL_))
        {
            for (int k = 0; k < 8; ++k) { line_[k].clear(); lp_[k] = 0.0f; }
            for (int j = 0; j < 4; ++j) apY_[j] = 0.0f;
            shiftL_.r.clear(); shiftR_.r.clear();
            shOutL_ = shOutR_ = 0.0f; wetRms_ = 0.0f;
            shLp1L_ = shLp2L_ = shHpL_ = shLp1R_ = shLp2R_ = shHpR_ = 0.0f;
        }
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        char buf[32];
        switch (index)
        {
        case kDecay:
            std::snprintf(buf, sizeof(buf), v < 10.0f ? "%.1f s" : "%.0f s", (double)v);
            break;
        case kRise:
            if (v < 1.0f) std::snprintf(buf, sizeof(buf), "%.0f ms", (double)v * 1000.0);
            else std::snprintf(buf, sizeof(buf), "%.1f s", (double)v);
            break;
        case kPreDelay:
            std::snprintf(buf, sizeof(buf), "%.0f ms", (double)v);
            break;
        case kFade:
            if (v < 1000.0f) std::snprintf(buf, sizeof(buf), "%.0f ms", (double)v);
            else std::snprintf(buf, sizeof(buf), "%.1f s", (double)v * 0.001);
            break;
        case kTone:
        case kLowCut:
            if (v >= 1000.0f) std::snprintf(buf, sizeof(buf), "%.1f kHz", (double)v * 0.001);
            else std::snprintf(buf, sizeof(buf), "%.0f Hz", (double)v);
            break;
        default:
            return false;
        }
        out = buf;
        return true;
    }

private:
    // Loop gains for a decay time (RT60) and the damping coefficient for Tone; the input gain keeps a
    // held sound at about the same level whatever the decay (from Abyss).
    void computeLoop(float decay, float toneHz, float* g, float& d, float& inG) const
    {
        float sumG2 = 0.0f;
        for (int k = 0; k < 8; ++k)
        {
            g[k] = std::pow(10.0f, -3.0f * (float)fdnLen_[k] / (sr_ * std::max(0.1f, decay)));
            sumG2 += g[k] * g[k];
        }
        inG = 0.75f * std::sqrt(std::max(0.02f, 1.0f - sumG2 / 8.0f));
        d = 1.0f - std::exp(-kTwoPi * clamp(toneHz, 200.0f, 0.45f * sr_) / sr_);
    }

    float modDepthTarget() const { return get(kMotion) * 0.01f * kModMaxDepth * scale_; }
    // How much shorter than nominal a modulated line is: always at least 2 samples, so the
    // allpass read never touches the sample being written.
    static float modOffset(int /*j*/, float phase, float depth) { return 2.0f + depth * (1.0f + fastSin01(phase)); }
    float preDelayTarget() const { return clamp(get(kPreDelay), 0.0f, 250.0f) * 0.001f * sr_; }

    static void mixGains(float mix, float& dry, float& wet)
    {
        const float t = clamp(mix, 0.0f, 1.0f) * kHalfPi;
        dry = std::cos(t);
        wet = std::sin(t);
    }

    float sr_ = 44100.0f, scale_ = 1.0f;

    Ring line_[8];
    int fdnLen_[8] = {};
    int fdnPos_ = 0;
    float lp_[8] = {};
    float g_[8] = {}, gTarget_[8] = {};
    float d_ = 0.5f, dTarget_ = 0.5f;
    float inG_ = 0.3f, inGTarget_ = 0.3f;
    float decaySm_ = 5.0f;
    float apY_[4] = {};
    float modPhase_[4] = {}, modNow_[4] = {};
    float modDepth_ = 0.0f;

    Diffuser diffL_[4], diffR_[4];
    Ring preL_, preR_;
    int prePos_ = 0;
    float preDelay_ = 0.0f;
    float hpL_ = 0.0f, hpR_ = 0.0f;

    Shifter shiftL_, shiftR_;
    float window_ = 3087.0f;
    float shLp1L_ = 0, shLp2L_ = 0, shHpL_ = 0, shLp1R_ = 0, shLp2R_ = 0, shHpR_ = 0;
    float shOutL_ = 0.0f, shOutR_ = 0.0f;
    float wetRms_ = 0.0f;

    float freeze_ = 0.0f;
    float env_ = 0.0f, envAtt_ = 0.01f, envRel_ = 0.001f;
    float swellState_ = 0.0f;

    LinearRamp duck_, shimGain_, dry_, wet_, out_, width_;
};

MPC_PLUGIN_ENTRY(Seaglass)
