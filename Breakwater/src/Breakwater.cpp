// Breakwater - a guitar amp for clean, chimey tones and the edge of breakup. Groundswell, MIT.
//
// Signal flow (the amp is mono: left and right are summed, (L + R) / 2; only the reverb is stereo):
//
//   in L+R -> Input gain -> Comp (gentle peak compressor) -> coupling high-pass
//          -> Bright (a treble shelf that fades out as Gain goes up, like a bright cap on a volume pot)
//          -> [ 2x oversampled:
//                 stage 1 (biased tanh, fixed drive per Voice) -> interstage low-pass + DC block
//                 -> Gain -> stage 2 (biased tanh) -> DC block
//                 -> tone stack (the passive bass/middle/treble circuit, Yeh & Smith)
//                 -> Chime (presence peak + a little even-harmonic "shimmer" on the upper mids)
//                 -> Master -> power stage (soft push-pull clip on a sagging supply rail) -> DC block ]
//          -> Tremolo (Smooth, Opto, or Harmonic: lows and highs in opposite phase)
//          -> Cabinet (1x12 open, 2x12 open, 4x12 closed or Direct: a handful of filters, Mic, Distance)
//          -> + Spring reverb (stereo: two springs) -> Output -> L, R
//
// Voice picks the character of the whole amp: tone-stack component values, how hard stage 1 is
// pushed, the bias (even harmonics) of each stage, interstage brightness, the Chime frequency and
// how much the power stage sags. "American" is the clean, scooped, big-headroom amp; "British" the
// chimey, mid-forward one that breaks up earlier; "Boutique" the fat, touch-sensitive edge.
//
// No per-sample transcendental functions: the shapers are rational (fastTanh); every exp/pow/log
// runs per block (coefficients) or every 16 samples (the compressor).

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/math.h"
#include "dsp/tempo.h"

#include "AmpParts.h"
#include "Spring.h"

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
    kInput,        // dB into the amp: sets where your playing sits against the breakup point
    kComp,         // % gentle compression before the preamp (evens pick strokes)
    kVoice,        // American / British / Boutique
    kGain,         // 0..10 preamp gain (how hard stage 2 is driven)
    kBright,       // bright switch: treble boost, strongest at low Gain
    kBass,         // 0..10 tone stack (audio-taper pot)
    kMiddle,       // 0..10 tone stack
    kTreble,       // 0..10 tone stack
    kChime,        // 0..10 upper-mid presence and class-A shimmer
    kSag,          // 0..10 supply sag: compression and bloom when you hit hard
    kMaster,       // 0..10 drive into the power stage (output is roughly compensated)
    kCab,          // 1x12 / 2x12 / 4x12 / Direct
    kMic,          // % 0 = centre of the cone (bright), 100 = edge (darker, rounder)
    kDistance,     // cm from the grille: proximity bass up close, floor bounce and less top further back
    kDwell,        // 0..10 how hard the spring is driven, and how long it rings
    kRevTone,      // 0..10 the reverb's brightness
    kRevMix,       // % reverb level
    kTremMode,     // Smooth / Opto / Harmonic
    kTremDepth,    // % 0 = off
    kTremSync,     // Free / Sync
    kTremRate,     // 0..1: 0.5..12 Hz free, or 2 bars..1/32 synced
    kNumParams
};

enum Voice { kAmerican = 0, kBritish, kBoutique };
enum Cab { k1x12 = 0, k2x12, k4x12, kDirect };
enum TremMode { kSmooth = 0, kOpto, kHarmonic };

// ---- Voices -----------------------------------------------------------------------------------
struct VoiceDef
{
    bw::ToneStackParts stack;
    float makeupDb;        // the tone stack's insertion loss at noon given back (American: +3 dB more, its clean range sits low)
    float pre1;            // stage 1 drive
    float asym1, asym2;    // how much later each stage clips on the negative swing: more = more even harmonics as you dig in
    float stageLpHz;       // interstage low-pass
    float gainLoDb, gainHiDb;
    float presenceHz;      // where Chime's peak sits
    float powerAsym;       // power stage asymmetry (class-A flavour)
    float sagScale;
};

