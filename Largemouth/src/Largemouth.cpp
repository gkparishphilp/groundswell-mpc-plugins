// Largemouth - an MPC insert effect by Groundswell.
//
// What it is: a bass player that listens to your drums. Put it on a drum track (or a submix with
// the music in it) and it plays a bassline on its own synth, locked to the kick, in the key and
// on the chord it hears. Your audio passes through untouched; every bass note is synthesised here.
//
// Signal flow
// -----------
//   input --+---------------------------------------------------------------- dry --+
//           |                                                                       |
//           +--> mono --> KickTracker (140 Hz band onsets) --> hits + velocity      |
//           |                    |                                                  |
//           |                    +--> groove memory: 2 bars of 16th steps, each     |
//           |                         with how often a kick lands there and how     |
//           |                         early or late (so swing and feel survive)     |
//           |                                                                       |
//           +--> mono --> Harmony (constant-Q chroma) --> key + current chord       |
//                                                                                   |
//   conductor: on every learned kick step plays the chord root (on time, from the   |
//   song position, not late from the detector); between kicks adds Busy notes       |
//   (pickups before a kick, offbeats) chosen from root/octave/fifth/passing tones   |
//   by Walk and Oct Jump; Length sets how much of the gap each note fills           |
//                         |                                                         |
//                         v                                                         |
//              BassVoice (Sub / 808 / Pluck / Acid / Reese) --> duck on kicks ------+--> out
//
// Two ways to follow (the Follow switch):
//   Grid  learns where the kicks fall in the bar while the MPC is playing and plays those steps
//         exactly on time from the song position. It needs one or two passes to learn a new
//         pattern; until then it plays each kick as it hears it. When the transport is stopped
//         it behaves like Hits.
//   Hits  plays a note on every kick it hears, about 5 ms after the kick starts (the time it takes
//         to be sure it was a kick). Busy adds one or two follow-up notes after each kick.
//
// Where the notes come from (the Changes knob): Follow plays the root of the chord it hears;
// Tonic always plays the key's root; the numbered progressions walk the scale degrees of the key
// (1-6-7-1 is i-VI-VII-i in a minor key) one chord per Chord Len.
//
// Realtime: nothing is allocated, locked or logged in process(). The chroma analysis is spread
// over several blocks (Harmony::work). Pitch and filter maths run every 16 samples.
//
// CPU: roughly 2-4% of one MPC core. Kick tracker ~0.3%, harmony ~0.7%, the voice the rest
// (Reese, with two saws, is the most expensive).

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/tempo.h"

#include "KickTracker.h"
#include "Harmony.h"
#include "BassVoice.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {

// ------------------------------------------------------------------------------------------
// Parameters. The order is permanent once anyone has a project: append, never insert.
// ------------------------------------------------------------------------------------------
enum ParamIndex
{
    // GROOVE
    kOutput = 0,   // master volume. The host nudges parameter 0 on every load: keep it harmless.
    kBass,         // level of the bass against the dry signal
    kFollow,       // Grid (learned, on time) or Hits (every kick as heard)
    kSens,         // how quiet a kick may be and still count

    kBusy,         // how many notes it adds between the kicks
    kRate,         // the grid those extra notes sit on
    kSwing,        // MPC-style swing for the extra notes (50% = straight)
    kVary,         // how often the extra notes change, per two bars

    // NOTES
    kChanges,      // Follow (heard chords), Tonic, or a progression in the key
    kChordLen,     // how long each chord of a progression lasts
    kWalk,         // fifths, passing tones and approach notes instead of roots
    kJump,         // chance of jumping up an octave

    kListen,       // Follow the key it hears, Lock it once sure, or Off (use Key/Scale)
    kKey,          // root used when Listen is Off, or until it is sure
    kScale,        // scale used likewise; also flavours a heard key (Dorian for a minor key...)
    kOctave,       // register of the whole line

    // SOUND
    kVoice,        // Sub, 808, Pluck, Acid, Reese
    kTone,         // low-pass cutoff
    kReso,         // filter resonance
    kDrive,        // saturation before the filter

    kDecay,        // envelope decay
    kPunch,        // pitch drop (Sub, 808) or filter envelope depth (Pluck, Acid, Reese)
    kGlide,        // slide time between overlapping notes
    kLength,       // how much of the gap to the next note each note fills; 100% = legato

    // MIX
    kDuck,         // how far the bass ducks under each kick
    kDuckTime,     // how long it takes to come back
    kSolo,         // Bass Only: mute the dry signal, to hear what it is playing

