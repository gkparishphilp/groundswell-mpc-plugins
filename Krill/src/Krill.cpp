// Krill - an old-school chip instrument for the MPC, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// Tiny, everywhere and chirpy: the sound of 8-bit home computers and game consoles.
//
// SIGNAL FLOW (per voice, 8 voices)
//
//   Osc A (pulse / 4-bit triangle / 5-bit saw / LFSR noise) ---------------------+
//   Osc B (same waves, own tuning) -- ring-modulated and/or hard-synced by A ----+--> dry mix
//                                                                                |
//   "Talk": dry mix (+ hiss for consonants) -> 3 formant band-passes stepping     |
//           through a word's phonemes at the frame rate --------------- talk mix -+
//                                                                                |
//   x amp envelope (ADSR, optionally stepped to 16 volume levels) x velocity ----+--> voice out
//
//   Pitch per voice = stepped glide + chord-arp offset + stepped vibrato + bend + pitch sweep.
//   Glide, vibrato, the chord arp and the talk phonemes all move on a global 50/60 Hz "frame"
//   clock, the way music drivers on old consoles updated once per video frame.
//
//   Drum voices (Play Mode Drums or Split) use the same oscillators with a fixed recipe per pad:
//   a pitch that sweeps from a start note, a noise or tone source and an exponential decay.
//
// OUTPUT: sum of voices -> sample-rate reduction (sample and hold) -> bit reduction
//         -> Tone (one-pole low-pass) -> DC blocker -> Volume -> soft clip -> both outputs (mono).
//
// PLAY MODES
//   Synth: every note plays the synth.  Drums: every note plays a drum (16 drums, repeating every
//   16 notes from C1 = note 36).  Split: notes below C2 (48) are drums, the rest play the synth.
//
// ARP
//   Chord: each held note cycles through the chord on its own, one step every "Arp Speed" frames
//          (1 frame = 1/50 or 1/60 s): the classic one-finger chord trill.  Polyphonic.
//   Up / Down / Up-Down / Random: a tempo-synced arpeggiator on one voice. Holding a single note
//          arpeggiates the chord built on it; holding several arpeggiates those notes.
//          Locks to the song position while the MPC is playing; free-runs at the tempo otherwise.
//
// CPU: integer phase accumulators and naive (deliberately aliasing) waves; pitch is recomputed
// every 32 samples, the formant filters only run while Talk is above zero.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/adsr.h"
#include "dsp/noise.h"
#include "dsp/smoother.h"
#include "dsp/tempo.h"

#include <cmath>
#include <cstdint>

using namespace mpc::dsp;

