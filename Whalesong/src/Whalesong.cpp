// Whalesong - an MPC insert effect by Groundswell.
//
// What it is: an accompanist. It listens to whatever is on the track, works out what key you are
// playing in, and then plays along on its own synth. Nothing of the input reaches the output
// except the dry signal passing through: every note you hear from it is synthesised here.
//
// Signal flow
// -----------
//                 +--> dry ------------------------------------------------+
//   input ---+----|                                                        |
//            |    +--> mono sum --> envelope follower --> "are they playing?"
//            |                  \
//            |                   +-> decimate x4 --> YIN every 46 ms --> pitch
//            |                                                  |
//            |                              pitch-class histogram (decaying)
//            |                                                  |
//            |                                      key + scale + confidence
//            |                                                  |
//            |                                   conductor: when to play, which degree
//            |                                                  |
//            +---------------------------------> voices (2 osc + SVF + ADSR) --> FDN --+
//                                                                                      |
//                                                              out = dry + wet * Mix <-+
//
// How the key is found
// --------------------
// YIN gives one pitch estimate roughly every 46 ms. Each confident estimate drops a vote into one
// of twelve pitch-class bins, weighted by how loud the input was at the time. The histogram decays
// with a time constant of about eight bars, so the plugin follows a modulation instead of being
// stuck with the first thing it heard.
//
// The key itself is read off that histogram by correlation: for every root (12) and every scale
// shape (6) the notes in the scale are added up and the notes outside it are subtracted. The tonic
// and the fifth count for more than the rest, which is what separates C major from A minor - they
// contain the same seven notes and differ only in which one the music leans on.
//
// Confidence is the gap between the best root and the best *different* root, as a fraction. A
// single held note scores every key containing it equally and so reports low confidence; a few
// bars of melody separates them quickly.
//
// Realtime rules observed here: nothing is allocated, locked or logged in process(). The buffers
// are sized in prepare(). The YIN pass is the one expensive thing and it runs on a hop counter,
// not every sample, so its cost lands in one block out of every ~40.
//
// CPU: about 4-6% of one MPC core with four voices, most of it the oscillators. The pitch
// detector adds roughly 1%.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/adsr.h"
#include "dsp/svf.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"
#include "dsp/tempo.h"

#include <cmath>
#include <cstdint>
#include <vector>
#include "InputSegmenter.h"

using namespace mpc::dsp;

namespace {

// ------------------------------------------------------------------------------------------
// Parameters. The order is permanent once anyone has a project: append, never insert.
// ------------------------------------------------------------------------------------------
enum ParamIndex
{
    kOutput = 0,   // master volume. The host nudges parameter 0 on every load: keep it harmless.
    kMix,          // how loud the accompaniment sits against the dry input
    kRole,         // what kind of line it plays
    kActivity,     // how often it plays

    kMotion,       // how far the line is allowed to range
    kOctave,       // transpose the whole line
    kSync,         // free running, or locked to the beat or the bar
    kRate,         // the note division it plays on when synced

    kHuman,        // timing and velocity jitter
    kGlide,        // portamento between notes
    kShape,        // oscillator waveform morph
    kTone,         // filter cutoff

    kReso,         // filter resonance
    kAttack,       // envelope attack
    kRelease,      // envelope release
    kDrive,        // saturation into the filter

    kSpread,       // stereo width and oscillator detune
    kSpace,        // reverb send
    kVoices,       // how many notes may sound at once
    kListen,       // Off (use the Key/Scale knobs), Follow, or Lock once sure

    kSens,         // how readily it decides the input is playing
    kWait,         // how long it listens before it starts playing
    kKey,          // the root, used when Listen is Off
    kScale,        // the scale shape, used when Listen is Off

    kLevel,        // meter: input level
    kHeard,        // meter: the pitch class it is hearing right now
    kFound,        // meter: the key it has settled on
    kConf,         // meter: how sure it is
    kFoundScale,   // meter: the scale shape it has settled on. Appended after the first four.

    // The effect section, appended. Every one of these defaults to zero depth, so a project or
    // program saved before they existed sounds exactly as it did.
    kDelayMix,     // how much tempo-synced delay
    kDelayTime,    // the delay's note division
    kDelayFb,      // delay feedback
    kChorus,       // chorus depth (its rate is fixed slow, the way a chorus wants to be)
    kVibrato,      // pitch wobble, in cents
    kVibRate,      // ...and how fast
    kTremolo,      // level wobble
    kTremRate,     // ...and how fast

    // The phrase window, appended. Off keeps the old behaviour exactly.
    kWindow,       // how long a phrase is, in bars
    kVary,         // how much each repeat differs from the last
    kRenew,        // how many repeats before it writes a fresh phrase

