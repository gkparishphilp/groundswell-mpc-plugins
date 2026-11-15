// Tidepools - a granular call-and-response effect for MPC standalone hardware.
//
// SPDX-License-Identifier: MIT
// (c) Groundswell
//
// ============================================================================================
// WHAT IT IS
// ============================================================================================
//
// Tidepools listens to what you play, waits until you stop, and answers with it.
//
// That is the whole idea, and it is why the plugin is built the way it is. A granular pedal
// gets a call-and-response feel somewhat by accident: it granulates a rolling window of the
// last few seconds, and if you happen to stop playing you hear that window on its own. Here it
// is done on purpose. An envelope follower watches the input and marks where a phrase starts.
// When you stop, and the silence lasts longer than Gap, Tidepools decides you have finished,
// waits for the next beat or bar if you asked it to, and plays the phrase back through one of
// four grain engines. Come back in and it ducks out of your way, so you can trade phrases.
//
// The signal flow:
//
//   in -+----------------------------------------------------------------> dry ---+--> out
//       |                                                                         |
//       +-> input gain -> capture buffer (8 s, always rolling)                    |
//                              |                                                  |
//       envelope follower -----+--> phrase detector (start, end, Gap)             |
//                              |         |                                        |
//                              |         v                                        |
//                              |    answer scheduler (Now / next Beat / next Bar) |
//                              |         |                                        |
//                              +---------+--> grain engine (16 voices) -----+     |
//                                                Cloud / Blocks / Glide / Bubbles |
//                                                        |                        |
//                                          tone low-pass +-> reverb (4-line FDN) -+
//                                                        |
//                                                 trade ducking
//
// ============================================================================================
// THE FOUR ENGINES
// ============================================================================================
//
//   Cloud    Grains scattered through the phrase at the pitch you played. The plain texture:
//            a wash that thickens with Activity and smears with Shape.
//   Blocks   The phrase cut into tempo-locked slices and handed back in a shuffled pattern,
//            some slices repeated, some reversed, some left as silence. The pattern is seeded
//            per answer, so each reply is a different but coherent arrangement, not mush.
//   Glide    Grains whose playback rate ramps while they sound, plus a slower drift across the
//            whole answer, so the reply bends into or away from the pitch you played.
//   Bubbles  Short grains, each through its own resonant band-pass, so every one becomes a
//            pitched blip rather than a piece of audio. Their frequencies can be snapped to a
//            scale, which turns a single held note into a sparkling arpeggio in key.
//
// Two knobs mean something slightly different in each engine, which is noted on every control:
// Scatter is about position and order, Motion is about pitch and movement.
//
// ============================================================================================
// REALTIME
// ============================================================================================
//
// Nothing allocates, locks or does I/O in process(). All buffers come from prepare(). Grain
// voices are a fixed pool; when they are all busy a new grain steals the oldest. Filter
// coefficients are computed when a grain starts, never per sample.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"
#include "dsp/noise.h"
#include "dsp/tempo.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace mpc::dsp;

