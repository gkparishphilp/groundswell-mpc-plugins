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
//   33 Freeze     Off/On           Off    hold the loop: it repeats forever at unity, input shut out (see below)
//   34 Space      %   0..100       0      spring reverb amount
//   35 Spring Decay s 0.3..6       1.8
//   36 Spring Tone %  0..100       50     loop low-pass 1.5..9 kHz
//   37 Drip       %   0..100       50     dispersion: how much each spring "chirps"
//   38 Space Position  After/In Loop  After   After: on the repeats, before the wet limiter. In Loop: after
//                                         pitch + FX, so every repeat gets more diffuse (see below)
//   39 Character  Digital/Tape/BBD Digital  the delay medium, applied to every repeat as it leaves the delay line
//   40 Age        %   0..100       50     how worn: Tape = duller, more compression, hiss and wow; BBD = fewer
//                                         stages (darker, grittier)
//   41 Head 2     %   0..100       0      second playback head: how much of the loop it replaces (0 = off)
//   42 Head Ratio 1/4 .. 2/1       2/3    its position as a fraction of the delay time
//   43 Head 2 Pan %  -100..100     0      which side head 2 leans to
//   44 Display    Off/On           On     animate the tube and the echo trail from the audio (see below)
//   45 Level      0..1             0      read-only: the repeat level the tube is drawn from; not automatable
//   46..109 Echo 1..64             0      read-only: one bar of the echo trail; not automatable, not saved
//  110 Noise      %   0..100       0      a noise bed poured into the loop (see below)
//  111 Noise Type Hiss, Air, Crackle, Hum, Rain
//  112 LFO>Noise  %  -100..100     0      added to Noise
//
// Head 2: a second tap on the same tape, at Head Ratio x the delay time, crossfaded into the electronics
// (weights summing to 1), so two heads never make the loop louder than Feedback says. Both heads therefore go
// through one set of electronics and feed back together, as on a multi-head tape echo. Pan uses a different
// crossfade on each side, so head 2 can sit to one side while head 1 stays centred.
//
// Animated display: nothing in an MPC skin moves by itself, so the plugin writes what it wants drawn into
// parameters and tells the host (audioMasterAutomate). The tube takes the level of the repeats, about 25 times a
// second. The echo trail on the HEADS page takes 64 bars holding the peak of the repeats over one slice each;
// the window is two delay times wide, so a steady echo pattern stands still while it decays and only the newest
// two bars are sent each slice (about 40 host calls a second for both together). Each bar also carries a colour:
// the two heads' contributions are measured separately as they come off the tape, and the bar is drawn in head
// 1's colour, head 2's, or the blend, so head 2's repeats are visible sliding between head 1's as Head Ratio
// changes. Display Off stops all of it.
//
// Character: Digital is clean. Tape: a one-pole high-frequency loss (12 kHz at Age 0 down to 3 kHz), a little
// low-frequency loss, soft compression (tanh, unity for quiet signals, a few dB of squash on loud
// material at Age 100 - so the repeats round off and fade a little faster than Digital) and extra wow. BBD: a bucket-brigade chip of 4096 (Age 0) to 900 (Age 100) stages clocked
// to give the delay time, so the clock is stages / time: the signal is sampled and held at that clock and
// filtered at about 0.45 x clock (two one-poles), so longer delays are darker and grittier, as on a real BBD,
// plus harder, slightly asymmetric saturation. Every stage has a gain of at most 1, so the modes never make the
// loop run away. Neither mode adds noise of its own: that is the Noise knob, which works in every mode.
//
// Noise: one of five beds - Hiss (white), Air (pink, a soft wash), Crackle (sparse clicks, like worn vinyl),
// Hum (50 Hz and two harmonics) or Rain (dense fine clicks) - mixed into the loop right after the heads, so it
// is filtered, aged, driven and repeated like anything else: each crackle echoes. Two things keep it civil.
// Successive noise samples are uncorrelated, so a loop at feedback f settles at inject / sqrt(1 - f^2): the
// injected level is multiplied by sqrt(1 - Feedback^2), which lands the steady bed on the same level whatever
// Feedback is doing instead of letting it build up pass after pass; and it is gated by the louder of the input and what is still going
// round, staying open until about 3 s after the last thing it heard, so a track that is not playing stays
// silent. The bed's own level is subtracted from what the loop reports, so the noise circulating in the loop
// can never hold its own gate open while anything louder than the bed always can.
// LFO>Noise is added to the knob, so the bed can breathe or arrive in waves.
//
// Freeze: as it engages, the loop length is latched (the current delay time; twice that in Ping-Pong, where the
// two lines feed each other) and the delay lines recirculate their own contents unprocessed at exactly unity.
// Filter, pitch, FX, drive and reverse then work on the output only, so sweeping them over a frozen loop never
// wears it away. Engaging and releasing crossfade over 20 ms; because the crossfade blends the normal loop into
// the held one, the loop's wrap point is seamless.
//
// Spring: one spring per side (43.7 / 51.3 ms): a delay line, 24 stretched first-order allpasses (the chirp,
// after Parker & Valimaki's spring models), a one-pole low-pass and a decay gain, with a little delay
// modulation, fed with the mono sum of the repeats and normalised to about unity power (After). A resonant spring
// like that inside the feedback loop would make it run away at its resonances, so In Loop uses the same spring as
// a Schroeder allpass (flat magnitude: they smear each repeat in time without changing its level at any
// frequency), followed by the tone low-pass, crossfaded in by Space: the loop's gain can never exceed Feedback.
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
    kEnvCutoff, kEnvFx, kEnvSpeed, kDuck,
    kFreeze, kSpace, kSpringDecay, kSpringTone, kDrip, kSpacePos, kCharacter, kAge,
    kHead2, kHeadRatio, kHead2Pan, kTube, kLevel, kEchoBase
};
constexpr int kEchoBars = 64;              // bars of the echo display, one parameter each
constexpr int kEchoLevels = 16;            // heights per bar
constexpr int kEchoColours = 3;            // which head fed the bar: head 1, head 2, or both
// Film strip: per colour, a bank of dim heights then a bank of lit ones. A bar keeps its colour as it dims,
// so the whole trail shows which head each peak came off.
constexpr int kEchoBank = 2 * kEchoLevels;
constexpr int kEchoFrames = kEchoColours * kEchoBank;
// Appended in 1.9. Parameter order is permanent, so these sit after the display parameters.
constexpr int kNoise = kEchoBase + kEchoBars;    // 110
constexpr int kNoiseType = kNoise + 1;           // 111
constexpr int kLfoNoise = kNoiseType + 1;        // 112
// Appended in 2.0, for the same reason.
constexpr int kMains = kLfoNoise + 1;            // 113
constexpr int kGlide = kMains + 1;               // 114
constexpr int kCross = kGlide + 1;               // 115
constexpr int kNumParams = kCross + 1;

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
enum Character { kCharDigital = 0, kCharTape, kCharBbd };
enum NoiseType { kNoiseHiss = 0, kNoiseAir, kNoiseCrackle, kNoiseHum, kNoiseRain };
constexpr float kNoiseCeilingDb = -30.0f;   // Noise at 100%, measured in the loop at Feedback 0
// Per-type trims so the five sources sound about as loud as each other at the same Noise setting (measured:
// every shape peaks within 1.5x of the figure below, which is what the gate leans on).
constexpr float kNoiseTrim[5] = { 1.0f, 2.5f, 4.2f, 0.8f, 2.2f };
constexpr float kNoiseGateFloor = 0.0032f;    // about -50 dBFS of activity opens the gate fully...
constexpr float kNoiseGateCeiling = 0.05f;    // ...and it never charges past here, so the fade is always ~3 s
constexpr float kMainsHz[2] = { 60.0f, 50.0f };   // Mains: 60 Hz (the Americas, Japan) or 50 Hz
constexpr float kHumDrift = 0.0012f;              // how far the mains frequency wanders: +-0.12%
constexpr float kGlideDefaultMs = 150.0f;         // Glide: the fixed tape-style value every project before 2.0 had
constexpr float kGlideMaxMs = 2000.0f;

