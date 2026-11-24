// Urchin - a spiky fuzz and distortion box with real character. Groundswell, MIT.
//
// Six Spines, each its own little circuit rather than another curve:
//   Gated Fuzz    a starved transistor fuzz: asymmetric hard clipping, a crossover dead zone that
//                 widens as the "battery" dies (Bias = Starve), and a bias point that chokes after
//                 loud notes, so decays sputter and gate.
//   Octave Fuzz   fuzz into a full-wave rectifier through a coupling cap into a second clipper: the
//                 rectifier doubles the frequency, so high notes ring an octave up (Bias = how much).
//   Torn Speaker  an amp into a speaker with a torn cone: the cone excursion (a low-passed copy of
//                 the signal) bottoms out sooner on one side, and past that limit the torn flap
//                 slaps and buzzes (Bias = how torn). Level dependent: quiet passes clean.
//   Rectifier     rectify, then fold: half-wave at Bias 0, full-wave at 50 (the fundamental all
//                 but vanishes: octave grit); Drive folds the rectified wave back on itself at hard
//                 corners (more folds, more grit), and past Bias 50 the folds shift for more spit.
//   Wavefolder    a West Coast style folder: past full scale the wave folds back on itself, again
//                 and again, for synthy spikes (Bias = symmetry offset, even harmonics).
//   Overdrive     a smooth, mid-focused soft clipper with a clean path beside it: the sane one
//                 (Bias = asymmetry).
//
// Signal flow (per sample unless noted):
//
//   in L,R ─┬──────────────────────────────── dry delay (23) ───────────────────────────┐
//           │                                                                         │
//           ├─> Low Keep split (Linkwitz-Riley 4th order) ── low band, clean ── delay ─┐│
//           │                                                                         ││
//           └─> high band ─> [Mono: L+R] ─> Spine coupling high-pass                  ││
//                  -> Focus pre-emphasis (Mids / Lows)                                 ││
//                  -> Drive x Bloom sag (playing hard pulls the drive down)            ││
//                  -> 2x up ─> Spine shaper(s), every one with ADAA ─> 2x down         ││
//                  -> DC block -> Focus de-emphasis -> Spine voicing (low-pass, peak)  ││
//                  -> Tone (tilt) -> auto level (instant curve + slow loudness match)  ││
//                  -> Level -> Gate                                                    ││
//                  -> + clean low band ───────────────────────────── wet ──┐           ││
//                                                                            v           v
//                                         Blend: dry x min(1, 2(1-b)) + wet x min(1, 2b)
//                                                                            │
//                                                                         Output -> L, R
//
// Split: set Spine R to a circuit and the (mono-summed) input runs through Spine on the left and
// Spine R on the right, like two amps panned apart; Width brings them back towards the middle.
//
// Spike is the performance macro: one Q-Link that pushes Drive, Bias, Bloom and the sputter up and
// tilts the tone brighter, from as-set (0) to ruined (100). The auto level follows it, so it gets
// nastier rather than just louder.
//
// Anti-aliasing: every memoryless shaper is a piecewise polynomial with a closed-form
// antiderivative, run as a first-order ADAA stage (UrchinParts.h) at 2x oversampling. No
// per-sample transcendental functions: exp/pow/log run per block for coefficients only.

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/math.h"

#include "UrchinParts.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace mpc::dsp;

// Parameter order is permanent: append new ones at the end.
enum ParamIndex
{
    kOutput = 0,   // dB after everything. Parameter 0: the host nudges it on load, so it does nothing else
    kSpine,        // which distortion circuit
    kDrive,        // % how hard the Spine is driven (each Spine has its own range)
    kSpike,        // % performance macro: as set -> ruined
    kTone,         // -100..+100 tilt EQ around 800 Hz: dark <-> bright (after the shaper)
    kFocus,        // Flat / Mids / Lows: pre-emphasis, so that band clips first (de-emphasised after)
    kBias,         // % the Spine's own character control (Starve, Octave, Tear, Rectify, Symmetry, Asymmetry)
    kBloom,        // % sag: playing hard compresses and spits
    kGate,         // % noise gate on the distortion; past 50 it turns into a chopper
    kBlend,        // % 0 dry, 50 full dry + full fuzz (parallel), 100 fuzz only
    kLowKeep,      // Hz crossover: below it the sound stays clean (20 = Off)
    kLevel,        // dB the distortion's own level (after the automatic level match)
    kStereo,       // Stereo (two cores) or Mono (one core on L+R, half the CPU)
    kSpineR,       // Same, or a second Spine for the right side: Split mode (L+R into two circuits, one per side)
    kWidth,        // % width of the distorted signal: 0 = both sides in the middle, 100 = as is
    kNumParams
};

enum Spine { kGated = 0, kOctave, kSpeaker, kRectifier, kFolder, kOverdrive, kNumSpines };
enum Focus { kFlat = 0, kMids, kLows };

const char* const kSpineNames[kNumSpines] = {"Gated Fuzz", "Octave Fuzz", "Torn Speaker", "Rectifier", "Wavefolder", "Overdrive"};

// Per-Spine voicing: the drive range, the coupling high-pass before it, and the filters after it
// that stand in for the rest of the circuit (tone caps, speaker, output stage).
struct SpineDef
{
    float driveLoDb, driveHiDb;   // gain into the shaper at Drive 0 and 100
    float spikeDb;                // extra drive at Spike 100
    float preHpHz;                // coupling cap before the shaper
    float postLpHz, postLpQ;      // output low-pass
    float peakHz, peakDb, peakQ;  // a resonance (0 dB = none)
    float postHpHz;               // DC block / output coupling
    float trimDb;                 // level trim (after the level match) so the Spines sit near each other
};

