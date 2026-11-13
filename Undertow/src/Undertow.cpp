// Undertow - a dub delay with an effects chain inside its feedback loop, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// Signal flow (per channel):
//
//   in ─┬─────────────────────────────────────────────────────────────────────────────┐ dry
//       └─► x Input ─► delay ─► FILTER ─► [ PITCH ─► FX ] ─► DRIVE ─► DC ─┬─► REVERSE ─► WET LIMITER ┴─► mix ─► Output ─► out
//            ▲     (Off/LP/    (semitones (Chorus,  (tube-style  │   (wet only:
//            │      BP/HP)      or Hz)    Flanger,  saturation,  │    each repeat
//            │                            Phaser,   always on)   │    plays
//            │                level match Bitcrush,              │    backwards)
//            │                (AGC) around Downsample,           │
//            │                pitch + FX  Ring Mod, Fuzz)        │
//            └──────────── x Feedback (0-120%) ◄─────────────────┘   Ping-Pong: mono input into L, L feeds R, R feeds L
//
// Gain staging: every stage in the loop is held at about unity gain, so Feedback means what it says: repeats
// grow only above 100%, or from ~67% at full Resonance. The filter's resonant peak is allowed up to +3.5 dB.
// Pitch and FX are wrapped in an automatic level match (AGC): it compares the signal power going in and coming
// out (40 ms followers, stereo-linked) and applies the difference, within +-12 dB. Effects that notch or
// cancel tones (chorus, flanger, phaser, ring mod) would otherwise lose 4-9 dB per repeat on tonal material.
//
// Pitch: Semitones mode is a two-tap granular shifter (50 ms Hann windows), -24..+24 semitones. Hz mode is a
// frequency shifter (Hilbert transform from two 4-stage allpass chains - Olli Niemitalo's public-domain
// coefficients - and a quadrature oscillator), -1000..+1000 Hz, finest near 0. Both are inside the loop, so
// each repeat is shifted again: +12 climbs octave by octave, small Hz values give barber-pole spirals.
//
// Reverse plays each repeat backwards: the wet signal is recorded in chunks as long as the delay time (up to
// 2 s) and each chunk is played back reversed while the next one records, with a 10 ms crossfade at the joins,
// so every event is heard exactly once. The loop itself runs forwards, so effects still compound normally.
//
// Why it stays musical at 120% feedback and full resonance: the drive stage is always in the loop and
// its output is bounded, so the repeats can grow until they saturate but never run to infinity. The
// wet limiter then pins the repeats under -3 dBFS (Limit pushes them into it, from 0 to +18 dB), and
// a soft knee on the final output catches anything above -1 dBFS. The dry signal is not limited.
//
// Delay time glides (about 150 ms) when you change it, so turning Time warps the pitch of
// the repeats like tape. Wobble adds wow (~0.5 Hz) and flutter (~6 Hz) to the delay time.
//
// Parameters (ids are permanent once released - never rename or reorder; append new ones at the end):
//    0 Output     dB  -30..0       0      level after everything (the host nudges parameter 0 on load)
//    1 Time Mode  Free/Sync        Sync   Sync: Time snaps to note divisions of the MPC tempo; Free: milliseconds
//    2 Division   (legacy, unused since 1.1: kept so parameter numbers stay put; hidden, not automatable)
//    3 Time       10..5000 ms      375    one control for both modes. In Sync mode its position picks one of
//                 or 1/64..2/1     1/8D   17 divisions (1/64 .. 2/1) and the display shows the division name
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
//   16 Pitch Mode Semitones/Hz     Semitones
//   17 Pitch      -1..1            0      one control for both modes: -24..+24 semitones (snapped), or
//                                         -1000..+1000 Hz (cubic, fine near 0); 0 = no shift
//   18 Pitch Mix  %   0..100       100    shifted vs unshifted signal inside the loop (50% stacks harmonies)
//   19 Reverse    Off/On           Off    repeats play backwards (wet only)
//   20 Input      dB  -30..+12     0      level into the delay (dry untouched); -30 = off
//   21 LFO Shape  Sine, Triangle, Saw Up, Saw Down, Square, Random (sample & hold), Drift (smooth random)
//   22 LFO Mode   Free/Sync        Sync   Sync: LFO Rate picks a note length (8 bars .. 1/32) locked to the song
//   23 LFO Rate   Hz  0.02..20     1 bar  one control for both modes, like Time
//   24 Spread     deg 0..180       90     right-channel LFO phase offset (affects Cutoff and Time modulation)
//   25 LFO>Cutoff oct -4..+4       0      the modulation depths are all bipolar; 0 = off
//   26 LFO>Pitch  -1..1            0      +-12 st (Semitones mode) or +-200 Hz (Hz mode, cubic)
//   27 LFO>FX     %  -100..100     0      added to FX Amount
//   28 LFO>Time   %  -100..100     0      100% = +-20% of the delay time (slow, deep tape warp)
//   29 Env>Cutoff oct -4..+4       0      envelope follower on the input (after Input)
//   30 Env>FX     %  -100..100     0
//   31 Env Speed  %   0..100       30     attack 1..30 ms, release 60..1000 ms (on the dB-scaled envelope)
//   32 Duck       %   0..100       0      repeats are turned down while the input is loud (100% = silent at -6 dBFS)
//
// Modulation: one LFO (computed per sample, smoothed over 4 ms so steps and saw resets do not click) and an
// envelope follower (peak, in dB over a 48 dB range) drive fixed destinations. Cutoff, FX Amount and Pitch
// are updated every 16 samples with the filter coefficients; Time and Duck per sample.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/noise.h"
#include "dsp/delay_line.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"
#include "dsp/svf.h"
#include "dsp/tempo.h"