    // Winding down, appended. Never is the default, so a free-running instance - and every
    // project saved before this existed - behaves exactly as it did.
    kLinger,       // how long it keeps playing after the input goes quiet
    kNumParams
};

enum Role   { kBass = 0, kLead, kCounter, kPad, kArp };
enum Sync   { kFree = 0, kOnBeat, kOnBar };
enum Listen { kListenOff = 0, kListenFollow, kListenLock };

// ------------------------------------------------------------------------------------------
// Pitch detection. Lifted from Tidepools, which has been through a round of hardware testing,
// but run on a hop here instead of once per phrase: the key needs a stream of estimates, not one.
// Decimating by four first is what makes that affordable - pitch lives below 1.5 kHz, so 11 kHz
// is plenty and the search shrinks sixteenfold.
// ------------------------------------------------------------------------------------------
constexpr int   kDecim = 4;
constexpr int   kYinWindow = 512;          // 46 ms at 11 kHz: three periods of the lowest note
constexpr int   kYinMinLag = 9;            // about 1225 Hz
constexpr int   kYinMaxLag = 140;          // about 79 Hz
constexpr float kYinGood = 0.22f;          // below this the estimate is trusted
constexpr float kYinUsable = 0.45f;        // above this the window is called unpitched
// The detector runs every kHopSamples. 23 ms, not the 46 ms it started at: the note segmenter
// cannot resolve notes much shorter than its hop, and at 46 ms a sixteenth at 120 bpm (125 ms)
// came back as roughly half the notes actually played. At 23 ms it comes back whole. The cost
// is one more YIN pass per 23 ms, which is the plugin's one expensive operation.
//
// The KEY estimate deliberately does not speed up with it. It was tuned and tested on hardware
// at one vote per 46 ms, so it still votes every kVoteEvery hops - the same cadence, the same
// decay per vote, the same time to lock. Only the pitch stream feeding the segmenter is faster.
constexpr int   kHopSamples = 1024;        // 23 ms at 44.1 kHz
constexpr int   kVoteEvery = 2;            // hops per key-histogram vote: back to 46 ms
constexpr int   kVoteHop = kHopSamples * kVoteEvery;
constexpr int   kYinSourceWindow = kYinWindow * kDecim;   // how far back a reading reaches
constexpr float kDecSeconds = 1.0f;        // how much decimated history to keep

// Key inference.
constexpr float kHistHalfLifeBeats = 32.0f;   // eight bars of 4/4
constexpr float kHistMinWeight = 2.5f;        // below this there is not enough evidence to call it
constexpr float kLockConfidence = 0.60f;      // Lock freezes the key once it is this sure...
// ...and has been that sure of the same root, without a break, for this long. Neither test is
// enough alone. After a single bar the detector is often confident AND wrong - a C major tune
// that leans on its fifth reads as G, and reads it firmly - and Lock would freeze that for good.
// Confidence on ambiguous material also spikes and collapses from one estimate to the next, so a
// single reading over the line means little. Requiring an unbroken run costs nothing: Lock is
// for settling down, not for speed.
constexpr int   kLockStableHops = 64;         // 64 hops x 46 ms ~ 3 s

// Effects. The delay line is sized for a bar at a slow tempo; anything longer stops being a
// delay and starts being a looper.
constexpr float kMaxDelaySeconds = 4.0f;
constexpr float kChorusBaseMs = 9.0f;      // the delay a chorus sits at before it is modulated
constexpr float kChorusSwingMs = 6.0f;     // ...and how far it swings
constexpr float kChorusRateHz = 0.45f;     // slow, the way a chorus wants to be

// The phrase window. Eight bars of sixteenths, with room for chords on each.
constexpr int kMaxPhraseNotes = 256;

// One note the plugin played, kept so the phrase can be played again. The pitch is stored as a
// scale degree rather than a MIDI note, so a repeat follows the key: if the detector changes its
// mind halfway through a take, the phrase transposes to the new key instead of fighting it.
struct PhraseNote
{
    int64_t offset = 0;      // samples from the start of the window
    int     degree = 0;
    int     octave = 4;
    int64_t hold = 0;
    float   velocity = 1.0f;
};

// Voices.
constexpr int kMaxVoices = 6;
constexpr int kMaxChannels = 2;

// Scales, as semitones from the root. "Off" is not offered here: an accompanist needs a scale.
constexpr int kMaxScaleNotes = 7;
struct ScaleDef { const char* name; int count; int notes[kMaxScaleNotes]; };
const ScaleDef kScales[] = {
    { "Major",      7, { 0, 2, 4, 5, 7, 9, 11 } },
    { "Minor",      7, { 0, 2, 3, 5, 7, 8, 10 } },
    { "Dorian",     7, { 0, 2, 3, 5, 7, 9, 10 } },
    { "Mixolydian", 7, { 0, 2, 4, 5, 7, 9, 10 } },
    { "Penta Maj",  5, { 0, 2, 4, 7, 9 } },
    { "Penta Min",  5, { 0, 3, 5, 7, 10 } },
};
constexpr int kNumScales = (int)(sizeof(kScales) / sizeof(kScales[0]));

// A prior over the shapes. Dorian and Mixolydian contain exactly the notes of some major scale,
// so no amount of counting can tell them apart from it - the histogram is identical. Only which
// note the music leans on differs, and the tonic weighting below is a weak signal when the music
// leans on its fifth, as music does. Without this, a C major tune that stresses G reads as G
// Mixolydian: true as far as the notes go, and the wrong answer. Major and minor get the benefit
// of the doubt; a mode has to earn it. The pentatonics need no prior, being subsets: they only
// win when the melody really does avoid the notes they leave out.
constexpr float kScalePrior[kNumScales] = {
    0.085f,   // Major
    0.070f,   // Minor
    0.0f,     // Dorian
    0.0f,     // Mixolydian
    0.015f,   // Penta Maj
    0.010f,   // Penta Min
};

const char* const kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

// How much each scale degree counts when scoring a key. The tonic and the fifth carry the key:
// without this weighting C major and A minor score identically, since they hold the same notes.
inline float degreeWeight(int semisFromRoot)
{
    switch (semisFromRoot)
    {
        case 0:  return 2.4f;    // tonic
        case 7:  return 1.6f;    // fifth
        case 3:
        case 4:  return 1.25f;   // the third decides major or minor
        default: return 1.0f;
    }
}

// A reverb line's length in samples at 44.1 kHz, mutually prime-ish so the tail does not ring.
constexpr int kFdnLines = 4;
constexpr int kFdnLengths[kFdnLines] = { 1931, 2749, 3671, 4493 };

// ------------------------------------------------------------------------------------------
// One synth voice: two detuned oscillators into a state-variable lowpass, shaped by an ADSR.
// ------------------------------------------------------------------------------------------
struct Voice
{
    PolyBlepOsc oscA, oscB;
    SVF         filter;
    ADSR        env;

    bool   active = false;
    float  targetMidi = 60.0f;    // where the glide is heading
    float  currentMidi = 60.0f;   // where it is now
    float  glideRate = 1.0f;      // per-sample coefficient toward targetMidi
    float  velocity = 1.0f;
    float  panL = 0.7071f, panR = 0.7071f;
    int64_t startedAt = 0;        // for voice stealing
    int64_t releaseAt = 0;        // sample position at which to release; -1 = hold

    void prepare(double sr)
    {
        oscA.setSampleRate(sr);
        oscB.setSampleRate(sr);
        filter.setSampleRate(sr);
        env.setSampleRate(sr);
        reset();
    }

    void reset()
    {
        active = false;
        env.reset();
        filter.reset();
        oscA.resetPhase(0.0f);
        oscB.resetPhase(0.5f);
        releaseAt = -1;
    }
};

// ------------------------------------------------------------------------------------------
// A small feedback delay network for Space. Four lines, Hadamard mixed, one-pole damped.
// ------------------------------------------------------------------------------------------
struct Fdn
{
    DelayLine line[kFdnLines];
    int       len[kFdnLines] = { kFdnLengths[0], kFdnLengths[1], kFdnLengths[2], kFdnLengths[3] };
    float     damp[kFdnLines] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float     feedback = 0.6f;
    float     dampCoef = 0.4f;

    void prepare(double sr)
    {
        const float scale = (float)sr / 44100.0f;
        for (int i = 0; i < kFdnLines; ++i)
        {
            len[i] = (int)((float)kFdnLengths[i] * scale);
            if (len[i] < 8) len[i] = 8;
            line[i].prepare(len[i] + 4);
        }
        reset();
    }

    void reset()
    {
        for (int i = 0; i < kFdnLines; ++i) { line[i].reset(); damp[i] = 0.0f; }
    }

    void process(float inL, float inR, float& outL, float& outR)
    {
        float r[kFdnLines];
        for (int i = 0; i < kFdnLines; ++i) r[i] = line[i].readInt(len[i]);

        // Hadamard: energy preserving, and cheap - adds and subtracts only.
        const float a = r[0] + r[1], b = r[2] + r[3];
        const float c = r[0] - r[1], d = r[2] - r[3];
        float m[kFdnLines] = { (a + b) * 0.5f, (c + d) * 0.5f, (a - b) * 0.5f, (c - d) * 0.5f };

        for (int i = 0; i < kFdnLines; ++i)
        {
            const float in = (i & 1) ? inR : inL;
            damp[i] += dampCoef * (m[i] * feedback + in - damp[i]);
            line[i].write(flushDenormal(damp[i]));
        }
        outL = (r[0] + r[2]) * 0.5f;
        outR = (r[1] + r[3]) * 0.5f;
    }
};

} // namespace

// ==========================================================================================