const SpineDef kSpines[kNumSpines] = {
    // drive lo/hi  spike  preHp  postLp  Q      peak             postHp trim
    {  6.0f, 48.0f, 16.0f,  90.0f, 4800.0f, 0.60f,    0.0f, 0.0f, 0.7f, 10.0f,  1.5f },   // Gated Fuzz
    {  0.0f, 34.0f, 14.0f, 160.0f, 7000.0f, 0.70f, 1200.0f, 3.0f, 0.8f, 20.0f,  2.0f },   // Octave Fuzz (drive after the rectifier)
    {  0.0f, 34.0f, 14.0f,  60.0f, 4500.0f, 0.90f, 1700.0f, 6.0f, 1.4f, 85.0f,  0.5f },   // Torn Speaker
    {  0.0f, 22.0f,  8.0f,  40.0f, 9000.0f, 0.70f,    0.0f, 0.0f, 0.7f, 15.0f,  0.5f },   // Rectifier (drive = folds)
    {  6.0f, 30.0f, 10.0f,  25.0f, 12000.0f, 0.70f,   0.0f, 0.0f, 0.7f, 15.0f,  0.0f },   // Wavefolder
    {  0.0f, 36.0f, 18.0f, 250.0f, 6500.0f, 0.70f,    0.0f, 0.0f, 0.7f, 15.0f,  3.5f },   // Overdrive
};

constexpr float kOdClean = 0.35f;        // Overdrive: the clean path beside the clipper
constexpr float kOctFront = 2.0f;        // Octave Fuzz: fixed gain into the first (soft) stage
constexpr float kFoldLpHz = 2500.0f;     // Rectifier, Wavefolder: one-pole low-pass in front of the fold
constexpr int kDynEvery = 16;            // Bloom/sputter shaper parameters update every 16 samples
constexpr float kRefAmp = 0.25f;         // the auto level is matched for a sine at this peak (-12 dBFS)
constexpr float kLowKeepOff = 20.5f;     // Low Keep at (or below) this is Off
constexpr int kLatency = ur::kOversampleLatency;

struct Glide
{
    float v = 0.0f;
    float step(float target, float c) { v += c * (target - v); return v; }
};

inline float coeffMs(float ms, float sr) { return ms <= 0.0f ? 1.0f : 1.0f - std::exp(-1.0f / (ms * 0.001f * sr)); }

// Gate knob mapping (shared by the DSP and the display).
inline bool gateOff(float g) { return g < 0.5f; }                  // percent
inline float gateAbsDb(float g) { return g <= 50.0f ? -80.0f + 40.0f * g / 50.0f : -40.0f; }
inline float gateChop(float g) { return g <= 50.0f ? 0.0f : (g - 50.0f) / 50.0f; }

} // namespace

