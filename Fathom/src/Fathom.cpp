// Fathom - a filter that breathes with the music, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// Started as an auto-wah that does not just "quack" on each pick stroke: a slow follower swells the filter open
// while you dig in and lets it sink back when you ease off. It grew into two filters and three movers.
//
// Signal flow (per channel):
//
//   in ─┬──────────────────────────────────────────────────────────────────────────┐ dry
//       └─► DRIVE ─► FILTER A ─► FILTER B (Series)  ─────────────────────► soft knee ─► mix ┴─► Output ─► out
//                    ├─ FILTER A ─► A Pan ─┐
//                    └─ FILTER B ─► B Pan ─┴─ (Parallel: averaged) ─┘
//
//   Movers (all stereo-linked, all measured on the dry input):
//     FOLLOWER  how loud you are playing (RMS, dB-scaled), with its own attack and release. Unipolar 0..1.
//     LFO A     seven shapes, free-running in Hz or locked to the MPC's tempo and bar. Bipolar -1..1.
//     LFO B     moves filter B. Linked (the default) it IS LFO A; set to Own it has its own shape, sync and
//               rate, so the two filters can move in different rhythms. Spread and Fol>LFO apply to both.
//               Fol>LFO lets the follower set the LFOs' depth (play harder -> more movement).
//     ENVELOPE  fires on each new note (transient detector) and decays. The classic auto-wah quack. Unipolar.
//     BREATHE   not a mover of its own: it adds inertia (a glide on every filter movement, 5..600 ms) and a
//               slow drift (smooth random, ~11 s per wander) so the filter keeps moving on held notes.
//
//   Each filter's cutoff, in octaves:  log2(Freq) + Fol*Fol>X + LFO X*LFO>X + Env*Env>X + drift
//   then glided by Breathe, converted to Hz and clamped to 20 Hz .. 0.45 * sample rate.
//
// Filters are the framework's Cytomic SVF (topology preserving, stable under fast modulation). Coefficients are
// recomputed every 16 samples (one tan() per filter per channel), all mover maths runs at that control rate too.
//   Wah       a resonant band-pass with a little low-pass under it, louder at the peak: the shape of a pedal wah.
//   Lowpass / Bandpass (unity gain at the peak) / Highpass / Notch.
//
// Parameters (ids are permanent once released - never rename or reorder; append new ones at the end):
//    0 Output      dB  -24..+12    0      level after everything (the host nudges parameter 0 on load)
//    1 Mix         %   0..100      100    dry / filtered
//    2 Drive       %   0..100      15     soft saturation before the filters (grit, and it evens out dynamics)
//    3 Breathe     %   0..100      40     inertia + slow drift on the cutoff (see above)
//    4 A Type      Wah, Lowpass, Bandpass, Highpass, Notch
//    5 A Freq      Hz  60..8000    380    resting cutoff; the movers push it up and down from here
//    6 A Reso      %   0..100      60
//    7 Routing     Series / Parallel      Series: A then B. Parallel: A and B side by side (two band-passes = vowels)
//    8 B Type      Off, Wah, Lowpass, Bandpass, Highpass, Notch
//    9 B Freq      Hz  60..12000   1500
//   10 B Reso      %   0..100      40
//   11 Fol Attack  ms  1..1000     150    how quickly the follower rises as you play louder
//   12 Fol Release ms  10..5000    1500   how slowly it sinks back when you play softer
//   13 Fol Gain    dB  -12..+36    12     input gain into the follower: raise it for quiet sources
//   14 Fol>LFO     %   -100..100   60     +: the louder you play, the deeper the LFO. -: the quieter, the deeper
//   15 LFO A Shape Sine, Triangle, Ramp Up, Ramp Down, Square, Steps (sample & hold), Drift (smooth random)
//   16 LFO A Sync  Free / Sync            Sync locks the LFO to the MPC's tempo and bar
//   17 LFO A Rate  0..1        2 bars     one control for both modes: 0.02..20 Hz, or 8 bars .. 1/32
//   18 Spread      deg 0..180      20     right channel's LFO and drift phase offset (stereo movement)
//   19 Env Sens    %   0..100      50     how easily a new note fires the envelope
//   20 Env Decay   ms  20..3000    300    how long the envelope takes to fall back
//   21 Fol>A       oct -4..+4      2.5    how far each mover pushes each filter; all bipolar, 0 = off
//   22 LFO>A       oct -4..+4      0.6    LFO A -> filter A
//   23 Env>A       oct -4..+4      0
//   24 Fol>B       oct -4..+4      0
//   25 LFO>B       oct -4..+4      0      LFO B (= LFO A when linked) -> filter B
//   26 Env>B       oct -4..+4      0
//   27 A Cutoff    meter (not saved, not automatable): where filter A is right now, 20 Hz..20 kHz on a log
//   28 B Cutoff    meter            scale (0..1). The skin draws the response curves from these.
//   29 A Pan       -100..100       0      where filter A sits, Parallel only (a balance that folds the far side
//   30 B Pan       -100..100       0      in, at constant power for a mono source). Series has one output: unused there.
//   31 LFO B       Link A / Own    Link A Link A: filter B follows LFO A (as before 1.1). Own: LFO B below.
//   32 LFO B Shape (as LFO A Shape)
//   33 LFO B Sync  Free / Sync     Sync
//   34 LFO B Rate  0..1            1 bar
//   35 Env Swell   ms  1..2000     1.5    how long the envelope takes to rise after a note: short = the quack,
//                                        long = the filter swells open like a slowly rocked pedal (1.2)
//
// CPU: four SVFs, one drive, a handful of one-pole followers per sample; one tan(), exp2() and log10() per
// filter per 16 samples. Well under the budget of the slowest MPC.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/smoother.h"
#include "dsp/tempo.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace mpc::dsp;