namespace krill {

// ---------------------------------------------------------------------------------------------
// Waves, chords, drums and words
// ---------------------------------------------------------------------------------------------
enum Wave { kPulse = 0, kTri, kSaw, kNoise };
static const float kDuty[4] = {0.125f, 0.25f, 0.375f, 0.5f};
enum OscMod { kModOff = 0, kModRing, kModSync, kModBoth };

struct Chord { const char* name; int n; int tones[4]; };
static const Chord kChords[] = {
    {"Major", 3, {0, 4, 7, 0}}, {"Minor", 3, {0, 3, 7, 0}}, {"Maj7", 4, {0, 4, 7, 11}}, {"Min7", 4, {0, 3, 7, 10}},
    {"Sus4", 3, {0, 5, 7, 0}},  {"Power", 2, {0, 7, 0, 0}}, {"Dim", 3, {0, 3, 6, 0}},   {"Octave", 1, {0, 0, 0, 0}},
};
constexpr int kNumChords = (int)(sizeof(kChords) / sizeof(kChords[0]));

// Length of a chord arpeggio in steps, and the semitone offset of step i.
// An "Octave" chord with one octave would not move, so it always spans at least two.
inline int chordLength(int chord, int octaves)
{
    const Chord& c = kChords[chord];
    if (c.n == 1 && octaves < 2) octaves = 2;
    return c.n * octaves;
}
inline int chordOffset(int chord, int octaves, int step)
{
    const Chord& c = kChords[chord];
    const int len = chordLength(chord, octaves);
    step = ((step % len) + len) % len;
    return c.tones[step % c.n] + 12 * (step / c.n);
}

// One drum per pad. Pitch = note + sweep * (1 -> 0 exponential over sweepMs); positive sweeps fall.
struct DrumRecipe
{
    const char* name;
    int waveA, dutyA; float levelA;
    int waveB, dutyB; float levelB; float bSemi;
    int mod; bool shortNoise;
    float note, sweep, sweepMs, decayMs;
    int choke;   // voices with the same non-zero group cut each other off
};
static const DrumRecipe kDrums[16] = {
    // name        waveA  dA  lvlA   waveB   dB  lvlB  bSemi  mod       short  note   sweep  swMs  decay choke
    {"Kick",       kTri,  0, 1.00f,  kPulse, 3, 0.30f,  0.0f, kModOff,  false, 40.0f,  32.0f,  60, 300, 0},
    {"Snare",      kTri,  0, 0.55f,  kNoise, 0, 0.85f, 52.0f, kModOff,  false, 50.0f,  14.0f,  40, 180, 0},
    {"Closed Hat", kNoise,0, 0.90f,  kPulse, 0, 0.00f,  0.0f, kModOff,  false, 110.0f,  0.0f,  10,  45, 1},
    {"Open Hat",   kNoise,0, 0.80f,  kPulse, 0, 0.00f,  0.0f, kModOff,  false, 110.0f,  0.0f,  10, 380, 1},
    {"Low Tom",    kTri,  0, 1.00f,  kPulse, 3, 0.15f,  0.0f, kModOff,  false, 45.0f,  10.0f, 180, 320, 0},
    {"High Tom",   kTri,  0, 1.00f,  kPulse, 3, 0.15f,  0.0f, kModOff,  false, 55.0f,  10.0f, 150, 260, 0},
    {"Laser",      kPulse,1, 0.80f,  kPulse, 0, 0.00f,  0.0f, kModOff,  false, 60.0f,  36.0f, 220, 320, 0},
    {"Blip",       kPulse,3, 0.75f,  kPulse, 0, 0.00f,  0.0f, kModOff,  false, 79.0f, -12.0f,  25,  90, 0},
    {"Metal",      kNoise,0, 0.85f,  kPulse, 0, 0.00f,  0.0f, kModOff,  true,  70.0f,   0.0f,  10, 220, 0},
    {"Blast",      kNoise,0, 0.90f,  kPulse, 0, 0.00f,  0.0f, kModOff,  false, 72.0f,  30.0f, 600, 900, 0},
    {"Rise",       kPulse,0, 0.75f,  kPulse, 0, 0.00f,  0.0f, kModOff,  false, 72.0f, -24.0f, 400, 450, 0},
    {"Drop",       kTri,  0, 1.00f,  kPulse, 0, 0.00f,  0.0f, kModOff,  false, 33.0f,  24.0f, 500, 600, 0},
    {"Ping",       kTri,  0, 0.50f,  kPulse, 0, 0.60f, 19.0f, kModRing, false, 84.0f,   0.0f,  10, 260, 0},
    {"Click",      kNoise,0, 0.85f,  kPulse, 0, 0.00f,  0.0f, kModOff,  true,  96.0f,   0.0f,  10,  14, 0},
    {"Zap Up",     kPulse,1, 0.75f,  kPulse, 0, 0.00f,  0.0f, kModOff,  false, 84.0f, -36.0f, 120, 160, 0},
    {"Whoosh",     kNoise,0, 0.80f,  kPulse, 0, 0.00f,  0.0f, kModOff,  false, 90.0f, -36.0f, 500, 600, 0},
};
constexpr int kFirstDrumNote = 36;   // C1
constexpr int kSplitNote = 48;       // C2: below it, Split mode plays drums

// Phonemes for the talking voice: three formant frequencies, how much buzz (voiced) and how much
// hiss (noise) feed the formant filters, and whether the sound can be held at the end of a word.
struct Phoneme { float f1, f2, f3, voiced, noise; bool hold; };
enum Ph { SIL = 0, AA, AE, EH, IY, IH, OW, UW, ER, AH, W_, Y_, L_, R_, M_, N_, S_, SH, Z_, H_, BURST, kNumPh };
static const Phoneme kPhonemes[kNumPh] = {
    { 500, 1500, 2500, 0.0f, 0.0f, false},   // SIL   silence / closure
    { 730, 1090, 2440, 1.0f, 0.0f, true },   // AA    "father"
    { 660, 1720, 2410, 1.0f, 0.0f, true },   // AE    "cat"
    { 530, 1840, 2480, 1.0f, 0.0f, true },   // EH    "bed"
    { 270, 2290, 3010, 1.0f, 0.0f, true },   // IY    "see"
    { 390, 1990, 2550, 1.0f, 0.0f, true },   // IH    "sit"
    { 570,  840, 2410, 1.0f, 0.0f, true },   // OW    "go"
    { 300,  870, 2240, 1.0f, 0.0f, true },   // UW    "boot"
    { 490, 1350, 1690, 1.0f, 0.0f, true },   // ER    "bird"
    { 640, 1190, 2390, 1.0f, 0.0f, true },   // AH    "but"
    { 300,  610, 2200, 0.7f, 0.0f, true },   // W
    { 280, 2250, 2900, 0.7f, 0.0f, true },   // Y
    { 360, 1300, 2700, 0.7f, 0.0f, true },   // L
    { 420, 1300, 1600, 0.7f, 0.0f, true },   // R
    { 280,  900, 2200, 0.5f, 0.0f, true },   // M
    { 280, 1700, 2600, 0.5f, 0.0f, true },   // N
    {2500, 4500, 6000, 0.0f, 0.9f, false},   // S
    {1800, 2600, 3500, 0.0f, 0.9f, false},   // SH
    {2500, 4500, 6000, 0.35f,0.6f, false},   // Z
    { 640, 1190, 2390, 0.0f, 0.5f, false},   // H
    {1500, 3000, 4500, 0.0f, 1.0f, false},   // BURST (the release of p, t, k, b, d)
};
struct WordStep { uint8_t ph; uint8_t frames; };   // frames at 50 Hz x Talk Speed
struct Word { const char* name; int n; WordStep steps[8]; };
static const Word kWords[] = {
    {"Ah",    1, {{AA, 24}}},
    {"Uh-Oh", 4, {{AH, 8}, {SIL, 3}, {OW, 8}, {UW, 6}}},
    {"Wah",   2, {{W_, 4}, {AA, 18}}},
    {"Yeah",  3, {{Y_, 4}, {EH, 10}, {AE, 8}}},
    {"Wow",   4, {{W_, 4}, {AA, 8}, {OW, 5}, {UW, 6}}},
    {"Oi",    2, {{OW, 8}, {IY, 12}}},
    {"Hey",   3, {{H_, 4}, {EH, 8}, {IY, 8}}},
    {"No",    3, {{N_, 5}, {OW, 10}, {UW, 6}}},
    {"Yes",   3, {{Y_, 3}, {EH, 10}, {S_, 9}}},
    {"Robot", 7, {{R_, 4}, {OW, 6}, {SIL, 2}, {BURST, 1}, {AA, 7}, {SIL, 2}, {BURST, 2}}},
    {"Bleep", 6, {{SIL, 1}, {BURST, 1}, {L_, 3}, {IY, 12}, {SIL, 2}, {BURST, 1}}},
    {"Zap",   4, {{Z_, 5}, {AE, 10}, {SIL, 2}, {BURST, 1}}},
    {"Ee-Oo", 2, {{IY, 10}, {UW, 12}}},
};
constexpr int kNumWords = (int)(sizeof(kWords) / sizeof(kWords[0]));

// A band-pass formant (TPT state-variable filter), normalised to unity gain at its centre.
struct Formant
{
    float k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, ic1 = 0.0f, ic2 = 0.0f;
    void set(float hz, float q, float sr)
    {
        hz = mpc::clamp(hz, 60.0f, 0.45f * sr);
        const float g = std::tan(kPi * hz / sr);
        k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    float process(float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
        return v1 * k;
    }
    void reset() { ic1 = ic2 = 0.0f; }
};

} // namespace krill

using namespace krill;

class Krill : public mpc::Plugin
{
public:
    // Parameter order is PERMANENT: append new parameters at the end only.
    enum Params
    {
        kVolume = 0,                                   // master volume (parameter 0: harmless when nudged)
        kPlayMode, kVoiceMode,                         // Synth / Drums / Split; Poly / Mono
        kWaveA, kDutyA, kLevelA,                       // oscillator A
        kWaveB, kDutyB, kLevelB, kBSemi, kBDetune,     // oscillator B
        kOscMod, kNoiseMode,                           // ring / sync; LFSR long (hiss) or short (metallic)
        kSweep, kSweepTime, kSweepTo,                  // per-note pitch sweep
        kAttack, kDecay, kSustain, kRelease, kStepEnv, kVelSens,
        kArpMode, kChord, kArpSpeed, kArpDiv, kArpOcts, kArpGate, kFrameRate,
        kVibDepth, kVibSpeed, kVibDelay, kGlide, kBendRange,
        kTalk, kWord, kTalkSpeed, kFormant,
        kBits, kResample, kTone,
        kDrumTune, kDrumDecay, kDrumSweep, kDrumLevel,
        kNumParams
    };
    enum PlayMode { kPlaySynth = 0, kPlayDrums, kPlaySplit };
    enum ArpMode { kArpOff = 0, kArpChord, kArpUp, kArpDown, kArpUpDown, kArpRandom };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Krill";
        i.vendor = "Groundswell";
        i.uniqueId = MPC_FOURCC('G', 'F', 'v', 'q');   // == "uid" in plugin.json. Never change.
        i.version = 1100;            // 1.1.0.0
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Krill() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        const std::vector<std::string> waves = {"Pulse", "Tri", "Saw", "Noise"};
        const std::vector<std::string> duties = {"12.5%", "25%", "37.5%", "50%"};
        std::vector<std::string> chords, words;
        for (const Chord& c : kChords) chords.push_back(c.name);
        for (const Word& w : kWords) words.push_back(w.name);

