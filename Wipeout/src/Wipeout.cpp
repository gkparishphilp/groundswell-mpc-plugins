// Wipeout - a tempo-locked stutter / beat-repeat effect by Groundswell
//
// Wipeout cuts the track's audio up in time with the song: on a grid it rolls the dice and, when the
// dice say so, grabs a slice of what is playing and repeats it, plays it backwards or at half speed,
// stops it like a tape machine losing power, or chops it with a gate. Built for playing live on the
// Q-Links: the chances are knobs, Hold is a switch that stutters for as long as it is on.
//
// Signal flow
// -----------
//   input ──┬──────────────────────────────────────────── dry ──┐
//           ├─> history ring (10 s; the Tape Stop reads it)      ├─> Mix ─> Output
//           └─> event slots (2) ──────────────────────── wet ────┘
//
//   * Clock. With the transport playing, beats come from the host's song position (ppq), so events land
//     exactly on the sequence's grid and follow loops. With the transport stopped, a free-running clock
//     at the host tempo (120 BPM if none) takes over, and it restarts on the first hit after a quiet
//     spell, so a pad played on its own starts the grid.
//   * Rolls. Every Grid (moved later by Offset), Wipeout draws from its random generator: one draw for
//     Chance (does an event happen?) and one each for the event's type and options. An event lasts
//     Length. While one is running, the rolls that fall inside it are skipped.
//   * Event types, in order of precedence: Tape Stop (chance) → Gate (chance) → Repeat.
//       Repeat:    the first slice plays through as it is captured into the slot's own buffer, then the
//                  slice repeats until the event ends. Each repeat after the first may play reversed
//                  (Reverse chance). Half Speed (chance per event) plays half the slice at half speed
//                  (an octave down) in the same time, so repeats stay on the grid.
//       Gate:      the live signal is switched on for the first half of each slice and off for the
//                  second half.
//       Tape Stop: the history ring is played back at a speed that falls from 1 to 0 over the event
//                  (pitch and speed sink together), fading out as it stops.
//     Roll (chance per Repeat/Gate event) halves the slice after the first third of the event and again
//     after the second third: 1/8 → 1/16 → 1/32, the classic build-up. Vary (chance per event) picks a
//     random slice size instead of the Slice setting. Decay makes each repeat quieter than the last.
//   * Hold (switch) starts a Repeat on the next Slice boundary and keeps it going until it is switched
//     off; it overrides the dice. Its reverse draws use a separate generator so they do not disturb a
//     locked pattern.
//   * Pattern. "Free" draws new dice forever. Choosing 1, 2, 4 or 8 Bars freezes the pattern of the last
//     1, 2, 4 or 8 complete bars and replays it from the next bar on (the generator is reseeded at the
//     start of each cycle). Changing other parameters while locked changes what the dice mean, not the
//     dice themselves.
//   * Scenes A-D (1.1). Each scene is a snapshot of the controls you play (Chance, Slice, Length, Mix,
//     the event chances, Decay, Grid, Offset, Pattern; not Output, Hold or Smooth). Turning a knob edits
//     the scene in play; switching scenes puts the other scene's values on the knobs. Every program comes
//     with four scenes. Mix is ramped over a block so a scene change mid-event does not click.
//   * Clicks. Every slice is windowed with a short smoothstep fade (Smooth, ms) at its edges, and every
//     event fades in and out against the dry signal over the same time. Two slots let a new event fade
//     in while the old one fades out.
//
// Realtime: all buffers are allocated in prepare(); process() does no allocation, locking or I/O, and
// no transcendental functions per sample (a few multiplies, a linear interpolation, a smoothstep).

#include "mpc_plugin.h"
#include "dsp/smoother.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

// Test hooks (tests/listentest.cpp defines these before including this file).
#ifndef WIPEOUT_EVENT_HOOK
#define WIPEOUT_EVENT_HOOK(offset, type, sliceBeats, half, roll, hold)
#endif
#ifndef WIPEOUT_REP_HOOK
#define WIPEOUT_REP_HOOK(offset, repIndex, reversed, repLenSamples)
#endif