    // Meters, written by the plugin. Not saved, not automatable.
    kFoundKey,     // the key it hears ("--" until it is fairly sure)
    kSure,         // how sure it is of that key
    kChordNow,     // the chord the bass is playing over right now
    kKickMeter,    // flashes on every kick it hears

    kNumParams
};

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Progressions as zero-based scale degrees, in the order of the Changes choices after Follow/Tonic.
const int kProgs[][4] = {
    {0, 5, 6, 0},   // 1-6-7-1
    {0, 3, 4, 0},   // 1-4-5-1
    {0, 4, 5, 3},   // 1-5-6-4
    {0, 6, 5, 6},   // 1-7-6-7
    {0, 3, 5, 4},   // 1-4-6-5
    {0, 5, 3, 4},   // 1-6-4-5
};
const float kChordBars[4] = {0.5f, 1.0f, 2.0f, 4.0f};
const int kRateSteps[3] = {4, 2, 1};   // in 16ths: 1/4, 1/8, 1/16

constexpr int   kMaxSteps = 64;        // two bars of 16ths at up to 8/4... clamped to this
constexpr float kOnWeight = 0.45f;     // a step with at least this much kick history is a kick step
constexpr float kLearn = 0.6f;         // how fast a heard kick raises its step
constexpr float kForget = 0.55f;       // how fast a step without a kick fades, per pass
constexpr int   kMaxEvents = 32;
constexpr int   kLowestRoot = 28;      // E1: chord roots sit in E1..D#2, plus Octave

enum NoteType { kNone = 0, kAnchor, kExtra };

inline uint32_t mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}
// Deterministic 0..1 from a step, a seed and a purpose, so a pattern repeats until it varies.
inline float hash01(uint32_t step, uint32_t seed, uint32_t salt)
{
    return (float)(mix32(step * 0x9E3779B1u ^ mix32(seed + salt * 0x85EBCA77u)) >> 8) * (1.0f / 16777216.0f);
}
inline int modi(long long a, int m) { long long r = a % m; return (int)(r < 0 ? r + m : r); }

std::vector<std::string> keyMeterNames()
{
    std::vector<std::string> v{"--"};
    for (const char* n : kNoteNames) v.emplace_back(n);
    for (const char* n : kNoteNames) v.emplace_back(std::string(n) + "m");
    return v;
}
std::vector<std::string> chordMeterNames()
{
    std::vector<std::string> v{"--"};
    for (const char* n : kNoteNames)
    {
        v.emplace_back(n);
        v.emplace_back(std::string(n) + "m");
        v.emplace_back(std::string(n) + "dim");
    }
    return v;
}

} // namespace

class Largemouth : public mpc::Plugin
{
public:
    Largemouth() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        auto meter = [this](Param p) { p.automatable = false; p.saved = false; addParam(p); };

        // GROOVE
        addParam(floatParam("output", "Output", -24.0f, 6.0f, 0.0f, "dB", 1));
        addParam(floatParam("bass", "Bass", -36.0f, 6.0f, -4.0f, "dB", 1));
        addParam(choiceParam("follow", "Follow", {"Grid", "Hits"}, 0));
        addParam(floatParam("sens", "Kick Sens", 0.0f, 100.0f, 60.0f, "%", 0));

