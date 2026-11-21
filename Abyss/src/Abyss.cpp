// Abyss - a slowly evolving drone and pad instrument for the MPC, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// Hold one note and it starts as a near-pure sine, then over "Evolve" seconds opens up into a
// deep, moving pad: more harmonics, a drifting formant-like "bloom", a fifth layer, high
// shimmering partials, breathy air and a long reverb. After it has opened it keeps moving:
// every partial wanders in level and pitch, and a slow cycle (free, or locked to the song)
// sweeps the bloom, the brightness and the stereo image.
//
// SIGNAL FLOW (per voice, 6 voices)
//
//   24 sine partials, each with its own slow random walk in level and pitch:
//     Main     harmonics 1..10            -- brightness roll-off x bloom peak x drift
//     Twins    harmonics 1..3, detuned    -- chorus-like beating ("Detune")
//     Sub      one octave down            -- "Sub"
//     Interval root, x2, x3 of "Interval" -- "Int Level" (a fifth by default), fades in as it evolves
//     Shimmer  harmonics 12..28           -- random glints, fade in late ("Shimmer")
//   "Stretch" bends all ratios by a power law (bells / blur); "Hollow" drops even harmonics.
//   Main/Twins/Interval are normalised to a steady loudness, so brightness and bloom change the
//   colour of the sound, not its level. x envelope (Attack / Release) x velocity.
//   Partials are summed into three buses (centre, A = odd, B = even); A and B are panned apart by
//   "Width" and sway with the motion cycle. "Orbit" sends A and B round each other instead: the
//   half behind is quieter, darker (a one-pole low-pass per bus) and its bloom lower.
//
// OUTPUT: voices + Air (two band-passed noises whose level follows the voices) -> dry
//         dry -> high-pass 150 Hz -> 8-line feedback delay network reverb ("Space", "Size",
//         "Space Tone") -> dry/wet mix -> Volume -> soft clip with a -0.3 dBFS ceiling.
//
// EVOLUTION: each voice has an "unfold" value that rises from 0 to 1 over Evolve seconds after
//   note-on. It opens the brightness (from about one harmonic to the Bright setting), deepens
//   the bloom and the drift, and brings in the interval (from ~15 %) and the shimmer (from ~35 %).
//   With Evolve Dir = Sink the brightness runs the other way and the shimmer fades out.
//
// MOTION: one slow cycle (Sine, Drift, Swell or Tide; "Cycle" seconds, or "Cycle Div" bars when Sync is on; locked to
//   the song position while the MPC plays). Each voice reads it with its own phase offset.
//   "Motion" sets how far it moves the bloom centre, the brightness and the stereo sway.
//
// CPU: everything that needs a transcendental function runs at control rate (every 64 samples,
//   1.45 ms at 44.1 kHz): envelopes, drift, spectrum, pitch. The per-sample work is one table
//   look-up, one multiply-add and one ramp per sounding partial (silent layers and partials
//   above 18 kHz are skipped), plus the reverb (8 delay lines) and two noise filters.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/noise.h"
#include "dsp/smoother.h"
#include "dsp/svf.h"
#include "dsp/tempo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace mpc::dsp;

namespace abyss {

constexpr int kNumVoices = 6;
constexpr int kNumSlots  = 24;     // sine partials per voice
constexpr int kCtrl      = 64;     // samples per control tick
constexpr int kTableBits = 11;     // 2048-point sine table
constexpr int kTableSize = 1 << kTableBits;

// Layer names carry their own prefix: inside the class, kSub / kFifth are parameter numbers.
enum Layer { kLayerMain = 0, kLayerTwin, kLayerSub, kLayerFifth, kLayerShim };
enum Bus { kBusC = 0, kBusA, kBusB };

// The partial layout of one voice. ratio = frequency / note frequency; shape = the harmonic number
// the brightness and bloom curves see; twin = +1 / -1 for the detune direction of a twin.
struct Slot { float ratio; int layer; float shape; int bus; int twin; };
static const Slot kSlots[kNumSlots] = {
    { 1.0f, kLayerMain, 1.0f, kBusC, 0}, { 2.0f, kLayerMain, 2.0f, kBusB, 0}, { 3.0f, kLayerMain, 3.0f, kBusA, 0},
    { 4.0f, kLayerMain, 4.0f, kBusB, 0}, { 5.0f, kLayerMain, 5.0f, kBusA, 0}, { 6.0f, kLayerMain, 6.0f, kBusB, 0},
    { 7.0f, kLayerMain, 7.0f, kBusA, 0}, { 8.0f, kLayerMain, 8.0f, kBusB, 0}, { 9.0f, kLayerMain, 9.0f, kBusA, 0},
    {10.0f, kLayerMain,10.0f, kBusB, 0},
    { 1.0f, kLayerTwin, 1.0f, kBusA, +1}, { 2.0f, kLayerTwin, 2.0f, kBusA, -1}, { 3.0f, kLayerTwin, 3.0f, kBusB, +1},
    { 0.5f, kLayerSub,  0.5f, kBusC, 0},
    { 1.5f, kLayerFifth,1.5f, kBusA, 0}, { 3.0f, kLayerFifth,3.0f, kBusB, 0}, { 4.5f, kLayerFifth,4.5f, kBusA, 0},
    {12.0f, kLayerShim,12.0f, kBusA, 0}, {14.0f, kLayerShim,14.0f, kBusB, 0}, {16.0f, kLayerShim,16.0f, kBusA, 0},
    {18.0f, kLayerShim,18.0f, kBusB, 0}, {20.0f, kLayerShim,20.0f, kBusA, 0}, {24.0f, kLayerShim,24.0f, kBusB, 0},
    {28.0f, kLayerShim,28.0f, kBusA, 0},
};

// Reverb delay lengths in samples at 44.1 kHz (35..93 ms, mutually prime).
static const int kFdnLen[8] = {1559, 1907, 2243, 2467, 2851, 3307, 3637, 4111};

static const char* const kCycleDivNames[] = {"1 bar", "2 bars", "4 bars", "8 bars", "16 bars", "32 bars"};
static const float kCycleDivBars[] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};

// Orbit speed: orbits per motion cycle (so it follows Sync and Cycle Div too).
static const char* const kOrbitSpeedNames[] = {"1/2", "1x", "2x", "3x", "4x", "6x", "8x"};
static const float kOrbitRatios[] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
constexpr int kNumOrbitSpeeds = 7;

// The interval layer (was "Fifth" in 1.0): its root as a ratio of the note, in just intonation.
static const char* const kIntervalNames[] = {"Fifth", "Fourth", "Octave", "Maj 3rd", "Min 3rd", "Min 7th", "Maj 7th", "9th"};
static const float kIntervalRatios[] = {1.5f, 4.0f / 3.0f, 2.0f, 1.25f, 1.2f, 1.8f, 1.875f, 2.25f};
constexpr int kNumIntervals = 8;

enum MotionShape { kShapeSine = 0, kShapeDrift, kShapeSwell, kShapeTide };