// Head 2's position as a fraction of the delay time.
constexpr int kNumHeadRatios = 9;
constexpr float kHeadRatioValues[kNumHeadRatios] =
    { 0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 4.0f / 3.0f, 1.5f, 2.0f };
constexpr int kTubeFrames = 16;          // tube glow film strip
constexpr float kEchoFloorDb = -48.0f;   // repeat level that fills the first bar step
constexpr float kTubeFloorDb = -45.0f;   // level that lights the first frame

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
constexpr float kSpringOutGain = 3.5f;   // spring output trim (After: on top of power normalisation)

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

// Spring reverb building blocks.
constexpr int kSpringStages = 36;        // stretched allpasses per spring
constexpr int kSpringStretch = 4;        // their delay in samples (concentrates the chirp lower down)
constexpr int kSpringsPerChannel = 1;   // one spring per side (two per side doubled the CPU for little gain)
constexpr float kSpringMaxSeconds = 0.075f;
constexpr float kSpringLengthsMs[2][kSpringsPerChannel] = { { 43.7f }, { 51.3f } };

struct StretchedAllpass   // (a + z^-M) / (1 + a z^-M)
{
    float x[kSpringStretch] = {}, y[kSpringStretch] = {};
    int i = 0;
    float process(float in, float a)
    {
        const float out = a * in + x[i] - a * y[i];
        x[i] = in; y[i] = out;
        if (++i == kSpringStretch) i = 0;
        return out;
    }
    void reset() { for (int k = 0; k < kSpringStretch; ++k) x[k] = y[k] = 0.0f; i = 0; }
};