        addParam(floatParam ("vol",      "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("playmode", "Play Mode", {"Synth", "Drums", "Split"}, 0));
        addParam(choiceParam("voices",   "Voices", {"Poly", "Mono"}, 0));
        // Oscillator A: the main pitch.
        addParam(choiceParam("wavea",    "Wave A", waves, kPulse));
        addParam(choiceParam("dutya",    "Duty A", duties, 1));
        addParam(floatParam ("levela",   "Level A", 0.0f, 100.0f, 100.0f, "%", 0));
        // Oscillator B: tuned against A; ring-modulated / hard-synced by A.
        addParam(choiceParam("waveb",    "Wave B", waves, kPulse));
        addParam(choiceParam("dutyb",    "Duty B", duties, 3));
        addParam(floatParam ("levelb",   "Level B", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam   ("bsemi",    "B Semi", -24, 24, 12, "st"));
        addParam(floatParam ("bdetune",  "B Detune", -50.0f, 50.0f, 0.0f, "ct", 0));
        addParam(choiceParam("oscmod",   "Osc Mod", {"Off", "Ring", "Sync", "Both"}, 0));
        addParam(choiceParam("noisemode","Noise Mode", {"Long", "Short"}, 0));
        // Pitch sweep at note start: positive starts above and falls, negative rises from below.
        addParam(floatParam ("sweep",    "Sweep", -48.0f, 48.0f, 0.0f, "st", 1));
        addParam(logParam   ("sweeptime","Sweep Time", 5.0f, 2000.0f, 120.0f, "ms", 0));
        addParam(choiceParam("sweepto",  "Sweep To", {"Pitch", "Osc B"}, 0));
        // Amp envelope.
        addParam(logParam   ("atk",      "Attack", 1.0f, 3000.0f, 2.0f, "ms", 0));
        addParam(logParam   ("dec",      "Decay", 1.0f, 5000.0f, 300.0f, "ms", 0));
        addParam(floatParam ("sus",      "Sustain", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(logParam   ("rel",      "Release", 1.0f, 5000.0f, 120.0f, "ms", 0));
        addParam(boolParam  ("stepenv",  "Step Env", true));             // 16 volume steps, like a 4-bit volume register
        addParam(floatParam ("velsens",  "Velocity", 0.0f, 100.0f, 50.0f, "%", 0));
        // Arpeggiator.
        addParam(choiceParam("arp",      "Arp", {"Off", "Chord", "Up", "Down", "Up-Down", "Random"}, kArpOff));
        addParam(choiceParam("chord",    "Chord", chords, 0));
        addParam(intParam   ("arpspeed", "Arp Speed", 1, 8, 1, "fr"));    // frames per chord step
        addParam(choiceParam("arpdiv",   "Arp Div", divisionNames(), kDivisionSixteenth));
        addParam(intParam   ("arpocts",  "Arp Octaves", 1, 4, 1));
        addParam(floatParam ("arpgate",  "Arp Gate", 10.0f, 100.0f, 75.0f, "%", 0));
        addParam(choiceParam("framerate","Frame Rate", {"50 Hz", "60 Hz"}, 0));
        // Tracker-style modulation (moves once per frame).
        addParam(floatParam ("vibdepth", "Vib Depth", 0.0f, 2.0f, 0.25f, "st", 2, 0.5f));
        addParam(logParam   ("vibspeed", "Vib Speed", 0.5f, 15.0f, 6.0f, "Hz", 1));
        addParam(floatParam ("vibdelay", "Vib Delay", 0.0f, 2000.0f, 250.0f, "ms", 0, 0.5f));
        addParam(floatParam ("glide",    "Glide", 0.0f, 2000.0f, 0.0f, "ms", 0, 0.5f));
        addParam(intParam   ("bend",     "Bend Range", 0, 24, 2, "st"));
        // Talking voice.
        addParam(floatParam ("talk",     "Talk", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("word",     "Word", words, 0));
        addParam(floatParam ("talkspeed","Talk Speed", 25.0f, 200.0f, 100.0f, "%", 0));
        addParam(floatParam ("formant",  "Formant", -12.0f, 12.0f, 0.0f, "st", 1));
        // Crunch and tone.
        addParam(intParam   ("bits",     "Bits", 2, 16, 16));
        addParam(logParam   ("resample", "Resample", 1000.0f, 44100.0f, 44100.0f, "", 0));   // text from formatValue
        addParam(logParam   ("tone",     "Tone", 500.0f, 20000.0f, 16000.0f, "", 0));        // text from formatValue
        // Drums.
        addParam(floatParam ("drumtune", "Drum Tune", -24.0f, 24.0f, 0.0f, "st", 1));
        addParam(floatParam ("drumdecay","Drum Decay", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("drumsweep","Drum Sweep", 0.0f, 200.0f, 100.0f, "%", 0));
        addParam(floatParam ("drumlevel","Drum Level", 0.0f, 100.0f, 80.0f, "%", 0));

        if (numParams() != kNumParams) mpc::log("Krill: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Pulse Lead", {});
        program("Chord Chirp", {{kDutyA, 0}, {kArpMode, kArpChord}, {kChord, 0}, {kArpSpeed, 1}, {kSustain, 85},
                                {kRelease, 200}, {kVibDepth, 0.0f}});
        program("Minor Arp Run", {{kVoiceMode, 1}, {kArpMode, kArpUpDown}, {kChord, 1}, {kArpOcts, 2},
                                  {kArpDiv, kDivisionSixteenth}, {kArpGate, 60}, {kDecay, 180}, {kSustain, 40},
                                  {kVibDepth, 0.0f}, {kLevelB, 35}, {kWaveB, kPulse}, {kDutyB, 0}, {kBSemi, 12},
                                  {kBDetune, 8}});
        program("Sync Sweep Bass", {{kVoiceMode, 1}, {kWaveA, kTri}, {kLevelA, 45}, {kWaveB, kSaw}, {kLevelB, 90},
                                    {kOscMod, kModSync}, {kBSemi, 12}, {kSweep, 24}, {kSweepTo, 1}, {kSweepTime, 260},
                                    {kGlide, 60}, {kDecay, 400}, {kSustain, 60}, {kVibDepth, 0.0f}, {kBits, 8}});
        program("Ring Bells", {{kWaveA, kTri}, {kLevelA, 50}, {kWaveB, kPulse}, {kDutyB, 3}, {kLevelB, 80},
                               {kBSemi, 19}, {kOscMod, kModRing}, {kDecay, 900}, {kSustain, 0}, {kRelease, 700},
                               {kVibDepth, 0.1f}, {kTone, 9000}});
        program("Talking Toy", {{kVoiceMode, 1}, {kTalk, 100}, {kWord, 3}, {kDutyA, 0}, {kSustain, 100}, {kRelease, 150},
                                {kVibDepth, 0.0f}, {kBits, 6}, {kResample, 8000}, {kTone, 5000}, {kStepEnv, 0}});
        program("Pad Drum Kit", {{kPlayMode, kPlayDrums}, {kResample, 22050}, {kBits, 10}});
        program("Split Kit + Bass", {{kPlayMode, kPlaySplit}, {kVoiceMode, 1}, {kWaveA, kTri}, {kGlide, 40},
                                     {kVibDepth, 0.0f}, {kDecay, 250}, {kSustain, 80}, {kRelease, 60}});

        // ---- 1.1: more programs. Appended only: the eight above keep their numbers. ----
        // Leads
        program("Duo Lead", {{kVoiceMode, 1}, {kDutyA, 3}, {kWaveB, kPulse}, {kDutyB, 1}, {kLevelB, 55}, {kBSemi, 0},
                             {kBDetune, 14}, {kGlide, 45}, {kVibDepth, 0.3f}, {kVibDelay, 300}, {kSustain, 85},
                             {kRelease, 180}, {kVolume, -8}});
        program("Laser Lead", {{kVoiceMode, 1}, {kDutyA, 1}, {kSweep, 12}, {kSweepTime, 70}, {kGlide, 25},
                               {kVibDepth, 0.35f}, {kVibSpeed, 7}, {kVibDelay, 200}, {kDecay, 500}, {kSustain, 75}});
        program("Sync Scream", {{kVoiceMode, 1}, {kLevelA, 0}, {kWaveB, kSaw}, {kLevelB, 100}, {kOscMod, kModSync},
                                {kBSemi, 19}, {kSweep, -12}, {kSweepTo, 1}, {kSweepTime, 900}, {kGlide, 70},
                                {kVibDepth, 0.4f}, {kVibDelay, 350}, {kSustain, 90}, {kBits, 10}});
        program("Pulse Whistle", {{kVoiceMode, 1}, {kDutyA, 0}, {kTone, 5000}, {kVibDepth, 0.5f}, {kVibSpeed, 7.5f},
                                  {kVibDelay, 120}, {kGlide, 60}, {kAttack, 25}, {kSustain, 90}, {kRelease, 200},
                                  {kStepEnv, 0}});
        // Chord trills (frame-rate arp) and tempo arps
        program("Maj7 Shimmer", {{kArpMode, kArpChord}, {kChord, 2}, {kArpSpeed, 2}, {kArpOcts, 2}, {kDutyA, 0},
                                 {kWaveB, kTri}, {kLevelB, 40}, {kBSemi, 12}, {kAttack, 8}, {kDecay, 1200},
                                 {kSustain, 40}, {kRelease, 600}, {kVibDepth, 0.0f}});
        program("Power Trill", {{kArpMode, kArpChord}, {kChord, 5}, {kArpSpeed, 1}, {kArpOcts, 2}, {kFrameRate, 1},
                                {kWaveA, kSaw}, {kBits, 8}, {kSustain, 90}, {kRelease, 150}, {kVibDepth, 0.0f}});
        program("Dim Alarm", {{kArpMode, kArpChord}, {kChord, 6}, {kArpSpeed, 1}, {kFrameRate, 1}, {kDutyA, 3},
                              {kVibDepth, 0.5f}, {kVibSpeed, 10}, {kVibDelay, 0}, {kSustain, 85}});
        program("Random Bleeps", {{kArpMode, kArpRandom}, {kChord, 3}, {kArpOcts, 3}, {kArpDiv, kDivisionSixteenth},
                                  {kArpGate, 40}, {kDutyA, 3}, {kDecay, 90}, {kSustain, 0}, {kRelease, 40},
                                  {kSweep, -5}, {kSweepTime, 15}, {kVibDepth, 0.0f}, {kVolume, -3}});
        program("Triplet Climb", {{kArpMode, kArpUp}, {kChord, 4}, {kArpOcts, 3}, {kArpDiv, 5 /* 1/8T */},
                                  {kArpGate, 90}, {kWaveA, kTri}, {kWaveB, kPulse}, {kDutyB, 0}, {kLevelB, 30},
                                  {kBSemi, 12}, {kSustain, 80}, {kVibDepth, 0.0f}});
        // Basses
        program("Octave Pump Bass", {{kVoiceMode, 1}, {kWaveA, kTri}, {kArpMode, kArpChord}, {kChord, 7}, {kArpSpeed, 3},
                                     {kArpOcts, 2}, {kWaveB, kPulse}, {kDutyB, 0}, {kLevelB, 25}, {kBSemi, 0},
                                     {kSustain, 90}, {kRelease, 60}, {kVibDepth, 0.0f}, {kTone, 6000}});
        program("Saw Grind Bass", {{kVoiceMode, 1}, {kWaveA, kSaw}, {kWaveB, kPulse}, {kDutyB, 0}, {kLevelB, 70},
                                   {kBSemi, -12}, {kBDetune, 6}, {kGlide, 35}, {kBits, 6}, {kResample, 11025},
                                   {kTone, 7000}, {kDecay, 350}, {kSustain, 65}, {kRelease, 60}, {kVibDepth, 0.0f},
                                   {kVolume, -8}});
        // Pads and textures
        program("Chip Pad", {{kDutyA, 2}, {kWaveB, kPulse}, {kDutyB, 3}, {kLevelB, 70}, {kBSemi, 0}, {kBDetune, -12},
                             {kAttack, 450}, {kDecay, 1500}, {kSustain, 75}, {kRelease, 1100}, {kVibDepth, 0.12f},
                             {kVibSpeed, 4}, {kVibDelay, 0}, {kTone, 7000}, {kVolume, -10}});
        program("Metal Drone", {{kWaveA, kNoise}, {kNoiseMode, 1}, {kLevelA, 70}, {kWaveB, kTri}, {kLevelB, 55},
                                {kBSemi, 0}, {kAttack, 60}, {kSustain, 100}, {kRelease, 700}, {kVibDepth, 0.0f},
                                {kTone, 8000}});
        // Sound effects, played from any key
        program("Pickup Chime", {{kArpMode, kArpChord}, {kChord, 4}, {kArpSpeed, 4}, {kDutyA, 3}, {kDecay, 450},
                                 {kSustain, 0}, {kRelease, 300}, {kVibDepth, 0.0f}});
        program("Power-Up", {{kSweep, -24}, {kSweepTime, 700}, {kArpMode, kArpChord}, {kChord, 0}, {kArpSpeed, 1},
                             {kDutyA, 1}, {kDecay, 900}, {kSustain, 0}, {kRelease, 200}, {kVibDepth, 0.0f}});
        program("Noise Blast", {{kWaveA, kNoise}, {kSweep, 24}, {kSweepTime, 600}, {kDecay, 1000}, {kSustain, 0},
                                {kRelease, 300}, {kResample, 8000}, {kVibDepth, 0.0f}});
        // Talking
        program("Robot Chant", {{kTalk, 100}, {kWord, 9}, {kArpMode, kArpUp}, {kChord, 1}, {kArpDiv, kDivisionEighth},
                                {kArpGate, 90}, {kDutyA, 0}, {kBits, 8}, {kResample, 11025}, {kTone, 6000},
                                {kSustain, 100}, {kRelease, 80}, {kVibDepth, 0.0f}, {kStepEnv, 0}});
        program("Wah Bass", {{kVoiceMode, 1}, {kTalk, 85}, {kWord, 2}, {kTalkSpeed, 70}, {kFormant, -5},
                             {kWaveA, kSaw}, {kGlide, 40}, {kSustain, 90}, {kRelease, 90}, {kVibDepth, 0.0f},
                             {kBits, 10}, {kVolume, 0}});
        program("Ee-Oo Choir", {{kTalk, 100}, {kWord, 12}, {kTalkSpeed, 35}, {kFormant, 3}, {kDutyA, 3},
                                {kWaveB, kPulse}, {kDutyB, 1}, {kLevelB, 50}, {kBSemi, 0}, {kBDetune, 10},
                                {kAttack, 250}, {kSustain, 100}, {kRelease, 700}, {kVibDepth, 0.15f}, {kVibSpeed, 5},
                                {kVibDelay, 100}, {kStepEnv, 0}});
        // Kits
        program("Crunch Kit", {{kPlayMode, kPlayDrums}, {kBits, 5}, {kResample, 8000}, {kDrumDecay, 35},
                               {kDrumSweep, 140}, {kTone, 9000}});
        program("Boom Kit", {{kPlayMode, kPlayDrums}, {kDrumTune, -7}, {kDrumDecay, 75}, {kDrumSweep, 160},
                             {kTone, 5000}, {kResample, 22050}});
        program("Split Kit + Arp", {{kPlayMode, kPlaySplit}, {kArpMode, kArpUpDown}, {kChord, 1}, {kArpOcts, 2},
                                    {kArpDiv, kDivisionSixteenth}, {kArpGate, 55}, {kDutyA, 0}, {kDecay, 150},
                                    {kSustain, 30}, {kVibDepth, 0.0f}, {kResample, 22050}, {kBits, 10}});
    }

    // ----- lifecycle ------------------------------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        for (int i = 0; i < kNumVoices; ++i)
        {
            voices_[i].env.setSampleRate(sampleRate);
            voices_[i].rng.reseed(0x51F15EEDu + 7919u * (uint32_t)i);
        }
        volume_.setTime(10.0f, sampleRate);
        dcR_ = 1.0f - kTwoPi * 10.0f / sr_;
        reset();
    }

    void reset() override
    {
        for (Voice& v : voices_) v.kill();
        numHeld_ = 0;
        orderCounter_ = 0;
        lastNote_ = -1;
        sustainPedal_ = false;
        bendSemis_ = 0.0f; bendRaw_ = 0.0f; modWheel_ = 0.0f;
        frameAcc_ = 0.0f;
        arpPending_ = false; arpLastIdx_ = -1; arpFreeBeats_ = 0.0; arpPos_ = 0; arpDir_ = 1; arpGateLeft_ = 0;
        arpRng_.reseed(0xC0FFEEu);
        holdVal_ = 0.0f; holdAcc_ = 1.0f; toneY_ = 0.0f; dcX1_ = 0.0f; dcY1_ = 0.0f;
        lastVoiceMode_ = -1; lastArpPattern_ = -1; lastPlayMode_ = -1;
        volume_.snap(dbToGain(get(kVolume)));
    }

    // ----- audio ----------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx);

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(ctx, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            handleMidi(ev);
        }
        render(ctx, pos, ctx.numFrames);

        // Output stage: resample (sample and hold) -> bits -> tone -> DC block -> volume -> soft clip.
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const bool doHold = s_.resampleInc < 0.999f;
        const bool doBits = s_.bits < 16;
        const float q = (float)(1 << (s_.bits - 1)), invQ = 1.0f / q;
        const bool doTone = s_.toneCoef < 0.999f;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            float x = L[i];
            if (doHold)
            {
                holdAcc_ += s_.resampleInc;
                if (holdAcc_ >= 1.0f) { holdAcc_ -= 1.0f; holdVal_ = x; }
                x = holdVal_;
            }
            if (doBits) x = std::floor(x * q + 0.5f) * invQ;
            if (doTone) { toneY_ = flushDenormal(toneY_ + s_.toneCoef * (x - toneY_)); x = toneY_; }
            const float y = flushDenormal(x - dcX1_ + dcR_ * dcY1_);
            dcX1_ = x; dcY1_ = y;
            const float out = softClip(y * volume_.process());
            L[i] = out;
            if (R) R[i] = out;
        }
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        // Resample and Tone carry no fixed unit, so "Off" / "Open" read cleanly and kHz keeps the text short.
        if (index == kResample || index == kTone)
        {
            char buf[32];
            if (index == kResample && plainValue >= 44000.0f) out = "Off";
            else if (index == kTone && plainValue >= 19500.0f) out = "Open";
            else if (plainValue >= 1000.0f) { std::snprintf(buf, sizeof(buf), "%.1f kHz", (double)plainValue * 0.001); out = buf; }
            else { std::snprintf(buf, sizeof(buf), "%.0f Hz", (double)plainValue); out = buf; }
            return true;
        }
        if (index == kBits && plainValue >= 15.5f) { out = "Off"; return true; }
        return false;
    }

    static constexpr int kNumVoices = 8;

private:
    static constexpr int kChunk = 32;            // pitch / formant update interval in samples
    static constexpr float kVoiceGain = 0.22f;
    static constexpr int kMaxHeld = 16;

    struct Voice
    {
        bool active = false, held = false, sustained = false, isDrum = false;
        int note = -1;
        float vel = 1.0f;
        uint32_t order = 0;
        // pitch (synth)
        float glideNote = 60.0f, glideTarget = 60.0f, glideStep = 0.0f;
        int arpStep = 0, arpFrames = 0;
        float vibPhase = 0.0f, vibValue = 0.0f; int vibWait = 0;
        float sweepEnv = 0.0f;
        // oscillators
        uint32_t phA = 0, phB = 0, incA = 0, incB = 0;
        float nAccA = 0.0f, nAccB = 0.0f, nIncA = 0.0f, nIncB = 0.0f, nOutA = 1.0f, nOutB = 1.0f;
        uint16_t lfsrA = 1, lfsrB = 1;
        int waveA = 0, waveB = 0, mod = 0; uint32_t dutyA = 0, dutyB = 0; float levelA = 1.0f, levelB = 0.0f;
        bool shortNoise = false;
        // amplitude
        ADSR env;
        float drumAmp = 0.0f, drumCoef = 0.0f, drumAttack = 0.0f;
        int drum = 0;
        // talk
        Formant fm[3];
        float wordPos = 0.0f, curF[3] = {500, 1500, 2500}, curV = 0.0f, curN = 0.0f;
        Xorshift32 rng;

        void kill()
        {
            active = held = sustained = isDrum = false; note = -1;
            env.reset(); drumAmp = 0.0f;
            for (Formant& f : fm) f.reset();
        }
    };

    struct Settings
    {
        int playMode, voiceMode, arpMode, chord, arpSpeed, arpOcts, word, bits, noiseShort, oscMod, sweepTo, waveA, waveB;
        bool stepEnv;
        float levelA, levelB, dutyA, dutyB, bOffset, sweep, sweepTimeMs, velSens, arpBeats, arpGate, framePeriod, frameRate;
        float vibDepth, vibSpeed, vibDelayMs, glideMs, talk, talkSpeed, formantRatio, resampleInc, toneCoef;
        float drumTune, drumDecay, drumSweep, drumLevel;
        bool playing; double ppq, bpm;
    } s_{};

    bool patternArp() const { return s_.arpMode >= kArpUp; }
    bool monoVoice() const { return s_.voiceMode == 1 || patternArp(); }
    bool isDrumNote(int note) const { return s_.playMode == kPlayDrums || (s_.playMode == kPlaySplit && note < kSplitNote); }

    void readSettings(const mpc::ProcessContext& ctx)
    {
        s_.playMode = getInt(kPlayMode); s_.voiceMode = getInt(kVoiceMode);
        s_.waveA = getInt(kWaveA); s_.waveB = getInt(kWaveB);
        s_.dutyA = kDuty[mpc::clamp(getInt(kDutyA), 0, 3)]; s_.dutyB = kDuty[mpc::clamp(getInt(kDutyB), 0, 3)];
        s_.levelA = get(kLevelA) * 0.01f; s_.levelB = get(kLevelB) * 0.01f;
        s_.bOffset = (float)getInt(kBSemi) + get(kBDetune) * 0.01f;
        s_.oscMod = getInt(kOscMod); s_.noiseShort = getInt(kNoiseMode);
        s_.sweep = get(kSweep); s_.sweepTimeMs = get(kSweepTime); s_.sweepTo = getInt(kSweepTo);
        s_.stepEnv = getBool(kStepEnv); s_.velSens = get(kVelSens) * 0.01f;
        s_.arpMode = getInt(kArpMode); s_.chord = mpc::clamp(getInt(kChord), 0, kNumChords - 1);
        s_.arpSpeed = mpc::clamp(getInt(kArpSpeed), 1, 8); s_.arpBeats = divisionBeats(getInt(kArpDiv));
        s_.arpOcts = mpc::clamp(getInt(kArpOcts), 1, 4); s_.arpGate = get(kArpGate) * 0.01f;
        s_.frameRate = getInt(kFrameRate) == 1 ? 60.0f : 50.0f;
        s_.framePeriod = sr_ / s_.frameRate;
        s_.vibDepth = get(kVibDepth); s_.vibSpeed = get(kVibSpeed); s_.vibDelayMs = get(kVibDelay);
        s_.glideMs = get(kGlide);
        s_.talk = get(kTalk) * 0.01f; s_.word = mpc::clamp(getInt(kWord), 0, kNumWords - 1);
        s_.talkSpeed = get(kTalkSpeed) * 0.01f; s_.formantRatio = semitonesToRatio(get(kFormant));
        s_.bits = mpc::clamp(getInt(kBits), 2, 16);
        const float rs = get(kResample);
        s_.resampleInc = rs >= 44000.0f ? 1.0f : mpc::clamp(rs / sr_, 0.0f, 1.0f);
        const float tone = get(kTone);
        s_.toneCoef = tone >= 19500.0f ? 1.0f : 1.0f - std::exp(-kTwoPi * tone / sr_);
        s_.drumTune = get(kDrumTune); s_.drumDecay = std::exp2((get(kDrumDecay) - 50.0f) / 25.0f);
        s_.drumSweep = get(kDrumSweep) * 0.01f; s_.drumLevel = get(kDrumLevel) * 0.01f;

        s_.playing = ctx.transport.playing && ctx.transport.ppqValid;
        s_.ppq = ctx.transport.ppqPos;
        s_.bpm = safeBpm(ctx.transport.bpm);

        volume_.setTarget(dbToGain(get(kVolume)));
        const float rel = get(kRelease);
        for (Voice& v : voices_) v.env.set(get(kAttack), get(kDecay), get(kSustain) * 0.01f, rel);

        // Changing how notes are routed while keys are down would strand voices: release them.
        const int pat = patternArp() ? 1 : 0;
        if ((lastVoiceMode_ >= 0 && lastVoiceMode_ != s_.voiceMode) || (lastArpPattern_ >= 0 && lastArpPattern_ != pat)
            || (lastPlayMode_ >= 0 && lastPlayMode_ != s_.playMode))
        {
            for (Voice& v : voices_) if (v.active && !v.isDrum) { v.env.noteOff(); v.held = v.sustained = false; }
            arpPending_ = pat && numHeld_ > 0;
            arpLastIdx_ = -1;
        }
        lastVoiceMode_ = s_.voiceMode; lastArpPattern_ = pat; lastPlayMode_ = s_.playMode;
    }

    // ----- MIDI -----------------------------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            const int note = ev.note();
            const float vel = lerp(1.0f, (float)ev.velocity() / 127.0f, s_.velSens);
            if (isDrumNote(note)) { startDrum(note, vel); return; }
            if (patternArp())
            {
                const bool wasEmpty = numHeld_ == 0;
                addHeld(note, vel);
                if (wasEmpty) { arpPending_ = true; arpPos_ = 0; arpDir_ = 1; arpFreeBeats_ = 0.0; }
            }
            else if (monoVoice())
            {
                addHeld(note, vel);
                Voice& v = voices_[0];
                const bool legato = v.active && !v.isDrum && v.held;
                startSynth(v, note, vel, legato);
            }
            else
            {
                startSynth(voices_[allocVoice(0)], note, vel, false);
            }
        }
        else if (ev.isNoteOff())
        {
            const int note = ev.note();
            if (isDrumNote(note)) return;   // drums are one-shots
            if (patternArp())
            {
                removeHeld(note);
                if (numHeld_ == 0) { arpPending_ = false; Voice& v = voices_[0]; if (v.active && !v.isDrum) { v.env.noteOff(); v.held = false; } }
            }
            else if (monoVoice())
            {
                removeHeld(note);
                Voice& v = voices_[0];
                if (!v.active || v.isDrum || v.note != note) return;
                if (numHeld_ > 0) startSynth(v, held_[numHeld_ - 1].note, held_[numHeld_ - 1].vel, true);
                else { v.env.noteOff(); v.held = false; }
            }
            else
            {
                int best = -1;
                for (int i = 0; i < kNumVoices; ++i)
                {
                    const Voice& v = voices_[i];
                    if (v.active && !v.isDrum && v.held && v.note == note && (best < 0 || v.order < voices_[best].order)) best = i;
                }
                if (best < 0) return;
                voices_[best].held = false;
                if (sustainPedal_) voices_[best].sustained = true;
                else voices_[best].env.noteOff();
            }
        }
        else if (ev.isAllNotesOff())
        {
            for (Voice& v : voices_) if (v.active && !v.isDrum) { v.env.noteOff(); v.held = v.sustained = false; }
            numHeld_ = 0; arpPending_ = false;
        }
        else if (ev.isController())
        {
            if (ev.controller() == 1) modWheel_ = (float)ev.controllerValue() / 127.0f;
            else if (ev.controller() == 64)
            {
                sustainPedal_ = ev.controllerValue() >= 64;
                if (!sustainPedal_)
                    for (Voice& v : voices_) if (v.active && v.sustained) { v.sustained = false; v.env.noteOff(); }
            }
        }
        else if (ev.isPitchBend())
        {
            bendRaw_ = (float)ev.pitchBend() / 8192.0f;
        }
    }

    void addHeld(int note, float vel)
    {
        removeHeld(note);
        if (numHeld_ == kMaxHeld) { for (int i = 1; i < kMaxHeld; ++i) held_[i - 1] = held_[i]; --numHeld_; }
        held_[numHeld_].note = note; held_[numHeld_].vel = vel; ++numHeld_;
    }
    void removeHeld(int note)
    {
        int j = 0;
        for (int i = 0; i < numHeld_; ++i) if (held_[i].note != note) held_[j++] = held_[i];
        numHeld_ = j;
    }

    // Free voice first, then the oldest released one, then the oldest. Voice 0 is reserved in mono.
    int allocVoice(int lo)
    {
        for (int i = lo; i < kNumVoices; ++i) if (!voices_[i].active) return i;
        int best = -1;
        for (int i = lo; i < kNumVoices; ++i)
            if (!voices_[i].held && (best < 0 || voices_[i].order < voices_[best].order)) best = i;
        if (best >= 0) return best;
        for (int i = lo; i < kNumVoices; ++i)
            if (best < 0 || voices_[i].order < voices_[best].order) best = i;
        return best < 0 ? lo : best;
    }

    void startSynth(Voice& v, int note, float vel, bool legato)
    {
        const bool wasActive = v.active && !v.isDrum;
        if (!wasActive) { v.phA = v.phB = 0; v.lfsrA = v.lfsrB = 1; v.glideNote = (float)note; }
        // Stepped portamento from the previous note: equal steps, one per frame.
        const float from = wasActive ? v.glideNote : (lastNote_ >= 0 ? (float)lastNote_ : (float)note);
        const float glideFrames = s_.glideMs * 0.001f * s_.frameRate;
        if (s_.glideMs > 0.0f && glideFrames >= 1.0f && lastNote_ >= 0)
        {
            v.glideNote = from;
            v.glideStep = std::fabs((float)note - from) / glideFrames;
        }
        else { v.glideNote = (float)note; v.glideStep = 0.0f; }
        v.glideTarget = (float)note;
        v.note = note; v.vel = vel; v.active = true; v.held = true; v.sustained = false; v.isDrum = false;
        v.order = ++orderCounter_;
        lastNote_ = note;
        if (!legato)
        {
            v.arpStep = 0; v.arpFrames = 0;
            v.vibPhase = 0.0f; v.vibValue = 0.0f; v.vibWait = (int)(s_.vibDelayMs * 0.001f * s_.frameRate);
            v.sweepEnv = 1.0f;
            v.env.noteOn();
            v.wordPos = 0.0f;
            const Phoneme& p = kPhonemes[kWords[s_.word].steps[0].ph];
            v.curF[0] = p.f1 * s_.formantRatio; v.curF[1] = p.f2 * s_.formantRatio; v.curF[2] = p.f3 * s_.formantRatio;
            v.curV = p.voiced; v.curN = p.noise;
        }
    }

    void startDrum(int note, float vel)
    {
        const int d = (((note - kFirstDrumNote) % 16) + 16) % 16;
        const DrumRecipe& r = kDrums[d];
        if (r.choke > 0)
            for (Voice& o : voices_)
                if (o.active && o.isDrum && kDrums[o.drum].choke == r.choke)
                    o.drumCoef = std::exp(-1.0f / (0.004f * sr_));   // fast 4 ms fade
        Voice& v = voices_[allocVoice(monoVoice() ? 1 : 0)];
        v.active = true; v.held = false; v.sustained = false; v.isDrum = true; v.note = note; v.drum = d;
        v.vel = vel * s_.drumLevel; v.order = ++orderCounter_;
        v.phA = v.phB = 0; v.lfsrA = v.lfsrB = 1; v.nAccA = v.nAccB = 0.0f;
        v.waveA = r.waveA; v.waveB = r.waveB; v.mod = r.mod; v.shortNoise = r.shortNoise;
        v.dutyA = (uint32_t)(kDuty[r.dutyA] * 4294967295.0f); v.dutyB = (uint32_t)(kDuty[r.dutyB] * 4294967295.0f);
        v.levelA = r.levelA; v.levelB = r.levelB;
        v.sweepEnv = 1.0f;
        v.drumAmp = 0.0f; v.drumAttack = 1.0f / (0.0007f * sr_);   // 0.7 ms attack: punchy, no hard click
        v.drumCoef = std::exp(-6.9f / (r.decayMs * s_.drumDecay * 0.001f * sr_));   // -60 dB over decayMs
    }

    // ----- the frame clock ------------------------------------------------------------------------
    void frameTick()
    {
        const int chordLen = chordLength(s_.chord, s_.arpOcts);
        const float vibInc = s_.vibSpeed / s_.frameRate;
        for (Voice& v : voices_)
        {
            if (!v.active || v.isDrum) continue;
            // stepped glide
            if (v.glideNote != v.glideTarget)
            {
                if (v.glideStep <= 0.0f) v.glideNote = v.glideTarget;
                else if (v.glideNote < v.glideTarget) v.glideNote = std::fmin(v.glideNote + v.glideStep, v.glideTarget);
                else v.glideNote = std::fmax(v.glideNote - v.glideStep, v.glideTarget);
            }
            // frame-rate chord arpeggio
            if (s_.arpMode == kArpChord && ++v.arpFrames >= s_.arpSpeed) { v.arpFrames = 0; v.arpStep = (v.arpStep + 1) % chordLen; }
            // stepped vibrato: a triangle sampled once per frame, after the delay
            if (v.vibWait > 0) --v.vibWait;
            else
            {
                v.vibPhase += vibInc; v.vibPhase -= std::floor(v.vibPhase);
                float ph = v.vibPhase + 0.25f; if (ph >= 1.0f) ph -= 1.0f;    // start at the centre, going up
                const float tri = 1.0f - 4.0f * std::fabs(ph - 0.5f);         // -1..1
                v.vibValue = tri * (s_.vibDepth + modWheel_ * 0.5f);
            }
            v.wordPos += s_.talkSpeed;
        }
    }

    // ----- the tempo arpeggiator ------------------------------------------------------------------
    int buildArpNotes(int* out) const
    {
        int n = 0;
        if (numHeld_ == 1)
        {
            const int len = chordLength(s_.chord, s_.arpOcts);
            for (int i = 0; i < len && n < 32; ++i) out[n++] = held_[0].note + chordOffset(s_.chord, s_.arpOcts, i);
        }
        else
        {
            int sorted[kMaxHeld];
            for (int i = 0; i < numHeld_; ++i) sorted[i] = held_[i].note;
            for (int i = 1; i < numHeld_; ++i) { int k = sorted[i], j = i - 1; while (j >= 0 && sorted[j] > k) { sorted[j + 1] = sorted[j]; --j; } sorted[j + 1] = k; }
            for (int o = 0; o < s_.arpOcts; ++o)
                for (int i = 0; i < numHeld_ && n < 64; ++i) out[n++] = sorted[i] + 12 * o;
        }
        return n;
    }

    void arpStep(float stepSamples)
    {
        int notes[64];
        const int n = buildArpNotes(notes);
        if (n <= 0) return;
        int idx = 0;
        switch (s_.arpMode)
        {
            case kArpUp:   idx = arpPos_ % n; arpPos_ = (arpPos_ + 1) % n; break;
            case kArpDown: idx = (n - 1) - (arpPos_ % n); arpPos_ = (arpPos_ + 1) % n; break;
            case kArpUpDown:
                if (n == 1) { idx = 0; break; }
                if (arpPos_ >= n) arpPos_ = n - 1;
                idx = arpPos_;
                if (arpPos_ + arpDir_ >= n || arpPos_ + arpDir_ < 0) arpDir_ = -arpDir_;
                arpPos_ += arpDir_;
                break;
            default:       idx = (int)(arpRng_.next() % (uint32_t)n); break;
        }
        Voice& v = voices_[0];
        if (v.isDrum) v.kill();
        startSynth(v, notes[idx], held_[numHeld_ - 1].vel, false);   // velocity of the newest key
        arpGateLeft_ = (int)(stepSamples * s_.arpGate);
    }

    void runArp(int chunkStart, int n)
    {
        if (!patternArp()) return;
        const float samplesPerBeat = 60.0f / (float)s_.bpm * sr_;
        const float stepSamples = s_.arpBeats * samplesPerBeat;
        if (numHeld_ > 0)
        {
            const double beats = s_.playing ? s_.ppq + (double)chunkStart / samplesPerBeat : arpFreeBeats_;
            const long long idx = (long long)std::floor(beats / (double)s_.arpBeats + 1.0e-6);
            if (arpPending_ || idx != arpLastIdx_) { arpStep(stepSamples); arpPending_ = false; }
            arpLastIdx_ = idx;
            arpFreeBeats_ += (double)n / samplesPerBeat;
        }
        if (arpGateLeft_ > 0)
        {
            arpGateLeft_ -= n;
            if (arpGateLeft_ <= 0) { Voice& v = voices_[0]; if (v.active && !v.isDrum) { v.env.noteOff(); v.held = false; } }
        }
    }

    // ----- rendering ------------------------------------------------------------------------------
    static inline float oscOut(int wave, uint32_t ph, uint32_t duty, float noise)
    {
        switch (wave)
        {
            case kPulse: return ph < duty ? 1.0f : -1.0f;
            case kTri:  { const int s = (int)(ph >> 27); const int lvl = s < 16 ? s : 31 - s; return (float)lvl * (2.0f / 15.0f) - 1.0f; }
            case kSaw:   return (float)(ph >> 27) * (2.0f / 31.0f) - 1.0f;
            default:     return noise;
        }
    }
    static inline void clockNoise(float& acc, float inc, uint16_t& lfsr, float& out, bool shortMode)
    {
        acc += inc;
        while (acc >= 1.0f)
        {
            acc -= 1.0f;
            const uint16_t fb = (uint16_t)((lfsr ^ (lfsr >> (shortMode ? 6 : 1))) & 1u);
            lfsr = (uint16_t)((lfsr >> 1) | (fb << 14));
        }
        out = (lfsr & 1u) ? -1.0f : 1.0f;
    }
    inline uint32_t hzToInc(float hz) const { return (uint32_t)(mpc::clamp(hz, 0.0f, 0.49f * sr_) / sr_ * 4294967296.0f); }
    static inline float noteHz(float note) { return 440.0f * std::exp2((note - 69.0f) * (1.0f / 12.0f)); }

    void render(mpc::ProcessContext& ctx, int from, int to)
    {
        float* out = ctx.outputs[0];
        while (from < to)
        {
            const int n = (to - from) < kChunk ? (to - from) : kChunk;
            if (frameAcc_ >= s_.framePeriod) { frameAcc_ -= s_.framePeriod; if (frameAcc_ > s_.framePeriod) frameAcc_ = 0.0f; frameTick(); }
            frameAcc_ += (float)n;
            runArp(from, n);
            bendSemis_ = bendRaw_ * (float)getInt(kBendRange);
            const float sweepDecay = std::exp(-5.0f * (float)n / (s_.sweepTimeMs * 0.001f * sr_));

            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (!v.active) continue;
                if (v.isDrum) renderDrum(v, out + from, n);
                else renderSynth(v, out + from, n, sweepDecay);
            }
            from += n;
        }
    }