namespace {

// Parameter order and ids are permanent: append new parameters at the end.
enum ParamIndex
{
    kOutput = 0,   // dB: output level (parameter 0 is nudged by the host on every load; harmless here)
    kChance,       // %: chance that an event starts at each grid point
    kSlice,        // choice: size of the repeated slice (1/4 ... 1/32T)
    kLength,       // choice: how long an event lasts (1/8, 1/4, 1/2, 1, 2 or 4 bars)
    kMix,          // %: level of the effect during an event (100 = the dry signal is replaced)
    kReverse,      // %: chance that each repeat after the first plays backwards
    kHalf,         // %: chance that a Repeat event plays at half speed (an octave down)
    kTape,         // %: chance that an event is a Tape Stop
    kGate,         // %: chance that an event is a Gate (live audio chopped on/off per slice)
    kVary,         // %: chance that an event uses a random slice size
    kRoll,         // %: chance that the slice halves as the event goes on (1/8 → 1/16 → 1/32)
    kDecay,        // %: each repeat quieter than the last
    kHold,         // switch: repeat now (from the next slice boundary) until switched off
    kGrid,         // choice: how often the dice are rolled (1/4, 1/2, 1, 2, 4 or 8 bars)
    kOffset,       // 16ths (0..127, shown bar.beat.16th): moves the roll points later inside the grid; wraps at the grid
                   //   (12 = 1.4.1 = beat 4; with an 8-bar grid, 96 = 7.1.1 = the last two bars)
    kSmooth,       // ms: fade at every slice edge and event edge
    kPattern,      // choice: Free, or lock the last 1/2/4/8 bars of dice and replay them
    kScene,        // choice: A-D, the scene in play (switching recalls its settings; turning a knob edits it)
    kNumParams
};

enum EventType { kTypeRepeat = 0, kTypeTape = 1, kTypeGate = 2 };

// What a scene holds: every control you play, but not Output, Hold, Smooth or the Scene switch itself.
constexpr int kSceneParams[] = {kChance, kSlice, kLength, kMix, kReverse, kHalf, kTape, kGate,
                                kVary, kRoll, kDecay, kGrid, kOffset, kPattern};
constexpr int kNumScenes = 4;
inline bool inScene(int i) { for (int k : kSceneParams) if (k == i) return true; return false; }

constexpr int    kNumSlices = 8;
constexpr double kSliceBeats[kNumSlices] = {1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0};

constexpr double kMinBpm = 40.0, kMaxBpm = 300.0;
constexpr double kRingSeconds = 10.0;                 // Tape Stop history (a stop lags by up to half its length)
constexpr double kMinRepBeats = 0.0625;               // Roll never goes below a 1/64 note

inline float smoothstep01(float x)
{
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return x * x * (3.0f - 2.0f * x);
}

// xorshift32 with visible state, so Pattern can save and restore it.
struct Rng
{
    uint32_t s = 0x2545F491u;
    float next() { uint32_t x = s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s = x; return (float)(x >> 8) * (1.0f / 16777216.0f); }
};

struct Slot
{
    bool   active = false, releasing = false, hold = false;
    int    type = kTypeRepeat;
    double startBeat = 0.0, endBeat = 0.0, lengthBeats = 1.0;
    long long t = 0;              // samples since the event started
    float  ev = 0.0f, evStep = 0.0f, fade = 1.0f;

    // Repeat / Gate
    double sliceSamps = 1.0, minRep = 1.0;
    bool   half = false, roll = false;
    int    repIndex = 0;
    double repStart = 0.0, repLen = 1.0;   // exact (fractional) start and length, for the grid
    long long repT0 = 0;                    // first whole sample of the repetition: reads start here, so repeats are exact copies
    float  repFade = 1.0f;
    bool   repRev = false;
    float  repGain = 1.0f, decayMul = 1.0f;
    int    capLen = 0, capTarget = 0;
    Rng*   rng = nullptr;

    // Tape Stop
    double tapeLen = 1.0, readAbs = 0.0;
};

} // namespace

