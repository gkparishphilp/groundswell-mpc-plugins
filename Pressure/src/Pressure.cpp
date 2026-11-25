// Pressure - a compressor and saturator with a lot of character. Groundswell, MIT.
//
// One big Depth knob squeezes harder the deeper you go: threshold, ratio, saturation and density
// all build together along a tuned curve, and the output is loudness-matched, so Depth makes the
// sound thicker, not just louder. Four Hulls give four compressor personalities; Implode is the
// "every button pressed at once" drum-room explosion; the saturation growls harder the more gain
// reduction there is; the release can breathe in note values with the song.
//
// Signal flow (per sample unless noted):
//
//   in L,R ──┬───────────────────────────────── dry delay (23 samples) ───────────────┐
//            │                                                                        │
//            ├─> [Stereo = Mid/Side: encode M,S]                                       │
//            │      │                                                                  │
//            │      ├─> detector: SC HPF (biquad) -> peak or RMS level (dB)            │
//            │      │     feed-forward Hulls (Bite, Bus) listen to the input;          │
//            │      │     feedback Hulls (Glass, Valve) listen to their own output      │
//            │      │   -> stereo Link -> gain computer (knee / rising ratio / Implode) │
//            │      │   -> attack / release smoothing (two-stage memory on Glass, Bus) │
//            │      │   -> Bloom (release overshoots and swells back)                   │
//            │      v                                                                  │
//            │   gain element (x * gain) -> [decode M/S]                               │
//            │      -> predicted makeup                                                │
//            │      -> reactive saturation, 2x oversampled: Tape / Tube / Iron, drive    │
//            │         rises with the gain reduction (Growl), Bite and Implode add a     │
//            │         gain-reduction-dependent asymmetry (dirt)                        │
//            │      -> DC block ──────────────────────────────────── wet ──┐            │
//            │                                                             v            v
//            │                                          Blend: dry x min(1, 2(1-b)) + wet x min(1, 2b)
//            │                                                             │
//            │                                          measured loudness match (slow, +-9 dB)
//            │                                                             │
//            │                                          + Breath (hiss that rises with the GR)
//            │                                                             │
//            └── level tracker (per block) sets the reference              Output -> L, R
//
// The reference: Pressure follows the programme's level (a blend of a slow peak envelope and a
// slow RMS, pulled a little towards a fixed -14 dBFS) and places the threshold relative to it.
// That is why Depth does roughly the same amount of squeezing on a quiet single sound, a hot drum
// bus or a full mix, without an input or threshold knob. The saturator is normalised to the same
// reference, so its colour does not depend on how hot the track arrives either.
//
// No per-sample transcendental functions: levels and gains use fastLog2 / fastExp2 (bit tricks
// and polynomials, PressureParts.h); the shaper is a rational tanh; every exp/pow/log runs per
// block for coefficients.

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/math.h"
#include "dsp/noise.h"
#include "dsp/tempo.h"

#include "PressureParts.h"

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
    kDepth,        // % the macro: threshold, ratio, saturation, density and (Auto) timing together
    kHull,         // Glass / Bite / Valve / Bus: the compressor's personality
    kImplode,      // on/off: the "all buttons in" drum-room crush
    kColour,       // Tape / Tube / Iron: the saturator's flavour
    kGrowl,        // % how much saturation, and how hard it reacts to the gain reduction
    kAttack,       // 0..1 knob: 0 = Auto (from Hull and Depth), else 0.05..120 ms
    kTide,         // Auto / Free / Sync: where the release time comes from
    kRelease,      // 0..1 knob: 20 ms..3 s (Free) or a note value (Sync); ignored in Auto
    kBloom,        // % the release overshoots (a swell above unity) and settles back
    kBlend,        // % 0 = dry, 50 = full dry + full smash (parallel), 100 = compressed only
    kScHpf,        // Hz sidechain high-pass: Off, or 60..250 Hz, so the kick does not yank the mix
    kLink,         // % stereo link of the detectors (100 = one gain for both sides)
    kStereo,       // L/R or Mid/Side: which pair of signals is compressed
    kBreath,       // % old-gear noise floor that rises as it compresses
    kGrMeter,      // meter: gain reduction in dB (written by the plugin, not saved, not automatable)
    kNumParams
};

enum Hull { kGlass = 0, kBite, kValve, kBus };
enum Colour { kTape = 0, kTube, kIron };
enum TideMode { kTideAuto = 0, kTideFree, kTideSync };
enum Curve { kCurveKnee = 0, kCurveRising, kCurveImplode };