    void setupPitch(Voice& v, float noteA, float noteB)
    {
        const float hzA = noteHz(noteA), hzB = noteHz(noteB);
        v.incA = hzToInc(hzA); v.incB = hzToInc(hzB);
        const float mult = v.shortNoise ? 93.0f : 32.0f;   // short mode: the 93-step loop sounds at the note's pitch
        v.nIncA = mpc::clamp(hzA * mult / sr_, 0.0f, 8.0f);
        v.nIncB = mpc::clamp(hzB * mult / sr_, 0.0f, 8.0f);
    }

    void renderSynth(Voice& v, float* out, int n, float sweepDecay)
    {
        // Settings are read per chunk so knob moves are heard on held notes.
        v.waveA = s_.waveA; v.waveB = s_.waveB; v.mod = s_.oscMod; v.shortNoise = s_.noiseShort == 1;
        v.dutyA = (uint32_t)(s_.dutyA * 4294967295.0f); v.dutyB = (uint32_t)(s_.dutyB * 4294967295.0f);
        v.levelA = s_.levelA; v.levelB = s_.levelB;

        const float arpOff = s_.arpMode == kArpChord ? (float)chordOffset(s_.chord, s_.arpOcts, v.arpStep) : 0.0f;
        const float base = v.glideNote + arpOff + v.vibValue + bendSemis_;
        const float sw = s_.sweep * v.sweepEnv;
        v.sweepEnv *= sweepDecay;
        setupPitch(v, base + (s_.sweepTo == 0 ? sw : 0.0f), base + s_.bOffset + sw);

        // Talking voice: find this frame's phoneme and glide the formants towards it.
        const bool talk = s_.talk > 0.001f;
        if (talk)
        {
            const Word& w = kWords[s_.word];
            float acc = 0.0f; int ph = -1;
            for (int i = 0; i < w.n; ++i) { acc += (float)w.steps[i].frames; if (v.wordPos < acc) { ph = w.steps[i].ph; break; } }
            if (ph < 0) { const int last = w.steps[w.n - 1].ph; ph = kPhonemes[last].hold ? last : SIL; }
            const Phoneme& p = kPhonemes[ph];
            const float tf[3] = {p.f1 * s_.formantRatio, p.f2 * s_.formantRatio, p.f3 * s_.formantRatio};
            static const float kQ[3] = {6.0f, 10.0f, 12.0f};
            for (int k = 0; k < 3; ++k) { v.curF[k] += 0.3f * (tf[k] - v.curF[k]); v.fm[k].set(v.curF[k], kQ[k], sr_); }
            v.curV += 0.35f * (p.voiced - v.curV);
            v.curN += 0.35f * (p.noise - v.curN);
        }
        const float talkMix = s_.talk, dryMix = 1.0f - s_.talk;
        const float gain = kVoiceGain * v.vel;
        const bool ring = v.mod == kModRing || v.mod == kModBoth, sync = v.mod == kModSync || v.mod == kModBoth;

        for (int i = 0; i < n; ++i)
        {
            if (v.waveA == kNoise) clockNoise(v.nAccA, v.nIncA, v.lfsrA, v.nOutA, v.shortNoise);
            if (v.waveB == kNoise) clockNoise(v.nAccB, v.nIncB, v.lfsrB, v.nOutB, v.shortNoise);
            const float a = oscOut(v.waveA, v.phA, v.dutyA, v.nOutA);
            float b = oscOut(v.waveB, v.phB, v.dutyB, v.nOutB);
            if (ring && (v.phA & 0x80000000u)) b = -b;
            const uint32_t prevA = v.phA;
            v.phA += v.incA;
            v.phB += v.incB;
            if (sync && v.phA < prevA) v.phB = 0;   // A wrapped: restart B
            float x = a * v.levelA + b * v.levelB;
            if (talk)
            {
                const float exc = x * v.curV + v.rng.nextBipolar() * v.curN;
                const float t = v.fm[0].process(exc) + 0.8f * v.fm[1].process(exc) + 0.5f * v.fm[2].process(exc);
                x = x * dryMix + t * 2.2f * talkMix;
            }
            float env = v.env.process();
            if (s_.stepEnv) env = std::floor(env * 15.0f + 0.5f) * (1.0f / 15.0f);
            out[i] += x * env * gain;
        }
        if (!v.env.isActive()) v.kill();
    }