// Component values: C1, C2, C3 (F), R1 treble, R2 bass, R3 middle, R4 slope (ohm).
const VoiceDef kVoices[3] = {
    // American clean: scooped stack, lots of headroom, stiff supply.
    { {250e-12, 100e-9, 47e-9, 250e3, 250e3, 10e3, 100e3}, 15.65f, 0.9f, 0.15f, 0.25f, 9000.0f,
      -6.0f, 26.0f, 3300.0f, 0.08f, 0.8f },
    // British chime: mid-forward stack, brighter interstage, earlier breakup, class-A asymmetry.
    { {470e-12, 22e-9, 22e-9, 220e3, 1e6, 25e3, 33e3}, 5.31f, 1.3f, 0.35f, 0.50f, 11000.0f,
      -3.0f, 29.0f, 2700.0f, 0.40f, 1.0f },
    // Boutique edge: fat mids, warmer interstage, the most touch-sensitive breakup and sag.
    { {250e-12, 20e-9, 20e-9, 250e3, 1e6, 25e3, 56e3}, 7.65f, 1.6f, 0.40f, 0.45f, 7500.0f,
      0.0f, 30.0f, 2400.0f, 0.20f, 1.25f },
};

// ---- Cabinets ---------------------------------------------------------------------------------
struct CabDef
{
    float hpHz, hpQ;              // low-end roll-off and its resonant bump
    float lowHz, lowDb;           // low-mid body
    float midHz, midDb, midQ;     // the cone's mid dip (or, for the small open back, its honk)
    float presHz, presDb, presQ;  // the speaker's presence peak
    float lpHz;                   // where the cone stops: three low-pass sections from here up
    float backLevel;              // open back: a reflection from the wall behind
};
const CabDef kCabs[3] = {
    // 1x12 open back: lighter low end, a boxy upper-mid honk, the brightest top.
    { 105.0f, 0.70f, 220.0f, 1.0f, 1100.0f,  2.0f, 1.2f, 3200.0f, 3.0f, 1.4f, 4700.0f, 0.30f },
    // 2x12 open back: fuller lows, a scooped lower mid, a broad presence hump.
    {  82.0f, 1.00f, 150.0f, 3.0f,  650.0f, -4.0f, 0.9f, 2200.0f, 5.0f, 1.3f, 4800.0f, 0.22f },
    // 4x12 closed back: the tight resonant thump, the deepest mid scoop, a sharp upper presence.
    {  75.0f, 1.60f, 110.0f, 4.0f,  480.0f, -6.0f, 0.8f, 3000.0f, 6.0f, 2.2f, 4200.0f, 0.0f  },
};
constexpr int kCabDelaySize = 2048;      // floor bounce + back wall, up to ~9 ms at 192 kHz

// ---- Tremolo rates ----------------------------------------------------------------------------
struct SyncDiv { const char* name; float beats; };
constexpr SyncDiv kSyncDivs[] = {
    {"2 bars", 8.0f}, {"1 bar", 4.0f}, {"1/2", 2.0f}, {"1/4D", 1.5f}, {"1/4", 1.0f}, {"1/8D", 0.75f},
    {"1/4T", 0.66667f}, {"1/8", 0.5f}, {"1/8T", 0.33333f}, {"1/16", 0.25f}, {"1/16T", 0.16667f}, {"1/32", 0.125f}
};
constexpr int kNumSyncDivs = (int)(sizeof(kSyncDivs) / sizeof(kSyncDivs[0]));
inline int syncIndex(float r) { return mpc::clamp((int)std::lround(r * (kNumSyncDivs - 1)), 0, kNumSyncDivs - 1); }
inline float syncPos(int i) { return (float)i / (float)(kNumSyncDivs - 1); }
inline float freeHz(float r) { return 0.5f * std::pow(24.0f, mpc::clamp(r, 0.0f, 1.0f)); }
inline float freePos(float hz) { return std::log(hz / 0.5f) / std::log(24.0f); }

constexpr int kCompTick = 16;

// Per-sample one-pole glide for gains, so knob moves never zipper.
struct Glide
{
    float v = 0.0f;
    float step(float target, float c) { v += c * (target - v); return v; }
};

} // namespace

