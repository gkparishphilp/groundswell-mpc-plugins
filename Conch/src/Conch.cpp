// Conch - an MPC insert effect by Groundswell.
//
// What it is: a bank of tuned resonators, like the sympathetic strings of a sitar, that ring at the
// chord or the key. Put it on a drum loop and the hits come out as pitched, in-key tones; put it on a
// mix and it adds a singing, harmonic halo. Hold it to your ear and the track sings back.
//
// Signal flow
// -----------
//   input --+------------------------------------------------------------------- dry ---+
//           |                                                                           |
//           +--> mono --> In HPF --> Drive --> Hits (transient gate) --> tone --> excite  |
//           |                                      |                       |            |
//           |                                      +--> Duck envelope      v            |
//           |                                                   resonator banks (x3)    |
//           |                                                   String / Bell / Pipe    |
//           |                                                   up to 16 notes, panned  |
//           |                                                   by Width                |
//           |                                                           |               |
//           |                                         level guard --> duck --> Mix -----+--> Output
//           |
//           +--> mono --> Harmony (constant-Q chroma, from Largemouth) --> key + chord
//                         (only in the Listen sources)
//
// Which notes ring (Source):
//   Listen      the key and the current chord it hears. Until it is sure of a key (a drum loop on
//               its own has none to hear) it uses Key and Scale, and the Tuned By meter says
//               "Key (unsure)".
//   Listen Key  the key it hears, tonic chord only: steadier on a busy mix.
//   Key         the tonic chord of Key and Scale.
//   Chord       Chord Root + Quality, set by hand.
//   Prog        a progression in Key and Scale, one chord per Chord Len, locked to the song position.
// Tones says how much of a key's chord to ring (triad, 7th, 9th or root and fifth). Spread and Low
// Note set the range: every chord tone between Low Note and Spread octaves above it gets a resonator.
//
// Chord changes: there are three resonator banks. The new chord starts in a fresh bank (its input
// fades in over 5 ms; it starts silent, so nothing clicks) while the old bank stops listening and
// rings out over at most 1.2 s. A third bank covers quick changes; if all three are busy the
// quietest ring-out fades away in 15 ms first.
//
// Freeze: the current bank stops listening and its decay goes to about five minutes; the chord
// holds until Freeze is off. New chords wait until then.
//
// Realtime: nothing is allocated, locked or logged in process(). The chroma analysis is spread over
// several blocks (Harmony::work). Resonator coefficients are computed only when a bank starts or a
// knob moves; everything else runs in 32-sample sub-blocks.
//
// CPU: see README. Roughly 1-3% of an MPC core with 8 notes; a chord change briefly runs two banks.

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/math.h"

#include "Harmony.h"
#include "ChordTones.h"
#include "Resonators.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace conch;

// ------------------------------------------------------------------------------------------
// Parameters. The order is permanent once anyone has a project: append, never insert.
// ------------------------------------------------------------------------------------------
enum ParamIndex
{
    kOutput = 0,   // master volume. The host nudges parameter 0 on every load: keep it harmless.
    kMix,          // dry/wet: 0 dry, 50 both full, 100 wet only
    kMaterial,     // String, Bell, Pipe
    kDecay,        // ring time to -60 dB

    kBright,       // damping: dark to bright
    kSpread,       // how many octaves of chord tones get a resonator
    kLowNote,      // the lowest resonator octave
    kTones,        // Triad, 7th, 9th, 5ths (root and fifth)

    kWidth,        // resonators panned across the stereo field
    kHits,         // 0: all of the input rings them; 100: only the transients do
    kHpf,          // high-pass on what excites them
    kDrive,        // saturation on what excites them

    kDuck,         // the ringing dips under each new hit
    kSource,       // Listen, Listen Key, Key, Chord, Prog
    kKey,          // key root for Key/Prog, and for Listen until it is sure
    kScale,        // scale likewise; also flavours a heard key