    void renderDrum(Voice& v, float* out, int n)
    {
        const DrumRecipe& r = kDrums[v.drum];
        const float sweepDecay = std::exp(-5.0f * (float)n / (r.sweepMs * 0.001f * sr_));
        const float note = r.note + s_.drumTune + r.sweep * s_.drumSweep * v.sweepEnv;
        v.sweepEnv *= sweepDecay;
        setupPitch(v, note, note + r.bSemi);
        const bool ring = v.mod == kModRing;
        const float gain = kVoiceGain * 1.4f * v.vel;
        for (int i = 0; i < n; ++i)
        {
            if (v.waveA == kNoise) clockNoise(v.nAccA, v.nIncA, v.lfsrA, v.nOutA, v.shortNoise);
            if (v.waveB == kNoise) clockNoise(v.nAccB, v.nIncB, v.lfsrB, v.nOutB, false);
            const float a = oscOut(v.waveA, v.phA, v.dutyA, v.nOutA);
            float b = oscOut(v.waveB, v.phB, v.dutyB, v.nOutB);
            if (ring && (v.phA & 0x80000000u)) b = -b;
            v.phA += v.incA; v.phB += v.incB;
            if (v.drumAttack > 0.0f)
            {
                v.drumAmp += v.drumAttack;
                if (v.drumAmp >= 1.0f) { v.drumAmp = 1.0f; v.drumAttack = 0.0f; }
            }
            else v.drumAmp *= v.drumCoef;
            out[i] += (a * v.levelA + b * v.levelB) * v.drumAmp * gain;
        }
        if (v.drumAttack <= 0.0f && v.drumAmp < 0.0003f) v.kill();
    }

    // ----- state ----------------------------------------------------------------------------------
    struct Held { int note; float vel; };

    Voice voices_[kNumVoices];
    Held held_[kMaxHeld] = {};
    int numHeld_ = 0;
    uint32_t orderCounter_ = 0;
    int lastNote_ = -1;
    bool sustainPedal_ = false;
    float bendRaw_ = 0.0f, bendSemis_ = 0.0f, modWheel_ = 0.0f;
    float sr_ = 44100.0f;
    float frameAcc_ = 0.0f;
    // tempo arp
    bool arpPending_ = false;
    long long arpLastIdx_ = -1;
    double arpFreeBeats_ = 0.0;
    int arpPos_ = 0, arpDir_ = 1, arpGateLeft_ = 0;
    Xorshift32 arpRng_;
    int lastVoiceMode_ = -1, lastArpPattern_ = -1, lastPlayMode_ = -1;
    // output stage
    float holdVal_ = 0.0f, holdAcc_ = 1.0f, toneY_ = 0.0f, dcX1_ = 0.0f, dcY1_ = 0.0f, dcR_ = 0.999f;
    Smoother volume_;
};

MPC_PLUGIN_ENTRY(Krill)
