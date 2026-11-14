// Tidepool - a granular call-and-response effect for MPC standalone hardware.
//
// SPDX-License-Identifier: MIT
// (c) Groundswell
//
// ============================================================================================
// WHAT IT IS
// ============================================================================================
//
// Tidepool listens to what you play, waits until you stop, and answers with it.
//
// That is the whole idea, and it is why the plugin is built the way it is. A granular pedal
// gets a call-and-response feel somewhat by accident: it granulates a rolling window of the
// last few seconds, and if you happen to stop playing you hear that window on its own. Here it
// is done on purpose. An envelope follower watches the input and marks where a phrase starts.
// When you stop, and the silence lasts longer than Gap, Tidepool decides you have finished,
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
    kNumParams
};

enum Mode   { kCloud = 0, kBlocks, kGlide, kBubbles };
enum Listen { kListenOff = 0, kListenAuto, kListenHold };
enum Answer { kAnswerNow = 0, kAnswerBeat, kAnswerBar };

constexpr float kCaptureSeconds = 8.0f;    // how far back the buffer remembers
constexpr int   kMaxGrains = 16;
constexpr int   kWindowBits = 10;
constexpr int   kWindowSize = 1 << kWindowBits;    // raised-cosine grain envelope
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

class Tidepool : public mpc::Plugin
{
public:
    Tidepool() : mpc::Plugin(makeInfo())
    {
        // Parameter 0 is nudged by the host on every load, so it has to be harmless.
        addParam(mpc::floatParam ("out",      "Output",   -30.0f, 6.0f,   0.0f,  "dB", 1));
        addParam(mpc::floatParam ("mix",      "Mix",       0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::choiceParam("mode",     "Mode",     { "Cloud", "Blocks", "Glide", "Bubbles" }, kCloud));
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
        meterFall_  = 1.0f - std::exp(-1.0f / (0.150f * sr_));

        rng_.reseed(0x51A3C7D9u);
        reset();
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill(buf_[c].begin(), buf_[c].end(), 0.0f);
            tone_[c].reset();
            fdn_[c].reset();
        }
        for (Grain& g : grain_) { g.active = false; g.band.reset(); }
        writePos_ = 0;
        env_ = 0.0f;
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

        const int   mode     = clampi(getInt(kMode), 0, 3);
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

        const float gapSamples = get(kGap) * 0.001f * sr_;
        const float toneHz = clampf(get(kTone), 100.0f, 0.45f * sr_);
        for (int c = 0; c < 2; ++c) tone_[c].set(toneHz, 0.1f);

        const float space = get(kSpace) * 0.01f;
        const float decay = clampf(get(kDecay), 0.3f, 12.0f);
        // Per-pass gain that gives the asked-for RT60 on the shortest line.
        const float fdnFeedback = clampf(std::pow(0.001f, (float)kFdnLengths[0] / (decay * 44100.0f)),
                                         0.0f, 0.999f);
        for (int c = 0; c < 2; ++c) fdn_[c].setDamping(clampf(toneHz, 500.0f, 12000.0f));

        const float interval = get(kInterval);
        const double baseRate = (double)semitonesToRatio(interval);

        // The envelope thresholds that decide "you are playing" and "you have stopped".
        constexpr float kOpen = 0.004f;    // about -48 dBFS
        constexpr float kShut = 0.0016f;   // about -56 dBFS, hysteresis so a decay does not flicker

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

            // Capture. The buffer always rolls unless something has frozen it, so there is always
            // a phrase to answer with and Trade can hear you come back in.
            const bool frozen = (listen == kListenHold && hold) || (answering_ && hold);
            if (!frozen)
            {
                const int i = (int)(writePos_ & (int64_t)bufMask_);
                buf_[0][(size_t)i] = capL;
                buf_[1][(size_t)i] = capR;
                ++writePos_;
            }

            // ---- phrase detection --------------------------------------------------------
            if (listen == kListenAuto)
            {
                if (!listening_ && env_ > kOpen)
                {
                    // A phrase begins. Back-date the start a little so the attack is not clipped.
                    listening_ = true;
                    phraseStart_ = writePos_ - (int64_t)(0.01f * sr_);
                    gapCount_ = 0;
                }
                else if (listening_)
                {
                    if (env_ > kShut)
                        gapCount_ = 0;
                    else if (++gapCount_ > (int)gapSamples)
                    {
                        // Long enough a silence: that was the phrase.
                        listening_ = false;
                        phraseEnd_ = writePos_ - gapCount_;
                        int64_t len = phraseEnd_ - phraseStart_;
                        if (len > maxPhrase) phraseStart_ = phraseEnd_ - maxPhrase;
                        if (len >= minPhrase) scheduleAnswer(answerAt, ctx, beatSamples, n);
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
                    spawnGrain(mode, shape, scatter, motion, spread, baseRate, phraseLen);
                    spawnCount_ = nextSpawnInterval(mode, activity);
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
            wetL *= trim * answerGain_;
            wetR *= trim * answerGain_;

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
        patternSeed_ = rng_.next();
    }

    int nextSpawnInterval(int mode, float activity) const
    {
        if (mode == kBlocks) return (int)clampf(sliceSamples_, 64.0f, 4.0f * sr_);
        // 4 grains a second at the bottom of Activity, about 90 at the top.
        const float density = 4.0f + activity * activity * 86.0f;
        return (int)clampf(sr_ / density, 16.0f, sr_);
    }

    // Snap a frequency to the chosen scale, so Bubbles sparkles in key instead of at random.
    float snapToScale(float hz) const
    {
        const int scaleIndex = clampi(getInt(kScale), 0, kNumScales - 1);
        if (scaleIndex == 0) return hz;
        const ScaleDef& sc = kScales[scaleIndex];
        const int root = clampi(getInt(kRoot), 0, 11);
        const float midi = hzToMidi(clampf(hz, 20.0f, 18000.0f));
        const float rel = midi - (float)root;
        const float octave = std::floor(rel / 12.0f);
        const float within = rel - octave * 12.0f;
        float best = (float)sc.notes[0];
        float bestD = 99.0f;
        for (int i = 0; i < sc.count; ++i)
        {
            const float d = std::fabs(within - (float)sc.notes[i]);
            if (d < bestD) { bestD = d; best = (float)sc.notes[i]; }
        }
        return midiToHz((float)root + octave * 12.0f + best);
    }

    uint32_t patternRand()
    {
        patternSeed_ = patternSeed_ * 1664525u + 1013904223u;
        return patternSeed_;
    }
    float patternFloat() { return (float)(patternRand() >> 8) * (1.0f / 16777216.0f); }

    // ----- the engines ---------------------------------------------------------------------
    void spawnGrain(int mode, float shape, float scatter, float motion,
                    float spread, double baseRate, int64_t phraseLen)
    {
        Grain* g = nullptr;
        int oldest = -1;
        for (int i = 0; i < kMaxGrains; ++i)
        {
            if (!grain_[i].active) { g = &grain_[i]; break; }
            if (oldest < 0 || grain_[i].age > grain_[oldest].age) oldest = i;
        }
        if (g == nullptr) g = &grain_[oldest];   // steal the one furthest through its life

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
                // Bend across the grain: up to an octave either way, direction from the dice.
                const float semis = motion * 12.0f * (patternFloat() < 0.5f ? -1.0f : 1.0f);
                const double endRate = baseRate * (double)semitonesToRatio(semis);
                rate = baseRate;
                rateInc = (endRate - baseRate) / (double)length;
                break;
            }
            case kBubbles:
            {
                // Short grains through their own resonant band-pass: each becomes a pitched blip.
                length = (int)clampf(grainLength(shape) * 0.35f, 0.006f * sr_, 0.12f * sr_);
                start = (double)ansStart_ + (double)patternFloat() * (double)phraseLen;
                const float lo = 180.0f, hi = 180.0f + motion * 5200.0f;
                const float hz = snapToScale(lo + patternFloat() * (hi - lo));
                constexpr float kRes = 0.84f;
                g->band.set(clampf(hz, 60.0f, 0.45f * sr_), kRes);
                g->band.reset();
                // The SVF's band-pass peaks at 1/k, so undo that and add a little back: a bubble
                // should sit with the other engines, not three times above them.
                g->filtGain = (2.0f - 1.98f * kRes) * 2.6f;
                filtered = true;
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
        if (!filtered) g->filtGain = 1.0f;
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
            v.resize((size_t)kNumParams, 0.0f);
            addProgram(name, std::move(v));
        };
        const float T8 = timeFor(kDivisionEighth), T16 = timeFor(kDivisionSixteenth),
                    T4 = timeFor(kDivisionQuarter);

        prog("Call and Answer", { 0, 50, kCloud,   50, 45, 35, 30, 1, T8,  40, 9000,  30, 3.0f,
                                  kListenAuto, 220, kAnswerBeat, 35,  0, 0, 0, 50, 70, 0, 0 });
        prog("Chopped Reply",   { 0, 55, kBlocks,  60, 30, 55, 45, 1, T16, 45, 8000,  22, 2.4f,
                                  kListenAuto, 180, kAnswerBeat, 50,  0, 0, 0, 55, 80, 0, 0 });
        prog("Bending Back",    { 0, 50, kGlide,   45, 55, 40, 55, 1, T8,  45, 7000,  35, 3.5f,
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
    }

    float timeFor(int division) const
    {
        // The plain value whose normalised position selects this division.
        return param(kTime).toPlain((float)division / (float)(kNumDivisions - 1));
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Tidepool";
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

    // listening
    float   env_ = 0.0f, envAttack_ = 0.1f, envRelease_ = 0.01f;
    bool    listening_ = false;
    int     gapCount_ = 0;
    int64_t phraseStart_ = 0, phraseEnd_ = 0;    // what the detector is marking up
    int64_t ansStart_ = 0, ansEnd_ = 0;          // what the answer currently playing is reading

    // answering
    bool    answering_ = false, pending_ = false;
    int     pendingCount_ = 0;
    int64_t answerPos_ = 0;
    float   answerGain_ = 1.0f;
    int     passIndex_ = 0, blockIndex_ = 0;
    int     spawnCount_ = 0;
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

MPC_PLUGIN_ENTRY(Tidepool)