namespace {

// ------------------------------------------------------------------------------------------
// Parameters. The order is permanent once anyone has a project: append, never insert.
// ------------------------------------------------------------------------------------------
enum ParamIndex
{
    kOutput = 0, kMix, kMode, kActivity, kShape, kScatter, kMotion,
    kSync, kTime, kRepeats, kTone, kSpace, kDecay,
    kListen, kGap, kAnswer, kReply, kInterval, kScale, kRoot, kTrade, kSpread,
    kHold, kInput,
    kLevel, kState,          // meters: the plugin writes these, nobody automates them
    kKeyMode, kHeard,        // appended: pitch tracking
    kGapSync,                // appended: Gap in note divisions
    kSens,                   // appended: how readily the detector decides a phrase is over
    kChange,                 // appended: how often Chord moves to the next chord
    kGlideDir,               // appended: where Glide slides to
    kNumParams
};

enum Mode   { kCloud = 0, kBlocks, kGlide, kBubbles, kChord, kRoll, kEbb };
enum Listen { kListenOff = 0, kListenAuto, kListenHold };
enum Answer { kAnswerNow = 0, kAnswerBeat, kAnswerBar };
enum KeyMode { kKeyFixed = 0, kKeyFollow };
enum GlideDir { kGlideDown = 0, kGlideUp, kGlideMixed };

// Pitch detection. The phrase is analysed once, when it ends, so the cost lands in one block an
// answer or two apart rather than on every sample. Decimating by four first is what makes that
// affordable: pitch lives below 1.5 kHz, so 11 kHz is plenty, and the search shrinks sixteenfold.
constexpr int   kDecim = 4;
constexpr int   kYinWindow = 512;          // 46 ms at 11 kHz: three periods of the lowest note
constexpr int   kYinMinLag = 9;            // about 1225 Hz
constexpr int   kYinMaxLag = 140;          // about 79 Hz
constexpr float kYinGood = 0.22f;          // below this the estimate is trusted
constexpr float kYinUsable = 0.45f;        // above this the phrase is called unpitched

constexpr float kCaptureSeconds = 8.0f;    // how far back the buffer remembers
constexpr int   kMaxGrains = 16;
constexpr int   kWindowBits = 10;
constexpr int   kWindowSize = 1 << kWindowBits;    // raised-cosine grain envelope
// Gap as a note division. The table runs to two bars, which is nonsense for "have they stopped
// playing yet"; half a note is the longest that means anything. The result is clamped to the
// knob's own range, so a slow tempo cannot make it wait forever.
// Where Chord goes next, in degrees of the scale, counted from the note you played: its own
// chord, then the one a sixth above, the fourth, the fifth. I - vi - IV - V, relative to whatever
// you happened to play, so it lands in the key wherever you are in it.
constexpr int   kChordWalk[4] = { 0, 5, 3, 4 };
// Where Glide can land, in semitones, nearest first. A whole tone is a wobble; a fourth, a fifth
// and an octave are intervals you would actually play, which is what makes a long slide sound
// deliberate instead of broken. Motion decides how far down this list it is allowed to reach.
constexpr int   kGlideTargets[4] = { 2, 5, 7, 12 };
// A major scale in semitones, for working those moves out when no Scale is set.
constexpr int   kMajorSteps[7] = { 0, 2, 4, 5, 7, 9, 11 };

constexpr int   kGapMaxDivision = 13;      // 1/2
constexpr float kGapMinSeconds = 0.04f, kGapMaxSeconds = 1.5f;
constexpr float kMinPhraseSeconds = 0.08f;
constexpr float kMaxPhraseSeconds = 6.0f;
constexpr float kWetCeiling = 0.7079f;     // -3 dBFS: where the grain limiter holds the wet path

// Transparent below `knee`, then a tanh curve that never passes `ceiling`.
inline float softKnee(float x, float knee, float ceiling)
{
    const float a = std::fabs(x);
    if (a <= knee) return x;
    const float range = ceiling - knee;
    const float y = knee + range * fastTanh((a - knee) / range);
    return x < 0.0f ? -y : y;
}

// Scales for Bubbles. Semitones from the root; the last entry of each is the count.
constexpr int kMaxScaleNotes = 12;
struct ScaleDef { const char* name; int count; int notes[kMaxScaleNotes]; };
const ScaleDef kScales[] = {
    { "Off",       12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
    { "Major",      7, { 0, 2, 4, 5, 7, 9, 11 } },
    { "Minor",      7, { 0, 2, 3, 5, 7, 8, 10 } },
    { "Penta Maj",  5, { 0, 2, 4, 7, 9 } },
    { "Penta Min",  5, { 0, 3, 5, 7, 10 } },
    { "Dorian",     7, { 0, 2, 3, 5, 7, 9, 10 } },
    { "Whole Tone", 6, { 0, 2, 4, 6, 8, 10 } },
};
constexpr int kNumScales = (int)(sizeof(kScales) / sizeof(kScales[0]));

// A reverb line's length in samples at 44.1 kHz. Mutually prime-ish so the tail does not ring.
constexpr int kFdnLines = 4;
constexpr int kFdnLengths[kFdnLines] = { 1931, 2749, 3671, 4493 };

// ------------------------------------------------------------------------------------------
// One grain: a windowed read from the capture buffer.
// ------------------------------------------------------------------------------------------
struct Grain
{
    bool   active = false;
    double pos = 0.0;          // absolute read position in the capture buffer, fractional
    double rate = 1.0;         // buffer samples consumed per output sample (negative = reverse)
    double rateInc = 0.0;      // added to rate every sample: Glide's bend
    int    age = 0, length = 1;
    int    wait = 0;           // samples to hold off before sounding: Chord's strum
    float  gainL = 0.5f, gainR = 0.5f;
    float  amp = 1.0f;
    bool   filtered = false;   // Bubbles
    float  filtGain = 1.0f;    // 1/Q of the band-pass, so a resonant bubble is not 12x too loud
    SVF    band;
};

// ------------------------------------------------------------------------------------------
// A four-line feedback delay network: the Space control. Cheap, dense, and it does not ring
// the way a spring does, which suits a grain cloud better.
// ------------------------------------------------------------------------------------------
class Fdn
{
public:
    void prepare(double sampleRate)
    {
        const float scale = (float)sampleRate / 44100.0f;
        for (int i = 0; i < kFdnLines; ++i)
        {
            length_[i] = (int)(kFdnLengths[i] * scale);
            if (length_[i] < 16) length_[i] = 16;
            line_[i].prepare(length_[i] + 4);
        }
        sr_ = sampleRate;
        setDamping(8000.0f);
        reset();
    }
    void reset()
    {
        for (int i = 0; i < kFdnLines; ++i) { line_[i].reset(); damp_[i].reset(); }
    }
    void setDamping(float hz)
    {
        for (int i = 0; i < kFdnLines; ++i) damp_[i].setCutoff(hz, sr_);
    }
    // `feedback` is the per-pass gain of each line, 0..0.999.
    void process(float inL, float inR, float feedback, float& outL, float& outR)
    {
        float v[kFdnLines];
        for (int i = 0; i < kFdnLines; ++i) v[i] = line_[i].readInt(length_[i]);

        // Hadamard mixing: every line feeds every other, with no multiplies beyond the halving.
        const float a = v[0] + v[1], b = v[2] + v[3], c = v[0] - v[1], d = v[2] - v[3];
        float m[kFdnLines] = { 0.5f * (a + b), 0.5f * (c + d), 0.5f * (a - b), 0.5f * (c - d) };

        const float in[kFdnLines] = { inL, inR, inR, inL };
        for (int i = 0; i < kFdnLines; ++i)
            line_[i].write(flushDenormal(in[i] * 0.5f + damp_[i].process(m[i]) * feedback));

        outL = 0.5f * (v[0] + v[2]);
        outR = 0.5f * (v[1] + v[3]);
    }

private:
    DelayLine  line_[kFdnLines];
    OnePoleLP  damp_[kFdnLines];
    int        length_[kFdnLines] = {};
    double     sr_ = 44100.0;
};

} // namespace

// ==========================================================================================

class Tidepools : public mpc::Plugin
{
public:
    Tidepools() : mpc::Plugin(makeInfo())
    {
        // Parameter 0 is nudged by the host on every load, so it has to be harmless.
        addParam(mpc::floatParam ("out",      "Output",   -30.0f, 6.0f,   0.0f,  "dB", 1));
        addParam(mpc::floatParam ("mix",      "Mix",       0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::choiceParam("mode",     "Mode",     { "Cloud", "Blocks", "Glide", "Bubbles",
                                                            "Chord", "Roll", "Ebb" }, kCloud));
        addParam(mpc::floatParam ("activity", "Activity",  0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam ("shape",    "Shape",     0.0f, 100.0f, 45.0f, "%", 0));
        addParam(mpc::floatParam ("scatter",  "Scatter",   0.0f, 100.0f, 35.0f, "%", 0));
        addParam(mpc::floatParam ("motion",   "Motion",    0.0f, 100.0f, 30.0f, "%", 0));
        addParam(mpc::choiceParam("sync",     "Time Mode", { "Free", "Sync" }, 1));
        addParam(mpc::logParam   ("time",     "Time",      30.0f, 4000.0f, 250.0f, "", 0));  // text from formatValue
        addParam(mpc::floatParam ("repeats",  "Repeats",   0.0f, 100.0f, 40.0f, "%", 0));
        addParam(mpc::logParam   ("tone",     "Tone",      200.0f, 16000.0f, 9000.0f, "Hz", 0));
        addParam(mpc::floatParam ("space",    "Space",     0.0f, 100.0f, 30.0f, "%", 0));
        addParam(mpc::logParam   ("decay",    "Decay",     0.3f, 12.0f,  3.0f,  "s", 1));
        addParam(mpc::choiceParam("listen",   "Listen",   { "Off", "Auto", "Hold" }, kListenAuto));
        addParam(mpc::logParam   ("gap",      "Gap",       40.0f, 1500.0f, 220.0f, "ms", 0));
        addParam(mpc::choiceParam("answer",   "Answer",   { "Now", "Beat", "Bar" }, kAnswerBeat));
        addParam(mpc::floatParam ("reply",    "Reply",     0.0f, 100.0f, 35.0f, "%", 0));
        addParam(mpc::floatParam ("interval", "Interval", -12.0f, 12.0f,  0.0f,  "st", 0));
        addParam(mpc::choiceParam("scale",    "Scale",    { "Off", "Major", "Minor", "Penta Maj",
                                                            "Penta Min", "Dorian", "Whole Tone" }, 0));
        addParam(mpc::choiceParam("root",     "Root",     { "C", "C#", "D", "D#", "E", "F", "F#", "G",
                                                            "G#", "A", "A#", "B" }, 0));
        addParam(mpc::floatParam ("trade",    "Trade",     0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam ("spread",   "Spread",    0.0f, 100.0f, 70.0f, "%", 0));
        addParam(mpc::boolParam  ("hold",     "Hold",      false));
        addParam(mpc::floatParam ("input",    "Input",    -30.0f, 12.0f,  0.0f,  "dB", 1));

        // Meters. The plugin writes these from the audio thread so the screen can follow what it
        // is doing; they are not automation targets and are kept out of saved state.
        mpc::Param level = mpc::floatParam("level", "Level", 0.0f, 1.0f, 0.0f, "", 2);
        level.automatable = false; level.saved = false;
        addParam(level);
        mpc::Param state = mpc::choiceParam("state", "State",
                                            { "Idle", "Hear", "Wait", "Talk" }, 0);
        state.automatable = false; state.saved = false;
        addParam(state);

        // Appended after the meters, so nothing above moves.
        addParam(mpc::choiceParam("keymode", "Key", { "Fixed", "Follow" }, kKeyFixed));
        mpc::Param heard = mpc::choiceParam("heard", "Heard",
                                            { "-", "C", "C#", "D", "D#", "E", "F",
                                              "F#", "G", "G#", "A", "A#", "B" }, 0);
        heard.automatable = false; heard.saved = false;
        addParam(heard);
        addParam(mpc::choiceParam("gapsync", "Gap Mode", { "Free", "Sync" }, 0));
        addParam(mpc::floatParam ("sens",     "Sens",      0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::choiceParam("change",   "Change",   { "Off", "1 Bar", "2 Bars", "4 Bars" }, 1));
        addParam(mpc::choiceParam("glidedir", "Glide",    { "Down", "Up", "Mixed" }, kGlideMixed));

        addPrograms();
    }

    // --------------------------------------------------------------------------------------
    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = (float)sampleRate;

        int size = 16;
        while (size < (int)(kCaptureSeconds * sr_)) size <<= 1;
        bufMask_ = size - 1;
        for (int c = 0; c < 2; ++c) buf_[c].assign((size_t)size, 0.0f);

        // A mono copy at a quarter of the rate, only ever read by the pitch detector.
        decBuf_.assign((size_t)(size / kDecim), 0.0f);
        decMask_ = size / kDecim - 1;
        // Anti-alias before throwing samples away: two poles at 3.5 kHz, which is well above any
        // fundamental we look for and well below the decimated Nyquist.
        decLp_.setCutoff(3500.0f, sr_);
        decLp2_.setCutoff(3500.0f, sr_);

        if (window_.empty())
        {
            window_.resize(kWindowSize);
            for (int i = 0; i < kWindowSize; ++i)
                window_[(size_t)i] = 0.5f * (1.0f - std::cos(6.2831853f * (float)i / (float)kWindowSize));
        }

        for (Grain& g : grain_) g.band.setSampleRate(sampleRate);
        for (int c = 0; c < 2; ++c) { tone_[c].setSampleRate(sampleRate); fdn_[c].prepare(sampleRate); }

        mix_.setTime(20.0f, sampleRate);
        output_.setTime(20.0f, sampleRate);
        input_.setTime(20.0f, sampleRate);
        wetTrim_.setTime(30.0f, sampleRate);

        limAttack_  = 1.0f - std::exp(-1.0f / (0.0005f * sr_));  // 0.5 ms
        limRelease_ = 1.0f - std::exp(-1.0f / (0.120f * sr_));   // 120 ms
        envAttack_  = 1.0f - std::exp(-1.0f / (0.002f * sr_));   // 2 ms
        envRelease_ = 1.0f - std::exp(-1.0f / (0.060f * sr_));   // 60 ms
        envFastRelease_ = 1.0f - std::exp(-1.0f / (0.022f * sr_));   // 22 ms: two cycles at 80 Hz
        armFall_ = 1.0f - std::exp(-1.0f / (0.400f * sr_));   // 400 ms: how fast re-arming relaxes
        answerGainRate_ = 1.0f - std::exp(-1.0f / (0.004f * sr_));   // 4 ms
        meterFall_  = 1.0f - std::exp(-1.0f / (0.150f * sr_));

        rng_.reseed(0x51A3C7D9u);
        reset();
    }

    void reset() override
    {
        std::fill(decBuf_.begin(), decBuf_.end(), 0.0f);
        decLp_.reset(); decLp2_.reset();
        heardMidi_ = -1.0f;
        heardClass_ = -1;
        lastHeardSent_ = -1.0f;
        analyse_ = false;
        for (int c = 0; c < 2; ++c)
        {
            std::fill(buf_[c].begin(), buf_[c].end(), 0.0f);
            tone_[c].reset();
            fdn_[c].reset();
        }
        for (Grain& g : grain_) { g.active = false; g.band.reset(); }
        writePos_ = 0;
        env_ = 0.0f;
        envFast_ = 0.0f;
        phrasePeak_ = 0.0f;
        armLevel_ = 0.0f;
        answerGainSm_ = 0.0f;
        listening_ = false;
        gapCount_ = 0;
        phraseStart_ = phraseEnd_ = 0;
        ansStart_ = ansEnd_ = 0;
        answering_ = false;
        pending_ = false;
        pendingCount_ = 0;
        answerPos_ = 0;
        answerGain_ = 1.0f;
        passIndex_ = 0;
        spawnCount_ = 0;
        blockIndex_ = 0;
        rollPos_ = 0.0;
        rollHold_ = 0;
        meter_ = 0.0f;
        lastMeterSent_ = -1.0f;
        lastStateSent_ = -1.0f;
        meterCountdown_ = 0;
        duck_ = 1.0f;
        limGain_ = 1.0f;
        mix_.snap(get(kMix) * 0.01f);
        output_.snap(dbToGain(get(kOutput)));
        input_.snap(dbToGain(get(kInput)));
        wetTrim_.snap(1.0f);
    }

    // --------------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];

        // ---- per-block settings ----------------------------------------------------------
        mix_.setTarget(get(kMix) * 0.01f);
        output_.setTarget(dbToGain(get(kOutput)));
        input_.setTarget(dbToGain(get(kInput)));

        const int   mode     = clampi(getInt(kMode), 0, 6);
        const int   listen   = clampi(getInt(kListen), 0, 2);
        const int   answerAt = clampi(getInt(kAnswer), 0, 2);
        const float activity = get(kActivity) * 0.01f;
        const float shape    = get(kShape) * 0.01f;
        const float scatter  = get(kScatter) * 0.01f;
        const float motion   = get(kMotion) * 0.01f;
        const float repeats  = get(kRepeats) * 0.01f;
        const float reply    = get(kReply) * 0.01f;
        const float spread   = get(kSpread) * 0.01f;
        const float trade    = get(kTrade) * 0.01f;
        const bool  hold     = getBool(kHold);

        const double bpm = ctx.transport.bpm > 1.0 ? ctx.transport.bpm : 120.0;
        const float  beatSamples = (float)(60.0 / bpm) * sr_;

        // Time: a note division when synced, milliseconds when free. It sets the slice length in
        // Blocks and the longest grain everywhere else.
        sliceSamples_ = getInt(kSync) == 1
            ? divisionBeats(divisionFromPosition(getNormalised(kTime))) * beatSamples
            : get(kTime) * 0.001f * sr_;
        sliceSamples_ = clampf(sliceSamples_, 0.01f * sr_, 4.0f * sr_);

        // Gap in milliseconds, or as a note division of the MPC tempo so the listening window
        // scales with the track the way Time already does.
        const float gapSamples = getInt(kGapSync) == 1
            ? clampf(divisionBeats(gapDivisionFromPosition(getNormalised(kGap))) * beatSamples,
                     kGapMinSeconds * sr_, kGapMaxSeconds * sr_)
            : get(kGap) * 0.001f * sr_;
        const float toneHz = clampf(get(kTone), 100.0f, 0.45f * sr_);
        for (int c = 0; c < 2; ++c) tone_[c].set(toneHz, 0.1f);

        const float space = get(kSpace) * 0.01f;
        const float decay = clampf(get(kDecay), 0.3f, 12.0f);
        // Per-pass gain that gives the asked-for RT60 on the shortest line.
        const float fdnFeedback = clampf(std::pow(0.001f, (float)kFdnLengths[0] / (decay * 44100.0f)),
                                         0.0f, 0.999f);
        for (int c = 0; c < 2; ++c) fdn_[c].setDamping(clampf(toneHz, 500.0f, 12000.0f));

        // Change: how long one chord holds, in samples. A stopped transport still gives bars,
        // off the 120 bpm fallback, so Chord keeps moving when the MPC is not running.
        const int changeSel = clampi(getInt(kChange), 0, 3);
        const int tsNum = ctx.transport.timeSigNumerator > 0 ? ctx.transport.timeSigNumerator : 4;
        chordSpan_ = (int64_t)clampf((float)(1 << (changeSel > 0 ? changeSel - 1 : 0))
                                     * (float)tsNum * beatSamples, 0.25f * sr_, 60.0f * sr_);

        const double baseRate = answerRatio();

        // The thresholds that decide "you are playing" and "you have stopped", set by Sens.
        //
        // A fixed floor cannot do this job. Ask it to call a note finished at -56 dBFS and a
        // long-decay patch - a string, a pad, anything with a tail - sits above that for seconds
        // or for ever, so the phrase never ends and nothing is ever answered. So the shut
        // threshold is relative: a phrase is over once it has dropped a given distance below its
        // own loudest point, which is what "stopped playing" actually sounds like. The absolute
        // floor stays underneath it, for quiet playing that never got loud to begin with.
        const float sens = clampf(get(kSens) * 0.01f, 0.0f, 1.0f);
        const float openThresh = dbToGain(-44.0f - 10.0f * sens);     // -44 dBFS .. -54 dBFS
        const float shutFloor  = openThresh * 0.4f;                   // 8 dB of hysteresis
        const float shutDrop   = dbToGain(-34.0f + 24.0f * sens);     // -34 dB .. -10 dB of peak
        // And a phrase that simply never drops - a held drone - has to be cut off somewhere, or
        // again nothing is answered at all.
        const int64_t maxListen = (int64_t)(clampf(6.0f - 4.8f * sens,
                                                   kMinPhraseSeconds * 2.0f,
                                                   kMaxPhraseSeconds) * sr_);

        const int maxPhrase = (int)(kMaxPhraseSeconds * sr_);
        const int minPhrase = (int)(kMinPhraseSeconds * sr_);

        // ---- per sample -------------------------------------------------------------------
        for (int n = 0; n < ctx.numFrames; ++n)
        {
            const float dryL = inL[n], dryR = inR[n];
            const float g = input_.process();
            const float capL = dryL * g, capR = dryR * g;

            // Follower on what is going in. One number drives phrase detection and the ducking.
            const float peak = std::fmax(std::fabs(capL), std::fabs(capR));
            env_ += (peak > env_ ? envAttack_ : envRelease_) * (peak - env_);
            envFast_ += (peak > envFast_ ? envAttack_ : envFastRelease_) * (peak - envFast_);

            // Capture. The buffer always rolls unless something has frozen it, so there is always
            // a phrase to answer with and Trade can hear you come back in.
            const bool frozen = (listen == kListenHold && hold) || (answering_ && hold);
            if (!frozen)
            {
                const int i = (int)(writePos_ & (int64_t)bufMask_);
                buf_[0][(size_t)i] = capL;
                buf_[1][(size_t)i] = capR;
                const float mono = decLp2_.process(decLp_.process(0.5f * (capL + capR)));
                if ((writePos_ & (kDecim - 1)) == 0)
                    decBuf_[(size_t)((writePos_ >> 2) & (int64_t)decMask_)] = mono;
                ++writePos_;
            }

            // ---- phrase detection --------------------------------------------------------
            if (listen == kListenAuto)
            {
                // Re-arming. A phrase that ended on its decay leaves the input still well above
                // the open threshold, so without this the tail of one note would open phrase
                // after phrase and each new answer would cut off the last. The level the phrase
                // ended at has to be beaten before anything counts as playing again - either by
                // a new attack, or by the old note getting out of the way, which is what the
                // relaxing does.
                armLevel_ -= armFall_ * armLevel_;

                if (!listening_ && envFast_ > openThresh && envFast_ > armLevel_)
                {
                    // A phrase begins. Back-date the start a little so the attack is not clipped.
                    listening_ = true;
                    phraseStart_ = writePos_ - (int64_t)(0.01f * sr_);
                    phrasePeak_ = envFast_;
                    gapCount_ = 0;
                }
                else if (listening_)
                {
                    if (envFast_ > phrasePeak_) phrasePeak_ = envFast_;
                    const float shut = std::fmax(shutFloor, phrasePeak_ * shutDrop);

                    if (envFast_ > shut) gapCount_ = 0; else ++gapCount_;

                    const bool wentQuiet = gapCount_ > (int)gapSamples;
                    const bool ranOn     = (writePos_ - phraseStart_) > maxListen;
                    if (wentQuiet || ranOn)
                    {
                        // Either a long enough drop, or a phrase that would not end by itself.
                        listening_ = false;
                        // Trim the trailing quiet, unless there was none to trim.
                        phraseEnd_ = wentQuiet ? writePos_ - gapCount_ : writePos_;
                        armLevel_ = envFast_ * 1.4f;   // ~3 dB above where it left off
                        int64_t len = phraseEnd_ - phraseStart_;
                        if (len > maxPhrase) phraseStart_ = phraseEnd_ - maxPhrase;
                        if (len >= minPhrase)
                        {
                            // Analysing here would put 66k multiply-adds inside one sample slot.
                            // Flag it instead and do it once, at the end of the block.
                            analyse_ = true;
                            scheduleAnswer(answerAt, ctx, beatSamples, n);
                        }
                    }
                }
            }
            else if (listen == kListenHold)
            {
                // Hold: the buffer freezes and the grains work it continuously.
                if (hold && !answering_)
                {
                    phraseEnd_ = writePos_;
                    phraseStart_ = phraseEnd_ - (int64_t)clampf(sliceSamples_ * 4.0f,
                                                                (float)minPhrase, (float)maxPhrase);
                    beginAnswer();
                }
                else if (!hold && answering_)
                    answering_ = false;
            }
            else   // Off: granulate the live rolling window. No listening, no waiting, no decay.
            {
                phraseEnd_ = writePos_;
                phraseStart_ = phraseEnd_ - (int64_t)clampf(sliceSamples_ * 4.0f,
                                                            (float)minPhrase, (float)maxPhrase);
                if (!answering_) beginAnswer();
                ansStart_ = phraseStart_;      // the window follows the input, it is not a phrase
                ansEnd_ = phraseEnd_;
                answerGain_ = 1.0f;
            }

            // A waiting answer starts when its beat arrives.
            if (pending_ && --pendingCount_ <= 0) beginAnswer();

            // ---- grain scheduling --------------------------------------------------------
            if (answering_)
            {
                const int64_t phraseLen = (ansEnd_ - ansStart_) > 0 ? (ansEnd_ - ansStart_) : 1;
                if (--spawnCount_ <= 0)
                {
                    // Roll's grains butt up against each other to make a continuous roll, so the
                    // grain needs to know how long it has before the next one.
                    const int gap = nextSpawnInterval(mode, activity);
                    spawnGrain(mode, shape, scatter, motion, spread, baseRate, phraseLen, gap);
                    spawnCount_ = gap;
                }
                // The chord moves on its own clock, not the phrase's, so it lands with the track.
                if (mode == kChord && changeSel > 0 && --chordCount_ <= 0)
                {
                    chordIndex_ = (chordIndex_ + 1) & 3;
                    chordSlice_ = patternFloat();
                    chordCount_ = chordSpan_;
                }
                ++answerPos_;
                if (answerPos_ >= phraseLen)
                {
                    // One pass of the phrase done. Decay, and let Reply move the dice for the next.
                    answerPos_ = 0;
                    ++passIndex_;
                    blockIndex_ = 0;
                    answerGain_ *= 0.15f + 0.85f * repeats;
                    patternSeed_ = patternSeed_ * 1664525u + 1013904223u + (uint32_t)(reply * 8191.0f);
                    if (listen == kListenAuto && answerGain_ < 0.02f) answering_ = false;
                }
            }

            // ---- render the grains --------------------------------------------------------
            float wetL = 0.0f, wetR = 0.0f;
            for (Grain& gr : grain_)
            {
                if (!gr.active) continue;
                if (gr.wait > 0) { --gr.wait; continue; }
                float s = readBuffer(0, gr.pos);
                float sR = readBuffer(1, gr.pos);
                if (gr.filtered)
                {
                    const float mono = 0.5f * (s + sR);
                    s = sR = gr.band.process(mono, SVF::Mode::Bandpass) * gr.filtGain;
                }
                const float w = window_[(size_t)((gr.age << kWindowBits) / gr.length) & (kWindowSize - 1)]
                                * gr.amp;
                wetL += s * w * gr.gainL;
                wetR += sR * w * gr.gainR;
                gr.pos += gr.rate;
                gr.rate += gr.rateInc;
                if (++gr.age >= gr.length) gr.active = false;
            }

            // Grains overlap, so the sum grows with density: hold it to roughly unity.
            const float trim = wetTrim_.process();
            answerGainSm_ += answerGainRate_ * (answerGain_ - answerGainSm_);
            wetL *= trim * answerGainSm_;
            wetR *= trim * answerGainSm_;

            // Tone, then the reverb, which is fed from the grains only.
            wetL = tone_[0].process(wetL, SVF::Mode::Lowpass);
            wetR = tone_[1].process(wetR, SVF::Mode::Lowpass);
            if (space > 0.0f)
            {
                float rL = 0.0f, rR = 0.0f;
                fdn_[0].process(wetL, wetR, fdnFeedback, rL, rR);
                wetL += space * (rL - wetL * 0.3f);
                wetR += space * (rR - wetR * 0.3f);
            }

            // Wet limiter: fast enough to catch a grain's attack, slow enough not to pump.
            const float lp = std::fmax(std::fabs(wetL), std::fabs(wetR));
            const float want = lp > kWetCeiling ? kWetCeiling / lp : 1.0f;
            limGain_ += (want < limGain_ ? limAttack_ : limRelease_) * (want - limGain_);
            wetL = softKnee(wetL * limGain_, 0.8f * kWetCeiling, kWetCeiling);
            wetR = softKnee(wetR * limGain_, 0.8f * kWetCeiling, kWetCeiling);

            // Trade: get out of the way while the player is playing.
            const float duckTarget = 1.0f - trade * clampf(env_ * 12.0f, 0.0f, 1.0f);
            duck_ += (duckTarget < duck_ ? 0.01f : 0.0004f) * (duckTarget - duck_);
            wetL *= duck_;
            wetR *= duck_;

            // Mix: dry holds full level to half way, wet arrives by half way.
            const float m = mix_.process();
            const float dryGain = std::fmin(1.0f, 2.0f * (1.0f - m));
            const float wetGain = std::fmin(1.0f, 2.0f * m);
            const float outGain = output_.process();
            float oL = (dryL * dryGain + wetL * wetGain) * outGain;
            float oR = (dryR * dryGain + wetR * wetGain) * outGain;
            outL[n] = softClip(oL);
            outR[n] = softClip(oR);

            const float o = std::fmax(std::fabs(outL[n]), std::fabs(outR[n]));
            meter_ += (o > meter_ ? 0.5f : meterFall_) * (o - meter_);
        }

        if (analyse_)
        {
            analyse_ = false;
            detectPitch();
        }

        // Keep the grain sum near unity however many are sounding.
        wetTrim_.setTarget(1.0f / std::sqrt((float)countActive() + 1.0f) * 1.6f);

        sendMeters(ctx.numFrames);
    }

    // --------------------------------------------------------------------------------------
    bool formatValue(int index, float plainValue, std::string& out) override
    {
        char buf[48];
        if (index == kInput && plainValue <= -29.95f) { out = "-inf dB"; return true; }
        if (index == kTime)
        {
            if (getInt(kSync) == 1)
                out = divisionName(divisionFromPosition(param(kTime).toNormalised(plainValue)));
            else { std::snprintf(buf, sizeof(buf), "%.0f ms", (double)plainValue); out = buf; }
            return true;
        }
        if (index == kGap && getInt(kGapSync) == 1)
        {
            out = divisionName(gapDivisionFromPosition(param(kGap).toNormalised(plainValue)));
            return true;
        }
        if (index == kInterval)
        {
            const int st = (int)std::lround(plainValue);
            std::snprintf(buf, sizeof(buf), st == 0 ? "0 st" : "%+d st", st);
            out = buf;
            return true;
        }
        return false;
    }

private:
    // ----- helpers -------------------------------------------------------------------------
    static int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static int   divisionFromPosition(float norm)
    {
        return clampi((int)std::lround(norm * (kNumDivisions - 1)), 0, kNumDivisions - 1);
    }
    static int   gapDivisionFromPosition(float norm)
    {
        return clampi((int)std::lround(norm * kGapMaxDivision), 0, kGapMaxDivision);
    }

    float readBuffer(int ch, double pos) const
    {
        const double fl = std::floor(pos);
        const int i0 = (int)((int64_t)fl & (int64_t)bufMask_);
        const int i1 = (i0 + 1) & bufMask_;
        const float f = (float)(pos - fl);
        const float a = buf_[ch][(size_t)i0], b = buf_[ch][(size_t)i1];
        return a + (b - a) * f;
    }

    int countActive() const
    {
        int n = 0;
        for (const Grain& g : grain_) if (g.active) ++n;
        return n;
    }

    // Wait for the next beat or bar before answering, so the reply lands on the grid.
    void scheduleAnswer(int answerAt, const mpc::ProcessContext& ctx, float beatSamples, int frame)
    {
        if (answerAt == kAnswerNow || !ctx.transport.playing || !ctx.transport.ppqValid)
        {
            beginAnswer();
            return;
        }
        const double beatsPerUnit = answerAt == kAnswerBar
            ? (double)(ctx.transport.timeSigNumerator > 0 ? ctx.transport.timeSigNumerator : 4)
            : 1.0;
        const double ppq = ctx.transport.ppqPos + (double)frame / (double)beatSamples;
        const double next = std::ceil(ppq / beatsPerUnit) * beatsPerUnit;
        const double wait = (next - ppq) * (double)beatSamples;
        pending_ = true;
        pendingCount_ = (int)clampf((float)wait, 1.0f, 8.0f * sr_);
    }

    void beginAnswer()
    {
        // Copy the phrase the answer is about to play. From here the detector is free to mark up
        // the next one without pulling the ground out from under this answer.
        ansStart_ = phraseStart_;
        ansEnd_   = phraseEnd_;
        if (ansEnd_ - ansStart_ < 64) ansStart_ = ansEnd_ - 64;
        pending_ = false;
        answering_ = true;
        answerPos_ = 0;
        answerGain_ = 1.0f;
        passIndex_ = 0;
        blockIndex_ = 0;
        spawnCount_ = 1;
        rollHold_ = 0;
        shCount_ = 0;
        glidePass_ = -1;
        glideFrom_ = glideTo_ = 0.0f;
        chordIndex_ = 0;
        chordCount_ = chordSpan_;
        chordSlice_ = 0.0f;
        patternSeed_ = rng_.next();
    }

    int nextSpawnInterval(int mode, float activity) const
    {
        if (mode == kBlocks) return (int)clampf(sliceSamples_, 64.0f, 4.0f * sr_);
        if (mode == kRoll)
        {
            // A roll that tightens: start at the slice length and subdivide it as the pass runs,
            // up to eight times faster by the end. Motion is how far it goes.
            const int64_t len = (ansEnd_ - ansStart_) > 0 ? (ansEnd_ - ansStart_) : 1;
            const float t = clampf((float)answerPos_ / (float)len, 0.0f, 1.0f);
            const float divide = 1.0f + get(kMotion) * 0.01f * 7.0f * t;
            return (int)clampf(sliceSamples_ / divide, 48.0f, 4.0f * sr_);
        }
        // A bubble is a distinct pitched blip rather than a grain of wash, so the ear counts them
        // one by one and the same Activity reads as far busier. Half the density keeps the knob
        // meaning roughly the same thing whichever engine is running.
        // Bubbles are counted one by one by the ear, and every Chord spawn is two to four grains
        // at once: both want a lower ceiling than a wash does.
        const float top = (mode == kBubbles) ? 38.0f : (mode == kChord ? 26.0f : 86.0f);
        // 4 grains a second at the bottom of Activity, about 90 at the top - 42 for Bubbles.
        const float density = 4.0f + activity * activity * top;
        return (int)clampf(sr_ / density, 16.0f, sr_);
    }

    // What the scale is built on. Fixed means the Root knob, which is the key of the music and is
    // almost always what you want: the detected pitch then says where the note you played sits in
    // that key, which is how "+4" can come back as the third the key actually has. Follow takes
    // the root from the phrase instead, which lands right only if you started on the tonic.
    int scaleRoot() const
    {
        if (getInt(kKeyMode) == kKeyFollow && heardClass_ >= 0) return heardClass_;
        return clampi(getInt(kRoot), 0, 11);
    }

    float snapMidiToScale(float midi) const
    {
        const int scaleIndex = clampi(getInt(kScale), 0, kNumScales - 1);
        if (scaleIndex == 0) return midi;
        const ScaleDef& sc = kScales[scaleIndex];
        const int root = scaleRoot();
        const float rel = midi - (float)root;
        const float octave = std::floor(rel / 12.0f);
        const float within = rel - octave * 12.0f;
        float best = (float)sc.notes[0], bestD = 99.0f;
        for (int i = 0; i < sc.count; ++i)
        {
            const float d = std::fabs(within - (float)sc.notes[i]);
            if (d < bestD) { bestD = d; best = (float)sc.notes[i]; }   // ties resolve downward
        }
        return (float)root + octave * 12.0f + best;
    }

    // Snap a frequency to the chosen scale, so Bubbles sparkles in key instead of at random.
    float snapToScale(float hz) const
    {
        if (clampi(getInt(kScale), 0, kNumScales - 1) == 0) return hz;
        return midiToHz(snapMidiToScale(hzToMidi(clampf(hz, 20.0f, 18000.0f))));
    }

    // Which degree of the scale a root-relative offset is nearest to.
    static int degreeOf(const ScaleDef& sc, float withinOctave)
    {
        int best = 0;
        float bestD = 99.0f;
        for (int i = 0; i < sc.count; ++i)
        {
            const float d = std::fabs(withinOctave - (float)sc.notes[i]);
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    // How much the answer is transposed.
    //
    // Without a scale, Interval is plain semitones. With one, it becomes a DIATONIC interval: the
    // knob is read as a number of scale steps, and the answer moves that many degrees from
    // wherever the note you played sits in the key. So "+4" is a third, and it comes back major
    // from the first degree and minor from the second, the way a third does.
    //
    // Doing it by steps rather than by snapping the shifted note matters. Snapping leaves the
    // tritone ambiguous - F# in C major is one semitone from both F and G - so a note detected a
    // hundredth of a semitone sharp would answer a fourth up where a flat one answered a third.
    // Counting degrees has no such edge: the source is quantised once, and the step count is exact.
    double answerRatio() const
    {
        const float interval = get(kInterval);
        const int si = clampi(getInt(kScale), 0, kNumScales - 1);
        if (si == 0 || heardMidi_ < 0.0f) return (double)semitonesToRatio(interval);
        const ScaleDef& sc = kScales[si];
        const int root = scaleRoot();

        // Where the phrase sits in the key.
        const float rel = heardMidi_ - (float)root;
        const float oct = std::floor(rel / 12.0f);
        const int srcDeg = degreeOf(sc, rel - oct * 12.0f);

        // How many degrees the knob is asking for.
        const float ask = std::fabs(interval);
        const float askOct = std::floor(ask / 12.0f);
        int steps = (int)askOct * sc.count + degreeOf(sc, ask - askOct * 12.0f);
        if (interval < 0.0f) steps = -steps;

        // Move that many degrees, carrying octaves.
        const int deg = srcDeg + steps;
        const int shift = (int)std::floor((float)deg / (float)sc.count);
        const int within = deg - shift * sc.count;
        const float target = (float)root + (oct + (float)shift) * 12.0f + (float)sc.notes[within];
        const float source = (float)root + oct * 12.0f + (float)sc.notes[srcDeg];
        return (double)semitonesToRatio(target - source);
    }

    // YIN, on the decimated copy: the squared difference between the phrase and itself delayed by
    // tau, normalised by its own running mean so that a flat, quiet passage cannot look periodic.
    // Runs once per phrase.
    void detectPitch()
    {
        const int64_t len = ansEndCandidate() - phraseStart_;
        const int need = (kYinWindow + kYinMaxLag) * kDecim;
        if (len < need) { setHeard(-1.0f); return; }

        // A third of the way in: past the attack transient, before the note has decayed away.
        const int64_t from = phraseStart_ + (len - need) / 3;
        const int64_t base = from >> 2;

        auto at = [this, base](int i) { return decBuf_[(size_t)((base + i) & (int64_t)decMask_)]; };

        // Too quiet to have a pitch at all.
        float energy = 0.0f;
        for (int j = 0; j < kYinWindow; ++j) energy += at(j) * at(j);
        if (energy / kYinWindow < 1.0e-7f) { setHeard(-1.0f); return; }

        float runningSum = 0.0f;
        int bestLag = -1;
        float bestVal = 1.0e9f;
        for (int tau = 1; tau <= kYinMaxLag; ++tau)
        {
            float d = 0.0f;
            for (int j = 0; j < kYinWindow; ++j)
            {
                const float diff = at(j) - at(j + tau);
                d += diff * diff;
            }
            runningSum += d;
            yin_[tau] = runningSum > 0.0f ? d * (float)tau / runningSum : 1.0f;
            if (tau < kYinMinLag) continue;
            if (yin_[tau] < bestVal) { bestVal = yin_[tau]; bestLag = tau; }
            // The first dip under the threshold wins, which is what stops YIN picking an octave
            // too low: a deeper minimum usually sits at twice the true period.
            if (tau > kYinMinLag && yin_[tau - 1] < kYinGood && yin_[tau] > yin_[tau - 1])
            {
                bestLag = tau - 1;
                bestVal = yin_[tau - 1];
                break;
            }
        }
        if (bestLag < kYinMinLag || bestVal > kYinUsable) { setHeard(-1.0f); return; }

        // Parabolic interpolation around the dip, so the estimate is not quantised to whole lags.
        float lag = (float)bestLag;
        if (bestLag > 1 && bestLag < kYinMaxLag)
        {
            const float a = yin_[bestLag - 1], b = yin_[bestLag], c = yin_[bestLag + 1];
            const float denom = a - 2.0f * b + c;
            if (std::fabs(denom) > 1.0e-9f) lag += 0.5f * (a - c) / denom;
        }
        const float hz = (sr_ / (float)kDecim) / clampf(lag, 2.0f, (float)kYinMaxLag);
        setHeard(hzToMidi(clampf(hz, 20.0f, 4000.0f)));
    }

    // The phrase end as it stands when the detector runs (the answer may not have copied it yet).
    int64_t ansEndCandidate() const { return phraseEnd_; }

    void setHeard(float midi)
    {
        heardMidi_ = midi;
        if (midi < 0.0f) { heardClass_ = -1; return; }
        int cls = (int)std::lround(midi) % 12;
        if (cls < 0) cls += 12;
        heardClass_ = cls;
    }

    uint32_t patternRand()
    {
        patternSeed_ = patternSeed_ * 1664525u + 1013904223u;
        return patternSeed_;
    }
    float patternFloat() { return (float)(patternRand() >> 8) * (1.0f / 16777216.0f); }

    // ----- the engines ---------------------------------------------------------------------
    void spawnGrain(int mode, float shape, float scatter, float motion,
                    float spread, double baseRate, int64_t phraseLen, int gap)
    {
        Grain* g = takeGrain();

        // Reply nudges the dice every pass, so the next answer is a variation, not a copy.
        const float drift = get(kReply) * 0.01f * (float)passIndex_ * 0.25f;
        const float jitter = clampf(scatter + drift, 0.0f, 1.0f);

        double start = (double)ansStart_;
        int    length = 0;
        double rate = baseRate;
        double rateInc = 0.0;
        bool   filtered = false;
        float  amp = 1.0f;

        switch (mode)
        {
            case kBlocks:
            {
                // The phrase cut into slices, handed back in a seeded order. Some repeat, some
                // reverse, some are dropped for silence - the gaps are half of what makes a
                // pattern sound deliberate.
                const int slices = clampi((int)std::round((float)phraseLen / sliceSamples_), 1, 32);
                const int pick = jitter > 0.02f
                    ? (int)(patternFloat() * (float)slices)
                    : blockIndex_ % slices;
                const int slot = clampi(pick, 0, slices - 1);
                length = (int)clampf(sliceSamples_, 64.0f, (float)phraseLen);
                start = (double)ansStart_ + (double)slot * sliceSamples_;
                if (patternFloat() < motion * 0.6f)     // reverse this slice
                {
                    start += length;
                    rate = -baseRate;
                }
                if (patternFloat() < jitter * 0.25f) amp = 0.0f;   // a rest
                ++blockIndex_;
                break;
            }
            case kGlide:
            {
                length = grainLength(shape);
                start = (double)ansStart_ + (double)patternFloat() * (double)phraseLen * jitter;
                // One slide, shared by every grain, instead of a separate bend per grain. Giving
                // each grain its own random direction made them cancel out into a chorus, and a
                // short grain - which is most of them - barely moved before it ended, so nothing
                // actually sounded like it was sliding. Here the offset depends only on how far
                // through the pass we are, so consecutive grains carry the slide on from each
                // other and the whole answer moves as one. The grain's own rateInc keeps it going
                // while the grain sounds, so there is no stepping between them.
                //
                // Each pass slides from wherever the last one finished to a new target, so the
                // pitch is continuous across pass boundaries however far apart the targets are.
                // Picking the target per pass rather than deriving it from the pass number is
                // what lets Mixed go up as well as down: the old version was a fixed triangle
                // that always set off downwards and never rose above the note you played.
                if (glidePass_ != passIndex_)
                {
                    glidePass_ = passIndex_;
                    glideFrom_ = glideTo_;
                    glideTo_ = nextGlideTarget(motion);
                }
                const float t = (float)answerPos_ / (float)phraseLen;
                const float span = glideTo_ - glideFrom_;
                const float here = glideFrom_ + span * t;
                const double perSample = (double)span / (double)phraseLen;
                rate = baseRate * (double)semitonesToRatio(here);
                const double endRate = baseRate
                    * (double)semitonesToRatio(here + (float)(perSample * (double)length));
                rateInc = (endRate - rate) / (double)length;
                break;
            }
            case kBubbles:
            {
                // Short grains through their own resonant band-pass: each becomes a pitched blip.
                length = (int)clampf(grainLength(shape) * 0.35f, 0.006f * sr_, 0.12f * sr_);
                start = (double)ansStart_ + (double)patternFloat() * (double)phraseLen;

                // Sample and hold. Scatter is how many bubbles share one setting before a new
                // one is drawn: at 0 every bubble is its own, which is where this started, and
                // turned up they arrive in groups on one pitch. Holding the value is what turns
                // a shimmer into a pattern - the ear hears a repeated note rather than noise.
                // Counting bubbles rather than milliseconds keeps the steps in step with
                // Activity, so changing the density does not change the rhythm of the pattern.
                if (--shCount_ <= 0)
                {
                    const float lo = 180.0f, hi = 180.0f + motion * 5200.0f;
                    shHz_ = snapToScale(lo + patternFloat() * (hi - lo));
                    // Resonance is drawn with it, so Range widens the character as well as the
                    // pitch: some bubbles ping, others only blip. At Range 0 it stays on the
                    // one value this always used, so the engine's old sound is still in here.
                    shRes_ = clampf(0.84f + (patternFloat() - 0.5f) * motion * 0.28f,
                                    0.60f, 0.97f);
                    shCount_ = 1 + (int)(scatter * 11.0f + 0.5f);
                }
                g->band.set(clampf(shHz_, 60.0f, 0.45f * sr_), shRes_);
                g->band.reset();
                // The SVF's band-pass peaks at 1/k, so undo that and add a little back: a bubble
                // should sit with the other engines, not three times above them. This has to use
                // the resonance this bubble actually got, or a varied Q becomes a varied level.
                g->filtGain = (2.0f - 1.98f * shRes_) * 2.6f;
                filtered = true;
                break;
            }
            case kRoll:
            {
                // One short fragment, drilled. Scatter is how readily it jumps to a new one; the
                // grain fills the gap to the next so the roll is continuous rather than dotted.
                if (rollHold_ <= 0)
                {
                    rollPos_ = (double)ansStart_ + (double)patternFloat() * (double)phraseLen * 0.85;
                    rollHold_ = 2 + (int)(patternFloat() * 10.0f * (1.0f - jitter));
                }
                --rollHold_;
                start = rollPos_;
                length = clampi(gap + gap / 8, 48, (int)(0.5f * sr_));
                break;
            }
            case kEbb:
            {
                // The whole answer in reverse: the playhead walks from the end of the phrase back
                // to its start and every grain reads backwards, so a note swells into its attack
                // instead of decaying away from it. Motion lets the tide run out - the playback
                // slows by up to half across the pass.
                length = grainLength(shape);
                const float t = (float)answerPos_ / (float)(phraseLen > 0 ? phraseLen : 1);
                // The read position IS the far end of the grain, because the grain runs backwards
                // from it. Adding the grain length on top pushed the first reads past the end of
                // the captured phrase, into buffer that had not been written yet - silence. The
                // walk also stops a grain's length short of the start, so it never reads behind
                // the phrase either.
                const double span = (double)phraseLen - (double)length;
                const double head = (double)ansEnd_ - (span > 0.0 ? span : 0.0) * (double)t;
                start = head + (double)(patternFloat() - 0.5f) * (double)phraseLen * jitter * 0.4;
                if (start > (double)ansEnd_) start = (double)ansEnd_;
                rate = -baseRate * (double)(1.0f - motion * 0.5f * t);
                break;
            }
            case kChord:
            {
                // One slice of the phrase, played as a voicing. Scatter is how far the grain
                // wanders; the chord itself is built in spawnChordVoices below.
                //
                // Change moves the chord every bar or two, and when it moves it also moves to a
                // different part of your phrase - so a new chord is a new sound as well as a new
                // harmony, which is what stops it sitting still.
                length = grainLength(shape);
                const double centre = (double)ansStart_ + (double)chordSlice_ * (double)phraseLen * 0.7;
                start = centre + (double)patternFloat() * (double)phraseLen * jitter * 0.3;
                rate = baseRate * chordDegreeRatio(chordStep(), 0);
                break;
            }
            default:   // Cloud
            {
                length = grainLength(shape);
                start = (double)ansStart_
                      + (double)phraseLen * (double)(answerPos_ / (float)(phraseLen > 0 ? phraseLen : 1))
                      + (double)(patternFloat() - 0.5f) * (double)phraseLen * jitter;
                break;
            }
        }

        if (length < 32) length = 32;
        // Keep the read inside the part of the buffer that still holds the phrase.
        const double limit = (double)(writePos_ - (int64_t)bufMask_ + 64);
        if (start < limit) start = limit;

        const float pan = (patternFloat() - 0.5f) * 2.0f * spread;
        float gl = 0.0f, gr = 0.0f;
        equalPowerPan(clampf(pan, -1.0f, 1.0f), gl, gr);

        g->active = true;
        g->pos = start;
        g->rate = rate;
        g->rateInc = rateInc;
        g->age = 0;
        g->length = length;
        g->gainL = gl;
        g->gainR = gr;
        g->amp = amp;
        g->filtered = filtered;
        g->wait = 0;
        if (!filtered) g->filtGain = 1.0f;
        if (mode == kChord) spawnChordVoices(*g, motion, scatter, spread);
    }

    Grain* takeGrain()
    {
        int oldest = -1;
        for (int i = 0; i < kMaxGrains; ++i)
        {
            if (!grain_[i].active) return &grain_[i];
            if (oldest < 0 || grain_[i].age > grain_[oldest].age) oldest = i;
        }
        return &grain_[oldest];   // steal the one furthest through its life
    }

    // The ratio for the v-th note of a voicing above whatever the answer is already playing.
    // With a scale and a detected pitch these are real diatonic thirds - the chord the key has on
    // that degree, major or minor as it falls. Without either, a plain stacked major seventh, so
    // the engine still does something musical on a drum loop.
    // Where the chord sits right now, as a number of scale degrees above the note you played.
    int chordStep() const { return kChordWalk[chordIndex_ & 3]; }

    // Move the played note by `steps` degrees of the scale, plus whole octaves. Two steps is a
    // third, so a chord is steps 0, 2, 4, 6 from wherever the progression has put its root.
    double chordDegreeRatio(int steps, int octave) const
    {
        const int si = clampi(getInt(kScale), 0, kNumScales - 1);
        if (si == 0 || heardMidi_ < 0.0f)
        {
            // No key to work in, so assume a major scale. Stacking steps 0/2/4/6 on that gives
            // the major seventh this used to build unconditionally.
            const int oct = (int)std::floor((float)steps / 7.0f);
            const int within = steps - oct * 7;
            return (double)semitonesToRatio((float)(kMajorSteps[clampi(within, 0, 6)]
                                                    + 12 * oct + octave));
        }
        const ScaleDef& sc = kScales[si];
        const int root = scaleRoot();
        const float rel = heardMidi_ - (float)root;
        const float oct = std::floor(rel / 12.0f);
        const int srcDeg = degreeOf(sc, rel - oct * 12.0f);
        const int deg = srcDeg + steps;
        const int shift = (int)std::floor((float)deg / (float)sc.count);
        const int within = deg - shift * sc.count;
        const float target = (float)root + (oct + (float)shift) * 12.0f + (float)sc.notes[within];
        const float source = (float)root + oct * 12.0f + (float)sc.notes[srcDeg];
        return (double)semitonesToRatio(target - source + (float)octave);
    }

    double chordVoiceRatio(int v, float motion) const
    {
        const int octave = (motion > 0.66f && v >= 2) ? 12 : 0;   // wide voicings open upward
        return chordDegreeRatio(chordStep() + 2 * v, octave);
    }

    // Chord spawns the rest of its voicing alongside the grain just made: same slice of the
    // phrase, different transpositions, optionally spread in time so the chord is strummed.
    void spawnChordVoices(const Grain& root, float motion, float scatter, float spread)
    {
        const int voices = clampi(2 + (int)std::lround(motion * 2.0f), 2, 4);
        const int strum = (int)(scatter * 0.12f * sr_);
        for (int v = 1; v < voices; ++v)
        {
            Grain* g = takeGrain();
            *g = root;
            g->rate = root.rate * chordVoiceRatio(v, motion);
            g->rateInc = 0.0;
            g->wait = strum * v;
            g->age = 0;
            const float pan = ((float)v / (float)(voices - 1) - 0.5f) * 2.0f * spread;
            equalPowerPan(clampf(pan, -1.0f, 1.0f), g->gainL, g->gainR);
            g->active = true;
        }
    }

    // Where the pass that is just starting should end up, in semitones from the note you played.
    float nextGlideTarget(float motion)
    {
        if (motion <= 0.0f) return 0.0f;
        const int dir = clampi(getInt(kGlideDir), 0, 2);
        if (dir != kGlideMixed)
        {
            // Down and Up are a tide: out to the full depth, then back to where you played.
            const float depth = motion * 12.0f * (dir == kGlideUp ? 1.0f : -1.0f);
            return (passIndex_ & 1) ? 0.0f : depth;
        }
        // Mixed: a different interval each pass, up or down, and now and then back to the note
        // itself so it keeps coming home. Motion is how far down the list of intervals it may
        // reach, so turning it up lets the fifths and octaves in rather than making one bend
        // wider - which is what you want from a slide, where the interval is the whole point.
        const int reach = clampi((int)(motion * 4.0f), 0, 3);
        // Landing on the target it is already sitting at would be a pass with no slide in it at
        // all, so keep rolling until it is somewhere else.
        for (int tries = 0; tries < 4; ++tries)
        {
            const int pick = (int)(patternFloat() * (float)(reach + 2));
            const float semis = (float)kGlideTargets[clampi(pick, 0, 3)];
            const float target = pick > reach ? 0.0f          // the spare slot is "home"
                                              : (patternFloat() < 0.5f ? -semis : semis);
            if (target != glideFrom_) return target;
        }
        return glideFrom_ == 0.0f ? (float)kGlideTargets[reach] : 0.0f;
    }

    int grainLength(float shape) const
    {
        // Shape runs from short and percussive to long and smeared, capped by Time.
        const float ms = 18.0f + shape * shape * 480.0f;
        return (int)clampf(ms * 0.001f * sr_, 0.004f * sr_, std::fmin(sliceSamples_, 1.2f * sr_));
    }

    // ----- meters ----------------------------------------------------------------------------
    void sendMeters(int frames)
    {
        meterCountdown_ -= frames;
        if (meterCountdown_ > 0) return;
        meterCountdown_ = (int)(sr_ / 25.0f);    // at most 25 updates a second

        const float lvl = std::floor(clampf(meter_ * 1.6f, 0.0f, 1.0f) * 15.0f) / 15.0f;
        if (lvl != lastMeterSent_)
        {
            lastMeterSent_ = lvl;
            setNormalised(kLevel, lvl);
            notifyHostParameterChanged(kLevel);
        }
        const float hv = (float)(heardClass_ >= 0 ? heardClass_ + 1 : 0);
        if (hv != lastHeardSent_)
        {
            lastHeardSent_ = hv;
            set(kHeard, hv);
            notifyHostParameterChanged(kHeard);
        }
        const float st = (float)(answering_ ? 3 : (pending_ ? 2 : (listening_ ? 1 : 0)));
        if (st != lastStateSent_)
        {
            lastStateSent_ = st;
            set(kState, st);
            notifyHostParameterChanged(kState);
        }
    }

    // ----- programs ----------------------------------------------------------------------------
    void addPrograms()
    {
        // Out Mix Mode Act Shp Sct Mot Sync Time Rep Tone Spc Dec Lsn Gap Ans Rep Int Scl Rt Trd Spr Hld In
        auto prog = [this](const char* name, std::vector<float> v)
        {
            // Anything a program does not list keeps the parameter's own default, so appending a
            // parameter later does not silently set every program's copy of it to zero.
            const size_t given = v.size();
            v.resize((size_t)kNumParams);
            for (size_t i = given; i < v.size(); ++i) v[i] = param((int)i).def;
            addProgram(name, std::move(v));
        };
        const float T8 = timeFor(kDivisionEighth), T16 = timeFor(kDivisionSixteenth),
                    T4 = timeFor(kDivisionQuarter);

        prog("Call and Answer", { 0, 50, kCloud,   50, 45, 35, 30, 1, T8,  40, 9000,  30, 3.0f,
                                  kListenAuto, 220, kAnswerBeat, 35,  0, 0, 0, 50, 70, 0, 0 });
        prog("Chopped Reply",   { 0, 55, kBlocks,  60, 30, 55, 45, 1, T16, 45, 8000,  22, 2.4f,
                                  kListenAuto, 180, kAnswerBeat, 50,  0, 0, 0, 55, 80, 0, 0 });
        prog("Bending Back",    { 0, 50, kGlide,   45, 55, 40, 85, 1, T8,  45, 7000,  35, 3.5f,
                                  kListenAuto, 260, kAnswerBeat, 45,  0, 0, 0, 50, 75, 0, 0 });
        prog("Bubble Bath",     { 0, 55, kBubbles, 70, 30, 50, 45, 1, T16, 40, 11000, 40, 4.0f,
                                  kListenAuto, 200, kAnswerBeat, 35,  0, 3, 0, 50, 85, 0, 0 });
        prog("In Key",          { 0, 55, kBubbles, 75, 25, 60, 55, 1, T16, 50, 12000, 45, 5.0f,
                                  kListenAuto, 220, kAnswerBar,  40,  0, 4, 9, 55, 90, 0, 0 });
        prog("Octave Answer",   { 0, 50, kCloud,   45, 50, 30, 20, 1, T4,  50, 8000,  35, 4.0f,
                                  kListenAuto, 300, kAnswerBar,  30, 12, 0, 0, 55, 70, 0, 0 });
        prog("Texture (no reply)", { 0, 40, kCloud, 60, 70, 50, 25, 1, T8, 55, 7000, 45, 5.0f,
                                  kListenOff,  220, kAnswerNow,  25,  0, 0, 0,  0, 85, 0, 0 });
        prog("Freeze and Play", { 0, 60, kBubbles, 65, 35, 45, 50, 1, T16, 60, 10000, 50, 6.0f,
                                  kListenHold, 220, kAnswerNow,  30,  0, 2, 0,  0, 90, 1, 0 });
        // Chord wants a scale to be worth anything, so this one arrives with the key already set.
        prog("One Finger Chords", { 0, 55, kChord, 40, 55, 25, 50, 1, T4,  45, 8000, 35, 4.0f,
                                  kListenAuto, 260, kAnswerBar,  30,  0, 1, 0, 55, 70, 0, 0 });
        prog("Tightening Roll", { 0, 55, kRoll, 50, 30, 30, 70, 1, T8,  35, 9000, 25, 2.0f,
                                  kListenAuto, 180, kAnswerBeat, 45,  0, 0, 0, 60, 60, 0, 0 });
        prog("Ebb Tide",        { 0, 55, kEbb,  45, 60, 35, 45, 1, T4,  50, 6000, 55, 6.0f,
                                  kListenAuto, 300, kAnswerBar,  35,  0, 0, 0, 50, 80, 0, 0 });
    }

    float timeFor(int division) const
    {
        // The plain value whose normalised position selects this division.
        return param(kTime).toPlain((float)division / (float)(kNumDivisions - 1));
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Tidepools";
        info.vendor       = "Groundswell";
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);
        info.version      = 1000;
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    static constexpr char kUid[5] = "GjgQ";

    // capture
    std::vector<float> buf_[2];
    int       bufMask_ = 0;
    int64_t   writePos_ = 0;

    // pitch tracking
    std::vector<float> decBuf_;        // mono, decimated by kDecim
    int        decMask_ = 0;
    OnePoleLP  decLp_, decLp2_;
    bool       analyse_ = false;
    float      heardMidi_ = -1.0f;     // the phrase's pitch, or -1 when it had none
    int        heardClass_ = -1;       // ...as a pitch class 0..11
    float      lastHeardSent_ = -1.0f;
    float      yin_[kYinMaxLag + 1] = {};

    // listening
    float   env_ = 0.0f, envAttack_ = 0.1f, envRelease_ = 0.01f;
    float   envFast_ = 0.0f, envFastRelease_ = 0.05f;   // marks where a phrase really stops
    float   phrasePeak_ = 0.0f;   // loudest the phrase in progress has been, for the relative shut
    float   armLevel_ = 0.0f;     // after a phrase ends, what the input must beat to start another
    float   armFall_ = 0.0f;
    bool    listening_ = false;
    int     gapCount_ = 0;
    int64_t phraseStart_ = 0, phraseEnd_ = 0;    // what the detector is marking up
    int64_t ansStart_ = 0, ansEnd_ = 0;          // what the answer currently playing is reading

    // answering
    bool    answering_ = false, pending_ = false;
    int     pendingCount_ = 0;
    int64_t answerPos_ = 0;
    float   answerGain_ = 1.0f;
    // The gain the audio actually gets. answerGain_ is the logical one, and it steps: down a
    // notch on every repeat pass, and straight back to 1 whenever an answer restarts. Applying
    // those steps to grains that are already sounding is a click, so the audio follows this.
    float   answerGainSm_ = 0.0f, answerGainRate_ = 1.0f;
    int     passIndex_ = 0, blockIndex_ = 0;
    int     spawnCount_ = 0;
    double  rollPos_ = 0.0;        // Roll: the fragment being drilled
    int     rollHold_ = 0;         // ...and how many more hits before it moves
    float   shHz_ = 440.0f;        // Bubbles: the held sample-and-hold frequency...
    float   shRes_ = 0.84f;        // ...its resonance...
    int     shCount_ = 0;          // ...and how many more bubbles it has to last
    int     glidePass_ = -1;       // Glide: which pass the targets below were picked for
    float   glideFrom_ = 0.0f;     // Glide: semitones at the start of this pass...
    float   glideTo_ = 0.0f;       // ...and where it is heading by the end of it
    int     chordIndex_ = 0;       // Chord: where it is in the progression
    int64_t chordCount_ = 0;       // ...samples until it moves on
    int64_t chordSpan_ = 44100;    // ...and how many that is, from Change and the tempo
    float   chordSlice_ = 0.0f;    // ...which part of the phrase this chord is reading
    uint32_t patternSeed_ = 1u;
    float   sliceSamples_ = 11025.0f;

    // voices and output
    Grain          grain_[kMaxGrains];
    std::vector<float> window_;
    SVF            tone_[2];
    Fdn            fdn_[2];
    Smoother       mix_, output_, input_, wetTrim_;
    float          duck_ = 1.0f;
    float          limGain_ = 1.0f, limAttack_ = 0.1f, limRelease_ = 0.001f;
    Xorshift32     rng_;

    // meters
    float meter_ = 0.0f, meterFall_ = 0.01f;
    float lastMeterSent_ = -1.0f, lastStateSent_ = -1.0f;
    int   meterCountdown_ = 0;

    float sr_ = 44100.0f;
};

MPC_PLUGIN_ENTRY(Tidepools)
