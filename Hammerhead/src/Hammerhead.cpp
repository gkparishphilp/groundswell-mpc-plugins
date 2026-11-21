// Hammerhead - struck and plucked physical modelling for the MPC, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// Hit something and it rings: marimba, vibraphone, xylophone and glockenspiel bars, kalimba
// tines, bells, glass, a steel pan, and plucked or hammered strings. Every note is a mallet (or
// plectrum) hitting a resonating object, so the attack does the talking: how hard the mallet is,
// where it lands, what the object is made of and how it is damped.
//
// WHY THIS MODEL (and what it costs)
//
//   Bars, tines, bells, glass and pans are inharmonic and dominated by a handful of strong modes,
//   so they are modelled MODALLY: 8 two-pole resonators per voice, each tuned to one mode of the
//   object, plus one more for the tuned resonator tube under a marimba bar. A free-ringing mode
//   costs 3 multiplies and 2 adds per sample, no table look-ups and no transcendental functions.
//   Strings are harmonic with dozens of audible partials, which a modal bank would need 30+
//   resonators for, so the two string models use a WAVEGUIDE instead (Karplus-Strong family): one
//   delay line, a cubic read, a one-pole loss filter and a gain, about 20 operations per sample
//   whatever the pitch. Everything that needs exp/cos/pow runs once per note, once per 32-sample
//   tick while the pitch is moving, or when a knob moves - never per sample.
//   Worst case, 16 voices all ringing: ~16 x 50 = 800 simple operations per sample (a bowed or
//   blown modal voice is ~90). Voices that have died away (-90 dB for 100 ms) cost nothing.
//
// SIGNAL FLOW (per voice, 16 voices)
//
//   EXCITER  a half-sine force pulse whose length is the mallet's contact time ("Hardness", made
//            harder by velocity via "Vel>Hard"): a soft mallet stays on the bar for ~5 ms and its
//            pulse has no energy above a few hundred Hz, so only the low modes ring; a hard one
//            is a 0.1 ms tap that rings everything. Plus a burst of band-passed impact noise:
//            a little goes into the resonators (no two hits are identical), and "Click" sends it
//            straight to the output - the thwack of the mallet itself.
//   MODAL    8 resonators at the object's mode ratios ("Model"; "Stretch" bends the ratios).
//            "Strike Pos" weights each mode by its shape at the strike point (beam mode shapes
//            for bars: hitting the centre kills the antisymmetric modes, hitting near the node
//            kills the fundamental; a dark/bright tilt for bells, glass, tines and pans).
//            Decay: the lowest mode rings for "Decay" (T60, shorter for high notes, as on real
//            instruments); "Damping" makes each higher mode die faster (wood) or not (metal).
//            "Resonator": a tuned tube driven by the fundamental - louder, rounder, a slight bloom.
//   STRING   delay line tuned to the note, loss filter set by "Damping", loop gain by "Decay";
//            the exciter is fed in twice, the second copy inverted and delayed by the strike
//            point ("Strike Pos": the middle of the string is hollow, near the bridge is thin).
//   "Drop"   the pitch starts up to 12 semitones sharp and falls to the note over "Drop Time"
//            (harder hits drop further): log drums, toms, tabla-ish bends, sonar pings.
//   "Mute"   how hard a note-off (or sustain pedal up) chokes the ring; 0 = ring out like a
//            marimba, 100 = a palm on the bar.
//   Each voice is panned by pitch ("Width"), like standing in front of a keyboard instrument.
//
// EXCITERS (1.1, "Exciter"): Mallet is the strike above. Bow and Blow keep feeding the object
//   while the note is held, so it sustains, and build up over "Swell" ("Pressure", and pad
//   aftertouch, play them; "Air" is rosin scratch or breath noise):
//   BOW, modal   stick-slip friction at the contact point: the bow drags the bar along until the
//                bar's own velocity tears it free (the classic bow table, a power law in the
//                relative velocity). The contact velocity is the sum of the mode velocities, each
//                normalised so every mode's resonant gain is its table strength whatever Decay is.
//   BOW, string  the string is split at the bow point into a nut-side and a bridge-side delay line
//                with the friction junction between them (the McIntyre-Schumacher-Woodhouse /
//                Smith bowed string).
//   BLOW         a jet: the contact velocity, through a saturating gain, pushes back on the object
//                (on the strings, on the waveguide), so it self-oscillates at its own resonance
//                once the breath is strong enough, plus turbulent breath noise. More pressure
//                overblows: brighter, and it can jump to an upper mode.
//   "Vibrato" bends the pitch at "Motor Rate", fading in over 0.4 s after each note starts.
//
// BUZZ (1.2): a loose buzzer (kalimba bottle caps, the membranes on balafon gourds, a sitar-ish
//   bridge) touches the vibrating object whenever it swings past a threshold, a fraction of the
//   note's own peak: the part of the wave beyond it, roughened by contact noise and high-passed, is
//   added to the voice. So it rattles while the note is loud and cuts out as the ring decays.
//
// MODULATION (1.2): one LFO (free in Hz, or locked to the song; Sine, Triangle, Square, Ramp,
//   S&H, Drift) with a depth knob per destination: Strike Pos and Hardness (read at each hit, so
//   a sequence of hits walks across the bar; a bow moves along it), Stretch (the overtones bend
//   while the note rings) and Pressure (bow and breath swell). Plus per-hit randomness: position,
//   hardness, pitch and level, so repeated notes never sound copy-pasted.
//
// OUTPUT   voices -> "Motor" tremolo (the vibraphone's spinning discs; mod wheel adds to it)
//          -> "Body" (four resonant band-passes, a wooden box; "Body Size" moves them)
//          -> "Room" (8-line feedback delay network, "Room Size" = T60)
//          -> "Volume" -> soft limiter with a -0.2 dBFS ceiling (transparent below -3 dBFS).

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/smoother.h"
#include "dsp/svf.h"
#include "dsp/tempo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace mpc::dsp;

namespace hammerhead {

constexpr int kNumVoices = 16;     // polyphony
constexpr int kNumSlots  = 20;     // voice slots: 4 spare, so a stolen voice can fade while the new note starts
constexpr int kModes     = 8;      // the driven render sums them as a tree of 8 (see renderModalDriven)
constexpr int kCtrl      = 32;      // samples per control tick (pitch drop, voice housekeeping)
constexpr float kVoiceGain = 0.30f; // overall voice level (calibrated by tests/listentest.cpp)

enum Engine  { kModal = 0, kString };
enum Excite  { kMallet = 0, kBow, kBlow };
enum LfoShape { kLfoSine = 0, kLfoTri, kLfoSquare, kLfoRamp, kLfoSH, kLfoDrift };
static const char* const kLfoDivNames[] = {"4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/4T", "1/8T", "1/16T"};
static const float kLfoDivBeats[] = {16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 2.0f / 3.0f, 1.0f / 3.0f, 1.0f / 6.0f};
constexpr int kNumLfoDivs = 10;
constexpr float kBowGain   = 40.0f;   // modal bow: friction force gain (x 1 / the note's mode)
constexpr float kOutBowModal = 1.8f, kOutBlowModal = 0.85f, kOutBowString = 2.1f, kOutBlowString = 0.9f;
enum PosType { kPosBeam = 0, kPosTilt };

// One object. ratio: mode frequency / note frequency. amp: relative strength. dec: decay
// multiplier on top of the Damping law (weak torsional modes die fast). beam: bending-mode
// number for the beam position response (0 = not affected by where it is hit).
struct ModelDef
{
    int engine, posType;
    float ratio[kModes], amp[kModes], dec[kModes];
    int beam[kModes];
    float noiseIn;      // how much mallet noise enters the resonators (relative to the pulse)
    float contact;      // contact-time multiplier (plectrum vs. yarn mallet)
    float dBase, dSpan; // strings: loop low-pass amount = dBase + dSpan * Damping
    float gain;         // level trim so every model sits at about the same loudness
};

// Mode tables: tuned bar ratios for marimba (1:4:10), vibraphone (1:4:10) and xylophone (1:3:6),
// the untuned free bar for the glockenspiel (1, 2.76, 5.40, 8.93 ...), a clamped-free tine for the
// kalimba (1, 6.27, 17.55, 34.39), a minor-third church bell (hum, prime, tierce, quint, nominal...),
// a glass bowl with split (beating) doublets, and a steel pan's tuned octave and twelfth. The
// small extra modes are torsional and plate modes that give each object its grit.
static const ModelDef kModels[] = {
    // Marimba
    {kModal, kPosBeam,
     {1.0f, 2.04f, 3.98f, 6.1f, 9.38f, 12.9f, 14.8f, 20.0f},
     {1.0f, 0.08f, 0.85f, 0.06f, 0.50f, 0.05f, 0.22f, 0.10f},
     {1.0f, 0.30f, 1.0f, 0.30f, 1.0f, 0.25f, 1.0f, 1.0f},
     {1, 0, 2, 0, 3, 0, 4, 5}, 0.25f, 1.0f, 0.0f, 0.0f, 1.20f},
    // Vibes
    {kModal, kPosBeam,
     {1.0f, 2.6f, 3.99f, 6.6f, 9.9f, 13.4f, 16.2f, 21.5f},
     {1.0f, 0.06f, 0.60f, 0.05f, 0.35f, 0.04f, 0.14f, 0.08f},
     {1.0f, 0.40f, 1.0f, 0.30f, 1.0f, 0.30f, 1.0f, 1.0f},
     {1, 0, 2, 0, 3, 0, 4, 5}, 0.20f, 1.0f, 0.0f, 0.0f, 1.10f},
    // Xylophone
    {kModal, kPosBeam,
     {1.0f, 2.3f, 3.0f, 5.2f, 6.0f, 8.9f, 11.6f, 15.1f},
     {1.0f, 0.06f, 0.90f, 0.06f, 0.55f, 0.32f, 0.20f, 0.12f},
     {1.0f, 0.30f, 1.0f, 0.30f, 1.0f, 1.0f, 1.0f, 1.0f},
     {1, 0, 2, 0, 3, 4, 5, 6}, 0.30f, 0.8f, 0.0f, 0.0f, 1.30f},
    // Glock
    {kModal, kPosBeam,
     {1.0f, 2.756f, 3.9f, 5.404f, 8.933f, 13.344f, 18.64f, 24.8f},
     {1.0f, 0.45f, 0.05f, 0.30f, 0.18f, 0.10f, 0.06f, 0.04f},
     {1.0f, 1.0f, 0.30f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
     {1, 2, 0, 3, 4, 5, 6, 7}, 0.20f, 0.6f, 0.0f, 0.0f, 1.00f},
    // Kalimba
    {kModal, kPosTilt,
     {1.0f, 3.0f, 5.4f, 6.27f, 11.0f, 17.55f, 24.5f, 34.39f},
     {1.0f, 0.05f, 0.06f, 0.50f, 0.04f, 0.22f, 0.03f, 0.10f},
     {1.0f, 0.40f, 0.40f, 0.70f, 0.30f, 0.50f, 0.30f, 0.40f},
     {0, 0, 0, 0, 0, 0, 0, 0}, 0.30f, 0.8f, 0.0f, 0.0f, 1.25f},
    // Bell
    {kModal, kPosTilt,
     {0.5f, 1.0f, 1.2f, 1.5f, 2.0f, 2.5f, 2.67f, 3.0f},
     {0.55f, 0.80f, 0.60f, 0.25f, 1.0f, 0.35f, 0.30f, 0.25f},
     {1.6f, 1.0f, 0.80f, 0.60f, 0.70f, 0.45f, 0.40f, 0.35f},
     {0, 0, 0, 0, 0, 0, 0, 0}, 0.20f, 0.7f, 0.0f, 0.0f, 0.78f},
    // Glass
    {kModal, kPosTilt,
     {1.0f, 1.0015f, 2.32f, 2.325f, 4.25f, 6.63f, 9.38f, 12.5f},
     {0.70f, 0.50f, 0.30f, 0.20f, 0.20f, 0.12f, 0.08f, 0.05f},
     {1.0f, 1.0f, 0.90f, 0.90f, 0.80f, 0.70f, 0.60f, 0.50f},
     {0, 0, 0, 0, 0, 0, 0, 0}, 0.10f, 0.8f, 0.0f, 0.0f, 0.95f},
    // Steel Pan
    {kModal, kPosTilt,
     {1.0f, 1.51f, 2.0f, 3.0f, 4.02f, 5.03f, 6.05f, 8.1f},
     {1.0f, 0.06f, 0.75f, 0.40f, 0.25f, 0.10f, 0.08f, 0.04f},
     {1.0f, 0.30f, 0.90f, 0.70f, 0.60f, 0.50f, 0.40f, 0.30f},
     {0, 0, 0, 0, 0, 0, 0, 0}, 0.25f, 0.9f, 0.0f, 0.0f, 0.80f},
    // Pluck (nylon-ish string, plectrum or finger)
    {kString, kPosTilt, {}, {}, {}, {}, 0.50f, 0.6f, 0.15f, 0.60f, 0.62f},
    // Dulcimer (metal string, hammer)
    {kString, kPosTilt, {}, {}, {}, {}, 0.20f, 0.5f, 0.02f, 0.50f, 0.60f},
};
constexpr int kNumModels = (int)(sizeof(kModels) / sizeof(kModels[0]));
static const char* const kModelNames[kNumModels] = {
    "Marimba", "Vibes", "Xylophone", "Glock", "Kalimba", "Bell", "Glass", "Steel Pan", "Pluck", "Dulcimer"};

// Soft limiter: untouched below 0.7 (-3.1 dBFS), then a tanh knee to a 0.98 ceiling.
inline float limitOut(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.7f) return x;
    const float y = 0.7f + 0.28f * fastTanh((a - 0.7f) * (1.0f / 0.28f));
    return x < 0.0f ? -y : y;
}

// ---------------------------------------------------------------------------------------------
// The mallet: a half-sine force pulse (a recursive sine, no table) plus a burst of impact noise.
// ---------------------------------------------------------------------------------------------
struct Exciter
{
    int wait = 0, t = 0, len = 0, nlen = 0;
    float c2 = 0.0f, s1 = 0.0f, s2 = 0.0f, amp = 0.0f;
    float env = 0.0f, envK = 0.0f, k1 = 0.0f, k2 = 0.0f, lp1 = 0.0f, lp2 = 0.0f, nRes = 0.0f, nOut = 0.0f;
    uint32_t rng = 1u;