namespace {

enum ParamIndex
{
    kOutput = 0, kMix, kDrive, kBreathe,
    kAType, kAFreq, kARes, kRouting,
    kBType, kBFreq, kBRes,
    kFolAttack, kFolRelease, kFolGain, kFolLfo,
    kLfoShape, kLfoSync, kLfoRate, kSpread,
    kEnvSens, kEnvDecay,
    kFolA, kLfoA, kEnvA, kFolB, kLfoB, kEnvB,
    kMeterA, kMeterB,
    kAPan, kBPan, kLfoBMode, kLfoBShape, kLfoBSync, kLfoBRate,
    kEnvSwell,
    kNumParams
};

enum FilterType { kWah = 0, kLowpass, kBandpass, kHighpass, kNotch };   // B's menu has "Off" first, so B = index - 1
enum LfoShape { kSine = 0, kTriangle, kRampUp, kRampDown, kSquare, kSteps, kDrift };

constexpr int kCtrl = 16;   // control-rate interval in samples

// LFO Rate in Sync mode: slowest to fastest, so turning the knob up always speeds it up.
struct SyncDiv { const char* name; float beats; };
constexpr SyncDiv kSyncDivs[] = {
    {"8 bars", 32.0f}, {"4 bars", 16.0f}, {"2 bars", 8.0f}, {"1 bar", 4.0f}, {"1/2D", 3.0f}, {"1/2", 2.0f},
    {"1/4D", 1.5f}, {"1/4", 1.0f}, {"1/8D", 0.75f}, {"1/4T", 0.66667f}, {"1/8", 0.5f}, {"1/8T", 0.33333f},
    {"1/16", 0.25f}, {"1/16T", 0.16667f}, {"1/32", 0.125f}
};
constexpr int kNumSyncDivs = (int)(sizeof(kSyncDivs) / sizeof(kSyncDivs[0]));
inline int syncIndex(float rate01) { return mpc::clamp((int)std::lround(rate01 * (kNumSyncDivs - 1)), 0, kNumSyncDivs - 1); }
inline float syncPos(int index) { return (float)index / (float)(kNumSyncDivs - 1); }   // for presets
// Meters: 0..1 across 20 Hz..20 kHz (log), the same axis as the skin's response graph.
constexpr float kMeterLowOct = 4.321928f;    // log2(20)
constexpr float kMeterSpanOct = 9.965784f;   // log2(1000)
constexpr float kMeterSteps = 96.0f;         // written in steps this fine, so the host is not flooded
inline float meterHz(float v01) { return std::exp2(kMeterLowOct + kMeterSpanOct * v01); }

inline float freeHz(float rate01) { return 0.02f * std::pow(1000.0f, mpc::clamp(rate01, 0.0f, 1.0f)); }

// Deterministic random value in -1..1 for cycle n. Hash-based so the left and right channels (at different
// phases) walk through the same sequence, and a synced LFO lands on the same values each time the song plays.
inline float hashBipolar(int64_t n, uint32_t seed)
{
    uint32_t x = (uint32_t)n * 0x9E3779B1u ^ (uint32_t)(n >> 32) ^ seed;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
}
inline float smoothRandom(double cycles, uint32_t seed)
{
    const double fl = std::floor(cycles);
    const float t = (float)(cycles - fl);
    const float s = t * t * (3.0f - 2.0f * t);
    const int64_t n = (int64_t)fl;
    return lerp(hashBipolar(n, seed), hashBipolar(n + 1, seed), s);
}

// One LFO value at an absolute cycle position (phase = fractional part). All shapes start low at phase 0, so a
// synced LFO has the filter closed on the downbeat and opening from there.
inline float lfoShape(int shape, double cycles)
{
    const float p = (float)(cycles - std::floor(cycles));
    switch (shape)
    {
        case kSine:     return fastSin01(p - 0.25f);                          // -cos: bottom at phase 0
        case kTriangle: return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
        case kRampUp:   return 2.0f * p - 1.0f;
        case kRampDown: return 1.0f - 2.0f * p;
        case kSquare:   return p < 0.5f ? -1.0f : 1.0f;
        case kSteps:    return hashBipolar((int64_t)std::floor(cycles), 0x51ED270Bu);
        case kDrift:
        default:        return smoothRandom(cycles, 0x2545F491u);
    }
}

std::vector<std::string> shapeNames() { return {"Sine", "Triangle", "Ramp Up", "Ramp Down", "Square", "Steps", "Drift"}; }

// Soft knee above -3 dBFS so resonant peaks cannot clip harshly. Transparent below 0.7.
inline float softKnee(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.7f) return x;
    const float y = 0.7f + 0.3f * fastTanh((a - 0.7f) * (1.0f / 0.3f));
    return x < 0.0f ? -y : y;
}