    kChordRoot,    // Chord source: the root
    kQuality,      // Chord source: the chord
    kProg,         // Prog source: which progression
    kChordLen,     // Prog source: how long each chord lasts

    kFreeze,       // hold the current chord indefinitely
    kShimmer,      // slow detune drift, a few cents per note
    kTune,         // fine tune, for tracks that are not at A440

    // Meters, written by the plugin. Not saved, not automatable.
    kFoundKey,     // the key it hears ("--" until fairly sure)
    kSure,         // how sure it is of that key
    kChordNow,     // the chord the resonators are tuned to
    kTunedBy,      // where that chord comes from right now

    kNumParams
};

enum Source { kSrcListen = 0, kSrcListenKey, kSrcKey, kSrcChord, kSrcProg };
enum TunedBy { kByKey = 0, kByUnsure, kByHeard, kByChord, kByProg, kByFrozen };

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const int kLowNotes[4] = {24, 36, 48, 60};     // C1..C4

// Progressions as zero-based scale degrees (the same set as Largemouth's Changes).
const int kProgs[][4] = {
    {0, 5, 6, 0},   // 1-6-7-1
    {0, 3, 4, 0},   // 1-4-5-1
    {0, 4, 5, 3},   // 1-5-6-4
    {0, 6, 5, 6},   // 1-7-6-7
    {0, 3, 5, 4},   // 1-4-6-5
    {0, 5, 3, 4},   // 1-6-4-5
};
constexpr int kNumProgs = (int)(sizeof(kProgs) / sizeof(kProgs[0]));
const float kChordBars[4] = {0.5f, 1.0f, 2.0f, 4.0f};

constexpr float kRingOut = 1.2f;          // seconds (T60) an old chord rings on after a change
constexpr float kShimmerMax = 0.0046f;    // 8 cents
constexpr float kWetMakeup = 1.0f;        // overall wet level
// Per-material level trims so a material change keeps roughly the same loudness (measured by
// tests/conchtest.cpp on a drum loop).
const float kMaterialTrim[kNumMaterials] = {1.0f, 0.27f, 0.78f};
constexpr float kKeySure = 0.35f;         // Harmony confidence that counts as "found"
constexpr float kTonalSure = 0.3f;        // ...and how tonal the input must be (drums alone score ~0.15)
constexpr float kKeyHold = 1.0f;          // seconds a different key must be heard before switching

std::vector<std::string> noteChoices() { return std::vector<std::string>(kNoteNames, kNoteNames + 12); }

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
        for (const Quality& q : kQualities) v.emplace_back(std::string(n) + q.suffix);
    return v;
}
inline int modi(long long a, int m) { long long r = a % m; return (int)(r < 0 ? r + m : r); }

} // namespace

class Conch : public mpc::Plugin
{
public:
    Conch() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        auto meter = [this](Param p) { p.automatable = false; p.saved = false; addParam(p); };

        // Value text comes from formatValue (units included), so the unit fields stay empty.
        addParam(floatParam("output", "Output", -24.0f, 12.0f, 0.0f, "", 1));
        addParam(floatParam("mix", "Mix", 0.0f, 100.0f, 40.0f, "", 0));
        addParam(choiceParam("material", "Material", {"String", "Bell", "Pipe"}, kString));
        addParam(logParam("decay", "Decay", 0.1f, 20.0f, 2.5f, "", 2));

        addParam(floatParam("bright", "Bright", 0.0f, 100.0f, 55.0f, "", 0));
        addParam(intParam("spread", "Spread", 1, 4, 3, ""));
        addParam(choiceParam("lownote", "Low Note", {"C1", "C2", "C3", "C4"}, 1));
        addParam(choiceParam("tones", "Tones", {"Triad", "7th", "9th", "5ths"}, kTriad));