// ---- Hulls ------------------------------------------------------------------------------------
// Ratios for the feedback Hulls are effective ratios (the gain computer slope is set so that the
// loop settles at this ratio). Times: attack is a time constant; release is the recovery time
// (about 95 %: three time constants), so a synced 1/4 has recovered by the next beat.
struct HullDef
{
    bool  rms;              // RMS detector (else instantaneous peak)
    float rmsMs;            // RMS averaging time
    bool  feedback;         // detector listens to the output (else to the input)
    int   curve;            // kCurveKnee or kCurveRising
    float kneeDb;           // knee width (Rising: where the ratio has climbed half way)
    float ratioLo, ratioHi; // ratio at Depth 0 and 100
    float thrLo, thrHi;     // threshold relative to the reference, at Depth 0 and 100 (dB)
    float attLo, attHi;     // Auto attack (ms) at Depth 0 and 100
    float relLo, relHi;     // Auto release (ms, recovery) at Depth 0 and 100
    float memory;           // two-stage release: fraction of the long-term memory the release waits for
    float memSlow;          // the memory's release, as a multiple of the release time constant
    float memChargeMs;      // how long a squeeze takes to fill the memory (short hits barely touch it)
    float satDb;            // the Hull's own saturation offset (dB of drive)
    float asym;             // its own asymmetry (even harmonics)
    float dirt;             // asymmetry added per unit of gain reduction (dirty gain element)
};

const HullDef kHulls[4] = {
    // Glass: a slow, smooth optical character. RMS detector in a feedback loop, very soft knee,
    // and a two-stage release that lets go quickly after a short hit but slowly after a long squeeze.
    { true, 12.0f, true,  kCurveKnee,   14.0f, 2.0f,  6.0f,  2.0f, -24.0f, 40.0f, 22.0f,  240.0f, 160.0f, 0.65f, 14.0f, 2000.0f, -2.0f, 0.10f, 0.00f },
    // Bite: a fast, aggressive transistor grab. Instantaneous peak detector, feed-forward, hard knee,
    // sub-millisecond attack, and a gain element that gets dirtier the more it grabs.
    { false, 0.0f, false, kCurveKnee,    3.0f, 4.0f, 14.0f,  1.0f, -21.0f,  0.8f,  0.15f, 350.0f, 140.0f, 0.00f,  1.0f,    0.0f,  1.0f, 0.05f, 0.05f },
    // Valve: a variable-mu tube style. Feedback, a ratio that keeps rising with level (soft at the
    // threshold, firm when pushed), slow attack, smooth release, warm even-harmonic glue.
    { true, 20.0f, true,  kCurveRising, 10.0f, 3.0f, 10.0f,  5.0f, -21.0f, 80.0f, 40.0f, 1100.0f, 600.0f, 0.25f,  6.0f, 1500.0f,  0.0f, 0.30f, 0.02f },
    // Bus: tight, punchy console/VCA glue. Feed-forward RMS, medium knee, attack slow enough to
    // keep the transients, and an auto release (short memory) that keeps it from pumping by accident.
    { true,  3.0f, false, kCurveKnee,    6.0f, 2.0f,  6.0f,  2.0f, -21.0f, 12.0f,  4.0f,  500.0f, 250.0f, 0.45f,  6.0f, 1500.0f, -1.0f, 0.08f, 0.00f },
};

struct ColourDef { float satDb; float asym; };
const ColourDef kColours[3] = {
    { 0.0f, 0.00f },   // Tape: symmetric, highs saturate first (pre/de-emphasis), soft top
    { 1.0f, 0.35f },   // Tube: asymmetric, second harmonic, warm
    { 2.0f, 0.05f },   // Iron: the low end saturates first (transformer core), weight and grit
};

// ---- Release note values ------------------------------------------------------------------------
struct SyncDiv { const char* name; float beats; };
constexpr SyncDiv kSyncDivs[] = {
    {"1/32", 0.125f}, {"1/16T", 0.16667f}, {"1/16", 0.25f}, {"1/8T", 0.33333f}, {"1/16D", 0.375f},
    {"1/8", 0.5f}, {"1/4T", 0.66667f}, {"1/8D", 0.75f}, {"1/4", 1.0f}, {"1/2T", 1.33333f},
    {"1/4D", 1.5f}, {"1/2", 2.0f}, {"1/2D", 3.0f}, {"1 bar", 4.0f}, {"2 bars", 8.0f}
};
constexpr int kNumSyncDivs = (int)(sizeof(kSyncDivs) / sizeof(kSyncDivs[0]));
inline int syncIndex(float r) { return mpc::clamp((int)std::lround(r * (kNumSyncDivs - 1)), 0, kNumSyncDivs - 1); }
inline float syncPos(int i) { return (float)i / (float)(kNumSyncDivs - 1); }
inline float freeReleaseMs(float r) { return 20.0f * std::pow(150.0f, mpc::clamp(r, 0.0f, 1.0f)); }
inline float freeReleasePos(float ms) { return std::log(ms / 20.0f) / std::log(150.0f); }

// ---- Attack knob ------------------------------------------------------------------------------
constexpr float kAttackAutoBelow = 0.02f;
inline bool attackIsAuto(float v) { return v < kAttackAutoBelow; }
inline float attackMs(float v) { return 0.05f * std::pow(2400.0f, mpc::clamp((v - kAttackAutoBelow) / (1.0f - kAttackAutoBelow), 0.0f, 1.0f)); }
inline float attackPos(float ms) { return kAttackAutoBelow + (1.0f - kAttackAutoBelow) * std::log(ms / 0.05f) / std::log(2400.0f); }