#include <algorithm>
#include <cmath>

using namespace mpc::dsp;

namespace {

enum ParamIndex
{
    kOutput = 0, kSync, kDivision, kTime, kFeedback, kMix, kPingPong, kWobble, kLimit,
    kFilterMode, kCutoff, kResonance, kDrive, kFxType, kFxAmount, kFxRate,
    kPitchMode, kPitch, kPitchMix, kReverse, kInput,
    kLfoShape, kLfoSync, kLfoRate, kLfoSpread, kLfoCutoff, kLfoPitch, kLfoFx, kLfoTime,
    kEnvCutoff, kEnvFx, kEnvSpeed, kDuck, kNumParams
};

enum FilterMode { kFilterOff = 0, kFilterLP, kFilterBP, kFilterHP };
enum FxType { kFxOff = 0, kFxChorus, kFxFlanger, kFxPhaser, kFxBitcrush, kFxDownsample, kFxRingMod, kFxFuzz };

constexpr float kMaxDelaySeconds = 5.0f;
constexpr float kFxDelaySeconds = 0.04f;     // chorus / flanger delay line
constexpr int   kCoeffInterval = 16;         // filter coefficients are refreshed every 16 samples
constexpr float kWetCeiling = 0.7079f;       // -3 dBFS
constexpr float kOutKnee = 0.8913f;          // -1 dBFS: the output soft knee starts here...
constexpr float kOutCeiling = 0.9886f;       // ...and never passes -0.1 dBFS
constexpr float kPitchWindowSeconds = 0.05f; // granular pitch shifter window
constexpr float kReverseMaxSeconds = 2.0f;   // longest reversed chunk
constexpr float kAgcMinGain = 0.25f, kAgcMaxGain = 4.0f;   // level match range: -12..+12 dB
constexpr int   kMaxSemitones = 24;
constexpr float kMaxShiftHz = 1000.0f;
enum PitchMode { kPitchSemitones = 0, kPitchHz };

// Modulation.
enum LfoShape { kLfoSine = 0, kLfoTriangle, kLfoSawUp, kLfoSawDown, kLfoSquare, kLfoRandom, kLfoDrift };
constexpr int kNumLfoDivisions = 16;     // synced LFO cycle lengths, slowest first
const char* const kLfoDivisionNames[kNumLfoDivisions] = {
    "8 bars", "4 bars", "2 bars", "1 bar", "1/2D", "1/2", "1/4D", "1/2T",
    "1/4", "1/8D", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
constexpr double kLfoDivisionBeats[kNumLfoDivisions] = {
    32.0, 16.0, 8.0, 4.0, 3.0, 2.0, 1.5, 4.0 / 3.0, 1.0, 0.75, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125 };
constexpr float kLfoMinHz = 0.02f, kLfoMaxHz = 20.0f;
constexpr float kLfoPitchSemis = 12.0f;  // LFO>Pitch at 100% in Semitones mode
constexpr float kLfoPitchHz = 200.0f;    // ...and in Hz mode
constexpr float kLfoTimeDepth = 0.2f;    // LFO>Time at 100%: +-20% of the delay time

// One LFO output (left or right). Random and Drift pick a new value each time the phase wraps.
struct LfoVoice
{
    float prevPhase = 0.0f, cur = 0.0f, next = 0.0f, out = 0.0f;
};

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

// Four second-order allpass sections, H(z) = (c - z^-2) / (1 - c z^-2), in series.
struct AllpassChain4
{
    float c[4] = {};
    float x1[4] = {}, x2[4] = {}, y1[4] = {}, y2[4] = {};
    float process(float x)
    {
        for (int k = 0; k < 4; ++k)
        {
            const float y = c[k] * (x + y2[k]) - x2[k];
            x2[k] = x1[k]; x1[k] = x; y2[k] = y1[k]; y1[k] = y;
            x = y;
        }
        return x;
    }
    void reset() { for (int k = 0; k < 4; ++k) x1[k] = x2[k] = y1[k] = y2[k] = 0.0f; }
};

// Hilbert transformer: two allpass chains whose outputs are 90 degrees apart from ~20 Hz to ~20 kHz
// (Olli Niemitalo's coefficients, public domain; the coefficients below are squared). Path A takes an
// extra one-sample delay. Verified: 44-74 dB rejection of the unwanted sideband from 200 Hz to 8 kHz.
struct Hilbert
{
    AllpassChain4 a, b;
    float aDelayed = 0.0f;
    Hilbert()
    {
        const double ka[4] = { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
        const double kb[4] = { 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 };
        for (int k = 0; k < 4; ++k) { a.c[k] = (float)(ka[k] * ka[k]); b.c[k] = (float)(kb[k] * kb[k]); }
    }
    void process(float x, float& i, float& q) { i = aDelayed; aDelayed = a.process(x); q = b.process(x); }
    void reset() { a.reset(); b.reset(); aDelayed = 0.0f; }
};

// Read two taps from `line` that sweep through a window of `w` samples, Hann-crossfaded so their gains
// always sum to 1. `phase` (0..1) sets where tap 1 is; tap 2 is half a window behind (granular pitch shifter).
inline float twoTapRead(const DelayLine& line, float phase, float w)
{
    const float p2 = phase < 0.5f ? phase + 0.5f : phase - 0.5f;
    const float s = sine01(phase * 0.5f);          // sin(pi * phase)
    const float g1 = s * s;                        // Hann window; tap 2 gets cos^2 = 1 - g1
    return line.readLinear(1.0f + phase * w) * g1 + line.readLinear(1.0f + p2 * w) * (1.0f - g1);
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
    DelayLine pitchBuf;          // granular pitch shifter
    Hilbert hilbert;             // frequency shifter
    DelayLine reverseBuf;        // reverser (wet only)
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
    void clearPitch() { pitchBuf.reset(); hilbert.reset(); }
    void clear()
    {
        delay.reset();
        filter.reset();
        clearFx();
        clearPitch();
        reverseBuf.reset();
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
        addParam(choiceParam("sync",     "Time Mode", {"Free", "Sync"}, 1));
        Param legacyDiv = choiceParam("div", "Division", divisionNames(), 9);   // unused since 1.1, see header
        legacyDiv.automatable = false;
        addParam(legacyDiv);
        addParam(logParam   ("time",     "Time",      10.0f, 5000.0f, 375.0f, "", 0));   // unit added by formatValue
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
        addParam(choiceParam("pmode",    "Pitch Mode", {"Semitones", "Hz"}, kPitchSemitones));
        addParam(floatParam ("pitch",    "Pitch",     -1.0f, 1.0f,    0.0f,   "", 3));    // text from formatValue
        addParam(floatParam ("pmix",     "Pitch Mix", 0.0f, 100.0f,   100.0f, "%", 0));
        addParam(boolParam  ("reverse",  "Reverse",   false));
        addParam(floatParam ("input",    "Input",     -30.0f, 12.0f,  0.0f,   "dB", 1));   // level into the delay (dry untouched)
        addParam(choiceParam("lfoshape", "LFO Shape", {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random",
                                                       "Drift"}, kLfoSine));
        addParam(choiceParam("lfosync",  "LFO Mode",  {"Free", "Sync"}, 1));
        addParam(logParam   ("lforate",  "LFO Rate",  kLfoMinHz, kLfoMaxHz, lfoSyncPositionHz(3), "", 2));   // 1 bar
        addParam(floatParam ("lfospread","Spread",    0.0f, 180.0f,   90.0f,  "deg", 0));
        addParam(floatParam ("lfocut",   "LFO>Cutoff", -4.0f, 4.0f,   0.0f,   "", 1));    // units come from formatValue
        addParam(floatParam ("lfopitch", "LFO>Pitch", -1.0f, 1.0f,    0.0f,   "", 3));    // text from formatValue
        addParam(floatParam ("lfofx",    "LFO>FX",    -100.0f, 100.0f, 0.0f,  "", 0));
        addParam(floatParam ("lfotime",  "LFO>Time",  -100.0f, 100.0f, 0.0f,  "", 0));
        addParam(floatParam ("envcut",   "Env>Cutoff", -4.0f, 4.0f,   0.0f,   "", 1));
        addParam(floatParam ("envfx",    "Env>FX",    -100.0f, 100.0f, 0.0f,  "", 0));
        addParam(floatParam ("envspeed", "Env Speed", 0.0f, 100.0f,   30.0f,  "%", 0));
        addParam(floatParam ("duck",     "Duck",      0.0f, 100.0f,   0.0f,   "%", 0));
        if (numParams() != kNumParams) mpc::log("Undertow: parameter count mismatch");

        // Programs. The Time column is given as a division (D(9) = 1/8D) because the programs are synced.
        // Divisions: 6=1/16D 7=1/8 8=1/4T 9=1/8D 10=1/4 12=1/4D. Filter 0..3 = Off/LP/BP/HP.
        // FX 0..7 = Off/Chorus/Flanger/Phaser/Bitcrush/Downsample/Ring Mod/Fuzz.
        // Pitch column: ST(12) = +12 semitones, HZ(40) = +40 Hz (the Pitch Mode column must match).
        auto D = [](int division) { return syncTimeMs(division); };
        auto ST = [](float semis) { return semis / (float)kMaxSemitones; };
        auto HZ = [](float hz) { return std::cbrt(hz / kMaxShiftHz); };
        // Modulation tail (21..32): LR(3) = LFO Rate position for the synced division 3 (1 bar), 13 = 1/16, 2 = 2 bars.
        // Shapes 0..6 = Sine/Triangle/Saw Up/Saw Down/Square/Random/Drift.
        auto LR = [](int division) { return lfoSyncPositionHz(division); };
        //                                    Shape Mode Rate   Sprd L>Cut L>Pit L>FX L>Time E>Cut E>FX ESpd Duck
        const std::vector<float> noMod     = { 0,    1,   LR(3),  90,  0,    0,    0,   0,     0,    0,   30,  0 };
        auto prog = [this](const char* name, std::vector<float> v, const std::vector<float>& mod)
        {
            v.insert(v.end(), mod.begin(), mod.end());
            addProgram(name, std::move(v));
        };
        //                            Out Sync Div Time    Fb   Mix PP Wob Lim  Flt Cut     Res Drv  FX Amt Rate  PMd Pitch    PMix Rev In
        prog("Dub Echo",         { 0, 1,  9, D(9),   60,  35, 1, 15, 30,  1, 1800,   35, 25,  0, 50, 30,  0, 0,        100, 0, 0 }, noMod);
        prog("Siren Sweep",      { 0, 1,  7, D(7),   68,  40, 1, 10, 60,  2, 1200,   75, 40,  0, 50, 30,  0, 0,        100, 0, 0 }, noMod);
        prog("Chorus Wash",      { 0, 1, 10, D(10),  65,  35, 1, 25, 30,  1, 4000,   20, 15,  1, 60, 25,  0, 0,        100, 0, 0 }, noMod);
        prog("Crushed Repeats",  { 0, 1,  6, D(6),   70,  35, 1,  5, 40,  3,  300,   30, 30,  4, 65, 40,  0, 0,        100, 0, 0 }, noMod);
        prog("Flange Spiral",    { 0, 1,  8, D(8),   80,  40, 1, 10, 40,  1, 6000,   40, 20,  2, 70, 20,  0, 0,        100, 0, 0 }, noMod);
        prog("Lo-Fi Tape",       { 0, 1,  9, D(9),   55,  30, 1, 45, 30,  1, 2500,   15, 35,  5, 35, 60,  0, 0,        100, 0, 0 }, noMod);
        prog("Self-Oscillation", { 0, 1, 12, D(12), 110,  45, 1, 20, 70,  2,  800,   92, 55,  0, 50, 30,  0, 0,        100, 0, 0 }, noMod);
        prog("Octave Climb",     { 0, 1, 10, D(10),  70,  35, 1,  5, 40,  1, 7000,   15, 15,  0, 50, 30,  0, ST(12),    50, 0, 0 }, noMod);
        prog("Down the Well",    { 0, 1,  9, D(9),   72,  40, 1, 10, 40,  1, 3000,   25, 30,  0, 50, 30,  0, ST(-12),  100, 0, 0 }, noMod);
        prog("Barber Pole",      { 0, 1,  7, D(7),   85,  40, 1,  0, 40,  0, 1800,   35, 20,  0, 50, 30,  1, HZ(25),   100, 0, 0 }, noMod);
        prog("Reverse Dub",      { 0, 1, 10, D(10),  55,  40, 1, 10, 30,  1, 2500,   30, 25,  0, 50, 30,  0, 0,        100, 1, 0 }, noMod);
        prog("Backwards Fifths", { 0, 1, 12, D(12),  65,  40, 1, 10, 40,  1, 5000,   20, 20,  1, 40, 20,  0, ST(7),     60, 1, 0 }, noMod);
        prog("Auto-Wah Echo",      { 0, 1,  9, D(9),   62,  40, 1, 10, 30,  2,  500,   70, 30,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   { 0,    1,   LR(3),  90,  0,    0,    0,   0,     3.0f, 0,   15,  0 });
        prog("Sample & Hold",      { 0, 1,  7, D(7),   65,  40, 1,  5, 40,  2,  900,   65, 30,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   { 5,    1,   LR(13), 90,  2.5f, 0,    0,   0,     0,    0,   30,  0 });
        prog("Seasick Tape",       { 0, 1, 10, D(10),  60,  35, 1, 30, 30,  1, 3500,   20, 30,  5, 20, 40,  0, 0,        100, 0, 0 },
                                   { 6,    1,   LR(2), 120,  0,    0.08f, 0,  40,     0,    0,   30,  0 });
        prog("Ducked Dub",         { 0, 1,  9, D(9),   72,  50, 1, 15, 35,  1, 2200,   35, 25,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   { 0,    1,   LR(3),  90,  0,    0,    0,   0,     0,    0,   45, 75 });
        prog("Breathing Flange",   { 0, 1,  8, D(8),   75,  40, 1, 10, 40,  1, 6000,   35, 20,  2, 35, 15,  0, 0,        100, 0, 0 },
                                   { 1,    1,   LR(2),  90,  0,    0,    55,  0,     0,    0,   30,  0 });
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
            c.pitchBuf.prepare((int)(sampleRate * kPitchWindowSeconds) + 8);
            c.reverseBuf.prepare((int)(sampleRate * (2.0f * kReverseMaxSeconds + 0.05f)) + 8);   // 2 chunks + fade
        }
        delayTime_.setTime(150.0f, sampleRate);   // tape-style glide
        feedback_.setTime(30.0f, sampleRate);
        mix_.setTime(20.0f, sampleRate);
        output_.setTime(20.0f, sampleRate);
        input_.setTime(20.0f, sampleRate);
        logCutoff_.setTime(25.0f, sampleRate);
        limAttack_ = 1.0f - std::exp(-1.0f / (0.0003f * sr_));   // 0.3 ms
        limRelease_ = 1.0f - std::exp(-1.0f / (0.080f * sr_));   // 80 ms
        agcFollow_ = 1.0f - std::exp(-1.0f / (0.040f * sr_));    // 40 ms power followers
        agcSmooth_ = 1.0f - std::exp(-1.0f / (0.060f * sr_));    // 60 ms gain glide
        // While LFO>Pitch moves the shifter, its latency keeps changing and a fast level match would boost the
        // start of every repeat (enough to make the loop sustain itself), so it then averages over 150 ms.
        agcFollowSlow_ = 1.0f - std::exp(-1.0f / (0.150f * sr_));
        agcSmoothSlow_ = 1.0f - std::exp(-1.0f / (0.150f * sr_));
        pitchMix_.setTime(25.0f, sampleRate);
        lfoSmooth_ = 1.0f - std::exp(-1.0f / (0.004f * sr_));    // 4 ms: softens steps and saw resets
        duckSmooth_ = 1.0f - std::exp(-1.0f / (0.005f * sr_));
        peakRelease_ = 1.0f - std::exp(-1.0f / (0.030f * sr_));
        reverseMix_.setTime(25.0f, sampleRate);
    }

    void reset() override
    {
        for (LoopChannel& c : ch_) c.clear();
        delayTime_.snap(targetDelaySamples(120.0));
        feedback_.snap(get(kFeedback) * 0.01f);
        mix_.snap(get(kMix) * 0.01f);
        output_.snap(dbToGain(get(kOutput)));
        input_.snap(inputGain());
        logCutoff_.snap(std::log2(get(kCutoff)));
        limGain_ = 1.0f;
        wowPhase_ = 0.0f; flutterPhase_ = 0.3f; fxLfoPhase_ = 0.0f; ringPhase_ = 0.0f;
        coeffCounter_ = 0;
        lastFxType_ = getInt(kFxType);
        lastPitchMode_ = getInt(kPitchMode);
        agcIn_ = agcOut_ = 0.0f; agcGain_ = agcTarget_ = 1.0f;
        pitchPhase_ = 0.0f; shiftCos_ = 1.0f; shiftSin_ = 0.0f;
        pitchInc_ = 0.0f; shiftRotC_ = 1.0f; shiftRotS_ = 0.0f;
        lfoPhase_ = 0.0f;
        for (LfoVoice& v : lfo_) { v = LfoVoice(); v.cur = rng_.nextBipolar(); v.next = rng_.nextBipolar(); }
        env_ = envNorm_ = 0.0f; duckGain_ = duckTarget_ = 1.0f;
        revPos_ = 0; revChunk_ = revPrevChunk_ = (int)clamp(delayTime_.value(), 0.02f * sr_, kReverseMaxSeconds * sr_);
        pitchMix_.snap(pitchMixTarget());
        reverseMix_.snap(getBool(kReverse) ? 1.0f : 0.0f);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // ----- per block: read parameters -----------------------------------------------------
        delayTime_.setTarget(targetDelaySamples(ctx.transport.bpm));
        feedback_.setTarget(get(kFeedback) * 0.01f);
        mix_.setTarget(get(kMix) * 0.01f);
        output_.setTarget(dbToGain(get(kOutput)));
        input_.setTarget(inputGain());
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

        // Pitch: granular shifter (semitones) or frequency shifter (Hz).
        const int pitchMode = getInt(kPitchMode);
        if (pitchMode != lastPitchMode_) { for (LoopChannel& c : ch_) c.clearPitch(); lastPitchMode_ = pitchMode; }
        pitchMix_.setTarget(pitchMixTarget());
        const float pitchWindow = kPitchWindowSeconds * sr_;
        const int baseSemis = semitones();
        const float baseShiftHz = shiftHz();
        const bool levelMatch = fx.type != kFxOff || pitchMix_.target() > 0.0f || pitchMix_.value() > 1e-4f;

        // Reverse: chunk length follows the delay time; it is latched at each chunk boundary.
        reverseMix_.setTarget(getBool(kReverse) ? 1.0f : 0.0f);
        const int reverseChunk = (int)clamp(delayTime_.target(), 0.02f * sr_, kReverseMaxSeconds * sr_);

        // Modulation: LFO speed (and song-position lock when synced and playing), envelope follower times.
        const int lfoShape = getInt(kLfoShape);
        const double bpm = ctx.transport.bpm > 1.0 ? ctx.transport.bpm : 120.0;
        const float lfoInc = lfoRateHz(bpm) / sr_;
        if (getInt(kLfoSync) == 1 && ctx.transport.playing && ctx.transport.ppqValid)
        {
            const double cycles = ctx.transport.ppqPos / kLfoDivisionBeats[lfoDivisionFromPosition(getNormalised(kLfoRate))];
            const float target = (float)(cycles - std::floor(cycles));
            float diff = target - lfoPhase_;
            diff -= std::floor(diff + 0.5f);                    // shortest way round
            if (std::fabs(diff) > 0.01f) lfoPhase_ = target;
        }
        const float spread = get(kLfoSpread) / 360.0f;
        const float lfoCut = get(kLfoCutoff), envCut = get(kEnvCutoff);
        const float lfoPitch = get(kLfoPitch);
        const float lfoPitchSt = lfoPitch * kLfoPitchSemis, lfoPitchHz = lfoPitch * lfoPitch * lfoPitch * kLfoPitchHz;
        const float lfoFx = get(kLfoFx) * 0.01f, envFx = get(kEnvFx) * 0.01f;
        const float lfoTime = get(kLfoTime) * 0.01f * kLfoTimeDepth;
        const float envSpeed = get(kEnvSpeed) * 0.01f;
        const float envAttack = 1.0f - std::exp(-(float)kCoeffInterval / ((0.001f + 0.029f * envSpeed) * sr_));
        const float envRelease = 1.0f - std::exp(-(float)kCoeffInterval / ((0.06f + 0.94f * envSpeed) * sr_));
        const float duck = get(kDuck) * 0.01f;
        const float baseFxAmount = fx.amount;
        const bool fxMod = fx.type != kFxOff && (lfoFx != 0.0f || envFx != 0.0f);
        const float maxCutoff = std::fmin(18000.0f, 0.42f * sr_);
        const bool pitchModulated = std::fabs(lfoPitch) >= 0.0005f;
        const float agcFollow = pitchModulated ? agcFollowSlow_ : agcFollow_;
        const float agcSmooth = pitchModulated ? agcSmoothSlow_ : agcSmooth_;

        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        const float maxDelay = (float)ch_[0].delay.maxDelay() - 2.0f;

        // ----- per sample ---------------------------------------------------------------------
        for (int n = 0; n < ctx.numFrames; ++n)
        {
            const float dryL = inL[n], dryR = inR[n];
            const float inGain = input_.process();
            const float sendL = dryL * inGain, sendR = dryR * inGain;

            // Envelope follower (peak) on what goes into the delay.
            const float level = std::fmax(std::fabs(sendL), std::fabs(sendR));
            env_ += (level > env_ ? 1.0f : peakRelease_) * (level - env_);   // instant attack, 30 ms release

            // LFO: left at lfoPhase_, right offset by Spread.
            lfoPhase_ += lfoInc;
            if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;
            for (int c = 0; c < 2; ++c)
            {
                float ph = c == 0 ? lfoPhase_ : lfoPhase_ + spread;
                if (ph >= 1.0f) ph -= 1.0f;
                LfoVoice& v = lfo_[c];
                v.out += lfoSmooth_ * (lfoValue(v, lfoShape, ph) - v.out);
            }

            // Filter coefficients follow the smoothed cutoff, refreshed every 16 samples.
            const float logCut = logCutoff_.process();
            if (coeffCounter_-- <= 0)
            {
                coeffCounter_ = kCoeffInterval - 1;
                // Envelope in dB over a 48 dB range -> 0..1.
                // Envelope: peak level in dB, -48..-6 dBFS -> 0..1, then the Env Speed attack / release.
                const float levelNorm = env_ > 1e-5f ? clamp((6.0206f * std::log2(env_) + 48.0f) / 42.0f, 0.0f, 1.0f) : 0.0f;
                envNorm_ += (levelNorm > envNorm_ ? envAttack : envRelease) * (levelNorm - envNorm_);
                const float duckGain = 1.0f - duck * envNorm_;
                duckTarget_ = duckGain * duckGain;   // 50% ~ -10 dB, 100% down to silence on loud input
                for (int c = 0; c < 2; ++c)
                {
                    const float oct = lfoCut * lfo_[c].out + envCut * envNorm_;
                    ch_[c].filter.set(clamp(std::exp2(logCut + oct), 20.0f, maxCutoff), reso);
                }
                if (fxMod)
                {
                    fx = fxSettings(clamp(baseFxAmount + lfoFx * lfo_[0].out + envFx * envNorm_, 0.0f, 1.0f));
                    if (fx.type == kFxDownsample || fx.type == kFxFuzz)
                        for (LoopChannel& c : ch_) c.tone.setCutoff(fx.toneHz, sr_);
                }
                if (fx.type == kFxPhaser) updatePhaser(fx);
                // Pitch (mono: the left LFO).
                if (pitchMode == kPitchSemitones)
                {
                    const float st = (float)baseSemis + lfoPitchSt * lfo_[0].out;
                    pitchInc_ = (1.0f - std::exp2(st / 12.0f)) / pitchWindow;   // tap phase per sample
                }
                else
                {
                    const float step = 6.2831853f * (baseShiftHz + lfoPitchHz * lfo_[0].out) / sr_;
                    shiftRotC_ = std::cos(step);
                    shiftRotS_ = std::sin(step);
                }
                // Level match: new target from the power going into and out of pitch + FX. Hold the gain
                // while the loop is (nearly) silent so it does not drift up on nothing.
                if (levelMatch && agcIn_ > 1e-9f && agcOut_ > 1e-12f)
                    agcTarget_ = clamp(std::sqrt(agcIn_ / agcOut_), kAgcMinGain, kAgcMaxGain);
                else if (!levelMatch)
                    agcTarget_ = 1.0f;
            }

            // Delay time: glide + wow and flutter (right channel slightly out of phase for width).
            const float base = delayTime_.process();
            wowPhase_ = advance(wowPhase_, wowInc);
            flutterPhase_ = advance(flutterPhase_, flutterInc);
            const float modL = wowDepth * sine01(wowPhase_) + flutterDepth * sine01(flutterPhase_);
            const float modR = wowDepth * sine01(advance(wowPhase_, 0.21f))
                             + flutterDepth * sine01(advance(flutterPhase_, 0.37f));
            const float dL = clamp(base * (1.0f + lfoTime * lfo_[0].out) + modL, 2.0f, maxDelay);
            const float dR = clamp(base * (1.0f + lfoTime * lfo_[1].out) + modR, 2.0f, maxDelay);

            // Modulation sources for the FX slot.
            fxLfoPhase_ = advance(fxLfoPhase_, fx.lfoInc);
            ringPhase_ = advance(ringPhase_, fx.ringInc);
            const float lfoL = sine01(fxLfoPhase_);
            const float lfoR = sine01(advance(fxLfoPhase_, 0.25f));   // 90 degrees apart
            const float ring = sine01(ringPhase_);

            // Shared modulators for pitch: granular tap phase, frequency-shift oscillator.
            pitchPhase_ += pitchInc_;
            if (pitchPhase_ >= 1.0f) pitchPhase_ -= 1.0f; else if (pitchPhase_ < 0.0f) pitchPhase_ += 1.0f;
            {
                const float c = shiftCos_ * shiftRotC_ - shiftSin_ * shiftRotS_;
                shiftSin_ = shiftSin_ * shiftRotC_ + shiftCos_ * shiftRotS_;
                shiftCos_ = c;
            }
            const float pMix = pitchMix_.process();
            agcGain_ += agcSmooth * (agcTarget_ - agcGain_);

            // The loop: read -> filter -> [pitch -> FX, level-matched] -> drive -> DC block.
            float wet[2];
            float powIn = 0.0f, powOut = 0.0f;
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

                if (levelMatch)
                {
                    powIn += x * x;
                    if (pMix > 1e-4f)
                    {
                        float shifted;
                        if (pitchMode == kPitchSemitones)
                        {
                            ch.pitchBuf.write(x);
                            shifted = twoTapRead(ch.pitchBuf, pitchPhase_, pitchWindow);
                        }
                        else
                        {
                            float i, q;
                            ch.hilbert.process(x, i, q);
                            shifted = i * shiftCos_ + q * shiftSin_;
                        }
                        x += (shifted - x) * pMix;
                    }
                    x = processFx(ch, x, fx, lfo[c], ring);
                    powOut += x * x;
                    x *= agcGain_;
                }

                x = (fastTanh(driveGain * x + kBias) - tb) * driveNorm;
                x = ch.dc.process(x);
                ch.loopOut = x;
                wet[c] = x;
            }
            agcIn_ += agcFollow * (powIn - agcIn_);
            agcOut_ += agcFollow * (powOut - agcOut_);

            // Reverse (wet only). At position t into a chunk, read 2t+1 samples back: that walks backwards
            // through the previous chunk. For the first 10 ms of a chunk, crossfade from the previous read
            // head, which carries on backwards past the start of the chunk it was playing.
            const float rMix = reverseMix_.process();
            if (++revPos_ >= revChunk_) { revPos_ = 0; revPrevChunk_ = revChunk_; revChunk_ = reverseChunk; }
            const int fade = std::min((int)(0.010f * sr_), revChunk_ / 4);
            for (int c = 0; c < 2; ++c)
            {
                ch_[c].reverseBuf.write(wet[c]);
                if (rMix <= 1e-4f) continue;
                float r = ch_[c].reverseBuf.readInt(2 * revPos_ + 1);
                if (revPos_ < fade)
                {
                    const float x = (float)revPos_ / (float)fade;                 // 0 -> 1
                    const float prev = ch_[c].reverseBuf.readInt(2 * (revPrevChunk_ + revPos_) + 1);
                    r = r * sine01(x * 0.25f) + prev * sine01(0.25f + x * 0.25f);   // equal-power: sin / cos
                }
                wet[c] += (r - wet[c]) * rMix;
            }

            // Feed the repeats back.
            const float fb = feedback_.process();
            if (pingPong)
            {
                ch_[0].delay.write(0.5f * (sendL + sendR) + ch_[1].loopOut * fb);
                ch_[1].delay.write(ch_[0].loopOut * fb);
            }
            else
            {
                ch_[0].delay.write(sendL + ch_[0].loopOut * fb);
                ch_[1].delay.write(sendR + ch_[1].loopOut * fb);
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
            duckGain_ += duckSmooth_ * (duckTarget_ - duckGain_);
            const float wetGain = std::fmin(1.0f, 2.0f * mix) * duckGain_;
            const float gain = output_.process();
            outL[n] = softKnee((dryL * dryGain + wL * wetGain) * gain, kOutKnee, kOutCeiling);
            outR[n] = softKnee((dryR * dryGain + wR * wetGain) * gain, kOutKnee, kOutCeiling);
        }

        // Keep the frequency-shift oscillator on the unit circle.
        const float mag = std::sqrt(shiftCos_ * shiftCos_ + shiftSin_ * shiftSin_);
        if (mag > 0.0f) { shiftCos_ /= mag; shiftSin_ /= mag; }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Undertow";
        info.vendor       = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1400;            // 1.4.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 0;               // unknown: depends on Feedback (endless above 100%)
        return info;
    }

    static constexpr char kUid[5] = "GGxC";

    // Time's range is 10..5000 ms on a log scale. In Sync mode its 0..1 position is split into the
    // 17 divisions, so the same knob picks a division and shows its name.
    static constexpr float kTimeMinMs = 10.0f, kTimeMaxMs = 5000.0f;
    static int divisionFromPosition(float norm) { return clamp((int)std::lround(norm * (kNumDivisions - 1)), 0, kNumDivisions - 1); }
    static float syncTimeMs(int division) { return kTimeMinMs * std::pow(kTimeMaxMs / kTimeMinMs, (float)division / (kNumDivisions - 1)); }

    float targetDelaySamples(double bpm) const
    {
        const float seconds = getInt(kSync) == 1
            ? beatsToSeconds(divisionBeats(divisionFromPosition(getNormalised(kTime))), bpm)
            : get(kTime) * 0.001f;
        return clamp(seconds, 0.001f, kMaxDelaySeconds) * sr_;
    }

    // Pitch: one control, two meanings. Semitones snap to whole steps; Hz is cubic so small shifts are easy.
    int semitones() const
    {
        return getInt(kPitchMode) == kPitchSemitones ? (int)std::lround(get(kPitch) * kMaxSemitones) : 0;
    }
    float shiftHz() const
    {
        const float x = get(kPitch);
        return getInt(kPitchMode) == kPitchHz ? kMaxShiftHz * x * x * x : 0.0f;
    }
    // The pitch stage is faded in only while it actually shifts, so 0 st / 0 Hz is a clean bypass.
    float pitchMixTarget() const
    {
        const bool active = (getInt(kPitchMode) == kPitchSemitones ? semitones() != 0 : std::fabs(shiftHz()) >= 0.05f)
                         || std::fabs(get(kLfoPitch)) >= 0.0005f;
        return active ? get(kPitchMix) * 0.01f : 0.0f;
    }

    // LFO Rate works like Time: in Sync mode its 0..1 position picks one of 16 cycle lengths.
    static int lfoDivisionFromPosition(float norm)
    {
        return clamp((int)std::lround(norm * (kNumLfoDivisions - 1)), 0, kNumLfoDivisions - 1);
    }
    static float lfoSyncPositionHz(int division)   // plain LFO Rate value at a division's position (for programs)
    {
        return kLfoMinHz * std::pow(kLfoMaxHz / kLfoMinHz, (float)division / (kNumLfoDivisions - 1));
    }
    float lfoRateHz(double bpm) const
    {
        if (getInt(kLfoSync) == 1)
            return (float)(bpm / 60.0 / kLfoDivisionBeats[lfoDivisionFromPosition(getNormalised(kLfoRate))]);
        return get(kLfoRate);
    }
    // One LFO sample, -1..1, at phase ph (0..1).
    float lfoValue(LfoVoice& v, int shape, float ph)
    {
        if (ph < v.prevPhase) { v.cur = v.next; v.next = rng_.nextBipolar(); }
        v.prevPhase = ph;
        switch (shape)
        {
            case kLfoTriangle: return 1.0f - 4.0f * std::fabs(ph - 0.5f);
            case kLfoSawUp:    return 2.0f * ph - 1.0f;
            case kLfoSawDown:  return 1.0f - 2.0f * ph;
            case kLfoSquare:   return ph < 0.5f ? 1.0f : -1.0f;
            case kLfoRandom:   return v.cur;
            case kLfoDrift:    return v.cur + (v.next - v.cur) * ph * ph * (3.0f - 2.0f * ph);
            default:           return sine01(ph);
        }
    }

    // Input: -30 dB at the bottom of the knob means off, so a throw can start from silence.
    float inputGain() const { const float db = get(kInput); return db <= -29.95f ? 0.0f : dbToGain(db); }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index == kInput && plainValue <= -29.95f) { out = "-inf dB"; return true; }
        char mb[32];
        if (index == kLfoRate)
        {
            if (getInt(kLfoSync) == 1) out = kLfoDivisionNames[lfoDivisionFromPosition(param(kLfoRate).toNormalised(plainValue))];
            else { std::snprintf(mb, sizeof(mb), plainValue < 10.0f ? "%.2f Hz" : "%.1f Hz", (double)plainValue); out = mb; }
            return true;
        }
        if (index == kLfoCutoff || index == kEnvCutoff)
        {
            if (std::fabs(plainValue) < 0.05f) out = "0 oct";
            else { std::snprintf(mb, sizeof(mb), "%+.1f oct", (double)plainValue); out = mb; }
            return true;
        }
        if (index == kLfoFx || index == kEnvFx || index == kLfoTime)
        {
            if (std::fabs(plainValue) < 0.5f) out = "0 %";
            else { std::snprintf(mb, sizeof(mb), "%+.0f %%", (double)plainValue); out = mb; }
            return true;
        }
        if (index == kLfoPitch)
        {
            const float st = plainValue * kLfoPitchSemis;
            const float hz = plainValue * plainValue * plainValue * kLfoPitchHz;
            if (getInt(kPitchMode) == kPitchSemitones)
            {
                if (std::fabs(st) < 0.05f) out = "0 st";
                else { std::snprintf(mb, sizeof(mb), "%+.1f st", (double)st); out = mb; }
            }
            else
            {
                if (std::fabs(hz) < 0.05f) out = "0 Hz";
                else { std::snprintf(mb, sizeof(mb), std::fabs(hz) < 10.0f ? "%+.2f Hz" : "%+.1f Hz", (double)hz); out = mb; }
            }
            return true;
        }
        if (index == kPitch)
        {
            char buf[32];
            if (getInt(kPitchMode) == kPitchSemitones)
            {
                const int st = (int)std::lround(plainValue * kMaxSemitones);
                std::snprintf(buf, sizeof(buf), st == 0 ? "0 st" : "%+d st", st);
            }
            else
            {
                const float hz = kMaxShiftHz * plainValue * plainValue * plainValue;
                if (std::fabs(hz) < 0.05f) std::snprintf(buf, sizeof(buf), "0 Hz");
                else if (std::fabs(hz) < 100.0f) std::snprintf(buf, sizeof(buf), "%+.1f Hz", (double)hz);
                else std::snprintf(buf, sizeof(buf), "%+.0f Hz", (double)hz);
            }
            out = buf;
            return true;
        }
        if (index != kTime) return false;
        if (getInt(kSync) == 1)
        {
            out = divisionName(divisionFromPosition(param(kTime).toNormalised(plainValue)));
            return true;
        }
        char buf[32];
        if (plainValue >= 1000.0f) std::snprintf(buf, sizeof(buf), "%.2f s", (double)(plainValue * 0.001f));
        else std::snprintf(buf, sizeof(buf), "%.0f ms", (double)plainValue);
        out = buf;
        return true;
    }

    // Turn FX Amount / FX Rate into the numbers each effect needs (once per block).
    FxSettings fxSettings(float amount = -1.0f) const   // amount 0..1 overrides FX Amount (modulation)
    {
        FxSettings s;
        s.type = getInt(kFxType);
        s.amount = amount >= 0.0f ? amount : get(kFxAmount) * 0.01f;
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
    Smoother delayTime_, feedback_, mix_, output_, input_, logCutoff_;
    float limGain_ = 1.0f, limAttack_ = 0.1f, limRelease_ = 0.001f;
    float wowPhase_ = 0.0f, flutterPhase_ = 0.3f, fxLfoPhase_ = 0.0f, ringPhase_ = 0.0f;
    int coeffCounter_ = 0;
    int lastFxType_ = 0;
    int lastPitchMode_ = 0;
    float agcIn_ = 0.0f, agcOut_ = 0.0f, agcGain_ = 1.0f, agcTarget_ = 1.0f, agcFollow_ = 0.01f, agcSmooth_ = 0.01f;
    float agcFollowSlow_ = 0.001f, agcSmoothSlow_ = 0.001f;
    float pitchPhase_ = 0.0f, shiftCos_ = 1.0f, shiftSin_ = 0.0f;
    int revPos_ = 0, revChunk_ = 1, revPrevChunk_ = 1;
    Smoother pitchMix_, reverseMix_;
    float pitchInc_ = 0.0f, shiftRotC_ = 1.0f, shiftRotS_ = 0.0f;   // pitch modulators, refreshed every 16 samples
    float lfoPhase_ = 0.0f, lfoSmooth_ = 0.01f;
    LfoVoice lfo_[2];
    Xorshift32 rng_{0x51F15EEDu};
    float env_ = 0.0f, envNorm_ = 0.0f, peakRelease_ = 0.001f, duckGain_ = 1.0f, duckTarget_ = 1.0f, duckSmooth_ = 0.01f;
};

MPC_PLUGIN_ENTRY(Undertow)
