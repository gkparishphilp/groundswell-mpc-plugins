// Driftwood - one pad, one chord: a strummed chord instrument for the MPC, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// Set a Key and a Scale, and every note you play becomes the chord built on that scale degree,
// strummed like a hand across strings, played by Driftwood's own warm voices and worn in a little
// by tape and dust. Plugins cannot send MIDI to the MPC, so Driftwood makes its own sound.
//
// SIGNAL FLOW
//
//   NOTE IN ─> CHORD   Key + Scale + Note Map pick the scale degree; Quality builds the chord on it
//                      (ChordTheory.h). Out-of-key notes snap to the nearest degree or play their own
//                      (major-family) chord. Voicing (close / open / spread) and Inversion shape it;
//                      Voice Lead instead picks the inversion and octave that move least from the
//                      last chord, staying near Register. Bass adds the root an octave or two down.
//          ─> STRUM    the chord's notes are queued with offsets: Strum spreads them over a time
//                      (Vel>Strum makes hard hits faster), Direction picks the order (down = low to
//                      high, like a guitar downstroke), Loose Time / Loose Vel humanise each note.
//          ─> PATTERN  (optional) re-strums the held chord on a 16-step rhythm-guitar pattern: down
//                      and up strokes, muted chucks, offbeat chops. Locked to the MPC's transport
//                      while it plays; free-running at the song tempo from the first note when stopped.
//          ─> ARP      (optional, overrides Pattern) plays the held chord one note at a time on the
//                      same clock: Up, Down, Up-Down, Down-Up, Random or Thumb (the lowest note alternating
//                      with the others, like fingerpicking), 1/4 to 1/32 with triplets, over 1-3 octaves,
//                      each note held for Arp Gate of a step and then left to ring for Release.
//          ─> VOICES   16 voices (+4 spare slots so a stolen voice can fade out without a click):
//                        Keys   2-operator FM tine: a sine modulated at 1:1, the index decaying from a
//                               bright bark to a mellow floor, a fast-decaying bell partial and a touch
//                               of pickup asymmetry. Tone = brightness (FM index, bell).
//                        Pad    two detuned anti-aliased saws and a sub sine through a gentle per-voice
//                               low pass that opens with the swell. Tone = cutoff.
//                        Pluck  a Karplus-Strong waveguide string (the loop of Hammerhead's Pluck): a
//                               filtered noise pick comb-filtered by the pick position, a cubic delay
//                               read, a one-pole loss filter tuned out of the pitch. Tone = brightness.
//                        Organ  drawbars: sub, 1, 2, 3, 4, 6 x the note from one sine table, plus key
//                               click. Tone pulls in the upper drawbars.
//                      Attack / Release shape every voice; Width spreads the chord across the stereo.
//          ─> OUTPUT   Filter (low pass) ─> Chorus ─> Room ─> Warmth (tape saturation) ─> Wobble (wow, flutter)
//                      ─> Dust (crackle, hiss, top-end roll-off; fades with the sound) ─> Output
//                      ─> soft limiter.
//
// CPU: per voice per sample about 15 (Pluck, Organ) to 30 (Pad) simple operations and 1-6 table
// reads; the output stage is about 60. All exp/pow/cos runs at note-on or once per 32-sample tick.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/smoother.h"
#include "dsp/svf.h"

#include "ChordTheory.h"
#include "LofiParts.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Test hook: called when a voice starts (tests/listentest.cpp logs the notes).
#ifndef DRIFTWOOD_NOTE_HOOK
#define DRIFTWOOD_NOTE_HOOK(samplePos, pitch, vel, chordId)
#endif