class Whalesong : public mpc::Plugin
{
public:
    Whalesong() : mpc::Plugin(makeInfo())
    {
        std::vector<std::string> noteChoices;
        for (int i = 0; i < 12; ++i) noteChoices.push_back(kNoteNames[i]);
        std::vector<std::string> scaleChoices;
        for (int i = 0; i < kNumScales; ++i) scaleChoices.push_back(kScales[i].name);

        addParam(mpc::floatParam ("out",      "Output",   -24.0f, 12.0f,   0.0f,  "dB", 1));
        addParam(mpc::floatParam ("mix",      "Level",      0.0f, 100.0f, 60.0f,  "%",  0));
        addParam(mpc::choiceParam("role",     "Role",     { "Bass", "Lead", "Counter", "Pad", "Arp" }, kLead));
        addParam(mpc::floatParam ("activity", "Activity",   0.0f, 100.0f, 45.0f,  "%",  0));

        addParam(mpc::floatParam ("motion",   "Motion",     0.0f, 100.0f, 50.0f,  "%",  0));
        addParam(mpc::intParam   ("octave",   "Octave",       -2,      2,     0,  ""));
        addParam(mpc::choiceParam("sync",     "Sync",     { "Free", "Beat", "Bar" }, kOnBeat));
        addParam(mpc::choiceParam("rate",     "Rate",     divisionNames(), kDivisionEighth));

        addParam(mpc::floatParam ("human",    "Human",      0.0f, 100.0f, 25.0f,  "%",  0));
        addParam(mpc::logParam   ("glide",    "Glide",      1.0f, 400.0f,  1.0f,  "ms", 0));
        addParam(mpc::floatParam ("shape",    "Shape",      0.0f, 100.0f, 35.0f,  "%",  0));
        addParam(mpc::logParam   ("tone",     "Tone",     120.0f, 12000.0f, 2200.0f, "Hz", 0));

        addParam(mpc::floatParam ("reso",     "Reso",       0.0f, 100.0f, 20.0f,  "%",  0));
        addParam(mpc::logParam   ("attack",   "Attack",     1.0f, 2000.0f, 12.0f, "ms", 0));
        addParam(mpc::logParam   ("release",  "Release",   20.0f, 4000.0f, 420.0f, "ms", 0));
        addParam(mpc::floatParam ("drive",    "Drive",      0.0f, 100.0f, 15.0f,  "%",  0));

        addParam(mpc::floatParam ("spread",   "Spread",     0.0f, 100.0f, 40.0f,  "%",  0));
        addParam(mpc::floatParam ("space",    "Space",      0.0f, 100.0f, 30.0f,  "%",  0));
        addParam(mpc::intParam   ("voices",   "Voices",        1, kMaxVoices, 4, ""));
        addParam(mpc::choiceParam("listen",   "Listen",   { "Off", "Follow", "Lock" }, kListenFollow));

        addParam(mpc::floatParam ("sens",     "Sens",       0.0f, 100.0f, 50.0f,  "%",  0));
        addParam(mpc::choiceParam("wait",     "Wait",     { "Now", "1 Bar", "2 Bars", "4 Bars" }, 2));
        addParam(mpc::choiceParam("key",      "Key",      noteChoices, 0));
        addParam(mpc::choiceParam("scale",    "Scale",    scaleChoices, 0));

        // Meters. The plugin writes these from the audio thread so the screen can follow what it
        // is hearing; they are outputs, not settings, so they are not automation targets and are
        // kept out of saved state - a project should not store a stale level reading and push it
        // back in on load.
        auto meter = [this](mpc::Param p) { p.automatable = false; p.saved = false; addParam(p); };
        // "Input", not "Level": this meter reads how loud YOU are, while the Level control
        // above sets how loud the whalesong is. Two things called Level on one panel is one too
        // many. The id stays "level" because ids are permanent.
        meter(mpc::floatParam ("level", "Input", 0.0f, 100.0f, 0.0f, "%", 0));
        meter(mpc::choiceParam("heard", "Heard", { "-", "C", "C#", "D", "D#", "E", "F",
                                                   "F#", "G", "G#", "A", "A#", "B" }, 0));
        meter(mpc::choiceParam("found", "Found", { "-", "C", "C#", "D", "D#", "E", "F",
                                                   "F#", "G", "G#", "A", "A#", "B" }, 0));
        meter(mpc::floatParam ("conf",  "Conf",  0.0f, 100.0f, 0.0f, "%", 0));
        {
            std::vector<std::string> found = { "-" };
            for (int i = 0; i < kNumScales; ++i) found.push_back(kScales[i].name);
            meter(mpc::choiceParam("foundscale", "Mode", found, 0));
        }

        addParam(mpc::floatParam ("delaymix",  "Delay",    0.0f, 100.0f,  0.0f, "%",  0));
        addParam(mpc::choiceParam("delaytime", "Time",     divisionNames(), kDivisionEighth));
        addParam(mpc::floatParam ("delayfb",   "Feedback", 0.0f,  90.0f, 35.0f, "%",  0));
        addParam(mpc::floatParam ("chorus",    "Chorus",   0.0f, 100.0f,  0.0f, "%",  0));
        addParam(mpc::floatParam ("vibrato",   "Vibrato",  0.0f, 100.0f,  0.0f, "ct", 0));
        addParam(mpc::logParam   ("vibrate",   "Vib Rate", 0.1f,  12.0f,  5.0f, "Hz", 1));
        addParam(mpc::floatParam ("tremolo",   "Tremolo",  0.0f, 100.0f,  0.0f, "%",  0));
        addParam(mpc::logParam   ("tremrate",  "Trem Rate", 0.1f, 16.0f,  4.0f, "Hz", 1));

        addParam(mpc::choiceParam("window", "Window", { "Off", "1 Bar", "2 Bars", "4 Bars",
                                                          "8 Bars" }, 0));
        addParam(mpc::floatParam ("vary",   "Vary",   0.0f, 100.0f, 25.0f, "%", 0));
        addParam(mpc::choiceParam("renew",  "Renew",  { "Every 2", "Every 4", "Every 8",
                                                        "Never" }, 1));

        addParam(mpc::choiceParam("linger", "Linger", { "Never", "1 Bar", "2 Bars", "4 Bars",
                                                        "8 Bars", "16 Bars" }, 0));

        addPrograms();
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = (float)sampleRate;

        // Decimated history for the pitch detector, rounded up to a power of two for cheap wrapping.
        int want = (int)(kDecSeconds * sr_ / (float)kDecim);
        if (want < kYinWindow + kYinMaxLag + 8) want = kYinWindow + kYinMaxLag + 8;
        int size = 1;
        while (size < want) size <<= 1;
        decBuf_.assign((size_t)size, 0.0f);
        decMask_ = (int64_t)size - 1;
        yin_.assign((size_t)(kYinMaxLag + 2), 0.0f);

        seg_.prepare(sr_, kHopSamples, kYinSourceWindow);

        for (Voice& v : voices_) v.prepare(sampleRate);
        fdn_.prepare(sampleRate);

        maxDelay_ = (int)(kMaxDelaySeconds * sr_);
        delayL_.prepare(maxDelay_ + 4);
        delayR_.prepare(maxDelay_ + 4);
        const int chMax = (int)((kChorusBaseMs + kChorusSwingMs + 2.0f) * 0.001f * sr_);
        chorusL_.prepare(chMax + 4);
        chorusR_.prepare(chMax + 4);

        envFastRate_ = 1.0f - std::exp(-1.0f / (0.004f * sr_));
        envSlowRate_ = 1.0f - std::exp(-1.0f / (0.120f * sr_));
        outSm_.setTime(12.0f, sampleRate);
        mixSm_.setTime(12.0f, sampleRate);

        reset();
    }