        addParam(floatParam("width", "Width", 0.0f, 100.0f, 60.0f, "", 0));
        addParam(floatParam("hits", "Hits", 0.0f, 100.0f, 60.0f, "", 0));
        addParam(logParam("hpf", "In HPF", 20.0f, 2000.0f, 120.0f, "", 0));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 0.0f, "", 0));

        addParam(floatParam("duck", "Duck", 0.0f, 100.0f, 20.0f, "", 0));
        addParam(choiceParam("source", "Source", {"Listen", "Listen Key", "Key", "Chord", "Prog"}, kSrcListen));
        addParam(choiceParam("key", "Key", noteChoices(), 9));
        {
            std::vector<std::string> names;
            for (const ScaleDef& s : kScales) names.emplace_back(s.name);
            addParam(choiceParam("scale", "Scale", names, 0));
        }

        addParam(choiceParam("chordroot", "Chord Root", noteChoices(), 9));
        {
            std::vector<std::string> names;
            for (const Quality& q : kQualities) names.emplace_back(q.name);
            addParam(choiceParam("quality", "Quality", names, 1));
        }
        addParam(choiceParam("prog", "Prog", {"1-6-7-1", "1-4-5-1", "1-5-6-4", "1-7-6-7", "1-4-6-5", "1-6-4-5"}, 0));
        addParam(choiceParam("chordlen", "Chord Len", {"1/2 Bar", "1 Bar", "2 Bars", "4 Bars"}, 1));

        addParam(boolParam("freeze", "Freeze", false));
        addParam(floatParam("shimmer", "Shimmer", 0.0f, 100.0f, 10.0f, "", 0));
        addParam(floatParam("tune", "Tune", -50.0f, 50.0f, 0.0f, "", 0));

        meter(choiceParam("foundkey", "Found", keyMeterNames(), 0));
        meter(floatParam("sure", "Sure", 0.0f, 100.0f, 0.0f, "", 0));
        meter(choiceParam("chordnow", "Ringing", chordMeterNames(), 0));
        meter(choiceParam("tunedby", "Tuned By", {"Key/Scale", "Key (unsure)", "Heard", "Chord", "Prog", "Frozen"}, 0));

        addPrograms();
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = sampleRate;
        // Delay lines: long enough for C1 (or half of it for a pipe) plus tune and shimmer.
        int len = 256;
        while ((double)len < sampleRate / 30.0 + 16.0) len *= 2;
        lineLen_ = len;
        lines_.assign((size_t)3 * kMaxPitches * (size_t)len, 0.0f);
        for (int b = 0; b < 3; ++b)
        {
            banks_[b].attach(lines_.data() + (size_t)b * kMaxPitches * (size_t)len, len);
            banks_[b].prepare(sampleRate);
        }
        harm_.prepare(sampleRate);

        const float fs = (float)sampleRate;
        fastAtt_ = 1.0f - std::exp(-1.0f / (0.0015f * fs));
        slowAtt_ = 1.0f - std::exp(-1.0f / (0.04f * fs));
        hitSmooth_ = 1.0f - std::exp(-1.0f / (0.0005f * fs));
        duckAtt_ = 1.0f - std::exp(-1.0f / (0.001f * fs));
        duckRel_ = std::exp(-1.0f / (0.15f * fs));
        inEnvRel_ = std::exp(-1.0f / (3.0f * fs));
        guardRelSub_ = 1.0f - std::exp(-(float)kSub / (0.3f * fs));
        gainSmooth_ = 1.0f - std::exp(-1.0f / (0.02f * fs));
        meterInterval_ = (int)(0.1 * sampleRate);
        hpfHz_ = -1.0f;
        reset();
    }

    void reset() override
    {
        for (Bank& b : banks_) b.reset();
        harm_.reset();
        hpf_.reset();
        active_ = -1;
        pending_ = false;
        reqN_ = -1; reqMaterial_ = -1;
        toneZ_ = fast1_ = fastEnv_ = slowEnv_ = hitSm_ = duckEnv_ = inEnv_ = 0.0f;
        guard_ = 1.0f;
        haveFound_ = false; foundRoot_ = 9; foundMinor_ = true;
        candRoot_ = -1; candMinor_ = false; candTime_ = 0.0f;
        progIdx_ = 0;
        outGain_ = mpc::dsp::dbToGain(get(kOutput));
        const float mix = get(kMix) * 0.01f;
        dryGain_ = mix < 0.5f ? 1.0f : 2.0f * (1.0f - mix);
        wetGain_ = (mix > 0.5f ? 1.0f : 2.0f * mix) * kWetMakeup;
        tunedBy_ = kByKey;
        reqChord_ = Chord();
        meterCountdown_ = 0;
        for (float& m : lastMeter_) m = -1.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const int numOut = mpc::clamp(ctx.numOutputs, 0, 2);
        if (n <= 0 || numOut == 0) return;

        // ---- parameters, once per block --------------------------------------------------
        Shared sh;
        sh.decay = get(kDecay);
        sh.bright = get(kBright) * 0.01f;
        sh.width = get(kWidth) * 0.01f;
        sh.tune = std::pow(2.0f, get(kTune) / 1200.0f);
        sh.shimmer = get(kShimmer) * 0.01f * kShimmerMax;
        const bool frozen = getBool(kFreeze);
        const float hits = get(kHits) * 0.01f;
        const float drive = get(kDrive) * 0.01f;
        const float duck = get(kDuck) * 0.01f;
        const float mix = get(kMix) * 0.01f;
        const float tOut = mpc::dsp::dbToGain(get(kOutput));
        const float tDry = mix < 0.5f ? 1.0f : 2.0f * (1.0f - mix);
        const float tWet = (mix > 0.5f ? 1.0f : 2.0f * mix) * kWetMakeup;
        const float hpfHz = get(kHpf);
        if (hpfHz != hpfHz_)
        {
            hpfHz_ = hpfHz;
            hpf_.setCoeffs(mpc::dsp::BiquadCoeffs::highpass(sr_, mpc::clamp(hpfHz, 10.0f, 0.4f * (float)sr_)));
        }
        const float driveGain = 1.0f + 15.0f * drive * drive;
        const float driveMakeup = 1.0f / std::sqrt(driveGain);
        // Bright also shades what excites the resonators: a one-pole low-pass from ~1.2 kHz (dark)
        // up to fully open.
        const float toneHz = 1200.0f * std::pow(2.0f, 4.2f * sh.bright);
        const float toneCoef = toneHz > 16000.0f ? 1.0f : 1.0f - std::exp(-kTwoPi * toneHz / (float)sr_);
        const int source = getInt(kSource);
        const bool listening = source == kSrcListen || source == kSrcListenKey;

        // ---- which chord, and does it need a new bank ---------------------------------------
        const Chord chord = targetChord(ctx, n, source);
        const int material = mpc::clamp(getInt(kMaterial), 0, kNumMaterials - 1);
        if (!frozen)
        {
            int notes[kMaxPitches];
            const int lowNote = kLowNotes[mpc::clamp(getInt(kLowNote), 0, 3)];
            const int count = voiceChord(chord, lowNote, mpc::clamp(getInt(kSpread), 1, 4), notes, kMaxPitches);
            if (!sameNotes(notes, count, material))
            {
                for (int i = 0; i < count; ++i) reqNotes_[i] = notes[i];
                reqN_ = count;
                reqMaterial_ = material;
                reqChord_ = chord;
                pending_ = true;
            }
        }
        if (!frozen) startPending(sh);   // a chord waiting when Freeze went on waits for it to go off
        if (active_ >= 0) banks_[active_].setFrozen(frozen);

        // ---- audio, in sub-blocks ----------------------------------------------------------
        const float* inL = (ctx.inputs && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* inR = (ctx.inputs && ctx.numInputs > 1) ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = numOut > 1 ? ctx.outputs[1] : nullptr;
        const float subSeconds = (float)kSub / (float)sr_;

        for (int pos = 0; pos < n; pos += kSub)
        {
            const int m = (n - pos) < kSub ? (n - pos) : kSub;
            float dl[kSub], dr[kSub], exc[kSub], dk[kSub], wl[kSub], wr[kSub];

            for (int s = 0; s < m; ++s)
            {
                const float l = inL ? inL[pos + s] : 0.0f;
                const float r = inR ? inR[pos + s] : l;
                dl[s] = l; dr[s] = r;
                const float mono = 0.5f * (l + r);
                if (listening) harm_.push(mono);

                const float a = std::fabs(mono);
                if (a > inEnv_) inEnv_ = a;
                else if (!frozen) inEnv_ *= inEnvRel_;

                float x = hpf_.process(mono);
                // Transient detector: two averages of the rectified excitation, a fast one (two 1.5 ms
                // poles, so the ripple of a held note's waveform is smoothed away) and a slow one
                // (40 ms). On a held note they agree; while a hit is starting the fast one is far above.
                const float ax = std::fabs(x);
                fast1_ += fastAtt_ * (ax - fast1_);
                fastEnv_ += fastAtt_ * (fast1_ - fastEnv_);
                slowEnv_ += slowAtt_ * (ax - slowEnv_);
                float t = (fastEnv_ - slowEnv_) / (fastEnv_ + 1e-5f);
                t = mpc::clamp((t - 0.25f) * 2.2f, 0.0f, 1.0f);
                hitSm_ += hitSmooth_ * (t - hitSm_);

                if (drive > 0.0f) x = mpc::dsp::fastTanh(x * driveGain) * driveMakeup;
                x *= 1.0f - hits + hits * hitSm_;
                toneZ_ += toneCoef * (x - toneZ_);
                exc[s] = toneZ_;

                duckEnv_ = hitSm_ > duckEnv_ ? duckEnv_ + duckAtt_ * (hitSm_ - duckEnv_) : duckEnv_ * duckRel_;
                dk[s] = 1.0f - 0.9f * duck * duckEnv_;
                wl[s] = 0.0f; wr[s] = 0.0f;
            }

            for (Bank& b : banks_)
            {
                if (b.state() == Bank::kIdle) continue;
                b.update(sh, subSeconds);
                b.process(exc, m, wl, wr);
            }

            // Level guard: the ringing may not get more than ~3 dB above the input's recent peak
            // (a held note right on a resonator's pitch with a long decay would otherwise boom).
            float pk = 0.0f;
            for (int s = 0; s < m; ++s)
            {
                const float a = std::fabs(wl[s]) > std::fabs(wr[s]) ? std::fabs(wl[s]) : std::fabs(wr[s]);
                if (a > pk) pk = a;
            }
            const float ceiling = mpc::clamp(1.4f * inEnv_, 0.1f, 0.8f);
            const float want = pk > ceiling ? ceiling / pk : 1.0f;
            const float g0 = guard_;
            const float g1 = want < guard_ ? want : guard_ + guardRelSub_ * (want - guard_);
            guard_ = g1;
            const float gStep = (g1 - g0) / (float)m;

            for (int s = 0; s < m; ++s)
            {
                outGain_ += gainSmooth_ * (tOut - outGain_);
                dryGain_ += gainSmooth_ * (tDry - dryGain_);
                wetGain_ += gainSmooth_ * (tWet - wetGain_);
                const float gw = (g0 + gStep * (float)(s + 1)) * dk[s] * wetGain_;
                const float l = softLimit(wl[s] * gw);
                const float r = softLimit(wr[s] * gw);
                outL[pos + s] = (dl[s] * dryGain_ + l) * outGain_;
                if (outR) outR[pos + s] = (dr[s] * dryGain_ + r) * outGain_;
            }
        }

        if (listening) harm_.work();
        if (frozen) tunedBy_ = kByFrozen;
        updateMeters(n);
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
            case kDecay:
                std::snprintf(b, sizeof(b), v < 1.0f ? "%.2f s" : "%.1f s", (double)v); out = b; return true;
            case kHpf:
                if (v < 1000.0f) std::snprintf(b, sizeof(b), "%.0f Hz", (double)v);
                else std::snprintf(b, sizeof(b), "%.2f kHz", (double)v * 0.001);
                out = b; return true;
            case kSpread:
                std::snprintf(b, sizeof(b), "%d oct", (int)std::lround(v)); out = b; return true;
            case kTune:
                std::snprintf(b, sizeof(b), "%+d ct", (int)std::lround(v)); out = b; return true;
            case kMix: case kBright: case kWidth: case kHits: case kDrive: case kDuck: case kShimmer: case kSure:
                std::snprintf(b, sizeof(b), "%d%%", (int)std::lround(v)); out = b; return true;
            default: return false;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Conch";
        info.vendor       = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;            // 1.0.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 44100 * 20;      // a 20 s decay rings on after the input stops
        return info;
    }
    static constexpr char kUid[5] = "GHiy";

    static float softLimit(float x)
    {
        // Transparent below 0.8, then bends smoothly toward 1.0.
        const float a = std::fabs(x);
        if (a <= 0.8f) return x;
        const float y = 0.8f + 0.2f * mpc::dsp::fastTanh((a - 0.8f) * 5.0f);
        return x < 0.0f ? -y : y;
    }

    bool sameNotes(const int* notes, int count, int material) const
    {
        if (count != reqN_ || material != reqMaterial_) return false;
        for (int i = 0; i < count; ++i) if (notes[i] != reqNotes_[i]) return false;
        return true;
    }

    // Start the requested chord in a free bank; make room if all three are busy.
    void startPending(const Shared& sh)
    {
        if (!pending_) return;
        int idle = -1;
        bool killing = false;
        for (int b = 0; b < 3; ++b)
        {
            if (banks_[b].state() == Bank::kIdle && idle < 0) idle = b;
            if (banks_[b].state() == Bank::kKilling) killing = true;
        }
        if (idle < 0)
        {
            if (killing) return;          // one is already making room
            int quiet = -1;
            for (int b = 0; b < 3; ++b)
                if (b != active_ && (quiet < 0 || banks_[b].peak() < banks_[quiet].peak())) quiet = b;
            if (quiet >= 0) banks_[quiet].kill();
            return;
        }
        if (active_ >= 0) banks_[active_].release(kRingOut);
        banks_[idle].start(reqNotes_, reqN_, reqMaterial_, kMaterialTrim[reqMaterial_], sh);
        active_ = idle;
        activeChord_ = reqChord_;
        pending_ = false;
    }

    // ------------------------------------------------------------------------------------------
    // Which chord to ring this block.
    // ------------------------------------------------------------------------------------------
    Chord targetChord(const mpc::ProcessContext& ctx, int n, int source)
    {
        const int userKey = mpc::clamp(getInt(kKey), 0, 11);
        const int userScale = mpc::clamp(getInt(kScale), 0, kNumScales - 1);
        const int tones = mpc::clamp(getInt(kTones), 0, kNumTones - 1);

        if (source == kSrcListen || source == kSrcListenKey)
        {
            followKey((float)n / (float)sr_);
            int root = userKey, scale = userScale;
            if (haveFound_)
            {
                root = foundRoot_;
                // The Scale knob flavours a heard key (Dorian for a minor key...) when it is of the
                // same kind; otherwise plain minor or major.
                scale = (kScales[userScale].minorish == foundMinor_) ? userScale : (foundMinor_ ? 0 : 1);
                tunedBy_ = kByHeard;
            }
            else tunedBy_ = kByUnsure;
            harm_.setKey(root, scale);
            const int degree = (source == kSrcListen && haveFound_ && harm_.chordValid()) ? harm_.chordDegree() : 0;
            return diatonicChord(root, scale, degree, tones);
        }
        if (source == kSrcChord)
        {
            tunedBy_ = kByChord;
            return chordFromQuality(getInt(kChordRoot), getInt(kQuality));
        }
        if (source == kSrcProg)
        {
            tunedBy_ = kByProg;
            const mpc::Transport& tr = ctx.transport;
            if (tr.valid && tr.playing && tr.ppqValid)
            {
                const double bpm = (tr.tempoValid && tr.bpm > 1.0 && tr.bpm < 999.0) ? tr.bpm : 120.0;
                const double spb = sr_ * 60.0 / bpm;
                const int num = tr.timeSigNumerator > 0 ? tr.timeSigNumerator : 4;
                const int den = tr.timeSigDenominator > 0 ? tr.timeSigDenominator : 4;
                const double beatsPerBar = (double)num * 4.0 / (double)den;
                const double chordBeats = kChordBars[mpc::clamp(getInt(kChordLen), 0, 3)] * beatsPerBar;
                // Judge by the end of the block, so a hit on the downbeat already rings the new chord.
                const double ppq = tr.ppqPos + (double)n / spb;
                progIdx_ = modi((long long)std::floor(ppq / chordBeats + 1e-6), 4);
            }
            // Transport stopped: stay on the chord it was on.
            const int prog = mpc::clamp(getInt(kProg), 0, kNumProgs - 1);
            return diatonicChord(userKey, userScale, kProgs[prog][progIdx_], tones);
        }
        tunedBy_ = kByKey;
        return diatonicChord(userKey, userScale, 0, tones);
    }

    // Adopt a heard key once it is sure; switch to a different one only after hearing it for kKeyHold.
    void followKey(float blockSeconds)
    {
        // Drums alone can make a confident-looking key (a kick's pitch, a snare's ring); real harmony
        // also makes the input tonal. Both must hold.
        if (harm_.confidence() < kKeySure || harm_.tonality() < kTonalSure) { candTime_ = 0.0f; return; }
        const int r = harm_.foundRoot();
        const bool mn = harm_.foundMinor();
        if (!haveFound_) { haveFound_ = true; foundRoot_ = r; foundMinor_ = mn; return; }
        if (r == foundRoot_ && mn == foundMinor_) { candTime_ = 0.0f; return; }
        if (r != candRoot_ || mn != candMinor_) { candRoot_ = r; candMinor_ = mn; candTime_ = 0.0f; }
        candTime_ += blockSeconds;
        if (candTime_ >= kKeyHold) { foundRoot_ = r; foundMinor_ = mn; candTime_ = 0.0f; }
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
        const bool listening = getInt(kSource) <= kSrcListenKey;
        float found = 0.0f;
        if (listening && haveFound_) found = (float)(1 + foundRoot_ + (foundMinor_ ? 12 : 0));
        sendMeter(0, kFoundKey, found / 24.0f);
        sendMeter(1, kSure, listening ? std::round(conf * 100.0f) * 0.01f : 0.0f);
        const Chord& c = active_ >= 0 ? activeChord_ : reqChord_;
        const float idx = (float)(1 + c.root * kNumQualities + qualityOf(c));
        sendMeter(2, kChordNow, idx / (float)(12 * kNumQualities));
        sendMeter(3, kTunedBy, (float)tunedBy_ / 5.0f);
    }

    void sendMeter(int slot, int param, float norm)
    {
        if (norm == lastMeter_[slot]) return;
        lastMeter_[slot] = norm;
        setNormalised(param, norm);
        notifyHostParameterChanged(param);
    }

    // ------------------------------------------------------------------------------------------
    void addPrograms()
    {
        auto prog = [this](const char* name, std::initializer_list<std::pair<int, float>> changes)
        {
            std::vector<float> v((size_t)kNumParams);
            for (int i = 0; i < kNumParams; ++i) v[(size_t)i] = param(i).def;
            for (const auto& c : changes) v[(size_t)c.first] = c.second;
            addProgram(name, v);
        };
        prog("Drum Strings", {{kSource, kSrcKey}});
        prog("Glass Kit", {{kMaterial, kBell}, {kSource, kSrcKey}, {kDecay, 3.0f}, {kBright, 75}, {kSpread, 3},
                           {kLowNote, 2}, {kTones, kNinth}, {kHits, 75}, {kHpf, 150}, {kMix, 50}, {kWidth, 85},
                           {kShimmer, 15}});
        prog("Hollow Shell", {{kMaterial, kPipe}, {kSource, kSrcKey}, {kDecay, 1.8f}, {kBright, 40}, {kSpread, 2},
                              {kLowNote, 1}, {kHits, 60}, {kMix, 50}, {kShimmer, 25}, {kWidth, 70}});
        prog("Mix Halo", {{kSource, kSrcListen}, {kDecay, 4.0f}, {kBright, 45}, {kSpread, 3}, {kLowNote, 2},
                          {kTones, kSeventh}, {kHits, 20}, {kHpf, 200}, {kMix, 22}, {kWidth, 85}, {kDuck, 0},
                          {kShimmer, 30}});
        prog("Dub Pipes", {{kMaterial, kPipe}, {kSource, kSrcProg}, {kProg, 0}, {kChordLen, 2}, {kDecay, 3.0f},
                           {kBright, 35}, {kSpread, 2}, {kLowNote, 1}, {kTones, kFifths}, {kHits, 80},
                           {kDrive, 30}, {kDuck, 60}, {kMix, 45}});
        prog("Frozen Chord", {{kSource, kSrcChord}, {kChordRoot, 9}, {kQuality, 12}, {kDecay, 12.0f},
                              {kBright, 40}, {kSpread, 3}, {kLowNote, 1}, {kHits, 30}, {kMix, 40},
                              {kShimmer, 35}, {kWidth, 90}, {kDuck, 0}});
        prog("Gamelan Hits", {{kMaterial, kBell}, {kSource, kSrcKey}, {kDecay, 2.2f}, {kBright, 80}, {kSpread, 3},
                              {kLowNote, 2}, {kTones, kFifths}, {kHits, 80}, {kHpf, 120}, {kMix, 50},
                              {kWidth, 100}, {kShimmer, 15}});
        prog("Listening Strings", {{kSource, kSrcListen}});
    }

    // ---- state ---------------------------------------------------------------------------------
    double sr_ = 44100.0;
    Harmony harm_;
    Bank banks_[3];
    std::vector<float> lines_;
    int lineLen_ = 4096;
    int active_ = -1;

    // requested notes
    bool pending_ = false;
    int reqNotes_[kMaxPitches] = {};
    int reqN_ = -1, reqMaterial_ = -1;
    Chord reqChord_, activeChord_;

    // key following
    bool haveFound_ = false, foundMinor_ = true, candMinor_ = false;
    int foundRoot_ = 9, candRoot_ = -1;
    float candTime_ = 0.0f;
    int progIdx_ = 0;
    int tunedBy_ = kByKey;

    // excitation
    mpc::dsp::Biquad hpf_;
    float hpfHz_ = -1.0f;
    float toneZ_ = 0.0f, fast1_ = 0.0f, fastEnv_ = 0.0f, slowEnv_ = 0.0f, hitSm_ = 0.0f, duckEnv_ = 0.0f, inEnv_ = 0.0f;
    float fastAtt_ = 0.01f, slowAtt_ = 0.001f, hitSmooth_ = 0.05f;
    float duckAtt_ = 0.02f, duckRel_ = 0.999f, inEnvRel_ = 0.9999f;

    // output
    float guard_ = 1.0f, guardRelSub_ = 0.01f, gainSmooth_ = 0.001f;
    float outGain_ = 1.0f, dryGain_ = 1.0f, wetGain_ = 1.0f;

    // meters
    int meterCountdown_ = 0, meterInterval_ = 4410;
    float lastMeter_[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(Conch)