namespace driftwood {

constexpr int kMaxLive   = 16;     // voices that may sound at once
constexpr int kNumSlots  = 20;     // + 4 spare slots: a stolen voice fades over 5 ms in a spare one
constexpr int kCtrl      = 32;     // samples per control tick
constexpr int kMaxPend   = 96;     // queued (strummed) note starts
constexpr int kTable     = 2048;   // sine table size
constexpr int kMaxKeys   = 16;     // held keys remembered for Pattern
constexpr float kVoiceGain = 0.30f;   // calibrated by tests/listentest.cpp (a chord peaks around -8 dBFS)

enum Engine { kKeys = 0, kPad, kPluck, kOrgan, kNumEngines };
enum Stage  { kStOff = 0, kStAttack, kStDecay, kStRelease };   // (kSt: the parameter enum has kAttack/kRelease)
enum Dir    { kDown = 0, kUp, kAlternate, kByVelocity };

static const char* const kKeyNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

// ---- Patterns ----------------------------------------------------------------------------------
// 16 steps (sixteenth notes at Pat Speed Normal = one bar of 4/4).
//   D down stroke (all strings, low to high)      U up stroke (top strings, high to low)
//   X muted chuck (all strings, choked at once)   K chop (short offbeat up stroke, top strings)
static const char* const kPatternNames[] = {"Off", "Quarters", "Eighths", "Folk", "Tresillo", "Ballad", "Gallop", "Skank", "Funk 16"};
static const char* const kPatterns[] = {
    "................",
    "D...D...D...D...",
    "D.U.D.U.D.U.D.U.",
    "D...D.U...U.D.U.",
    "D..D..D...D.U.U.",
    "D.......D...U.U.",
    "D.DUD.DUD.DUD.DU",
    "..K...K...K...K.",
    "DXUXDUXUDXUXDUXU",
};
constexpr int kNumPatterns = 9;

// ---- Arpeggiator ---------------------------------------------------------------------------
enum ArpMode { kArpOff = 0, kArpUp, kArpDown, kArpUpDown, kArpDownUp, kArpRandom, kArpThumb, kNumArpModes };
static const char* const kArpModeNames[] = {"Off", "Up", "Down", "Up-Down", "Down-Up", "Random", "Thumb"};
static const char* const kArpRateNames[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const double kArpRateBeats[] = {1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};
static const bool kArpRateTriplet[] = {false, false, true, false, true, false};
constexpr int kNumArpRates = 6;
constexpr int kMaxArpNotes = kMaxChordVoices * 3;

struct Voice
{
    int stage = kStOff, engine = kKeys, pitch = -1;
    uint32_t chord = 0, order = 0;
    bool held = false, pedal = false, fading = false;
    float fade = 1.0f, fadeStep = 0.0f;
    int gate = -1;                       // samples until an automatic (muted) release, -1 = none
    float level = 0.0f, gL = 0.7f, gR = 0.7f, vel = 0.0f;
    // envelope
    float env = 0.0f, attInc = 1.0f, decCoef = 1.0f, sus = 1.0f, relCoef = 0.999f;
    // oscillators
    float ph = 0.0f, inc = 0.0f, ph2 = 0.0f, ph3 = 0.0f, inc3 = 0.0f, sub = 0.0f;
    // keys
    float fmI = 0.0f, fmFloor = 0.0f, fmDec = 1.0f, bell = 0.0f, bellDec = 1.0f, bark = 0.0f;
    // pad
    float lo = 0.0f, bp = 0.0f, f0hz = 0.0f, cutMul = 1.0f, svfF = 0.1f, detune = 1.0f;
    // pluck
    float* buf = nullptr; int size = 0, mask = 0, wpos = 0;
    float D = 2.0f, lp = 0.0f, lpA = 0.5f, loopG = 0.99f, dcx = 0.0f, dcy = 0.0f;
    // organ
    float bar[6] = {0, 0, 0, 0, 0, 0}, click = 0.0f, clickDec = 0.0f;
    uint32_t rng = 1;
    int quiet = 0;
};

class Driftwood : public mpc::Plugin
{
public:
    enum Params
    {
        kOutput = 0,
        kKey, kScale, kQuality, kVoicing, kInversion, kLead, kRegister, kBass, kMap, kOffKey,
        kStrum, kDirection, kVelStrum, kLooseTime, kLooseVel,
        kEngine, kTone, kAttack, kRelease, kFilter,
        kWobble, kDust, kChorus, kRoom,
        kHold, kPattern, kPatSpeed, kSwing, kWidth,
        // (added late in 1.0 development; kept at the end)
        kArp, kArpRate, kArpOct, kArpGate, kWarmth, kVelocity,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Driftwood";
        i.vendor = "Groundswell";
        i.uniqueId = MPC_FOURCC('G', '5', 'e', 'q');   // == "uid" in plugin.json. Never change.
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Driftwood() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        std::vector<std::string> keys, pats, arps, rates;
        for (const char* k : kKeyNames) keys.push_back(k);
        for (const char* p : kPatternNames) pats.push_back(p);
        for (const char* a : kArpModeNames) arps.push_back(a);
        for (const char* r : kArpRateNames) rates.push_back(r);

        // Output level (parameter 0: the host nudges it on load, so it only sets a gain).
        addParam(floatParam ("out",       "Output", -40.0f, 6.0f, -3.0f, "dB", 1));
        // ---- the chord ----
        addParam(choiceParam("key",       "Key", keys, 0));
        addParam(choiceParam("scale",     "Scale", {"Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Harm Minor", "Mel Minor"}, 0));
        // Chord type built on each degree (stacked scale thirds; sus chords are true sus chords).
        addParam(choiceParam("quality",   "Quality", {"Triad", "7th", "9th", "Sus2", "Sus4", "Add9"}, 0));
        addParam(choiceParam("voicing",   "Voicing", {"Close", "Open", "Spread"}, 0));
        // Raises the lowest 1-3 chord tones an octave (ignored while Voice Lead is on, except for the first chord).
        addParam(choiceParam("inversion", "Inversion", {"Root", "1st", "2nd", "3rd"}, 0));
        // Pick the inversion and octave that move least from the last chord, staying near Register.
        addParam(boolParam  ("lead",      "Voice Lead", true));
        // Where the chords sit: the centre for Voice Lead, the octave of pad 1 for the Pads map.
        addParam(intParam   ("register",  "Register", 40, 84, 60));          // text from formatValue (note name)
        addParam(choiceParam("bass",      "Bass", {"Off", "-1 Oct", "-2 Oct"}, 0));
        // Keys: a note's pitch picks its degree. Pads: each note is the next degree (pad 1 = I).
        addParam(choiceParam("map",       "Note Map", {"Keys", "Pads"}, 0));
        // Keys map: out-of-key notes snap to the nearest degree, or play their own major-family chord.
        addParam(choiceParam("offkey",    "Off-Key", {"Snap", "Own Chord"}, 0));
        // ---- strum and humanise ----
        // Time from the first note of the chord to the last.
        addParam(floatParam ("strum",     "Strum", 0.0f, 500.0f, 35.0f, "", 0, 0.5f));      // text from formatValue
        addParam(choiceParam("dir",       "Direction", {"Down", "Up", "Alt", "Velocity"}, 0));
        // How much harder hits strum faster (and soft ones slower).
        addParam(floatParam ("velstrum",  "Vel>Strum", 0.0f, 100.0f, 40.0f, "%", 0));
        // Random lateness of each note, and random velocity spread.
        addParam(floatParam ("loosetime", "Loose Time", 0.0f, 40.0f, 5.0f, "ms", 0));
        addParam(floatParam ("loosevel",  "Loose Vel", 0.0f, 100.0f, 20.0f, "%", 0));
        // ---- the sound ----
        addParam(choiceParam("engine",    "Voice", {"Keys", "Pad", "Pluck", "Organ"}, 0));
        addParam(floatParam ("tone",      "Tone", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("attack",    "Attack", 1.0f, 3000.0f, 2.0f, "", 0));     // text from formatValue
        addParam(logParam   ("release",   "Release", 10.0f, 5000.0f, 450.0f, "", 0)); // text from formatValue
        addParam(logParam   ("filter",    "Filter", 300.0f, 20000.0f, 20000.0f, "", 0)); // text from formatValue
        // ---- lo-fi and space ----
        addParam(floatParam ("wobble",    "Wobble", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam ("dust",      "Dust", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(floatParam ("chorus",    "Chorus", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("room",      "Room", 0.0f, 100.0f, 20.0f, "%", 0));
        // ---- performance ----
        // Latch: chords keep sounding after the pad is released; the next pad replaces them.
        addParam(boolParam  ("hold",      "Hold", false));
        addParam(choiceParam("pattern",   "Pattern", pats, 0));
        addParam(choiceParam("patspeed",  "Pat Speed", {"Half", "Normal", "Double"}, 1));
        // MPC-style swing of the off-beat steps: 50 % straight, 66 % triplet feel.
        addParam(floatParam ("swing",     "Swing", 50.0f, 75.0f, 50.0f, "%", 0));
        // Spread of the chord's notes across the stereo field, low notes left.
        addParam(floatParam ("width",     "Width", 0.0f, 100.0f, 50.0f, "%", 0));
        // ---- arpeggiator, warmth, velocity ----
        // Arpeggiator: plays the held chord one note at a time on the song's grid (overrides Pattern).
        //   Thumb alternates the lowest note with the others going up, like fingerpicking.
        addParam(choiceParam("arp",       "Arp", arps, kArpOff));
        addParam(choiceParam("arprate",   "Arp Rate", rates, 3));
        // How many octaves the arpeggio climbs through.
        addParam(intParam   ("arpoct",    "Arp Oct", 1, 3, 1));
        // How long each arpeggio note is held, as a share of a step (Release then lets it ring).
        addParam(floatParam ("arpgate",   "Arp Gate", 10.0f, 100.0f, 70.0f, "%", 0));
        // Gentle tape saturation on the whole instrument.
        addParam(floatParam ("warmth",    "Warmth", 0.0f, 100.0f, 20.0f, "%", 0));
        // How much velocity changes the level (0 = every note full level; brightness still follows it).
        addParam(floatParam ("velocity",  "Velocity", 0.0f, 100.0f, 100.0f, "%", 0));

        if (numParams() != kNumParams) mpc::log("Driftwood: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Driftwood Keys", {{kQuality, kSeventh}, {kVoicing, kOpen}});
        program("Beach Fire Strum", {{kOutput, -0.5f}, {kEngine, kPluck}, {kTone, 55}, {kStrum, 45}, {kVoicing, kOpen}, {kPattern, 3},
                                     {kSwing, 54}, {kRelease, 300}, {kRoom, 22}, {kWobble, 15}, {kDust, 10}, {kLooseTime, 6},
                                     {kRegister, 57}});
        program("Sea Mist Pad", {{kEngine, kPad}, {kQuality, kNinth}, {kVoicing, kSpread}, {kTone, 40}, {kStrum, 0},
                                 {kAttack, 700}, {kRelease, 2500}, {kChorus, 45}, {kRoom, 50}, {kWobble, 30}, {kDust, 10},
                                 {kBass, 1}, {kWidth, 80}});
        program("Boardwalk Organ", {{kEngine, kOrgan}, {kQuality, kSeventh}, {kTone, 45}, {kStrum, 0}, {kLooseTime, 3},
                                    {kRelease, 60}, {kChorus, 40}, {kRoom, 25}, {kWobble, 10}, {kDust, 5}});
        program("Low Tide Keys", {{kQuality, kNinth}, {kVoicing, kOpen}, {kTone, 30}, {kStrum, 25}, {kLooseTime, 12},
                                  {kLooseVel, 35}, {kFilter, 5500}, {kWobble, 60}, {kDust, 45}, {kRoom, 25}, {kRegister, 57}});
        program("Harbour Harp", {{kEngine, kPluck}, {kQuality, kAdd9}, {kVoicing, kSpread}, {kTone, 70}, {kStrum, 260},
                                 {kDirection, kUp}, {kVelStrum, 60}, {kRelease, 1500}, {kRoom, 45}, {kWidth, 75},
                                 {kRegister, 64}, {kDust, 5}});
        program("Island Skank", {{kOutput, 0.0f}, {kEngine, kOrgan}, {kTone, 60}, {kPattern, 7}, {kStrum, 8}, {kRelease, 40},
                                 {kChorus, 25}, {kRoom, 20}, {kWobble, 15}, {kDust, 20}, {kBass, 0}, {kRegister, 62}});
        program("Rope Swing Funk", {{kEngine, kPluck}, {kQuality, kNinth}, {kVoicing, kClose}, {kTone, 65}, {kStrum, 18},
                                    {kPattern, 8}, {kSwing, 58}, {kRelease, 120}, {kRoom, 12}, {kWobble, 10}, {kDust, 15},
                                    {kRegister, 64}});
        // arpeggios
        program("Tidepool Arp", {{kQuality, kSeventh}, {kVoicing, kSpread}, {kTone, 55}, {kArp, kArpUpDown}, {kArpRate, 3},
                                 {kArpOct, 2}, {kArpGate, 60}, {kRelease, 900}, {kChorus, 25}, {kRoom, 40}, {kWobble, 25},
                                 {kDust, 10}, {kWidth, 70}, {kRegister, 60}, {kOutput, 0.0f}});
        program("Pier Fingerpicking", {{kEngine, kPluck}, {kTone, 50}, {kVoicing, kOpen}, {kArp, kArpThumb}, {kArpRate, 1},
                                       {kArpGate, 100}, {kRelease, 600}, {kSwing, 56}, {kBass, 1}, {kRoom, 25}, {kWobble, 15},
                                       {kDust, 15}, {kLooseTime, 6}, {kLooseVel, 25}, {kRegister, 57}, {kOutput, 0.0f}});

        for (int i = 0; i <= kTable; ++i) sine_[i] = std::sin(kTwoPi * (float)i / (float)kTable);
    }

    // ----- lifecycle ------------------------------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        int size = 1;
        while ((float)size < sr_ / 25.0f + 8.0f) size <<= 1;      // strings down to 25 Hz
        strBuf_.assign((size_t)size * kNumSlots, 0.0f);
        for (int i = 0; i < kNumSlots; ++i)
        {
            voices_[i].buf = strBuf_.data() + (size_t)i * (size_t)size;
            voices_[i].size = size; voices_[i].mask = size - 1;
        }
        chorus_.prepare(sr_); room_.prepare(sr_); wobble_.prepare(sr_); dust_.prepare(sr_);
        filtL_.setSampleRate(sampleRate); filtR_.setSampleRate(sampleRate);
        outGain_.setTime(20.0f, sampleRate);
        lastFilter_ = -1.0f;
    }

    void reset() override
    {
        for (Voice& v : voices_) { v.stage = kStOff; v.pitch = -1; v.held = v.pedal = v.fading = false; v.chord = 0; }
        std::fill(strBuf_.begin(), strBuf_.end(), 0.0f);
        numPend_ = 0;
        numChords_ = 0;
        numKeys_ = 0;
        prevN_ = 0;
        altUp_ = false;
        nextChord_ = 1; orderCounter_ = 0;
        pedal_ = false; holdWas_ = getBool(kHold);
        patActive_ = false; patChordValid_ = false; strokeChord_ = 0; lastStep_ = -1000000;
        arpCount_ = 0; arpLast_ = -1;
        freePpq_ = 0.0; wasPlaying_ = false; expectPpq_ = -1.0;
        clock_ = 0;
        rng_.s = 0x2545F491u;
        chorus_.reset(); room_.reset(); wobble_.reset(); dust_.reset();
        filtL_.reset(); filtR_.reset();
        dcxL_ = dcyL_ = dcxR_ = dcyR_ = 0.0f;
        outGain_.snap(mpc::dsp::dbToGain(get(kOutput)));
        lastFilter_ = -1.0f;
    }

    // ----- value text ----------------------------------------------------------------------------
    bool formatValue(int index, float v, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case kRegister: { const int n = (int)std::lround(v); std::snprintf(b, sizeof(b), "%s%d", kKeyNames[mod(n, 12)], floorDiv(n, 12) - 1); out = b; return true; }
            case kAttack: case kRelease:
                if (v < 1000.0f) std::snprintf(b, sizeof(b), "%.0f ms", v); else std::snprintf(b, sizeof(b), "%.2f s", v * 0.001f);
                out = b; return true;
            case kFilter:
                if (v >= 19900.0f) out = "Open";
                else { if (v < 1000.0f) std::snprintf(b, sizeof(b), "%.0f Hz", v); else std::snprintf(b, sizeof(b), "%.1f kHz", v * 0.001f); out = b; }
                return true;
            case kArpOct: std::snprintf(b, sizeof(b), "%d oct", (int)std::lround(v)); out = b; return true;
            case kStrum:
                if (v < 0.5f) out = "Block"; else { std::snprintf(b, sizeof(b), "%.0f ms", v); out = b; }
                return true;
            default: return false;
        }
    }

    // ----- audio ----------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings();
        const int n = ctx.numFrames;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const mpc::Transport& tr = ctx.transport;

        // Tempo and pattern clock for this block.
        bpm_ = (tr.tempoValid && tr.bpm > 20.0 && tr.bpm < 400.0) ? tr.bpm : 100.0;
        spb_ = (double)sr_ * 60.0 / bpm_;                 // samples per beat
        playing_ = tr.valid && tr.playing && tr.ppqValid;
        if (playing_)
        {
            hostPpq_ = tr.ppqPos;
            // A jump (locate, loop) means the step counter must re-sync instead of firing a burst.
            if (!wasPlaying_ || expectPpq_ < 0.0 || std::fabs(hostPpq_ - expectPpq_) > 0.05) resyncSteps(hostPpq_);
            expectPpq_ = hostPpq_ + (double)n / spb_;
        }
        else
        {
            if (wasPlaying_) { freePpq_ = expectPpq_ >= 0.0 ? expectPpq_ : 0.0; }   // carry on free from where the song stopped
            expectPpq_ = -1.0;
        }
        wasPlaying_ = playing_;

        // Hold switched off: release whatever is no longer physically held.
        if (holdWas_ && !s_.hold) releaseUnheld();
        holdWas_ = s_.hold;
        // Pattern switched off while it was playing: let the last stroke ring out like a released chord.
        if (patActive_ && !seqOn()) { stopPattern(); numKeys_ = 0; }

        if (std::fabs(s_.filter - lastFilter_) > 1.0f)
        {
            lastFilter_ = s_.filter;
            filtL_.set(s_.filter, 0.05f); filtR_.set(s_.filter, 0.05f);
        }
        outGain_.setTarget(s_.outGain);

        int mi = 0;
        for (int pos = 0; pos < n;)
        {
            blockPos_ = pos;
            while (mi < ctx.numMidiEvents && ctx.midi[mi].sampleOffset <= pos) handleMidi(ctx.midi[mi++]);
            if (patActive_ && seqOn()) fireDueSteps(pos);
            startDuePending();

            int end = std::min(n, pos + kCtrl);
            if (mi < ctx.numMidiEvents) end = std::min(end, std::max(pos + 1, ctx.midi[mi].sampleOffset));
            for (int i = 0; i < numPend_; ++i)
            {
                const long long off = pend_[i].t - clock_;
                if (off > pos && off < end) end = (int)off;
            }
            if (patActive_ && seqOn())
            {
                const int next = nextStepOffset(pos);
                if (next > pos && next < end) end = next;
            }
            const int len = end - pos;
            renderChunk(L + pos, R ? R + pos : nullptr, len);
            pos = end;
        }
        clock_ += n;
        if (!playing_) freePpq_ += (double)n / spb_;
    }

private:
    // ===== settings ===============================================================================
    struct Settings
    {
        float outGain = 1.0f;
        int key = 0, scale = 0, quality = 0, voicing = 0, inversion = 0, reg = 60, bass = 0, map = 0, offKey = 0;
        bool lead = true, hold = false;
        float strum = 0.035f, velStrum = 0.4f, looseT = 0.005f, looseV = 0.2f;
        int dir = 0, engine = 0, pattern = 0, patSpeed = 1;
        float tone = 0.5f, attack = 0.002f, release = 0.45f, filter = 20000.0f;
        float wobble = 0.2f, dust = 0.15f, chorus = 0.0f, room = 0.2f, swing = 0.5f, width = 0.5f;
        int arp = 0, arpRate = 3, arpOct = 1;
        float arpGate = 0.7f, warmth = 0.2f, velSens = 1.0f;
    } s_;

    void readSettings()
    {
        s_.outGain = mpc::dsp::dbToGain(get(kOutput));
        s_.key = getInt(kKey); s_.scale = mpc::clamp(getInt(kScale), 0, kNumScales - 1);
        s_.quality = getInt(kQuality); s_.voicing = getInt(kVoicing); s_.inversion = getInt(kInversion);
        s_.lead = getBool(kLead); s_.reg = getInt(kRegister); s_.bass = getInt(kBass);
        s_.map = getInt(kMap); s_.offKey = getInt(kOffKey);
        s_.strum = get(kStrum) * 0.001f; s_.dir = getInt(kDirection); s_.velStrum = get(kVelStrum) * 0.01f;
        s_.looseT = get(kLooseTime) * 0.001f; s_.looseV = get(kLooseVel) * 0.01f;
        s_.engine = mpc::clamp(getInt(kEngine), 0, kNumEngines - 1); s_.tone = get(kTone) * 0.01f;
        s_.attack = get(kAttack) * 0.001f; s_.release = get(kRelease) * 0.001f; s_.filter = get(kFilter);
        s_.wobble = get(kWobble) * 0.01f; s_.dust = get(kDust) * 0.01f;
        s_.chorus = get(kChorus) * 0.01f; s_.room = get(kRoom) * 0.01f;
        s_.hold = getBool(kHold); s_.pattern = mpc::clamp(getInt(kPattern), 0, kNumPatterns - 1);
        s_.patSpeed = getInt(kPatSpeed); s_.swing = get(kSwing) * 0.01f; s_.width = get(kWidth) * 0.01f;
        s_.arp = mpc::clamp(getInt(kArp), 0, kNumArpModes - 1); s_.arpRate = mpc::clamp(getInt(kArpRate), 0, kNumArpRates - 1);
        s_.arpOct = mpc::clamp(getInt(kArpOct), 1, 3); s_.arpGate = get(kArpGate) * 0.01f;
        s_.warmth = get(kWarmth) * 0.01f; s_.velSens = get(kVelocity) * 0.01f;
    }

    // ===== MIDI ===================================================================================
    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn()) noteOn(e.note(), (float)e.velocity() / 127.0f);
        else if (e.isNoteOff()) noteOff(e.note());
        else if (e.isAllNotesOff()) allOff();
        else if (e.isController() && e.controller() == 64)
        {
            const bool down = e.controllerValue() >= 64;
            if (pedal_ && !down)
                for (Voice& v : voices_) if (v.stage != kStOff && v.pedal) { v.pedal = false; releaseVoice(v, false); }
            pedal_ = down;
        }
    }

    struct ChordRec { uint32_t id; int key; bool down; };
    struct Pending { long long t; int pitch; float vel; uint32_t chord; int gate; bool release; bool reown; bool mute; };

    // Build the voiced chord for a key (with voice leading against the last chord).
    int buildVoicing(int note, int* out)
    {
        const Chord c = resolveChord(note, s_.key, s_.scale, s_.quality, s_.map, s_.offKey, s_.reg);
        int v[kMaxTones];
        int nv;
        if (s_.lead) nv = leadChord(c, s_.voicing, s_.inversion, s_.reg, prev_, prevN_, v);
        else
        {
            // Keys map: the root is the played note; Pads map: resolveChord already placed it by Register.
            nv = voiceChord(c, c.root, s_.inversion, s_.voicing, v);
        }
        for (int i = 0; i < nv; ++i) { prev_[i] = v[i]; out[i] = v[i]; }
        prevN_ = nv;
        int total = nv;
        if (s_.bass > 0)
        {
            const int b = bassNote(c, v, s_.bass);
            for (int i = total; i > 0; --i) out[i] = out[i - 1];   // bass first (lowest)
            out[0] = b; ++total;
        }
        for (int i = 0; i < total; ++i) out[i] = mpc::clamp(out[i], 12, 120);
        return total;
    }

    void noteOn(int note, float vel)
    {
        int pitches[kMaxChordVoices];
        const int np = buildVoicing(note, pitches);

        if (seqOn())
        {
            // Pattern or Arp: the key sets the chord they play.
            if (s_.hold) numKeys_ = 0;
            pushKey(note, vel);
            for (int i = 0; i < np; ++i) patPitches_[i] = pitches[i];
            patN_ = np; patVel_ = vel; patChordValid_ = true; patBass_ = s_.bass > 0;
            if (!patActive_)
            {
                patActive_ = true;
                arpCount_ = 0;
                if (playing_) { resyncSteps(ppqAt(blockPos_)); lateCatch(); }
                else
                {
                    freePpq_ = -(double)blockPos_ / spb_;     // step 0 lands on this note
                    lastStep_ = -1;
                }
            }
            else if (playing_) lateCatch();
            return;
        }

        // Hold: a new chord replaces the latched one.
        if (s_.hold)
        {
            for (Voice& v : voices_) if (v.stage != kStOff && !v.fading && v.held) releaseVoice(v, false);
            for (int i = 0; i < numPend_; ++i) pend_[i].release = true;
            numChords_ = 0;
        }
        const uint32_t id = newChord(note);

        // Strum direction and speed.
        bool up = false;
        switch (s_.dir)
        {
            case kUp: up = true; break;
            case kAlternate: up = altUp_; altUp_ = !altUp_; break;
            case kByVelocity: up = vel < 0.5f; break;
            default: break;
        }
        const float velFac = mpc::clamp(1.0f + s_.velStrum * (0.5f - vel) * 1.5f, 0.2f, 2.0f);
        queueStrum(pitches, np, up, s_.strum * velFac, vel, id, -1, true);
    }

    void noteOff(int note)
    {
        if (seqOn() || patActive_)
        {
            for (int i = 0; i < numChords_; ++i) if (chords_[i].key == note) chords_[i].down = false;
            removeKey(note);
            if (s_.hold) return;
            if (numKeys_ == 0) stopPattern();
            else
            {
                // Fall back to the chord of the key still held underneath.
                int pitches[kMaxChordVoices];
                patN_ = buildVoicing(keyStack_[numKeys_ - 1], pitches);
                for (int i = 0; i < patN_; ++i) patPitches_[i] = pitches[i];
                patVel_ = keyVel_[numKeys_ - 1];
            }
            if (seqOn()) return;
        }
        for (int i = 0; i < numChords_; ++i)
        {
            if (chords_[i].key != note || !chords_[i].down) continue;
            chords_[i].down = false;
            if (s_.hold) continue;
            releaseChord(chords_[i].id);
        }
    }

    void allOff()
    {
        for (Voice& v : voices_) if (v.stage != kStOff) releaseVoice(v, true);
        numPend_ = 0; numChords_ = 0; numKeys_ = 0;
        patActive_ = false; patChordValid_ = false;
        pedal_ = false;
    }

    void releaseUnheld()
    {
        for (int i = 0; i < numChords_; ++i) if (!chords_[i].down) releaseChord(chords_[i].id);
        if (patActive_ && numKeys_ == 0) stopPattern();
    }

    void releaseChord(uint32_t id)
    {
        for (Voice& v : voices_)
            if (v.stage != kStOff && v.chord == id && v.held)
            {
                if (pedal_) v.pedal = true; else releaseVoice(v, false);
            }
        for (int i = 0; i < numPend_; ++i) if (pend_[i].chord == id) pend_[i].release = true;
    }

    uint32_t newChord(int key)
    {
        const uint32_t id = nextChord_++;
        if (nextChord_ == 0) nextChord_ = 1;
        if (numChords_ >= 32) { std::memmove(chords_, chords_ + 1, sizeof(ChordRec) * 31); numChords_ = 31; }
        chords_[numChords_++] = {id, key, true};
        return id;
    }

    void pushKey(int note, float vel)
    {
        removeKey(note);
        if (numKeys_ >= kMaxKeys) { std::memmove(keyStack_, keyStack_ + 1, sizeof(int) * (kMaxKeys - 1)); std::memmove(keyVel_, keyVel_ + 1, sizeof(float) * (kMaxKeys - 1)); numKeys_ = kMaxKeys - 1; }
        keyStack_[numKeys_] = note; keyVel_[numKeys_] = vel; ++numKeys_;
    }
    void removeKey(int note)
    {
        for (int i = 0; i < numKeys_; ++i)
            if (keyStack_[i] == note)
            {
                for (int j = i; j < numKeys_ - 1; ++j) { keyStack_[j] = keyStack_[j + 1]; keyVel_[j] = keyVel_[j + 1]; }
                --numKeys_; --i;
            }
    }

    // ===== strum queue ============================================================================
    // Queue the chord's notes: offsets spread over strumSec in the strum order, plus Loose Time.
    void queueStrum(const int* pitches, int np, bool up, float strumSec, float vel, uint32_t id, int gate, bool reown, bool mute = true)
    {
        const long long now = clock_ + blockPos_;
        for (int k = 0; k < np; ++k)
        {
            const int idx = up ? np - 1 - k : k;
            float off = np > 1 ? strumSec * (float)k / (float)(np - 1) : 0.0f;
            off += s_.looseT * rng_.next();
            float v = vel * (1.0f + s_.looseV * 0.4f * rng_.bi());
            v = mpc::clamp(v, 0.02f, 1.0f);
            if (numPend_ >= kMaxPend) break;
            pend_[numPend_++] = {now + (long long)std::lround(off * sr_), pitches[idx], v, id, gate, false, reown, mute};
        }
    }

    void startDuePending()
    {
        const long long now = clock_ + blockPos_;
        for (int i = 0; i < numPend_;)
        {
            if (pend_[i].t <= now)
            {
                const Pending p = pend_[i];
                pend_[i] = pend_[--numPend_];
                Voice* v = startVoice(p.pitch, p.vel, p.chord, p.gate, p.reown, p.mute);
                if (v && p.release) { if (pedal_) v->pedal = true; else releaseVoice(*v, false); }
            }
            else ++i;
        }
    }

    // ===== pattern ================================================================================
    bool seqOn() const { return s_.pattern > 0 || s_.arp > 0; }
    // Step length in beats: the Arp Rate while the Arp is on, else a pattern sixteenth at Pat Speed.
    double stepLen() const
    {
        if (s_.arp > 0) return kArpRateBeats[s_.arpRate];
        return 0.25 * (s_.patSpeed == 0 ? 2.0 : (s_.patSpeed == 2 ? 0.5 : 1.0));
    }
    // Swing delays the odd steps (not on triplet arp rates, which already swing).
    double onsetPpq(long long k) const
    {
        double t = (double)k * stepLen();
        const bool trip = s_.arp > 0 && kArpRateTriplet[s_.arpRate];
        if ((k & 1) && !trip) t += (s_.swing - 0.5) * 2.0 * stepLen();
        return t;
    }
    double ppqAt(int pos) const { return (playing_ ? hostPpq_ : freePpq_) + (double)pos / spb_; }

    // First step whose onset is at or after ppq.
    long long firstStepFrom(double ppq) const
    {
        long long k = (long long)std::floor(ppq / stepLen()) - 1;
        while (onsetPpq(k) < ppq - 1.0e-9) ++k;
        return k;
    }
    void resyncSteps(double ppq) { lastStep_ = firstStepFrom(ppq - 0.5 / spb_) - 1; }

    int nextStepOffset(int pos) const
    {
        const double a = ppqAt(pos);
        long long k = std::max(lastStep_ + 1, firstStepFrom(a));
        const double off = (onsetPpq(k) - a) * spb_;
        if (off > 4096.0) return 1 << 30;
        return pos + std::max(1, (int)std::ceil(off - 0.5));
    }

    void fireDueSteps(int pos)
    {
        const double a = ppqAt(pos) + 0.5 / spb_;     // half a sample of tolerance
        // Far behind or ahead (Pat Speed or Swing changed, a tempo jump): re-sync instead of firing a burst.
        const double nextOn = onsetPpq(lastStep_ + 1);
        if (nextOn < a - stepLen() || nextOn > a + 2.0 * stepLen()) lastStep_ = firstStepFrom(a - 1.0 / spb_) - 1;
        int guard = 0;
        while (onsetPpq(lastStep_ + 1) <= a && guard++ < 8)
        {
            ++lastStep_;
            fireStep(lastStep_);
        }
    }

    // When a pad is hit just after a step (a human is a little late, or the chord changed on the
    // beat), play that step now instead of waiting for the next one.
    void lateCatch()
    {
        const double a = ppqAt(blockPos_);
        const long long k = firstStepFrom(a + 0.5 / spb_) - 1;          // the step at or just before now
        const double lateSec = (a - onsetPpq(k)) * 60.0 / bpm_;
        if (lateSec >= 0.0 && lateSec < std::min(0.04, 0.5 * stepLen() * 60.0 / bpm_))
        {
            const char c = s_.arp > 0 ? 'A' : kPatterns[s_.pattern][mod((int)(k % 16), 16)];
            if (c != '.') fireStep(k);
            if (k > lastStep_) lastStep_ = k;
        }
    }

    void fireStep(long long k)
    {
        if (!patChordValid_ || !seqOn()) return;
        if (s_.arp > 0) { fireArp(k); return; }
        const int step = mod((int)(k % 16), 16);
        const char c = kPatterns[s_.pattern][step];
        if (c == '.') return;
        // The previous stroke is choked by this one (a hand re-strumming the strings).
        for (Voice& v : voices_) if (v.stage != kStOff && v.chord == strokeChord_ && !v.fading) chokeVoice(v);
        const uint32_t id = nextChord_++;
        if (nextChord_ == 0) nextChord_ = 1;
        strokeChord_ = id;
        // Accents: the beat, then the off-beat eighth, then the sixteenths.
        float acc = (step % 4 == 0) ? 1.0f : (step % 2 == 0 ? 0.85f : 0.72f);
        const float stepSec = (float)(stepLen() * 60.0 / bpm_);
        const float strumSec = std::min(s_.strum, 0.6f * stepSec);
        int p[kMaxChordVoices];
        int np = 0;
        const int first = patBass_ ? 1 : 0;   // up strokes and chops skip the bass
        switch (c)
        {
            case 'D':
                for (int i = 0; i < patN_; ++i) p[np++] = patPitches_[i];
                queueStrum(p, np, false, strumSec, patVel_ * acc, id, -1, false);
                break;
            case 'U':
                for (int i = std::max(first, patN_ - 4); i < patN_; ++i) p[np++] = patPitches_[i];
                queueStrum(p, np, true, strumSec * 0.8f, patVel_ * acc * 0.8f, id, -1, false);
                break;
            case 'X':
                for (int i = 0; i < patN_; ++i) p[np++] = patPitches_[i];
                queueStrum(p, np, false, strumSec * 0.4f, patVel_ * 0.55f, id, (int)(0.03f * sr_), false);
                break;
            case 'K':
                for (int i = std::max(first, patN_ - 4); i < patN_; ++i) p[np++] = patPitches_[i];
                queueStrum(p, np, true, strumSec * 0.5f, patVel_ * 0.9f, id, (int)(0.55f * stepSec * sr_), false);
                break;
            default: break;
        }
    }

    // One arpeggio step: the next note of the held chord (spread over Arp Oct octaves) in the Arp's order.
    void fireArp(long long k)
    {
        int seq[kMaxArpNotes];
        int n = 0;
        for (int o = 0; o < s_.arpOct; ++o)
            for (int i = 0; i < patN_ && n < kMaxArpNotes; ++i) seq[n++] = std::min(120, patPitches_[i] + 12 * o);
        if (n == 0) return;
        const int c = arpCount_++;
        int idx = 0;
        switch (s_.arp)
        {
            case kArpUp:   idx = c % n; break;
            case kArpDown: idx = n - 1 - c % n; break;
            case kArpUpDown:
            case kArpDownUp:
            {
                const int period = n > 1 ? 2 * n - 2 : 1;
                const int i = c % period;
                idx = i < n ? i : period - i;
                if (s_.arp == kArpDownUp) idx = n - 1 - idx;
                break;
            }
            case kArpRandom:
            {
                idx = (int)(rng_.next() * (float)n) % n;
                if (n > 1 && idx == arpLast_) idx = (idx + 1 + (int)(rng_.next() * (float)(n - 1))) % n;
                break;
            }
            case kArpThumb:
            default:
                // Even steps: the lowest note (the thumb); odd steps: the others in turn, going up.
                idx = (c % 2 == 0 || n < 2) ? 0 : 1 + (c / 2) % (n - 1);
                break;
        }
        arpLast_ = idx;
        const uint32_t id = nextChord_++;
        if (nextChord_ == 0) nextChord_ = 1;
        strokeChord_ = id;
        const double beatPos = onsetPpq(k) - std::floor(onsetPpq(k) + 1.0e-9);
        const float acc = beatPos < 1.0e-6 ? 1.0f : 0.86f;
        const float stepSec = (float)(stepLen() * 60.0 / bpm_);
        const int gate = std::max(16, (int)(s_.arpGate * stepSec * sr_));
        int p[1] = {seq[idx]};
        queueStrum(p, 1, false, 0.0f, patVel_ * acc, id, gate, false, false);
    }

    void stopPattern()
    {
        patActive_ = false; patChordValid_ = false;
        for (Voice& v : voices_) if (v.stage != kStOff && v.chord == strokeChord_ && !v.fading) { if (pedal_) v.pedal = true; else releaseVoice(v, false); }
        for (int i = 0; i < numPend_; ++i) if (pend_[i].chord == strokeChord_) pend_[i].release = true;
    }

    // ===== voices =================================================================================
    float sineAt(float ph) const
    {
        ph -= (float)(int)ph;
        if (ph < 0.0f) ph += 1.0f;
        const float x = ph * (float)kTable;
        const int i = (int)x;
        const float f = x - (float)i;
        return sine_[i] + f * (sine_[i + 1] - sine_[i]);
    }

    static float t60Coef(float sec, float sr) { return std::exp(-6.9078f / (std::max(sec, 0.001f) * sr)); }

    Voice* allocVoice()
    {
        int live = 0;
        for (Voice& v : voices_) if (v.stage != kStOff && !v.fading) ++live;
        if (live >= kMaxLive)
        {
            // Steal: the quietest released voice, else the oldest.
            Voice* victim = nullptr;
            float best = 1.0e9f;
            for (Voice& v : voices_)
                if (v.stage == kStRelease && !v.fading && v.env * v.level < best) { best = v.env * v.level; victim = &v; }
            if (!victim)
            {
                uint32_t oldest = 0xFFFFFFFFu;
                for (Voice& v : voices_) if (v.stage != kStOff && !v.fading && v.order < oldest) { oldest = v.order; victim = &v; }
            }
            if (victim) fadeVoice(*victim);
        }
        for (Voice& v : voices_) if (v.stage == kStOff) return &v;
        // No free slot: drop the fading voice closest to silence.
        Voice* f = nullptr;
        for (Voice& v : voices_) if (v.fading && (!f || v.fade < f->fade)) f = &v;
        if (f) f->stage = kStOff;
        return f;
    }

    void fadeVoice(Voice& v) { v.fading = true; v.fadeStep = 1.0f / (0.005f * sr_); v.held = false; }

    Voice* startVoice(int pitch, float vel, uint32_t chord, int gate, bool reown, bool mute = true)
    {
        const int eng = s_.engine;
        // The same pitch already sounding: sustained voices (pad, organ) are taken over by the new
        // chord without re-attacking; struck voices (keys, pluck) are re-struck.
        for (Voice& v : voices_)
        {
            if (v.stage == kStOff || v.fading || v.pitch != pitch) continue;
            if (reown && v.engine == eng && (eng == kPad || eng == kOrgan) && gate < 0)
            {
                v.chord = chord; v.held = true; v.pedal = false; v.order = ++orderCounter_;
                if (v.stage == kStRelease) v.stage = kStAttack;
                DRIFTWOOD_NOTE_HOOK(clock_ + blockPos_, pitch, vel, chord);
                return &v;
            }
            fadeVoice(v);
        }
        Voice* vp = allocVoice();
        if (!vp) return nullptr;
        Voice& v = *vp;
        DRIFTWOOD_NOTE_HOOK(clock_ + blockPos_, pitch, vel, chord);

        const float f0 = 440.0f * std::exp2(((float)pitch - 69.0f) / 12.0f);
        v.engine = eng; v.pitch = pitch; v.chord = chord; v.order = ++orderCounter_;
        v.held = true; v.pedal = false; v.fading = false; v.fade = 1.0f; v.gate = gate;
        v.vel = vel; v.quiet = 0;
        v.inc = f0 / sr_;
        v.ph = 0.0f; v.ph2 = 0.37f; v.ph3 = 0.0f; v.sub = 0.0f;
        v.env = 0.0f; v.stage = kStAttack;
        v.rng = (uint32_t)(pitch * 2654435761u) ^ (orderCounter_ * 40503u) ^ 0x5bd1e995u;
        if (v.rng == 0) v.rng = 1;
        const float pan = s_.width * mpc::clamp((float)(pitch - 60) / 18.0f, -1.0f, 1.0f) * 0.8f;
        mpc::dsp::equalPowerPan(pan, v.gL, v.gR);
        const float tone = s_.tone;
        // Level from velocity, scaled by Velocity (brightness keeps following the real velocity).
        const float velCurve = 1.0f - s_.velSens * (1.0f - (0.18f + 0.82f * std::pow(vel, 1.3f)));
        const float hi = mpc::clamp(((float)pitch - 48.0f) / 36.0f, 0.0f, 1.0f);   // 0 at C3, 1 at C6

        switch (eng)
        {
            case kKeys:
            {
                v.level = kVoiceGain * 1.0f * velCurve;
                const float i0 = (0.25f + 2.4f * tone) * (0.3f + 0.9f * vel) * (1.0f - 0.6f * hi);
                v.fmI = i0; v.fmFloor = 0.22f * i0;
                v.fmDec = t60Coef(0.5f + 0.9f * (1.0f - hi), sr_);
                v.inc3 = v.inc * 7.0f;
                v.bell = (v.inc3 < 0.45f) ? (0.04f + 0.22f * tone) * vel : 0.0f;
                v.bellDec = t60Coef(0.18f, sr_);
                v.bark = 0.35f * tone * vel;
                v.attInc = 1.0f / (std::max(0.001f, s_.attack) * sr_);
                v.sus = 0.0f;
                v.decCoef = t60Coef(mpc::clamp(7.0f * std::exp2(-((float)pitch - 48.0f) / 18.0f), 1.2f, 10.0f), sr_);
                v.relCoef = t60Coef(s_.release, sr_);
                break;
            }
            case kPad:
            {
                v.level = kVoiceGain * 0.62f * (0.45f + 0.55f * velCurve);
                v.detune = std::exp2(7.0f / 1200.0f);
                v.f0hz = f0;
                v.cutMul = 1.4f + 9.0f * tone * tone;
                v.lo = v.bp = 0.0f;
                v.attInc = 1.0f / (std::max(0.001f, 0.12f + s_.attack) * sr_);
                v.sus = 0.85f;
                v.decCoef = t60Coef(3.0f, sr_);
                v.relCoef = t60Coef(std::max(0.08f, s_.release), sr_);
                updatePadFilter(v);
                break;
            }
            case kPluck:
            {
                v.level = kVoiceGain * 2.6f * velCurve;
                startString(v, f0, vel, tone);
                v.attInc = 1.0f / (std::max(0.0005f, s_.attack) * sr_);
                v.sus = 1.0f; v.decCoef = 1.0f;           // the string decays by itself
                v.relCoef = t60Coef(s_.release, sr_);
                break;
            }
            case kOrgan:
            default:
            {
                v.level = kVoiceGain * 0.42f * (0.7f + 0.3f * velCurve);
                static const float kBase[6] = {0.55f, 1.0f, 0.45f, 0.22f, 0.0f, 0.0f};
                static const float kTone[6] = {0.0f, 0.0f, 0.25f, 0.35f, 0.45f, 0.35f};
                static const float kMul[6]  = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f};
                for (int k = 0; k < 6; ++k)
                {
                    float b = kBase[k] + kTone[k] * tone;
                    if (f0 * kMul[k] > 0.42f * sr_) b = 0.0f;        // no partials above ~18 kHz
                    v.bar[k] = b;
                }
                v.click = (0.08f + 0.3f * tone) * vel;
                v.clickDec = std::exp(-1.0f / (0.003f * sr_));
                v.attInc = 1.0f / (std::max(0.004f, s_.attack) * sr_);
                v.sus = 1.0f; v.decCoef = 1.0f;
                v.relCoef = t60Coef(std::max(0.03f, s_.release), sr_);
                break;
            }
        }
        if (v.gate >= 0 && mute) v.relCoef = t60Coef(eng == kPluck ? 0.05f : 0.06f, sr_);   // a muted chuck dies fast
        return &v;
    }

    // Karplus-Strong string: noise pick of one period, low-passed by Tone and velocity, comb-filtered
    // by the pick position; the loop is a cubic delay read, a one-pole loss filter and a gain set from
    // the wanted decay time. The loss filter's phase delay at the note is subtracted, so it stays in tune.
    void startString(Voice& v, float f0, float vel, float tone)
    {
        f0 = std::max(f0, 26.0f);
        const float P = sr_ / f0;
        const float hiNote = mpc::clamp(((float)v.pitch - 40.0f) / 50.0f, 0.0f, 1.0f);
        v.lpA = mpc::clamp(0.22f + 0.6f * tone + 0.25f * hiNote, 0.08f, 0.97f);
        const float w = kTwoPi * f0 / sr_;
        const float b = 1.0f - v.lpA;
        const float lpDelay = std::atan2(b * std::sin(w), 1.0f - b * std::cos(w)) / w;
        const float mag = v.lpA / std::sqrt(1.0f - 2.0f * b * std::cos(w) + b * b);
        v.D = std::max(2.0f, P - lpDelay);
        const float t60 = mpc::clamp(6.0f * std::exp2(-((float)v.pitch - 45.0f) / 20.0f), 0.9f, 8.0f) * (0.7f + 0.6f * tone);
        v.loopG = std::min(0.9995f, std::pow(10.0f, -3.0f * P / (t60 * sr_)) / std::max(mag, 0.1f));
        v.lp = 0.0f; v.dcx = v.dcy = 0.0f;
        std::memset(v.buf, 0, sizeof(float) * (size_t)v.size);
        v.wpos = 0;
        // Fill the D samples the read head will see first with the pick.
        const int len = std::min((int)std::ceil(v.D), v.size - 4);
        const float pa = mpc::clamp(0.06f + 0.5f * tone * (0.45f + 0.55f * vel), 0.04f, 1.0f);
        float z = 0.0f, mean = 0.0f;
        for (int k = 0; k < len; ++k)
        {
            v.rng ^= v.rng << 13; v.rng ^= v.rng >> 17; v.rng ^= v.rng << 5;
            const float r = (float)(v.rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
            z += pa * (r - z);
            v.buf[(size_t)((v.wpos - len + k) & v.mask)] = z;
            mean += z;
        }
        mean /= (float)std::max(1, len);
        const int m = std::max(1, (int)(0.18f * (float)len));          // pick position, 18 % along
        for (int k = len - 1; k >= 0; --k)
        {
            const size_t i = (size_t)((v.wpos - len + k) & v.mask);
            const float prev = k >= m ? v.buf[(size_t)((v.wpos - len + k - m) & v.mask)] : mean;
            v.buf[i] = (v.buf[i] - prev);
        }
    }

    void updatePadFilter(Voice& v)
    {
        float fc = v.f0hz * v.cutMul * (0.55f + 0.45f * v.env) + 250.0f;
        fc = std::min(fc, 0.16f * sr_);
        const float x = 3.14159265f * fc / sr_;
        v.svfF = 2.0f * (x - x * x * x * (1.0f / 6.0f));
    }

    void releaseVoice(Voice& v, bool fast)
    {
        if (v.stage == kStOff) return;
        v.held = false; v.pedal = false;
        if (fast) v.relCoef = t60Coef(0.05f, sr_);
        v.stage = kStRelease;
    }

    // Choke by the next pattern stroke: a hand back on the strings.
    void chokeVoice(Voice& v)
    {
        v.held = false; v.pedal = false;
        const float t = (v.engine == kPad || v.engine == kOrgan) ? 0.06f : 0.09f;
        v.relCoef = std::min(v.relCoef, t60Coef(t, sr_));
        v.stage = kStRelease;
    }

    // Envelope: linear attack, exponential decay to sustain, exponential release.
    static inline float envStep(Voice& v)
    {
        switch (v.stage)
        {
            case kStAttack: v.env += v.attInc; if (v.env >= 1.0f) { v.env = 1.0f; v.stage = kStDecay; } break;
            case kStDecay:  v.env = v.sus + (v.env - v.sus) * v.decCoef; break;
            case kStRelease: v.env *= v.relCoef; break;
            default: break;
        }
        return v.env;
    }

    void renderChunk(float* L, float* R, int n)
    {
        float mixL[kCtrl], mixR[kCtrl], mono[kCtrl];
        std::memset(mixL, 0, sizeof(float) * (size_t)n);
        std::memset(mixR, 0, sizeof(float) * (size_t)n);
        for (Voice& v : voices_)
        {
            if (v.stage == kStOff) continue;
            if (v.gate >= 0) { v.gate -= n; if (v.gate < 0) { v.gate = -1; if (v.stage != kStRelease) { v.stage = kStRelease; v.held = false; } } }
            switch (v.engine)
            {
                case kKeys:  renderKeys(v, mono, n); break;
                case kPad:   updatePadFilter(v); renderPad(v, mono, n); break;
                case kPluck: renderPluck(v, mono, n); break;
                default:     renderOrgan(v, mono, n); break;
            }
            float peak = 0.0f;
            if (v.fading)
            {
                for (int i = 0; i < n; ++i)
                {
                    v.fade -= v.fadeStep;
                    const float g = v.fade > 0.0f ? v.fade : 0.0f;
                    mono[i] *= g;
                }
                if (v.fade <= 0.0f) { v.stage = kStOff; v.fading = false; }
            }
            for (int i = 0; i < n; ++i)
            {
                const float s = mono[i];
                mixL[i] += s * v.gL; mixR[i] += s * v.gR;
                peak = std::max(peak, std::fabs(s));
            }
            // A voice that has gone quiet (released, or a string that has rung out) is freed.
            if (v.stage != kStOff)
            {
                const bool decaying = v.stage == kStRelease || (v.stage == kStDecay && v.sus <= 0.0f) || v.engine == kPluck;
                if (decaying && peak < 2.0e-5f) { v.quiet += n; if (v.quiet > (int)(0.05f * sr_)) v.stage = kStOff; }
                else v.quiet = 0;
                if (v.stage == kStRelease && v.env < 1.0e-5f) v.stage = kStOff;
            }
        }
        postChain(mixL, mixR, n);
        for (int i = 0; i < n; ++i) { L[i] = mixL[i]; if (R) R[i] = mixR[i]; }
    }

    void renderKeys(Voice& v, float* out, int n)
    {
        float ph = v.ph, ph3 = v.ph3, fmI = v.fmI, bell = v.bell;
        const float inc = v.inc, inc3 = v.inc3, fl = v.fmFloor, fd = v.fmDec, bd = v.bellDec, bark = v.bark, lvl = v.level;
        for (int i = 0; i < n; ++i)
        {
            const float m = sineAt(ph);
            float y = sineAt(ph + fmI * m * 0.159155f);
            y += bell * sineAt(ph3);
            y += bark * (y * y - 0.35f);               // pickup asymmetry (a little 2nd harmonic)
            out[i] = y * envStep(v) * lvl;
            ph += inc; if (ph >= 1.0f) ph -= 1.0f;
            ph3 += inc3; if (ph3 >= 1.0f) ph3 -= 1.0f;
            fmI = fl + (fmI - fl) * fd;
            bell *= bd;
        }
        v.ph = ph; v.ph3 = ph3; v.fmI = fmI; v.bell = mpc::dsp::flushDenormal(bell);
    }

    static inline float blepSaw(float ph, float inc)
    {
        float y = 2.0f * ph - 1.0f;
        if (ph < inc) { const float t = ph / inc; y -= t + t - t * t - 1.0f; }
        else if (ph > 1.0f - inc) { const float t = (ph - 1.0f) / inc; y -= t * t + t + t + 1.0f; }
        return y;
    }

    void renderPad(Voice& v, float* out, int n)
    {
        float ph = v.ph, ph2 = v.ph2, sub = v.sub, lo = v.lo, bp = v.bp;
        const float i1 = v.inc / v.detune, i2 = v.inc * v.detune, is = v.inc * 0.5f, f = v.svfF, q = 1.25f, lvl = v.level;
        for (int i = 0; i < n; ++i)
        {
            const float x = 0.5f * (blepSaw(ph, i1) + blepSaw(ph2, i2)) + 0.45f * sineAt(sub);
            lo += f * bp;
            const float hp = x - lo - q * bp;
            bp += f * hp;
            out[i] = lo * envStep(v) * lvl;
            ph += i1; if (ph >= 1.0f) ph -= 1.0f;
            ph2 += i2; if (ph2 >= 1.0f) ph2 -= 1.0f;
            sub += is; if (sub >= 1.0f) sub -= 1.0f;
        }
        v.ph = ph; v.ph2 = ph2; v.sub = sub; v.lo = mpc::dsp::flushDenormal(lo); v.bp = mpc::dsp::flushDenormal(bp);
    }

    void renderPluck(Voice& v, float* out, int n)
    {
        float* buf = v.buf;
        const int mask = v.mask;
        const float fsize = (float)v.size;
        int w = v.wpos;
        float lp = v.lp, dcx = v.dcx, dcy = v.dcy;
        const float D = v.D, lpA = v.lpA, g = v.loopG, lvl = v.level;
        for (int i = 0; i < n; ++i)
        {
            float rp = (float)w - D;
            if (rp < 0.0f) rp += fsize;
            const int i0 = (int)rp;
            const float fr = rp - (float)i0;
            const float xm1 = buf[(i0 - 1) & mask], x0 = buf[i0 & mask], x1 = buf[(i0 + 1) & mask], x2 = buf[(i0 + 2) & mask];
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            const float y = ((c3 * fr + c2) * fr + c1) * fr + x0;
            lp += lpA * (y - lp);
            buf[w] = g * lp;
            w = (w + 1) & mask;
            const float o = y - dcx + 0.995f * dcy;
            dcx = y; dcy = o;
            out[i] = o * envStep(v) * lvl;
        }
        v.wpos = w; v.lp = mpc::dsp::flushDenormal(lp); v.dcx = dcx; v.dcy = mpc::dsp::flushDenormal(dcy);
    }

    void renderOrgan(Voice& v, float* out, int n)
    {
        float ph = v.ph, sub = v.sub, click = v.click;
        const float inc = v.inc, is = v.inc * 0.5f, cd = v.clickDec, lvl = v.level;
        const float b0 = v.bar[0], b1 = v.bar[1], b2 = v.bar[2], b3 = v.bar[3], b4 = v.bar[4], b5 = v.bar[5];
        uint32_t r = v.rng;
        for (int i = 0; i < n; ++i)
        {
            float y = b0 * sineAt(sub) + b1 * sineAt(ph) + b2 * sineAt(2.0f * ph) + b3 * sineAt(3.0f * ph);
            if (b4 > 0.0f) y += b4 * sineAt(4.0f * ph);
            if (b5 > 0.0f) y += b5 * sineAt(6.0f * ph);
            if (click > 1.0e-4f)
            {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                y += click * ((float)(r >> 8) * (1.0f / 8388608.0f) - 1.0f);
                click *= cd;
            }
            out[i] = y * envStep(v) * lvl;
            ph += inc; if (ph >= 1.0f) ph -= 1.0f;
            sub += is; if (sub >= 1.0f) sub -= 1.0f;
        }
        v.ph = ph; v.sub = sub; v.click = click; v.rng = r;
    }

    // ===== output stage ===========================================================================
    void postChain(float* L, float* R, int n)
    {
        const bool filt = s_.filter < 19900.0f;
        const float chorusInc = 0.55f / sr_;
        const float rollA = 1.0f - 0.55f * s_.dust;       // dust darkens the top a little
        for (int i = 0; i < n; ++i)
        {
            float l = L[i], r = R[i];
            // DC blocker (the keys' pickup asymmetry and the strings leave a little DC).
            const float ol = l - dcxL_ + 0.9985f * dcyL_; dcxL_ = l; dcyL_ = ol; l = ol;
            const float orr = r - dcxR_ + 0.9985f * dcyR_; dcxR_ = r; dcyR_ = orr; r = orr;
            if (filt) { l = filtL_.process(l, mpc::dsp::SVF::Mode::Lowpass); r = filtR_.process(r, mpc::dsp::SVF::Mode::Lowpass); }
            if (s_.chorus > 0.001f) chorus_.process(l, r, s_.chorus, chorusInc);
            else { chorus_.dl.push(l); chorus_.dr.push(r); }
            if (s_.room > 0.001f) room_.process(l, r, s_.room);
            else room_.process(l, r, 0.0f);
            L[i] = l; R[i] = r;
        }
        if (s_.warmth > 0.001f)
        {
            // Warmth: tape saturation (unity gain for small signals, rounder and a little compressed when loud,
            // a touch of even harmonics from the bias).
            const float drive = 1.0f + 3.5f * s_.warmth, inv = 1.0f / drive, ev = 0.12f * s_.warmth;
            for (int i = 0; i < n; ++i)
            {
                L[i] = mpc::dsp::fastTanh(drive * (L[i] + ev * L[i] * L[i])) * inv;
                R[i] = mpc::dsp::fastTanh(drive * (R[i] + ev * R[i] * R[i])) * inv;
            }
        }
        wobble_.process(L, R, n, s_.wobble);
        for (int i = 0; i < n; ++i)
        {
            float l = L[i], r = R[i];
            dust_.process(l, r, s_.dust, rollA);
            const float g = outGain_.process();
            L[i] = limit(l * g); R[i] = limit(r * g);
        }
        dcyL_ = mpc::dsp::flushDenormal(dcyL_); dcyR_ = mpc::dsp::flushDenormal(dcyR_);
    }

    // Transparent below -3 dBFS, then a smooth knee to a -0.2 dBFS ceiling.
    static inline float limit(float x)
    {
        const float a = std::fabs(x);
        if (a <= 0.7f) return x;
        const float over = (a - 0.7f) / 0.277f;
        const float y = 0.7f + 0.277f * over / (1.0f + over);
        return x < 0.0f ? -y : y;
    }

    // ===== state ==================================================================================
    float sr_ = 44100.0f;
    float sine_[kTable + 1];
    Voice voices_[kNumSlots];
    std::vector<float> strBuf_;
    uint32_t orderCounter_ = 0, nextChord_ = 1;

    Pending pend_[kMaxPend];
    int numPend_ = 0;
    ChordRec chords_[32];
    int numChords_ = 0;
    int keyStack_[kMaxKeys];
    float keyVel_[kMaxKeys];
    int numKeys_ = 0;
    int prev_[kMaxTones] = {0, 0, 0, 0, 0};
    int prevN_ = 0;
    bool altUp_ = false, pedal_ = false, holdWas_ = false;

    // pattern
    bool patActive_ = false, patChordValid_ = false, patBass_ = false;
    int patPitches_[kMaxChordVoices] = {0, 0, 0, 0, 0, 0};
    int patN_ = 0;
    float patVel_ = 0.8f;
    uint32_t strokeChord_ = 0;
    int arpCount_ = 0, arpLast_ = -1;
    long long lastStep_ = -1000000;
    double freePpq_ = 0.0, hostPpq_ = 0.0, expectPpq_ = -1.0, bpm_ = 120.0, spb_ = 22050.0;
    bool playing_ = false, wasPlaying_ = false;

    long long clock_ = 0;
    int blockPos_ = 0;
    Rng rng_;

    // output
    mpc::dsp::SVF filtL_, filtR_;
    float lastFilter_ = -1.0f;
    float dcxL_ = 0.0f, dcyL_ = 0.0f, dcxR_ = 0.0f, dcyR_ = 0.0f;
    Chorus chorus_;
    Room room_;
    Wobble wobble_;
    Dust dust_;
    mpc::dsp::Smoother outGain_;
};

} // namespace driftwood

using driftwood::Driftwood;
MPC_PLUGIN_ENTRY(Driftwood)