// One filter slot: an SVF plus the output shape for its type.
struct FilterSlot
{
    SVF svf;
    int type = kWah;
    float k = 1.0f;   // 1/Q, matching what the SVF uses, for gain normalisation

    void set(float hz, float reso01)
    {
        float r;
        switch (type)
        {
            case kWah:   r = 0.35f + 0.60f * reso01; break;   // a wah is never flat: Q ~0.7 .. ~8
            default:     r = 0.95f * reso01; break;
        }
        svf.set(hz, r);
        k = 2.0f - 1.98f * clamp(r, 0.0f, 1.0f);
    }

    float process(float x)
    {
        float lp, bp, hp;
        svf.processAll(x, lp, bp, hp);
        switch (type)
        {
            case kWah:      return 1.6f * k * bp + 0.2f * lp;
            case kLowpass:  return lp;
            case kBandpass: return 1.4f * k * bp;
            case kHighpass: return hp;
            case kNotch:
            default:        return lp + hp;
        }
    }
};

} // namespace

class Fathom : public mpc::Plugin
{
public:
    Fathom() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("output",   "Output",      -24.0f, 12.0f,   0.0f,    "dB", 1));
        addParam(mpc::floatParam ("mix",      "Mix",         0.0f,   100.0f,  100.0f,  "%",  0));
        addParam(mpc::floatParam ("drive",    "Drive",       0.0f,   100.0f,  15.0f,   "%",  0));
        addParam(mpc::floatParam ("breathe",  "Breathe",     0.0f,   100.0f,  40.0f,   "%",  0));
        addParam(mpc::choiceParam("a_type",   "A Type",      {"Wah", "Lowpass", "Bandpass", "Highpass", "Notch"}, kWah));
        addParam(mpc::logParam   ("a_freq",   "A Freq",      60.0f,  8000.0f, 380.0f,  "", 0));   // unit from formatValue
        addParam(mpc::floatParam ("a_reso",   "A Reso",      0.0f,   100.0f,  60.0f,   "%",  0));
        addParam(mpc::choiceParam("routing",  "Routing",     {"Series", "Parallel"}, 0));
        addParam(mpc::choiceParam("b_type",   "B Type",      {"Off", "Wah", "Lowpass", "Bandpass", "Highpass", "Notch"}, 0));
        addParam(mpc::logParam   ("b_freq",   "B Freq",      60.0f,  12000.0f, 1500.0f, "", 0));
        addParam(mpc::floatParam ("b_reso",   "B Reso",      0.0f,   100.0f,  40.0f,   "%",  0));
        addParam(mpc::logParam   ("fol_att",  "Fol Attack",  1.0f,   1000.0f, 150.0f,  "ms", 0));
        addParam(mpc::logParam   ("fol_rel",  "Fol Release", 10.0f,  5000.0f, 1500.0f, "ms", 0));
        addParam(mpc::floatParam ("fol_gain", "Fol Gain",    -12.0f, 36.0f,   12.0f,   "dB", 1));
        addParam(mpc::floatParam ("fol_lfo",  "Fol>LFO",     -100.0f, 100.0f, 60.0f,   "",   0));
        addParam(mpc::choiceParam("lfo_shape","LFO A Shape", shapeNames(), kSine));
        addParam(mpc::choiceParam("lfo_sync", "LFO A Sync",  {"Free", "Sync"}, 1));
        addParam(mpc::floatParam ("lfo_rate", "LFO A Rate",  0.0f,   1.0f,    syncPos(2), "", 3));
        addParam(mpc::floatParam ("spread",   "Spread",      0.0f,   180.0f,  20.0f,   "deg", 0));
        addParam(mpc::floatParam ("env_sens", "Env Sens",    0.0f,   100.0f,  50.0f,   "%",  0));
        addParam(mpc::logParam   ("env_dec",  "Env Decay",   20.0f,  3000.0f, 300.0f,  "ms", 0));
        addParam(mpc::floatParam ("fol_a",    "Fol>A",       -4.0f,  4.0f,    2.5f,    "", 1));
        addParam(mpc::floatParam ("lfo_a",    "LFO>A",       -4.0f,  4.0f,    0.6f,    "", 1));
        addParam(mpc::floatParam ("env_a",    "Env>A",       -4.0f,  4.0f,    0.0f,    "", 1));
        addParam(mpc::floatParam ("fol_b",    "Fol>B",       -4.0f,  4.0f,    0.0f,    "", 1));
        addParam(mpc::floatParam ("lfo_b",    "LFO>B",       -4.0f,  4.0f,    0.0f,    "", 1));
        addParam(mpc::floatParam ("env_b",    "Env>B",       -4.0f,  4.0f,    0.0f,    "", 1));
        auto meter = [this](mpc::Param p) { p.automatable = false; p.saved = false; addParam(p); };
        meter(mpc::floatParam("a_now", "A Cutoff", 0.0f, 1.0f, 0.4f, "", 3));
        meter(mpc::floatParam("b_now", "B Cutoff", 0.0f, 1.0f, 0.6f, "", 3));
        // 1.1: pans and a second LFO, appended so nothing above moves.
        addParam(mpc::floatParam ("a_pan",     "A Pan",       -100.0f, 100.0f, 0.0f, "", 0));   // text from formatValue
        addParam(mpc::floatParam ("b_pan",     "B Pan",       -100.0f, 100.0f, 0.0f, "", 0));
        addParam(mpc::choiceParam("lfo_b_mode","LFO B",       {"Link A", "Own"}, 0));
        addParam(mpc::choiceParam("lfo_b_shape","LFO B Shape", shapeNames(), kSine));
        addParam(mpc::choiceParam("lfo_b_sync","LFO B Sync",  {"Free", "Sync"}, 1));
        addParam(mpc::floatParam ("lfo_b_rate","LFO B Rate",  0.0f,   1.0f,    syncPos(3), "", 3));
        // 1.2: the envelope's rise time, appended.
        addParam(mpc::logParam   ("env_swell", "Env Swell",   1.0f,   2000.0f, 1.5f, "", 1));   // text from formatValue

        // Every program sets every parameter (a program that left the 1.1 ones out would inherit the previous
        // program's pans and LFO B). The tail: pans, LFO B mode / shape / sync / rate, Env Swell. Meters are written live.
        auto prog = [this](const char* name, std::vector<float> v, std::vector<float> tail)
        {
            v.push_back(0.4f); v.push_back(0.6f);                   // 27, 28 meters (overwritten at once)
            v.insert(v.end(), tail.begin(), tail.end());
            addProgram(name, v);
        };
        const std::vector<float> centred = {0, 0, 0, kSine, 1, syncPos(3), 1.5f};   // no pan, LFO B linked, snappy envelope
        //                     Out  Mix Drv Brth  AType  AFreq ARes Rt BType BFreq BRes  FAtt FRel  FGain FLfo  Shape     Sync Rate         Sprd ESens EDec  FA    LA    EA   FB    LB    EB
        prog("Breathing Wah",   {0,  100, 15, 40,  kWah,  380,  60,  0, 0,    1500, 40,   150, 1500, 12,   60,   kSine,    1,   syncPos(2),  20,  50,   300,  2.5f, 0.6f, 0,   0,    0,    0}, centred);
        prog("Classic Quack",   {0,  100, 10, 0,   kWah,  350,  70,  0, 0,    1500, 40,   3,   120,  12,   0,    kSine,    1,   syncPos(3),  0,   60,   180,  0.6f, 0,    2.8f,0,    0,    0}, centred);
        prog("Slow Tide",       {0,  100, 20, 70,  kLowpass, 300, 45, 0, 0,   1500, 40,   300, 3000, 12,   0,    kSine,    1,   syncPos(0),  90,  50,   300,  1.0f, 2.0f, 0,   0,    0,    0}, centred);
        prog("Tempo Wobble",    {0,  100, 25, 10,  kLowpass, 500, 65, 0, 0,   1500, 40,   150, 1500, 12,   0,    kSine,    1,   syncPos(10), 0,   50,   300,  0,    2.5f, 0,   0,    0,    0}, centred);
        prog("Vowel Talk",      {0,  100, 15, 30,  kBandpass, 600, 70, 1, 3,  1700, 70,   80,  900,  12,   30,   kDrift,   1,   syncPos(3),  30,  50,   300,  1.2f, 0.3f, 0,   0.7f, -0.6f,0}, centred);
        prog("Random Steps",    {0,  100, 15, 5,   kBandpass, 900, 60, 0, 0,  1500, 40,   150, 1500, 12,   0,    kSteps,   1,   syncPos(12), 60,  50,   300,  0,    1.5f, 0,   0,    0,    0}, centred);
        prog("Notch Sweep",     {0,  100, 0,  25,  kNotch, 800,  30,  0, 0,   1500, 40,   150, 1500, 12,   0,    kTriangle,1,   syncPos(3),  90,  50,   300,  0,    2.0f, 0,   0,    0,    0}, centred);
        prog("Smoky Room",      {0,  100, 35, 55,  kWah,  320,  45,  0, 2,    2500, 20,   250, 2500, 12,   80,   kDrift,   1,   syncPos(1),  45,  50,   300,  1.8f, 0.8f, 0,   0.5f, 0,    0}, centred);
        //                     ...                                                                                                           APan  BPan  LfoB Shape  Sync Rate  Swell
        prog("Twin Tides",      {0,  100, 15, 35,  kWah,  420,  60,  1, 1,    700,  55,   150, 1500, 12,   50,   kSine,    1,   syncPos(3),  0,   50,   300,  2.0f, 0.8f, 0,   1.6f, 0.8f, 0},  {-80, 80, 1, kSine, 1, syncPos(4), 1.5f});
        prog("Ping-Pong Talk",  {0,  100, 15, 10,  kBandpass, 600, 70, 1, 3,  1500, 70,   100, 1000, 12,   20,   kTriangle,1,   syncPos(7),  0,   50,   300,  0.8f, 1.5f, 0,   0.6f, -1.5f,0},  {-100, 100, 1, kTriangle, 1, syncPos(8), 1.5f});

        // 1.2
        const float hz01 = 0.233f, hz015 = 0.292f, hz013 = 0.271f, hz021 = 0.340f;   // free rates: 0.10, 0.15, 0.13, 0.21 Hz
        //                     Out  Mix Drv Brth  AType  AFreq ARes Rt BType BFreq BRes  FAtt FRel  FGain FLfo  Shape     Sync Rate         Sprd ESens EDec  FA    LA    EA   FB    LB    EB
        prog("Foot Rock",       {0,  100, 15, 20,  kWah,  400,  65,  0, 0,    1500, 40,   150, 1500, 12,   0,    kSine,    1,   syncPos(3),  0,   55,   900,  0.8f, 0,    2.2f,0,    0,    0},  {0, 0, 0, kSine, 1, syncPos(3), 350});
        prog("Parked Wah",      {0,  100, 20, 15,  kWah,  750,  85,  0, 0,    1500, 40,   150, 1500, 12,   0,    kDrift,   0,   hz01,        30,  50,   300,  0.3f, 0.25f,0,   0,    0,    0}, centred);
        prog("Bloom",           {0,  100, 10, 10,  kLowpass, 2500, 35, 0, 0,  1500, 40,   150, 1500, 12,   0,    kSine,    1,   syncPos(3),  0,   55,   1200, 0,    0,   -2.5f,0,    0,    0}, centred);
        prog("Duck & Glow",     {0,  100, 10, 40,  kLowpass, 1800, 40, 0, 0,  1500, 40,   60,  2500, 12,   0,    kSine,    1,   syncPos(2),  30,  50,   300, -2.0f, 0.3f, 0,   0,    0,    0}, centred);
        prog("Deep Water",      {0,  100, 10, 100, kLowpass, 280, 55, 0, 2,   700,  10,   300, 3000, 12,   0,    kDrift,   0,   hz015,       120, 50,   300,  0.8f, 1.2f, 0,   0.4f, 0.6f, 0}, centred);
        prog("Filter Pump",     {0,  100, 20, 0,   kLowpass, 1200, 30, 0, 0,  1500, 40,   150, 1500, 12,   0,    kRampUp,  1,   syncPos(7),  0,   50,   300,  0,    2.0f, 0,   0,    0,    0}, centred);
        prog("Wide Phase",      {0,  100, 0,  20,  kNotch, 600,  30,  1, 5,   1400, 30,   150, 1500, 12,   0,    kTriangle,0,   hz013,       0,   50,   300,  0,    2.0f, 0,   0,    2.0f, 0},  {-70, 70, 1, kTriangle, 0, hz021, 1.5f});
        prog("Funk Bass",       {-1, 100, 20, 0,   kLowpass, 180, 70, 0, 0,   1500, 40,   20,  300,  12,   0,    kSine,    1,   syncPos(3),  0,   55,   220,  0.5f, 0,    2.6f,0,    0,    0}, centred);
        prog("Chop",            {0,  100, 0,  0,   kLowpass, 400, 40, 0, 0,   1500, 40,   150, 1500, 12,   0,    kSquare,  1,   syncPos(12), 0,   50,   300,  0,    2.5f, 0,   0,    0,    0}, centred);
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = (float)sampleRate;
        for (int c = 0; c < 2; ++c) { a_[c].svf.setSampleRate(sampleRate); b_[c].svf.setSampleRate(sampleRate); }
        powCoef_ = onePoleCoeff(10.0f, sampleRate);   // RMS pre-smoothing for the follower
        reset();
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c) { a_[c].svf.reset(); b_[c].svf.reset(); }
        power_ = 0.0f; peak_ = 0.0f; ctrlCount_ = 0;
        fol_ = 0.0f; fast_ = slow_ = 0.0f; env_ = 0.0f; envRising_ = false; riseFrom_ = riseT_ = 0.0f; holdoff_ = 0;
        lfoCycles_ = lfoCyclesB_ = 0.0; driftCycles_ = 0.0;
        panA_.snap(get(kAPan) * 0.01f);
        panB_.snap(get(kBPan) * 0.01f);
        glideSnap_ = true;
        meterWait_ = 0; lastMeterA_ = lastMeterB_ = -1.0f;
        mix_.snap(get(kMix) * 0.01f);
        out_.snap(dbToGain(get(kOutput)));
        drive_.snap(get(kDrive) * 0.01f);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const int nch = mpc::clamp(ctx.numOutputs, 0, 2);
        if (nch == 0) return;
        const bool haveIn = ctx.inputs != nullptr && ctx.numInputs > 0;

        // ---- per-block parameter reads ----------------------------------------------------------------
        a_[0].type = a_[1].type = getInt(kAType);
        const int bSel = getInt(kBType);
        const bool bOn = bSel > 0;
        b_[0].type = b_[1].type = bOn ? bSel - 1 : kLowpass;
        const bool parallel = getInt(kRouting) == 1;

        mix_.setTarget(get(kMix) * 0.01f, n);
        out_.setTarget(dbToGain(get(kOutput)), n);
        drive_.setTarget(get(kDrive) * 0.01f, n);
        panA_.setTarget(get(kAPan) * 0.01f, n);
        panB_.setTarget(get(kBPan) * 0.01f, n);
        const bool panned = parallel && nch == 2;   // B Off in Parallel: A alone, still placed by A Pan

        updateLfoClock(ctx);

        const float* inL = haveIn ? ctx.inputs[0] : nullptr;
        const float* inR = haveIn ? ctx.inputs[ctx.numInputs > 1 ? 1 : 0] : nullptr;
        float* outL = ctx.outputs[0];
        float* outR = nch > 1 ? ctx.outputs[1] : nullptr;

        for (int i = 0; i < n; ++i)
        {
            if (ctrlCount_ == 0) controlTick();
            if (++ctrlCount_ >= kCtrl) ctrlCount_ = 0;

            const float xl = (inL != nullptr && inL[i] == inL[i]) ? inL[i] : 0.0f;
            const float xr = (inR != nullptr && inR[i] == inR[i]) ? inR[i] : xl;

            // Movers listen to the dry input.
            const float p = 0.5f * (xl * xl + xr * xr);
            power_ += powCoef_ * (p - power_);
            const float pk = std::fmax(std::fabs(xl), std::fabs(xr));
            if (pk > peak_) peak_ = pk;

            const float mix = mix_.next();
            const float og = out_.next();
            const float dr = drive_.next();
            const float g = 1.0f + 9.0f * dr * dr;            // 1 .. 10
            const float comp = 1.0f / (0.4f + 0.6f * g);

            const float pa = panA_.next(), pb = panB_.next();
            const float x[2] = { xl, xr };
            float wet[2] = { 0.0f, 0.0f };
            if (panned)
            {
                float ya[2], yb[2];
                for (int c = 0; c < 2; ++c)
                {
                    const float s = dr > 0.002f ? fastTanh(x[c] * g) * comp : x[c];
                    ya[c] = a_[c].process(s);
                    yb[c] = bOn ? b_[c].process(s) : 0.0f;
                }
                pan(ya, pa);
                if (bOn) pan(yb, pb);
                const float sum = bOn ? 0.5f : 1.0f;
                wet[0] = sum * (ya[0] + yb[0]);
                wet[1] = sum * (ya[1] + yb[1]);
            }
            else
            {
                for (int c = 0; c < nch; ++c)
                {
                    const float s = dr > 0.002f ? fastTanh(x[c] * g) * comp : x[c];
                    if (!bOn)          wet[c] = a_[c].process(s);
                    else if (parallel) wet[c] = 0.5f * (a_[c].process(s) + b_[c].process(s));
                    else               wet[c] = b_[c].process(a_[c].process(s));
                }
            }
            for (int c = 0; c < nch; ++c)
            {
                float y = softKnee(wet[c]);
                y = x[c] + (y - x[c]) * mix;
                y *= og;
                if (!std::isfinite(y)) { y = 0.0f; a_[c].svf.reset(); b_[c].svf.reset(); }
                (c == 0 ? outL : outR)[i] = y;
            }
        }

        // Meters, at most every ~30 ms and only when they have moved a step.
        meterWait_ -= n;
        if (meterWait_ <= 0)
        {
            meterWait_ = (int)(0.03f * sr_);
            sendMeter(kMeterA, octA_[0], lastMeterA_);
            sendMeter(kMeterB, octB_[0], lastMeterB_);
        }
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case kMeterA: case kMeterB:
                v = meterHz(v);
                [[fallthrough]];
            case kAFreq: case kBFreq:
                if (v >= 1000.0f) std::snprintf(b, sizeof(b), "%.2f kHz", (double)(v * 0.001f));
                else              std::snprintf(b, sizeof(b), "%.0f Hz", (double)v);
                out = b; return true;
            case kLfoRate: case kLfoBRate:
                if (getInt(index == kLfoRate ? kLfoSync : kLfoBSync) == 1) out = kSyncDivs[syncIndex(v)].name;
                else { const float hz = freeHz(v); std::snprintf(b, sizeof(b), hz < 10.0f ? "%.2f Hz" : "%.1f Hz", (double)hz); out = b; }
                return true;
            case kFolA: case kLfoA: case kEnvA: case kFolB: case kLfoB: case kEnvB:
                if (std::fabs(v) < 0.05f) out = "0 oct";
                else { std::snprintf(b, sizeof(b), "%+.1f oct", (double)v); out = b; }
                return true;
            case kAPan: case kBPan:
                if (std::fabs(v) < 0.5f) out = "C";
                else { std::snprintf(b, sizeof(b), "%s %.0f", v < 0.0f ? "L" : "R", (double)std::fabs(v)); out = b; }
                return true;
            case kEnvSwell:
                if (v >= 1000.0f)   std::snprintf(b, sizeof(b), "%.2f s", (double)(v * 0.001f));
                else if (v < 10.0f) std::snprintf(b, sizeof(b), "%.1f ms", (double)v);
                else                std::snprintf(b, sizeof(b), "%.0f ms", (double)v);
                out = b; return true;
            case kFolLfo:
                if (std::fabs(v) < 0.5f) out = "0 %";
                else { std::snprintf(b, sizeof(b), "%+.0f %%", (double)v); out = b; }
                return true;
            default: return false;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Fathom";
        info.vendor       = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1200;           // 1.2.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 1;
        return info;
    }
    static constexpr char kUid[5] = "GpBV";

    void sendMeter(int index, float oct, float& last)
    {
        const float v = std::round(clamp((oct - kMeterLowOct) / kMeterSpanOct, 0.0f, 1.0f) * kMeterSteps) / kMeterSteps;
        if (v == last) return;
        last = v;
        setNormalised(index, v);
        notifyHostParameterChanged(index);
    }

    // Balance a stereo pair: the far side is folded into the near one, so a mono source keeps constant power
    // (hard left = L + 0.414 R, at sqrt(2) of the centred level) and a stereo one keeps its image until pushed.
    static void pan(float* y, float p)
    {
        const float a = std::fabs(p);
        if (a < 1.0e-4f) return;
        if (p < 0.0f) { y[0] += 0.41421f * a * y[1]; y[1] *= 1.0f - a; }
        else          { y[1] += 0.41421f * a * y[0]; y[0] *= 1.0f - a; }
    }

    // Where an LFO is: locked to the song position while a synced LFO plays, free-running otherwise.
    void clockLfo(const mpc::ProcessContext& ctx, int syncParam, int rateParam, double& cycles, double& inc)
    {
        const float rate = get(rateParam);
        if (getInt(syncParam) == 1)
        {
            const float beats = kSyncDivs[syncIndex(rate)].beats;
            inc = (double)beatsToHz(beats, ctx.transport.bpm) / sr_;
            if (ctx.transport.playing && ctx.transport.ppqValid)
            {
                cycles = ctx.transport.ppqPos / (double)beats;
                ctrlCount_ = 0;   // start a control tick on the block boundary so the phase lands exactly
            }
        }
        else
            inc = (double)freeHz(rate) / sr_;
    }
    void updateLfoClock(const mpc::ProcessContext& ctx)
    {
        clockLfo(ctx, kLfoSync, kLfoRate, lfoCycles_, lfoInc_);
        clockLfo(ctx, kLfoBSync, kLfoBRate, lfoCyclesB_, lfoIncB_);
    }

    // Everything that moves the filters, once every kCtrl samples.
    void controlTick()
    {
        const float dt = (float)kCtrl;

        // Follower: RMS -> dB -> 0..1 over a 36 dB window (soft vs hard playing is ~15-20 dB), then attack / release.
        const float amp = std::sqrt(power_ > 0.0f ? power_ : 0.0f);
        const float db = gainToDb(amp, -120.0f) + get(kFolGain);
        const float target = clamp((db + 36.0f) * (1.0f / 36.0f), 0.0f, 1.0f);
        const float att = 1.0f - std::exp(-dt / (get(kFolAttack) * 0.001f * sr_));
        const float rel = 1.0f - std::exp(-dt / (get(kFolRelease) * 0.001f * sr_));
        fol_ += (target > fol_ ? att : rel) * (target - fol_);

        // Transient envelope: a fast peak follower jumping well above a slow one means a new note.
        const float pk = peak_; peak_ = 0.0f;
        const float sens = get(kEnvSens) * 0.01f;
        fast_ += (pk > fast_ ? 0.7f : 0.06f) * (pk - fast_);
        slow_ += (fast_ > slow_ ? 0.012f : 0.004f) * (fast_ - slow_);
        const float ratio = 1.3f + 2.7f * (1.0f - sens);
        const float floorAmp = dbToGain(-54.0f + 24.0f * (1.0f - sens));
        if (holdoff_ > 0) --holdoff_;
        if (holdoff_ == 0 && fast_ > floorAmp && fast_ > slow_ * ratio)
        {
            if (!envRising_) { envRising_ = true; riseFrom_ = env_; riseT_ = 0.0f; }   // a note mid-swell lets it carry on
            holdoff_ = (int)(0.06f * sr_ / dt);   // 60 ms before it can fire again
        }
        if (envRising_)
        {
            // Rise over Env Swell along a smoothstep, from wherever the decay had got to: a long swell eases in
            // and out like a foot on a pedal; the default 1.5 ms is the quack.
            riseT_ += dt / (get(kEnvSwell) * 0.001f * sr_);
            if (riseT_ >= 1.0f) { env_ = 1.0f; envRising_ = false; }
            else env_ = riseFrom_ + (1.0f - riseFrom_) * riseT_ * riseT_ * (3.0f - 2.0f * riseT_);
        }
        else
            env_ *= std::exp(-dt * 3.0f / (get(kEnvDecay) * 0.001f * sr_));   // ~5% left after Env Decay

        // LFO, its depth set partly by the follower.
        const int shape = getInt(kLfoShape);
        const double phaseR = get(kSpread) / 360.0f;
        const float lfoL = lfoShape(shape, lfoCycles_);
        const float lfoR = lfoShape(shape, lfoCycles_ + phaseR);
        lfoCycles_ += lfoInc_ * kCtrl;
        if (lfoCycles_ > 1.0e7) lfoCycles_ -= 1.0e7;
        float lfoBL = lfoL, lfoBR = lfoR;
        if (getInt(kLfoBMode) == 1)
        {
            const int shapeB = getInt(kLfoBShape);
            lfoBL = lfoShape(shapeB, lfoCyclesB_);
            lfoBR = lfoShape(shapeB, lfoCyclesB_ + phaseR);
        }
        lfoCyclesB_ += lfoIncB_ * kCtrl;   // runs even while linked, so it is in time the moment it is switched on
        if (lfoCyclesB_ > 1.0e7) lfoCyclesB_ -= 1.0e7;
        const float fl = get(kFolLfo) * 0.01f, afl = std::fabs(fl);
        const float lfoDepth = (1.0f - afl) + afl * (fl > 0.0f ? fol_ : 1.0f - fol_);

        // Breathe: drift and glide.
        const float br = get(kBreathe) * 0.01f;
        const float driftL = 0.8f * br * smoothRandom(driftCycles_, 0x68E31DA4u);
        const float driftR = 0.8f * br * smoothRandom(driftCycles_ + phaseR, 0x68E31DA4u);
        driftCycles_ += 0.09 / sr_ * kCtrl;
        if (driftCycles_ > 1.0e7) driftCycles_ -= 1.0e7;
        const float glideMs = 5.0f + 600.0f * br * std::sqrt(br);
        const float glide = glideSnap_ ? 1.0f : 1.0f - std::exp(-dt / (glideMs * 0.001f * sr_));
        glideSnap_ = false;

        const float lfo[2] = { lfoL * lfoDepth, lfoR * lfoDepth };
        const float lfoB[2] = { lfoBL * lfoDepth, lfoBR * lfoDepth };
        const float drift[2] = { driftL, driftR };
        const float baseA = std::log2(get(kAFreq)), baseB = std::log2(get(kBFreq));
        const float resA = get(kARes) * 0.01f, resB = get(kBRes) * 0.01f;
        const float maxOct = std::log2(0.45f * sr_);
        for (int c = 0; c < 2; ++c)
        {
            const float tA = baseA + fol_ * get(kFolA) + lfo[c] * get(kLfoA) + env_ * get(kEnvA) + drift[c];
            const float tB = baseB + fol_ * get(kFolB) + lfoB[c] * get(kLfoB) + env_ * get(kEnvB) + drift[c];
            octA_[c] += glide * (tA - octA_[c]);
            octB_[c] += glide * (tB - octB_[c]);
            a_[c].set(std::exp2(clamp(octA_[c], 4.33f, maxOct)), resA);   // 2^4.33 ~ 20 Hz
            b_[c].set(std::exp2(clamp(octB_[c], 4.33f, maxOct)), resB);
        }
    }

    FilterSlot a_[2], b_[2];
    float sr_ = 44100.0f;
    float powCoef_ = 0.0f, power_ = 0.0f, peak_ = 0.0f;
    int ctrlCount_ = 0;
    float fol_ = 0.0f, fast_ = 0.0f, slow_ = 0.0f, env_ = 0.0f;
    bool envRising_ = false;
    float riseFrom_ = 0.0f, riseT_ = 0.0f;
    int holdoff_ = 0;
    double lfoCycles_ = 0.0, lfoInc_ = 0.0, lfoCyclesB_ = 0.0, lfoIncB_ = 0.0, driftCycles_ = 0.0;
    float octA_[2] = {8.6f, 8.6f}, octB_[2] = {10.5f, 10.5f};
    bool glideSnap_ = true;
    int meterWait_ = 0;
    float lastMeterA_ = -1.0f, lastMeterB_ = -1.0f;
    LinearRamp mix_, out_, drive_, panA_, panB_;
};

MPC_PLUGIN_ENTRY(Fathom)