class Urchin : public mpc::Plugin
{
public:
    Urchin() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("output",   "Output",   -24.0f, 12.0f, 0.0f, "", 1));     // "dB" from formatValue
        addParam(mpc::choiceParam("spine",    "Spine",    {kSpineNames[0], kSpineNames[1], kSpineNames[2],
                                                           kSpineNames[3], kSpineNames[4], kSpineNames[5]}, kGated));
        addParam(mpc::floatParam ("drive",    "Drive",    0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam ("spike",    "Spike",    0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam ("tone",     "Tone",     -100.0f, 100.0f, 0.0f, "", 0));    // text from formatValue
        addParam(mpc::choiceParam("focus",    "Focus",    {"Flat", "Mids", "Lows"}, kFlat));
        addParam(mpc::floatParam ("bias",     "Bias",     0.0f, 100.0f, 25.0f, "", 0));      // text from formatValue
        addParam(mpc::floatParam ("bloom",    "Bloom",    0.0f, 100.0f, 25.0f, "%", 0));
        addParam(mpc::floatParam ("gate",     "Gate",     0.0f, 100.0f, 0.0f, "", 0));       // text from formatValue
        addParam(mpc::floatParam ("blend",    "Blend",    0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::logParam   ("low_keep", "Low Keep", 20.0f, 800.0f, 20.0f, "", 0));     // text from formatValue
        addParam(mpc::floatParam ("level",    "Level",    -24.0f, 12.0f, 0.0f, "", 1));      // "dB" from formatValue
        addParam(mpc::choiceParam("stereo",   "Stereo",   {"Stereo", "Mono"}, 0));
        addParam(mpc::choiceParam("spine_r",  "Spine R",  {"Same", kSpineNames[0], kSpineNames[1], kSpineNames[2],
                                                           kSpineNames[3], kSpineNames[4], kSpineNames[5]}, 0));
        addParam(mpc::floatParam ("width",    "Width",    0.0f, 100.0f, 100.0f, "%", 0));

        // Spine R values: 0 = Same, 1 + Spine = that Spine on the right.
        //                                Out  Spine       Drive Spike Tone Focus  Bias Bloom Gate Blend LowK  Level St SpineR          Width
        addProgram("Drum Smasher",      { 0.0f, kGated,     70,   0,   15, kMids,  15,  50,   0,   55,   20,  0.0f, 0, 0,              100 });
        addProgram("Bass Grind",        { 0.0f, kRectifier, 60,   0,  -10, kMids,  20,  30,   0,  100,  130,  0.0f, 1, 0,              100 });
        addProgram("Dying Battery",     { 0.0f, kGated,     75,   0,  -20, kFlat,  75,  40,  25,  100,   20,  0.0f, 1, 0,              100 });
        addProgram("Octave Spit",       { 0.0f, kOctave,    65,   0,   10, kMids,  80,  30,  15,  100,   20,  0.0f, 1, 0,              100 });
        addProgram("Torn Speaker",      { 0.0f, kSpeaker,   55,   0,    0, kFlat,  60,  30,   0,  100,   20,  0.0f, 0, 0,              100 });
        addProgram("Folded Synth",      { 0.0f, kFolder,    55,   0,    5, kFlat,  20,  10,   0,  100,   20,  0.0f, 0, 0,              100 });
        addProgram("Gentle Hair",       { 0.0f, kOverdrive, 25,   0,    0, kMids,  15,  15,   0,   70,   20,  0.0f, 0, 0,              100 });
        addProgram("Total Urchin",      { 0.0f, kRectifier, 85,  80,   20, kMids,  90,  60,  60,  100,   20,  0.0f, 0, 0,              100 });
        addProgram("Twin Amps",         { 0.0f, kOverdrive, 55,   0,    0, kMids,  20,  25,   0,  100,   20,  0.0f, 0, 1 + kGated,     100 });
        addProgram("Split Spit",        { 0.0f, kOctave,    55,   0,    5, kMids,  70,  25,  10,  100,   20,  0.0f, 0, 1 + kSpeaker,    80 });
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = (float)sampleRate;
        sr2_ = 2.0f * sr_;
        hb_.design();
        for (int j = 0; j < kStaticPts; ++j) sinTab_[j] = std::sin(6.2831853f * (float)j / (float)kStaticPts);
        glide_ = coeffMs(12.0f, sr_);
        fadeStep_ = 1.0f / (0.004f * sr_);
        settleN_ = (int)(0.003f * sr_);
        envFastA_ = coeffMs(2.0f, sr_);   envFastR_ = coeffMs(120.0f, sr_);
        envSlowA_ = coeffMs(10.0f, sr_);  envSlowR_ = coeffMs(350.0f, sr_);
        gateEnvA_ = coeffMs(0.3f, sr_);   gateEnvR_ = coeffMs(15.0f, sr_);
        gateOpen_ = coeffMs(0.2f, sr_);
        for (int c = 0; c < 2; ++c)
        {
            circ_[c].matchHpIn.set(100.0f, sr_); circ_[c].matchHpOut.set(100.0f, sr_);
            core_[c].coneLp.set(120.0f, sr2_);
            core_[c].coneHp.set(1200.0f, sr2_);
            core_[c].couple.set(60.0f, sr2_);
            core_[c].tiltLp.set(800.0f, sr_);
            core_[c].foldLp.set(kFoldLpHz, sr_);
            core_[c].foldLp2.set(kFoldLpHz, sr_);
            core_[c].noise.s = 0x9E3779B9u + 7u * (uint32_t)c;
            circ_[c].lastCompKey = -1.0f;
        }
        lastFocus_ = -1; lastLowKeep_ = -1.0f;
        reset();
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            core_[c].reset();
            lkLp1_[c].reset(); lkLp2_[c].reset(); lkHp1_[c].reset(); lkHp2_[c].reset();
            lowDelay_[c].reset(); dryDelay_[c].reset();
            Circuit& k = circ_[c];
            k.fade = 1.0f; k.settle = 0;
            k.matchHpIn.reset(); k.matchHpOut.reset();
            k.measIn = k.measOut = k.msIn = k.msOut = 0.0f;
            k.corrDb = 0.0f; k.corrT = 1.0f;
        }
        envFast_ = envSlow_ = 0.0f;
        gateEnv_ = gatePeak_ = 0.0f; gateGain_ = 1.0f; gateIsOpen_ = true; gateHold_ = 0;
        dynCountdown_ = 0; dyn_ = Dyn();

        snapControls_ = true;
        updateControls();
        snapControls_ = false;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (n <= 0 || ctx.numOutputs < 1) return;

        const float* inL = (ctx.inputs && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* inR = (ctx.inputs && ctx.numInputs > 1) ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        updateControls();
        const bool mono = mono_, split = split_;
        const int nCirc = mono ? 1 : 2;
        const bool lk = lowKeepOn_;
        const bool gateOn = gateOn_;
        const float bloom = bloomEff_;

        for (int i = 0; i < n; ++i)
        {
            const float l = inL ? inL[i] : 0.0f;
            const float r = inR ? inR[i] : l;

            // ---- Low Keep: a clean low band and a high band for the Spine ----
            float hl = l, hr = r, ll = 0.0f, lr = 0.0f;
            if (lk)
            {
                ll = lkLp2_[0].process(lkLp1_[0].process(l));
                lr = lkLp2_[1].process(lkLp1_[1].process(r));
                hl = lkHp2_[0].process(lkHp1_[0].process(l));
                hr = lkHp2_[1].process(lkHp1_[1].process(r));
            }

            // ---- Bloom: how hard you are playing (relative to -10 dBFS peaks) ----
            const float pk = std::max(std::fabs(l), std::fabs(r));
            envFast_ += (pk > envFast_ ? envFastA_ : envFastR_) * (pk - envFast_);
            envSlow_ += (pk > envSlow_ ? envSlowA_ : envSlowR_) * (pk - envSlow_);
            const float eF = std::min(envFast_ * (1.0f / 0.3f), 2.5f);
            const float eS = std::min(envSlow_ * (1.0f / 0.3f), 2.5f);
            const float sag = 1.0f / (1.0f + 1.2f * bloom * eF);          // compresses
            if (--dynCountdown_ <= 0)
            {
                dynCountdown_ = kDynEvery;
                dyn_.spit = spitBase_ + 0.006f * bloom * eS;               // ...and spits: a wider dead zone (input units)
                dyn_.lean = 0.25f * bloom * eS;                            // and a bias that leans after loud notes
                dyn_.eS = eS;                                              // (a starved fuzz leans further, per circuit)
                ++ver_;                                                    // the shapers' curves moved
            }

            // ---- Gate (listens to the input, a little ahead of the wet path) ----
            float gg = 1.0f;
            if (gateOn) gg = gateStep(pk);

            // ---- the circuits: one per side (Stereo, Split) or one for both (Mono) ----
            const float xm = 0.5f * (hl + hr);
            float w[2];
            for (int c = 0; c < nCirc; ++c)
            {
                Circuit& k = circ_[c];
                switchStep(c);
                const float drive = k.driveG.step(k.driveT, glide_) * sag;
                const float x = (mono || split) ? xm : (c == 0 ? hl : hr);
                float y = runCore(core_[c], k, x, drive, dyn_);
                // Auto level: the static curve at once, a slow loudness match on top.
                const float cg = k.compG.step(k.compT, glide_) * k.corrG.step(k.corrT, glide_);
                y *= cg;
                const float mi = k.matchHpIn.process(core_[c].measIn);
                const float mo = k.matchHpOut.process(core_[c].measOut * cg);
                k.measIn += mi * mi; k.measOut += mo * mo;
                w[c] = y * k.levelG.step(k.levelT, glide_) * k.fade;
            }
            float wl = w[0], wr = mono ? w[0] : w[1];

            // ---- Width (on the distortion only), Gate, the clean low band ----
            const float wd = widthG_.step(widthT_, glide_);
            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * wd;
            wl = (mid + side) * gg; wr = (mid - side) * gg;
            if (lk) { wl += lowDelay_[0].process(ll, kLatency); wr += lowDelay_[1].process(lr, kLatency); }

            // ---- Blend and Output ----
            const float dl = dryDelay_[0].process(l, kLatency);
            const float dr = dryDelay_[1].process(r, kLatency);
            const float dg = dryG_.step(dryGT_, glide_), wg = wetG_.step(wetGT_, glide_);
            const float og = outG_.step(outGT_, glide_);
            // Filter tails ring down to nothing in silence: snap the last 1e-20 to zero.
            const float yl = (dg * dl + wg * wl) * og, yr = (dg * dr + wg * wr) * og;
            outL[i] = std::fabs(yl) < 1.0e-20f ? 0.0f : yl;
            if (outR) outR[i] = std::fabs(yr) < 1.0e-20f ? 0.0f : yr;
        }

        for (int c = 0; c < 2; ++c)
        {
            matchLoudness(circ_[c], n);
            core_[c].flush();
            circ_[c].matchHpIn.flush(); circ_[c].matchHpOut.flush();
        }
        envFast_ = flushDenormal(envFast_); envSlow_ = flushDenormal(envSlow_);
        gateEnv_ = flushDenormal(gateEnv_); gatePeak_ = flushDenormal(gatePeak_);
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case kOutput:
            case kLevel:
                if (std::fabs(v) < 0.05f) out = "0.0 dB";
                else { std::snprintf(b, sizeof(b), "%+.1f dB", (double)v); out = b; }
                return true;
            case kTone:
                if (std::fabs(v) < 0.5f) out = "Flat";
                else { std::snprintf(b, sizeof(b), v < 0.0f ? "Dark %.0f" : "Bright %.0f", (double)std::fabs(v)); out = b; }
                return true;
            case kBias:
            {
                static const char* const kBiasNames[kNumSpines] = {"Starve", "Octave", "Tear", "Rectify", "Symm", "Asym"};
                const int sp = mpc::clamp(getInt(kSpine), 0, kNumSpines - 1);
                if (sp == kRectifier)
                    std::snprintf(b, sizeof(b), "%s %.0f", v < 25.0f ? "Half" : (v < 75.0f ? "Full" : "Fold"), (double)v);
                else
                    std::snprintf(b, sizeof(b), "%s %.0f", kBiasNames[sp], (double)v);
                out = b;
                return true;
            }
            case kGate:
                if (gateOff(v)) out = "Off";
                else if (v <= 50.0f) { std::snprintf(b, sizeof(b), "%.0f dB", (double)gateAbsDb(v)); out = b; }
                else { std::snprintf(b, sizeof(b), "Chop %.0f", (double)(gateChop(v) * 100.0f)); out = b; }
                return true;
            case kLowKeep:
                if (v <= kLowKeepOff) out = "Off";
                else { std::snprintf(b, sizeof(b), "%.0f Hz", (double)v); out = b; }
                return true;
            default: return false;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name           = "Urchin";
        info.vendor         = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId       = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version        = 1100;            // 1.1.0.0
        info.isInstrument   = false;
        info.numInputs      = 2;
        info.numOutputs     = 2;
        info.latencySamples = kLatency;
        info.tailSamples    = 1024;            // oversampler, filters and gate; no reverb-like tail
        return info;
    }
    static constexpr char kUid[5] = "GO0Z";

    // Per-sample dynamics shared by both sides.
    struct Dyn { float spit = 0.0f, lean = 0.0f, eS = 0.0f; };

    // One side's circuit: which Spine, its shaper settings for this block, its drive and its level.
    struct Circuit
    {
        int spine = kGated, pending = kGated;
        bool peakOn = false;
        ur::DzClip gated, pre, post, amp, od;
        ur::RectTri rtri;
        ur::Folder fold;
        float coneLp = 0.5f, coneLn = 0.4f, buzzAmt = 2.0f, rattleAmt = 0.0f;
        float starve = 0.0f, starveDz = 0.0f, inScale = 1.0f;
        float driveT = 1.0f, compT = 1.0f, levelT = 1.0f, lastCompKey = -1.0f;
        Glide driveG, compG, levelG, corrG;
        // loudness match
        ur::OnePoleHP matchHpIn, matchHpOut;
        float measIn = 0.0f, measOut = 0.0f, msIn = 0.0f, msOut = 0.0f, corrDb = 0.0f, corrT = 1.0f;
        // Spine switching
        float fade = 1.0f;
        int settle = 0;
    };

    // One side's signal path (filters and oversampler state).
    struct Core
    {
        ur::Upsampler2x up;
        ur::Downsampler2x down;
        ur::Adaa a1, a3;
        ur::OnePoleHP preHp, postHp, couple, coneHp;
        ur::OnePoleLP coneLp, tiltLp, foldLp, foldLp2;
        Biquad focusPre, focusPost, postLp, peak;
        ur::ShortDelay cleanDelay;
        ur::Lcg noise;
        float measIn = 0.0f, measOut = 0.0f;

        void resetSpine()
        {
            // The shared filters keep running (only their coefficients change); the circuit's own
            // state starts fresh.
            a1.reset(); a3.reset();
            couple.reset(); coneHp.reset(); coneLp.reset(); foldLp.reset(); foldLp2.reset();
        }
        void reset()
        {
            resetSpine();
            preHp.reset(); postHp.reset(); postLp.reset(); peak.reset();
            up.reset(); down.reset(); tiltLp.reset(); focusPre.reset(); focusPost.reset(); cleanDelay.reset();
        }
        void flush()
        {
            preHp.flush(); postHp.flush(); couple.flush(); coneHp.flush();
            coneLp.z = flushDenormal(coneLp.z); tiltLp.z = flushDenormal(tiltLp.z); foldLp.z = flushDenormal(foldLp.z); foldLp2.z = flushDenormal(foldLp2.z);
        }
    };

    // ---- Spine switch on one side: a 4 ms dip, 3 ms for the new circuit to settle, 4 ms back up ----
    void switchStep(int c)
    {
        Circuit& k = circ_[c];
        if (k.pending != k.spine)
        {
            k.fade -= fadeStep_;
            if (k.fade <= 0.0f)
            {
                // Silent now: swap the circuit, and set its drive and level at once.
                k.fade = 0.0f;
                k.spine = k.pending;
                core_[c].resetSpine();
                applySpineFilters(c);
                setupCircuit(k, blockCtl_);
                k.driveG.v = k.driveT; k.compG.v = k.compT; k.levelG.v = k.levelT;
                k.corrDb = 0.0f; k.corrT = k.corrG.v = 1.0f;
                k.settle = settleN_;
            }
        }
        else if (k.settle > 0) --k.settle;
        else if (k.fade < 1.0f) k.fade = std::min(1.0f, k.fade + fadeStep_);
    }

    // ---- one base-rate sample through one side's Spine ----
    float runCore(Core& c, const Circuit& k, float x, float drive, const Dyn& d)
    {
        const int sp = k.spine;
        float v = c.preHp.process(x);
        c.measIn = v;                                       // what the circuit is fed (for the level match)
        if (focusOn_) v = c.focusPre.process(v);
        // The folding Spines (and the octave fuzz, which doubles first) hear a slightly darker input:
        // a high note folds fewer times than a low one, which keeps the folds from spraying aliases on
        // top notes (the fold puts the top back). The Rectifier doubles before it folds: two poles.
        if (sp == kFolder || sp == kOctave) v = c.foldLp.process(v);
        else if (sp == kRectifier) v = c.foldLp2.process(c.foldLp.process(v));
        // The octave fuzz is driven after its rectifier: a fixed, sag-following gain goes in front.
        const float front = sp == kOctave ? kOctFront * drive / std::max(k.driveT, 1.0e-6f) : drive;
        float u0, u1;
        c.up.process(v * front, hb_, u0, u1);
        const float y0 = shape(c, k, u0, d, drive);
        const float y1 = shape(c, k, u1, d, drive);
        float y = c.down.process(y0, y1, hb_);
        if (sp == kOverdrive) y += kOdClean * c.cleanDelay.process(x, kLatency);
        y = c.postHp.process(y);
        if (focusOn_) y = c.focusPost.process(y);
        c.measOut = y;                                      // what the circuit gives back, before the voicing and Tone
        y = c.postLp.process(y);
        if (k.peakOn) y = c.peak.process(y);
        const float lo = c.tiltLp.process(y);
        return tiltLo_ * lo + tiltHi_ * (y - lo);
    }

    // ---- the shapers, at 2x ----
    float shape(Core& c, const Circuit& k, float u, const Dyn& d, float drive)
    {
        const uint32_t v = ver_;
        const float lean = d.lean + k.starve * 0.3f * d.eS;
        switch (k.spine)
        {
            case kGated:
            {
                // Starved fuzz: the bias leans further after loud notes (the coupling cap charges),
                // which widens the gap and chokes the decay into sputter.
                // Gap and lean are input-referred (times inScale), so they bite at any Drive.
                ur::DzClip s = k.gated;
                s.b -= 0.03 * lean * k.inScale;
                s.dz += (d.spit + 0.4 * lean * k.starveDz) * k.inScale;
                return c.a1.process(s, u, v);
            }
            case kOctave:
            {
                ur::DzClip s1 = k.pre;                         // soft stage, rectified (one ADAA stage)
                s1.dz += d.spit * k.inScale;
                const float r = c.a1.process(s1, u, v);
                const float cc = c.couple.process(r);          // coupling cap: the rectified DC drains away
                ur::DzClip s3 = k.post;
                s3.b = 0.15 * lean;
                return c.a3.process(s3, (double)cc * drive, v);
            }
            case kSpeaker:
            {
                ur::DzClip s = k.amp;
                s.dz += d.spit * k.inScale;
                s.b += 0.2 * lean;
                const float a = c.a1.process(s, u, v);
                // The cone: excursion follows the lows. Past its limits (nearer on the negative
                // side) it bottoms out, and the torn flap slaps: a buzz gated by how far past it is.
                const float e = c.coneLp.process(a);
                const float ex = e > k.coneLp ? e - k.coneLp : (e < -k.coneLn ? e + k.coneLn : 0.0f);
                const float hp = c.coneHp.process(a);
                const float bz = softClip(clamp(hp * 6.0f, -1.5f, 1.5f));
                const float aex = std::fabs(ex);
                const float y = a - 0.85f * ex + k.buzzAmt * aex * bz + k.rattleAmt * aex * c.noise.next();
                return 1.25f * softClip(y * 0.8f);              // the cone can only move so far
            }
            case kRectifier:
            {
                ur::RectTri r = k.rtri;
                r.phi += 0.3 * lean;
                return c.a1.process(r, u, v);
            }
            case kFolder:
            {
                ur::Folder f = k.fold;
                f.off += 0.3 * lean;
                return c.a1.process(f, u, v);
            }
            default: // kOverdrive
            {
                ur::DzClip s = k.od;
                s.dz += d.spit * k.inScale;
                s.b += 0.15 * lean;
                return c.a1.process(s, u, v);
            }
        }
    }

    // The same circuits without state, filters or dynamics: what the auto level measures (and the
    // test's naive reference). For the octave fuzz, `dc` is the rectified wave's mean, which the
    // coupling cap removes before the second stage; `stage1` returns the rectified wave itself.
    static float shapeStatic(const Circuit& k, float x, float drive, float dc = 0.0f, bool stage1 = false)
    {
        const double u = (double)x * drive;
        switch (k.spine)
        {
            case kGated: return (float)k.gated.f(u);
            case kOctave:
            {
                const double r = k.pre.f((double)x * kOctFront);
                if (stage1) return (float)r;
                return (float)k.post.f((r - dc) * drive);
            }
            case kSpeaker: return (float)k.amp.f(u);
            case kRectifier: return (float)k.rtri.f(u);
            case kFolder: return (float)k.fold.f(u);
            default: return (float)(k.od.f(u) + kOdClean * x);
        }
    }

    // ---- automatic level: match a -12 dBFS sine's RMS through the static curve (per block, only
    // when something that changes it moved) ----
    void computeComp(Circuit& k)
    {
        float ys[kStaticPts];
        float dc = 0.0f;
        if (k.spine == kOctave)
        {
            double m = 0.0;
            for (int j = 0; j < kStaticPts; ++j) m += shapeStatic(k, kRefAmp * sinTab_[j], k.driveT, 0.0f, true);
            dc = (float)(m / kStaticPts);
        }
        double s = 0.0, s2 = 0.0;
        for (int j = 0; j < kStaticPts; ++j) { ys[j] = shapeStatic(k, kRefAmp * sinTab_[j], k.driveT, dc); s += ys[j]; }
        const double mean = s / kStaticPts;
        for (int j = 0; j < kStaticPts; ++j) s2 += (ys[j] - mean) * (ys[j] - mean);
        const double rms = std::sqrt(s2 / kStaticPts);
        const float target = kRefAmp * 0.70710678f;
        k.compT = clamp((float)(target / std::max(rms, 1.0e-5)), dbToGain(-40.0f), dbToGain(12.0f));
    }

    // ---- slow loudness match: keeps the circuit's output about as loud as what it is fed (measured
    // around the shaper, so the voicing filters and Tone still change the level as they should), so
    // Drive and Spike add dirt and sustain rather than volume (+-9 dB, about 1.5 s) ----
    void matchLoudness(Circuit& k, int n)
    {
        const float dt = (float)n / sr_;
        const float a = 1.0f - std::exp(-dt / 0.4f);
        k.msIn += a * (k.measIn / (float)n - k.msIn);
        k.msOut += a * (k.measOut / (float)n - k.msOut);
        k.measIn = k.measOut = 0.0f;
        k.msIn = flushDenormal(k.msIn); k.msOut = flushDenormal(k.msOut);
        if (k.msIn > 1.0e-6f && k.msOut > 1.0e-9f)
        {
            const float err = clamp(10.0f * std::log10(k.msIn / k.msOut), -6.0f, 6.0f);
            k.corrDb = clamp(k.corrDb + err * dt / 1.5f, -9.0f, 9.0f);
        }
        k.corrT = dbToGain(k.corrDb);
    }

    // ---- the gate: a peak follower against a threshold; past 50 % the threshold also follows the
    // recent peak, so every decay is cut short and fluctuating sounds chop ----
    float gateStep(float pk)
    {
        gateEnv_ += (pk > gateEnv_ ? gateEnvA_ : gateEnvR_) * (pk - gateEnv_);
        gatePeak_ = pk > gatePeak_ ? pk : gatePeak_ * gatePeakDecay_;
        const float thr = std::max(gateAbs_, gatePeak_ * gateRel_);
        if (gateEnv_ > thr) { gateIsOpen_ = true; gateHold_ = gateHoldN_; }
        else if (gateEnv_ < thr * gateHyst_)
        {
            if (gateHold_ > 0) --gateHold_;
            else gateIsOpen_ = false;
        }
        gateGain_ += (gateIsOpen_ ? gateOpen_ : gateClose_) * ((gateIsOpen_ ? 1.0f : 0.0f) - gateGain_);
        return gateGain_;
    }

    void applySpineFilters(int c)
    {
        Circuit& k = circ_[c];
        const SpineDef& sd = kSpines[k.spine];
        core_[c].preHp.set(sd.preHpHz, sr_);
        core_[c].postHp.set(sd.postHpHz, sr_);
        core_[c].postLp.setCoeffs(BiquadCoeffs::lowpass(sr_, std::min(sd.postLpHz, 0.45f * sr_), sd.postLpQ));
        if (sd.peakDb != 0.0f) core_[c].peak.setCoeffs(BiquadCoeffs::peak(sr_, sd.peakHz, sd.peakQ, sd.peakDb));
        k.peakOn = sd.peakDb != 0.0f;
        k.lastCompKey = -1.0f;
    }

    // The knob values a circuit is set up from (read once per block).
    struct BlockCtl { float spike = 0.0f, drive = 0.5f, biasE = 0.25f, level = 0.0f; };

    // ---- one side's circuit for this block: drive, shaper settings, auto level ----
    void setupCircuit(Circuit& k, const BlockCtl& b)
    {
        const SpineDef& sd = kSpines[k.spine];
        const float biasE = b.biasE;
        // A starved fuzz also has less gain: Starve takes up to 9 dB of the drive away.
        const float starveDb = k.spine == kGated ? 9.0f * biasE : 0.0f;
        const float driveDb = sd.driveLoDb + (sd.driveHiDb - sd.driveLoDb) * b.drive + sd.spikeDb * b.spike - starveDb;
        k.driveT = dbToGain(driveDb);
        k.inScale = k.spine == kOctave ? kOctFront : k.driveT;   // shaper units per input unit, first stage

        k.starve = 0.0f;
        switch (k.spine)
        {
            case kGated:
                // Starve, input-referred: a crossover gap that grows from -80 dBFS to 0.12 (-18 dBFS) at
                // full Starve, off-centre so the two halves of the wave open at different levels (splatty,
                // asymmetric), and a lower negative clip level. Notes break up as they decay and then gate.
                k.starve = biasE;
                k.starveDz = 0.12f * std::pow(biasE, 1.25f);
                k.gated.b = -0.6 * k.starveDz * k.driveT;
                k.gated.dz = (0.0001 + k.starveDz) * k.driveT;    // healthy: a -80 dBFS gap
                k.gated.set(1.0, 1.0 - 0.6 * biasE, 0.2);
                break;
            case kOctave:
                k.pre.b = 0.0; k.pre.dz = 0.0; k.pre.set(1.0, 1.0, 1.0);
                k.pre.rect = true; k.pre.w = 0.35 + 0.65 * biasE;
                k.post.b = 0.0; k.post.dz = 0.0; k.post.set(1.0, 1.0, 0.6);
                break;
            case kSpeaker:
                k.amp.b = 0.15 * biasE; k.amp.dz = 0.0; k.amp.set(1.0, 0.75, 0.9);
                k.coneLp = 0.75f - 0.3f * biasE;
                k.coneLn = 0.45f - 0.3f * biasE;
                k.buzzAmt = 1.5f + 2.0f * biasE;
                k.rattleAmt = 0.6f * biasE;
                break;
            case kRectifier:
                k.rtri.w = std::min(1.0f, 2.0f * biasE);
                k.rtri.phi = 0.5f * std::max(0.0f, 2.0f * biasE - 1.0f);   // up to a quarter period (a half only inverts)
                break;
            case kFolder:
                k.fold.off = 0.8 * biasE;
                break;
            default:
                k.od.b = 0.12 * biasE; k.od.dz = 0.0; k.od.set(1.0, 1.0 - 0.35 * biasE, 1.0);
                break;
        }

        // Auto level, only when the static curve changed.
        const float key = (float)k.spine * 1000.0f + driveDb * 7.0f + biasE * 31.0f + b.spike * 3.0f;
        if (key != k.lastCompKey) { computeComp(k); k.lastCompKey = key; }
        k.levelT = dbToGain(b.level + sd.trimDb);
    }

    // ---- per-block control updates ----
    void updateControls()
    {
        const int sp = mpc::clamp(getInt(kSpine), 0, kNumSpines - 1);
        const int spR = mpc::clamp(getInt(kSpineR), 0, (int)kNumSpines);
        const bool mono = getInt(kStereo) == 1;
        // Split (Spine R set) always runs two circuits on L+R, whatever Stereo says.
        const bool split = spR > 0;
        const int target[2] = {sp, split ? spR - 1 : sp};
        if ((mono && !split) != mono_ && !snapControls_) { core_[1].reset(); circ_[1].corrDb = 0.0f; circ_[1].corrT = circ_[1].corrG.v = 1.0f; }
        mono_ = mono && !split;
        split_ = split;

        const float spike = get(kSpike) * 0.01f;
        const float bias = get(kBias) * 0.01f;
        const float bloom = get(kBloom) * 0.01f;
        blockCtl_.spike = spike;
        blockCtl_.drive = get(kDrive) * 0.01f;
        blockCtl_.biasE = bias + (1.0f - bias) * 0.65f * spike;
        blockCtl_.level = get(kLevel);
        bloomEff_ = bloom + (1.0f - bloom) * 0.8f * spike;
        spitBase_ = 0.02f * spike * spike;                         // input units: up to -34 dBFS of dead zone

        for (int c = 0; c < 2; ++c)
        {
            Circuit& k = circ_[c];
            k.pending = target[c];
            // On a reset, or when the right side has been idle (Mono), take the new Spine at once.
            const bool idle = c == 1 && mono_;
            if (snapControls_ || idle) { k.spine = k.pending; applySpineFilters(c); k.fade = 1.0f; k.settle = 0; }
            setupCircuit(k, blockCtl_);
            if (snapControls_) { k.driveG.v = k.driveT; k.compG.v = k.compT; k.levelG.v = k.levelT; k.corrG.v = k.corrT; }
        }
        ++ver_;                                                    // block-rate shaper changes

        // Focus: pre-emphasis before the shaper, most of it taken back after.
        const int focus = getInt(kFocus);
        if (focus != lastFocus_)
        {
            for (int c = 0; c < 2; ++c)
            {
                if (focus == kMids)
                {
                    core_[c].focusPre.setCoeffs(BiquadCoeffs::peak(sr_, 1000.0f, 0.7f, 10.0f));
                    core_[c].focusPost.setCoeffs(BiquadCoeffs::peak(sr_, 1000.0f, 0.7f, -7.0f));
                }
                else if (focus == kLows)
                {
                    core_[c].focusPre.setCoeffs(BiquadCoeffs::lowShelf(sr_, 250.0f, 0.7f, 10.0f));
                    core_[c].focusPost.setCoeffs(BiquadCoeffs::lowShelf(sr_, 250.0f, 0.7f, -7.0f));
                }
                core_[c].focusPre.reset(); core_[c].focusPost.reset();
            }
            lastFocus_ = focus;
        }
        focusOn_ = focus != kFlat;

        // Tone: tilt around 800 Hz, +-9 dB at the ends; Spike leans it brighter.
        const float tilt = clamp(get(kTone) * 0.01f + 0.3f * spike, -1.0f, 1.0f);
        tiltHi_ = dbToGain(9.0f * tilt);
        tiltLo_ = dbToGain(-9.0f * tilt);

        // Low Keep: Linkwitz-Riley crossover (two Butterworth stages each side).
        const float lkHz = get(kLowKeep);
        const bool lkOn = lkHz > kLowKeepOff;
        if (lkOn && std::fabs(lkHz - lastLowKeep_) > 0.002f * lkHz)
        {
            const BiquadCoeffs lo = BiquadCoeffs::lowpass(sr_, lkHz, 0.70710678f);
            const BiquadCoeffs hi = BiquadCoeffs::highpass(sr_, lkHz, 0.70710678f);
            for (int c = 0; c < 2; ++c)
            {
                lkLp1_[c].setCoeffs(lo); lkLp2_[c].setCoeffs(lo);
                lkHp1_[c].setCoeffs(hi); lkHp2_[c].setCoeffs(hi);
            }
            lastLowKeep_ = lkHz;
        }
        if (lkOn && !lowKeepOn_)
            for (int c = 0; c < 2; ++c) { lkLp1_[c].reset(); lkLp2_[c].reset(); lkHp1_[c].reset(); lkHp2_[c].reset(); lowDelay_[c].reset(); }
        lowKeepOn_ = lkOn;

        // Gate.
        const float g = get(kGate);
        const bool gOn = !gateOff(g);
        if (gOn && !gateOn_) { gateIsOpen_ = true; gateGain_ = 1.0f; gateHold_ = 0; }
        gateOn_ = gOn;
        {
            const float chop = gateChop(g);
            gateAbs_ = dbToGain(gateAbsDb(g));
            gateRel_ = chop > 0.0f ? dbToGain(-36.0f + 30.0f * chop) : 0.0f;
            const float relMs = g <= 50.0f ? 120.0f - 80.0f * g / 50.0f : 40.0f * std::pow(0.08f, chop);
            gateClose_ = coeffMs(relMs, sr_);
            gateHoldN_ = (int)(0.025f * (1.0f - chop) * sr_);
            gateHyst_ = 0.5f + 0.35f * chop;
            gatePeakDecay_ = std::exp(-1.0f / ((1.5f - 1.2f * chop) * sr_));
        }

        // Blend: 0 dry, 50 full dry + full fuzz (parallel), 100 fuzz only.
        const float bl = get(kBlend) * 0.01f;
        dryGT_ = std::min(1.0f, 2.0f * (1.0f - bl));
        wetGT_ = std::min(1.0f, 2.0f * bl);
        widthT_ = get(kWidth) * 0.01f;

        outGT_ = dbToGain(get(kOutput));

        if (snapControls_) { dryG_.v = dryGT_; wetG_.v = wetGT_; outG_.v = outGT_; widthG_.v = widthT_; }
    }

    // ---- state ----
    static constexpr int kStaticPts = 64;
    float sr_ = 44100.0f, sr2_ = 88200.0f;
    ur::HalfbandTaps hb_;
    Core core_[2];
    Circuit circ_[2];
    BlockCtl blockCtl_;
    Biquad lkLp1_[2], lkLp2_[2], lkHp1_[2], lkHp2_[2];
    ur::ShortDelay lowDelay_[2], dryDelay_[2];
    float sinTab_[kStaticPts] = {};
    float bloomEff_ = 0.0f, spitBase_ = 0.0f;

    // controls
    bool snapControls_ = false, focusOn_ = false, lowKeepOn_ = false, gateOn_ = false, mono_ = false, split_ = false;
    int lastFocus_ = -1;
    float lastLowKeep_ = -1.0f;
    float dryGT_ = 0.0f, wetGT_ = 1.0f, outGT_ = 1.0f, widthT_ = 1.0f;
    float tiltLo_ = 1.0f, tiltHi_ = 1.0f;
    Glide dryG_, wetG_, outG_, widthG_;
    Dyn dyn_;
    int dynCountdown_ = 0;
    uint32_t ver_ = 0;
    float glide_ = 0.01f, fadeStep_ = 0.005f;
    int settleN_ = 132;

    // Bloom detector
    float envFast_ = 0.0f, envSlow_ = 0.0f, envFastA_ = 0.1f, envFastR_ = 0.001f, envSlowA_ = 0.01f, envSlowR_ = 0.0001f;

    // gate
    float gateEnv_ = 0.0f, gatePeak_ = 0.0f, gateGain_ = 1.0f;
    float gateEnvA_ = 0.1f, gateEnvR_ = 0.01f, gateOpen_ = 0.1f, gateClose_ = 0.001f;
    float gateAbs_ = 0.0f, gateRel_ = 0.0f, gateHyst_ = 0.5f, gatePeakDecay_ = 0.9999f;
    bool gateIsOpen_ = true;
    int gateHold_ = 0, gateHoldN_ = 0;
};

MPC_PLUGIN_ENTRY(Urchin)