    void reset() override
    {
        for (float& s : decBuf_) s = 0.0f;
        for (Voice& v : voices_) v.reset();
        fdn_.reset();
        delayL_.reset(); delayR_.reset(); chorusL_.reset(); chorusR_.reset();
        vibPhase_ = tremPhase_ = chorusPhase_ = 0.0f;

        writePos_ = 0; decPos_ = 0; decPhase_ = 0; hopCount_ = 0; voteCount_ = 0;
        hopPeak_ = 0.0f; hopPeakPos_ = 0;
        seg_.reset();
        envFast_ = envSlow_ = 0.0f;
        heardClass_ = -1; heardMidi_ = -1.0f; lastHeardMidi_ = -1.0f; contour_ = 0.0f;
        for (float& h : pcHist_) h = 0.0f;
        foundRoot_ = -1; foundScale_ = 0; keyConf_ = 0.0f; locked_ = false; stableHops_ = 0;
        listenedSamples_ = 0; quietSamples_ = 0;
        nextNoteAt_ = 0; stepIndex_ = 0; arpIndex_ = 0; lastDegree_ = 0;
        phraseCount_ = 0; replayIdx_ = 0; passIndex_ = 0; writingPhrase_ = true;
        windowPos_ = 0; windowSamples_ = 0;
        seed_ = 0x3C6EF35Fu;
        outSm_.snap(dbToGain(get(kOutput)));
        mixSm_.snap(get(kMix) * 0.01f);
        lastLevelSent_ = lastConfSent_ = -1.0f;
        lastHeardSent_ = lastFoundSent_ = lastScaleSent_ = -1.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const int outCh = clamp(ctx.numOutputs, 0, kMaxChannels);
        if (outCh <= 0) return;

        // ---- per-block parameter reads -------------------------------------------------
        const float targetOut = dbToGain(get(kOutput));
        const float targetMix = get(kMix) * 0.01f;
        const int   role      = clamp(getInt(kRole), 0, 4);
        const float activity  = clamp(get(kActivity) * 0.01f, 0.0f, 1.0f);
        const float motion    = clamp(get(kMotion) * 0.01f, 0.0f, 1.0f);
        const int   octave    = clamp(getInt(kOctave), -2, 2);
        const int   sync      = clamp(getInt(kSync), 0, 2);
        const int   rateIdx   = clamp(getInt(kRate), 0, kNumDivisions - 1);
        const float human     = clamp(get(kHuman) * 0.01f, 0.0f, 1.0f);
        const float shape     = clamp(get(kShape) * 0.01f, 0.0f, 1.0f);
        const float tone      = clamp(get(kTone), 60.0f, 0.45f * sr_);
        const float reso      = clamp(get(kReso) * 0.01f, 0.0f, 1.0f);
        const float drive     = clamp(get(kDrive) * 0.01f, 0.0f, 1.0f);
        const float spread    = clamp(get(kSpread) * 0.01f, 0.0f, 1.0f);
        const float space     = clamp(get(kSpace) * 0.01f, 0.0f, 1.0f);
        const int   listen    = clamp(getInt(kListen), 0, 2);

        const int maxVoices = clamp(getInt(kVoices), 1, kMaxVoices);
        const float attackMs  = clamp(get(kAttack), 1.0f, 2000.0f);
        const float releaseMs = clamp(get(kRelease), 20.0f, 4000.0f);
        const float glideMs   = clamp(get(kGlide), 1.0f, 400.0f);
        const float glideRate = 1.0f - std::exp(-1.0f / (0.001f * glideMs * sr_));

        // Voices follow the panel without being retriggered.
        for (int i = 0; i < kMaxVoices; ++i)
        {
            voices_[i].env.set(attackMs, 180.0f, 0.65f, releaseMs);
            voices_[i].filter.set(tone, 0.2f + reso * 0.72f);
            voices_[i].filter.setDrive(1.0f + drive * 6.0f);
            voices_[i].oscA.setWave(waveFor(shape));
            voices_[i].oscB.setWave(waveFor(shape));
            voices_[i].glideRate = glideRate;
        }
        fdn_.feedback = 0.42f + space * 0.52f;
        fdn_.dampCoef = 0.22f + (1.0f - space) * 0.45f;

        // Effects. All default to zero depth, and each is skipped entirely when it is off.
        const float delayMix  = clamp(get(kDelayMix) * 0.01f, 0.0f, 1.0f);
        const float delayFb   = clamp(get(kDelayFb) * 0.01f, 0.0f, 0.9f);
        const int   delayDiv  = clamp(getInt(kDelayTime), 0, kNumDivisions - 1);
        const float chorus    = clamp(get(kChorus) * 0.01f, 0.0f, 1.0f);
        const float vibCents  = clamp(get(kVibrato), 0.0f, 100.0f);
        const float vibRate   = clamp(get(kVibRate), 0.1f, 12.0f);
        const float tremDepth = clamp(get(kTremolo) * 0.01f, 0.0f, 1.0f);
        const float tremRate  = clamp(get(kTremRate), 0.1f, 16.0f);

        const float vibInc  = vibRate / sr_;
        const float tremInc = tremRate / sr_;
        const float chInc   = kChorusRateHz / sr_;
        const float chBase  = kChorusBaseMs * 0.001f * sr_;
        const float chSwing = kChorusSwingMs * 0.001f * sr_ * chorus;

        const float bpm = safeBpm(ctx.transport.tempoValid ? ctx.transport.bpm : 120.0);
        const float beatSamples = beatsToSamples(1.0f, bpm, sr_);
        const float barSamples  = beatSamples * (float)clamp(ctx.transport.timeSigNumerator, 1, 16);
        histDecay_ = std::exp(-(float)kVoteHop / (kHistHalfLifeBeats * beatSamples + 1.0f));

        float delaySamples = beatSamples * divisionBeats(delayDiv);
        delaySamples = clamp(delaySamples, 32.0f, (float)(maxDelay_ - 2));

        // The phrase window is both the span it remembers and the length it loops. Off means the
        // old behaviour: generate forever and never repeat.
        const int winSel = clamp(getInt(kWindow), 0, 4);
        const float vary = clamp(get(kVary) * 0.01f, 0.0f, 1.0f);
        const int renewSel = clamp(getInt(kRenew), 0, 3);
        const int renewEvery = renewSel >= 3 ? 0 : (2 << renewSel);   // 2, 4, 8, or never
        const int64_t wantWindow = winSel == 0 ? 0
                                 : (int64_t)(barSamples * (float)(1 << (winSel - 1)));
        if (wantWindow != windowSamples_)
        {
            windowSamples_ = wantWindow;
            windowPos_ = 0; replayIdx_ = 0; passIndex_ = 0;
            phraseCount_ = 0; writingPhrase_ = true;
        }
        // Align the window to the song, so a phrase starts on a bar line rather than wherever
        // the plugin happened to load.
        if (windowSamples_ > 0 && ctx.transport.ppqValid && ctx.transport.playing)
        {
            const double beatsPerWindow = (double)clamp(ctx.transport.timeSigNumerator, 1, 16)
                                        * (double)(1 << (winSel - 1));
            double posBeats = std::fmod(ctx.transport.ppqPos, beatsPerWindow);
            if (posBeats < 0.0) posBeats += beatsPerWindow;
            windowPos_ = (int64_t)(posBeats * (double)beatSamples);
        }

        // How long it listens before it is allowed to play.
        const int waitSel = clamp(getInt(kWait), 0, 3);
        const int64_t waitSamples = waitSel == 0 ? 0 : (int64_t)(barSamples * (float)(1 << (waitSel - 1)));

        // The detector decides "they are playing" relative to its own recent level.
        const float sens = clamp(get(kSens) * 0.01f, 0.0f, 1.0f);
        const float onThresh = dbToGain(-44.0f - 10.0f * sens);

        // How long it keeps playing once the input has gone quiet. Never means it does not stop
        // on its own, which is what you want when it is jamming alongside another instrument,
        // and is the default so nothing that already exists changes behaviour.
        const int lingerSel = clamp(getInt(kLinger), 0, 5);
        const int64_t lingerSamples = lingerSel == 0 ? 0
                                    : (int64_t)(barSamples * (float)(1 << (lingerSel - 1)));

        // ---- the audio loop ------------------------------------------------------------
        for (int i = 0; i < n; ++i)
        {
            const float inL = (ctx.inputs && ctx.numInputs > 0 && ctx.inputs[0]) ? ctx.inputs[0][i] : 0.0f;
            const float inR = (ctx.inputs && ctx.numInputs > 1 && ctx.inputs[1]) ? ctx.inputs[1][i]
                                                                                 : inL;
            const float mono = 0.5f * (inL + inR);

            // Envelope followers: fast for "is there a note", slow for the level meter.
            const float rect = std::fabs(mono);
            envFast_ += envFastRate_ * (rect - envFast_);
            envSlow_ += envSlowRate_ * (rect - envSlow_);

            // The loudest the input got since the last hop, and where. The segmenter needs
            // this to find attacks: by the time a hop comes round the envelope has already
            // decayed past the transient, so sampling it at the hop alone would miss every one.
            if (envFast_ > hopPeak_) { hopPeak_ = envFast_; hopPeakPos_ = writePos_; }

            // Decimate into the detector's history.
            decAccum_ += mono;
            if (++decPhase_ >= kDecim)
            {
                decPhase_ = 0;
                decBuf_[(size_t)(decPos_ & decMask_)] = decAccum_ * (1.0f / (float)kDecim);
                ++decPos_;
                decAccum_ = 0.0f;
            }

            if (envFast_ > onThresh) { ++listenedSamples_; quietSamples_ = 0; }
            else                     ++quietSamples_;

            // The pitch detector and the key estimate run on a hop, not per sample.
            if (++hopCount_ >= kHopSamples)
            {
                hopCount_ = 0;
                analyse(envFast_ > onThresh, listen);
                // analyse() has just refreshed heardMidi_, so the segmenter sees this hop's
                // estimate rather than the previous one's.
                seg_.hop(heardMidi_, hopPeak_, hopPeakPos_, onThresh, writePos_);
                hopPeak_ = 0.0f; hopPeakPos_ = writePos_;
            }

            // Should a note start on this sample?
            // Has it been quiet long enough to wind down? Only new notes are withheld; voices
            // already sounding keep their scheduled release, so it tails off rather than
            // cutting. Start playing again and the next note brings it straight back in.
            //
            // listenedSamples_ > 0 is the guard that stops this being a trap. Winding down
            // "after the input goes quiet" presupposes there WAS an input. Without the guard,
            // a free-running instance on an empty track - Listen off, nothing plugged in -
            // would play for one bar and then fall silent for good, which looks exactly like a
            // broken plugin. Having never heard anything, it has nothing to wind down from.
            const bool windDown = lingerSamples > 0 && listenedSamples_ > 0
                                  && quietSamples_ >= lingerSamples;

            const bool mayPlay = !windDown
                               && ((listen == kListenOff)
                                   || (listenedSamples_ >= waitSamples && foundRoot_ >= 0));

            if (windowSamples_ > 0)
            {
                if (++windowPos_ >= windowSamples_)
                {
                    windowPos_ = 0;
                    ++passIndex_;
                    replayIdx_ = 0;
                    // A fresh phrase every so often, so a take does not ossify; otherwise play
                    // the one we have back again.
                    const bool renew = (renewEvery > 0 && (passIndex_ % renewEvery) == 0);
                    if (renew || phraseCount_ == 0) { phraseCount_ = 0; writingPhrase_ = true; }
                    else                            { writingPhrase_ = false; }
                }

                if (mayPlay && !writingPhrase_)
                    replayPhrase(vary, maxVoices, spread);
                else if (mayPlay && writePos_ >= nextNoteAt_)
                    scheduleNext(role, activity, motion, octave, sync, rateIdx, human,
                                 beatSamples, barSamples, maxVoices, spread, ctx.transport);
            }
            else if (mayPlay && writePos_ >= nextNoteAt_)
            {
                scheduleNext(role, activity, motion, octave, sync, rateIdx, human,
                             beatSamples, barSamples, maxVoices, spread, ctx.transport);
            }

            // ---- render the voices ----
            float wetL = 0.0f, wetR = 0.0f;
            for (int v = 0; v < kMaxVoices; ++v)
            {
                Voice& vo = voices_[v];
                if (!vo.active) continue;

                if (vo.releaseAt >= 0 && writePos_ >= vo.releaseAt)
                {
                    vo.env.noteOff();
                    vo.releaseAt = -1;
                }

                vo.currentMidi += vo.glideRate * (vo.targetMidi - vo.currentMidi);
                const float hz = clamp(midiToHz(vo.currentMidi + vibDepth_), 16.0f, 0.45f * sr_);
                vo.oscA.setFrequency(hz);
                vo.oscB.setFrequency(hz * centsToRatio(spread * 11.0f));

                float s = 0.62f * vo.oscA.process() + 0.38f * vo.oscB.process();
                if (drive > 0.0f) s = fastTanh(s * (1.0f + drive * 3.0f)) * (1.0f / (1.0f + drive * 1.1f));
                s = vo.filter.process(s, SVF::Mode::Lowpass);
                s *= vo.env.process() * vo.velocity;

                if (!vo.env.isActive()) { vo.active = false; continue; }

                wetL += s * vo.panL;
                wetR += s * vo.panR;
            }

            // A little headroom so stacked voices cannot slam the limiter.
            wetL *= 0.34f;
            wetR *= 0.34f;

            // Vibrato is read here and applied on the next sample's oscillators: one sample of
            // lag at an audio rate is nothing, and it keeps the LFO out of the voice loop.
            vibPhase_ += vibInc; if (vibPhase_ >= 1.0f) vibPhase_ -= 1.0f;
            vibDepth_ = vibCents > 0.0f ? fastSin01(vibPhase_) * vibCents * 0.01f : 0.0f;

            if (tremDepth > 0.001f)
            {
                tremPhase_ += tremInc; if (tremPhase_ >= 1.0f) tremPhase_ -= 1.0f;
                const float g = 1.0f - tremDepth * (0.5f - 0.5f * fastSin01(tremPhase_));
                wetL *= g;
                wetR *= g;
            }

            if (chorus > 0.001f)
            {
                chorusPhase_ += chInc; if (chorusPhase_ >= 1.0f) chorusPhase_ -= 1.0f;
                // The two sides run a quarter cycle apart, which is what makes a chorus wide
                // rather than just detuned.
                const float dl = chBase + chSwing * 0.5f * (1.0f + fastSin01(chorusPhase_));
                const float dr = chBase + chSwing * 0.5f * (1.0f + fastSin01(chorusPhase_ + 0.25f));
                const float cl = chorusL_.process(wetL, dl);
                const float cr = chorusR_.process(wetR, dr);
                wetL += (cl - wetL) * chorus * 0.5f;
                wetR += (cr - wetR) * chorus * 0.5f;
            }

            if (delayMix > 0.001f)
            {
                // Feedback crosses over, so repeats walk left and right.
                const float dl = delayL_.readLinear(delaySamples);
                const float dr = delayR_.readLinear(delaySamples);
                delayL_.write(flushDenormal(wetL + dr * delayFb));
                delayR_.write(flushDenormal(wetR + dl * delayFb));
                wetL += dl * delayMix;
                wetR += dr * delayMix;
            }
            else
            {
                delayL_.write(0.0f);
                delayR_.write(0.0f);
            }

            if (space > 0.001f)
            {
                float rvL = 0.0f, rvR = 0.0f;
                fdn_.process(wetL * space, wetR * space, rvL, rvR);
                wetL += rvL * space;
                wetR += rvR * space;
            }

            wetL = softClip(wetL);
            wetR = softClip(wetR);

            const float mix = mixSm_.process();
            const float gain = outSm_.process();
            const float oL = (inL + wetL * mix) * gain;
            const float oR = (inR + wetR * mix) * gain;

            ctx.outputs[0][i] = sanitize(oL);
            if (outCh > 1) ctx.outputs[1][i] = sanitize(oR);

            ++writePos_;
        }

        mixSm_.setTarget(targetMix);
        outSm_.setTarget(targetOut);
        updateMeters();
    }