inline float smoothstep(float a, float b, float x)
{
    const float t = clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// One sine partial for n samples: table look-up with linear interpolation and a linear amplitude
// ramp. This is the only per-sample work per partial.
inline void renderPartial(const float* tab, uint32_t& phase, uint32_t inc, float& amp, float dAmp,
                          float* out, int n)
{
    uint32_t p = phase;
    float a = amp;
    constexpr int kFracBits = 32 - kTableBits;
    constexpr uint32_t kFracMask = (1u << kFracBits) - 1u;
    constexpr float kFracScale = 1.0f / (float)(1u << kFracBits);
    for (int i = 0; i < n; ++i)
    {
        const uint32_t idx = p >> kFracBits;
        const float fr = (float)(p & kFracMask) * kFracScale;
        const float s0 = tab[idx];
        out[i] += (s0 + fr * (tab[idx + 1] - s0)) * a;
        a += dAmp;
        p += inc;
    }
    phase = p;
    amp = a;
}

// A slow random walk: picks a new target in [-1, 1] every `periodTicks` and glides toward it.
struct Walk
{
    float value = 0.0f, target = 0.0f, coef = 0.05f;
    int left = 0;
    void step(Xorshift32& rng, float minTicks, float maxTicks)
    {
        if (--left <= 0)
        {
            target = rng.nextBipolar();
            const float ticks = minTicks + (maxTicks - minTicks) * rng.nextFloat();
            left = (int)ticks + 1;
            coef = clamp(1.6f / ticks, 0.0005f, 1.0f);
        }
        value += (target - value) * coef;
    }
};

// ---------------------------------------------------------------------------------------------
// The plugin
// ---------------------------------------------------------------------------------------------
class Abyss : public mpc::Plugin
{
public:
    enum Params
    {
        kVolume = 0,
        kEvolve, kBright, kBloom, kDrift, kDetune,
        kSub, kFifth, kShimmer, kAir,
        kMotion, kSync, kCycle, kCycleDiv,
        kAttack, kRelease, kWidth,
        kSpace, kSize, kSpaceTone,
        kVelocity, kOctave,
        // 1.1
        kOrbit, kOrbitSpeed, kStretch, kHollow, kInterval, kMotionShape, kEvolveDir,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Abyss";
        i.vendor = "Groundswell";
        i.uniqueId = MPC_FOURCC('G', 'O', 'l', 'A');   // == "uid" in plugin.json. Never change.
        i.version = 1100;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Abyss() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        std::vector<std::string> divs, orbitSpeeds, intervals;
        for (const char* d : kCycleDivNames) divs.push_back(d);
        for (const char* d : kOrbitSpeedNames) orbitSpeeds.push_back(d);
        for (const char* d : kIntervalNames) intervals.push_back(d);

        addParam(floatParam ("vol",      "Volume", -40.0f, 6.0f, -3.0f, "dB", 1));
        // How long a held note takes to open fully (from a sine to the full pad).
        addParam(logParam   ("evolve",   "Evolve", 1.0f, 60.0f, 15.0f, "", 1));      // text (with unit) from formatValue
        // How many harmonics are open once the note has evolved.
        addParam(floatParam ("bright",   "Bright", 0.0f, 100.0f, 50.0f, "%", 0));
        // Depth of the moving formant-like peak in the spectrum.
        addParam(floatParam ("bloom",    "Bloom", 0.0f, 100.0f, 50.0f, "%", 0));
        // Random wandering of each partial's level and pitch, and of each voice's brightness.
        addParam(floatParam ("drift",    "Drift", 0.0f, 100.0f, 40.0f, "%", 0));
        // Detune of the three twin partials (chorus beating in the low harmonics).
        addParam(floatParam ("detune",   "Detune", 0.0f, 30.0f, 7.0f, "ct", 0));
        // Layer levels.
        addParam(floatParam ("sub",      "Sub", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam ("fifth",    "Int Level", 0.0f, 100.0f, 50.0f, "%", 0));    // the interval layer ("Fifth" in 1.0)
        addParam(floatParam ("shimmer",  "Shimmer", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam ("air",      "Air", 0.0f, 100.0f, 15.0f, "%", 0));
        // The slow motion cycle.
        addParam(floatParam ("motion",   "Motion", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(choiceParam("sync",     "Sync", {"Free", "Sync"}, 0));
        addParam(logParam   ("cycle",    "Cycle", 2.0f, 120.0f, 20.0f, "", 1));      // text from formatValue
        addParam(choiceParam("cyclediv", "Cycle Div", divs, 2));
        // Envelope.
        addParam(logParam   ("attack",   "Attack", 0.01f, 20.0f, 2.5f, "", 2));      // text from formatValue
        addParam(logParam   ("release",  "Release", 0.05f, 30.0f, 6.0f, "", 2));     // text from formatValue
        addParam(floatParam ("width",    "Width", 0.0f, 100.0f, 70.0f, "%", 0));
        // Reverb.
        addParam(floatParam ("space",    "Space", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(logParam   ("size",     "Size", 1.0f, 20.0f, 8.0f, "", 1));         // text from formatValue
        addParam(logParam   ("spacetone","Space Tone", 1000.0f, 16000.0f, 5000.0f, "", 0));   // text from formatValue
        // Playing.
        addParam(floatParam ("velocity", "Velocity", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(intParam   ("octave",   "Octave", -2, 2, 0));
        // ---- 1.1: appended, so 1.0 projects load unchanged (these defaults sound exactly like 1.0). ----
        // Orbit: each voice's two stereo halves circle each other, swapping front and back.
        addParam(floatParam ("orbit",    "Orbit", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("orbitspeed","Orbit Speed", orbitSpeeds, 2));        // orbits per motion cycle
        // Stretch: + pulls the partials sharp (bells, glass), - squeezes them together (blur).
        addParam(floatParam ("stretch",  "Stretch", -100.0f, 100.0f, 0.0f, "%", 0));
        // Hollow: removes the even harmonics (a woodwind / clarinet colour).
        addParam(floatParam ("hollow",   "Hollow", 0.0f, 100.0f, 0.0f, "%", 0));
        // Which interval the interval layer plays above the note.
        addParam(choiceParam("interval", "Interval", intervals, 0));
        addParam(choiceParam("motionshape","Motion Shape", {"Sine", "Drift", "Swell", "Tide"}, kShapeSine));
        // Rise: notes open from dark to bright. Sink: they start bright and sink into the dark.
        addParam(choiceParam("evolvedir","Evolve Dir", {"Rise", "Sink"}, 0));

        if (numParams() != kNumParams) mpc::log("Abyss: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Sub and Int Level of the 1.0 programs were re-voiced in 1.1: in 1.0 those two knobs did nothing (the sub and
        // the fifth were always fully on), and these values come closest to how the programs sounded then.
        program("Abyssal Plain", {{kSub, 70}, {kFifth, 50}});
        program("Midnight Zone", {{kSub, 90}, {kFifth, 90}, {kEvolve, 30}, {kBright, 25}, {kBloom, 35}, {kDrift, 30}, {kDetune, 5},
                                  {kShimmer, 0}, {kAir, 5}, {kMotion, 30}, {kCycle, 60},
                                  {kAttack, 6}, {kRelease, 12}, {kSpace, 55}, {kSize, 14}, {kSpaceTone, 2500}});
        program("Bioluminescence", {{kSub, 70}, {kFifth, 60}, {kEvolve, 8}, {kBright, 70}, {kBloom, 40}, {kDrift, 50}, {kDetune, 10},
                                    {kShimmer, 80}, {kAir, 35}, {kMotion, 60}, {kCycle, 10},
                                    {kAttack, 1.5f}, {kRelease, 8}, {kWidth, 90}, {kSpace, 60}, {kSize, 12},
                                    {kSpaceTone, 9000}});
        program("Whale Fall", {{kSub, 70}, {kFifth, 100}, {kEvolve, 25}, {kBright, 40}, {kBloom, 60}, {kDrift, 35}, {kShimmer, 15}, {kAir, 10}, {kSync, 1}, {kCycleDiv, 3}, {kAttack, 4}, {kRelease, 10},
                               {kSpace, 50}, {kSize, 16}, {kSpaceTone, 3500}, {kOctave, -1}});
        program("Thermal Vent", {{kSub, 70}, {kFifth, 30}, {kEvolve, 12}, {kBright, 55}, {kBloom, 70}, {kDrift, 85}, {kDetune, 18},
                                 {kShimmer, 10}, {kAir, 25}, {kMotion, 40}, {kCycle, 14},
                                 {kSpace, 35}, {kSize, 6}, {kSpaceTone, 4000}});
        program("Hadal Choir", {{kSub, 60}, {kFifth, 50}, {kEvolve, 10}, {kBright, 45}, {kBloom, 90}, {kDrift, 45}, {kDetune, 9}, {kShimmer, 20}, {kAir, 20}, {kMotion, 70}, {kCycle, 16},
                                {kAttack, 3}, {kRelease, 7}, {kWidth, 85}, {kSpace, 50}, {kSize, 10}});
        program("Pressure Wave", {{kSub, 70}, {kFifth, 20}, {kEvolve, 4}, {kBright, 60}, {kBloom, 80}, {kDrift, 25}, {kShimmer, 20}, {kMotion, 100}, {kSync, 1}, {kCycleDiv, 0}, {kAttack, 0.8f},
                                  {kRelease, 4}, {kSpace, 35}, {kSize, 5}});
        program("Cold Seep", {{kSub, 80}, {kFifth, 90}, {kEvolve, 6}, {kBright, 85}, {kBloom, 30}, {kDrift, 30}, {kDetune, 4}, {kShimmer, 60}, {kAir, 30}, {kMotion, 35}, {kCycle, 30}, {kAttack, 1}, {kRelease, 9},
                              {kWidth, 100}, {kSpace, 65}, {kSize, 18}, {kSpaceTone, 11000}, {kOctave, 1}});
        // 1.1
        program("Glass Sponge", {{kSub, 0}, {kFifth, 30}, {kEvolve, 12}, {kBright, 75}, {kBloom, 35}, {kDrift, 30}, {kDetune, 4}, {kInterval, 2}, {kShimmer, 55}, {kAir, 20}, {kMotion, 40}, {kCycle, 24},
                                 {kStretch, 55}, {kOrbit, 35}, {kWidth, 90}, {kAttack, 1.2f}, {kRelease, 9},
                                 {kSpace, 60}, {kSize, 14}, {kSpaceTone, 10000}});
        program("Siphonophore", {{kSub, 35}, {kFifth, 45}, {kOrbit, 100}, {kOrbitSpeed, 2}, {kBloom, 70}, {kBright, 55}, {kInterval, 3},
                                 {kDrift, 35}, {kMotion, 55}, {kCycle, 16}, {kShimmer, 20},
                                 {kWidth, 80}, {kSpace, 45}, {kSize, 10}});
        program("Trench Wind", {{kSub, 25}, {kFifth, 0}, {kHollow, 85}, {kAir, 55}, {kDrift, 60}, {kMotionShape, kShapeDrift}, {kBright, 45},
                                {kBloom, 50}, {kShimmer, 0}, {kDetune, 12}, {kEvolve, 20}, {kAttack, 4},
                                {kRelease, 10}, {kOrbit, 30}, {kOrbitSpeed, 1}, {kSpace, 55}, {kSize, 16},
                                {kSpaceTone, 3000}});
        program("Sinking", {{kSub, 50}, {kFifth, 0}, {kEvolveDir, 1}, {kEvolve, 40}, {kBright, 85}, {kBloom, 45}, {kShimmer, 45}, {kDrift, 40}, {kAir, 15}, {kAttack, 1.5f}, {kRelease, 12}, {kSpace, 55}, {kSize, 18},
                            {kSpaceTone, 4000}});
        program("Leviathan", {{kSub, 85}, {kFifth, 40}, {kOctave, -1}, {kStretch, -35}, {kBright, 35}, {kBloom, 60},
                              {kDrift, 50}, {kDetune, 14}, {kMotion, 45}, {kMotionShape, kShapeTide}, {kCycle, 40},
                              {kOrbit, 60}, {kOrbitSpeed, 0}, {kEvolve, 30}, {kAttack, 5}, {kRelease, 14},
                              {kShimmer, 0}, {kAir, 10}, {kSpace, 50}, {kSize, 18}, {kSpaceTone, 2500}});
        program("Lanternfish", {{kSub, 35}, {kFifth, 40}, {kSync, 1}, {kCycleDiv, 1}, {kMotionShape, kShapeSwell}, {kMotion, 85}, {kInterval, 5},
                                {kOrbit, 70}, {kOrbitSpeed, 4}, {kShimmer, 45}, {kBright, 60},
                                {kBloom, 70}, {kEvolve, 5}, {kAttack, 0.6f}, {kRelease, 5}, {kAir, 10},
                                {kSpace, 40}, {kSize, 7}});
        program("Kelp Cathedral", {{kSub, 30}, {kFifth, 50}, {kInterval, 1}, {kHollow, 40}, {kBloom, 85}, {kMotionShape, kShapeTide},
                                   {kCycle, 30}, {kBright, 50}, {kDrift, 35}, {kShimmer, 25}, {kOrbit, 40},
                                   {kOrbitSpeed, 1}, {kWidth, 85}, {kAttack, 3}, {kRelease, 14}, {kSpace, 75},
                                   {kSize, 20}, {kSpaceTone, 6000}});
        program("Black Smoker", {{kSub, 55}, {kFifth, 0}, {kStretch, -60}, {kDrift, 90}, {kDetune, 25}, {kMotionShape, kShapeDrift}, {kBright, 50},
                                 {kBloom, 60}, {kAir, 35}, {kHollow, 20}, {kOrbit, 25}, {kEvolve, 14},
                                 {kSpace, 35}, {kSize, 6}, {kSpaceTone, 3500}});

        // Sine table (one extra point so the interpolation never wraps).
        for (int i = 0; i <= kTableSize; ++i) sine_[i] = std::sin(kTwoPi * (float)i / (float)kTableSize);

        updateSlots(0.0f, 0.0f, 0);
    }

    // ----- lifecycle ------------------------------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        const float scale = sr_ / 44100.0f;
        for (int i = 0; i < 8; ++i)
        {
            fdnLen_[i] = (int)std::lround((float)kFdnLen[i] * scale);
            int size = 1;
            while (size < fdnLen_[i] + 64) size <<= 1;
            fdnBuf_[i].assign((size_t)size, 0.0f);
            fdnMask_[i] = size - 1;
        }
        airL_.setSampleRate(sampleRate);
        airR_.setSampleRate(sampleRate);
        volume_.setTime(20.0f, sampleRate);
        for (int v = 0; v < kNumVoices; ++v)
        {
            voices_[v].rng.reseed(0xAB155u + 104729u * (uint32_t)(v + 1));
            voices_[v].rng2.reseed(0x0B17u + 7919u * (uint32_t)(v + 1));
        }
        lastSize_ = -1.0f;
        reset();
    }

    void reset() override
    {
        for (Voice& v : voices_) v.stage = kOff;
        for (int i = 0; i < 8; ++i)
        {
            std::fill(fdnBuf_[i].begin(), fdnBuf_[i].end(), 0.0f);
            fdnLp_[i] = 0.0f;
        }
        fdnPos_ = 0;
        hpL_ = hpR_ = 0.0f;
        airL_.reset(); airR_.reset();
        airEnv_ = 0.0f;
        modPhase1_ = 0.0f; modPhase2_ = 0.37f;
        motionPhase_ = 0.0f;
        orbitPhase_ = 0.0f;
        ctrlLeft_ = 0;
        orderCounter_ = 0;
        sustain_ = false;
        bend_ = 0.0f; modWheel_ = 0.0f;
        volume_.snap(dbToGain(get(kVolume)));
        dryRamp_.snap(dryGain(get(kSpace) * 0.01f));
        wetRamp_.snap(wetGain(get(kSpace) * 0.01f));
        airRamp_.snap(0.0f);
    }

    // ----- audio ----------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx);
        const int n = ctx.numFrames;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        // Voices: tick every kCtrl samples; segments also break at MIDI events.
        int pos = 0, e = 0;
        while (pos < n)
        {
            while (e < ctx.numMidiEvents && ctx.midi[e].sampleOffset <= pos) handleMidi(ctx.midi[e++]);
            if (ctrlLeft_ <= 0) { controlTick(); ctrlLeft_ = kCtrl; }
            int len = std::min(ctrlLeft_, n - pos);
            if (e < ctx.numMidiEvents && ctx.midi[e].sampleOffset > pos)
                len = std::min(len, ctx.midi[e].sampleOffset - pos);
            renderVoices(L + pos, R ? R + pos : nullptr, len);
            pos += len;
            ctrlLeft_ -= len;
        }
        while (e < ctx.numMidiEvents) handleMidi(ctx.midi[e++]);

        // Air, reverb and output stage.
        dryRamp_.setTarget(dryGain(s_.space), n);
        wetRamp_.setTarget(wetGain(s_.space), n);
        airRamp_.setTarget(s_.air * 0.05f * std::min(airEnv_, 1.5f), n);
        volume_.setTarget(s_.volume);
        const float d = s_.fdnDamp;
        const float hpC = s_.hpCoef;
        const float inG = s_.fdnIn;
        // Delay modulation for two lines (avoids a metallic ring); advanced per block.
        const float modDepth = 6.0f * sr_ / 44100.0f;
        const float mod1 = 8.0f + modDepth * (1.0f + fastSin01(modPhase1_));
        const float mod2 = 8.0f + modDepth * (1.0f + fastSin01(modPhase2_));
        modPhase1_ = wrapPhase(modPhase1_ + 0.13f * (float)n / sr_);
        modPhase2_ = wrapPhase(modPhase2_ + 0.21f * (float)n / sr_);

        for (int i = 0; i < n; ++i)
        {
            const float ag = airRamp_.next();
            float dl = L[i], dr = R ? R[i] : L[i];
            if (ag > 0.0f)
            {
                dl += ag * airL_.process(airNoiseL_.process(), SVF::Mode::Bandpass);
                dr += ag * airR_.process(airNoiseR_.process(), SVF::Mode::Bandpass);
            }

            // Reverb input: high-passed so the sub does not muddy the tail.
            hpL_ += hpC * (dl - hpL_);
            hpR_ += hpC * (dr - hpR_);
            const float inL = (dl - hpL_) * inG, inR = (dr - hpR_) * inG;

            // 8-line FDN: read, damp, Hadamard mix, inject, write.
            float y[8];
            for (int k = 0; k < 8; ++k)
            {
                float dly = (float)fdnLen_[k];
                if (k == 0) dly -= mod1;
                else if (k == 5) dly -= mod2;
                const float* buf = fdnBuf_[k].data();
                const int mask = fdnMask_[k];
                if (k == 0 || k == 5)
                {
                    const int di = (int)dly;
                    const float fr = dly - (float)di;
                    const float a = buf[(fdnPos_ - di) & mask], b = buf[(fdnPos_ - di - 1) & mask];
                    y[k] = a + fr * (b - a);
                }
                else y[k] = buf[(fdnPos_ - (int)dly) & mask];
            }
            const float wl = 0.5f * (y[0] - y[2] + y[4] - y[6]);
            const float wr = 0.5f * (y[1] - y[3] + y[5] - y[7]);
            for (int k = 0; k < 8; ++k)
            {
                fdnLp_[k] += d * (y[k] - fdnLp_[k]);
                y[k] = fdnLp_[k] * fdnG_[k];
            }
            // Fast Walsh-Hadamard transform, normalised by 1/sqrt(8).
            for (int h = 1; h < 8; h <<= 1)
                for (int a = 0; a < 8; a += h << 1)
                    for (int b = a; b < a + h; ++b)
                    {
                        const float x0 = y[b], x1 = y[b + h];
                        y[b] = x0 + x1; y[b + h] = x0 - x1;
                    }
            constexpr float kNorm = 0.35355339f;
            for (int k = 0; k < 8; ++k)
            {
                float v = y[k] * kNorm + ((k & 1) ? ((k & 2) ? -inR : inR) : ((k & 2) ? -inL : inL));
                fdnBuf_[k][(size_t)(fdnPos_ & fdnMask_[k])] = v;
            }
            ++fdnPos_;

            const float dg = dryRamp_.next(), wg = wetRamp_.next();
            const float vol = volume_.process();
            const float outL = 0.97f * softClip((dl * dg + wl * wg) * vol);
            const float outR = 0.97f * softClip((dr * dg + wr * wg) * vol);
            L[i] = outL;
            if (R) R[i] = outR;
        }
        fdnPos_ &= 0x3FFFFFFF;
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        char buf[32];
        if (index == kSpaceTone)
        {
            if (plainValue >= 1000.0f) std::snprintf(buf, sizeof(buf), "%.1f kHz", (double)plainValue * 0.001);
            else std::snprintf(buf, sizeof(buf), "%.0f Hz", (double)plainValue);
            out = buf;
            return true;
        }
        if (index == kAttack || index == kRelease)
        {
            if (plainValue < 1.0f) std::snprintf(buf, sizeof(buf), "%.0f ms", (double)plainValue * 1000.0);
            else std::snprintf(buf, sizeof(buf), "%.1f s", (double)plainValue);
            out = buf;
            return true;
        }
        if (index == kEvolve || index == kCycle || index == kSize)
        {
            if (plainValue < 10.0f) std::snprintf(buf, sizeof(buf), "%.1f s", (double)plainValue);
            else std::snprintf(buf, sizeof(buf), "%.0f s", (double)plainValue);
            out = buf;
            return true;
        }
        if (index == kOctave)
        {
            const int o = (int)std::lround(plainValue);
            std::snprintf(buf, sizeof(buf), o > 0 ? "+%d" : "%d", o);
            out = buf;
            return true;
        }
        return false;
    }

private:
    enum Stage { kOff = 0, kAttackStage, kHold, kReleaseStage, kStealing, kDying };

    struct Voice
    {
        int stage = kOff;
        int note = 60;
        float vel01 = 1.0f;
        bool held = false, sustained = false;
        float g = 0.0f;          // envelope gain
        float p = 0.0f;          // attack progress 0..1 (g = p^2 while attacking)
        float unfold1 = 0.0f;    // first lag of the unfold curve
        float unfold = 0.0f;     // 0 -> 1 over Evolve, an S-curve (slow start)
        float motionOff = 0.0f;  // phase offset into the motion cycle
        float orbitOff = 0.0f;   // phase offset into the orbit (each note of a chord circles on its own)
        uint32_t order = 0;      // for stealing: lower = older
        int stealTicks = 0;
        int pendingNote = -1;
        float pendingVel = 0.0f;
        bool pendingHeld = false;  // false once the stolen-for note has been released
        Walk bright, pitch;      // per-voice drift
        Walk mw, mw2;            // the "Drift" motion shape
        Xorshift32 rng;
        Xorshift32 rng2;         // 1.1 features draw from their own generator, so 1.0 sounds stay as they were

        uint32_t phase[kNumSlots] = {};
        uint32_t inc[kNumSlots] = {};
        float amp[kNumSlots] = {};
        float tgt[kNumSlots] = {};
        float dAmp[kNumSlots] = {};
        Walk lvl[kNumSlots], pit[kNumSlots];
        int active[kNumSlots] = {};
        int numActive = 0;
        bool panInit = false;    // false until the first tick sets the pan targets directly

        // Bus pan gains (A and B to L/R), ramped per tick.
        float gAL = 0.7f, gAR = 0.7f, gBL = 0.7f, gBR = 0.7f;
        float dAL = 0.0f, dAR = 0.0f, dBL = 0.0f, dBR = 0.0f;
        float tAL = 0.7f, tAR = 0.7f, tBL = 0.7f, tBR = 0.7f;
        // Orbit: a one-pole low-pass per bus that darkens the half that is behind.
        float lpA = 0.0f, lpB = 0.0f;       // filter states
        float kA = 1.0f, kB = 1.0f;         // coefficients now (smoothed per tick)
        float dkA = 0.0f, dkB = 0.0f;       // per-sample ramps
        float kAt = 1.0f, kBt = 1.0f;       // smoothed per-tick targets
    };

    // Settings read once per block.
    struct Settings
    {
        float volume = 1.0f;
        float attInc = 0.01f, relCoef = 0.99f, unfoldK = 0.001f;
        float brightH = 6.0f, bloom = 0.5f, drift = 0.4f, motion = 0.5f;
        float twinUp = 1.0f, twinDn = 1.0f, twinGain = 0.0f;
        float sub = 0.0f, fifth = 0.0f, shimmer = 0.0f, air = 0.0f;
        float width = 0.7f, velSens = 0.3f;
        int octave = 0;
        float maxHz = 18000.0f, incScale = 1.0f;
        float cycleTicks = 1000.0f;   // control ticks per motion cycle
        float orbit = 0.0f, orbitRatio = 2.0f;
        int shape = kShapeSine;
        bool sink = false;
        float space = 0.4f, fdnDamp = 0.5f, fdnIn = 0.3f, hpCoef = 0.02f;
    };

    // Per-slot frequency ratio, the harmonic number the brightness and bloom curves see, and the spectral
    // tilt (about -3.6 dB per octave, with Hollow taking out even harmonics). Recomputed only when
    // Stretch, Hollow or Interval change.
    void updateSlots(float stretch, float hollow, int interval)
    {
        interval = clamp(interval, 0, kNumIntervals - 1);
        if (slotsValid_ && stretch == lastStretch_ && hollow == lastHollow_ && interval == lastInterval_) return;
        slotsValid_ = true; lastStretch_ = stretch; lastHollow_ = hollow; lastInterval_ = interval;
        const float e = 1.0f + 0.35f * clamp(stretch, -1.0f, 1.0f);
        for (int k = 0; k < kNumSlots; ++k)
        {
            const Slot& sl = kSlots[k];
            float r = sl.ratio;
            if (sl.layer == kLayerFifth) r = kIntervalRatios[interval] * (sl.ratio / 1.5f);   // root, x2, x3
            const float shape = (sl.layer == kLayerFifth) ? r : sl.shape;
            if (sl.layer != kLayerSub) r = std::pow(r, e);
            float t = std::pow(shape, -0.6f);
            if (sl.layer == kLayerTwin) t *= 0.75f;
            if (sl.layer == kLayerFifth) t *= 0.8f;
            if ((sl.layer == kLayerMain || sl.layer == kLayerTwin) && ((int)sl.shape % 2) == 0) t *= 1.0f - 0.92f * hollow;
            slotRatio_[k] = r; slotShape_[k] = shape; slotTilt_[k] = t;
        }
    }

    // The motion signal for a voice at cycle phase p: -1..1.
    static float motionShape(int shape, float p, float walk)
    {
        p = wrapPhase(p);
        switch (shape)
        {
            case kShapeDrift: return clamp(1.3f * walk, -1.0f, 1.0f);
            case kShapeSwell:  // a long rise, then a quicker fall
                return p < 0.8f ? -1.0f + 2.0f * smoothstep(0.0f, 0.8f, p) : 1.0f - 2.0f * smoothstep(0.8f, 1.0f, p);
            case kShapeTide: return 0.6f * fastSin01(p) + 0.4f * fastSin01(3.0f * p);
            default: return fastSin01(p);
        }
    }

    static float dryGain(float space) { return 1.0f - 0.45f * space; }
    static float wetGain(float space) { return 1.25f * space; }

    void readSettings(const mpc::ProcessContext& ctx)
    {
        const float ctrlDt = (float)kCtrl / sr_;
        s_.volume = dbToGain(get(kVolume));
        s_.attInc = ctrlDt / get(kAttack);
        s_.relCoef = std::exp(-6.9078f * ctrlDt / get(kRelease));   // -60 dB over Release
        s_.unfoldK = 1.0f - std::exp(-4.74f * ctrlDt / get(kEvolve)); // two cascaded lags: ~95 % open after Evolve
        const float b = get(kBright) * 0.01f;
        s_.brightH = 1.5f + 22.0f * b * std::sqrt(b);
        s_.bloom = clamp(get(kBloom) * 0.01f + 0.5f * modWheel_, 0.0f, 1.5f);
        s_.drift = get(kDrift) * 0.01f;
        s_.motion = get(kMotion) * 0.01f;
        const float det = get(kDetune);
        s_.twinUp = centsToRatio(det);
        s_.twinDn = 1.0f / s_.twinUp;
        s_.twinGain = clamp(det / 3.0f, 0.0f, 1.0f);
        s_.sub = get(kSub) * 0.01f;
        s_.fifth = get(kFifth) * 0.01f;
        s_.shimmer = get(kShimmer) * 0.01f;
        s_.air = get(kAir) * 0.01f;
        s_.width = get(kWidth) * 0.01f;
        s_.velSens = get(kVelocity) * 0.01f;
        s_.octave = getInt(kOctave);
        s_.maxHz = std::min(0.45f * sr_, 18000.0f);
        s_.incScale = 4294967296.0f / sr_;
        s_.orbit = get(kOrbit) * 0.01f;
        s_.orbitRatio = kOrbitRatios[clamp(getInt(kOrbitSpeed), 0, kNumOrbitSpeeds - 1)];
        s_.shape = getInt(kMotionShape);
        s_.sink = getInt(kEvolveDir) == 1;
        updateSlots(get(kStretch) * 0.01f, get(kHollow) * 0.01f, getInt(kInterval));

        // Motion cycle length, and the phase when locked to the song.
        const mpc::Transport& t = ctx.transport;
        const bool sync = getInt(kSync) == 1;
        float cycleSec = get(kCycle);
        if (sync)
        {
            const float beatsPerBar = (float)std::max(1, t.timeSigNumerator) * 4.0f / (float)std::max(1, t.timeSigDenominator);
            const float beats = kCycleDivBars[clamp(getInt(kCycleDiv), 0, 5)] * beatsPerBar;
            cycleSec = beatsToSeconds(beats, t.tempoValid ? t.bpm : 120.0);
            if (t.playing && t.ppqValid)
            {
                motionPhase_ = ppqPhase(t.ppqPos, beats);
                orbitPhase_ = ppqPhase(t.ppqPos, beats / s_.orbitRatio);
            }
        }
        s_.cycleTicks = std::max(1.0f, cycleSec / ctrlDt);

        // Reverb.
        s_.space = get(kSpace) * 0.01f;
        const float size = get(kSize);
        if (size != lastSize_)
        {
            lastSize_ = size;
            float sumG2 = 0.0f;
            for (int k = 0; k < 8; ++k)
            {
                fdnG_[k] = std::pow(10.0f, -3.0f * (float)fdnLen_[k] / (sr_ * size));
                sumG2 += fdnG_[k] * fdnG_[k];
            }
            // Scale the input so a held sound reaches about the same level whatever the size.
            fdnInGain_ = 0.75f * std::sqrt(std::max(0.02f, 1.0f - sumG2 / 8.0f));
        }
        s_.fdnIn = fdnInGain_;
        s_.fdnDamp = 1.0f - std::exp(-kTwoPi * clamp(get(kSpaceTone), 200.0f, 0.45f * sr_) / sr_);
        s_.hpCoef = 1.0f - std::exp(-kTwoPi * 150.0f / sr_);

        // Air filters follow the motion cycle a little.
        const float airHz = 5500.0f * (1.0f + 0.3f * s_.motion * fastSin01(motionPhase_ + 0.5f));
        airL_.set(clamp(airHz, 1000.0f, 0.45f * sr_), 0.35f);
        airR_.set(clamp(airHz * 1.18f, 1000.0f, 0.45f * sr_), 0.35f);
    }

    // ----- MIDI -----------------------------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn()) noteOn(ev.note(), ev.velocity());
        else if (ev.isNoteOff()) noteOff(ev.note());
        else if (ev.isPitchBend()) bend_ = 2.0f * (float)ev.pitchBend() / 8192.0f;
        else if (ev.isAllNotesOff())
        {
            sustain_ = false;
            for (Voice& v : voices_) if (v.stage != kOff && v.stage != kDying) { v.held = false; v.sustained = false; release(v); }
        }
        else if (ev.isController())
        {
            if (ev.controller() == 1) modWheel_ = (float)ev.controllerValue() / 127.0f;
            else if (ev.controller() == 64)
            {
                sustain_ = ev.controllerValue() >= 64;
                if (!sustain_)
                    for (Voice& v : voices_) if (v.sustained) { v.sustained = false; release(v); }
            }
        }
    }

    void noteOn(int note, int velocity)
    {
        const float vel01 = (float)velocity / 127.0f;
        // The same note still sounding: pick it up again from where it is.
        for (Voice& v : voices_)
            if (v.note == note && (v.stage == kAttackStage || v.stage == kHold || v.stage == kReleaseStage))
            {
                v.vel01 = vel01; v.held = true; v.sustained = false;
                v.p = std::sqrt(clamp(v.g, 0.0f, 1.0f));
                v.stage = kAttackStage;
                v.order = ++orderCounter_;
                return;
            }
        // A free voice.
        for (Voice& v : voices_)
            if (v.stage == kOff) { startVoice(v, note, vel01); return; }
        // Steal: the quietest releasing voice, else the oldest.
        Voice* best = nullptr;
        for (Voice& v : voices_)
            if (v.stage == kReleaseStage && (!best || v.g < best->g)) best = &v;
        if (!best)
            for (Voice& v : voices_)
                if (v.stage != kStealing && (!best || v.order < best->order)) best = &v;
        if (!best) best = &voices_[0];
        best->stage = kStealing;
        best->stealTicks = 4;
        best->pendingNote = note;
        best->pendingVel = vel01;
        best->pendingHeld = true;
        best->held = false; best->sustained = false;
        best->order = ++orderCounter_;
    }

    void noteOff(int note)
    {
        for (Voice& v : voices_)
        {
            if (v.stage == kStealing && v.pendingNote == note) { v.pendingHeld = false; continue; }
            if (v.note == note && v.held && (v.stage == kAttackStage || v.stage == kHold))
            {
                v.held = false;
                if (sustain_) v.sustained = true;
                else release(v);
            }
        }
    }

    static void release(Voice& v)
    {
        if (v.stage == kAttackStage || v.stage == kHold) v.stage = kReleaseStage;
    }

    void startVoice(Voice& v, int note, float vel01)
    {
        v.stage = kAttackStage;
        v.note = note;
        v.vel01 = vel01;
        v.held = true; v.sustained = false;
        v.g = 0.0f; v.p = 0.0f; v.unfold1 = 0.0f; v.unfold = 0.0f;
        v.motionOff = 0.35f * v.rng.nextFloat();
        v.orbitOff = v.rng2.nextFloat();
        v.mw = Walk(); v.mw2 = Walk();
        v.mw.value = 0.5f * v.rng2.nextBipolar(); v.mw2.value = 0.5f * v.rng2.nextBipolar();
        v.lpA = v.lpB = 0.0f;
        v.kA = v.kB = v.kAt = v.kBt = 1.0f; v.dkA = v.dkB = 0.0f;
        v.panInit = false;
        v.order = ++orderCounter_;
        v.pendingNote = -1;
        v.bright = Walk(); v.pitch = Walk();
        v.bright.value = 0.5f * v.rng.nextBipolar();
        v.pitch.value = 0.5f * v.rng.nextBipolar();
        for (int k = 0; k < kNumSlots; ++k)
        {
            v.phase[k] = v.rng.next();          // random phases: a lower crest factor, and every note differs
            v.amp[k] = v.tgt[k] = v.dAmp[k] = 0.0f;
            v.lvl[k] = Walk(); v.pit[k] = Walk();
            v.lvl[k].value = (kSlots[k].layer == kLayerShim) ? -1.0f : 0.6f * v.rng.nextBipolar();
            v.pit[k].value = 0.6f * v.rng.nextBipolar();
            v.lvl[k].left = 1 + (int)(v.rng.nextFloat() * 200.0f);   // stagger the first new targets
            v.pit[k].left = v.lvl[k].left;
        }
        v.numActive = 0;
    }

    // ----- control rate ---------------------------------------------------------------------------
    void controlTick()
    {
        motionPhase_ = wrapPhase(motionPhase_ + 1.0f / s_.cycleTicks);
        orbitPhase_ = wrapPhase(orbitPhase_ + s_.orbitRatio / s_.cycleTicks);
        const float ticksPerSec = sr_ / (float)kCtrl;
        float envSum = 0.0f;
        for (Voice& v : voices_)
        {
            if (v.stage == kOff) continue;
            tickVoice(v, ticksPerSec);
            if (v.stage != kOff) envSum += v.g * (0.3f + 0.7f * v.unfold);
        }
        airEnv_ += 0.02f * (envSum - airEnv_);
    }

    void tickVoice(Voice& v, float tps)
    {
        // Snap last tick's ramps to their exact targets.
        for (int k = 0; k < kNumSlots; ++k) v.amp[k] = v.tgt[k];
        v.gAL = v.tAL; v.gAR = v.tAR; v.gBL = v.tBL; v.gBR = v.tBR;
        v.kA = v.kAt; v.kB = v.kBt;

        bool zero = false;
        switch (v.stage)
        {
            case kDying:
                v.stage = kOff; v.numActive = 0;
                return;
            case kStealing:
                if (v.stealTicks <= 0)
                {
                    if (v.pendingNote < 0) { v.stage = kOff; v.numActive = 0; return; }
                    const bool stillHeld = v.pendingHeld;
                    startVoice(v, v.pendingNote, v.pendingVel);
                    if (!stillHeld)
                    {
                        v.held = false;
                        if (sustain_) v.sustained = true;
                        else v.stage = kReleaseStage;
                    }
                }
                else
                {
                    --v.stealTicks;
                    if (v.stealTicks == 0) zero = true;
                    else v.g *= 0.3f;
                }
                break;
            case kAttackStage:
                v.p += s_.attInc;
                if (v.p >= 1.0f) { v.p = 1.0f; v.stage = kHold; }
                v.g = v.p * v.p;
                break;
            case kHold:
                v.g = 1.0f;
                break;
            case kReleaseStage:
                v.g *= s_.relCoef;
                if (v.g < 2.0e-4f) { v.stage = kDying; zero = true; }
                break;
            default: break;
        }

        v.unfold1 += (1.0f - v.unfold1) * s_.unfoldK;
        v.unfold += (v.unfold1 - v.unfold) * s_.unfoldK;
        const float u = v.unfold;

        // Per-voice drift walks: 4..10 s.
        v.bright.step(v.rng, 4.0f * tps, 10.0f * tps);
        v.pitch.step(v.rng, 4.0f * tps, 10.0f * tps);

        // Motion and drift.
        const float shapeTicks = s_.cycleTicks;
        v.mw.step(v.rng2, 0.15f * shapeTicks, 0.5f * shapeTicks);
        v.mw2.step(v.rng2, 0.15f * shapeTicks, 0.5f * shapeTicks);
        const float m = motionShape(s_.shape, motionPhase_ + v.motionOff, v.mw.value);
        const float m2 = motionShape(s_.shape, motionPhase_ + v.motionOff + 0.25f, v.mw2.value);
        const float drift = s_.drift;
        const float velGain = 1.0f - s_.velSens * (1.0f - v.vel01);

        // Brightness: number of open harmonics.
        // Rise opens from dark to bright; Sink starts bright and closes to near-dark.
        float h = 1.0f + (s_.brightH - 1.0f) * (s_.sink ? 1.0f - 0.85f * u : u);
        h *= (1.0f + 0.3f * s_.motion * m + 0.35f * drift * v.bright.value);
        h *= 1.0f - 0.4f * s_.velSens * (1.0f - v.vel01);
        h = std::max(h, 0.8f);
        // Bloom: a resonant peak that wanders through the harmonics.
        const float c = clamp(1.0f + h * (0.45f + 0.4f * s_.motion * m + 0.25f * drift * v.bright.value), 1.0f, 12.0f);

        // Orbit: the A and B halves circle each other. y > 0 is in front. The half in front is brighter, with
        // its bloom higher; the one behind is quieter and darker. A few cents of Doppler as they cross.
        const float o = s_.orbit;
        float ox = 0.0f, oy = 0.0f;
        if (o > 0.0f)
        {
            ox = fastSin01(orbitPhase_ + v.orbitOff);
            oy = fastSin01(orbitPhase_ + v.orbitOff + 0.25f);
        }
        const float busY[3] = {0.0f, oy, -oy};                 // C, A, B
        float busInvH2[3], busC[3], busDop[3];
        for (int b = 0; b < 3; ++b)
        {
            const float hb = std::max(0.8f, h * (1.0f + 0.25f * o * busY[b]));
            busInvH2[b] = 1.0f / (hb * hb);
            busC[b] = clamp(c * (1.0f + 0.3f * o * busY[b]), 1.0f, 12.0f);
        }
        busDop[kBusC] = 0.0f;
        busDop[kBusA] = 4.0f * o * ox;
        busDop[kBusB] = -busDop[kBusA];
        const float bloomAmt = 3.0f * s_.bloom * (0.3f + 0.7f * u);
        constexpr float kInvW2 = 1.0f / (1.4f * 1.4f);
        const float driftA = drift * (0.3f + 0.7f * u);
        const float centsToLin = 0.000577623f;   // ln(2) / 1200
        const float driftCents = drift * 7.0f * (0.4f + 0.6f * u);
        const float fifthIn = s_.fifth * smoothstep(0.15f, 0.6f, u);
        const float shimIn = s_.shimmer * (s_.sink ? 1.0f - smoothstep(0.3f, 0.85f, u) : smoothstep(0.35f, 0.9f, u));

        const float noteHz = midiToHz((float)(v.note + 12 * s_.octave) + bend_ + v.pitch.value * drift * 0.05f);

        float tmp[kNumSlots];
        float sumSq = 0.0f;
        for (int k = 0; k < kNumSlots; ++k)
        {
            const Slot& sl = kSlots[k];
            const bool shim = sl.layer == kLayerShim;
            v.lvl[k].step(v.rng, shim ? 0.3f * tps : 2.0f * tps, shim ? 1.5f * tps : 7.0f * tps);
            v.pit[k].step(v.rng, 2.0f * tps, 7.0f * tps);

            float ratio = slotRatio_[k];
            if (sl.twin > 0) ratio *= s_.twinUp;
            else if (sl.twin < 0) ratio *= s_.twinDn;
            const float shape = slotShape_[k];
            const float f = noteHz * ratio
                          * (1.0f + (v.pit[k].value * driftCents * (1.0f + 0.05f * shape) + busDop[sl.bus]) * centsToLin);

            float w;
            switch (sl.layer)
            {
                case kLayerSub:
                    w = 0.22f * s_.sub * clamp((f - 15.0f) * 0.05f, 0.0f, 1.0f);
                    break;
                case kLayerShim:
                {
                    const float tw = std::max(0.0f, v.lvl[k].value);
                    w = 0.055f * shimIn * tw * tw;
                    break;
                }
                default:
                {
                    const float x = shape * shape * busInvH2[sl.bus];
                    const float lp = 1.0f / (1.0f + x * x);
                    const float dd = shape - busC[sl.bus];
                    const float pk = 1.0f / (1.0f + dd * dd * kInvW2);
                    w = slotTilt_[k] * lp * (1.0f + bloomAmt * pk) * (1.0f + 0.7f * driftA * v.lvl[k].value);
                    if (sl.layer == kLayerTwin) w *= s_.twinGain;
                    if (sl.layer == kLayerFifth) w *= fifthIn;
                    else sumSq += w * w;
                    break;
                }
            }
            // Fade out partials near the top of the range; skip those above it.
            if (f > 0.85f * s_.maxHz) w *= clamp((s_.maxHz - f) / (0.15f * s_.maxHz), 0.0f, 1.0f);
            tmp[k] = w;
            v.inc[k] = (uint32_t)(clamp(f, 0.0f, 0.49f * sr_) * s_.incScale);
        }

        // Body normalised to a steady loudness; sub and shimmer at their own level.
        const float vg = zero ? 0.0f : v.g * velGain;
        const float bodyNorm = 0.16f / std::sqrt(0.5f * sumSq + 1.0e-9f);
        const float inv = 1.0f / (float)kCtrl;
        v.numActive = 0;
        for (int k = 0; k < kNumSlots; ++k)
        {
            const int layer = kSlots[k].layer;
            float t = tmp[k] * vg;
            if (layer != kLayerSub && layer != kLayerShim) t *= bodyNorm;
            if (t < 1.0e-6f) t = 0.0f;
            v.tgt[k] = t;
            v.dAmp[k] = (t - v.amp[k]) * inv;
            if (t != 0.0f || v.amp[k] != 0.0f) v.active[v.numActive++] = k;
        }

        // Stereo: buses A and B panned apart by Width, swaying with the motion cycle; with Orbit they circle
        // the full width instead, the one behind quieter and darker. Targets are smoothed (~20 ms) so a jump
        // in the cycle (song position, a program change) never clicks.
        const float spread = s_.width * (0.8f + 0.2f * s_.motion * m2);
        const float panA = lerp(-spread, 0.95f * ox, o), panB = lerp(spread, -0.95f * ox, o);
        const float backA = o * 0.5f * (1.0f - oy), backB = o * 0.5f * (1.0f + oy);   // 0 = front, 1 = behind
        const float gA = 1.0f - 0.3f * backA, gB = 1.0f - 0.3f * backB;
        float aL, aR, bL, bR;
        equalPowerPan(panA, aL, aR);
        equalPowerPan(panB, bL, bR);
        aL *= gA; aR *= gA; bL *= gB; bR *= gB;
        const float kAt = o > 0.0f ? 1.0f - std::exp(-kTwoPi * 16000.0f * std::pow(0.15f, backA) / sr_) : 1.0f;
        const float kBt = o > 0.0f ? 1.0f - std::exp(-kTwoPi * 16000.0f * std::pow(0.15f, backB) / sr_) : 1.0f;
        constexpr float kSmooth = 0.07f;
        if (!v.panInit)
        {
            v.panInit = true;
            v.gAL = v.tAL = aL; v.gAR = v.tAR = aR; v.gBL = v.tBL = bL; v.gBR = v.tBR = bR;
            v.kA = v.kAt = kAt; v.kB = v.kBt = kBt;
        }
        else
        {
            v.tAL += (aL - v.tAL) * kSmooth; v.tAR += (aR - v.tAR) * kSmooth;
            v.tBL += (bL - v.tBL) * kSmooth; v.tBR += (bR - v.tBR) * kSmooth;
            v.kAt += (kAt - v.kAt) * kSmooth; v.kBt += (kBt - v.kBt) * kSmooth;
        }
        v.dAL = (v.tAL - v.gAL) * inv; v.dAR = (v.tAR - v.gAR) * inv;
        v.dBL = (v.tBL - v.gBL) * inv; v.dBR = (v.tBR - v.gBR) * inv;
        v.dkA = (v.kAt - v.kA) * inv; v.dkB = (v.kBt - v.kB) * inv;
    }

    // ----- per sample: the partials -----------------------------------------------------------------
    void renderVoices(float* L, float* R, int n)
    {
        for (Voice& v : voices_)
        {
            if (v.stage == kOff || v.numActive == 0) continue;
            float bus[3][kCtrl];
            std::memset(bus, 0, sizeof(bus));
            for (int a = 0; a < v.numActive; ++a)
            {
                const int k = v.active[a];
                renderPartial(sine_, v.phase[k], v.inc[k], v.amp[k], v.dAmp[k], bus[kSlots[k].bus], n);
            }
            float gAL = v.gAL, gAR = v.gAR, gBL = v.gBL, gBR = v.gBR;
            float kA = v.kA, kB = v.kB, lpA = v.lpA, lpB = v.lpB;
            for (int i = 0; i < n; ++i)
            {
                lpA += kA * (bus[kBusA][i] - lpA);      // with Orbit off, k = 1 and this passes the bus through
                lpB += kB * (bus[kBusB][i] - lpB);
                const float c = bus[kBusC][i] * kInvSqrt2;
                L[i] += c + lpA * gAL + lpB * gBL;
                if (R) R[i] += c + lpA * gAR + lpB * gBR;
                gAL += v.dAL; gAR += v.dAR; gBL += v.dBL; gBR += v.dBR;
                kA += v.dkA; kB += v.dkB;
            }
            v.gAL = gAL; v.gAR = gAR; v.gBL = gBL; v.gBR = gBR;
            v.kA = kA; v.kB = kB; v.lpA = flushDenormal(lpA); v.lpB = flushDenormal(lpB);
        }
    }

    // ----- state ----------------------------------------------------------------------------------
    float sr_ = 44100.0f;
    Settings s_;
    Voice voices_[kNumVoices];
    float sine_[kTableSize + 1];
    float slotRatio_[kNumSlots] = {}, slotShape_[kNumSlots] = {}, slotTilt_[kNumSlots] = {};
    bool slotsValid_ = false;
    float lastStretch_ = 0.0f, lastHollow_ = 0.0f;
    int lastInterval_ = 0;
    int ctrlLeft_ = 0;
    uint32_t orderCounter_ = 0;
    bool sustain_ = false;
    float bend_ = 0.0f, modWheel_ = 0.0f;
    float motionPhase_ = 0.0f;
    float orbitPhase_ = 0.0f;
    float airEnv_ = 0.0f;
    WhiteNoise airNoiseL_{0x1A2B3C4Du}, airNoiseR_{0x5E6F7081u};
    SVF airL_, airR_;
    float hpL_ = 0.0f, hpR_ = 0.0f;
    std::vector<float> fdnBuf_[8];
    int fdnLen_[8] = {};
    int fdnMask_[8] = {};
    float fdnG_[8] = {};
    float fdnLp_[8] = {};
    float fdnInGain_ = 0.3f;
    float lastSize_ = -1.0f;
    int fdnPos_ = 0;
    float modPhase1_ = 0.0f, modPhase2_ = 0.37f;
    Smoother volume_;
    LinearRamp dryRamp_, wetRamp_, airRamp_;
};

} // namespace abyss

using abyss::Abyss;
MPC_PLUGIN_ENTRY(Abyss)
