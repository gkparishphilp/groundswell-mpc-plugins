// Undertow - a dub delay with an effects chain inside its feedback loop, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// Signal flow (per channel):
//
//   in ─┬────────────────────────────────────────────────────────────┐ dry
//       └─► delay ─► FILTER ─► FX ─► DRIVE ─► DC block ─┬─► WET LIMITER ┴─► mix ─► Output ─► safety knee ─► out
//            ▲     (Off/LP/  (Chorus,  (tube-style      │
//            │      BP/HP)    Flanger,  saturation,     │
//            │                Phaser,   always on)      │
//            │                Bitcrush, Downsample,     │
//            │                Ring Mod, Fuzz)           │
//            └──────────── x Feedback (0-120%) ◄────────┘    Ping-Pong: mono input into L, L feeds R, R feeds L
//
// Gain staging: every stage in the loop is compensated to about unity gain (the filter's resonant peak is
// allowed up to +3.5 dB), so Feedback means what it says: repeats grow only above 100%, or from ~67% at
// full Resonance.
//
// Why it stays musical at 120% feedback and full resonance: the drive stage is always in the loop and
// its output is bounded, so the repeats can grow until they saturate but never run to infinity. The
// wet limiter then pins the repeats under -3 dBFS (Limit pushes them into it, from 0 to +18 dB), and
// a soft knee on the final output catches anything above -1 dBFS. The dry signal is not limited.
//
// Delay time glides (about 150 ms) when you change it, so turning Time or Division warps the pitch of
// the repeats like tape. Wobble adds wow (~0.5 Hz) and flutter (~6 Hz) to the delay time.
//
// Parameters (ids are permanent once released - never rename or reorder; append new ones at the end):
//    0 Output     dB  -30..0       0      level after everything (the host nudges parameter 0 on load)
//    1 Sync       Off/On           On     tempo-synced Division, or free Time
//    2 Division   1/64..2/1        1/8D   delay time when synced to the MPC tempo
//    3 Time       ms  10..5000     375    delay time when not synced
//    4 Feedback   %   0..120       55     above 100% the loop runs away into the drive stage
//    5 Mix        %   0..100       35     dry stays at full level up to 50%, wet reaches full level at 50%
//    6 Ping-Pong  Off/On           On     repeats bounce between left and right
//    7 Wobble     %   0..100       15     tape wow and flutter on the delay time
//    8 Limit      %   0..100       30     how hard the repeats are pushed into the wet limiter (0..+18 dB)
//    9 Filter     Off/LP/BP/HP     LP     filter in the loop (every repeat goes through it again)
//   10 Cutoff     Hz  40..16000    1800
//   11 Resonance  %   0..100       35     near 100% the filter self-oscillates
//   12 Drive      %   0..100       25     tube-style saturation in the loop
//   13 FX Type    Off, Chorus, Flanger, Phaser, Bitcrush, Downsample, Ring Mod, Fuzz   (append new types at the end)
//   14 FX Amount  %   0..100       50     how much of the effect (depth / bits / reduction / mix / gain)
//   15 FX Rate    %   0..100       30     second control (speed / sample rate / smoothing / carrier / tone)

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/delay_line.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"
#include "dsp/svf.h"
#include "dsp/tempo.h"

#include <cmath>

using namespace mpc::dsp;