    void parameterChanged(int index, float plainValue) override
    {
        (void)plainValue;
        // Changing the key by hand clears what was learned, so the two cannot disagree on screen.
        if (index == kListen || index == kKey || index == kScale)
        {
            for (float& h : pcHist_) h = 0.0f;
            locked_ = false;
        }
    }

private:
    // --------------------------------------------------------------------------------------
    // Listening
    // --------------------------------------------------------------------------------------

    // One YIN pass over the newest window, then a vote into the histogram.
    void analyse(bool loudEnough, int listen)
    {
        const float midi = detectPitch();
        heardMidi_ = midi;
        if (midi < 0.0f) { heardClass_ = -1; }
        else
        {
            int cls = (int)std::lround(midi) % 12;
            if (cls < 0) cls += 12;
            heardClass_ = cls;

            // Contour: which way the line is moving, for the Counter role. The coefficient is
            // half what it was because the hop is half as long: same smoothing in seconds.
            if (lastHeardMidi_ > 0.0f)
            {
                const float d = clamp(midi - lastHeardMidi_, -12.0f, 12.0f);
                contour_ += 0.125f * (d - contour_);
            }
            lastHeardMidi_ = midi;
        }

        if (listen == kListenOff) return;

        // Everything below is the key estimate, which keeps its original 46 ms cadence.
        if (++voteCount_ < kVoteEvery) return;
        voteCount_ = 0;

        if (locked_ && listen == kListenLock) return;

        for (float& h : pcHist_) h *= histDecay_;

        if (loudEnough && heardClass_ >= 0)
        {
            // Louder notes and more confident estimates count for more.
            const float w = clamp(envFast_ * 6.0f, 0.05f, 1.0f);
            pcHist_[heardClass_] += w;
        }

        const int before = foundRoot_;
        estimateKey();
        if (foundRoot_ >= 0 && foundRoot_ == before && keyConf_ >= kLockConfidence) ++stableHops_;
        else stableHops_ = 0;
        if (listen == kListenLock && stableHops_ >= kLockStableHops) locked_ = true;
    }