class Breakwater : public mpc::Plugin
{
public:
    Breakwater() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("output",    "Output",     -24.0f, 12.0f, -2.0f, "", 1));    // "dB" from formatValue
        addParam(mpc::floatParam ("input",     "Input",      -12.0f, 18.0f, 0.0f, "", 1));     // "dB" from formatValue
        addParam(mpc::floatParam ("comp",      "Comp",       0.0f, 100.0f, 15.0f, "%", 0));
        addParam(mpc::choiceParam("voice",     "Voice",      {"American", "British", "Boutique"}, kBritish));
        addParam(mpc::floatParam ("gain",      "Gain",       0.0f, 10.0f, 6.0f, "", 1));
        addParam(mpc::boolParam  ("bright",    "Bright",     true));
        addParam(mpc::floatParam ("bass",      "Bass",       0.0f, 10.0f, 4.5f, "", 1));
        addParam(mpc::floatParam ("middle",    "Middle",     0.0f, 10.0f, 6.0f, "", 1));
        addParam(mpc::floatParam ("treble",    "Treble",     0.0f, 10.0f, 6.0f, "", 1));
        addParam(mpc::floatParam ("chime",     "Chime",      0.0f, 10.0f, 6.0f, "", 1));
        addParam(mpc::floatParam ("sag",       "Sag",        0.0f, 10.0f, 5.0f, "", 1));
        addParam(mpc::floatParam ("master",    "Master",     0.0f, 10.0f, 6.0f, "", 1));
        addParam(mpc::choiceParam("cab",       "Cab",        {"1x12", "2x12", "4x12", "Direct"}, k2x12));
        addParam(mpc::floatParam ("mic",       "Mic",        0.0f, 100.0f, 30.0f, "", 0));     // text from formatValue
        addParam(mpc::logParam   ("distance",  "Distance",   2.0f, 100.0f, 15.0f, "", 0));     // text from formatValue
        addParam(mpc::floatParam ("dwell",     "Dwell",      0.0f, 10.0f, 3.0f, "", 1));
        addParam(mpc::floatParam ("rev_tone",  "Rev Tone",   0.0f, 10.0f, 5.0f, "", 1));
        addParam(mpc::floatParam ("rev_mix",   "Rev Mix",    0.0f, 100.0f, 12.0f, "%", 0));
        addParam(mpc::choiceParam("trem_mode", "Trem Mode",  {"Smooth", "Opto", "Harmonic"}, kSmooth));
        addParam(mpc::floatParam ("trem_depth","Trem Depth", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::choiceParam("trem_sync", "Trem Sync",  {"Free", "Sync"}, 1));
        addParam(mpc::floatParam ("trem_rate", "Trem Rate",  0.0f, 1.0f, syncPos(7), "", 3));   // text from formatValue

        const float s18 = syncPos(7), s14 = syncPos(4);
        //                                Out   In  Comp Voice      Gain Brt  Bass Mid  Treb Chime Sag  Mast Cab    Mic Dist Dwell RTone RMix TMode      TDep TSync TRate
        addProgram("Edge of Breakup",   { -2.0f, 0, 15, kBritish,   6.0f, 1,  4.5f, 6.0f, 6.0f, 6.0f, 5.0f, 6.0f, k2x12, 30, 15, 3.0f, 5.0f, 12, kSmooth,    0,  1, s18 });
        addProgram("Clean Tremolo",     { 0.5f, 0, 20, kAmerican,  3.0f, 1,  5.0f, 4.5f, 6.0f, 3.0f, 2.0f, 4.0f, k2x12, 25, 20, 4.0f, 5.0f, 20, kSmooth,   55,  1, s18 });
        addProgram("Funk Glass",        { -2.0f, 0, 45, kAmerican,  2.5f, 1,  4.0f, 3.5f, 6.5f, 4.0f, 1.0f, 4.0f, k1x12, 15, 10, 2.0f, 6.0f,  6, kSmooth,    0,  1, s18 });
        addProgram("Surf Spring",       { -3.5f, 0, 10, kAmerican,  3.5f, 1,  4.0f, 5.0f, 7.0f, 4.0f, 3.0f, 5.0f, k2x12, 20, 20, 8.0f, 6.0f, 45, kSmooth,    0,  1, s18 });
        addProgram("Jazz Warm",         { 8.0f, 0, 25, kAmerican,  2.5f, 0,  6.0f, 5.5f, 3.5f, 1.0f, 3.0f, 4.0f, k1x12, 65, 35, 2.0f, 3.0f, 10, kSmooth,    0,  1, s18 });
        addProgram("Sparkle Arps",      { -3.5f, 0, 30, kBritish,   3.0f, 1,  4.0f, 5.0f, 7.5f, 8.0f, 2.0f, 4.0f, k2x12, 15, 20, 4.0f, 7.0f, 25, kSmooth,    0,  1, s18 });
        addProgram("Harmonic Swirl",    { 2.0f, 0, 15, kBoutique,  4.5f, 0,  5.0f, 6.0f, 5.5f, 4.0f, 4.0f, 5.0f, k1x12, 35, 25, 3.0f, 5.0f, 18, kHarmonic, 60,  1, s14 });
        addProgram("Boutique Push",     { -1.0f, 0, 10, kBoutique,  6.5f, 0,  5.0f, 7.0f, 5.5f, 5.0f, 7.0f, 7.0f, k4x12, 40, 15, 2.0f, 4.0f,  8, kSmooth,    0,  1, s18 });
        (void)freePos;
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = (float)sampleRate;
        sr2_ = 2.0f * sr_;
        hb_.design();
        spring_.prepare(sr_);

        inHp_.set(40.0f, sr_);
        dc1_.set(15.0f, sr2_);
        dc2_.set(15.0f, sr2_);
        dcP_.set(10.0f, sr2_);
        shimHp_.set(1200.0f, sr2_);
        harmLp_.setCoeffs(BiquadCoeffs::lowpass(sr_, 650.0f, 0.6f));

        compAtt_ = onePoleCoeff(1.5f, sr_);
        compRel_ = onePoleCoeff(120.0f, sr_);
        sagAtt_ = onePoleCoeff(8.0f, sr2_);
        sagRel_ = onePoleCoeff(220.0f, sr2_);
        glide_ = onePoleCoeff(12.0f, sr_);
        optoAtt_ = onePoleCoeff(3.0f, sr_);
        optoRel_ = onePoleCoeff(30.0f, sr_);
        reset();
    }

    void reset() override
    {
        up_.reset(); down_.reset();
        inHp_.reset(); lp1_.reset(); dc1_.reset(); dc2_.reset(); dcP_.reset(); shimHp_.reset();
        bright_.reset(); presence_.reset(); stack_.reset();
        for (auto& f : cabF_) f.reset();
        for (float& v : cabBuf_) v = 0.0f;
        cabW_ = 0;
        harmLp_.reset();
        spring_.reset();
        springOn_ = false;
        compEnv_ = 0.0f; compG_ = 1.0f; compStep_ = 0.0f; compCount_ = 0;
        sagEnv_ = 0.0f; optoEnv_ = 0.0f;
        tremPhase_ = 0.0;

        // Snap every smoothed control to its target, so a load starts settled.
        snapControls_ = true;
        updateControls(0);
        snapControls_ = false;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (n <= 0 || ctx.numOutputs < 1) return;
        updateControls(n);
        clockTremolo(ctx);

        const float* inL = (ctx.inputs && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* inR = (ctx.inputs && ctx.numInputs > 1) ? ctx.inputs[1] : nullptr;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        const VoiceDef& vd = kVoices[voice_];
        const bool cabOn = cab_ != kDirect;
        const bool tremOn = tremDepthT_ > 0.0f || tremDepth_.v > 1.0e-4f;
        const bool revOn = revMixT_ > 0.0f || revWet_.v > 1.0e-5f;
        if (!revOn && springOn_) { spring_.reset(); springOn_ = false; }
        if (revOn) springOn_ = true;

        // In chunks the spring can take at once: the amp runs per sample into mono_, then the
        // spring runs over the chunk, then the output is mixed.
        for (int start = 0; start < n; start += bw::SpringReverb::kBlock)
        {
            const int m = std::min(bw::SpringReverb::kBlock, n - start);
            for (int j = 0; j < m; ++j)
            {
                const int i = start + j;
                const float l = inL ? inL[i] : 0.0f;
                const float r = inR ? inR[i] : l;
                float x = 0.5f * (l + r);

                // ---- input and compressor (base rate) ----
                x *= inGain_.step(inGainT_, glide_);
                const float ax = std::fabs(x);
                compEnv_ += (ax > compEnv_ ? compAtt_ : compRel_) * (ax - compEnv_);
                if (--compCount_ <= 0) { compCount_ = kCompTick; compStep_ = (compTarget() - compG_) / (float)kCompTick; }
                compG_ += compStep_;
                x *= compG_;
                x = inHp_.process(x);
                x = bright_.process(x);

                // ---- 2x oversampled preamp, tone stack and power stage ----
                const float gain = gain_.step(gainT_, glide_);
                const float post = post_.step(postT_, glide_);
                const float pdrive = pDrive_.step(pDriveT_, glide_);
                const float pcomp = pComp_.step(pCompT_, glide_);
                float u0, u1;
                up_.process(x, hb_, u0, u1);
                const float p0 = ampSample(u0, vd, gain, post, pdrive, pcomp);
                const float p1 = ampSample(u1, vd, gain, post, pdrive, pcomp);
                float a = down_.process(p0, p1, hb_);

                // ---- tremolo, cabinet ----
                if (tremOn) a = tremolo(a);
                if (cabOn) a = cabinet(a);
                mono_[j] = a;
            }

            // ---- spring and output ----
            if (springOn_) spring_.processBlock(mono_, wetL_, wetR_, m);
            for (int j = 0; j < m; ++j)
            {
                const int i = start + j;
                const float og = outGain_.step(outGainT_, glide_);
                const float dry = revDry_.step(revDryT_, glide_);
                const float wet = revWet_.step(revWetT_, glide_);
                float yl = mono_[j] * dry, yr = yl;
                if (springOn_) { yl += wet * wetL_[j]; yr += wet * wetR_[j]; }
                outL[i] = yl * og;
                if (outR) outR[i] = yr * og;
            }
        }
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case kOutput: case kInput:
                std::snprintf(b, sizeof(b), "%+.1f dB", (double)v);
                if (std::fabs(v) < 0.05f) std::snprintf(b, sizeof(b), "0.0 dB");
                out = b; return true;
            case kMic:
                if (v < 2.5f) out = "Centre";
                else if (v > 97.5f) out = "Edge";
                else { std::snprintf(b, sizeof(b), "%.0f%% edge", (double)v); out = b; }
                return true;
            case kDistance:
                std::snprintf(b, sizeof(b), "%.0f cm", (double)v); out = b; return true;
            case kTremRate:
                if (getInt(kTremSync) == 1) out = kSyncDivs[syncIndex(v)].name;
                else { std::snprintf(b, sizeof(b), "%.1f Hz", (double)freeHz(v)); out = b; }
                return true;
            default: return false;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name           = "Breakwater";
        info.vendor         = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId       = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version        = 1000;            // 1.0.0.0
        info.isInstrument   = false;
        info.numInputs      = 2;
        info.numOutputs     = 2;
        info.latencySamples = bw::kOversampleLatency;
        info.tailSamples    = 5 * 44100;       // the spring at full Dwell
        return info;
    }
    static constexpr char kUid[5] = "GAxC";

    // ---- one oversampled sample through the amp ----
    float ampSample(float u, const VoiceDef& vd, float gain, float post, float pdrive, float pcomp)
    {
        float s = tube1_(vd.pre1 * u);
        s = dc1_.process(lp1_.process(s));
        s = tube2_(s * gain) * post;
        s = dc2_.process(s);
        s = stack_.process(s) * makeup_;

        // Chime: the presence peak, then the shimmer - the squared upper mids add a second harmonic
        // that grows with level (DC removed after the power stage).
        s = presence_.process(s);
        const float h = shimHp_.process(s);
        s += shimAmt_ * h * h;

        // Power stage on a sagging rail: the rail drops as the output gets loud, so the stage
        // compresses and clips earlier, then recovers over a couple of hundred ms (the bloom).
        const float invRail = 1.0f + sagK_ * sagEnv_;
        float y = tubeP_(pdrive * s * invRail) / invRail;
        const float ay = std::fabs(y);
        sagEnv_ += (ay > sagEnv_ ? sagAtt_ : sagRel_) * (ay - sagEnv_);
        return dcP_.process(y * pcomp);
    }

    float compTarget() const
    {
        if (compAmt_ < 0.005f) return 1.0f;
        const float lvl = gainToDb(compEnv_ + 1.0e-9f);
        return dbToGain(compGr(lvl) + compMakeup_);
    }
    // Gain change (dB, <= 0) for a peak level, with a soft knee.
    float compGr(float lvlDb) const
    {
        const float over = lvlDb - compThr_, knee = 8.0f;
        const float slope = 1.0f / compRatio_ - 1.0f;
        if (over <= -0.5f * knee) return 0.0f;
        if (over < 0.5f * knee) return slope * (over + 0.5f * knee) * (over + 0.5f * knee) / (2.0f * knee);
        return slope * over;
    }

    float tremolo(float x)
    {
        const float depth = tremDepth_.step(tremDepthT_, glide_);
        tremPhase_ += tremInc_;
        if (tremPhase_ >= 1.0) tremPhase_ -= std::floor(tremPhase_);
        const float c = fastSin01((float)tremPhase_ + 0.25f);   // +1 on the beat: full volume there
        switch (tremMode_)
        {
            case kOpto:
            {
                // A lamp and light cell: a squarer pulse that switches on quickly and lets go slowly.
                float o = 0.5f * (1.0f - c);
                o = o * o * (3.0f - 2.0f * o);
                o = o * o * (3.0f - 2.0f * o);
                optoEnv_ += (o > optoEnv_ ? optoAtt_ : optoRel_) * (o - optoEnv_);
                return x * (1.0f - depth * optoEnv_);
            }
            case kHarmonic:
            {
                // Lows and highs swell in opposite phase: the level barely moves, the tone swirls.
                const float lo = harmLp_.process(x), hi = x - lo;
                return lo * (1.0f - depth * 0.5f * (1.0f - c)) + hi * (1.0f - depth * 0.5f * (1.0f + c));
            }
            default:
                return x * (1.0f - depth * 0.5f * (1.0f - c));
        }
    }

    float cabinet(float x)
    {
        // Floor bounce and (open backs) the wall behind: two short reflections that comb the sound
        // more as the mic moves back.
        cabBuf_[cabW_] = x;
        const float y = x + floorLevel_ * readCab(floorDelay_) + backLevel_ * readCab(backDelay_);
        cabW_ = (cabW_ + 1) & (kCabDelaySize - 1);
        float s = y;
        for (auto& f : cabF_) s = f.process(s);
        return s * cabGain_;
    }
    float readCab(float d) const
    {
        const float rp = (float)cabW_ - d;
        const int i0 = (int)std::floor(rp);
        const float fr = rp - (float)i0;
        const float a = cabBuf_[i0 & (kCabDelaySize - 1)], b = cabBuf_[(i0 + 1) & (kCabDelaySize - 1)];
        return a + fr * (b - a);
    }

    // Tremolo phase: locked to the song while a synced tremolo plays, free-running otherwise.
    void clockTremolo(const mpc::ProcessContext& ctx)
    {
        const float rate = get(kTremRate);
        if (getInt(kTremSync) == 1)
        {
            const float beats = kSyncDivs[syncIndex(rate)].beats;
            tremInc_ = (double)beatsToHz(beats, ctx.transport.bpm) / sr_;
            if (ctx.transport.playing && ctx.transport.ppqValid)
            {
                double c = ctx.transport.ppqPos / (double)beats;
                tremPhase_ = c - std::floor(c);
            }
        }
        else
            tremInc_ = (double)freeHz(rate) / sr_;
    }

    // ---- per-block control updates: coefficients only when something moved ----
    void updateControls(int n)
    {
        // Knob glide for the controls that set filter coefficients (about 30 ms).
        const float k = snapControls_ ? 1.0f : 1.0f - std::exp(-(float)n / (0.03f * sr_));
        auto glideKnob = [&](float& s, float target) { s += k * (target - s); if (std::fabs(target - s) < 1.0e-4f) s = target; };

        const int voice = getInt(kVoice);
        const int cab = getInt(kCab);
        const bool brightOn = getBool(kBright);
        glideKnob(kGain_, get(kGain) * 0.1f);
        glideKnob(kBass_, get(kBass) * 0.1f);
        glideKnob(kMid_, get(kMiddle) * 0.1f);
        glideKnob(kTreb_, get(kTreble) * 0.1f);
        glideKnob(kChime_, get(kChime) * 0.1f);
        glideKnob(kMic_, get(kMic) * 0.01f);
        glideKnob(kDist_, get(kDistance));
        glideKnob(kDwell_, get(kDwell) * 0.1f);
        glideKnob(kRevTone_, get(kRevTone) * 0.1f);

        const bool voiceChanged = voice != voice_ || snapControls_;
        voice_ = voice;
        const VoiceDef& vd = kVoices[voice_];
        if (voiceChanged)
        {
            tube1_.set(vd.asym1);
            tube2_.set(vd.asym2);
            tubeP_.set(vd.powerAsym);
            lp1_.set(vd.stageLpHz, sr2_);
            makeup_ = dbToGain(vd.makeupDb);
        }

        // Tone stack.
        if (voiceChanged || kBass_ != tsB_ || kMid_ != tsM_ || kTreb_ != tsT_)
        {
            tsB_ = kBass_; tsM_ = kMid_; tsT_ = kTreb_;
            stack_.set(vd.stack, (double)tsT_, (double)tsM_, bw::audioTaper((double)tsB_), (double)sr2_);
        }

        // Gain and Bright: the bright cap passes treble around the volume pot, so its boost is
        // biggest with the volume low and gone at full.
        const float gainDb = vd.gainLoDb + (vd.gainHiDb - vd.gainLoDb) * kGain_;
        gainT_ = dbToGain(gainDb);
        postT_ = 1.0f / std::pow(gainT_, 0.6f);
        if (voiceChanged || kGain_ != brG_ || brightOn != brOn_)
        {
            brG_ = kGain_; brOn_ = brightOn;
            const float boost = brightOn ? 9.0f * std::pow(1.0f - kGain_, 1.3f) : 0.0f;
            bright_.setCoeffs(BiquadCoeffs::highShelf(sr_, 2200.0f, 0.7f, boost));
        }

        // Chime.
        if (voiceChanged || kChime_ != chimeSet_)
        {
            chimeSet_ = kChime_;
            presence_.setCoeffs(BiquadCoeffs::peak(sr2_, vd.presenceHz, 0.8f, 7.0f * kChime_));
            shimAmt_ = 0.9f * kChime_ * kChime_;
        }

        // Power stage.
        const float master = get(kMaster) * 0.1f;
        pDriveT_ = dbToGain(-12.0f + 24.0f * master);
        pCompT_ = 1.0f / std::pow(pDriveT_, 0.7f);
        sagK_ = get(kSag) * 0.1f * vd.sagScale * 3.0f;

        // Compressor.
        compAmt_ = get(kComp) * 0.01f;
        compThr_ = -14.0f - 16.0f * compAmt_;
        compRatio_ = 1.0f + 3.0f * compAmt_;
        compMakeup_ = -0.8f * compGr(-14.0f);

        // Cabinet.
        if (cab != cab_ || kMic_ != cabMic_ || kDist_ != cabDist_ || snapControls_)
        {
            cab_ = cab; cabMic_ = kMic_; cabDist_ = kDist_;
            if (cab_ != kDirect) setCab();
        }

        // Spring.
        if (kDwell_ != spDwell_ || kRevTone_ != spTone_ || snapControls_)
        {
            spDwell_ = kDwell_; spTone_ = kRevTone_;
            spring_.set(spDwell_, spTone_);
        }
        const float mix = get(kRevMix) * 0.01f;
        revMixT_ = mix;
        revDryT_ = 1.0f - 0.45f * mix * mix;
        revWetT_ = kSpringWet * mix;

        // Tremolo.
        tremMode_ = getInt(kTremMode);
        tremDepthT_ = get(kTremDepth) * 0.01f;

        // Gains.
        inGainT_ = dbToGain(get(kInput));
        outGainT_ = dbToGain(get(kOutput));

        if (snapControls_)
        {
            inGain_.v = inGainT_; gain_.v = gainT_; post_.v = postT_; pDrive_.v = pDriveT_; pComp_.v = pCompT_;
            outGain_.v = outGainT_; revDry_.v = revDryT_; revWet_.v = revWetT_; tremDepth_.v = tremDepthT_;
        }
    }

    void setCab()
    {
        const CabDef& c = kCabs[cab_];
        const float m = kMic_;                     // 0 centre .. 1 edge
        const float dm = kDist_ * 0.01f;           // metres
        const float close = 1.0f - mpc::clamp((kDist_ - 2.0f) / 48.0f, 0.0f, 1.0f);   // 1 at 2 cm, 0 past 50 cm

        // Mic: on the dust cap (centre) the presence peak is strong and the top extends; towards the
        // edge of the cone it fades, the top closes in and the low mids fill out.
        const float presDb = c.presDb * (1.4f - 1.1f * m);
        const float presHz = c.presHz * (1.2f - 0.4f * m);
        const float lpHz = c.lpHz * (1.25f - 0.6f * m) * (1.0f - 0.15f * (1.0f - close));
        const float lowDb = c.lowDb + 2.0f * m;
        const float proxDb = 7.0f * close * close;    // proximity effect: a directional mic up close

        cabF_[0].setCoeffs(BiquadCoeffs::highpass(sr_, c.hpHz, c.hpQ));
        cabF_[1].setCoeffs(BiquadCoeffs::lowShelf(sr_, 220.0f, 0.7f, proxDb));
        cabF_[2].setCoeffs(BiquadCoeffs::peak(sr_, c.lowHz, 0.8f, lowDb));
        cabF_[3].setCoeffs(BiquadCoeffs::peak(sr_, c.midHz, c.midQ, c.midDb));
        cabF_[4].setCoeffs(BiquadCoeffs::peak(sr_, presHz, c.presQ, presDb));
        cabF_[5].setCoeffs(BiquadCoeffs::lowpass(sr_, lpHz, 0.7f));
        cabF_[6].setCoeffs(BiquadCoeffs::lowpass(sr_, lpHz * 1.15f, 1.1f));
        cabF_[7].setCoeffs(BiquadCoeffs::lowpass(sr_, std::fmin(lpHz * 1.4f, 0.45f * sr_), 0.6f));

        // Floor bounce: mic 0.6 m above the floor. Path difference and level from the geometry.
        const float h2 = 1.2f;
        const float path = std::sqrt(dm * dm + h2 * h2);
        floorDelay_ = mpc::clamp((path - dm) / 343.0f * sr_, 1.0f, (float)kCabDelaySize - 4.0f);
        floorLevel_ = 0.6f * dm / path;
        // Open back: the wall behind the amp, about 4 ms further, louder in the room mix.
        backDelay_ = mpc::clamp((0.0042f + 0.002f * dm) * sr_, 1.0f, (float)kCabDelaySize - 4.0f);
        backLevel_ = c.backLevel * (0.35f + 0.65f * mpc::clamp(dm * 2.0f, 0.0f, 1.0f));
        cabGain_ = 1.0f / (1.0f + 0.5f * (floorLevel_ + backLevel_)) / dbToGain(0.5f * proxDb);
    }

    static constexpr float kSpringWet = 1.0f;   // wet normalisation: at Dwell 5, Mix 50% sits the tail ~6 dB under the dry

    // ---- state ----
    float sr_ = 44100.0f, sr2_ = 88200.0f;
    bw::HalfbandTaps hb_;
    bw::Upsampler2x up_;
    bw::Downsampler2x down_;
    bw::Tube tube1_, tube2_, tubeP_;
    bw::OnePoleLP lp1_;
    bw::OnePoleHP inHp_, dc1_, dc2_, dcP_, shimHp_;
    bw::ToneStack stack_;
    Biquad bright_, presence_, harmLp_;
    Biquad cabF_[8];
    float cabBuf_[kCabDelaySize] = {};
    int cabW_ = 0;
    float floorDelay_ = 10.0f, floorLevel_ = 0.0f, backDelay_ = 180.0f, backLevel_ = 0.0f, cabGain_ = 1.0f;
    bw::SpringReverb spring_;
    float mono_[bw::SpringReverb::kBlock] = {}, wetL_[bw::SpringReverb::kBlock] = {}, wetR_[bw::SpringReverb::kBlock] = {};
    bool springOn_ = false;

    // controls
    bool snapControls_ = false;
    int voice_ = -1, cab_ = -1, tremMode_ = 0;
    float kGain_ = 0, kBass_ = 0, kMid_ = 0, kTreb_ = 0, kChime_ = 0, kMic_ = 0, kDist_ = 15, kDwell_ = 0, kRevTone_ = 0;
    float tsB_ = -1, tsM_ = -1, tsT_ = -1, brG_ = -1, chimeSet_ = -1, cabMic_ = -1, cabDist_ = -1, spDwell_ = -1, spTone_ = -1;
    bool brOn_ = false;
    float makeup_ = 1.0f, shimAmt_ = 0.0f, sagK_ = 0.0f;
    float gainT_ = 1, postT_ = 1, pDriveT_ = 1, pCompT_ = 1, inGainT_ = 1, outGainT_ = 1;
    float revMixT_ = 0, revDryT_ = 1, revWetT_ = 0, tremDepthT_ = 0;
    Glide gain_, post_, pDrive_, pComp_, inGain_, outGain_, revDry_, revWet_, tremDepth_;
    float glide_ = 0.01f;

    // compressor
    float compAmt_ = 0, compThr_ = -14, compRatio_ = 1, compMakeup_ = 0;
    float compEnv_ = 0, compG_ = 1, compStep_ = 0, compAtt_ = 0.1f, compRel_ = 0.001f;
    int compCount_ = 0;

    // power stage
    float sagEnv_ = 0, sagAtt_ = 0.01f, sagRel_ = 0.0001f;

    // tremolo
    double tremPhase_ = 0.0, tremInc_ = 0.0;
    float optoEnv_ = 0, optoAtt_ = 0.1f, optoRel_ = 0.01f;
};

MPC_PLUGIN_ENTRY(Breakwater)