    bool active() const { return wait > 0 || t < len || t < nlen; }

    // res: force into the resonator. out: impact noise for the output ("Click").
    inline void next(float& res, float& out)
    {
        if (wait > 0) { --wait; res = out = 0.0f; return; }
        float r = 0.0f, o = 0.0f;
        if (t < len)
        {
            const float s = c2 * s1 - s2;
            s2 = s1; s1 = s;
            r = amp * s;
        }
        if (t < nlen)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const float wn = (float)(int32_t)rng * 4.656613e-10f;
            lp1 += k1 * (wn - lp1);
            lp2 += k2 * (lp1 - lp2);
            const float nz = (lp1 - lp2) * env;
            env *= envK;
            r += nRes * nz;
            o = nOut * nz;
        }
        ++t;
        res = r; out = o;
    }
};

enum Stage { kOff = 0, kOn, kSteal };

struct Voice
{
    int stage = kOff;
    int note = -1, model = 0;
    bool held = false, sustained = false, muted = false, choked = false;
    uint32_t order = 0;
    float hz = 261.6f;               // note frequency (with Octave and Fine), before bend and drop
    float level = 0.0f;              // velocity level
    float gL = 0.7f, gR = 0.7f;      // pan
    float fade = 0.0f, fadeStep = 0.0f;   // steal fade: 0 -> 1 over ~8 ms (smoothstep, so no slope kinks)
    int pendNote = -1; float pendVel = 0.0f;
    float peak = 0.0f; int quiet = 0;
    float dropSt = 0.0f;             // current pitch offset of the drop, semitones
    Exciter ex, exB;                 // exB: the delayed, inverted copy for strings (strike position)

    // modal
    float w[kModes] = {}, r[kModes] = {}, ratio[kModes] = {};
    float a1[kModes] = {}, a2[kModes] = {}, b[kModes] = {}, y1[kModes] = {}, y2[kModes] = {};
    bool alive[kModes] = {};
    float tw = 0.0f, tr = 0.0f, ta1 = 0.0f, ta2 = 0.0f, tb = 0.0f, t1 = 0.0f, t2 = 0.0f;

    // bowed / blown (exc != kMallet)
    int exc = kMallet;               // fixed for the voice's life: a new exciter starts a new voice
    float env = 0.0f;                // bow / breath: rises over Swell while held, falls in ~60 ms after
    float press = 0.5f;              // Pressure + aftertouch, this block
    float polyAT = 0.0f;
    float vb = 0.1f;                 // bow speed (from velocity)
    float gpos[kModes] = {};         // mode strength at the contact point (table amp x normalisation x position)
    float gmax = 1.0f;               // the note's own mode's strength (the one a bow or breath drives)
    int ref = 0;                     // the note's own mode: the lowest at or above the played pitch
    float cinv[kModes] = {};         // 1 / (2 sin(w/2)): a mode's sample difference -> its velocity
    float vPrev = 0.0f;              // contact velocity, last sample
    float susOut = 0.0f;             // output gain of a bowed/blown voice
    float beta = 0.127f;             // bowed string: bow point (fraction of the string from the bridge)
    float fbScale = 0.0f, hpS = 0.0f, jlp = 0.0f, jlpK = 0.5f;   // blown string: injection scale, jet band-pass
    uint32_t nrng = 1u; float nlp1 = 0.0f, nlp2 = 0.0f;   // bow / breath noise
    float aLp = 0.0f, aBp = 0.0f;                          // the audible air's band-pass (follows the note)
    float vibFade = 0.0f, vibCents = 0.0f;
    // 1.2
    float rndCents = 0.0f;           // per-note random detune (Rnd Pitch)
    float lnTab[kModes] = {};        // ln(table ratio): LFO>Stretch bends ratio by exp(lnTab * 0.25 * dStretch)
    float strMul[kModes] = {1, 1, 1, 1, 1, 1, 1, 1};
    float pos = 0.25f;               // strike / bow position used for this note (after LFO and random)
    float posRnd = 0.0f;             // this hit's random position offset (Rnd Pos)
    float buzzRef = 0.1f, buzzPk = 0.0f, bzH = 0.0f;   // buzz: floor for the peak, the note's peak, high-pass state
    uint32_t brng = 7u;

    // string
    float* buf = nullptr; int size = 0, mask = 0, wpos = 0;
    float* buf2 = nullptr; int wpos2 = 0; float D2 = 4.0f, dD2 = 0.0f, lpB = 0.0f;   // bowed: bridge side
    float D = 8.0f, dD = 0.0f; int dRemain = 0;
    float lp = 0.0f, lpA = 1.0f, loopG = 0.0f, exScale = 0.0f;
    float dcx = 0.0f, dcy = 0.0f;
};

// ---------------------------------------------------------------------------------------------
// The plugin
// ---------------------------------------------------------------------------------------------
class Hammerhead : public mpc::Plugin
{
public:
    enum Params
    {
        kVolume = 0,
        kModel, kHard, kPos, kClick,
        kDecay, kDamp, kMute, kStretch,
        kTube, kBody, kBodySize,
        kMotor, kMotorRate, kWidth,
        kRoom, kRoomSize,
        kVelocity, kVelHard, kOctave, kFine,
        kDrop, kDropTime,
        // 1.1
        kExciter, kPressure, kSwell, kAir, kVibrato,
        // 1.2
        kBuzz, kLfoSync, kLfoRate, kLfoDiv, kLfoShape, kLfoPos, kLfoHard, kLfoStretch, kLfoPress,
        kRndPos, kRndHard, kRndPitch, kRndLevel,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Hammerhead";
        i.vendor = "Groundswell";
        i.uniqueId = MPC_FOURCC('G', 'v', 'P', 'T');   // == "uid" in plugin.json. Never change.
        i.version = 1200;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Hammerhead() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        std::vector<std::string> models;
        for (const char* m : kModelNames) models.push_back(m);