namespace {

enum ParamIndex
{
    kOutput = 0, kSync, kDivision, kTime, kFeedback, kMix, kPingPong, kWobble, kLimit,
    kFilterMode, kCutoff, kResonance, kDrive, kFxType, kFxAmount, kFxRate, kNumParams
};

enum FilterMode { kFilterOff = 0, kFilterLP, kFilterBP, kFilterHP };
enum FxType { kFxOff = 0, kFxChorus, kFxFlanger, kFxPhaser, kFxBitcrush, kFxDownsample, kFxRingMod, kFxFuzz };

constexpr float kMaxDelaySeconds = 5.0f;
constexpr float kFxDelaySeconds = 0.04f;     // chorus / flanger delay line
constexpr int   kCoeffInterval = 16;         // filter coefficients are refreshed every 16 samples
constexpr float kWetCeiling = 0.7079f;       // -3 dBFS
constexpr float kOutKnee = 0.8913f;          // -1 dBFS: the output soft knee starts here...
constexpr float kOutCeiling = 0.9886f;       // ...and never passes -0.1 dBFS

// sin(2*pi*phase) for phase in [0,1): parabola plus one refinement step. Cleaner than the SDK's
// fastSin01 (which skips the refinement), which matters for the audible ring-mod carrier.
// (max error ~0.1%): plenty for LFOs and the ring modulator, far cheaper than std::sin.
inline float sine01(float phase)
{
    float x = phase < 0.5f ? 2.0f * phase : 2.0f * phase - 2.0f;   // -1..1, sin(pi * x)
    float y = 4.0f * x * (1.0f - std::fabs(x));
    return y * (0.775f + 0.225f * std::fabs(y));
}

inline float advance(float phase, float inc) { phase += inc; return phase >= 1.0f ? phase - 1.0f : phase; }

// Soft knee: transparent below `knee`, then a tanh curve that never exceeds `ceiling`.
inline float softKnee(float x, float knee, float ceiling)
{
    const float a = std::fabs(x);
    if (a <= knee) return x;
    const float range = ceiling - knee;
    const float y = knee + range * fastTanh((a - knee) / range);
    return x < 0.0f ? -y : y;
}

// Everything one channel needs inside the loop.
struct LoopChannel
{
    DelayLine delay;
    DelayLine fxDelay;           // chorus / flanger
    SVF filter;
    float flangeFb = 0.0f;
    float apState[4] = {};       // phaser allpass stages
    float phaserFb = 0.0f;
    float phaserCoef = 0.0f;     // refreshed every 16 samples from the LFO
    float held = 0.0f;           // bitcrush / downsample sample-and-hold
    float holdPhase = 1.0f;
    OnePoleLP tone;              // downsample smoothing / fuzz tone
    DCBlocker dc;
    float loopOut = 0.0f;        // this channel's processed repeat, fed back into the delay

    void clearFx()
    {
        fxDelay.reset();
        flangeFb = 0.0f;
        for (float& s : apState) s = 0.0f;
        phaserFb = 0.0f;
        held = 0.0f; holdPhase = 1.0f;
        tone.reset();
    }
    void clear()
    {
        delay.reset();
        filter.reset();
        clearFx();
        dc.reset();
        loopOut = 0.0f;
    }
};

// Per-block settings for the FX slot, shared by both channels.
struct FxSettings
{
    int type = kFxOff;
    float amount = 0.0f, rate = 0.0f;
    float lfoInc = 0.0f;             // modulation speed (phase per sample)
    float chorusCentre = 0.0f, chorusDepth = 0.0f, chorusMix = 0.0f;
    float flangeMin = 0.0f, flangeRange = 0.0f, flangeFb = 0.0f, flangeMix = 0.0f, flangeComp = 1.0f;
    float phaserLowHz = 0.0f, phaserOctaves = 0.0f, phaserFb = 0.0f, phaserMix = 0.0f, phaserComp = 1.0f;
    float quantSteps = 32768.0f, holdInc = 1.0f;
    float ringInc = 0.0f, ringMix = 0.0f;
    float fuzzGain = 1.0f;
    float toneHz = 18000.0f;         // downsample smoothing / fuzz tone
    float piOverSr = 0.0f;
};

} // namespace