        addParam(floatParam("busy", "Busy", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(choiceParam("rate", "Rate", {"1/4", "1/8", "1/16"}, 2));
        addParam(floatParam("swing", "Swing", 50.0f, 75.0f, 50.0f, "%", 0));
        addParam(floatParam("vary", "Vary", 0.0f, 100.0f, 25.0f, "%", 0));

        // NOTES
        addParam(choiceParam("changes", "Changes",
                             {"Follow", "Tonic", "1-6-7-1", "1-4-5-1", "1-5-6-4", "1-7-6-7", "1-4-6-5", "1-6-4-5"}, 0));
        addParam(choiceParam("chordlen", "Chord Len", {"1/2 Bar", "1 Bar", "2 Bars", "4 Bars"}, 1));
        addParam(floatParam("walk", "Walk", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("jump", "Oct Jump", 0.0f, 100.0f, 15.0f, "%", 0));

        addParam(choiceParam("listen", "Listen", {"Follow", "Lock", "Off"}, 0));
        addParam(choiceParam("key", "Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0));
        {
            std::vector<std::string> names;
            for (const ScaleDef& s : kScales) names.emplace_back(s.name);
            addParam(choiceParam("scale", "Scale", names, 0));
        }
        addParam(intParam("octave", "Octave", -1, 2, 0, "oct"));

        // SOUND
        addParam(choiceParam("voice", "Voice", {"Sub", "808", "Pluck", "Acid", "Reese"}, 1));
        addParam(logParam("tone", "Tone", 60.0f, 8000.0f, 1200.0f, "Hz", 0));
        addParam(floatParam("reso", "Reso", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 25.0f, "%", 0));

        addParam(logParam("decay", "Decay", 40.0f, 4000.0f, 600.0f, "ms", 0));
        addParam(floatParam("punch", "Punch", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam("glide", "Glide", 0.0f, 400.0f, 30.0f, "ms", 0));
        addParam(floatParam("length", "Length", 5.0f, 100.0f, 70.0f, "%", 0));

        // MIX
        addParam(floatParam("duck", "Duck", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam(logParam("ducktime", "Duck Time", 20.0f, 500.0f, 120.0f, "ms", 0));
        addParam(boolParam("solo", "Bass Only", false));

        meter(choiceParam("foundkey", "Found", keyMeterNames(), 0));
        meter(floatParam("sure", "Sure", 0.0f, 100.0f, 0.0f, "%", 0));
        meter(choiceParam("chordnow", "Chord", chordMeterNames(), 0));
        meter(floatParam("kick", "Kick", 0.0f, 100.0f, 0.0f, "%", 0));

        addPrograms();
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = sampleRate;
        kick_.prepare(sampleRate);
        harm_.prepare(sampleRate);
        voice_.prepare(sampleRate);
        duckSmooth_ = 1.0f - std::exp(-1.0f / (0.0015f * (float)sampleRate));
        gainSmooth_ = 1.0f - std::exp(-1.0f / (0.02f * (float)sampleRate));
        meterInterval_ = (int)(0.1 * sampleRate);
        reset();
    }

    void reset() override
    {
        kick_.reset();
        harm_.reset();
        voice_.reset();
        clearPattern();
        gridWasActive_ = false;
        lastFiredG_ = lastClosedG_ = 0;
        reseedDoneG_ = -1;
        lastPpqEnd_ = 0.0;
        seed_ = 0x1234567u;
        rng_ = 0xC0FFEEu;
        for (Pending& p : pending_) p.countdown = -1;
        noteOffCountdown_ = -1;
        samplesSinceNote_ = 1 << 30;
        samplesSinceDuck_ = 1 << 30;
        samplesSinceHit_ = 1 << 30;
        avgHitInterval_ = (float)(0.5 * sr_);
        duckTarget_ = duckEnv_ = 0.0f;
        outGain_ = mpc::dsp::dbToGain(get(kOutput));
        bassGain_ = mpc::dsp::dbToGain(get(kBass));
        dryGain_ = getBool(kSolo) ? 0.0f : 1.0f;
        locked_ = false; lockTimer_ = 0; haveFound_ = false;
        kickFlash_ = 0.0f;
        meterCountdown_ = 0;
        for (float& m : lastMeter_) m = -1.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const int numOut = mpc::clamp(ctx.numOutputs, 0, 2);
        if (n <= 0 || numOut == 0) return;

        // ---- parameters, once per block --------------------------------------------------
        kick_.setSensitivity(get(kSens) * 0.01f);
        const float tOut = mpc::dsp::dbToGain(get(kOutput));
        const float tBass = mpc::dsp::dbToGain(get(kBass));
        const float tDry = getBool(kSolo) ? 0.0f : 1.0f;
        const int follow = getInt(kFollow);
        busy_ = get(kBusy) * 0.01f;
        rateSteps_ = kRateSteps[mpc::clamp(getInt(kRate), 0, 2)];
        swingShift_ = mpc::clamp((get(kSwing) * 0.02f) - 1.0f, 0.0f, 0.45f);
        walk_ = get(kWalk) * 0.01f;
        jump_ = get(kJump) * 0.01f;
        changes_ = getInt(kChanges);
        chordLen_ = mpc::clamp(getInt(kChordLen), 0, 3);
        octave_ = getInt(kOctave);
        length_ = get(kLength) * 0.01f;
        const float duckDepth = get(kDuck) * 0.01f;
        const float duckDecay = std::exp(-1.0f / (get(kDuckTime) * 0.001f / 3.0f * (float)sr_));
        voice_.setParams(getInt(kVoice), get(kTone), get(kReso) * 0.01f, get(kDrive) * 0.01f,
                         get(kDecay), get(kPunch) * 0.01f, get(kGlide));

        updateKey(n);
        harm_.setKey(keyRoot_, keyScale_);

        // ---- transport ---------------------------------------------------------------------
        const mpc::Transport& tr = ctx.transport;
        bpm_ = mpc::dsp::safeBpm(tr.tempoValid ? tr.bpm : 120.0);
        spb_ = sr_ * 60.0 / bpm_;                                     // samples per beat
        num_ = tr.timeSigNumerator > 0 ? tr.timeSigNumerator : 4;
        den_ = tr.timeSigDenominator > 0 ? tr.timeSigDenominator : 4;
        const bool gridOk = tr.valid && tr.playing && tr.ppqValid;
        ppqValid_ = gridOk;
        ppq0_ = tr.ppqPos;
        const double ppqEnd = ppq0_ + (double)n / spb_;

        const int stepsPerBar = mpc::clamp((int)std::lround((double)num_ * 16.0 / (double)den_), 1, kMaxSteps / 2);
        if (2 * stepsPerBar != steps_) { steps_ = 2 * stepsPerBar; clearPattern(); }

        int numEvents = 0;
        if (gridOk)
        {
            const bool jumped = ppq0_ < lastPpqEnd_ - 1e-3 || ppq0_ > lastPpqEnd_ + 0.5;
            if (!gridWasActive_ || jumped)
            {
                lastFiredG_ = (long long)std::ceil(ppq0_ * 4.0 - 1e-6) - 1;
                lastClosedG_ = lastFiredG_;
                reseedDoneG_ = lastFiredG_;
                for (bool& h : hitSeen_) h = false;
            }
            closeSteps();
            hasKicks_ = false;
            for (int s = 0; s < steps_; ++s) if (weight_[s] >= kOnWeight) { hasKicks_ = true; break; }
            if (follow == 0) numEvents = scheduleEvents(ppqEnd);
            else lastFiredG_ = (long long)std::ceil(ppqEnd * 4.0 - 1e-6) - 1;   // keep the counter current
        }
        gridWasActive_ = gridOk;
        lastPpqEnd_ = ppqEnd;
        const bool liveMode = (follow == 1) || !gridOk;

        // ---- audio -------------------------------------------------------------------------
        const float* inL = (ctx.inputs && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* inR = (ctx.inputs && ctx.numInputs > 1) ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = numOut > 1 ? ctx.outputs[1] : nullptr;

        int ev = 0;
        for (int i = 0; i < n; ++i)
        {
            const float l = inL ? inL[i] : 0.0f;
            const float r = inR ? inR[i] : l;
            const float mono = 0.5f * (l + r);

            if (kick_.process(mono)) onHit(i, liveMode);
            harm_.push(mono);

            while (ev < numEvents && events_[ev].sample <= i)
            {
                const Event& e = events_[ev++];
                const int s = modi(e.g, steps_);
                const float vel = e.type == kAnchor ? stepVel_[s] : 0.55f + 0.3f * hash01((uint32_t)s, seed_, 51);
                const float gap = (float)(gapSteps(e.g) * 0.25 * spb_);
                playNote(e.type, e.g * 0.25, (uint32_t)s, vel, gap, true);
            }

            for (Pending& p : pending_)
                if (p.countdown >= 0 && --p.countdown < 0)
                {
                    const double beats = ppqValid_ ? ppq0_ + (double)i / spb_ : 0.0;
                    playNote(kExtra, beats, nextRand(), 0.5f + 0.3f * randf(), p.gap, ppqValid_);
                }

            if (noteOffCountdown_ >= 0 && --noteOffCountdown_ < 0) voice_.noteOff();

            // Duck: jump on a kick, recover exponentially, with a 1.5 ms edge so it never clicks.
            duckTarget_ *= duckDecay;
            duckEnv_ += duckSmooth_ * (duckTarget_ - duckEnv_);

            outGain_ += gainSmooth_ * (tOut - outGain_);
            bassGain_ += gainSmooth_ * (tBass - bassGain_);
            dryGain_ += gainSmooth_ * (tDry - dryGain_);

            const float bass = voice_.process() * bassGain_ * (1.0f - duckDepth * duckEnv_);
            outL[i] = (l * dryGain_ + bass) * outGain_;
            if (outR) outR[i] = (r * dryGain_ + bass) * outGain_;

            // Saturating counters: they only ever need to say "a long time ago".
            if (samplesSinceNote_ < (1 << 30)) ++samplesSinceNote_;
            if (samplesSinceDuck_ < (1 << 30)) ++samplesSinceDuck_;
            if (samplesSinceHit_ < (1 << 30)) ++samplesSinceHit_;
        }

        harm_.work();
        updateMeters(n);
    }

private:
    struct Event { int sample; int type; long long g; };
    struct Pending { int countdown = -1; float gap = 0.0f; };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Largemouth";
        info.vendor       = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1100;            // 1.1.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 44100 * 4;      // a long 808 rings on after the drums stop
        return info;
    }
    static constexpr char kUid[5] = "G72X";

    // ------------------------------------------------------------------------------------------
    // Key: which key the notes are chosen in.
    // ------------------------------------------------------------------------------------------
    void updateKey(int n)
    {
        const int listen = getInt(kListen);
        const int userKey = mpc::clamp(getInt(kKey), 0, 11);
        const int userScale = mpc::clamp(getInt(kScale), 0, kNumScales - 1);
        if (listen != 1) { locked_ = false; lockTimer_ = 0; }

        const float conf = harm_.confidence();
        if (conf >= 0.35f) { haveFound_ = true; foundRoot_ = harm_.foundRoot(); foundMinor_ = harm_.foundMinor(); }

        int root = userKey;
        bool minor = kScales[userScale].minorish;
        bool useFound = false;
        if (listen == 0) useFound = haveFound_;
        else if (listen == 1)
        {
            if (!locked_)
            {
                lockTimer_ = conf >= 0.6f ? lockTimer_ + n : 0;
                if (lockTimer_ > (int)(2.0 * sr_))
                { locked_ = true; lockedRoot_ = harm_.foundRoot(); lockedMinor_ = harm_.foundMinor(); }
            }
            if (locked_) { root = lockedRoot_; minor = lockedMinor_; }
            else useFound = haveFound_;
        }
        if (useFound) { root = foundRoot_; minor = foundMinor_; }

        // The Scale knob flavours a heard key: Dorian or Phrygian for a minor key, Mixolydian for
        // a major one. A scale of the other kind falls back to plain minor / major.
        int scale = userScale;
        if (listen != 2 && (useFound || locked_) && kScales[userScale].minorish != minor) scale = minor ? 0 : 1;
        keyRoot_ = root;
        keyScale_ = scale;
    }

    // ------------------------------------------------------------------------------------------
    // Chords: which scale degree the bass is on at a given song position (beats).
    // ------------------------------------------------------------------------------------------
    int degreeAt(double beats) const
    {
        if (changes_ == 0) return harm_.chordValid() ? harm_.chordDegree() : 0;
        if (changes_ == 1 || changes_ - 2 >= (int)(sizeof(kProgs) / sizeof(kProgs[0]))) return 0;
        if (!ppqValid_) return kProgs[changes_ - 2][0];
        const double beatsPerBar = (double)num_ * 4.0 / (double)den_;
        const double chordBeats = kChordBars[chordLen_] * beatsPerBar;
        const long long idx = (long long)std::floor(beats / chordBeats + 1e-6);
        return kProgs[changes_ - 2][modi(idx, 4)];
    }

    int rootMidiForDegree(int d) const
    {
        const int pc = (keyRoot_ + kScales[keyScale_].notes[d]) % 12;
        return kLowestRoot + modi(pc - (kLowestRoot % 12), 12) + 12 * octave_;
    }

    // Semitones from scale degree d to the degree k steps above it (k may be negative or > 6).
    int degreeOffset(int d, int k) const
    {
        const ScaleDef& sc = kScales[keyScale_];
        const int idx = d + k;
        const int oct = (int)std::floor((double)idx / 7.0);
        return sc.notes[modi(idx, 7)] + 12 * oct - sc.notes[d];
    }

    float pickNote(int type, double beats, uint32_t s)
    {
        const int d = degreeAt(beats);
        const int base = rootMidiForDegree(d);
        const float hj = hash01(s, seed_, 11), hw = hash01(s, seed_, 23), hv = hash01(s, seed_, 37);
        int semi = 0;
        if (type == kAnchor)
        {
            if (hj < jump_ * 0.5f) semi = 12;
        }
        else
        {
            // About to change chord? Lead into the next root from a step away.
            const int dNext = degreeAt(beats + rateSteps_ * 0.25);
            if (dNext != d && hw < walk_)
            {
                const int target = rootMidiForDegree(dNext) - base;
                if (walk_ > 0.65f) semi = target + (hv < 0.5f ? -1 : 1);                 // chromatic
                else semi = target + degreeOffset(dNext, hv < 0.5f ? -1 : 1);             // in the scale
            }
            else if (hw < walk_ * 0.45f)
            {
                static const int kSteps[4] = {1, 2, -1, 6};    // 2nd, 3rd, step below, 7th
                semi = degreeOffset(d, kSteps[(int)(hv * 4.0f) & 3]);
            }
            else if (hw < walk_)
                semi = degreeOffset(d, 4);                     // the fifth
            else
                semi = hj < jump_ ? 12 : 0;
        }
        return (float)mpc::clamp(base + semi, 20, 72);
    }

    // ------------------------------------------------------------------------------------------
    // Groove memory
    // ------------------------------------------------------------------------------------------
    void clearPattern()
    {
        for (int s = 0; s < kMaxSteps; ++s) { weight_[s] = 0.0f; offset_[s] = 0.0f; stepVel_[s] = 0.8f; hitSeen_[s] = false; }
        hasKicks_ = false;
    }

    // What step g of the song plays, from the current memory and seed. Deterministic, so the
    // same answer comes back when asking ahead (for note lengths).
    int plan(long long g) const
    {
        const int s = modi(g, steps_);
        if (weight_[s] >= kOnWeight) return kAnchor;
        if (busy_ <= 0.0f || !hasKicks_ || modi(g, rateSteps_) != 0) return kNone;
        float pw;
        if (weight_[modi(s + 1, steps_)] >= kOnWeight) pw = 1.0f;     // pickup into a kick
        else if (s % 4 == 2) pw = 0.85f;                             // offbeat 8th
        else if (s % 4 == 0) pw = 0.6f;                              // beat
        else pw = 0.45f;                                             // in-between 16th
        if (weight_[modi(s - 1, steps_)] >= kOnWeight) pw *= 0.4f;   // leave room after a kick
        return hash01((uint32_t)s, seed_, 3) < busy_ * pw * 1.3f ? kExtra : kNone;
    }

    double timeOf(long long g, int type) const
    {
        double off = 0.0;
        if (type == kAnchor) off = offset_[modi(g, steps_)];
        else if (type == kExtra && (g & 1)) off = swingShift_;
        return ((double)g + off) * 0.25;
    }

    long long gapSteps(long long g) const
    {
        for (long long k = 1; k <= steps_; ++k) if (plan(g + k) != kNone) return k;
        return 4;
    }

    // Close steps whose window has passed: a step that had no kick this time fades.
    void closeSteps()
    {
        const double latencyBeats = kick_.latencySamples() / spb_;
        const double closeBefore = ppq0_ - latencyBeats - 0.01;
        int guard = 0;
        while (((double)(lastClosedG_ + 1) + 0.5) * 0.25 < closeBefore && guard++ < 256)
        {
            const int s = modi(++lastClosedG_, steps_);
            if (!hitSeen_[s]) weight_[s] *= kForget;
            hitSeen_[s] = false;
        }
    }

    int scheduleEvents(double ppqEnd)
    {
        int count = 0;
        for (int guard = 0; guard < 64; ++guard)
        {
            const long long g = lastFiredG_ + 1;
            if (modi(g, steps_) == 0 && reseedDoneG_ < g)
            {
                reseedDoneG_ = g;
                if (randf() < get(kVary) * 0.01f) seed_ = nextRand();
            }
            const int type = plan(g);
            const double t = timeOf(g, type);
            if (t >= ppqEnd) break;
            lastFiredG_ = g;
            if (type == kNone || count >= kMaxEvents) continue;
            const int sample = mpc::clamp((int)((t - ppq0_) * spb_), 0, 1 << 20);
            events_[count++] = {sample, type, g};
        }
        return count;
    }

    // ------------------------------------------------------------------------------------------
    // A kick was heard at sample i of this block.
    // ------------------------------------------------------------------------------------------
    void onHit(int i, bool liveMode)
    {
        const float vel = kick_.velocity();
        kickFlash_ = 1.0f;
        if (samplesSinceHit_ < (1 << 29))
            avgHitInterval_ += 0.3f * (mpc::clamp((float)samplesSinceHit_, 0.08f * (float)sr_, 2.0f * (float)sr_) - avgHitInterval_);
        samplesSinceHit_ = 0;
        triggerDuck();

        bool scheduled = false;
        double beats = 0.0;
        if (ppqValid_)
        {
            beats = ppq0_ + (double)(i - kick_.latencySamples()) / spb_;
            const double gf = beats * 4.0;
            const long long gi = (long long)std::llround(gf);
            const float d = mpc::clamp((float)(gf - (double)gi), -0.45f, 0.45f);
            const int s = modi(gi, steps_);
            scheduled = weight_[s] >= kOnWeight;
            offset_[s] = scheduled ? offset_[s] + 0.35f * (d - offset_[s]) : d;
            weight_[s] += (1.0f - weight_[s]) * kLearn;
            stepVel_[s] += 0.5f * (vel - stepVel_[s]);
            hitSeen_[s] = true;
        }

        const double stepSamples = 0.25 * spb_;
        if (!liveMode)
        {
            // Grid: a kick the memory did not expect is played now, unless a note just went out.
            if (!scheduled && samplesSinceNote_ > (int)(0.6 * stepSamples))
                playNote(kAnchor, beats, (uint32_t)modi((long long)std::llround(beats * 4.0), steps_), vel,
                         (float)(4.0 * stepSamples), ppqValid_);
            return;
        }

        // Hits: play it now, and maybe one or two follow-ups on the Rate grid.
        for (Pending& p : pending_) p.countdown = -1;
        const float rateSamples = (float)(rateSteps_ * stepSamples);
        bool any = false;
        for (int j = 0; j < 2; ++j)
        {
            if (randf() < busy_ * (j == 0 ? 0.9f : 0.55f) && rateSamples * (float)(j + 1) < avgHitInterval_ * 0.95f)
            {
                pending_[j].countdown = (int)(rateSamples * (float)(j + 1));
                pending_[j].gap = rateSamples;
                any = true;
            }
        }
        const float gap = any ? rateSamples : avgHitInterval_;
        playNote(kAnchor, beats, nextRand(), vel, gap, ppqValid_);
    }

    void playNote(int type, double beats, uint32_t s, float vel, float gapSamples, bool haveBeats)
    {
        const float midi = pickNote(type, haveBeats ? beats : 0.0, s);
#ifdef LARGEMOUTH_NOTE_HOOK
        LARGEMOUTH_NOTE_HOOK(type, midi, vel);   // tests only: see tests/listentest.cpp
#endif
        voice_.noteOn(midi, vel, voice_.gate());
        gapSamples = mpc::clamp(gapSamples, 0.02f * (float)sr_, 4.0f * (float)sr_);
        noteOffCountdown_ = length_ >= 0.99f ? (int)(gapSamples + 0.02f * (float)sr_)   // overlap: legato
                                             : (int)(length_ * gapSamples);
        samplesSinceNote_ = 0;
        if (type == kAnchor) triggerDuck();
    }

    void triggerDuck()
    {
        if (samplesSinceDuck_ < (int)(0.06 * sr_)) return;
        duckTarget_ = 1.0f;
        samplesSinceDuck_ = 0;
    }

    // ------------------------------------------------------------------------------------------
    // Meters: once every 100 ms, and only when they moved.
    // ------------------------------------------------------------------------------------------
    void updateMeters(int n)
    {
        meterCountdown_ -= n;
        if (meterCountdown_ > 0) return;
        meterCountdown_ = meterInterval_;

        const float conf = harm_.confidence();
        const float found = conf >= 0.2f ? (float)(1 + harm_.foundRoot() + (harm_.foundMinor() ? 12 : 0)) : 0.0f;
        sendMeter(0, kFoundKey, found / 24.0f);
        sendMeter(1, kSure, std::round(conf * 100.0f) * 0.01f);

        const int d = degreeAt(ppqValid_ ? ppq0_ : 0.0);
        const ScaleDef& sc = kScales[keyScale_];
        const int third = modi(sc.notes[(d + 2) % 7] - sc.notes[d], 12);
        const int fifth = modi(sc.notes[(d + 4) % 7] - sc.notes[d], 12);
        const int quality = third == 3 ? (fifth == 6 ? 2 : 1) : 0;
        const int pc = (keyRoot_ + sc.notes[d]) % 12;
        sendMeter(2, kChordNow, (float)(1 + pc * 3 + quality) / 36.0f);

        sendMeter(3, kKickMeter, kickFlash_ > 0.05f ? std::round(kickFlash_ * 10.0f) * 0.1f : 0.0f);
        kickFlash_ *= 0.4f;
    }

    void sendMeter(int slot, int param, float norm)
    {
        if (norm == lastMeter_[slot]) return;
        lastMeter_[slot] = norm;
        setNormalised(param, norm);
        notifyHostParameterChanged(param);
    }

    // ------------------------------------------------------------------------------------------
    uint32_t nextRand() { rng_ = rng_ * 1664525u + 1013904223u; return rng_; }
    float randf() { return (float)(nextRand() >> 8) * (1.0f / 16777216.0f); }

    void addPrograms()
    {
        auto prog = [this](const char* name, std::initializer_list<std::pair<int, float>> changes)
        {
            std::vector<float> v((size_t)kNumParams);
            for (int i = 0; i < kNumParams; ++i) v[(size_t)i] = param(i).def;
            for (const auto& c : changes) v[(size_t)c.first] = c.second;
            addProgram(name, v);
        };
        prog("Init 808", {});
        prog("Deep 808", {{kVoice, 1}, {kBusy, 12}, {kDecay, 1100}, {kPunch, 50}, {kDrive, 35},
                          {kTone, 900}, {kLength, 95}, {kDuck, 25}, {kWalk, 15}, {kJump, 5}});
        prog("Sub Anchor", {{kVoice, 0}, {kBusy, 0}, {kWalk, 0}, {kJump, 0}, {kLength, 60},
                            {kDecay, 900}, {kPunch, 25}, {kDuck, 50}, {kTone, 400}});
        prog("Rolling Pluck", {{kVoice, 2}, {kBusy, 60}, {kRate, 2}, {kWalk, 35}, {kJump, 25},
                               {kTone, 450}, {kPunch, 60}, {kDecay, 320}, {kLength, 55}, {kDuck, 20}});
        prog("Acid Line", {{kVoice, 3}, {kBusy, 55}, {kWalk, 50}, {kJump, 30}, {kTone, 320}, {kReso, 70},
                           {kPunch, 55}, {kDecay, 260}, {kGlide, 60}, {kLength, 100}, {kDrive, 40},
                           {kOctave, 1}, {kDuck, 15}});
        prog("Reese Garage", {{kVoice, 4}, {kSwing, 62}, {kBusy, 35}, {kTone, 700}, {kDecay, 1500},
                              {kLength, 85}, {kGlide, 45}, {kDuck, 45}, {kPunch, 20}});
        prog("Dub Steps", {{kVoice, 0}, {kRate, 1}, {kBusy, 30}, {kChanges, 2}, {kChordLen, 2},
                           {kWalk, 20}, {kDecay, 800}, {kLength, 75}, {kTone, 300}, {kDuck, 40}});
        prog("Boom Bap", {{kVoice, 1}, {kBusy, 10}, {kSwing, 58}, {kPunch, 30}, {kDecay, 500},
                          {kTone, 1400}, {kLength, 70}, {kWalk, 20}});
    }

    // ---- state ---------------------------------------------------------------------------------
    double sr_ = 44100.0;
    KickTracker kick_;
    Harmony harm_;
    BassVoice voice_;

    // block-rate copies of parameters
    float busy_ = 0.25f, swingShift_ = 0.0f, walk_ = 0.3f, jump_ = 0.15f, length_ = 0.7f;
    int rateSteps_ = 1, changes_ = 0, chordLen_ = 1, octave_ = 0;

    // key
    int keyRoot_ = 0, keyScale_ = 0;
    bool haveFound_ = false, foundMinor_ = true, locked_ = false, lockedMinor_ = true;
    int foundRoot_ = 0, lockedRoot_ = 0, lockTimer_ = 0;

    // transport
    double bpm_ = 120.0, spb_ = 22050.0, ppq0_ = 0.0, lastPpqEnd_ = 0.0;
    int num_ = 4, den_ = 4;
    bool ppqValid_ = false, gridWasActive_ = false;

    // groove memory
    int steps_ = 32;
    float weight_[kMaxSteps] = {}, offset_[kMaxSteps] = {}, stepVel_[kMaxSteps] = {};
    bool hitSeen_[kMaxSteps] = {};
    bool hasKicks_ = false;
    long long lastFiredG_ = 0, lastClosedG_ = 0, reseedDoneG_ = -1;
    uint32_t seed_ = 0x1234567u, rng_ = 0xC0FFEEu;
    Event events_[kMaxEvents] = {};

    // notes
    Pending pending_[2];
    int noteOffCountdown_ = -1;
    int samplesSinceNote_ = 0, samplesSinceDuck_ = 0, samplesSinceHit_ = 0;
    float avgHitInterval_ = 22050.0f;

    // output
    float duckTarget_ = 0.0f, duckEnv_ = 0.0f, duckSmooth_ = 0.01f, gainSmooth_ = 0.001f;
    float outGain_ = 1.0f, bassGain_ = 1.0f, dryGain_ = 1.0f;

    // meters
    float kickFlash_ = 0.0f;
    int meterCountdown_ = 0, meterInterval_ = 4410;
    float lastMeter_[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(Largemouth)