        addParam(floatParam ("vol",      "Volume", -40.0f, 6.0f, -3.0f, "dB", 1));
        // What is being hit (or plucked).
        addParam(choiceParam("model",    "Model", models, 0));
        // Mallet hardness: contact time from ~5 ms (yarn, only low modes ring) to 0.1 ms (hard plastic).
        addParam(floatParam ("hard",     "Hardness", 0.0f, 100.0f, 45.0f, "%", 0));
        // Where the mallet lands: 0 = centre of the bar / middle of the string, 100 = near the end.
        addParam(floatParam ("pos",      "Strike Pos", 0.0f, 100.0f, 25.0f, "%", 0));
        // Level of the mallet's own impact noise, straight to the output.
        addParam(floatParam ("click",    "Click", 0.0f, 100.0f, 30.0f, "%", 0));
        // How long the lowest mode rings (T60 at middle C; higher notes ring shorter).
        addParam(logParam   ("decay",    "Decay", 0.05f, 30.0f, 1.6f, "", 2));        // text from formatValue
        // How much faster the upper modes die: high = wood, low = metal and glass.
        addParam(floatParam ("damp",     "Damping", 0.0f, 100.0f, 65.0f, "%", 0));
        // How hard a note-off (or sustain pedal up) chokes the ring. 0 = let it ring out.
        addParam(floatParam ("mute",     "Mute", 0.0f, 100.0f, 0.0f, "%", 0));
        // Bends the mode ratios: - squeezes the overtones down, + spreads them up (bars into bells).
        addParam(floatParam ("stretch",  "Stretch", -100.0f, 100.0f, 0.0f, "%", 0));
        // A tuned tube under the bar: louder, rounder fundamental with a slight bloom (modal models).
        addParam(floatParam ("tube",     "Resonator", 0.0f, 100.0f, 55.0f, "%", 0));
        // A wooden box around the instrument.
        addParam(floatParam ("body",     "Body", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(floatParam ("bodysize", "Body Size", 50.0f, 200.0f, 100.0f, "%", 0));
        // Vibraphone motor: tremolo depth and speed. The mod wheel adds depth.
        addParam(floatParam ("motor",    "Motor", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("motorrate","Motor Rate", 1.0f, 12.0f, 5.0f, "Hz", 1));
        // Spread of the notes across the stereo field by pitch.
        addParam(floatParam ("width",    "Width", 0.0f, 100.0f, 50.0f, "%", 0));
        // Room reverb.
        addParam(floatParam ("room",     "Room", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(logParam   ("roomsize", "Room Size", 0.3f, 6.0f, 1.2f, "", 1));      // text from formatValue
        // Playing: velocity to level, velocity to hardness, transpose.
        addParam(floatParam ("velocity", "Velocity", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam ("velhard",  "Vel>Hard", 0.0f, 100.0f, 60.0f, "%", 0));
        addParam(intParam   ("octave",   "Octave", -2, 2, 0));
        addParam(floatParam ("fine",     "Fine", -50.0f, 50.0f, 0.0f, "ct", 0));
        // Pitch drop at the strike: how far (semitones, scaled by velocity) and how fast it falls.
        addParam(floatParam ("drop",     "Drop", 0.0f, 12.0f, 0.0f, "st", 1, 0.5f));
        addParam(logParam   ("droptime", "Drop Time", 10.0f, 500.0f, 60.0f, "", 0));   // text from formatValue
        // ---- 1.1: appended, so 1.0 projects load unchanged (Mallet is the 1.0 sound). ----
        // What sets the object going: a mallet (one strike), a bow or a breath (both sustain while held).
        addParam(choiceParam("exciter",  "Exciter", {"Mallet", "Bow", "Blow"}, kMallet));
        // Bow pressure / breath pressure (pad aftertouch adds to it). More is grittier, brighter, overblown.
        addParam(floatParam ("pressure", "Pressure", 0.0f, 100.0f, 50.0f, "%", 0));
        // How long the bow or breath takes to build up after note-on.
        addParam(logParam   ("swell",    "Swell", 0.005f, 3.0f, 0.15f, "", 2));      // text from formatValue
        // Rosin scratch (Bow) or breath noise (Blow).
        addParam(floatParam ("air",      "Air", 0.0f, 100.0f, 25.0f, "%", 0));
        // Pitch vibrato at Motor Rate, up to +-35 cents, fading in after each note starts.
        addParam(floatParam ("vibrato",  "Vibrato", 0.0f, 100.0f, 0.0f, "%", 0));
        // ---- 1.2: appended, so 1.0 and 1.1 projects load unchanged (all of these start at "off"). ----
        // A loose buzzer on the object (kalimba bottle caps, balafon membranes): rattles while the note is loud.
        addParam(floatParam ("buzz",     "Buzz", 0.0f, 100.0f, 0.0f, "%", 0));
        // The LFO: free (LFO Rate) or locked to the song (LFO Div), and its shape.
        addParam(choiceParam("lfosync",  "LFO Sync", {"Free", "Sync"}, 0));
        addParam(logParam   ("lforate",  "LFO Rate", 0.05f, 20.0f, 0.5f, "Hz", 2));
        std::vector<std::string> divs;
        for (const char* d : kLfoDivNames) divs.push_back(d);
        addParam(choiceParam("lfodiv",   "LFO Div", divs, 2));
        addParam(choiceParam("lfoshape", "LFO Shape", {"Sine", "Triangle", "Square", "Ramp", "S&H", "Drift"}, kLfoSine));
        // How far the LFO moves each destination (bipolar). Position and Hardness are read at each hit.
        addParam(floatParam ("lfopos",   "LFO>Pos", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("lfohard",  "LFO>Hard", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("lfostretch","LFO>Stretch", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("lfopress", "LFO>Press", -100.0f, 100.0f, 0.0f, "%", 0));
        // Per-hit randomness.
        addParam(floatParam ("rndpos",   "Rnd Pos", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("rndhard",  "Rnd Hard", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("rndpitch", "Rnd Pitch", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("rndlevel", "Rnd Level", 0.0f, 100.0f, 0.0f, "%", 0));

        if (numParams() != kNumParams) mpc::log("Hammerhead: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Rosewood Marimba", {});
        program("Harbour Vibes", {{kModel, 1}, {kHard, 40}, {kPos, 20}, {kClick, 12}, {kDecay, 7}, {kDamp, 30}, {kMute, 65},
                                  {kTube, 45}, {kBody, 0}, {kMotor, 45}, {kMotorRate, 5.5f}, {kRoom, 30}, {kRoomSize, 1.8f},
                                  {kOctave, 1}});
        program("Reef Xylophone", {{kModel, 2}, {kHard, 80}, {kPos, 30}, {kClick, 45}, {kDecay, 0.9f}, {kDamp, 70}, {kTube, 40},
                                   {kRoom, 22}, {kOctave, 1}});
        program("Shell Kalimba", {{kModel, 4}, {kHard, 50}, {kPos, 40}, {kClick, 30}, {kDecay, 2.5f}, {kDamp, 50}, {kTube, 25},
                                  {kBody, 45}, {kBodySize, 130}, {kRoom, 18}, {kOctave, 1}});
        program("Buoy Bell", {{kModel, 5}, {kHard, 80}, {kPos, 50}, {kClick, 18}, {kDecay, 8}, {kDamp, 25}, {kTube, 0},
                              {kBody, 0}, {kRoom, 40}, {kRoomSize, 3}, {kWidth, 40}});
        program("Sonar Glass", {{kModel, 6}, {kHard, 70}, {kPos, 50}, {kClick, 8}, {kDecay, 6}, {kDamp, 15}, {kTube, 0},
                                {kBody, 0}, {kDrop, 1.0f}, {kDropTime, 200}, {kRoom, 55}, {kRoomSize, 4}, {kOctave, 1}});
        program("Island Pan", {{kModel, 7}, {kHard, 55}, {kPos, 55}, {kClick, 25}, {kDecay, 2.2f}, {kDamp, 45}, {kTube, 10},
                               {kBody, 10}, {kDrop, 0.3f}, {kDropTime, 40}, {kRoom, 25}, {kOctave, 1}});
        program("Nylon Pluck", {{kModel, 8}, {kHard, 45}, {kPos, 70}, {kClick, 20}, {kDecay, 3}, {kDamp, 55}, {kTube, 0},
                                {kBody, 55}, {kBodySize, 85}, {kRoom, 20}});
        program("Hammered Dulcimer", {{kModel, 9}, {kHard, 75}, {kPos, 75}, {kClick, 30}, {kDecay, 5}, {kDamp, 30}, {kTube, 0},
                                      {kBody, 35}, {kBodySize, 110}, {kRoom, 30}, {kOctave, 1}});
        program("Hull Drum", {{kModel, 0}, {kHard, 30}, {kPos, 10}, {kClick, 45}, {kDecay, 0.7f}, {kDamp, 85}, {kTube, 80},
                              {kDrop, 5}, {kDropTime, 80}, {kBody, 40}, {kBodySize, 160}, {kRoom, 20}});
        // 1.1: bowed and blown
        program("Bowed Vibes", {{kModel, 1}, {kExciter, kBow}, {kPressure, 45}, {kSwell, 0.35f}, {kAir, 15}, {kPos, 20},
                                {kDecay, 6}, {kDamp, 30}, {kTube, 45}, {kBody, 0}, {kRoom, 35}, {kRoomSize, 2.5f}, {kOctave, 1}});
        program("Moon Jelly", {{kModel, 6}, {kExciter, kBow}, {kPressure, 35}, {kSwell, 0.8f}, {kAir, 10}, {kPos, 50},
                               {kDecay, 8}, {kDamp, 15}, {kTube, 0}, {kBody, 0}, {kVibrato, 15}, {kMotorRate, 4.5f},
                               {kRoom, 55}, {kRoomSize, 4.5f}, {kWidth, 70}, {kOctave, 1}});
        program("Kelp Cello", {{kModel, 8}, {kExciter, kBow}, {kPressure, 55}, {kSwell, 0.12f}, {kAir, 25}, {kPos, 70},
                               {kDecay, 2.5f}, {kDamp, 45}, {kTube, 0}, {kBody, 60}, {kBodySize, 75}, {kVibrato, 25},
                               {kMotorRate, 5.5f}, {kRoom, 30}, {kRoomSize, 2}});
        program("Conch", {{kModel, 9}, {kExciter, kBlow}, {kPressure, 40}, {kSwell, 0.25f}, {kAir, 40}, {kPos, 50},
                          {kDecay, 3}, {kDamp, 40}, {kTube, 0}, {kBody, 25}, {kBodySize, 140}, {kVibrato, 12},
                          {kMotorRate, 5}, {kRoom, 40}, {kRoomSize, 3}});
        program("Bottle Breath", {{kModel, 6}, {kExciter, kBlow}, {kPressure, 30}, {kSwell, 0.2f}, {kAir, 55}, {kPos, 30},
                                  {kDecay, 3}, {kDamp, 40}, {kTube, 30}, {kBody, 0}, {kRoom, 30}, {kRoomSize, 1.8f}});
        // 1.2: buzz and modulation
        program("Barnacle Kalimba", {{kModel, 4}, {kBuzz, 55}, {kHard, 55}, {kPos, 40}, {kClick, 25}, {kDecay, 2.8f}, {kDamp, 50},
                                     {kTube, 20}, {kBody, 45}, {kBodySize, 125}, {kRoom, 18}, {kOctave, 1},
                                     {kRndPos, 20}, {kRndHard, 15}, {kRndLevel, 15}});
        program("Coral Balafon", {{kModel, 0}, {kBuzz, 40}, {kHard, 60}, {kPos, 20}, {kClick, 35}, {kDecay, 1.4f}, {kDamp, 75},
                                  {kTube, 70}, {kBody, 20}, {kRoom, 22}, {kRndPos, 25}, {kRndHard, 20}, {kRndPitch, 10},
                                  {kRndLevel, 20}});
        program("Tide Machine", {{kModel, 0}, {kHard, 50}, {kPos, 35}, {kClick, 30}, {kDecay, 1.2f}, {kDamp, 70}, {kTube, 50},
                                 {kLfoSync, 1}, {kLfoDiv, 6}, {kLfoShape, kLfoSH}, {kLfoPos, 80}, {kLfoHard, 60},
                                 {kRndLevel, 10}, {kRoom, 25}});
        program("Rip Current", {{kModel, 6}, {kHard, 70}, {kPos, 50}, {kClick, 10}, {kDecay, 6}, {kDamp, 15}, {kTube, 0},
                                {kBody, 0}, {kLfoShape, kLfoTri}, {kLfoRate, 0.15f}, {kLfoStretch, 45}, {kRoom, 45},
                                {kRoomSize, 3.5f}, {kOctave, 1}});
        program("Undercurrent", {{kModel, 1}, {kExciter, kBow}, {kPressure, 45}, {kSwell, 0.4f}, {kAir, 15}, {kPos, 30},
                                 {kDecay, 6}, {kDamp, 30}, {kTube, 45}, {kBody, 0}, {kLfoShape, kLfoDrift}, {kLfoRate, 0.3f},
                                 {kLfoPos, 60}, {kLfoPress, 40}, {kRoom, 40}, {kRoomSize, 2.5f}, {kOctave, 1}});

        // Per-model normalisation: the modes' strengths add up to the same power in every model.
        for (int m = 0; m < kNumModels; ++m)
        {
            float s = 0.0f;
            for (int k = 0; k < kModes; ++k) s += kModels[m].amp[k] * kModels[m].amp[k];
            ampNorm_[m] = s > 0.0f ? 1.0f / std::sqrt(s) : 1.0f;
        }
    }

    // ----- lifecycle ------------------------------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        // String delay lines: long enough for 15 Hz.
        int size = 1;
        while ((float)size < sr_ / 15.0f + 8.0f) size <<= 1;
        strBuf_.assign((size_t)size * kNumSlots * 2, 0.0f);
        for (int i = 0; i < kNumSlots; ++i)
        {
            voices_[i].buf = strBuf_.data() + (size_t)(2 * i) * (size_t)size;
            voices_[i].buf2 = strBuf_.data() + (size_t)(2 * i + 1) * (size_t)size;
            voices_[i].size = size;
            voices_[i].mask = size - 1;
        }
        // Room: 8 delay lines of 21..56 ms.
        static const int kFdnLen[8] = {937, 1151, 1361, 1489, 1709, 1987, 2179, 2467};
        for (int i = 0; i < 8; ++i)
        {
            fdnLen_[i] = std::max(16, (int)std::lround((float)kFdnLen[i] * sr_ / 44100.0f));
            int sz = 1;
            while (sz < fdnLen_[i] + 64) sz <<= 1;
            fdnBuf_[i].assign((size_t)sz, 0.0f);
            fdnMask_[i] = sz - 1;
        }
        for (SVF& f : body_) f.setSampleRate(sampleRate);
        volume_.setTime(20.0f, sampleRate);
        lastBodySize_ = -1.0f;
        lastRoomSize_ = -1.0f;
    }

    void reset() override
    {
        for (Voice& v : voices_)
        {
            v.stage = kOff; v.note = -1; v.held = v.sustained = v.muted = v.choked = false;
            std::memset(v.y1, 0, sizeof(v.y1)); std::memset(v.y2, 0, sizeof(v.y2));
            v.t1 = v.t2 = 0.0f; v.ex = Exciter(); v.exB = Exciter();
            v.lp = v.dcx = v.dcy = 0.0f;
            v.env = v.vPrev = v.hpS = v.lpB = v.nlp1 = v.nlp2 = v.polyAT = 0.0f;
        }
        chanAT_ = 0.0f;
        vibPhase_ = 0.0f;
        lfoPhase_ = 0.0f; lfoSH_ = lfoDriftT_ = lfoDrift_ = lfoDStr_ = 0.0f;
        rng_ = 0x2545F491u;
        std::fill(strBuf_.begin(), strBuf_.end(), 0.0f);
        for (auto& b : fdnBuf_) std::fill(b.begin(), b.end(), 0.0f);
        for (float& l : fdnLp_) l = 0.0f;
        fdnPos_ = 0;
        hpL_ = hpR_ = 0.0f;
        for (SVF& f : body_) f.reset();
        ctrlLeft_ = 0;
        orderCounter_ = 0;
        sustain_ = false;
        bend_ = 0.0f; modWheel_ = 0.0f;
        motorPhase_ = 0.0f;
        modPhase1_ = 0.0f; modPhase2_ = 0.37f;
        haveKey_ = false;
        readSettings();
        volume_.snap(s_.volume);
        bodyRamp_.snap(s_.body);
        dryRamp_.snap(dryGain(s_.room));
        wetRamp_.snap(wetGain(s_.room));
        motorRamp_.snap(s_.motor);
    }

    // ----- audio ----------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings();
        const int n = ctx.numFrames;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        if (coefDirty_)
        {
            coefDirty_ = false; tuneDirty_ = false;
            for (Voice& v : voices_) if (v.stage != kOff) computeCoeffs(v);
        }
        else if (tuneDirty_)
        {
            tuneDirty_ = false;
            for (Voice& v : voices_) if (v.stage != kOff) tune(v, false);
        }
        // LFO: in Sync, locked to the song position while the MPC plays.
        const mpc::Transport& tr = ctx.transport;
        lfoBeats_ = kLfoDivBeats[s_.lfoDiv];
        lfoHz_ = s_.lfoSync ? beatsToHz(lfoBeats_, tr.tempoValid ? tr.bpm : 120.0) : s_.lfoRate;
        if (s_.lfoSync && tr.playing && tr.ppqValid) setLfoPhase(ppqPhase(tr.ppqPos, lfoBeats_));
        for (Voice& v : voices_)
        {
            v.peak = 0.0f;
            v.press = clamp(s_.pressure + std::max(chanAT_, v.polyAT) + 0.5f * s_.lfoPress * lfoVal_, 0.0f, 1.0f);
        }

        // Voices: segments break at MIDI events and at control ticks.
        int pos = 0, e = 0;
        while (pos < n)
        {
            while (e < ctx.numMidiEvents && ctx.midi[e].sampleOffset <= pos) handleMidi(ctx.midi[e++]);
            if (ctrlLeft_ <= 0) { controlTick(); ctrlLeft_ = kCtrl; }
            int len = std::min(ctrlLeft_, n - pos);
            if (e < ctx.numMidiEvents && ctx.midi[e].sampleOffset > pos)
                len = std::min(len, ctx.midi[e].sampleOffset - pos);
            for (Voice& v : voices_)
                if (v.stage != kOff) renderVoice(v, L + pos, R ? R + pos : nullptr, len);
            pos += len;
            ctrlLeft_ -= len;
        }
        while (e < ctx.numMidiEvents) handleMidi(ctx.midi[e++]);

        // Retire voices that have been silent for 100 ms.
        const int quietLimit = (int)(0.1f * sr_);
        for (Voice& v : voices_)
        {
            if (v.stage != kOn) continue;
            const bool driven = v.ex.active() || v.exB.active() ||
                                (v.exc != kMallet && (v.held || v.sustained || v.env > 1e-3f));
            if (v.peak < 3e-5f && !driven) v.quiet += n; else v.quiet = 0;
            if (v.quiet > quietLimit) v.stage = kOff;
        }

        // Output stage: motor tremolo, body, room, volume, limiter.
        motorRamp_.setTarget(clamp(s_.motor + modWheel_, 0.0f, 1.0f), n);
        bodyRamp_.setTarget(s_.body, n);
        dryRamp_.setTarget(dryGain(s_.room), n);
        wetRamp_.setTarget(wetGain(s_.room), n);
        volume_.setTarget(s_.volume);
        const float motorInc = s_.motorRate / sr_;
        const float dampK = fdnDamp_, hpC = hpCoef_, inG = fdnInGain_;
        const float modDepth = 5.0f * sr_ / 44100.0f;
        const float mod1 = 6.0f + modDepth * (1.0f + fastSin01(modPhase1_));
        const float mod2 = 6.0f + modDepth * (1.0f + fastSin01(modPhase2_));
        modPhase1_ = wrapPhase(modPhase1_ + 0.17f * (float)n / sr_);
        modPhase2_ = wrapPhase(modPhase2_ + 0.29f * (float)n / sr_);

        for (int i = 0; i < n; ++i)
        {
            float dl = L[i], dr = R ? R[i] : L[i];

            const float md = motorRamp_.next();
            if (md > 0.0f)
            {
                const float g = 1.0f - md * (0.5f + 0.5f * fastSin01(motorPhase_));
                dl *= g; dr *= g;
            }
            motorPhase_ += motorInc;
            if (motorPhase_ >= 1.0f) motorPhase_ -= 1.0f;

            const float bg = bodyRamp_.next();
            if (bg > 0.0f)
            {
                const float mid = 0.5f * (dl + dr);
                float bo = 0.0f, lp, bp, hp;
                body_[0].processAll(mid, lp, bp, hp); bo += 1.0f * bp;
                body_[1].processAll(mid, lp, bp, hp); bo += 0.8f * bp;
                body_[2].processAll(mid, lp, bp, hp); bo += 0.55f * bp;
                body_[3].processAll(mid, lp, bp, hp); bo += 0.35f * bp;
                bo *= 0.30f * bg;
                const float dry = 1.0f - 0.35f * bg;
                dl = dl * dry + bo; dr = dr * dry + bo;
            }

            // Room: high-passed input into an 8-line FDN.
            hpL_ += hpC * (dl - hpL_);
            hpR_ += hpC * (dr - hpR_);
            const float inL = (dl - hpL_) * inG, inR = (dr - hpR_) * inG;
            float y[8];
            for (int k = 0; k < 8; ++k)
            {
                const float* buf = fdnBuf_[k].data();
                const int mask = fdnMask_[k];
                if (k == 0 || k == 5)
                {
                    const float dly = (float)fdnLen_[k] - (k == 0 ? mod1 : mod2);
                    const int di = (int)dly;
                    const float fr = dly - (float)di;
                    const float a = buf[(fdnPos_ - di) & mask], b = buf[(fdnPos_ - di - 1) & mask];
                    y[k] = a + fr * (b - a);
                }
                else y[k] = buf[(fdnPos_ - fdnLen_[k]) & mask];
            }
            const float wl = 0.5f * (y[0] - y[2] + y[4] - y[6]);
            const float wr = 0.5f * (y[1] - y[3] + y[5] - y[7]);
            for (int k = 0; k < 8; ++k)
            {
                fdnLp_[k] += dampK * (y[k] - fdnLp_[k]);
                y[k] = fdnLp_[k] * fdnG_[k];
            }
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
                const float v = y[k] * kNorm + ((k & 1) ? ((k & 2) ? -inR : inR) : ((k & 2) ? -inL : inL));
                fdnBuf_[k][(size_t)(fdnPos_ & fdnMask_[k])] = v;
            }
            ++fdnPos_;

            const float dg = dryRamp_.next(), wg = wetRamp_.next();
            const float vol = volume_.process();
            L[i] = limitOut((dl * dg + wl * wg) * vol);
            if (R) R[i] = limitOut((dr * dg + wr * wg) * vol);
        }
        fdnPos_ &= 0x3FFFFFFF;
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        char buf[32];
        switch (index)
        {
            case kDecay:
            case kRoomSize:
            case kSwell:
                if (plainValue < 1.0f) std::snprintf(buf, sizeof(buf), "%.0f ms", (double)plainValue * 1000.0);
                else if (plainValue < 10.0f) std::snprintf(buf, sizeof(buf), "%.2f s", (double)plainValue);
                else std::snprintf(buf, sizeof(buf), "%.1f s", (double)plainValue);
                break;
            case kDropTime: std::snprintf(buf, sizeof(buf), "%.0f ms", (double)plainValue); break;
            case kOctave: std::snprintf(buf, sizeof(buf), "%+d", (int)std::lround(plainValue)); break;
            case kStretch: std::snprintf(buf, sizeof(buf), "%+.0f", (double)plainValue); break;
            default: return false;
        }
        out = buf;
        return true;
    }

private:
    struct Settings
    {
        float volume = 0.7f;
        int model = 0;
        float hard = 0.45f, pos = 0.25f, click = 0.3f;
        float decay = 1.6f, damp = 0.65f, mute = 0.0f, stretch = 0.0f, tube = 0.55f;
        float body = 0.15f, bodySize = 1.0f, motor = 0.0f, motorRate = 5.0f, width = 0.5f;
        float room = 0.2f, roomSize = 1.2f, velSens = 0.7f, velHard = 0.6f;
        int octave = 0;
        float fine = 0.0f, drop = 0.0f, dropTime = 0.06f;
        int exc = kMallet;
        float pressure = 0.5f, swell = 0.15f, air = 0.25f, vibrato = 0.0f;
        float buzz = 0.0f;
        bool lfoSync = false; float lfoRate = 0.5f; int lfoDiv = 2, lfoShape = 0;
        float lfoPos = 0.0f, lfoHard = 0.0f, lfoStretch = 0.0f, lfoPress = 0.0f;
        float rndPos = 0.0f, rndHard = 0.0f, rndPitch = 0.0f, rndLevel = 0.0f;
    };
    // Everything a ringing voice's coefficients depend on.
    struct CoefKey { float decay, damp, stretch, tube, mute, fine; int octave; };

    static float dryGain(float room) { return 1.0f - 0.35f * room; }
    static float wetGain(float room) { return 0.9f * room; }

    void readSettings()
    {
        s_.volume = dbToGain(get(kVolume));
        s_.model = clamp(getInt(kModel), 0, kNumModels - 1);
        s_.hard = get(kHard) * 0.01f;
        s_.pos = get(kPos) * 0.01f;
        s_.click = get(kClick) * 0.01f;
        s_.decay = clamp(get(kDecay), 0.05f, 30.0f);
        s_.damp = get(kDamp) * 0.01f;
        s_.mute = get(kMute) * 0.01f;
        s_.stretch = get(kStretch) * 0.01f;
        s_.tube = get(kTube) * 0.01f;
        s_.body = get(kBody) * 0.01f;
        s_.bodySize = clamp(get(kBodySize) * 0.01f, 0.5f, 2.0f);
        s_.motor = get(kMotor) * 0.01f;
        s_.motorRate = clamp(get(kMotorRate), 1.0f, 12.0f);
        s_.width = get(kWidth) * 0.01f;
        s_.room = get(kRoom) * 0.01f;
        s_.roomSize = clamp(get(kRoomSize), 0.3f, 6.0f);
        s_.velSens = get(kVelocity) * 0.01f;
        s_.velHard = get(kVelHard) * 0.01f;
        s_.octave = getInt(kOctave);
        s_.fine = get(kFine);
        s_.drop = get(kDrop);
        s_.dropTime = clamp(get(kDropTime), 10.0f, 500.0f) * 0.001f;
        dropK_ = std::exp(-3.0f * (float)kCtrl / (s_.dropTime * sr_));
        s_.exc = clamp(getInt(kExciter), 0, 2);
        s_.pressure = get(kPressure) * 0.01f;
        s_.swell = clamp(get(kSwell), 0.005f, 3.0f);
        // Air taper (1.1.1): x^1.5, so the lower half of the knob is a hint and the top is still breathy.
        s_.air = std::pow(get(kAir) * 0.01f, 1.5f);
        s_.vibrato = get(kVibrato) * 0.01f;
        s_.buzz = get(kBuzz) * 0.01f;
        s_.lfoSync = getInt(kLfoSync) == 1;
        s_.lfoRate = clamp(get(kLfoRate), 0.05f, 20.0f);
        s_.lfoDiv = clamp(getInt(kLfoDiv), 0, kNumLfoDivs - 1);
        s_.lfoShape = clamp(getInt(kLfoShape), 0, 5);
        s_.lfoPos = get(kLfoPos) * 0.01f;
        s_.lfoHard = get(kLfoHard) * 0.01f;
        s_.lfoStretch = get(kLfoStretch) * 0.01f;
        s_.lfoPress = get(kLfoPress) * 0.01f;
        s_.rndPos = get(kRndPos) * 0.01f;
        s_.rndHard = get(kRndHard) * 0.01f;
        s_.rndPitch = get(kRndPitch) * 0.01f;
        s_.rndLevel = get(kRndLevel) * 0.01f;
        bzK_ = 1.0f - std::exp(-kTwoPi * 1200.0f / sr_);
        kSwell_ = 1.0f - std::exp(-3.0f / (s_.swell * sr_));
        kLift_ = 1.0f - std::exp(-3.0f / (0.06f * sr_));
        nk1_ = 1.0f - std::exp(-kTwoPi * std::min(900.0f + 2600.0f * s_.pressure, 0.45f * sr_) / sr_);
        nk2_ = 1.0f - std::exp(-kTwoPi * 300.0f / sr_);

        const CoefKey key{s_.decay, s_.damp, s_.stretch, s_.tube, s_.mute, s_.fine, s_.octave};
        if (!haveKey_ || std::memcmp(&key, &lastKey_, sizeof(key)) != 0) { lastKey_ = key; haveKey_ = true; coefDirty_ = true; }
        const float bendRatio = std::exp2(bend_ / 12.0f);
        if (bendRatio != bendRatio_) { bendRatio_ = bendRatio; tuneDirty_ = true; }

        if (s_.bodySize != lastBodySize_)
        {
            lastBodySize_ = s_.bodySize;
            static const float kBodyHz[4] = {98.0f, 212.0f, 455.0f, 1020.0f};
            for (int i = 0; i < 4; ++i) body_[i].set(clamp(kBodyHz[i] / s_.bodySize, 30.0f, 0.45f * sr_), 0.85f);
        }
        if (s_.roomSize != lastRoomSize_)
        {
            lastRoomSize_ = s_.roomSize;
            float sumG2 = 0.0f;
            for (int k = 0; k < 8; ++k)
            {
                fdnG_[k] = std::pow(10.0f, -3.0f * (float)fdnLen_[k] / (sr_ * s_.roomSize));
                sumG2 += fdnG_[k] * fdnG_[k];
            }
            fdnInGain_ = 0.8f * std::sqrt(std::max(0.02f, 1.0f - sumG2 / 8.0f));
        }
        fdnDamp_ = 1.0f - std::exp(-kTwoPi * std::min(6500.0f, 0.45f * sr_) / sr_);
        hpCoef_ = 1.0f - std::exp(-kTwoPi * 120.0f / sr_);
        hpK_ = 1.0f - std::exp(-kTwoPi * 30.0f / sr_);
    }

    // ----- MIDI -----------------------------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn()) noteOn(ev.note(), (float)ev.velocity() / 127.0f);
        else if (ev.isNoteOff()) noteOff(ev.note());
        else if (ev.isPitchBend())
        {
            bend_ = 2.0f * (float)ev.pitchBend() / 8192.0f;
            const float bendRatio = std::exp2(bend_ / 12.0f);
            if (bendRatio != bendRatio_) { bendRatio_ = bendRatio; for (Voice& v : voices_) if (v.stage != kOff) tune(v, false); }
        }
        else if (ev.isAftertouch()) chanAT_ = (float)ev.data[1] / 127.0f;            // channel pressure
        else if (ev.status() == 0xA0)                                                 // poly pressure
        {
            for (Voice& v : voices_) if (v.stage == kOn && v.note == ev.data[1]) v.polyAT = (float)ev.data[2] / 127.0f;
        }
        else if (ev.isAllNotesOff())
        {
            sustain_ = false;
            for (Voice& v : voices_)
                if (v.stage == kOn) { v.held = v.sustained = false; v.muted = v.choked = true; computeCoeffs(v); }
        }
        else if (ev.isController())
        {
            if (ev.controller() == 1) modWheel_ = (float)ev.controllerValue() / 127.0f;
            else if (ev.controller() == 64)
            {
                sustain_ = ev.controllerValue() >= 64;
                if (!sustain_)
                    for (Voice& v : voices_) if (v.stage == kOn && v.sustained) { v.sustained = false; release(v); }
            }
        }
    }

    void noteOn(int note, float vel01)
    {
        // The same note still ringing: hit it again. The bar keeps vibrating and the new strike
        // adds to it, which is what makes rolls and repeated notes sound real.
        // A new Model or Exciter on a note that is still ringing: that voice fades out (it is another
        // object) and the note starts a fresh one.
        for (Voice& v : voices_)
            if (v.stage == kOn && v.note == note)
            {
                if (v.model == s_.model && v.exc == s_.exc) { strike(v, vel01); return; }
                v.stage = kSteal;
                v.fade = 0.0f;
                v.fadeStep = 1.0f / std::max(16.0f, 0.008f * sr_);
                v.pendNote = -1;
            }
        int ringing = 0;
        for (const Voice& v : voices_) if (v.stage == kOn) ++ringing;
        if (ringing >= kNumVoices)
        {
            // Polyphony is full: the quietest ringing voice fades out over ~8 ms in its own slot.
            Voice* victim = nullptr;
            for (Voice& v : voices_)
                if (v.stage == kOn && (!victim || v.peak < victim->peak || (v.peak == victim->peak && v.order < victim->order))) victim = &v;
            victim->stage = kSteal;
            victim->fade = 0.0f;
            victim->fadeStep = 1.0f / std::max(16.0f, 0.008f * sr_);
            victim->pendNote = -1;
        }
        for (Voice& v : voices_)
            if (v.stage == kOff) { start(v, note, vel01); return; }
        // Every spare slot is still fading (a burst of more than 4 stolen notes in 8 ms): the note
        // waits for the fade that is furthest along.
        Voice* best = nullptr;
        for (Voice& v : voices_)
            if (v.stage == kSteal && (!best || v.fade > best->fade)) best = &v;
        if (best) { best->pendNote = note; best->pendVel = vel01; }
    }

    void noteOff(int note)
    {
        for (Voice& v : voices_)
            if (v.stage == kOn && v.note == note && v.held)
            {
                v.held = false;
                if (sustain_) v.sustained = true; else release(v);
            }
    }

    void release(Voice& v)
    {
        if (s_.mute > 0.005f && !v.muted) { v.muted = true; computeCoeffs(v); }
    }

    void start(Voice& v, int note, float vel01)
    {
        v.stage = kOn;
        v.note = note;
        v.model = s_.model;
        v.exc = s_.exc;
        v.muted = v.choked = false;
        v.fade = 0.0f; v.fadeStep = 0.0f; v.pendNote = -1;
        v.quiet = 0;
        v.dropSt = 0.0f;
        v.env = v.vPrev = v.hpS = v.lpB = v.nlp1 = v.nlp2 = v.polyAT = v.jlp = v.aLp = v.aBp = 0.0f;
        v.ex = Exciter(); v.exB = Exciter();
        v.vibFade = v.vibCents = 0.0f;
        std::memset(v.y1, 0, sizeof(v.y1)); std::memset(v.y2, 0, sizeof(v.y2));
        v.t1 = v.t2 = 0.0f;
        v.lp = v.dcx = v.dcy = 0.0f;
        if (kModels[v.model].engine == kString && v.buf)
        {
            std::memset(v.buf, 0, sizeof(float) * (size_t)v.size); v.wpos = 0;
            if (v.exc == kBow && v.buf2) { std::memset(v.buf2, 0, sizeof(float) * (size_t)v.size); v.wpos2 = 0; }
        }
        float pan = s_.width * clamp((float)(note - 60) / 24.0f, -1.0f, 1.0f) * 0.85f;
        equalPowerPan(pan, v.gL, v.gR);
        // Per-note randomness that has to be known before the coefficients: detune and the contact point.
        v.rndCents = (rnd() - 0.5f) * 50.0f * s_.rndPitch;             // +-25 cents at 100 %
        v.posRnd = (rnd() - 0.5f) * 0.8f * s_.rndPos;
        v.pos = notePos(v);
        v.beta = clamp(0.5f - 0.46f * v.pos, 0.04f, 0.5f);   // bowed string: the bow point
        v.buzzPk = 0.0f; v.bzH = 0.0f; v.brng = (uint32_t)(note * 2654435761u) | 1u;
        computeCoeffs(v, true);
        strike(v, vel01, true);
    }

    // A new hit (or a new bow stroke or breath) on a voice: velocity, mallet, contact point, pitch drop.
    void strike(Voice& v, float vel01, bool fresh = false)
    {
        const ModelDef& m = kModels[v.model];
        v.order = ++orderCounter_;
        v.held = true; v.sustained = false;
        const uint32_t seed = 0x9E3779B9u * (orderCounter_ + 17u) ^ 0x85EBCA6Bu;
        // A little natural variation: no two hits are exactly the same.
        const float jitA = 1.0f + 0.04f * ((float)((seed >> 8) & 1023) / 511.5f - 1.0f);
        const float jitT = 1.0f + 0.05f * ((float)((seed >> 18) & 1023) / 511.5f - 1.0f);

        v.level = (1.0f - s_.velSens * (1.0f - std::pow(vel01, 1.4f))) * kVoiceGain * m.gain * jitA;
        // 1.2: per-hit randomness and the LFO (Rnd Level is +-6 dB at 100 %).
        v.level *= std::exp2((rnd() - 0.5f) * 2.0f * s_.rndLevel);
        const float hardNow = clamp(s_.hard + (rnd() - 0.5f) * s_.rndHard + 0.5f * s_.lfoHard * lfoVal_, 0.0f, 1.0f);
        if (!fresh)
        {
            v.posRnd = (rnd() - 0.5f) * 0.8f * s_.rndPos;
            v.pos = notePos(v);
        }
        v.buzzPk = 0.0f;
        v.buzzRef = 0.35f * kVoiceGain * m.gain;
        const float h = clamp(hardNow + s_.velHard * (vel01 - 0.6f), 0.0f, 1.0f);

        v.dropSt = s_.drop * (0.35f + 0.65f * vel01);
        if (v.muted) { v.muted = v.choked = false; computeCoeffs(v); }
        else tune(v, false);

        // How strongly each mode is driven at the contact point (modal models).
        if (m.engine == kModal) modeWeights(v, v.pos);

        if (v.exc != kMallet)
        {
            // Bow or breath: nothing to strike; the envelope (re)starts rising towards full.
            v.ex = Exciter(); v.exB = Exciter();
            v.vb = 0.06f + 0.22f * vel01;            // bow speed: velocity is how hard the stroke starts
            v.nrng = seed | 1u;
            const float base = kVoiceGain * m.gain * jitA;
            if (m.engine == kModal) v.susOut = v.exc == kBow ? base * kOutBowModal : v.level * kOutBlowModal;
            else v.susOut = v.exc == kBow ? base * kOutBowString : v.level * kOutBlowString;
            if (m.engine == kModal)
            {
                setSustainB(v);
                v.susOut *= 0.55f / v.gmax;            // every model sings at about the same level
            }
            // The first grip of the bow, or the tongue starting a breath: a soft push that starts the
            // object moving, so it speaks at once instead of building up from noise. Smaller with a
            // long Swell, so slow swells still bloom.
            const float kick = 0.5f * (0.3f + 0.7f * vel01) * clamp(0.15f / s_.swell, 0.0f, 1.0f);
            if (kick > 1e-4f)
            {
                const float f1 = std::max(20.0f, v.hz);
                Exciter& x = v.ex;
                x.len = std::max(2, (int)std::lround(std::min(0.003f, 0.5f / f1) * sr_));
                const float th = kPi / (float)x.len;
                x.c2 = 2.0f * std::cos(th); x.s1 = 0.0f; x.s2 = -std::sin(th);
                if (m.engine == kModal)
                {
                    // Unit area rings the note's mode at gpos * scale: aim it at `kick` of full swing.
                    const float area = kick * (v.exc == kBow ? 0.3f : 1.0f) / std::max(1e-7f, v.b[v.ref] / std::max(1e-6f, std::sin(v.w[v.ref])));
                    x.amp = kPi / (2.0f * (float)x.len) * area;
                }
                else
                {
                    const float N = sr_ / f1;
                    x.amp = kPi / (2.0f * (float)x.len) * kick * (v.exc == kBow ? 0.2f : 1.0f);
                    v.exB = x;
                    v.exB.wait = std::max(1, (int)std::lround(v.beta * N));
                    v.exScale = 0.22f * std::min(N, 1200.0f) * std::sqrt((float)std::min(x.len, 60) / 20.0f);
                }
            }
            return;
        }

        // Contact time: 5 ms (soft) .. 0.1 ms (hard), shorter on higher (stiffer) notes, and never
        // so long that it would swallow the fundamental.
        const float f1 = std::max(20.0f, v.hz);
        float tc = 0.005f * std::pow(0.02f, h) * m.contact * std::sqrt(262.0f / f1) * jitT;
        tc = clamp(tc, 2.0f / sr_, 0.5f / f1);
        Exciter& x = v.ex;
        x = Exciter();
        x.len = std::max(2, (int)std::lround(tc * sr_));
        const float th = kPi / (float)x.len;
        x.c2 = 2.0f * std::cos(th); x.s1 = 0.0f; x.s2 = -std::sin(th);
        x.amp = kPi / (2.0f * (float)x.len) * v.level;      // unit area: slow modes ring at their table strength
        x.nlen = std::max(8, (int)((0.0015f + 0.006f * (1.0f - h)) * sr_));
        x.envK = std::exp(-5.0f / (float)x.nlen);
        x.env = 1.0f;
        const float fc = std::min(1500.0f + 10000.0f * h * h, 0.45f * sr_);
        x.k1 = 1.0f - std::exp(-kTwoPi * fc / sr_);
        x.k2 = 1.0f - std::exp(-kTwoPi * 250.0f / sr_);
        const float gN = 1.2f / std::sqrt(x.k1);
        x.nRes = m.noiseIn * x.amp * gN * 0.6f;
        x.nOut = s_.click * v.level * 0.55f * gN * (0.4f + 0.6f * h);
        x.rng = seed | 1u;

        if (m.engine == kModal)
        {
            v.exB = Exciter();
            for (int k = 0; k < kModes; ++k) v.b[k] = v.gpos[k] * std::sin(v.w[k]);
        }
        else
        {
            // The strike point: the force goes in again, inverted, after the time it takes to
            // reach the strike point and back (a comb filter on the excitation).
            const float beta = 0.5f - 0.46f * v.pos;
            const float N = sr_ / (f1 * bendRatio_ * std::exp2(v.dropSt / 12.0f));
            v.exB = x;
            v.exB.wait = std::max(1, (int)std::lround(beta * N));
            // A short pulse puts its energy into many harmonics: scale by sqrt(contact) so a hard
            // hit is brighter and spikier but not much louder.
            v.exScale = 0.22f * std::min(N, 1200.0f) * std::sqrt((float)std::min(x.len, 60) / 20.0f);
        }
    }

    // Bowed / blown modal voice: each mode's input gain is set so that its gain at resonance is its
    // strength at the contact point, whatever its decay. Then the friction or jet loop behaves the
    // same with a 0.5 s ring as with a 20 s one; Decay only changes how pure it is and how it rings on.
    void setSustainB(Voice& v)
    {
        // One force for every mode, scaled by the loss of the note's own mode: that mode's gain at
        // resonance is its strength, and every other mode's is its strength times how much longer
        // (or shorter) it rings. So the note's mode speaks first, and short-lived upper modes only
        // take over when the pressure is high (overblowing). Modes below the note (a bell's hum) are
        // left out, so a bowed or blown bell sings at the note.
        const float scale = 2.0f * (1.0f - v.r[v.ref]);
        for (int k = 0; k < kModes; ++k)
            v.b[k] = (v.alive[k] && k >= v.ref) ? v.gpos[k] * std::sin(v.w[k]) * scale : 0.0f;
        v.gmax = std::max(0.05f, v.gpos[v.ref]);
    }

    // Full coefficient update: frequencies, decays, tube, loss filter.
    void computeCoeffs(Voice& v, bool snap = false)
    {
        const ModelDef& m = kModels[v.model];
        v.hz = clamp(midiToHz((float)v.note + 12.0f * (float)s_.octave + 0.01f * (s_.fine + v.rndCents)), 20.0f, 0.4f * sr_);
        const float keyT = clamp(std::pow(262.0f / v.hz, 0.4f), 0.3f, 2.5f);   // high notes ring shorter
        const float muteT = v.choked ? 0.03f : 0.06f * std::pow(40.0f, 1.0f - s_.mute);
        float T0 = s_.decay * keyT;
        if (m.engine == kModal)
        {
            const float dampExp = 1.8f * s_.damp;
            const float str = 1.0f + 0.25f * s_.stretch;
            const float nyq = 0.45f * sr_;
            for (int k = 0; k < kModes; ++k)
            {
                const float ratio = std::pow(m.ratio[k], str);
                const float fk = v.hz * ratio;
                v.ratio[k] = ratio;
                v.lnTab[k] = std::log(m.ratio[k]);
                v.strMul[k] = lfoDStr_ != 0.0f ? std::exp(v.lnTab[k] * 0.25f * lfoDStr_) : 1.0f;
                v.alive[k] = fk < nyq && m.amp[k] > 0.0f;
                v.w[k] = kTwoPi * std::min(fk, nyq) / sr_;
                float T = T0 * m.dec[k] * std::pow(ratio, -dampExp) / (1.0f + (fk / 9000.0f) * (fk / 9000.0f));
                if (k == 0) T *= 1.0f - 0.3f * s_.tube;   // the tube takes energy from the fundamental
                if (v.muted) T = std::min(T, muteT);
                T = std::max(T, 0.004f);
                v.r[k] = std::exp(-6.9078f / (T * sr_));
                v.a2[k] = v.r[k] * v.r[k];
                if (!v.alive[k]) { v.y1[k] = v.y2[k] = 0.0f; v.b[k] = 0.0f; }
            }
            v.ref = 0;
            while (v.ref < kModes - 1 && m.ratio[v.ref] < 0.99f) ++v.ref;
            // The tube: a low-Q resonator (Q ~ 30) at the lowest mode, driven by that mode.
            const float f0 = v.hz * v.ratio[0];
            v.tw = kTwoPi * std::min(f0, nyq) / sr_;
            float tT = clamp(70.0f / f0, 0.06f, 1.2f);
            if (v.muted) tT = std::min(tT, muteT);
            v.tr = std::exp(-6.9078f / (tT * sr_));
            v.ta2 = v.tr * v.tr;
            const float G = 2.2f * s_.tube;
            v.tb = G * (1.0f - v.tr) * std::sqrt(std::max(1e-9f, 1.0f - 2.0f * v.tr * std::cos(2.0f * v.tw) + v.ta2));
            if (v.exc != kMallet) setSustainB(v);
        }
        else
        {
            const float d = clamp(m.dBase + m.dSpan * s_.damp, 0.0f, 0.95f);
            v.lpA = 1.0f - d;
            const float w = kTwoPi * v.hz / sr_;
            const float H = (1.0f - d) / std::sqrt(std::max(1e-9f, 1.0f - 2.0f * d * std::cos(w) + d * d));
            if (v.muted) T0 = std::min(T0, muteT);
            T0 = std::max(T0, 0.004f);
            const float gEff = std::pow(10.0f, -3.0f / (T0 * v.hz));   // round-trip gain at the fundamental
            v.loopG = std::min(0.99999f, gEff / H);
            v.fbScale = 2.0f * (1.0f - gEff);                          // blown: jet injection, normalised to the loss
            v.jlpK = 1.0f - std::exp(-kTwoPi * std::min(1.5f * v.hz, 0.4f * sr_) / sr_);
        }
        tune(v, snap);
    }

    // Pitch update only (bend, drop, vibrato): cheap enough to run every tick while the pitch moves.
    void tune(Voice& v, bool snap)
    {
        const ModelDef& m = kModels[v.model];
        float ratioNow = bendRatio_;
        if (v.dropSt > 0.0f) ratioNow *= std::exp2(v.dropSt / 12.0f);
        if (v.vibCents != 0.0f) ratioNow *= std::exp2(v.vibCents / 1200.0f);
        if (m.engine == kModal)
        {
            for (int k = 0; k < kModes; ++k)
            {
                float th = v.w[k] * ratioNow * v.strMul[k];
                if (th > 2.8f)
                {
                    // Pushed towards Nyquist by the drop: drop this mode for the rest of the note.
                    v.alive[k] = false; v.b[k] = 0.0f; v.gpos[k] = 0.0f; v.y1[k] = v.y2[k] = 0.0f; th = 2.8f;
                }
                v.a1[k] = 2.0f * v.r[k] * std::cos(th);
                if (v.exc != kMallet) v.cinv[k] = 0.5f / std::max(std::sin(0.5f * th), 1e-4f);
            }
            v.ta1 = 2.0f * v.tr * std::cos(std::min(v.tw * ratioNow * v.strMul[0], 2.8f));
        }
        else
        {
            const float f = std::max(20.0f, v.hz * ratioNow);
            const float w = kTwoPi * f / sr_;
            const float d = 1.0f - v.lpA;
            const float pd = std::atan2(d * std::sin(w), 1.0f - d * std::cos(w)) / w;   // loss filter delay
            const float total = clamp(sr_ / f - pd, 4.0f, (float)v.size - 4.0f);
            if (v.exc == kBow)
            {
                // Bowed: nut side and bridge side of the bow point.
                const float bridge = std::max(2.0f, v.beta * total), neck = std::max(2.0f, total - bridge);
                if (snap) { v.D = neck; v.D2 = bridge; v.dD = v.dD2 = 0.0f; v.dRemain = 0; }
                else { v.dD = (neck - v.D) / (float)kCtrl; v.dD2 = (bridge - v.D2) / (float)kCtrl; v.dRemain = kCtrl; }
            }
            else
            {
                const float target = std::max(3.0f, total);
                if (snap) { v.D = target; v.dD = 0.0f; v.dRemain = 0; }
                else { v.dD = (target - v.D) / (float)kCtrl; v.dRemain = kCtrl; }
            }
        }
    }

    // ----- LFO ------------------------------------------------------------------------------------------
    void lfoWrap()
    {
        lfoSH_ = 2.0f * rnd() - 1.0f;
        lfoDriftT_ = 2.0f * rnd() - 1.0f;
    }
    void setLfoPhase(float ph)
    {
        if (ph < lfoPhase_ - 0.5f) lfoWrap();          // crossed the start of a cycle
        lfoPhase_ = ph;
        lfoVal_ = lfoShape(lfoPhase_);
    }
    float lfoShape(float ph) const
    {
        switch (s_.lfoShape)
        {
            case kLfoTri:    return ph < 0.5f ? -1.0f + 4.0f * ph : 3.0f - 4.0f * ph;
            case kLfoSquare: return ph < 0.5f ? 1.0f : -1.0f;
            case kLfoRamp:   return 1.0f - 2.0f * ph;
            case kLfoSH:     return lfoSH_;
            case kLfoDrift:  return lfoDrift_;
            default:         return fastSin01(ph);
        }
    }
    void lfoTick()
    {
        const float inc = lfoHz_ * (float)kCtrl / sr_;
        lfoPhase_ += inc;
        if (lfoPhase_ >= 1.0f) { lfoPhase_ -= std::floor(lfoPhase_); lfoWrap(); }
        lfoDrift_ += (lfoDriftT_ - lfoDrift_) * std::min(1.0f, 4.0f * inc);   // Drift glides to each new random point
        lfoVal_ = lfoShape(lfoPhase_);
    }

    // Where this note is hit (or bowed): Strike Pos + its per-hit random offset + the LFO.
    float notePos(const Voice& v) const { return clamp(s_.pos + v.posRnd + 0.5f * s_.lfoPos * lfoVal_, 0.0f, 1.0f); }

    // How strongly each mode is driven at contact point `pos` (modal models).
    void modeWeights(Voice& v, float pos)
    {
        const ModelDef& m = kModels[v.model];
        const float norm = ampNorm_[v.model];
        const float d = 0.33f * pos;          // beam: distance of the strike from the centre
        const float t = 2.0f * pos - 1.0f;    // tilt: -1 dark .. +1 bright
        v.gmax = 0.05f;
        for (int k = 0; k < kModes; ++k)
        {
            if (!v.alive[k]) { v.gpos[k] = 0.0f; continue; }
            float g = 1.0f;
            if (m.posType == kPosBeam)
            {
                const int bn = m.beam[k];
                if (bn > 0)
                {
                    const float a = kPi * ((float)bn + 0.5f) * d;
                    const float sn = (bn & 1) ? std::cos(a) : std::sin(a);
                    g = 0.12f + 0.88f * std::fabs(sn);
                }
            }
            else g = clamp(std::pow(v.ratio[k], 0.6f * t), 0.1f, 4.0f);
            v.gpos[k] = m.amp[k] * norm * g;
            v.gmax = std::max(v.gmax, v.gpos[k]);
        }
    }

    void controlTick()
    {
        lfoTick();
        // LFO>Stretch bends every ringing modal voice's overtones; LFO>Pos moves a bow along the bar.
        const float dStr = s_.lfoStretch * lfoVal_;
        const bool strMoved = std::fabs(dStr - lfoDStr_) > 1e-3f || (dStr == 0.0f && lfoDStr_ != 0.0f);
        if (strMoved) lfoDStr_ = dStr;
        const bool bowMoves = s_.lfoPos != 0.0f;
        vibPhase_ = wrapPhase(vibPhase_ + s_.motorRate * (float)kCtrl / sr_);
        const float vibSin = fastSin01(vibPhase_);
        const float fadeInc = (float)kCtrl / (0.4f * sr_);
        for (Voice& v : voices_)
        {
            if (v.stage == kOff) continue;
            bool retune = false;
            if (kModels[v.model].engine == kModal)
            {
                if (strMoved)
                {
                    for (int k = 0; k < kModes; ++k) v.strMul[k] = std::exp(v.lnTab[k] * 0.25f * lfoDStr_);
                    retune = true;
                }
                if (bowMoves && v.exc != kMallet)
                {
                    v.pos = notePos(v);
                    modeWeights(v, v.pos);
                    setSustainB(v);
                }
            }
            if (v.dropSt > 0.0f)
            {
                v.dropSt *= dropK_;
                if (v.dropSt < 0.002f) v.dropSt = 0.0f;
                retune = true;
            }
            if (s_.vibrato > 0.0f || v.vibCents != 0.0f)
            {
                v.vibFade = std::min(1.0f, v.vibFade + fadeInc);
                const float c = 35.0f * s_.vibrato * v.vibFade * vibSin;
                if (c != v.vibCents) { v.vibCents = c; retune = true; }
            }
            if (retune) tune(v, false);
        }
    }

    // ----- per sample -------------------------------------------------------------------------------
    template <bool kExcite>
    static void renderModal(Voice& v, const float* exc, float* mono, int n)
    {
        float y1[kModes], y2[kModes], a1[kModes], a2[kModes], b[kModes];
        for (int k = 0; k < kModes; ++k) { y1[k] = v.y1[k]; y2[k] = v.y2[k]; a1[k] = v.a1[k]; a2[k] = v.a2[k]; b[k] = v.b[k]; }
        float t1 = v.t1, t2 = v.t2;
        const float ta1 = v.ta1, ta2 = v.ta2, tb = v.tb;
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            if (kExcite)
            {
                const float x = exc[i];
                for (int k = 0; k < kModes; ++k)
                {
                    const float y = a1[k] * y1[k] - a2[k] * y2[k] + b[k] * x;
                    y2[k] = y1[k]; y1[k] = y; s += y;
                }
            }
            else
            {
                for (int k = 0; k < kModes; ++k)
                {
                    const float y = a1[k] * y1[k] - a2[k] * y2[k];
                    y2[k] = y1[k]; y1[k] = y; s += y;
                }
            }
            const float t = ta1 * t1 - ta2 * t2 + tb * y1[0];
            t2 = t1; t1 = t;
            mono[i] = s + t;
        }
        for (int k = 0; k < kModes; ++k) { v.y1[k] = flushDenormal(y1[k]); v.y2[k] = flushDenormal(y2[k]); }
        v.t1 = flushDenormal(t1); v.t2 = flushDenormal(t2);
    }

    // The bow table (stick-slip friction): close to 1 while the bow grips (small relative velocity),
    // falling off as a power law once it slips. Pressure flattens it (wider grip).
    static inline float bowTable(float vrel, float slope)
    {
        float t = std::fabs(vrel * slope + 0.001f) + 0.75f;
        t *= t;
        return std::min(1.0f / (t * t), 0.98f);
    }

    inline float nextNoise(uint32_t& rng, float& n1, float& n2) const
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const float wn = (float)(int32_t)rng * 4.656613e-10f;
        n1 += nk1_ * (wn - n1);
        n2 += nk2_ * (n1 - n2);
        return n1 - n2;
    }

    // The air you hear (rosin scratch, breath) is shaped by the instrument: a wide band-pass centred two
    // to four times the note (higher with more pressure), not a hiss above everything. Chamberlin SVF;
    // the band output times 1/Q has unity gain at the centre.
    void airBand(const Voice& v, float p, float& f, float& q) const
    {
        const float fc = clamp(v.hz * (2.0f + 2.0f * p), 250.0f, std::min(3500.0f, 0.2f * sr_));
        f = 2.0f * std::sin(kPi * fc / sr_);
        q = 1.0f / 1.2f;
    }

    // Bowed or blown modal voice. The contact velocity (the sum of the mode velocities, one sample
    // late) drives the bow table or the jet; what comes out is the force into every mode.
    void renderModalDriven(Voice& v, const float* kick, float* mono, float* clk, int n)
    {
        float y1[kModes], y2[kModes], a1[kModes], a2[kModes], b[kModes], ci[kModes];
        for (int k = 0; k < kModes; ++k)
        { y1[k] = v.y1[k]; y2[k] = v.y2[k]; a1[k] = v.a1[k]; a2[k] = v.a2[k]; b[k] = v.b[k]; ci[k] = v.cinv[k]; }
        float t1 = v.t1, t2 = v.t2;
        const float ta1 = v.ta1, ta2 = v.ta2, tb = v.tb;
        float env = v.env, vp = v.vPrev, n1 = v.nlp1, n2 = v.nlp2, dcx = v.dcx, dcy = v.dcy;
        uint32_t rng = v.nrng;
        const float tgt = (v.held || v.sustained) ? 1.0f : 0.0f;
        const float kE = tgt > env ? kSwell_ : kLift_;
        const float p = v.press;
        const bool bow = v.exc == kBow;
        const float slope = 4.5f - 3.0f * p, vb = v.vb;
        const float G = kBowGain / v.gmax;
        const float jet = (3.0f + 8.0f * p) / v.gmax;
        const float push = 0.45f + 0.55f * p;          // blow: harder breath, louder and more saturated
        const float noiseIn = bow ? 0.25f * s_.air * vb : 0.5f * s_.air * (0.5f + p);
        const float airOut = s_.air * (bow ? 0.20f : 0.60f) * (0.5f + p);
        float aLp = v.aLp, aBp = v.aBp, af, aq;
        airBand(v, p, af, aq);
        const float out = v.susOut;
        for (int i = 0; i < n; ++i)
        {
            env += (tgt - env) * kE;
            const float nz = nextNoise(rng, n1, n2) * env;
            float x;
            if (bow)
            {
                const float vrel = vb * env - vp;
                x = G * env * vrel * bowTable(vrel, slope) + noiseIn * nz;
            }
            else x = env * push * fastTanh(jet * vp) + noiseIn * nz;
            if (kick) x += kick[i];
            // All modes side by side, then pairwise sums: the next sample waits on this one's contact
            // velocity, so the sum is a tree rather than a chain of eight adds.
            float yn[kModes], dv[kModes];
            for (int k = 0; k < kModes; ++k)
            {
                yn[k] = a1[k] * y1[k] - a2[k] * y2[k] + b[k] * x;
                dv[k] = (yn[k] - y1[k]) * ci[k];
            }
            vp = ((dv[0] + dv[1]) + (dv[2] + dv[3])) + ((dv[4] + dv[5]) + (dv[6] + dv[7]));
            const float s = ((yn[0] + yn[1]) + (yn[2] + yn[3])) + ((yn[4] + yn[5]) + (yn[6] + yn[7]));
            for (int k = 0; k < kModes; ++k) { y2[k] = y1[k]; y1[k] = yn[k]; }
            const float t = ta1 * t1 - ta2 * t2 + tb * y1[0];
            t2 = t1; t1 = t;
            const float o = s + t;
            const float hp = o - dcx + 0.995f * dcy;    // a steady bow force bends the bar: drop that DC
            dcx = o; dcy = hp;
            mono[i] = hp * out;
            aLp += af * aBp;
            const float aHp = nz - aLp - aq * aBp;
            aBp += af * aHp;
            clk[i] = aBp * aq * airOut * out;
        }
        for (int k = 0; k < kModes; ++k) { v.y1[k] = flushDenormal(y1[k]); v.y2[k] = flushDenormal(y2[k]); }
        v.t1 = flushDenormal(t1); v.t2 = flushDenormal(t2);
        v.env = flushDenormal(env); v.vPrev = flushDenormal(vp); v.nlp1 = n1; v.nlp2 = n2; v.nrng = rng;
        v.aLp = flushDenormal(aLp); v.aBp = flushDenormal(aBp);
        v.dcx = dcx; v.dcy = flushDenormal(dcy);
    }

    static void renderString(Voice& v, const float* exc, float* mono, int n)
    {
        float* buf = v.buf;
        const int mask = v.mask;
        const float fsize = (float)v.size;
        int w = v.wpos;
        float D = v.D, lp = v.lp, dcx = v.dcx, dcy = v.dcy;
        const float lpA = v.lpA, g = v.loopG, es = v.exScale;
        for (int i = 0; i < n; ++i)
        {
            float rp = (float)w - D;
            if (rp < 0.0f) rp += fsize;
            const int i0 = (int)rp;
            const float fr = rp - (float)i0;
            const float xm1 = buf[(i0 - 1) & mask], x0 = buf[i0 & mask], x1 = buf[(i0 + 1) & mask], x2 = buf[(i0 + 2) & mask];
            // 4-point cubic Hermite read.
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            const float y = ((c3 * fr + c2) * fr + c1) * fr + x0;
            lp += lpA * (y - lp);
            buf[w] = g * lp + (exc ? es * exc[i] : 0.0f);
            w = (w + 1) & mask;
            const float o = y - dcx + 0.995f * dcy;   // DC blocker on the output tap
            dcx = y; dcy = o;
            mono[i] = o;
            if (v.dRemain > 0) { D += v.dD; --v.dRemain; }
        }
        v.wpos = w; v.D = D; v.lp = flushDenormal(lp); v.dcx = dcx; v.dcy = flushDenormal(dcy);
    }

    // Blown string model: the waveguide becomes an air column. The (high-passed) wave at the
    // mouth, through a saturating jet, is pushed back in, scaled to the round-trip loss.
    void renderStringBlown(Voice& v, const float* kick, float* mono, float* clk, int n)
    {
        float* buf = v.buf;
        const int mask = v.mask;
        const float fsize = (float)v.size;
        int w = v.wpos;
        float D = v.D, lp = v.lp, dcx = v.dcx, dcy = v.dcy, hpS = v.hpS, jlp = v.jlp;
        const float jlpK = v.jlpK, es = v.exScale;
        float env = v.env, n1 = v.nlp1, n2 = v.nlp2;
        uint32_t rng = v.nrng;
        const float lpA = v.lpA, g = v.loopG, fb = v.fbScale, hpK = hpK_;
        const float tgt = (v.held || v.sustained) ? 1.0f : 0.0f;
        const float kE = tgt > env ? kSwell_ : kLift_;
        const float p = v.press;
        const float jet = 1.0f + 2.5f * p;
        const float push = 0.45f + 0.55f * p;
        const float noiseIn = 0.5f * s_.air * (0.5f + p);
        const float airOut = 0.60f * s_.air * (0.5f + p);
        float aLp = v.aLp, aBp = v.aBp, af, aq;
        airBand(v, p, af, aq);
        const float out = v.susOut;
        for (int i = 0; i < n; ++i)
        {
            env += (tgt - env) * kE;
            const float nz = nextNoise(rng, n1, n2) * env;
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
            hpS += hpK * (y - hpS);
            jlp += jlpK * ((y - hpS) - jlp);    // the jet hears the low harmonics best: the fundamental leads
            buf[w] = g * lp + fb * (env * push * fastTanh(jet * jlp) + noiseIn * nz) + (kick ? es * kick[i] : 0.0f);
            w = (w + 1) & mask;
            const float o = y - dcx + 0.995f * dcy;
            dcx = y; dcy = o;
            mono[i] = o * out;
            aLp += af * aBp;
            const float aHp = nz - aLp - aq * aBp;
            aBp += af * aHp;
            clk[i] = aBp * aq * airOut * out;
            if (v.dRemain > 0) { D += v.dD; --v.dRemain; }
        }
        v.wpos = w; v.D = D; v.lp = flushDenormal(lp); v.dcx = dcx; v.dcy = flushDenormal(dcy); v.hpS = flushDenormal(hpS);
        v.jlp = flushDenormal(jlp);
        v.env = flushDenormal(env); v.nlp1 = n1; v.nlp2 = n2; v.nrng = rng;
        v.aLp = flushDenormal(aLp); v.aBp = flushDenormal(aBp);
    }

    // Bowed string: a nut-side and a bridge-side delay line meet at the bow, where the bow table
    // decides how much of the bow's velocity the string takes (stick) or loses (slip).
    void renderStringBowed(Voice& v, const float* kick, float* mono, float* clk, int n)
    {
        float* nb = v.buf;
        float* bb = v.buf2;
        const int mask = v.mask;
        const float fsize = (float)v.size;
        int wn = v.wpos, wb = v.wpos2;
        float DN = v.D, DB = v.D2, lpB = v.lpB, dcx = v.dcx, dcy = v.dcy;
        float env = v.env, n1 = v.nlp1, n2 = v.nlp2;
        uint32_t rng = v.nrng;
        const float lpA = v.lpA, g = v.loopG, es = v.exScale;
        const float tgt = (v.held || v.sustained) ? 1.0f : 0.0f;
        const float kE = tgt > env ? kSwell_ : kLift_;
        const float p = v.press;
        const float slope = 4.5f - 3.0f * p, vb = v.vb;
        const float noiseIn = 0.25f * s_.air * vb;
        const float airOut = 0.20f * s_.air * (0.5f + p);
        float aLp = v.aLp, aBp = v.aBp, af, aq;
        airBand(v, p, af, aq);
        const float out = v.susOut;
        for (int i = 0; i < n; ++i)
        {
            env += (tgt - env) * kE;
            const float nz = nextNoise(rng, n1, n2) * env;
            float rp = (float)wn - DN;
            if (rp < 0.0f) rp += fsize;
            int i0 = (int)rp;
            float fr = rp - (float)i0;
            const float nOut = nb[i0 & mask] + fr * (nb[(i0 + 1) & mask] - nb[i0 & mask]);
            rp = (float)wb - DB;
            if (rp < 0.0f) rp += fsize;
            i0 = (int)rp;
            fr = rp - (float)i0;
            const float bOut = bb[i0 & mask] + fr * (bb[(i0 + 1) & mask] - bb[i0 & mask]);
            lpB += lpA * (bOut - lpB);
            const float bridgeRefl = -g * lpB, nutRefl = -nOut;
            const float vrel = vb * env - (bridgeRefl + nutRefl);
            const float f = env * vrel * bowTable(vrel, slope) + noiseIn * nz;
            nb[wn] = bridgeRefl + f + (kick ? es * kick[i] : 0.0f);
            bb[wb] = nutRefl + f;
            wn = (wn + 1) & mask; wb = (wb + 1) & mask;
            const float o = bOut - dcx + 0.995f * dcy;
            dcx = bOut; dcy = o;
            mono[i] = o * out;
            aLp += af * aBp;
            const float aHp = nz - aLp - aq * aBp;
            aBp += af * aHp;
            clk[i] = aBp * aq * airOut * out;
            if (v.dRemain > 0) { DN += v.dD; DB += v.dD2; --v.dRemain; }
        }
        v.wpos = wn; v.wpos2 = wb; v.D = DN; v.D2 = DB; v.lpB = flushDenormal(lpB); v.dcx = dcx; v.dcy = flushDenormal(dcy);
        v.env = flushDenormal(env); v.nlp1 = n1; v.nlp2 = n2; v.nrng = rng;
        v.aLp = flushDenormal(aLp); v.aBp = flushDenormal(aBp);
    }

    // Buzz: a loose buzzer touches the object whenever it swings past a threshold (a fraction of the
    // note's peak, lower with more Buzz). The part of the wave beyond it, roughened by contact noise
    // and high-passed at 1.2 kHz, is the rattle. It stops by itself as the ring decays below the
    // threshold. Soft notes buzz less: the peak is never taken as less than a medium hit's.
    void applyBuzz(Voice& v, float* mono, int n)
    {
        const float b = s_.buzz;
        const float frac = 0.55f - 0.5f * b;
        const float gain = 1.6f * b;
        const float k = bzK_;
        float pk = v.buzzPk, h = v.bzH;
        uint32_t r = v.brng;
        const float ref = v.buzzRef;
        for (int i = 0; i < n; ++i)
        {
            const float x = mono[i], a = std::fabs(x);
            pk = std::max(pk, a);
            const float e = a - frac * std::max(pk, ref);
            float rattle = 0.0f;
            if (e > 0.0f)
            {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                const float nz = (float)(int32_t)r * 4.656613e-10f;
                rattle = (x > 0.0f ? e : -e) * (0.6f + 1.4f * nz);
            }
            h += k * (rattle - h);
            mono[i] = x + (rattle - h) * gain;
        }
        v.buzzPk = pk; v.bzH = flushDenormal(h); v.brng = r;
    }

    void renderVoice(Voice& v, float* L, float* R, int n)
    {
        const bool string = kModels[v.model].engine == kString;
        float exc[kCtrl], clk[kCtrl], mono[kCtrl];
        bool haveClk = false;
        if (v.exc != kMallet)
        {
            const bool kicking = v.ex.active() || v.exB.active();
            if (kicking)
                for (int i = 0; i < n; ++i)
                {
                    float a, o, bb = 0.0f, ob;
                    v.ex.next(a, o);
                    if (string) v.exB.next(bb, ob);
                    exc[i] = a - bb;
                }
            const float* kick = kicking ? exc : nullptr;
            if (!string) renderModalDriven(v, kick, mono, clk, n);
            else if (v.exc == kBow) renderStringBowed(v, kick, mono, clk, n);
            else renderStringBlown(v, kick, mono, clk, n);
            haveClk = true;
        }
        else
        {
            const bool excite = v.ex.active() || v.exB.active();
            if (excite)
            {
                for (int i = 0; i < n; ++i)
                {
                    float a, o, bb = 0.0f, ob;
                    v.ex.next(a, o);
                    if (string) v.exB.next(bb, ob);
                    exc[i] = a - bb;
                    clk[i] = o;
                }
            }
            haveClk = excite;
            if (string) renderString(v, excite ? exc : nullptr, mono, n);
            else if (excite) renderModal<true>(v, exc, mono, n);
            else renderModal<false>(v, nullptr, mono, n);
        }

        if (haveClk) for (int i = 0; i < n; ++i) mono[i] += clk[i];
        if (s_.buzz > 0.0f) applyBuzz(v, mono, n);

        float peak = v.peak;
        const float gL = v.gL, gR = v.gR;
        if (v.stage == kSteal)
        {
            float u = v.fade;
            for (int i = 0; i < n; ++i)
            {
                const float g = 1.0f - u * u * (3.0f - 2.0f * u);
                const float s = mono[i] * g;
                u = std::min(1.0f, u + v.fadeStep);
                L[i] += s * gL;
                if (R) R[i] += s * gR;
            }
            v.fade = u;
            if (u >= 1.0f)
            {
                v.stage = kOff;
                if (v.pendNote >= 0) start(v, v.pendNote, v.pendVel);
            }
            return;
        }
        for (int i = 0; i < n; ++i)
        {
            const float s = mono[i];
            peak = std::max(peak, std::fabs(s));
            L[i] += s * gL;
            if (R) R[i] += s * gR;
        }
        v.peak = peak;
    }

    // ----- state ----------------------------------------------------------------------------------
    float sr_ = 44100.0f;
    Settings s_;
    CoefKey lastKey_{};
    bool haveKey_ = false, coefDirty_ = true, tuneDirty_ = false;
    float bendRatio_ = 1.0f, dropK_ = 0.9f;
    float kSwell_ = 0.01f, kLift_ = 0.01f, nk1_ = 0.3f, nk2_ = 0.04f, hpK_ = 0.004f;
    float chanAT_ = 0.0f, vibPhase_ = 0.0f;
    // 1.2: LFO and randomness
    float lfoHz_ = 0.5f, lfoPhase_ = 0.0f, lfoVal_ = 0.0f, lfoSH_ = 0.0f, lfoDriftT_ = 0.0f, lfoDrift_ = 0.0f, lfoBeats_ = 4.0f;
    float lfoDStr_ = 0.0f, bzK_ = 0.15f;
    uint32_t rng_ = 0x2545F491u;
    float rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return (float)(rng_ >> 8) * (1.0f / 16777216.0f); }
    float ampNorm_[kNumModels] = {};
    Voice voices_[kNumSlots];
    std::vector<float> strBuf_;
    int ctrlLeft_ = 0;
    uint32_t orderCounter_ = 0;
    bool sustain_ = false;
    float bend_ = 0.0f, modWheel_ = 0.0f;
    float motorPhase_ = 0.0f;
    SVF body_[4];
    float lastBodySize_ = -1.0f, lastRoomSize_ = -1.0f;
    float hpL_ = 0.0f, hpR_ = 0.0f, hpCoef_ = 0.02f;
    std::vector<float> fdnBuf_[8];
    int fdnLen_[8] = {};
    int fdnMask_[8] = {};
    float fdnG_[8] = {};
    float fdnLp_[8] = {};
    float fdnInGain_ = 0.3f, fdnDamp_ = 0.6f;
    int fdnPos_ = 0;
    float modPhase1_ = 0.0f, modPhase2_ = 0.37f;
    Smoother volume_;
    LinearRamp bodyRamp_, dryRamp_, wetRamp_, motorRamp_;
};

} // namespace hammerhead

using hammerhead::Hammerhead;
MPC_PLUGIN_ENTRY(Hammerhead)