class Wipeout : public mpc::Plugin
{
public:
    Wipeout() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("output",  "Output",     -24.0f, 6.0f,   0.0f,  "dB", 1));
        addParam(mpc::floatParam ("chance",  "Chance",       0.0f, 100.0f, 50.0f, "%",  0));
        addParam(mpc::choiceParam("slice",   "Slice",      {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"}, 4));
        addParam(mpc::choiceParam("length",  "Length",     {"1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars"}, 1));
        addParam(mpc::floatParam ("mix",     "Mix",          0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::floatParam ("reverse", "Reverse",      0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(mpc::floatParam ("half",    "Half Speed",   0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(mpc::floatParam ("tape",    "Tape Stop",    0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(mpc::floatParam ("gate",    "Gate",         0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(mpc::floatParam ("vary",    "Vary",         0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(mpc::floatParam ("roll",    "Roll",         0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(mpc::floatParam ("decay",   "Decay",        0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(mpc::boolParam  ("hold",    "Hold",       false));
        addParam(mpc::choiceParam("grid",    "Grid",       {"1/4", "1/2", "1 Bar", "2 Bars", "4 Bars", "8 Bars"}, 2));
        addParam(mpc::intParam   ("offset",  "Offset",       0, 127, 12));
        addParam(mpc::logParam   ("smooth",  "Smooth",       0.5f, 20.0f, 3.0f, "ms", 1));
        addParam(mpc::choiceParam("pattern", "Pattern",    {"Free", "1 Bar", "2 Bars", "4 Bars", "8 Bars"}, 0));
        addParam(mpc::choiceParam("scene",   "Scene",      {"A", "B", "C", "D"}, 0));      // 1.1

        // Programs: the values are scene A; B, C and D change only what is listed. The Scene switch is not
        // part of a program (a program starts on A).
        //                                 Out Chnc Slc Len  Mix  Rev Half Tape Gate Vary Roll Dcy Hold Grid Off Smth Pat
        addSceneProgram("Bar Fills",    {0,  50,  4,  1,  100,  0,   0,   0,   0,   0,   0,   0,  0,   2,  12, 3,   0},
            {{kLength, 2}, {kRoll, 50}, {kOffset, 8}},                                     // B: half-bar fill from beat 3, rolling
            {{kChance, 80}, {kSlice, 2}, {kLength, 2}, {kRoll, 100}, {kGrid, 3}, {kOffset, 24}},   // C: a roll-up every 2 bars
            {{kChance, 100}, {kLength, 2}, {kTape, 100}, {kGrid, 4}, {kOffset, 56}});      // D: tape stop at the end of 4 bars
        addSceneProgram("Glitch Storm", {0,  60,  6,  0,  100,  40,  25,  0,   20,  60,  0,   0,  0,   0,  0,  2,   0},
            {{kChance, 30}, {kVary, 30}},                                                  // B: sparser
            {{kChance, 90}, {kReverse, 60}, {kDecay, 40}},                                 // C: nearly constant
            {{kSlice, 5}, {kHalf, 50}, {kRoll, 60}, {kMix, 80}});                          // D: triplets, lower, rolling
        addSceneProgram("Tape Breaks",  {0,  45,  4,  2,  100,  0,   0,   80,  0,   0,   0,   0,  0,   2,  8,  3,   0},
            {{kTape, 40}, {kRoll, 40}},                                                    // B: tape stops and rolls
            {{kChance, 70}, {kLength, 3}, {kGrid, 3}, {kOffset, 16}},                      // C: a whole-bar stop on bar 2
            {{kChance, 100}, {kTape, 100}, {kLength, 4}, {kGrid, 5}, {kOffset, 96}});      // D: a 2-bar stop ending 8 bars
        addSceneProgram("Snare Rolls",  {0,  70,  2,  1,  100,  0,   0,   0,   0,   0,   100, 0,  0,   2,  12, 3,   0},
            {{kLength, 2}, {kOffset, 8}},                                                  // B: half-bar roll
            {{kChance, 100}, {kLength, 3}, {kGrid, 3}, {kOffset, 16}},                     // C: a bar of roll every 2 bars
            {{kChance, 100}, {kLength, 3}, {kDecay, 30}, {kGrid, 4}, {kOffset, 48}});      // D: bar 4 of every 4 rolls
        addSceneProgram("Reverse Wash", {0,  40,  2,  1,  80,   80,  30,  0,   0,   20,  0,   30, 0,   1,  0,  6,   0},
            {{kChance, 70}},                                                               // B: more often
            {{kReverse, 100}, {kHalf, 70}},                                                // C: all backwards, mostly low
            {{kLength, 2}, {kDecay, 60}, {kMix, 60}});                                     // D: long, fading tails
        addSceneProgram("Trance Gate",  {0,  50,  5,  2,  100,  0,   0,   0,   100, 0,   30,  0,  0,   1,  4,  2,   0},
            {{kChance, 100}, {kRoll, 0}},                                                  // B: gate all the time
            {{kSlice, 6}, {kRoll, 60}},                                                    // C: faster chops
            {{kChance, 100}, {kGate, 0}, {kTape, 100}, {kGrid, 4}, {kOffset, 56}});        // D: breakdown tape stop
        addSceneProgram("Hold To Play", {0,  0,   4,  1,  100,  25,  0,   0,   0,   0,   0,   0,  0,   2,  0,  3,   0},
            {{kSlice, 2}},                                                                 // B: 1/8 repeats
            {{kSlice, 6}, {kReverse, 50}},                                                 // C: 1/32 buzz
            {{kSlice, 3}, {kDecay, 40}});                                                  // D: 1/8T, fading
        addSceneProgram("Transitions",  {0,  100, 2,  4,  100,  0,   0,   0,   0,   0,   100, 0,  0,   5,  96, 3,   0},
            {{kLength, 3}, {kOffset, 112}},                                                // B: a 1-bar roll on bar 8
            {{kLength, 3}, {kOffset, 112}, {kRoll, 0}, {kTape, 100}},                      // C: a 1-bar tape stop on bar 8
            {{kLength, 5}, {kOffset, 64}});                                                // D: a 4-bar build from bar 5

        loadBank(programScenes_[0]);   // the defaults above are Bar Fills, scene A
    }

    // ---- scenes ---------------------------------------------------------------------------------------
    // Four snapshots of the controls in kSceneParams. The knobs always show the scene in play: turning one
    // edits that scene, and switching scenes writes the new scene's values to the knobs (and tells the host,
    // so the screen follows). Choosing a program loads its four scenes and starts on A. Scenes are saved
    // with the project. None of this runs on the audio thread's hot path: process() only reads the knobs.
    void parameterChanged(int index, float plain) override
    {
        if (recalling_.load()) return;                    // our own writes while recalling a scene
        // setProgram() sets the program number before it writes the parameters, so the first write of a
        // program change arrives here with a new number: load that program's scenes and start on A.
        const int prog = currentProgram();
        if (prog != seenProgram_)
        {
            seenProgram_ = prog;
            if (prog >= 0 && prog < (int)programScenes_.size())
            {
                loadBank(programScenes_[(size_t)prog]);
                scene_.store(0);
                recalling_.store(true); set(kScene, 0.0f); recalling_.store(false);
            }
        }
        if (index == kScene)
        {
            const int s = mpc::clamp((int)std::lround(plain), 0, kNumScenes - 1);
            if (s != scene_.load()) { scene_.store(s); recallScene(s); }
            return;
        }
        if (inScene(index)) scenes_[scene_.load()][index].store(getNormalised(index));   // normalised: recalls are exact
    }

    // "WSC1" | u32 numParams | u32 scene | numScenes x numParams f32 (normalised 0..1 values by parameter index)
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        auto putU32 = [&out](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i))); };
        out.push_back('W'); out.push_back('S'); out.push_back('C'); out.push_back('1');
        putU32((uint32_t)kNumParams);
        putU32((uint32_t)scene_.load());
        for (int s = 0; s < kNumScenes; ++s)
            for (int i = 0; i < kNumParams; ++i)
            {
                const float f = scenes_[s][i].load(); uint32_t v; std::memcpy(&v, &f, 4); putU32(v);
            }
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        if (data == nullptr || size < 12 || std::memcmp(data, "WSC1", 4) != 0) return;
        auto u32At = [data](size_t pos) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (uint32_t)data[pos + (size_t)i] << (8 * i); return v; };
        const uint32_t n = u32At(4);
        const int cur = mpc::clamp((int)u32At(8), 0, kNumScenes - 1);
        if (n == 0 || n > 1024 || size < 12 + (size_t)kNumScenes * n * 4) return;
        for (int s = 0; s < kNumScenes; ++s)
            for (uint32_t i = 0; i < n; ++i)
            {
                if ((int)i >= kNumParams || !inScene((int)i)) continue;
                const uint32_t v = u32At(12 + ((size_t)s * n + i) * 4); float f; std::memcpy(&f, &v, 4);
                if (std::isfinite(f)) scenes_[s][i].store(mpc::clamp(f, 0.0f, 1.0f));
            }
        scene_.store(cur);
        if (getInt(kScene) != cur) { recalling_.store(true); set(kScene, (float)cur); recalling_.store(false); }
        recallScene(cur);
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = sampleRate > 1000.0 ? sampleRate : 44100.0;
        ringLen_ = (int)(kRingSeconds * sr_) + 8;
        capMax_ = (int)(60.0 / kMinBpm * sr_) + 8;          // one beat at the slowest tempo
        for (int c = 0; c < 2; ++c) ring_[c].assign((size_t)ringLen_, 0.0f);
        for (int s = 0; s < 2; ++s)
            for (int c = 0; c < 2; ++c) cap_[s][c].assign((size_t)capMax_, 0.0f);
        envRel_ = (float)std::exp(-1.0 / (0.05 * sr_));
        reset();
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c) std::fill(ring_[c].begin(), ring_[c].end(), 0.0f);
        writeAbs_ = 0;
        for (Slot& s : slots_) s = Slot();
        cur_ = 0;
        beat_ = 0.0;
        wasPlaying_ = false;
        holdPending_ = false;
        patRng_.s = 0x2545F491u;
        holdRng_.s = 0x9E3779B9u;
        for (int i = 0; i < 16; ++i) seedRing_[i] = patRng_.s;
        lockActive_ = false; lockN_ = 0; lockBar_ = 0; lockSeed_ = patRng_.s;
        envF_ = 0.0f; quietSamps_ = 0;
        lastGrid_ = -1; lastOffset_ = -1; lastBarBeats_ = -1.0;
        out_.snap(dbToGain(get(kOutput)));
        mix_.snap(get(kMix) * 0.01f);
        needResync_ = true;
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index == kOffset)
        {
            const int o = (int)std::lround(plainValue);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d.%d.%d", o / 16 + 1, (o / 4) % 4 + 1, o % 4 + 1);   // bar.beat.16th
            out = buf;
            return true;
        }
        return false;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (n <= 0 || ctx.numOutputs < 1 || ring_[0].empty()) return;

        // ---- clock ------------------------------------------------------------------------------
        const mpc::Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        double bpm = (tr.valid && tr.tempoValid) ? tr.bpm : 120.0;
        bpm = bpm < kMinBpm ? kMinBpm : (bpm > kMaxBpm ? kMaxBpm : bpm);
        const double spb = sr_ * 60.0 / bpm;                 // samples per beat (quarter note)
        const double bps = 1.0 / spb;
        double barBeats = 4.0;
        if (tr.valid && tr.timeSigNumerator > 0 && tr.timeSigDenominator > 0)
            barBeats = mpc::clamp((double)tr.timeSigNumerator * 4.0 / (double)tr.timeSigDenominator, 1.0, 16.0);

        if (playing)
        {
            const double b = tr.ppqPos;
            if (!wasPlaying_ || std::fabs(b - beat_) > 0.05) { beat_ = b; needResync_ = true; }
            else beat_ = b;
        }
        else if (wasPlaying_) needResync_ = true;             // transport stopped: free clock from here
        wasPlaying_ = playing;

        // ---- parameters, once per block -----------------------------------------------------------
        const int   gridIdx  = getInt(kGrid);
        const double gridBeats = gridIdx == 0 ? 1.0 : gridIdx == 1 ? 2.0 : barBeats * (double)(1 << mpc::clamp(gridIdx - 2, 0, 3));
        const int   offset16 = getInt(kOffset);
        if (gridIdx != lastGrid_ || offset16 != lastOffset_ || barBeats != lastBarBeats_) needResync_ = true;
        lastGrid_ = gridIdx; lastOffset_ = offset16; lastBarBeats_ = barBeats;
        const double offBeats = std::fmod(offset16 * 0.25, gridBeats);

        const float chance  = get(kChance) * 0.01f;
        const int   sliceIdx = mpc::clamp(getInt(kSlice), 0, kNumSlices - 1);
        const int   lenIdx  = getInt(kLength);
        const double lenBeats = lenIdx == 0 ? 0.5 : lenIdx == 1 ? 1.0 : lenIdx == 2 ? 2.0 : barBeats * (double)(1 << mpc::clamp(lenIdx - 3, 0, 2));
        revChance_          = get(kReverse) * 0.01f;
        const float halfCh  = get(kHalf) * 0.01f;
        const float tapeCh  = get(kTape) * 0.01f;
        const float gateCh  = get(kGate) * 0.01f;
        const float varyCh  = get(kVary) * 0.01f;
        const float rollCh  = get(kRoll) * 0.01f;
        const float decayMul = 1.0f - 0.6f * get(kDecay) * 0.01f;
        const bool  holdOn  = getBool(kHold);
        const float fade    = (float)mpc::clamp(get(kSmooth) * 0.001 * sr_, 8.0, 0.05 * sr_);
        const int   patMode = getInt(kPattern);
        const int   patN    = patMode <= 0 ? 0 : 1 << mpc::clamp(patMode - 1, 0, 3);   // 0, 1, 2, 4, 8 bars
        out_.setTarget(dbToGain(get(kOutput)), n);
        mix_.setTarget(get(kMix) * 0.01f, n);               // ramped: a scene change can jump it mid-event

        if (needResync_) { resync(gridBeats, offBeats, barBeats); needResync_ = false; }

        const int nIn = ctx.inputs != nullptr ? ctx.numInputs : 0;
        const bool stereoOut = ctx.numOutputs >= 2;

        for (int i = 0; i < n; ++i)
        {
            const float inL = (nIn > 0 && ctx.inputs[0] != nullptr) ? ctx.inputs[0][i] : 0.0f;
            const float inR = (nIn > 1 && ctx.inputs[1] != nullptr) ? ctx.inputs[1][i] : inL;

            // history ring for Tape Stop
            const int w = (int)(writeAbs_ % ringLen_);
            ring_[0][(size_t)w] = inL; ring_[1][(size_t)w] = inR;

            // stopped transport: restart the free clock on the first hit after a quiet spell
            if (!playing)
            {
                const float a = std::fmax(std::fabs(inL), std::fabs(inR));
                envF_ = a > envF_ ? a : envF_ * envRel_;
                if (envF_ < 0.003f) ++quietSamps_;
                else if (envF_ > 0.02f)
                {
                    if (quietSamps_ > (long long)(0.25 * sr_))
                    {
                        beat_ = 0.0;
                        resync(gridBeats, offBeats, barBeats);
                    }
                    quietSamps_ = 0;
                }
            }

            Slot& c = slots_[cur_];

            // the running event ends on its grid line (Hold ends when switched off)
            if (c.active && !c.releasing && !c.hold && beat_ >= c.endBeat - 1e-9) release(c, fade);

            // bar lines: Pattern seeds
            if (beat_ >= nextBarBeat_ - 1e-9)
            {
                const long long bar = (long long)std::floor(nextBarBeat_ / barBeats + 0.5);
                nextBarBeat_ += barBeats;
                onBar(bar, patN);
            }

            // grid: roll the dice (always the same number of draws, so a locked pattern replays exactly)
            if (beat_ >= nextGridBeat_ - 1e-9)
            {
                nextGridBeat_ += gridBeats;
                const float rChance = patRng_.next(), rTape = patRng_.next(), rGate = patRng_.next();
                const float rVary = patRng_.next(), rPick = patRng_.next(), rRoll = patRng_.next(), rHalf = patRng_.next();
                const bool busy = slots_[cur_].active && !slots_[cur_].releasing;
                if (!busy && !holdOn && !holdPending_ && rChance < chance)
                {
                    const int type = rTape < tapeCh ? kTypeTape : (rGate < gateCh ? kTypeGate : kTypeRepeat);
                    int si = sliceIdx;
                    if (rVary < varyCh) si = mpc::clamp((int)(rPick * kNumSlices), 0, kNumSlices - 1);
                    double sb = kSliceBeats[si];
                    if (sb > lenBeats * 0.5) sb = lenBeats * 0.5;     // at least two slices per event
                    startEvent(i, type, sb, lenBeats, false, type == kTypeRepeat && rHalf < halfCh,
                               type != kTypeTape && rRoll < rollCh, spb, fade, decayMul, &patRng_);
                }
            }

            // Hold: start on the next slice boundary, stop when switched off
            {
                Slot& h = slots_[cur_];
                const bool holdRunning = h.active && h.hold && !h.releasing;
                const double sb = kSliceBeats[sliceIdx];
                if (holdOn)
                {
                    if (!holdRunning && !holdPending_)
                    {
                        holdPending_ = true;
                        holdStartBeat_ = std::ceil((beat_ - 1e-6) / sb) * sb;
                    }
                    if (holdPending_ && beat_ >= holdStartBeat_ - 1e-9)
                    {
                        holdPending_ = false;
                        startEvent(i, kTypeRepeat, sb, 1.0e9, true, false, false, spb, fade, decayMul, &holdRng_);
                    }
                }
                else
                {
                    holdPending_ = false;
                    if (holdRunning) release(h, fade);
                }
            }

            // run the slots
            float wetL = 0.0f, wetR = 0.0f, evSum = 0.0f;
            for (int s = 0; s < 2; ++s)
                if (slots_[s].active) runSlot(s, inL, inR, i, wetL, wetR, evSum);
            if (evSum > 1.0f) evSum = 1.0f;

            const float g = out_.next();
            const float mix = mix_.next();
            const float dryAmt = 1.0f - mix * evSum;
            const float oL = (inL * dryAmt + wetL * mix) * g;
            const float oR = (inR * dryAmt + wetR * mix) * g;
            ctx.outputs[0][i] = oL;
            if (stereoOut) ctx.outputs[1][i] = oR;

            ++writeAbs_;
            beat_ += bps;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Wipeout";
        info.vendor       = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1100;           // 1.1.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 0;              // repeats can outlast the input by up to a bar
        return info;
    }

    static constexpr char kUid[5] = "G0BG";

    static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }

    // ---- scenes ---------------------------------------------------------------------------------------
    using SceneValues = std::array<float, kNumParams>;
    using SceneBank   = std::array<SceneValues, kNumScenes>;
    using Changes     = std::initializer_list<std::pair<int, float>>;

    void addSceneProgram(const char* name, std::vector<float> a, Changes b, Changes c, Changes d)
    {
        addProgram(name, a);
        SceneBank bank{};
        const Changes* changes[kNumScenes] = {nullptr, &b, &c, &d};
        for (int s = 0; s < kNumScenes; ++s)
        {
            for (int i = 0; i < kNumParams; ++i) bank[(size_t)s][(size_t)i] = i < (int)a.size() ? a[(size_t)i] : 0.0f;
            if (changes[s] != nullptr) for (const auto& ch : *changes[s]) bank[(size_t)s][(size_t)ch.first] = ch.second;
        }
        programScenes_.push_back(bank);
    }

    void loadBank(const SceneBank& bank)
    {
        for (int s = 0; s < kNumScenes; ++s)
            for (int i = 0; i < kNumParams; ++i) scenes_[s][i].store(param(i).toNormalised(bank[(size_t)s][(size_t)i]));
    }

    // Write scene s to the knobs, and tell the host about each one that moved.
    void recallScene(int s)
    {
        bool moved = false;
        recalling_.store(true);
        for (int i : kSceneParams)
        {
            const float v = scenes_[s][i].load();
            if (getNormalised(i) == v) continue;
            setNormalised(i, v);
            notifyHostParameterChanged(i);
            moved = true;
        }
        recalling_.store(false);
        if (moved) requestDisplayUpdate();
    }

    std::vector<SceneBank> programScenes_;               // filled in the constructor, read-only after
    std::atomic<float> scenes_[kNumScenes][kNumParams];   // normalised values; only kSceneParams are used
    std::atomic<int>   scene_{0};
    std::atomic<bool>  recalling_{false};
    int                seenProgram_ = 0;

    // Recompute the next grid and bar lines from beat_, and let any running event fade out.
    void resync(double gridBeats, double offBeats, double barBeats)
    {
        nextGridBeat_ = std::ceil((beat_ - offBeats - 1e-6) / gridBeats) * gridBeats + offBeats;
        nextBarBeat_ = std::ceil((beat_ - 1e-6) / barBeats) * barBeats;
        holdPending_ = false;
        const float f = (float)(0.003 * sr_);
        for (Slot& s : slots_) if (s.active && !s.releasing) release(s, f);
    }

    void onBar(long long bar, int patN)
    {
        if (patN == 0)
        {
            seedRing_[bar & 15] = patRng_.s;          // remember where each bar's dice began
            lockActive_ = false;
            return;
        }
        if (!lockActive_ || patN != lockN_)
        {
            // freeze the last patN complete bars (or restart the cycle if the length changed)
            if (!lockActive_) lockSeed_ = seedRing_[(bar - patN) & 15];
            lockActive_ = true; lockN_ = patN; lockBar_ = bar;
            patRng_.s = lockSeed_;
            return;
        }
        long long k = (bar - lockBar_) % patN;
        if (k < 0) k += patN;
        if (k == 0) patRng_.s = lockSeed_;
    }

    void release(Slot& s, float fade)
    {
        s.releasing = true;
        s.evStep = -1.0f / (fade > 1.0f ? fade : 1.0f);
    }

    void startEvent(int offset, int type, double sliceBeats, double lenBeats, bool hold, bool half, bool roll,
                    double spb, float fade, float decayMul, Rng* rng)
    {
        Slot& old = slots_[cur_];
        if (old.active && !old.releasing) release(old, fade);
        cur_ ^= 1;
        Slot& s = slots_[cur_];
        s = Slot();
        s.active = true; s.hold = hold; s.type = type;
        s.startBeat = beat_; s.lengthBeats = lenBeats; s.endBeat = beat_ + lenBeats;
        s.ev = 0.0f; s.fade = fade; s.evStep = 1.0f / fade;
        s.half = half; s.roll = roll; s.rng = rng; s.decayMul = decayMul;
        s.sliceSamps = sliceBeats * spb;
        if (s.sliceSamps < 4.0 * fade) s.sliceSamps = 4.0 * fade;
        s.minRep = std::fmax(kMinRepBeats * spb, 4.0 * fade);
        s.capTarget = (int)std::fmin(std::ceil(s.sliceSamps) + 2.0, (double)capMax_);
        s.repIndex = 0; s.repStart = 0.0;
        beginRep(s);
        if (type == kTypeTape)
        {
            // the read head lags by up to half the event: keep it inside the ring
            s.tapeLen = std::fmin(lenBeats * spb, 2.0 * (ringLen_ - 16));
            s.readAbs = (double)writeAbs_;
        }
        WIPEOUT_EVENT_HOOK(offset, type, sliceBeats, half, roll, hold);
        (void)offset;
    }

    // Set up repetition s.repIndex starting at s.repStart.
    void beginRep(Slot& s)
    {
        int level = 0;
        if (s.roll && !s.hold)
        {
            const double q = (beat_ - s.startBeat) / s.lengthBeats;
            level = q < 1.0 / 3.0 - 1e-6 ? 0 : (q < 2.0 / 3.0 - 1e-6 ? 1 : 2);
            while (level > 0 && s.sliceSamps / (double)(1 << level) < s.minRep) --level;
        }
        s.repLen = s.sliceSamps / (double)(1 << level);
        const double onLen = s.type == kTypeGate ? s.repLen * 0.5 : s.repLen;
        s.repFade = (float)std::fmax(1.0, std::fmin((double)s.fade, onLen * 0.25));
        if (s.type == kTypeRepeat)
        {
            const float r = s.rng->next();          // always drawn, so a locked pattern stays in step
            s.repRev = s.repIndex >= 1 && r < revChance_;
        }
        else s.repRev = false;
        s.repGain = s.repIndex == 0 ? 1.0f : s.repGain * s.decayMul;
    }

    inline float readCap(const std::vector<float>& b, double pos, int len) const
    {
        if (len <= 0) return 0.0f;
        if (pos < 0.0) pos = 0.0;
        const double maxPos = (double)(len - 1);
        if (pos > maxPos) pos = maxPos;
        const int i0 = (int)pos;
        const int i1 = i0 + 1 < len ? i0 + 1 : i0;
        const float fr = (float)(pos - (double)i0);
        return b[(size_t)i0] + (b[(size_t)i1] - b[(size_t)i0]) * fr;
    }

    inline float readRing(const std::vector<float>& b, double absPos) const
    {
        const long long a0 = (long long)absPos;
        const float fr = (float)(absPos - (double)a0);
        const int i0 = (int)(a0 % ringLen_);
        const int i1 = i0 + 1 < ringLen_ ? i0 + 1 : 0;
        return b[(size_t)i0] + (b[(size_t)i1] - b[(size_t)i0]) * fr;
    }

    void runSlot(int si, float inL, float inR, int offset, float& wetL, float& wetR, float& evSum)
    {
        Slot& s = slots_[si];
        (void)offset;

        // event envelope against the dry signal
        s.ev += s.evStep;
        if (s.ev >= 1.0f) { s.ev = 1.0f; s.evStep = 0.0f; }
        if (s.ev <= 0.0f && s.releasing) { s.active = false; return; }
        const float ev = smoothstep01(s.ev);

        float yL = 0.0f, yR = 0.0f;
        if (s.type == kTypeTape)
        {
            const double v = std::fmax(0.0, 1.0 - (double)s.t / s.tapeLen);
            const float g = smoothstep01((float)v * 5.0f);
            yL = readRing(ring_[0], s.readAbs) * g;
            yR = readRing(ring_[1], s.readAbs) * g;
            s.readAbs += v;
        }
        else
        {
            // capture the first slice into this slot's own buffer (it plays through as it is captured)
            if (s.type == kTypeRepeat && s.capLen < s.capTarget)
            {
                cap_[si][0][(size_t)s.capLen] = inL;
                cap_[si][1][(size_t)s.capLen] = inR;
                ++s.capLen;
            }
            while ((double)s.t - s.repStart >= s.repLen)
            {
                s.repStart += s.repLen;
                s.repT0 = s.t;
                ++s.repIndex;
                beginRep(s);
                WIPEOUT_REP_HOOK(offset, s.repIndex, s.repRev, s.repLen);
            }
            const double p = (double)(s.t - s.repT0);
            const float f = s.repFade;
            const float pf = (float)p;
            if (s.type == kTypeRepeat)
            {
                double pos = s.half ? p * 0.5 : p;
                if (s.repRev) pos = (s.half ? s.repLen * 0.5 : s.repLen) - 1.0 - pos;
                float win = 1.0f;
                if (s.repIndex > 0 && pf < f) win = smoothstep01(pf / f);
                const float rem = (float)(s.repStart + s.repLen - (double)s.t);
                if (rem < f) win *= smoothstep01(rem / f);
                win *= s.repGain;
                yL = readCap(cap_[si][0], pos, s.capLen) * win;
                yR = readCap(cap_[si][1], pos, s.capLen) * win;
            }
            else // Gate
            {
                const float on = (float)(s.repLen * 0.5);
                float win = 0.0f;
                if (pf < on)
                {
                    win = 1.0f;
                    if (s.repIndex > 0 && pf < f) win = smoothstep01(pf / f);
                    if (on - pf < f) win *= smoothstep01((on - pf) / f);
                }
                win *= s.repGain;
                yL = inL * win;
                yR = inR * win;
            }
        }
        ++s.t;
        wetL += yL * ev;
        wetR += yR * ev;
        evSum += ev;
    }

    // buffers (allocated in prepare)
    std::vector<float> ring_[2];
    std::vector<float> cap_[2][2];
    int       ringLen_ = 0, capMax_ = 0;
    long long writeAbs_ = 0;
    double    sr_ = 44100.0;

    Slot   slots_[2];
    int    cur_ = 0;

    // clock
    double beat_ = 0.0, nextGridBeat_ = 0.0, nextBarBeat_ = 0.0, holdStartBeat_ = 0.0;
    bool   wasPlaying_ = false, holdPending_ = false, needResync_ = true;
    int    lastGrid_ = -1, lastOffset_ = -1;
    double lastBarBeats_ = -1.0;
    float  envF_ = 0.0f, envRel_ = 0.999f;
    long long quietSamps_ = 0;

    // dice
    Rng      patRng_, holdRng_;
    uint32_t seedRing_[16] = {};
    bool     lockActive_ = false;
    int      lockN_ = 0;
    long long lockBar_ = 0;
    uint32_t lockSeed_ = 0;
    float    revChance_ = 0.0f;

    mpc::dsp::LinearRamp out_, mix_;
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(Wipeout)