constexpr float kScOffBelow = 59.5f;   // SC HPF knob: below this it is Off
constexpr float kMeterMaxDb = 20.0f;

// Per-sample one-pole glide for gains, so knob moves never zipper.
struct Glide
{
    float v = 0.0f;
    float step(float target, float c) { v += c * (target - v); return v; }
};

inline float coeffMs(float ms, float sr) { return ms <= 0.0f ? 1.0f : 1.0f - std::exp(-1.0f / (ms * 0.001f * sr)); }

} // namespace

class Pressure : public mpc::Plugin
{
public:
    Pressure() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("output",  "Output",  -18.0f, 12.0f, 0.0f, "", 1));      // "dB" from formatValue
        addParam(mpc::floatParam ("depth",   "Depth",   0.0f, 100.0f, 40.0f, "%", 0));
        addParam(mpc::choiceParam("hull",    "Hull",    {"Glass", "Bite", "Valve", "Bus"}, kBus));
        addParam(mpc::boolParam  ("implode", "Implode", false));
        addParam(mpc::choiceParam("colour",  "Colour",  {"Tape", "Tube", "Iron"}, kTape));
        addParam(mpc::floatParam ("growl",   "Growl",   0.0f, 100.0f, 35.0f, "%", 0));
        addParam(mpc::floatParam ("attack",  "Attack",  0.0f, 1.0f, 0.0f, "", 3));         // text from formatValue
        addParam(mpc::choiceParam("tide",    "Tide",    {"Auto", "Free", "Sync"}, kTideAuto));
        addParam(mpc::floatParam ("release", "Release", 0.0f, 1.0f, 0.5f, "", 3));         // text from formatValue
        addParam(mpc::floatParam ("bloom",   "Bloom",   0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam ("blend",   "Blend",   0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::floatParam ("sc_hpf",  "SC HPF",  40.0f, 250.0f, 40.0f, "", 0));     // text from formatValue
        addParam(mpc::floatParam ("link",    "Link",    0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::choiceParam("stereo",  "Stereo",  {"L/R", "Mid/Side"}, 0));
        addParam(mpc::floatParam ("breath",  "Breath",  0.0f, 100.0f, 0.0f, "%", 0));
        {
            mpc::Param m = mpc::floatParam("gr", "GR", 0.0f, kMeterMaxDb, 0.0f, "", 1);  // "dB" from formatValue
            m.automatable = false;
            m.saved = false;
            addParam(m);
        }

        const float aA = 0.0f;                  // Attack: Auto
        const float r14 = syncPos(8), r18 = syncPos(5), r12 = syncPos(11);
        const float rF = freeReleasePos(90.0f), rB = freeReleasePos(140.0f);
        //                               Out   Depth Hull    Impl Colour Growl Attack          Tide       Rel  Bloom Blend SC    Link Stereo Breath
        addProgram("Gentle Glue",       { 0.0f, 38,  kBus,   0,  kIron,  20,   aA,              kTideAuto, 0.5f,  0,  100,  90,  100, 0,  0 });
        addProgram("Drum Smash",        { 0.0f, 75,  kBite,  1,  kTube,  55,   aA,              kTideFree, rF,   30,   50,  40,  100, 0, 10 });
        addProgram("Pumping Bus",       { 0.0f, 65,  kBus,   0,  kTape,  30,   attackPos(8.0f), kTideSync, r14,  45,  100,  40,  100, 0,  0 });
        addProgram("Vocal Glass",       { 0.0f, 50,  kGlass, 0,  kTube,  25,   aA,              kTideAuto, 0.5f,  0,  100, 120,  100, 0,  0 });
        addProgram("Bass Grip",         { 0.0f, 55,  kBite,  0,  kIron,  45,   attackPos(3.0f), kTideFree, rB,    0,  100,  40,  100, 0,  0 });
        addProgram("Warm Valve Tape",   { 0.0f, 45,  kValve, 0,  kTape,  50,   aA,              kTideAuto, 0.5f, 10,  100,  70,  100, 0, 20 });
        addProgram("Deep Trench",       { 0.0f, 100, kGlass, 0,  kTube,  85,   aA,              kTideSync, r12,  60,  100,  60,   70, 1, 35 });
        addProgram("Loop Crush",        { 0.0f, 85,  kBite,  1,  kTape,  70,   attackPos(5.0f), kTideSync, r18,  40,   80,  40,  100, 0, 25 });
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = (float)sampleRate;
        sr2_ = 2.0f * sr_;
        hb_.design();
        for (int c = 0; c < 2; ++c)
        {
            dcBlock_[c].set(8.0f, sr_);
            tapeHp1_[c].set(3000.0f, sr2_);
            tapeHp2_[c].set(1500.0f, sr2_);
            ironLp_[c].set(150.0f, sr2_);
            breathLp_[c].set(7000.0f, sr_);
            measHp_[c].set(100.0f, sr_);
        }
        glide_ = coeffMs(15.0f, sr_);
        implRel_ = coeffMs(20.0f, sr_);
        vuCoeff_ = coeffMs(90.0f, sr_);
        meterInterval_ = std::max(1, (int)(0.04f * sr_));
        reset();
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            up_[c].reset(); down_[c].reset();
            dryDelay_[c].reset(); driveDelay_[c].reset();
            dcBlock_[c].reset(); tapeHp1_[c].reset(); tapeHp2_[c].reset(); ironLp_[c].reset();
            breathLp_[c].reset(); measHp_[c].reset();
            scHp_[c].reset();
            ms_[c] = 0.0f; gr_[c] = 0.0f; slow_[c] = 0.0f; bloomF_[c] = 0.0f; fbPrev_[c] = 0.0f; implEnv_[c] = 0.0f;
        }
        breathNoise_[0].reseed(0x51A7u); breathNoise_[1].reseed(0x2B0Du);
        breathNoise_[0].reset(); breathNoise_[1].reset();

        // The level tracker starts at a typical programme level, so the first second is sensible.
        msTrack_ = dbToGain(-20.0f) * dbToGain(-20.0f);
        peakEnv_ = -10.0f;
        refDb_ = refFromTrackers();
        msIn_ = msOut_ = 0.0f;
        corrDb_ = 0.0f;
        vu_ = 0.0f;
        meterCountdown_ = 0;
        lastMeter_ = -1.0f;

        snapControls_ = true;
        updateControls();
        snapControls_ = false;

        // The drive is undone 23 samples after it was applied: start that history at today's drive.
        const float drive0 = pr::fastDbToGain(satBaseDb_) * invRef_;
        driveDelay_[0].reset(drive0); driveDelay_[1].reset(drive0);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (n <= 0 || ctx.numOutputs < 1) return;

        const float* inL = (ctx.inputs && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* inR = (ctx.inputs && ctx.numInputs > 1) ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        // ---- the level tracker: read the whole block first (only reading, so in-place is safe) ----
        trackLevel(inL, inR, n);
        bpm_ = (float)safeBpm(ctx.transport.bpm);
        updateControls();

        const HullDef& hd = kHulls[hull_];
        const bool ms = midSide_;
        const bool fb = feedback_;
        const bool rmsDet = rmsDet_;
        const float link = link_, invLink = 1.0f - link_;
        const int curve = curve_;
        float measInSum = 0.0f, measOutSum = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float l = inL ? inL[i] : 0.0f;
            const float r = inR ? inR[i] : l;

            // ---- detector and gain element ----
            float a = l, b = r;
            if (ms) { a = 0.5f * (l + r); b = 0.5f * (l - r); }
            float lvl[2];
            for (int c = 0; c < 2; ++c)
            {
                float s = fb ? fbPrev_[c] : (c == 0 ? a : b);
                if (scOn_) s = scHp_[c].process(s);
                if (rmsDet) { ms_[c] += rmsCoeff_ * (s * s - ms_[c]); lvl[c] = pr::fastPowerToDb(ms_[c] + 1.0e-12f); }
                else lvl[c] = pr::fastPowerToDb(s * s + 1.0e-12f);
            }
            const float lmax = std::max(lvl[0], lvl[1]);
            float gain[2], grNow[2];
            for (int c = 0; c < 2; ++c)
            {
                const float L = link * lmax + invLink * lvl[c];
                const float target = gainComputer(L - thr_, curve);
                float g = gr_[c];
                if (target > g) g += attCoeff_ * (target - g);
                else
                {
                    // Two-stage release: let go quickly down to what the long-term memory holds.
                    const float floorDb = std::max(target, memory_ * slow_[c]);
                    g += relCoeff_ * (floorDb - g);
                }
                gr_[c] = g;
                // The memory charges from the gain reduction and lets go towards the target, at its
                // own slow rate (not towards g, which it is holding up: that would compound).
                if (memory_ > 0.0f)
                {
                    if (g > slow_[c]) slow_[c] += memCharge_ * (g - slow_[c]);
                    else slow_[c] += memRelease_ * (std::min(target, slow_[c]) - slow_[c]);
                }
                // Bloom: a slower follower; when the release outruns it, the gain overshoots above
                // unity and swells back as the follower catches up.
                bloomF_[c] += bloomCoeff_ * (g - bloomF_[c]);
                float eff = g - bloomAmt_ * std::max(0.0f, bloomF_[c] - g);
                eff = std::max(eff, -6.0f);
                // What the saturator reacts to: the smoothed gain reduction, or with Implode the
                // instantaneous one, so a transient is crushed the moment it arrives.
                // (a peak-hold of the target: instant up, 20 ms down, so the drive never wobbles at
                // audio rate).
                if (implode_) implEnv_[c] += (target > implEnv_[c] ? 1.0f : implRel_) * (target - implEnv_[c]);
                grNow[c] = implode_ ? std::max(g, implEnv_[c]) : g;
                gain[c] = pr::fastDbToGain(-eff);
            }
            float ga = a * gain[0], gb = b * gain[1];
            fbPrev_[0] = ga; fbPrev_[1] = gb;
            float wl = ga, wr = gb;
            if (ms) { wl = ga + gb; wr = ga - gb; }
            const float grm = std::max(grNow[0], grNow[1]);
            vu_ += vuCoeff_ * (grm - vu_);

            // ---- predicted makeup, then the reactive saturator ----
            const float mk = makeup_.step(makeupT_, glide_);
            wl *= mk; wr *= mk;
            const float wetL = saturate(0, wl, ms ? grm : grNow[0], hd);
            const float wetR = saturate(1, wr, ms ? grm : grNow[1], hd);

            // ---- blend, loudness match, breath, output ----
            const float dl = dryDelay_[0].process(l, pr::kOversampleLatency);
            const float dr = dryDelay_[1].process(r, pr::kOversampleLatency);
            const float dg = dryG_.step(dryGT_, glide_), wg = wetG_.step(wetGT_, glide_);
            const float cg = corr_.step(corrT_, glide_);
            float yl = (dg * dl + wg * wetL) * cg;
            float yr = (dg * dr + wg * wetR) * cg;

            const float mi = measHp_[0].process(0.5f * (dl + dr));   // the input, aligned with the output
            const float mo = measHp_[1].process(0.5f * (yl + yr));
            measInSum += mi * mi;
            measOutSum += mo * mo;

            if (breathOn_)
            {
                const float hg = breath_.step(breathT_, glide_);
                yl += hg * breathLp_[0].process(breathNoise_[0].process());
                yr += hg * breathLp_[1].process(breathNoise_[1].process());
            }

            const float og = outGain_.step(outGainT_, glide_);
            outL[i] = yl * og;
            if (outR) outR[i] = yr * og;
        }

        // Keep the slow states clean of denormals (belt and braces; the framework sets FTZ).
        for (int c = 0; c < 2; ++c)
        {
            ms_[c] = flushDenormal(ms_[c]); gr_[c] = flushDenormal(gr_[c]);
            slow_[c] = flushDenormal(slow_[c]); bloomF_[c] = flushDenormal(bloomF_[c]);
            fbPrev_[c] = flushDenormal(fbPrev_[c]); implEnv_[c] = flushDenormal(implEnv_[c]);
        }
        matchLoudness(measInSum, measOutSum, n);
        updateMeter(n);
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case kOutput:
                if (std::fabs(v) < 0.05f) out = "0.0 dB";
                else { std::snprintf(b, sizeof(b), "%+.1f dB", (double)v); out = b; }
                return true;
            case kAttack:
                if (attackIsAuto(v)) out = "Auto";
                else { const float ms = attackMs(v); std::snprintf(b, sizeof(b), ms < 1.0f ? "%.2f ms" : (ms < 10.0f ? "%.1f ms" : "%.0f ms"), (double)ms); out = b; }
                return true;
            case kRelease:
            {
                const int tide = getInt(kTide);
                if (tide == kTideAuto) out = "Auto";
                else if (tide == kTideSync) out = kSyncDivs[syncIndex(v)].name;
                else
                {
                    const float ms = freeReleaseMs(v);
                    if (ms < 1000.0f) std::snprintf(b, sizeof(b), "%.0f ms", (double)ms);
                    else std::snprintf(b, sizeof(b), "%.2f s", (double)(ms * 0.001f));
                    out = b;
                }
                return true;
            }
            case kScHpf:
                if (v < kScOffBelow) out = "Off";
                else { std::snprintf(b, sizeof(b), "%.0f Hz", (double)v); out = b; }
                return true;
            case kGrMeter:
                std::snprintf(b, sizeof(b), "%.1f dB", (double)v); out = b; return true;
            default: return false;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name           = "Pressure";
        info.vendor         = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId       = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version        = 1100;            // 1.1.0.0
        info.isInstrument   = false;
        info.numInputs      = 2;
        info.numOutputs     = 2;
        info.latencySamples = pr::kOversampleLatency;
        info.tailSamples    = 1024;            // the oversampler and dry delay; no reverb-like tail
        return info;
    }
    static constexpr char kUid[5] = "GKp3";

    // ---- gain computer: level over the threshold (dB) -> gain reduction (dB, >= 0) ----
    float gainComputer(float over, int curve) const
    {
        const float W = knee_;
        switch (curve)
        {
            case kCurveRising:
            {
                // The ratio climbs with level: the slope starts at zero half a knee below the
                // threshold and approaches the full slope as the signal pushes further in.
                const float o = over + 0.5f * W;
                if (o <= 0.0f) return 0.0f;
                return slope_ * o * o / (o + W);
            }
            case kCurveImplode:
            {
                // Nearly a limiter, and past 6 dB over it folds back: the louder the hit, the
                // quieter it comes out, so the room and the tail explode around it.
                if (over <= -0.5f) return 0.0f;
                const float o = over + 0.5f;
                return slope_ * o + 0.55f * std::max(0.0f, over - 6.0f);
            }
            default:
            {
                if (2.0f * over <= -W) return 0.0f;
                if (2.0f * over < W) { const float t = over + 0.5f * W; return slope_ * t * t / (2.0f * W); }
                return slope_ * over;
            }
        }
    }

    // Static gain reduction (feed-forward equivalent) at a level over the threshold, for the
    // predicted makeup. A feedback loop with slope (R - 1) settles at the same static curve.
    float staticGr(float over) const
    {
        const float s = 1.0f - 1.0f / ratio_;
        const float W = knee_;
        if (curve_ == kCurveImplode) return over > 0.0f ? 0.95f * over + 0.55f * std::max(0.0f, over - 6.0f) : 0.0f;
        if (curve_ == kCurveRising) { const float o = over + 0.5f * W; return o > 0.0f ? s * o * o / (o + W) : 0.0f; }
        if (2.0f * over <= -W) return 0.0f;
        if (2.0f * over < W) { const float t = over + 0.5f * W; return s * t * t / (2.0f * W); }
        return s * over;
    }

    // ---- reactive saturation, 2x oversampled ----
    float saturate(int c, float x, float grDb, const HullDef& hd)
    {
        // Drive in dB: a base that builds with Depth and Growl, plus Growl x the gain reduction.
        const float driveDb = satBaseDb_ + satGrowlK_ * grDb;
        const float drive = pr::fastDbToGain(driveDb) * invRef_;
        const float asym = asymBase_ + (hd.dirt + implodeDirt_) * grDb;
        const float kn = 1.0f + std::min(asym, 2.0f);
        const float ikn = 1.0f / kn;

        float u0, u1;
        up_[c].process(x * drive, hb_, u0, u1);
        const float p0 = shapeOne(c, u0, kn, ikn);
        const float p1 = shapeOne(c, u1, kn, ikn);
        float y = down_[c].process(p0, p1, hb_);
        y = dcBlock_[c].process(y);
        // Undo the drive with the value that went in 23 samples ago (the oversampler's latency).
        const float dDel = driveDelay_[c].process(drive, pr::kOversampleLatency);
        return y / std::max(dDel, 1.0e-6f);
    }

    float shapeOne(int c, float u, float kn, float ikn)
    {
        switch (colour_)
        {
            case kTube:
                return pr::shape(u, kn, ikn);
            case kIron:
            {
                // The core saturates on the lows first: the band under ~150 Hz is driven harder.
                const float lo = ironLp_[c].process(u);
                const float hi = u - lo;
                return pr::shape(lo * 2.2f, kn, ikn) * (1.0f / 2.2f) + pr::shape(hi * 0.6f, kn, ikn) * (1.0f / 0.6f);
            }
            default:
            {
                // Tape: pre-emphasis (highs +6 dB) into the shaper, matching de-emphasis after, so
                // the highs saturate first and the top end softens as it is pushed.
                const float e = u + tapeHp1_[c].process(u);
                const float y = pr::shape(e, kn, ikn);
                return y - 0.5f * tapeHp2_[c].process(y);
            }
        }
    }

    // ---- the programme level the threshold follows (per block) ----
    void trackLevel(const float* inL, const float* inR, int n)
    {
        float sum = 0.0f, pk = 0.0f;
        if (inL)
            for (int i = 0; i < n; ++i)
            {
                const float l = inL[i], r = inR ? inR[i] : l;
                sum += 0.5f * (l * l + r * r);
                pk = std::max(pk, std::max(std::fabs(l), std::fabs(r)));
            }
        const float msBlock = sum / (float)n;
        const float dt = (float)n / sr_;
        // Mean square: rises in ~0.3 s, falls in ~3 s.
        const float kRise = 1.0f - std::exp(-dt / 0.3f), kFall = 1.0f - std::exp(-dt / 3.0f);
        msTrack_ += (msBlock > msTrack_ ? kRise : kFall) * (msBlock - msTrack_);
        msTrack_ = std::max(msTrack_, 1.0e-12f);
        // Peak envelope (dB): catches peaks in ~50 ms, falls 4 dB per second.
        const float pkDb = gainToDb(pk, -120.0f);
        if (pkDb > peakEnv_) peakEnv_ += (1.0f - std::exp(-dt / 0.05f)) * (pkDb - peakEnv_);
        else peakEnv_ = std::max(pkDb, peakEnv_ - 4.0f * dt);
        peakEnv_ = std::max(peakEnv_, -70.0f);
        refDb_ = refFromTrackers();
    }

    float refFromTrackers() const
    {
        const float rmsDb = 10.0f * std::log10(msTrack_);
        float ref = 0.5f * peakEnv_ + 0.5f * (rmsDb + 8.0f);
        ref = 0.8f * ref + 0.2f * -14.0f;   // a little of a fixed point: loud passages squeeze a bit more
        return std::max(ref, -50.0f);
    }

    // ---- measured loudness match: a slow integrator that keeps the output as loud as the input ----
    void matchLoudness(float inSum, float outSum, int n)
    {
        const float dt = (float)n / sr_;
        const float k = 1.0f - std::exp(-dt / 0.4f);
        msIn_ += k * (inSum / (float)n - msIn_);
        msOut_ += k * (outSum / (float)n - msOut_);
        msIn_ = flushDenormal(msIn_); msOut_ = flushDenormal(msOut_);
        if (msIn_ > 1.0e-6f && msOut_ > 1.0e-9f)
        {
            const float err = clamp(10.0f * std::log10(msIn_ / msOut_), -6.0f, 6.0f);
            corrDb_ = clamp(corrDb_ + err * dt / 1.5f, -9.0f, 9.0f);
        }
        corrT_ = dbToGain(corrDb_);
    }

    // ---- meter: about 25 times a second, only when it moved ----
    void updateMeter(int n)
    {
        meterCountdown_ -= n;
        if (meterCountdown_ > 0) return;
        meterCountdown_ = meterInterval_;
        // Quantised to 1/128 of the scale: one film-strip frame of a needle.
        const float norm = std::round(clamp(vu_ / kMeterMaxDb, 0.0f, 1.0f) * 128.0f) / 128.0f;
        if (norm == lastMeter_) return;
        lastMeter_ = norm;
        setNormalised(kGrMeter, norm);
        notifyHostParameterChanged(kGrMeter);
    }

    // ---- per-block control updates ----
    void updateControls()
    {
        const int hull = getInt(kHull);
        const bool implode = getBool(kImplode);
        hull_ = hull;
        colour_ = getInt(kColour);
        const HullDef& hd = kHulls[hull_];
        const float d = get(kDepth) * 0.01f;
        const float dc = std::pow(d, 0.8f);

        // Detector and curve. Implode always listens with the Bite-style peak detector, feed-forward.
        implode_ = implode;
        rmsDet_ = implode ? false : hd.rms;
        feedback_ = implode ? false : hd.feedback;
        curve_ = implode ? (int)kCurveImplode : hd.curve;
        knee_ = implode ? 1.0f : hd.kneeDb;
        ratio_ = implode ? 20.0f : hd.ratioLo * std::pow(hd.ratioHi / hd.ratioLo, dc);
        slope_ = feedback_ ? (ratio_ - 1.0f) : (1.0f - 1.0f / ratio_);
        thr_ = refDb_ + hd.thrLo + (hd.thrHi - hd.thrLo) * dc + (implode ? -6.0f : 0.0f);
        rmsCoeff_ = coeffMs(hd.rmsMs, sr_);

        // Timing. Attack: Auto follows Hull and Depth.
        const float atkKnob = get(kAttack);
        float att = attackIsAuto(atkKnob) ? hd.attLo * std::pow(hd.attHi / hd.attLo, d) : attackMs(atkKnob);
        if (implode) att = std::max(att, 0.1f);
        const int tide = getInt(kTide);
        float rel;
        if (tide == kTideSync) rel = beatsToSeconds(kSyncDivs[syncIndex(get(kRelease))].beats, bpm_) * 1000.0f;
        else if (tide == kTideFree) rel = freeReleaseMs(get(kRelease));
        else rel = hd.relLo * std::pow(hd.relHi / hd.relLo, d);
        if (implode && tide == kTideAuto) rel *= 0.45f;
        const float relTau = rel / 3.0f;
        attCoeff_ = coeffMs(att, sr_);
        relCoeff_ = coeffMs(relTau, sr_);
        // The two-stage memory is part of the Hull's own release; a synced release is exactly the
        // note value (so the pump stays in time), and Implode lets go at once.
        memory_ = (implode || tide == kTideSync) ? 0.0f : hd.memory;
        memCharge_ = coeffMs(hd.memChargeMs, sr_);
        memRelease_ = coeffMs(relTau * hd.memSlow, sr_);
        bloomCoeff_ = coeffMs(relTau * 2.5f, sr_);
        bloomAmt_ = clamp(get(kBloom) * 0.01f + (implode ? 0.35f : 0.0f), 0.0f, 1.2f);

        // Sidechain high-pass (detector only).
        const float sc = get(kScHpf);
        const bool scOn = sc >= kScOffBelow;
        if (scOn && (sc != scHz_ || !scOn_))
        {
            const BiquadCoeffs co = BiquadCoeffs::highpass(sr_, sc, 0.7071f);
            scHp_[0].setCoeffs(co); scHp_[1].setCoeffs(co);
        }
        if (!scOn && scOn_) { scHp_[0].reset(); scHp_[1].reset(); }
        scOn_ = scOn; scHz_ = sc;

        link_ = get(kLink) * 0.01f;
        const bool midSide = getInt(kStereo) == 1;
        if (midSide != midSide_ && !snapControls_)
        {
            // Changing the pair being compressed: start the detectors fresh rather than jump.
            for (int c = 0; c < 2; ++c) { ms_[c] = 0.0f; fbPrev_[c] = 0.0f; }
        }
        midSide_ = midSide;

        // Saturation. Normalised to the reference level, so the colour does not depend on how hot
        // the track arrives; the predicted makeup has already put the wet path at that loudness.
        const float growl = get(kGrowl) * 0.01f;
        const ColourDef& cd = kColours[colour_];
        satBaseDb_ = -20.0f + 8.0f * dc + 15.0f * growl + hd.satDb + cd.satDb + (implode ? 10.0f : 0.0f);
        satGrowlK_ = 0.75f * growl + (implode ? 0.3f : 0.0f);
        asymBase_ = cd.asym + hd.asym + (implode ? 0.45f : 0.0f);
        implodeDirt_ = implode ? 0.06f : 0.0f;
        invRef_ = 1.0f / dbToGain(refDb_);

        // Predicted makeup: most of the static gain reduction at the reference level comes back at
        // once; the measured loudness match trims the rest slowly.
        const float pm = staticGr(refDb_ - thr_) * (implode ? 0.5f : 0.6f);
        makeupT_ = dbToGain(clamp(pm, 0.0f, 24.0f));

        // Blend: 0 dry, 50 full dry + full compressed (parallel smash), 100 compressed only.
        const float bl = get(kBlend) * 0.01f;
        dryGT_ = std::min(1.0f, 2.0f * (1.0f - bl));
        wetGT_ = std::min(1.0f, 2.0f * bl);

        // Breath: hiss about 60 dB under the reference, rising 1 dB per dB of gain reduction.
        const float breath = get(kBreath) * 0.01f;
        breathOn_ = breath > 0.0f || breath_.v > 1.0e-7f;
        breathT_ = breath > 0.0f ? breath * dbToGain(refDb_ - 58.0f + 1.0f * std::min(vu_, 24.0f)) * 3.0f : 0.0f;

        outGainT_ = dbToGain(get(kOutput));

        if (snapControls_)
        {
            makeup_.v = makeupT_; dryG_.v = dryGT_; wetG_.v = wetGT_; corr_.v = corrT_ = dbToGain(corrDb_);
            breath_.v = breathT_; outGain_.v = outGainT_;
        }
    }

    // ---- state ----
    float sr_ = 44100.0f, sr2_ = 88200.0f, bpm_ = 120.0f;
    pr::HalfbandTaps hb_;
    pr::Upsampler2x up_[2];
    pr::Downsampler2x down_[2];
    pr::ShortDelay dryDelay_[2], driveDelay_[2];
    pr::OnePoleHP dcBlock_[2], tapeHp1_[2], tapeHp2_[2], measHp_[2];
    pr::OnePoleLP ironLp_[2], breathLp_[2];
    Biquad scHp_[2];
    PinkNoise breathNoise_[2];

    // detector / gain element
    float ms_[2] = {}, gr_[2] = {}, slow_[2] = {}, bloomF_[2] = {}, fbPrev_[2] = {}, implEnv_[2] = {};
    float implRel_ = 0.001f;

    // controls (block rate)
    bool snapControls_ = false;
    int hull_ = kBus, colour_ = kTape, curve_ = kCurveKnee;
    bool implode_ = false, rmsDet_ = true, feedback_ = false, scOn_ = false, midSide_ = false, breathOn_ = false;
    float scHz_ = 0.0f;
    float knee_ = 6.0f, ratio_ = 2.0f, slope_ = 0.5f, thr_ = -20.0f, rmsCoeff_ = 0.01f;
    float attCoeff_ = 0.01f, relCoeff_ = 0.001f, memory_ = 0.0f, memCharge_ = 0.0f, memRelease_ = 0.0f;
    float bloomCoeff_ = 0.001f, bloomAmt_ = 0.0f, link_ = 1.0f;
    float satBaseDb_ = -20.0f, satGrowlK_ = 0.0f, asymBase_ = 0.0f, implodeDirt_ = 0.0f, invRef_ = 1.0f;
    float makeupT_ = 1.0f, dryGT_ = 0.0f, wetGT_ = 1.0f, corrT_ = 1.0f, breathT_ = 0.0f, outGainT_ = 1.0f;
    Glide makeup_, dryG_, wetG_, corr_, breath_, outGain_;
    float glide_ = 0.01f;

    // level tracker and loudness match
    float msTrack_ = 1.0e-4f, peakEnv_ = -10.0f, refDb_ = -14.0f;
    float msIn_ = 0.0f, msOut_ = 0.0f, corrDb_ = 0.0f;

    // meter
    float vu_ = 0.0f, vuCoeff_ = 0.001f, lastMeter_ = -1.0f;
    int meterCountdown_ = 0, meterInterval_ = 1764;
};

MPC_PLUGIN_ENTRY(Pressure)