    // Correlate the histogram against every (root, scale) pair.
    void estimateKey()
    {
        float total = 0.0f;
        for (float h : pcHist_) total += h;
        if (total < kHistMinWeight) { keyConf_ = 0.0f; return; }

        // The best score each root can reach, whichever scale shape gets it there.
        float bestPerRoot[12];
        int   bestScalePerRoot[12];
        for (int r = 0; r < 12; ++r) { bestPerRoot[r] = -1.0e9f; bestScalePerRoot[r] = 0; }

        for (int si = 0; si < kNumScales; ++si)
        {
            const ScaleDef& sc = kScales[si];
            for (int root = 0; root < 12; ++root)
            {
                float inKey = 0.0f, outKey = 0.0f;
                for (int pc = 0; pc < 12; ++pc)
                {
                    const float h = pcHist_[pc];
                    if (h <= 0.0f) continue;
                    int rel = (pc - root) % 12;
                    if (rel < 0) rel += 12;

                    bool found = false;
                    for (int k = 0; k < sc.count; ++k)
                        if (sc.notes[k] == rel) { inKey += h * degreeWeight(rel); found = true; break; }
                    if (!found) outKey += h;
                }
                // Notes outside the scale cost more than notes inside earn: a scale that merely
                // contains everything should not win by being permissive.
                const float score = (inKey - 1.6f * outKey) / total + kScalePrior[si];
                if (score > bestPerRoot[root]) { bestPerRoot[root] = score; bestScalePerRoot[root] = si; }
            }
        }

        int bestRoot = 0;
        for (int r = 1; r < 12; ++r) if (bestPerRoot[r] > bestPerRoot[bestRoot]) bestRoot = r;

        float bestOtherRoot = -1.0e9f;
        for (int r = 0; r < 12; ++r)
            if (r != bestRoot && bestPerRoot[r] > bestOtherRoot) bestOtherRoot = bestPerRoot[r];

        const float bestScore = bestPerRoot[bestRoot];
        foundRoot_ = bestRoot;
        foundScale_ = bestScalePerRoot[bestRoot];

        // Confidence is the margin over the best *other* root. Which scale shape won matters far
        // less than which note the music is leaning on.
        //
        // Measured margins run from about 0.001, where a tune sits exactly between a key and its
        // relative minor and the winner is a coin toss, to about 0.35 for four unambiguous bars.
        // This curve puts a near-tie near zero and saturates around a third, so the meter reads
        // the way a listener would: "no idea" until there is a reason, then quickly sure.
        const float margin = bestScore - bestOtherRoot;
        rawMargin_ = margin;
        const float sharpness = clamp(1.0f - std::exp(-margin / 0.12f), 0.0f, 1.0f);

        // ...and tempered by how much there is to go on. A margin can be wide for a bad reason:
        // one note played over and over beats every key that does not have it as the tonic, and
        // says nothing, because that note could be the tonic of one key, the third of another and
        // the fifth of a third. Count the pitch classes carrying real weight instead. Five of
        // them is a pentatonic's worth and earns full marks; one earns none.
        int distinct = 0;
        for (int pc = 0; pc < 12; ++pc) if (pcHist_[pc] > total * 0.04f) ++distinct;
        const float breadth = clamp((float)(distinct - 1) * 0.25f, 0.0f, 1.0f);

        keyConf_ = sharpness * breadth;
    }

    // YIN on the decimated history: the squared difference between the signal and itself delayed
    // by tau, normalised by its own running mean so a flat quiet passage cannot look periodic.
    float detectPitch()
    {
        const int need = kYinWindow + kYinMaxLag;
        if (decPos_ < need + 2) return -1.0f;

        const int64_t base = decPos_ - need;
        auto at = [this, base](int i) { return decBuf_[(size_t)((base + i) & decMask_)]; };

        float energy = 0.0f;
        for (int j = 0; j < kYinWindow; ++j) energy += at(j) * at(j);
        if (energy / (float)kYinWindow < 1.0e-7f) return -1.0f;

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
            yin_[(size_t)tau] = runningSum > 0.0f ? d * (float)tau / runningSum : 1.0f;
            if (tau < kYinMinLag) continue;
            if (yin_[(size_t)tau] < bestVal) { bestVal = yin_[(size_t)tau]; bestLag = tau; }
            // The first dip under the threshold wins: a deeper minimum usually sits at twice the
            // true period, which is how YIN ends up an octave low.
            if (tau > kYinMinLag && yin_[(size_t)(tau - 1)] < kYinGood
                && yin_[(size_t)tau] > yin_[(size_t)(tau - 1)])
            {
                bestLag = tau - 1;
                bestVal = yin_[(size_t)(tau - 1)];
                break;
            }
        }
        if (bestLag < kYinMinLag || bestVal > kYinUsable) return -1.0f;