class Undertow : public mpc::Plugin
{
public:
    Undertow() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("output",   "Output",    -30.0f, 0.0f,    0.0f,  "dB", 1));
        addParam(choiceParam("sync",     "Sync",      {"Off", "On"}, 1));
        addParam(choiceParam("div",      "Division",  divisionNames(), 9));   // 1/8D
        addParam(logParam   ("time",     "Time",      10.0f, 5000.0f, 375.0f, "ms", 0));
        addParam(floatParam ("feedback", "Feedback",  0.0f, 120.0f,   55.0f,  "%", 0));
        addParam(floatParam ("mix",      "Mix",       0.0f, 100.0f,   35.0f,  "%", 0));
        addParam(boolParam  ("pingpong", "Ping-Pong", true));
        addParam(floatParam ("wobble",   "Wobble",    0.0f, 100.0f,   15.0f,  "%", 0));
        addParam(floatParam ("limit",    "Limit",     0.0f, 100.0f,   30.0f,  "%", 0));
        addParam(choiceParam("fmode",    "Filter",    {"Off", "LP", "BP", "HP"}, kFilterLP));
        addParam(logParam   ("cutoff",   "Cutoff",    40.0f, 16000.0f, 1800.0f, "Hz", 0));
        addParam(floatParam ("reso",     "Resonance", 0.0f, 100.0f,   35.0f,  "%", 0));
        addParam(floatParam ("drive",    "Drive",     0.0f, 100.0f,   25.0f,  "%", 0));
        addParam(choiceParam("fxtype",   "FX Type",   {"Off", "Chorus", "Flanger", "Phaser", "Bitcrush",
                                                       "Downsample", "Ring Mod", "Fuzz"}, kFxOff));
        addParam(floatParam ("fxamount", "FX Amount", 0.0f, 100.0f,   50.0f,  "%", 0));
        addParam(floatParam ("fxrate",   "FX Rate",   0.0f, 100.0f,   30.0f,  "%", 0));
        if (numParams() != kNumParams) mpc::log("Undertow: parameter count mismatch");

        // Programs. Division indexes: 6=1/16D 7=1/8 8=1/4T 9=1/8D 10=1/4 12=1/4D. Filter 0..3 = Off/LP/BP/HP.
        // FX 0..7 = Off/Chorus/Flanger/Phaser/Bitcrush/Downsample/Ring Mod/Fuzz.
        //                            Out Sync Div Time   Fb   Mix PP Wob Lim  Flt Cut     Res Drv  FX Amt Rate
        addProgram("Dub Echo",         { 0, 1,  9, 375,   60,  35, 1, 15, 30,  1, 1800,   35, 25,  0, 50, 30 });
        addProgram("Siren Sweep",      { 0, 1,  7, 375,   68,  40, 1, 10, 60,  2, 1200,   75, 40,  0, 50, 30 });
        addProgram("Chorus Wash",      { 0, 1, 10, 500,   65,  35, 1, 25, 30,  1, 4000,   20, 15,  1, 60, 25 });
        addProgram("Crushed Repeats",  { 0, 1,  6, 375,   70,  35, 1,  5, 40,  3,  300,   30, 30,  4, 65, 40 });
        addProgram("Flange Spiral",    { 0, 1,  8, 375,   80,  40, 1, 10, 40,  1, 6000,   40, 20,  2, 70, 20 });
        addProgram("Lo-Fi Tape",       { 0, 1,  9, 375,   55,  30, 1, 45, 30,  1, 2500,   15, 35,  5, 35, 60 });
        addProgram("Self-Oscillation", { 0, 1, 12, 375,  110,  45, 1, 20, 70,  2,  800,   92, 55,  0, 50, 30 });
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        const int maxDelay = (int)(sampleRate * (kMaxDelaySeconds + 0.1f)) + 4;   // + room for wobble
        for (LoopChannel& c : ch_)
        {
            c.delay.prepare(maxDelay);
            c.fxDelay.prepare((int)(sampleRate * kFxDelaySeconds) + 4);
            c.filter.setSampleRate(sampleRate);
            c.dc.prepare(sampleRate, 10.0f);
        }
        delayTime_.setTime(150.0f, sampleRate);   // tape-style glide
        feedback_.setTime(30.0f, sampleRate);
        mix_.setTime(20.0f, sampleRate);
        output_.setTime(20.0f, sampleRate);
        logCutoff_.setTime(25.0f, sampleRate);
        limAttack_ = 1.0f - std::exp(-1.0f / (0.0003f * sr_));   // 0.3 ms
        limRelease_ = 1.0f - std::exp(-1.0f / (0.080f * sr_));   // 80 ms
    }

    void reset() override
    {
        for (LoopChannel& c : ch_) c.clear();
        delayTime_.snap(targetDelaySamples(120.0));
        feedback_.snap(get(kFeedback) * 0.01f);
        mix_.snap(get(kMix) * 0.01f);
        output_.snap(dbToGain(get(kOutput)));
        logCutoff_.snap(std::log2(get(kCutoff)));
        limGain_ = 1.0f;
        wowPhase_ = 0.0f; flutterPhase_ = 0.3f; fxLfoPhase_ = 0.0f; ringPhase_ = 0.0f;
        coeffCounter_ = 0;
        lastFxType_ = getInt(kFxType);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // ----- per block: read parameters -----------------------------------------------------
        delayTime_.setTarget(targetDelaySamples(ctx.transport.bpm));
        feedback_.setTarget(get(kFeedback) * 0.01f);
        mix_.setTarget(get(kMix) * 0.01f);
        output_.setTarget(dbToGain(get(kOutput)));
        logCutoff_.setTarget(std::log2(get(kCutoff)));

        const bool pingPong = getBool(kPingPong);
        const float wobble = get(kWobble) * 0.01f;
        const float wowDepth = wobble * 0.0040f * sr_;        // up to 4 ms of slow drift
        const float flutterDepth = wobble * 0.0006f * sr_;    // up to 0.6 ms of fast flutter
        const float wowInc = 0.5f / sr_, flutterInc = 6.1f / sr_;

        const int filterMode = getInt(kFilterMode);
        const float reso = get(kResonance) * 0.01f;
        // Loop gain staging: the filter's resonant peak is held to (1 + 0.5 x Resonance), so with Feedback below
        // 100% the repeats die away unless Resonance is high (at full Resonance they take off from ~67%).
        const float k = 2.0f - 1.98f * reso;                  // same damping the SDK's SVF uses (peak gain 1/k)
        const float peakTarget = 1.0f + 0.5f * reso;
        const float lpHpGain = std::fmin(1.0f, peakTarget * k);
        const float bpGain = peakTarget * k;                  // BP's raw peak is also 1/k

        const float drive = get(kDrive) * 0.01f;
        const float driveGain = 1.0f + drive * drive * 9.0f;  // 1x .. 10x
        constexpr float kBias = 0.2f;                         // asymmetry: even harmonics, like a tube stage
        const float tb = std::tanh(kBias);
        const float driveNorm = 1.0f / (driveGain * (1.0f - tb * tb));   // unity gain for small signals

        const float limitDrive = dbToGain(get(kLimit) * 0.18f);          // 0..+18 dB

        FxSettings fx = fxSettings();
        if (fx.type != lastFxType_)       // switching effects: start the new one from silence
        {
            for (LoopChannel& c : ch_) c.clearFx();
            lastFxType_ = fx.type;
        }
        if (fx.type == kFxDownsample || fx.type == kFxFuzz)
            for (LoopChannel& c : ch_) c.tone.setCutoff(fx.toneHz, sr_);

        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        const float maxDelay = (float)ch_[0].delay.maxDelay() - 2.0f;

        // ----- per sample ---------------------------------------------------------------------
        for (int n = 0; n < ctx.numFrames; ++n)
        {
            const float dryL = inL[n], dryR = inR[n];

            // Filter coefficients follow the smoothed cutoff, refreshed every 16 samples.
            const float logCut = logCutoff_.process();
            if (coeffCounter_-- <= 0)
            {
                coeffCounter_ = kCoeffInterval - 1;
                const float hz = std::exp2(logCut);
                ch_[0].filter.set(hz, reso);
                ch_[1].filter.set(hz, reso);
                if (fx.type == kFxPhaser) updatePhaser(fx);
            }

            // Delay time: glide + wow and flutter (right channel slightly out of phase for width).
            const float base = delayTime_.process();
            wowPhase_ = advance(wowPhase_, wowInc);
            flutterPhase_ = advance(flutterPhase_, flutterInc);
            const float modL = wowDepth * sine01(wowPhase_) + flutterDepth * sine01(flutterPhase_);
            const float modR = wowDepth * sine01(advance(wowPhase_, 0.21f))
                             + flutterDepth * sine01(advance(flutterPhase_, 0.37f));
            const float dL = clamp(base + modL, 2.0f, maxDelay);
            const float dR = clamp(base + modR, 2.0f, maxDelay);

            // Modulation sources for the FX slot.
            fxLfoPhase_ = advance(fxLfoPhase_, fx.lfoInc);
            ringPhase_ = advance(ringPhase_, fx.ringInc);
            const float lfoL = sine01(fxLfoPhase_);
            const float lfoR = sine01(advance(fxLfoPhase_, 0.25f));   // 90 degrees apart
            const float ring = sine01(ringPhase_);

            // The loop: read -> filter -> FX -> drive -> DC block.
            float wet[2];
            const float lfo[2] = { lfoL, lfoR };
            const float d[2] = { dL, dR };
            for (int c = 0; c < 2; ++c)
            {
                LoopChannel& ch = ch_[c];
                float x = ch.delay.readLinear(d[c]);

                switch (filterMode)
                {
                    case kFilterLP: x = ch.filter.process(x, SVF::Mode::Lowpass) * lpHpGain; break;
                    case kFilterBP: x = ch.filter.process(x, SVF::Mode::Bandpass) * bpGain; break;
                    case kFilterHP: x = ch.filter.process(x, SVF::Mode::Highpass) * lpHpGain; break;
                    default: break;
                }

                x = processFx(ch, x, fx, lfo[c], ring);

                x = (fastTanh(driveGain * x + kBias) - tb) * driveNorm;
                x = ch.dc.process(x);
                ch.loopOut = x;
                wet[c] = x;
            }

            // Feed the repeats back.
            const float fb = feedback_.process();
            if (pingPong)
            {
                ch_[0].delay.write(0.5f * (dryL + dryR) + ch_[1].loopOut * fb);
                ch_[1].delay.write(ch_[0].loopOut * fb);
            }
            else
            {
                ch_[0].delay.write(dryL + ch_[0].loopOut * fb);
                ch_[1].delay.write(dryR + ch_[1].loopOut * fb);
            }

            // Wet limiter: stereo-linked, fast attack, then a soft knee for whatever gets past it.
            float wL = wet[0] * limitDrive, wR = wet[1] * limitDrive;
            const float peak = std::fmax(std::fabs(wL), std::fabs(wR));
            const float target = peak > kWetCeiling ? kWetCeiling / peak : 1.0f;
            limGain_ += (target < limGain_ ? limAttack_ : limRelease_) * (target - limGain_);
            wL = softKnee(wL * limGain_, 0.8f * kWetCeiling, kWetCeiling);
            wR = softKnee(wR * limGain_, 0.8f * kWetCeiling, kWetCeiling);

            // Mix: dry stays at full level up to 50%, wet reaches full level at 50%.
            const float mix = mix_.process();
            const float dryGain = std::fmin(1.0f, 2.0f * (1.0f - mix));
            const float wetGain = std::fmin(1.0f, 2.0f * mix);
            const float gain = output_.process();
            outL[n] = softKnee((dryL * dryGain + wL * wetGain) * gain, kOutKnee, kOutCeiling);
            outR[n] = softKnee((dryR * dryGain + wR * wetGain) * gain, kOutKnee, kOutCeiling);
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Undertow";
        info.vendor       = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;            // 1.0.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 0;               // unknown: depends on Feedback (endless above 100%)
        return info;
    }

    static constexpr char kUid[5] = "GGxC";

    float targetDelaySamples(double bpm) const
    {
        const float seconds = getInt(kSync) == 1 ? beatsToSeconds(divisionBeats(getInt(kDivision)), bpm)
                                                 : get(kTime) * 0.001f;
        return clamp(seconds, 0.001f, kMaxDelaySeconds) * sr_;
    }

    // Turn FX Amount / FX Rate into the numbers each effect needs (once per block).
    FxSettings fxSettings() const
    {
        FxSettings s;
        s.type = getInt(kFxType);
        s.amount = get(kFxAmount) * 0.01f;
        s.rate = get(kFxRate) * 0.01f;
        s.piOverSr = 3.14159265f / sr_;
        const float a = s.amount, r = s.rate;
        switch (s.type)
        {
            case kFxChorus:    // Amount = depth, Rate = speed 0.05..3 Hz
                s.lfoInc = (0.05f + r * r * 2.95f) / sr_;
                s.chorusCentre = 0.012f * sr_;
                s.chorusDepth = (0.0005f + a * 0.005f) * sr_;
                s.chorusMix = clamp(a * 4.0f, 0.0f, 1.0f) * 0.5f;
                break;
            case kFxFlanger:   // Amount = depth and resonance, Rate = speed 0.03..2 Hz
                s.lfoInc = (0.03f + r * r * 1.97f) / sr_;
                s.flangeMin = 0.0002f * sr_;
                s.flangeRange = (0.0005f + a * 0.0035f) * sr_;
                s.flangeFb = 0.3f + a * 0.55f;
                s.flangeMix = clamp(a * 4.0f, 0.0f, 1.0f) * 0.7f;
                s.flangeComp = std::sqrt(1.0f - s.flangeFb);   // tames the comb peaks, keeps repeats decaying like plain ones
                break;
            case kFxPhaser:    // Amount = depth and feedback, Rate = speed 0.03..4 Hz
                s.lfoInc = (0.03f + r * r * 3.97f) / sr_;
                s.phaserLowHz = 200.0f;
                s.phaserOctaves = 0.5f + a * 3.5f;
                s.phaserFb = 0.2f + a * 0.5f;
                s.phaserMix = clamp(a * 4.0f, 0.0f, 1.0f) * 0.5f;
                s.phaserComp = std::sqrt(1.0f - s.phaserFb * s.phaserFb);   // keeps the average level at unity
                break;
            case kFxBitcrush:  // Amount = bits 16..2, Rate = sample rate reduction 1x..8x
                s.quantSteps = std::exp2(15.0f - a * 14.0f);
                s.holdInc = 1.0f / (1.0f + r * r * 7.0f);
                break;
            case kFxDownsample:// Amount = reduction 1x..48x, Rate = smoothing (0 = raw aliasing, 100 = muffled)
                s.holdInc = 1.0f / (1.0f + a * a * 47.0f);
                s.toneHz = 18000.0f * std::pow(0.08f, r);
                break;
            case kFxRingMod:   // Amount = mix, Rate = carrier 20..2000 Hz
                s.ringInc = 20.0f * std::pow(100.0f, r) / sr_;
                s.ringMix = a;
                break;
            case kFxFuzz:      // Amount = clipping 1..40x (level-compensated), Rate = tone 600 Hz..15 kHz
                s.fuzzGain = 1.0f + a * a * 39.0f;
                s.toneHz = 600.0f * std::pow(25.0f, r);
                break;
            default: break;
        }
        return s;
    }

    // Phaser sweep position for each channel; tan() is approximated by its argument (fine below ~4 kHz).
    void updatePhaser(const FxSettings& s)
    {
        for (int c = 0; c < 2; ++c)
        {
            const float lfo = sine01(c == 0 ? fxLfoPhase_ : advance(fxLfoPhase_, 0.25f));
            const float hz = s.phaserLowHz * std::exp2(s.phaserOctaves * (0.5f + 0.5f * lfo));
            const float w = hz * s.piOverSr;
            ch_[c].phaserCoef = (w - 1.0f) / (w + 1.0f);
        }
    }

    float processFx(LoopChannel& ch, float x, const FxSettings& s, float lfo, float ring) const
    {
        switch (s.type)
        {
            case kFxChorus:
            {
                ch.fxDelay.write(x);
                const float v = ch.fxDelay.readLinear(s.chorusCentre + s.chorusDepth * lfo);
                return x + (v - x) * s.chorusMix;
            }
            case kFxFlanger:
            {
                const float v = ch.fxDelay.readLinear(s.flangeMin + s.flangeRange * (0.5f + 0.5f * lfo));
                ch.fxDelay.write(x + v * s.flangeFb);
                return x + (v * s.flangeComp - x) * s.flangeMix;
            }
            case kFxPhaser:
            {
                // Four first-order allpasses swept together (coefficient from updatePhaser()).
                const float coef = ch.phaserCoef;
                float y = x + ch.phaserFb * s.phaserFb;
                for (float& st : ch.apState)
                {
                    const float out = coef * y + st;
                    st = y - coef * out;
                    y = out;
                }
                ch.phaserFb = y;
                return x + (y * s.phaserComp - x) * s.phaserMix;
            }
            case kFxBitcrush:
            {
                ch.holdPhase += s.holdInc;
                if (ch.holdPhase >= 1.0f) { ch.holdPhase -= 1.0f; ch.held = std::round(x * s.quantSteps) / s.quantSteps; }
                return ch.held;
            }
            case kFxDownsample:
            {
                ch.holdPhase += s.holdInc;
                if (ch.holdPhase >= 1.0f) { ch.holdPhase -= 1.0f; ch.held = x; }
                return ch.tone.process(ch.held);
            }
            case kFxRingMod:
                return x + (x * ring - x) * s.ringMix;
            case kFxFuzz:
            {
                const float y = fastTanh(x * s.fuzzGain) / s.fuzzGain;   // unity for quiet signals, so repeats still decay
                return ch.tone.process(y);
            }
            default:
                return x;
        }
    }

    LoopChannel ch_[2];
    float sr_ = 44100.0f;
    Smoother delayTime_, feedback_, mix_, output_, logCutoff_;
    float limGain_ = 1.0f, limAttack_ = 0.1f, limRelease_ = 0.001f;
    float wowPhase_ = 0.0f, flutterPhase_ = 0.3f, fxLfoPhase_ = 0.0f, ringPhase_ = 0.0f;
    int coeffCounter_ = 0;
    int lastFxType_ = 0;
};

MPC_PLUGIN_ENTRY(Undertow)