struct Spring
{
    DelayLine line;
    StretchedAllpass ap[kSpringStages];
    float lp = 0.0f, length = 1000.0f, modPhase = 0.0f, modInc = 0.0f;
    float g = 0.5f, norm = 1.0f;          // decay gain and output normalisation, set per block
    void reset() { line.reset(); for (StretchedAllpass& s : ap) s.reset(); lp = 0.0f; }
    float process(float in, float a, float lpCoef, float modDepth)
    {
        modPhase = advance(modPhase, modInc);
        float v = line.readLinear(length + modDepth * (1.0f + sine01(modPhase)));
        for (StretchedAllpass& s : ap) v = s.process(v, a);
        lp += lpCoef * (v - lp);
        line.write(in + g * lp);
        return lp * norm;
    }
    // The same spring as a Schroeder allpass, (A - g) / (1 - g A) with A = delay + dispersion: flat magnitude, so
    // inside the feedback loop it smears each repeat without changing the loop's gain at any frequency.
    float processAllpass(float in, float a, float modDepth)
    {
        modPhase = advance(modPhase, modInc);
        float d = line.readLinear(length + modDepth * (1.0f + sine01(modPhase)));
        for (StretchedAllpass& s : ap) d = s.process(d, a);
        const float w = in + g * d;
        line.write(w);
        return d - g * w;
    }
};

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
    Spring springs[kSpringsPerChannel];
    OnePoleHP springHp;          // springs do not do bass
    float springTone = 0.0f;     // In Loop: tone low-pass on the spring part
    float loopOut = 0.0f;        // this channel's processed repeat, fed back into the delay
    OnePoleLP tapeHf, bbdLp1, bbdLp2;   // delay character
    OnePoleHP tapeLf;
    float bbdHeld = 0.0f, bbdPhase = 1.0f;
    PinkNoise pink;              // Noise: Air
    float crackle = 0.0f;        // Noise: Crackle / Rain - the decaying impulse
    float crackleTone = 0.0f;    // ...and its one-pole, subtracted to leave the click
    float humDc = 0.0f;          // Noise: Hum - running average of the rectifier pulse, subtracted as its DC

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
    void clearSprings() { for (Spring& sp : springs) sp.reset(); springHp.reset(); springTone = 0.0f; }
    float spring(float in, float a, float lpCoef, float modDepth)
    {
        const float x = springHp.process(in);
        float sum = 0.0f;
        for (Spring& sp : springs) sum += sp.process(x, a, lpCoef, modDepth);
        return sum;
    }
    // In Loop: the spring as an allpass, then the tone low-pass (gain <= 1 everywhere).
    float springInLoop(float in, float a, float lpCoef, float modDepth)
    {
        float y = in;
        for (Spring& sp : springs) y = sp.processAllpass(y, a, modDepth);
        springTone += lpCoef * (y - springTone);
        return springTone;
    }
    void clear()
    {
        delay.reset();
        filter.reset();
        clearFx();
        clearPitch();
        reverseBuf.reset();
        clearSprings();
        tapeHf.reset(); tapeLf.reset(); bbdLp1.reset(); bbdLp2.reset(); bbdHeld = 0.0f; bbdPhase = 1.0f;
        pink.reset(); crackle = 0.0f; crackleTone = 0.0f; humDc = 0.0f;
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
        addParam(boolParam  ("freeze",   "Freeze",    false));
        addParam(floatParam ("space",    "Space",     0.0f, 100.0f,   0.0f,   "%", 0));
        addParam(logParam   ("sdecay",   "Spring Decay", 0.3f, 6.0f,  1.8f,   "s", 1));
        addParam(floatParam ("stone",    "Spring Tone", 0.0f, 100.0f, 50.0f,  "%", 0));
        addParam(floatParam ("drip",     "Drip",      0.0f, 100.0f,   50.0f,  "%", 0));
        addParam(choiceParam("spos",     "Space Position", {"After", "In Loop"}, 0));
        addParam(choiceParam("char",     "Character", {"Digital", "Tape", "BBD"}, kCharDigital));
        addParam(floatParam ("age",      "Age",       0.0f, 100.0f,   50.0f,  "%", 0));
        addParam(floatParam ("head2",    "Head 2",    0.0f, 100.0f,   0.0f,   "%", 0));
        addParam(choiceParam("hratio",   "Head Ratio", {"1/4", "1/3", "1/2", "2/3", "3/4", "1/1", "4/3", "3/2",
                                                        "2/1"}, 3));
        addParam(floatParam ("h2pan",    "Head 2 Pan", -100.0f, 100.0f, 0.0f,  "%", 0));
        addParam(boolParam  ("tube",     "Display",   true));
        Param meter = floatParam("level", "Level", 0.0f, 1.0f, 0.0f, "", 2);   // written by the plugin, for the tube
        meter.automatable = false;
        meter.saved = false;          // written by the plugin every block; never restored from a project
        addParam(meter);
        for (int i = 0; i < kEchoBars; ++i)   // the ECHO display: one parameter per bar, same treatment
        {
            char id[12], name[12];
            std::snprintf(id, sizeof id, "bar%02d", i);
            std::snprintf(name, sizeof name, "Echo %d", i + 1);
            Param bar = floatParam(id, name, 0.0f, 1.0f, 0.0f, "", 2);
            bar.automatable = false;
            bar.saved = false;
            addParam(bar);
        }
        // 1.9, appended after the display parameters so earlier projects keep their numbering.
        addParam(floatParam ("noise",    "Noise",     0.0f, 100.0f,   0.0f,   "%", 0));
        addParam(choiceParam("ntype",    "Noise Type", {"Hiss", "Air", "Crackle", "Hum", "Rain"}, kNoiseHiss));
        addParam(floatParam ("lfonoise", "LFO>Noise", -100.0f, 100.0f, 0.0f,  "", 0));
        addParam(choiceParam("mains",    "Mains",     {"60 Hz", "50 Hz"}, 0));
        // Glide is skewed so the first third of the knob covers the short, musical times.
        addParam(floatParam ("glide",    "Glide",     0.0f, kGlideMaxMs, kGlideDefaultMs, "ms", 0, 0.4f));
        addParam(floatParam ("cross",    "Cross",     0.0f, 100.0f,   100.0f, "%", 0));
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
        // Freeze/Space tail (33..38):      Frz Space Decay Tone Drip Pos
        const std::vector<float> noSpace = { 0,  0,    1.8f,  50,  50,  0 };
        // Character tail (39..40):         Char Age   (0..2 = Digital/Tape/BBD)
        const std::vector<float> digital = { 0,   50 };
        // Head tail (41..45):           H2  Ratio Pan Tube Level
        const std::vector<float> oneHead = { 0,  3,    0,  1,   0 };
        // Noise tail (110..115):        Amt Type L>Noise Mains Glide Cross
        //                               (types 0..4 = Hiss/Air/Crackle/Hum/Rain; mains 0 = 60 Hz, 1 = 50 Hz)
        const std::vector<float> quiet = { 0,  0,   0,      0,    kGlideDefaultMs, 100 };
        // The display parameters (46..109) sit between the head tail and the noise tail; they are always 0.
        auto progH = [this](const char* name, std::vector<float> v, const std::vector<float>& mod,
                            const std::vector<float>& sp, const std::vector<float>& chr, const std::vector<float>& hd,
                            const std::vector<float>& nz = { 0.0f, 0.0f, 0.0f, 0.0f, kGlideDefaultMs, 100.0f })
        {
            for (const std::vector<float>* tail : { &mod, &sp, &chr, &hd }) v.insert(v.end(), tail->begin(), tail->end());
            v.resize((size_t)kEchoBase + (size_t)kEchoBars, 0.0f);
            v.insert(v.end(), nz.begin(), nz.end());
            addProgram(name, std::move(v));
        };
        auto progC = [&](const char* name, std::vector<float> v, const std::vector<float>& mod,
                         const std::vector<float>& sp, const std::vector<float>& chr,
                         const std::vector<float>& nz = { 0.0f, 0.0f, 0.0f, 0.0f, kGlideDefaultMs, 100.0f })
        {
            progH(name, std::move(v), mod, sp, chr, oneHead, nz);
        };
        auto progS = [&](const char* name, std::vector<float> v, const std::vector<float>& mod, const std::vector<float>& sp)
        {
            progC(name, std::move(v), mod, sp, digital);
        };
        auto prog = [&](const char* name, std::vector<float> v, const std::vector<float>& mod) { progS(name, std::move(v), mod, noSpace); };
        //                            Out Sync Div Time    Fb   Mix PP Wob Lim  Flt Cut     Res Drv  FX Amt Rate  PMd Pitch    PMix Rev In
        prog("Dub Echo",         { 0, 1,  9, D(9),   60,  35, 1, 15, 30,  1, 1800,   35, 25,  0, 50, 30,  0, 0,        100, 0, 0 }, noMod);
        prog("Siren Sweep",      { 0, 1,  7, D(7),   68,  40, 1, 10, 60,  2, 1200,   75, 40,  0, 50, 30,  0, 0,        100, 0, 0 }, noMod);
        prog("Chorus Wash",      { 0, 1, 10, D(10),  65,  35, 1, 25, 30,  1, 4000,   20, 15,  1, 60, 25,  0, 0,        100, 0, 0 }, noMod);
        prog("Crushed Repeats",  { 0, 1,  6, D(6),   70,  35, 1,  5, 40,  3,  300,   30, 30,  4, 65, 40,  0, 0,        100, 0, 0 }, noMod);
        prog("Flange Spiral",    { 0, 1,  8, D(8),   80,  40, 1, 10, 40,  1, 6000,   40, 20,  2, 70, 20,  0, 0,        100, 0, 0 }, noMod);
        progC("Lo-Fi Tape",      { 0, 1,  9, D(9),   55,  30, 1, 45, 30,  1, 2500,   15, 35,  5, 35, 60,  0, 0,        100, 0, 0 }, noMod,
                                 noSpace, { 1, 75 }, { 48, kNoiseHiss, 0, 0, 260, 100 });
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
        progC("Seasick Tape",      { 0, 1, 10, D(10),  60,  35, 1, 30, 30,  1, 3500,   20, 30,  5, 20, 40,  0, 0,        100, 0, 0 },
                                   { 6,    1,   LR(2), 120,  0,    0.08f, 0,  40,     0,    0,   30,  0 }, noSpace, { 1, 55 },
                                   { 34, kNoiseAir, 0, 0, 520, 100 });
        prog("Ducked Dub",         { 0, 1,  9, D(9),   72,  50, 1, 15, 35,  1, 2200,   35, 25,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   { 0,    1,   LR(3),  90,  0,    0,    0,   0,     0,    0,   45, 75 });
        prog("Breathing Flange",   { 0, 1,  8, D(8),   75,  40, 1, 10, 40,  1, 6000,   35, 20,  2, 35, 15,  0, 0,        100, 0, 0 },
                                   { 1,    1,   LR(2),  90,  0,    0,    55,  0,     0,    0,   30,  0 });
        progS("Spring Dub",        { 0, 1,  9, D(9),   60,  40, 1, 15, 30,  1, 2000,   35, 25,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   noMod, { 0, 45, 2.4f, 45, 70, 0 });
        progS("Washout",           { 0, 1, 10, D(10),  70,  45, 1, 15, 35,  1, 3000,   25, 25,  1, 40, 20,  0, 0,        100, 0, 0 },
                                   noMod, { 0, 60, 3.0f, 40, 60, 1 });
        progC("Tape Echo",         { 0, 1,  9, D(9),   65,  35, 1, 20, 30,  1, 5000,   20, 30,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   noMod, noSpace, { 1, 45 }, { 26, kNoiseHiss, 0, 0, 220, 100 });
        progC("Bucket Brigade",    { 0, 1, 10, D(10),  70,  40, 1, 10, 35,  0, 1800,   35, 40,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   noMod, noSpace, { 2, 50 }, { 30, kNoiseHiss, 0, 0, 150, 100 });
        progH("Twin Heads",        { 0, 1,  9, D(9),   62,  40, 1, 18, 30,  1, 4000,   25, 30,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   noMod, noSpace, { 1, 45 }, { 45, 3, 55, 1, 0 });      // 1/8D + a straight 1/8
        progH("Triplet Machine",   { 0, 1, 10, D(10),  68,  40, 0, 12, 35,  1, 3000,   30, 35,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   noMod, noSpace, { 1, 60 }, { 55, 1, -60, 1, 0 });     // 1/4 + a 1/4 triplet
        progC("Dusty 45",          { 0, 1,  9, D(9),   66,  38, 1, 22, 35,  1, 3200,   25, 30,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   noMod, noSpace, { 1, 60 }, { 55, kNoiseCrackle, 0, 0, 300, 100 });
        progC("Rain on the Roof",  { 0, 1, 10, D(10),  72,  42, 1, 12, 35,  1, 2400,   30, 25,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   { 0,    1,   LR(1),  90,  0,    0,    0,   0,     0,    0,   30,  0 },
                                   noSpace, digital, { 42, kNoiseRain, 45, 0, 150, 100 });
        // Glide at 900 ms: the repeats take almost a second to arrive at a new delay time, warping on the way.
        progC("Slow Tide",         { 0, 1, 11, D(11),  70,  40, 1, 20, 35,  1, 2600,   30, 28,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   noMod, noSpace, { 1, 55 }, { 22, kNoiseHiss, 0, 0, 900, 100 });
        // Cross at 55%: the repeats lean across instead of snapping side to side, so the pattern wanders.
        progC("Half Crossed",      { 0, 1,  9, D(9),   66,  38, 1, 14, 30,  1, 3400,   28, 25,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   noMod, noSpace, { 1, 40 }, { 18, kNoiseHiss, 0, 0, 150, 55 });
        // Mains buzz: Age drives how rough it gets, so this one leans on it.
        progC("Bad Ground",        { 0, 1,  8, D(8),   72,  42, 1, 16, 40,  1, 2000,   40, 35,  0, 50, 30,  0, 0,        100, 0, 0 },
                                   noMod, noSpace, { 1, 85 }, { 46, kNoiseHum, 0, 0, 150, 100 });
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
            c.springHp.setCutoff(150.0f, sampleRate);
        }
        ch_[0].pink.reseed(0x7654321u);        // different seeds so Air is stereo, not a mono wash
        ch_[1].pink.reseed(0x2BD1E99Du);
        for (int c = 0; c < 2; ++c)
        {
            for (int k = 0; k < kSpringsPerChannel; ++k)
            {
                Spring& sp = ch_[c].springs[k];
                sp.line.prepare((int)(sampleRate * kSpringMaxSeconds) + 4);
                sp.length = kSpringLengthsMs[c][k] * 0.001f * sr_;
                sp.modInc = (0.6f + 0.23f * (float)k + 0.11f * (float)c) / sr_;
                sp.modPhase = 0.37f * (float)(2 * c + k);
            }
        }
        delayTime_.setTime(kGlideDefaultMs, sampleRate);   // Glide; process() re-times it when the knob moves
        glideMs_ = -1.0f;
        cross_.setTime(20.0f, sampleRate);
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
        space_.setTime(30.0f, sampleRate);
        lfoSmooth_ = 1.0f - std::exp(-1.0f / (0.004f * sr_));    // 4 ms: softens steps and saw resets
        duckSmooth_ = 1.0f - std::exp(-1.0f / (0.005f * sr_));
        peakRelease_ = 1.0f - std::exp(-1.0f / (0.030f * sr_));
        meterRelease_ = 1.0f - std::exp(-1.0f / (0.150f * sr_));   // VU-ish fall for the tube
        reverseMix_.setTime(25.0f, sampleRate);
    }

    void reset() override
    {
        for (LoopChannel& c : ch_) c.clear();
        delayTime_.snap(targetDelaySamples(120.0));
        cross_.snap(getBool(kPingPong) ? clamp(get(kCross) * 0.01f, 0.0f, 1.0f) : 0.0f);
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
        freezeMix_ = getBool(kFreeze) ? 1.0f : 0.0f;
        freezeLen_ = (int)clamp(delayTime_.value(), 64.0f, (float)ch_[0].delay.maxDelay() - 2.0f);
        space_.snap(get(kSpace) * 0.01f);
        meterPeak_ = 0.0f; meterCounter_ = 0; lastMeterFrame_ = -1;
        echoPeak_ = 0.0f; echoTap1_ = 0.0f; echoTap2_ = 0.0f;
        echoPos_ = 0; echoBar_ = 0; echoClearPending_ = true;
        humPhase_ = 0.0f; noiseGate_ = 0.0f; humDrift_ = 1.0f;
        for (int& f : echoFrame_) f = -1;
        springWasActive_ = false;
        revPos_ = 0; revChunk_ = revPrevChunk_ = (int)clamp(delayTime_.value(), 0.02f * sr_, kReverseMaxSeconds * sr_);
        pitchMix_.snap(pitchMixTarget());
        reverseMix_.snap(getBool(kReverse) ? 1.0f : 0.0f);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // ----- per block: read parameters -----------------------------------------------------
        delayTime_.setTarget(targetDelaySamples(ctx.transport.bpm));
        // Glide: how long the delay time takes to reach a new setting. At 0 it jumps (and clicks, like a
        // digital delay); turned up, the tape has to spin up to speed and the repeats warp on the way.
        const float glideMs = clamp(get(kGlide), 0.0f, kGlideMaxMs);
        if (glideMs != glideMs_) { glideMs_ = glideMs; delayTime_.setTime(glideMs, sr_); }
        feedback_.setTarget(get(kFeedback) * 0.01f);
        mix_.setTarget(get(kMix) * 0.01f);
        output_.setTarget(dbToGain(get(kOutput)));
        input_.setTarget(inputGain());
        logCutoff_.setTarget(std::log2(get(kCutoff)));

        const bool pingPong = getBool(kPingPong);
        // Cross: how much of each side's repeat lands on the other. The switch picks the endpoint, so a
        // project made before Cross existed - Straight, or Ping-Pong with the knob at its 100% default -
        // behaves exactly as it always did.
        const float crossTarget = pingPong ? clamp(get(kCross) * 0.01f, 0.0f, 1.0f) : 0.0f;
        cross_.setTarget(crossTarget);
        const bool crossSwap = crossTarget >= 0.5f;   // which buffer holds which half of a frozen loop
        const bool tubeOn = getBool(kTube);
        // ECHO display: the window is two delay times wide, so a steady pattern of repeats stands still.
        const int echoSlice = (int)clamp(2.0f * delayTime_.target() / (float)kEchoBars, 0.004f * sr_, 0.1f * sr_);
        const float wobble = get(kWobble) * 0.01f;
        // Delay character.
        const int character = getInt(kCharacter);
        const float age = get(kAge) * 0.01f;
        // Tape leans on its compression and BBD on its clock; the hiss they used to add is the Noise knob's job.
        const float tapeSat = 1.0f + 3.5f * age, bbdSat = 1.5f + 1.5f * age;
        float bbdHoldInc = 1.0f;
        if (character == kCharTape)
        {
            for (LoopChannel& c : ch_)
            {
                c.tapeHf.setCutoff(12000.0f * std::pow(0.25f, age), sr_);
                c.tapeLf.setCutoff(30.0f + 50.0f * age, sr_);
            }
        }
        else if (character == kCharBbd)
        {
            const float stages = 4096.0f * std::pow(0.22f, age);
            const float clock = stages / std::fmax(delayTime_.target() / sr_, 0.001f);   // Hz
            bbdHoldInc = std::fmin(1.0f, clock / sr_);
            const float lpHz = clamp(0.45f * clock, 500.0f, 16000.0f);
            for (LoopChannel& c : ch_) { c.bbdLp1.setCutoff(lpHz, sr_); c.bbdLp2.setCutoff(lpHz, sr_); }
        }
        // Head 2: a second tap on the same tape, crossfaded in (weights sum to 1, so the loop gain is unchanged).
        const float head2 = get(kHead2) * 0.01f;
        const bool head2On = head2 > 0.0f;
        const float headRatio = kHeadRatioValues[clamp(getInt(kHeadRatio), 0, kNumHeadRatios - 1)];
        const float h2pan = get(kHead2Pan) * 0.01f;
        const float headMix[2] = { head2 * (h2pan > 0.0f ? 1.0f - h2pan : 1.0f),
                                   head2 * (h2pan < 0.0f ? 1.0f + h2pan : 1.0f) };

        // Noise: a hiss, wash, crackle, hum or rain bed poured into the loop just after the heads, so it is
        // filtered, aged, driven and repeated exactly like a repeat. The injected level is scaled against
        // Feedback, so the steady noise floor sits where the knob puts it instead of multiplying with each
        // pass, and the whole thing is gated by what is actually going round, so an idle track stays silent.
        const int noiseType = clamp(getInt(kNoiseType), 0, 4);
        const float noiseSet = clamp(get(kNoise) * 0.01f, 0.0f, 1.0f);
        const float lfoNoise = get(kLfoNoise) * 0.01f;
        const bool noiseOn = noiseSet > 0.0f || lfoNoise != 0.0f;
        // Successive noise samples are uncorrelated, so a loop at feedback f settles at inject / sqrt(1 - f^2).
        // Injecting sqrt(1 - f^2) therefore lands the steady bed on the same level whatever Feedback is doing.
        const float fbNow = clamp(feedback_.target(), 0.0f, 0.98f);
        const float noiseInject = std::sqrt(1.0f - fbNow * fbNow)
                                  * dbToGain(kNoiseCeilingDb) * kNoiseTrim[noiseType];
        // Mains hum. Real mains wanders a little either side of nominal; letting it drift keeps the hum
        // from standing perfectly still against the music. One update a block is slow enough to be smooth.
        humDrift_ += 0.05f * (1.0f + kHumDrift * rng_.nextBipolar() - humDrift_);
        const float humInc = kMainsHz[clamp(getInt(kMains), 0, 1)] * humDrift_ / sr_;
        // How rough the mains sounds: worn, badly grounded gear buzzes, clean gear drones. There is always
        // a little of it, and Age opens it up.
        const float humBuzz = 0.25f + 0.75f * age;
        const float crackleProb = (noiseType == kNoiseRain ? 1800.0f : 28.0f) / sr_;
        const float crackleDecay = std::exp(-1.0f / ((noiseType == kNoiseRain ? 0.0005f : 0.0018f) * sr_));
        const float noiseGateRelease = 1.0f - std::exp(-1.0f / (0.6f * sr_));
        // The gate watches the loop as well as the input, and the loop carries the bed itself, so the bed's
        // own level is subtracted from what the loop reports: noise can never hold its own gate open, while
        // anything louder than the bed opens it at once.
        const float noiseMax = clamp(noiseSet + std::fabs(lfoNoise), 0.0f, 1.0f);
        // 3.5x the steady level, not 1.5x: noise that has been round the loop a few times keeps the same
        // average but grows a longer crest, and anything short of its real peaks lets the bed hold its own
        // gate open at high Feedback. Repeats loud enough to hear still clear this easily.
        const float noiseBedPeak = 3.5f * dbToGain(kNoiseCeilingDb) * kNoiseTrim[noiseType] * noiseMax * noiseMax;
        if (!noiseOn) noiseGate_ = 0.0f;

        const float wowDepth = (wobble * 0.0040f + (character == kCharTape ? age * 0.0025f : 0.0f)) * sr_;   // slow drift
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

        // Freeze: latch the loop length as it starts to engage.
        const float freezeTarget = getBool(kFreeze) ? 1.0f : 0.0f;
        if (freezeTarget > 0.0f && freezeMix_ <= 0.0f)
            freezeLen_ = (int)clamp(std::round(delayTime_.value()), 64.0f, maxDelay);
        const float freezeStep = 1.0f / (0.020f * sr_);

        // Spring reverb settings.
        space_.setTarget(get(kSpace) * 0.01f);
        const bool springActive = space_.target() > 0.0f || space_.value() > 1e-4f;
        const bool springInLoop = getInt(kSpacePos) == 1;
        if ((!springActive && springWasActive_) || springInLoop != lastSpringInLoop_)   // the two modes use the
            for (LoopChannel& c : ch_) c.clearSprings();                                // springs differently
        springWasActive_ = springActive;
        lastSpringInLoop_ = springInLoop;
        // Negative coefficients delay the lows most: the descending "drip" (about 2..16 ms of spread).
        const float springA = -(0.15f + 0.65f * get(kDrip) * 0.01f);
        const float springLp = 1.0f - std::exp(-6.2831853f * 1500.0f * std::pow(6.0f, get(kSpringTone) * 0.01f) / sr_);
        const float springMod = 0.0002f * sr_;
        if (springActive)
        {
            const float decay = get(kSpringDecay);
            for (LoopChannel& c : ch_)
                for (Spring& sp : c.springs)
                {
                    sp.g = std::pow(10.0f, -3.0f * (sp.length / sr_) / decay);
                    sp.norm = std::sqrt(1.0f - sp.g * sp.g) * kSpringOutGain;   // After: about unity power
                }
        }

        // One sample of the chosen noise source for one channel. The two channels draw from the same
        // generator in turn, which decorrelates them; Air and the impulses keep their state per channel.
        auto noiseOf = [&](LoopChannel& ch, int c) -> float
        {
            switch (noiseType)
            {
                case kNoiseAir:
                    return ch.pink.process();
                case kNoiseCrackle:
                case kNoiseRain:
                {
                    if (rng_.nextFloat() < crackleProb)
                        ch.crackle = rng_.nextBipolar() * (0.35f + 0.65f * rng_.nextFloat());
                    const float out = ch.crackle;
                    ch.crackle *= crackleDecay;
                    ch.crackleTone += 0.45f * (out - ch.crackleTone);   // subtract the body, keep the click
                    return (out - ch.crackleTone) * 2.2f;
                }
                case kNoiseHum:
                {
                    // Two things at once. The body is the transformer: the mains fundamental and a couple of
                    // harmonics, which is the part that drones. On top of it sits the rectifier ripple - a
                    // narrow spike twice per cycle, so 120 Hz on 60 Hz mains - and that is what buzzes. Age
                    // trades one for the other. A little hiss underneath stops it sounding like a test tone.
                    const float p = c ? advance(humPhase_, 0.33f) : humPhase_;
                    const float p2 = advance(p, p), p3 = advance(p2, p);
                    const float body = 0.80f * sine01(p) + 0.30f * sine01(p2) + 0.14f * sine01(p3);
                    float u = 0.5f + 0.5f * sine01(advance(p2, 0.25f));   // 1 at the top of each half cycle
                    u *= u; u *= u; u *= u;                               // ^8: a narrow, harmonic-rich spike
                    ch.humDc += 0.0015f * (u - ch.humDc);                 // ...with its own DC taken back off
                    return body * (1.0f - 0.45f * humBuzz) + (u - ch.humDc) * (3.2f * humBuzz)
                           + 0.05f * rng_.nextBipolar();
                }
                default:
                    return rng_.nextBipolar();
            }
        };

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

            const float space = space_.process();

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

            // Noise bed level, per channel: the knob plus LFO>Noise, squared for a fine bottom end,
            // then opened by the activity gate.
            float noiseGain[2] = { 0.0f, 0.0f };
            if (noiseOn)
            {
                humPhase_ = advance(humPhase_, humInc);
                const float gate = std::fmin(1.0f, noiseGate_ * (1.0f / kNoiseGateFloor));
                for (int c = 0; c < 2; ++c)
                {
                    const float a = clamp(noiseSet + lfoNoise * lfo_[c].out, 0.0f, 1.0f);
                    noiseGain[c] = a * a * noiseInject * gate;
                }
            }

            // The loop: read -> filter -> [pitch -> FX, level-matched] -> drive -> DC block.
            float wet[2];
            float powIn = 0.0f, powOut = 0.0f;
            float tap1Peak = 0.0f, tap2Peak = 0.0f;     // what each head contributed, for the echo display
            const float lfo[2] = { lfoL, lfoR };
            const float d[2] = { dL, dR };
            for (int c = 0; c < 2; ++c)
            {
                LoopChannel& ch = ch_[c];
                const float head1 = ch.delay.readLinear(d[c]);
                float x = head1;
                if (head2On)
                {
                    const float x2 = ch.delay.readLinear(clamp(d[c] * headRatio, 2.0f, maxDelay));
                    x += (x2 - x) * headMix[c];
                    const float t2 = std::fabs(x2) * headMix[c];
                    if (t2 > tap2Peak) tap2Peak = t2;
                }
                const float t1 = std::fabs(head1) * (1.0f - headMix[c]);
                if (t1 > tap1Peak) tap1Peak = t1;
                if (noiseOn) x += noiseGain[c] * noiseOf(ch, c);
                if (character == kCharTape)
                {
                    x = ch.tapeLf.process(ch.tapeHf.process(x));
                    x = fastTanh(tapeSat * x) / tapeSat;
                }
                else if (character == kCharBbd)
                {
                    ch.bbdPhase += bbdHoldInc;
                    if (ch.bbdPhase >= 1.0f) { ch.bbdPhase -= 1.0f; ch.bbdHeld = x; }
                    x = ch.bbdLp2.process(ch.bbdLp1.process(ch.bbdHeld));
                    x = (fastTanh(bbdSat * x + 0.1f) - 0.0997f) / (bbdSat * 0.990f);
                }

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

                if (springActive && springInLoop)   // weights sum to 1 and |spring| <= 1: the loop gain cannot grow
                {
                    const float r = ch.springInLoop(x, springA, springLp, springMod);
                    x += (r - x) * space;
                }

                x = (fastTanh(driveGain * x + kBias) - tb) * driveNorm;
                x = ch.dc.process(x);
                ch.loopOut = x;
                wet[c] = x;
            }
            agcIn_ += agcFollow * (powIn - agcIn_);
            agcOut_ += agcFollow * (powOut - agcOut_);

            // Noise gate: instant attack on input or anything still going round, then a long release.
            if (noiseOn)
            {
                const float activity = std::fmax(level, std::fmax(tap1Peak, tap2Peak) - noiseBedPeak);
                noiseGate_ += (activity > noiseGate_ ? 1.0f : noiseGateRelease) * (activity - noiseGate_);
                // Capped, so the bed always fades in about the same time however loud the last hit was.
                if (noiseGate_ > kNoiseGateCeiling) noiseGate_ = kNoiseGateCeiling;
            }

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

            // Spring after the delay (on the repeats, before the wet limiter).
            if (springActive && !springInLoop)   // like a spring tank: mono in, a different spring on each side
            {
                const float mono = 0.5f * (wet[0] + wet[1]);
                for (int c = 0; c < 2; ++c) wet[c] += space * ch_[c].spring(mono, springA, springLp, springMod);
            }

            // Feed the repeats back (or, frozen, recirculate the held loop unprocessed at unity).
            const float fb = feedback_.process();
            // Cross-feed. The two feedback weights always sum to one, so crossing decides where a repeat
            // lands, not how much of it survives: measured repeat to repeat, the decay is the same -3.3 dB a
            // pass at every setting. At 0 this is two independent delays; at 1 each repeat swaps sides and
            // the input is summed to mono and started on the left, exactly the ping-pong it has always been.
            // (The input routing does change the overall level: mono-summing a one-sided source costs 6 dB
            // going in. That is ping-pong's own behaviour, not the crossing.)
            const float xo = cross_.process(), xs = 1.0f - xo;
            const float monoSend = 0.5f * (sendL + sendR);
            float writeL = sendL * xs + monoSend * xo + (ch_[0].loopOut * xs + ch_[1].loopOut * xo) * fb;
            float writeR = sendR * xs                 + (ch_[1].loopOut * xs + ch_[0].loopOut * xo) * fb;
            freezeMix_ += clamp(freezeTarget - freezeMix_, -freezeStep, freezeStep);
            if (freezeMix_ > 0.0f)
            {
                const float heldL = (crossSwap ? ch_[1] : ch_[0]).delay.readInt(freezeLen_);
                const float heldR = (crossSwap ? ch_[0] : ch_[1]).delay.readInt(freezeLen_);
                if (freezeMix_ >= 1.0f) { writeL = heldL; writeR = heldR; }          // exactly unity
                else
                {
                    const float gHeld = sine01(freezeMix_ * 0.25f), gNormal = sine01(0.25f + freezeMix_ * 0.25f);
                    writeL = writeL * gNormal + heldL * gHeld;
                    writeR = writeR * gNormal + heldR * gHeld;
                }
            }
            ch_[0].delay.write(writeL);
            ch_[1].delay.write(writeR);

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
            if (tubeOn)
            {
                const float peakWet = std::fmax(std::fabs(wL), std::fabs(wR));
                const float tubeWet = peakWet * wetGain;
                meterPeak_ = tubeWet > meterPeak_ ? tubeWet : meterPeak_ + meterRelease_ * (tubeWet - meterPeak_);
                if (peakWet > echoPeak_) echoPeak_ = peakWet;
                if (tap1Peak > echoTap1_) echoTap1_ = tap1Peak;
                if (tap2Peak > echoTap2_) echoTap2_ = tap2Peak;
                if (++echoPos_ >= echoSlice) { echoPos_ = 0; echoAdvance_ = true; }
            }

            outL[n] = softKnee((dryL * dryGain + wL * wetGain) * gain, kOutKnee, kOutCeiling);
            outR[n] = softKnee((dryR * dryGain + wR * wetGain) * gain, kOutKnee, kOutCeiling);
        }

        // The ECHO bars: the finished bar goes to its measured height and lights up, the one before it dims.
        if (tubeOn && echoClearPending_)
        {
            echoClearPending_ = false;
            for (int i = 0; i < kEchoBars; ++i) setEchoBar(i, 0);
        }
        if (echoAdvance_)
        {
            echoAdvance_ = false;
            const float db = echoPeak_ > 1e-6f ? 20.0f * std::log10(echoPeak_) : -120.0f;
            const int level = (int)clamp(std::round((db - kEchoFloorDb) / -kEchoFloorDb * (kEchoLevels - 1)),
                                         0.0f, (float)(kEchoLevels - 1));
            // Which head fed this slice: mostly head 1, mostly head 2, or both.
            const float tapSum = echoTap1_ + echoTap2_;
            int colour = 0;
            if (tapSum > 1e-9f)
            {
                const float share = echoTap2_ / tapSum;
                colour = share < 0.35f ? 0 : (share > 0.65f ? 1 : 2);
            }
            echoPeak_ = 0.0f; echoTap1_ = 0.0f; echoTap2_ = 0.0f;
            const int previous = echoBar_ > 0 ? echoBar_ - 1 : kEchoBars - 1;
            setEchoBar(previous, echoFrame_[previous] % kEchoBank >= kEchoLevels
                                 ? echoFrame_[previous] - kEchoLevels : -1);
            setEchoBar(echoBar_, colour * kEchoBank + kEchoLevels + level);
            echoBar_ = echoBar_ + 1 < kEchoBars ? echoBar_ + 1 : 0;
        }

        // Tube: tell the host the new level, at most 25 times a second and only when the frame changes.
        if (tubeOn)
        {
            meterCounter_ += ctx.numFrames;
            if (meterCounter_ >= (int)(0.04f * sr_))
            {
                meterCounter_ = 0;
                const float db = meterPeak_ > 1e-6f ? 20.0f * std::log10(meterPeak_) : -120.0f;
                const int frame = (int)clamp(std::round((db - kTubeFloorDb) / -kTubeFloorDb * (kTubeFrames - 1)),
                                             0.0f, (float)(kTubeFrames - 1));
                if (frame != lastMeterFrame_)
                {
                    lastMeterFrame_ = frame;
                    setNormalised(kLevel, (float)frame / (float)(kTubeFrames - 1));
                    notifyHostParameterChanged(kLevel);
                }
            }
        }
        else if (lastMeterFrame_ != 0)
        {
            lastMeterFrame_ = 0; meterPeak_ = 0.0f;
            setNormalised(kLevel, 0.0f);
            notifyHostParameterChanged(kLevel);
            echoClearPending_ = true;          // refill the bars when the display comes back on
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
        info.version      = 2000;            // 2.0.0.0
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

    // One ECHO bar: frame 0..15 is a finished bar, 16..31 the same height lit (the newest). -1 leaves it alone.
    void setEchoBar(int bar, int frame)
    {
        if (frame < 0 || frame == echoFrame_[bar]) return;
        echoFrame_[bar] = frame;
        setNormalised(kEchoBase + bar, (float)frame / (float)(kEchoFrames - 1));
        notifyHostParameterChanged(kEchoBase + bar);
    }

    // Input: -30 dB at the bottom of the knob means off, so a throw can start from silence.
    float inputGain() const { const float db = get(kInput); return db <= -29.95f ? 0.0f : dbToGain(db); }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index == kInput && plainValue <= -29.95f) { out = "-inf dB"; return true; }
        char mb[32];
        if (index == kGlide)
        {
            if (plainValue < 1.0f) out = "Instant";
            else { std::snprintf(mb, sizeof(mb), "%.0f ms", (double)plainValue); out = mb; }
            return true;
        }
        if (index == kCross)
        {
            if (plainValue < 0.5f) out = "Straight";
            else if (plainValue > 99.5f) out = "Ping-Pong";
            else { std::snprintf(mb, sizeof(mb), "%.0f %%", (double)plainValue); out = mb; }
            return true;
        }
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
        if (index == kLfoFx || index == kEnvFx || index == kLfoTime || index == kLfoNoise)
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
    Smoother delayTime_, feedback_, mix_, output_, input_, logCutoff_, cross_;
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
    float freezeMix_ = 0.0f;
    int freezeLen_ = 1000;
    Smoother space_;
    float meterPeak_ = 0.0f, meterRelease_ = 0.01f;
    int meterCounter_ = 0, lastMeterFrame_ = -1;
    float echoPeak_ = 0.0f, echoTap1_ = 0.0f, echoTap2_ = 0.0f;
    float humPhase_ = 0.0f, noiseGate_ = 0.0f;
    float humDrift_ = 1.0f;          // slow wander of the mains frequency, updated once a block
    float glideMs_ = -1.0f;          // last Glide applied to delayTime_, so setTime() is not called every block
    int echoPos_ = 0, echoBar_ = 0, echoFrame_[kEchoBars] = {};
    bool echoAdvance_ = false, echoClearPending_ = true;
    bool springWasActive_ = false, lastSpringInLoop_ = false;
    float env_ = 0.0f, envNorm_ = 0.0f, peakRelease_ = 0.001f, duckGain_ = 1.0f, duckTarget_ = 1.0f, duckSmooth_ = 0.01f;
};

MPC_PLUGIN_ENTRY(Undertow)