        float lag = (float)bestLag;
        if (bestLag > 1 && bestLag < kYinMaxLag)
        {
            const float a = yin_[(size_t)(bestLag - 1)], b = yin_[(size_t)bestLag],
                        c = yin_[(size_t)(bestLag + 1)];
            const float denom = a - 2.0f * b + c;
            if (std::fabs(denom) > 1.0e-9f) lag += 0.5f * (a - c) / denom;
        }
        const float hz = (sr_ / (float)kDecim) / clamp(lag, 2.0f, (float)kYinMaxLag);
        return hzToMidi(clamp(hz, 20.0f, 4000.0f));
    }

    // --------------------------------------------------------------------------------------
    // Playing
    // --------------------------------------------------------------------------------------

    int activeRoot() const
    {
        if (clamp(getInt(kListen), 0, 2) == kListenOff || foundRoot_ < 0)
            return clamp(getInt(kKey), 0, 11);
        return foundRoot_;
    }

    const ScaleDef& activeScale() const
    {
        if (clamp(getInt(kListen), 0, 2) == kListenOff || foundRoot_ < 0)
            return kScales[clamp(getInt(kScale), 0, kNumScales - 1)];
        return kScales[clamp(foundScale_, 0, kNumScales - 1)];
    }

    // A scale degree (which may run past an octave) as a MIDI note.
    float degreeToMidi(int degree, int baseOctave) const
    {
        const ScaleDef& sc = activeScale();
        const int shift = (int)std::floor((float)degree / (float)sc.count);
        int within = degree - shift * sc.count;
        if (within < 0) within += sc.count;
        return (float)(activeRoot() + 12 * (baseOctave + shift) + sc.notes[within]);
    }

    void scheduleNext(int role, float activity, float motion, int octave, int sync, int rateIdx,
                      float human, float beatSamples, float barSamples, int maxVoices,
                      float spread, const mpc::Transport& transport)
    {
        (void)transport;

        // How long until the next opportunity to play.
        float stepSamples;
        if (sync == kFree)
            stepSamples = beatSamples * mapRange(1.0f - activity, 0.18f, 2.2f);
        else if (sync == kOnBar)
            stepSamples = barSamples;
        else
            stepSamples = beatSamples * divisionBeats(rateIdx);

        if (stepSamples < 64.0f) stepSamples = 64.0f;
        nextNoteAt_ = writePos_ + (int64_t)stepSamples;
        ++stepIndex_;

        // Activity is the chance that this opportunity is taken. Bass and Pad hold notes for
        // longer so they are rationed harder, or they would never stop.
        float chance = activity;
        if (role == kBass) chance *= 0.55f;
        if (role == kPad)  chance *= 0.30f;
        if (role == kArp)  chance = clamp(activity * 1.5f, 0.0f, 1.0f);

        // ...but rationing must not be able to silence a part that is supposed to sustain. A pad
        // rolling a 27% die once a bar spends most of its time silent, which is not a pad at all;
        // it is a pad that sometimes happens. So when nothing of theirs is sounding, the two
        // sustaining roles always take the opportunity, and Activity goes back to meaning how
        // often the chord CHANGES rather than whether there is one.
        if (role == kPad || role == kBass)
        {
            int sounding = 0;
            for (int v = 0; v < maxVoices; ++v)
                if (voices_[v].active && !voices_[v].env.isReleasing()) ++sounding;
            if (sounding == 0) chance = 1.0f;
        }

        if (randf() > chance) return;

        // Bass sits an octave below the middle, lead an octave above. Two octaves down put the
        // bass at C1, which is below most speakers and reads as a thud rather than a note.
        const int baseOct = 4 + octave + (role == kBass ? -1 : 0) + (role == kLead ? 1 : 0);
        const int reach = 1 + (int)(motion * 6.0f);

        int degree = 0;
        int noteCount = 1;
        int degrees[3] = { 0, 0, 0 };
        float holdBeats = 1.0f;

        switch (role)
        {
            case kBass:
                // Roots and fifths, mostly roots, moving on the bar.
                degree = (randf() < 0.72f) ? 0 : 4;
                if (randf() < motion * 0.3f) degree = 2;
                holdBeats = 1.5f + randf() * 1.5f;
                break;

            case kLead:
            {
                // A line that steps more often than it leaps, kept inside Motion's reach.
                const int step = (randf() < 0.68f) ? (randf() < 0.5f ? 1 : -1)
                                                   : (int)(randf() * (float)reach) - reach / 2;
                lastDegree_ = clamp(lastDegree_ + step, -reach, reach);
                degree = lastDegree_;
                holdBeats = 0.4f + randf() * 0.9f;
                break;
            }

            case kCounter:
            {
                // Move opposite to the player: when they rise, it falls.
                const int dir = contour_ > 0.3f ? -1 : (contour_ < -0.3f ? 1 : (randf() < 0.5f ? -1 : 1));
                const int step = dir * (1 + (int)(randf() * motion * 3.0f));
                lastDegree_ = clamp(lastDegree_ + step, -reach, reach);
                degree = lastDegree_;
                holdBeats = 0.6f + randf() * 1.2f;
                break;
            }

            case kPad:
                // A triad from the current degree, held for a bar or more.
                degree = (randf() < 0.6f) ? 0 : (randf() < 0.5f ? 3 : 4);
                degrees[0] = degree; degrees[1] = degree + 2; degrees[2] = degree + 4;
                noteCount = 3;
                holdBeats = 3.0f + randf() * 2.0f;
                break;

            case kArp:
            default:
            {
                // Cycle chord tones: root, third, fifth, octave.
                static const int kArpSteps[4] = { 0, 2, 4, 7 };
                degree = kArpSteps[arpIndex_ & 3];
                arpIndex_ = (arpIndex_ + 1) & 3;
                if (randf() < motion * 0.25f) degree += 7;
                holdBeats = 0.25f + randf() * 0.25f;
                break;
            }
        }

        if (noteCount == 1) degrees[0] = degree;

        // Humanise: a little early or late, a little louder or quieter.
        const int64_t jitter = (int64_t)((randf() - 0.5f) * human * 0.06f * beatSamples);
        const float velocity = clamp(0.78f + (randf() - 0.5f) * human * 0.55f, 0.18f, 1.0f);
        const int64_t holdSamples = (int64_t)(holdBeats * beatSamples);

        for (int k = 0; k < noteCount; ++k)
        {
            const float midi = clamp(degreeToMidi(degrees[k], baseOct), 21.0f, 108.0f);
            startNote(midi, velocity, holdSamples, jitter, maxVoices, spread,
                      noteCount > 1 ? (float)k / (float)(noteCount - 1) : randf());

            // Keep it, so this window can be played again. Degrees, not notes: see PhraseNote.
            if (windowSamples_ > 0 && writingPhrase_ && phraseCount_ < kMaxPhraseNotes)
            {
                PhraseNote& nt = phrase_[phraseCount_++];
                nt.offset = windowPos_;
                nt.degree = degrees[k];
                nt.octave = baseOct;
                nt.hold = holdSamples;
                nt.velocity = velocity;
            }
        }
    }

    // Play the stored phrase back, bending it a little as it goes. Variation is what keeps a
    // repeat from turning into a loop: the shape stays, the detail moves. Four kinds, in rough
    // order of how much they change the line - a neighbouring degree, a rest, an octave, a
    // different weight - because a phrase that only ever gains notes stops breathing.
    void replayPhrase(float vary, int maxVoices, float spread)
    {
        while (replayIdx_ < phraseCount_ && phrase_[replayIdx_].offset <= windowPos_)
        {
            const PhraseNote nt = phrase_[replayIdx_++];
            int   deg = nt.degree;
            int   oct = nt.octave;
            float vel = nt.velocity;

            if (vary > 0.0f && randf() < vary)
            {
                const float r = randf();
                if (r < 0.45f)      deg += (randf() < 0.5f ? 1 : -1);
                else if (r < 0.68f) continue;                              // leave a rest
                else if (r < 0.82f) oct += (randf() < 0.5f ? 1 : -1);
                else                vel = clamp(vel * (0.55f + randf() * 0.7f), 0.15f, 1.0f);
            }

            const float midi = clamp(degreeToMidi(deg, oct), 21.0f, 108.0f);
            startNote(midi, vel, nt.hold, 0, maxVoices, spread, randf());
        }
    }

    void startNote(float midi, float velocity, int64_t holdSamples, int64_t jitter,
                   int maxVoices, float spread, float panPos)
    {
        // Find a free voice, or steal the one that has been going longest.
        int pick = -1;
        for (int v = 0; v < maxVoices; ++v)
            if (!voices_[v].active) { pick = v; break; }
        if (pick < 0)
        {
            int64_t oldest = INT64_MAX;
            for (int v = 0; v < maxVoices; ++v)
                if (voices_[v].startedAt < oldest) { oldest = voices_[v].startedAt; pick = v; }
        }
        if (pick < 0) return;

        Voice& vo = voices_[pick];
        const bool wasActive = vo.active;

        vo.targetMidi = midi;
        if (!wasActive) vo.currentMidi = midi;    // no glide into silence
        vo.velocity = velocity;
        vo.active = true;
        vo.startedAt = writePos_;
        vo.releaseAt = writePos_ + (holdSamples > 0 ? holdSamples : 1) + jitter;
        vo.env.noteOn();

        const float pan = clamp((panPos - 0.5f) * 2.0f * spread, -1.0f, 1.0f);
        equalPowerPan(pan, vo.panL, vo.panR);
    }

    static int waveFor(float shape)
    {
        if (shape < 0.25f) return 0;         // sine
        if (shape < 0.50f) return 1;         // triangle
        if (shape < 0.80f) return 2;         // saw
        return 3;                            // square
    }

    uint32_t rand32()
    {
        seed_ = seed_ * 1664525u + 1013904223u;
        return seed_;
    }
    float randf() { return (float)(rand32() >> 8) * (1.0f / 16777216.0f); }

    // --------------------------------------------------------------------------------------
    // Meters. Written once per block, and only when they have actually moved.
    // --------------------------------------------------------------------------------------
    void updateMeters()
    {
        const float lvl = clamp(envSlow_ * 320.0f, 0.0f, 100.0f);
        if (std::fabs(lvl - lastLevelSent_) > 1.5f)
        {
            lastLevelSent_ = lvl;
            setNormalised(kLevel, lvl * 0.01f);
            notifyHostParameterChanged(kLevel);
        }

        const float hv = (float)(heardClass_ >= 0 ? heardClass_ + 1 : 0);
        if (hv != lastHeardSent_)
        {
            lastHeardSent_ = hv;
            setNormalised(kHeard, hv / 12.0f);
            notifyHostParameterChanged(kHeard);
        }

        const float fv = (float)(foundRoot_ >= 0 ? foundRoot_ + 1 : 0);
        if (fv != lastFoundSent_)
        {
            lastFoundSent_ = fv;
            setNormalised(kFound, fv / 12.0f);
            notifyHostParameterChanged(kFound);
        }

        const float sv = (float)(foundRoot_ >= 0 ? foundScale_ + 1 : 0);
        if (sv != lastScaleSent_)
        {
            lastScaleSent_ = sv;
            setNormalised(kFoundScale, sv / (float)kNumScales);
            notifyHostParameterChanged(kFoundScale);
        }

        const float cf = clamp(keyConf_ * 100.0f, 0.0f, 100.0f);
        if (std::fabs(cf - lastConfSent_) > 2.0f)
        {
            lastConfSent_ = cf;
            setNormalised(kConf, cf * 0.01f);
            notifyHostParameterChanged(kConf);
        }
    }

    void addPrograms()
    {
        // Positional, in parameter order up to kScale; anything left off takes its default.
        auto prog = [this](const char* name, std::vector<float> v,
                           std::initializer_list<std::pair<int, float>> extra = {})
        {
            const size_t given = v.size();
            v.resize((size_t)kNumParams);
            for (size_t i = given; i < v.size(); ++i) v[i] = param((int)i).def;
            for (const auto& e : extra)
                if (e.first >= 0 && e.first < kNumParams) v[(size_t)e.first] = e.second;
            addProgram(name, std::move(v));
        };
        //            out   mix  role          act   motion oct sync      rate              human glide shape tone    reso att    rel    drive spread space voices listen         sens wait key scale
        prog("First Light", { 0, 55, (float)kLead,    40,  45, 0, (float)kOnBeat, (float)kDivisionEighth,  28,  1,  30, 2600, 18,  14,  380, 12, 45, 35, 4, (float)kListenFollow, 50, 2, 0, 0 });
        prog("Deep Voice",  { 0, 62, (float)kBass,    55,  25, 0, (float)kOnBeat, (float)kDivisionQuarter, 18,  40, 55, 900,  28,  18,  300, 30, 15, 12, 2, (float)kListenFollow, 50, 2, 0, 0 });
        prog("Answering",   { 0, 58, (float)kCounter, 38,  60, 0, (float)kOnBeat, (float)kDivisionEighth,  35,  18, 25, 3200, 22,  26,  620, 10, 60, 45, 4, (float)kListenFollow, 60, 2, 0, 0 });
        prog("Slow Drift",  { 0, 48, (float)kPad,     30,  35, 0, (float)kOnBar,  (float)kDivisionBar,     12,  90, 12, 1700, 15,  600, 2200, 8, 75, 65, 5, (float)kListenFollow, 45, 3, 0, 0 },
             { { kChorus, 55.0f }, { kVibrato, 9.0f }, { kVibRate, 0.8f } });
        prog("Sonar",       { 0, 52, (float)kArp,     62,  40, 1, (float)kOnBeat, (float)kDivisionSixteenth, 20, 1, 70, 4200, 35,  3,   180, 22, 55, 40, 4, (float)kListenFollow, 50, 1, 0, 0 },
             { { kDelayMix, 42.0f }, { kDelayTime, (float)kDivisionEighth }, { kDelayFb, 46.0f } });
        prog("Pod Chorus",  { 0, 60, (float)kPad,     35,  55, 0, (float)kOnBar,  (float)kDivisionBar,     22,  120, 20, 2100, 20, 900, 2800, 6, 85, 78, 6, (float)kListenLock,  55, 3, 0, 0 },
             { { kChorus, 82.0f }, { kVibrato, 14.0f }, { kVibRate, 0.6f } });

        // Built around the effects rather than merely wearing them.
        prog("Tide Echo",   { 0, 54, (float)kLead,    30,  55, 0, (float)kOnBeat, (float)kDivisionEighth,  30,  18, 40, 3000, 24,  16,  300, 12, 55, 30, 3, (float)kListenFollow, 55, 2, 0, 0 },
             { { kDelayMix, 58.0f }, { kDelayTime, (float)kDivisionEighth }, { kDelayFb, 62.0f },
               { kChorus, 30.0f } });
        prog("Siren",       { 0, 58, (float)kLead,    26,  70, 0, (float)kFree,   (float)kDivisionQuarter, 15,  260, 18, 1500, 30, 420, 1800, 10, 70, 55, 2, (float)kListenFollow, 60, 2, 0, 0 },
             { { kVibrato, 46.0f }, { kVibRate, 4.6f }, { kTremolo, 38.0f }, { kTremRate, 3.2f },
               { kDelayMix, 26.0f }, { kDelayTime, (float)kDivisionQuarter } });
        prog("Fixed C Min", { 0, 55, (float)kLead,    45,  50, 0, (float)kOnBeat, (float)kDivisionEighth,  30,  1,  35, 2400, 20,  12,  420, 15, 40, 30, 4, (float)kListenOff,   50, 0, 0, 1 });
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Whalesong";
        info.vendor       = "Groundswell";
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);
        info.version      = 1000;
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 96000;    // the reverb and a long release both ring on after the input
        return info;
    }

    static constexpr char kUid[5] = "G89M";

    // ---- state -------------------------------------------------------------------------
    float sr_ = 44100.0f;

    std::vector<float> decBuf_;        // decimated input history for the detector
    std::vector<float> yin_;           // YIN difference function
    int64_t decMask_ = 0;
    int64_t writePos_ = 0, decPos_ = 0;
    int     decPhase_ = 0;
    float   decAccum_ = 0.0f;
    int     hopCount_ = 0, voteCount_ = 0;
    float   hopPeak_ = 0.0f;
    int64_t hopPeakPos_ = 0;

    // The input, as notes rather than as a stream of estimates. Nothing plays from this yet:
    // it is the groundwork for roles that answer what you actually played - echo the intervals
    // back, invert them, reverse them - and it is tested on its own before any of that lands.
    ws::InputSegmenter seg_;

    float envFast_ = 0.0f, envSlow_ = 0.0f;
    float envFastRate_ = 1.0f, envSlowRate_ = 1.0f;

    int   heardClass_ = -1;
    float heardMidi_ = -1.0f, lastHeardMidi_ = -1.0f;
    float contour_ = 0.0f;             // smoothed direction of the input line, in semitones

    float pcHist_[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    float histDecay_ = 0.999f;
    int   foundRoot_ = -1, foundScale_ = 0;
    float keyConf_ = 0.0f;
    int   stableHops_ = 0;      // consecutive estimates that agreed, and were sure
    float rawMargin_ = 0.0f;
    bool  locked_ = false;
    int64_t listenedSamples_ = 0, quietSamples_ = 0;

    PhraseNote phrase_[kMaxPhraseNotes];
    int     phraseCount_ = 0;
    int     replayIdx_ = 0;
    int     passIndex_ = 0;
    bool    writingPhrase_ = true;    // false once a phrase exists and is being played back
    int64_t windowPos_ = 0, windowSamples_ = 0;

    int64_t nextNoteAt_ = 0;
    int     stepIndex_ = 0, arpIndex_ = 0, lastDegree_ = 0;
    uint32_t seed_ = 0x3C6EF35Fu;

    Voice voices_[kMaxVoices];
    Fdn   fdn_;

    DelayLine delayL_, delayR_, chorusL_, chorusR_;
    int   maxDelay_ = 1024;
    float vibPhase_ = 0.0f, tremPhase_ = 0.0f, chorusPhase_ = 0.0f;
    float vibDepth_ = 0.0f;              // semitones, smoothed per block into the voices

    Smoother outSm_, mixSm_;

    float lastLevelSent_ = -1.0f, lastHeardSent_ = -1.0f, lastFoundSent_ = -1.0f,
          lastConfSent_ = -1.0f, lastScaleSent_ = -1.0f;
};

MPC_PLUGIN_ENTRY(Whalesong)
