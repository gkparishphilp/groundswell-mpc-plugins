// Cuttlefish - a morphing wavetable polysynth for the MPC, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// A cuttlefish changes colour; this synth changes timbre. Two wavetable oscillators sweep through
// sets of drawn, additive, vocal, metallic and digital waveforms with Morph, warped by bend, squash,
// sync or FM, and kept moving by a morph envelope, two LFOs (free or locked to the song), a small
// mod matrix, Drift and one Colour macro made for a Q-Link. Meant as the family's everyday synth.
//
// SIGNAL FLOW (per voice)
//
//   OSC 1 ─┐  Table + Morph (32 frames, read between neighbours) ─ Warp (Bend / Squash / Sync / FM)
//   OSC 2 ─┤  Oct / Semi / Fine / Level; Unison 1-4 detuned copies spread across the stereo field
//   SUB   ─┤  sine/triangle/square one or two octaves under Osc 1
//   NOISE ─┘  white noise through a one-pole tone control
//          ─> FILTER  multimode SVF (LP12, LP24, BP, HP, Notch) with Drive; cutoff from the knob +
//                     filter envelope + key track + velocity + Colour + matrix + Drift
//          ─> AMP     ADSR x velocity x matrix Level, pan
//   all voices ─> CHORUS ─> PING-PONG DELAY ─> REVERB ─> Output ─> soft limiter
//
//   Motion: Mod Env (ADSR, Env>Morph to both morphs), LFO 1 / LFO 2 (7 shapes incl. smooth random;
//   Free, Retrig, Sync = locked to the transport, Sync Rtg = tempo rate restarted per note), LFO 1
//   fade-in, 4 matrix slots, Drift (slow random wander of pitch per oscillator, morph and cutoff),
//   Colour (moves Morph 1, Morph 2 and cutoff by the Col> amounts; also a matrix source).
//
// CPU: everything with exp/tan/pow runs once per 16-sample control tick per voice (fast polynomial
// versions) or per block. Per sample per unison copy: two phase steps, two warps, two 4-read
// table lookups. See README.md for measured numbers.

#include "mpc_plugin.h"
#include "dsp/math.h"

#include "Wavetables.h"
#include "Fx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Test hook: tests/rendertest.cpp switches per-sample morph ramping off to prove the zipper check works.
#ifndef CUTTLEFISH_NORAMP
#define CUTTLEFISH_NORAMP false
#endif

namespace cuttlefish {

constexpr int kMaxVoices = 16;         // live voices
constexpr int kNumSlots  = 20;         // + 4 spare slots so a stolen voice fades out in its own slot
constexpr int kCtrl      = 16;         // samples per control tick
constexpr int kMaxUni    = 4;
constexpr int kMaxStack  = 16;
constexpr float kVoiceGain = 0.90f;    // calibrated with tests/rendertest.cpp

enum PlayMode  { kPoly = 0, kMono, kLegato };
enum WarpMode  { kWBend = 0, kWSquash, kWSync, kWFM };
enum LfoShape  { kShSine = 0, kShTri, kShSawUp, kShSawDown, kShSquare, kShSH, kShSmooth, kNumShapes };
enum LfoMode   { kLfoFree = 0, kLfoRetrig, kLfoSync, kLfoSyncRtg };
enum Source    { kSrcOff = 0, kSrcLfo1, kSrcLfo2, kSrcModEnv, kSrcFiltEnv, kSrcAmpEnv, kSrcVel, kSrcWheel,
                 kSrcAT, kSrcKey, kSrcLfo1MW, kSrcRandom, kSrcColour, kNumSrc };
enum Dest      { kDstMorph1 = 0, kDstMorph2, kDstMorphBoth, kDstWarp1, kDstWarp2, kDstCutoff, kDstReso,
                 kDstPitch, kDstPitch2, kDstOscMix, kDstPan, kDstLevel, kDstDetune, kDstNoise, kNumDst };
enum FiltMode  { kLP12 = 0, kLP24, kBP, kHP, kNotch };
enum EnvStage  { kEnvOff = 0, kEnvAttack, kEnvDecay, kEnvRelease };

static const char* const kShapeNames[kNumShapes] = {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth Rnd"};
static const char* const kSrcNames[kNumSrc] = {"Off", "LFO 1", "LFO 2", "Mod Env", "Filt Env", "Amp Env", "Velocity",
                                               "Mod Wheel", "Aftertouch", "Key", "LFO1 x MW", "Random", "Colour"};
static const char* const kDstNames[kNumDst] = {"Morph 1", "Morph 2", "Morph 1+2", "Warp 1", "Warp 2", "Cutoff", "Reso",
                                               "Pitch", "Pitch 2", "Osc Mix", "Pan", "Level", "Detune", "Noise"};
// LFO divisions (quarter-note beats; bars assume 4/4)
static const char* const kLfoDivNames[] = {"8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/2T", "1/4D", "1/4", "1/4T",
                                           "1/8D", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const double kLfoDivBeats[] = {32.0, 16.0, 8.0, 4.0, 2.0, 4.0 / 3.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0,
                                      0.25, 1.0 / 6.0, 0.125};
constexpr int kNumLfoDivs = 15;
static const char* const kDlyDivNames[] = {"1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4",
                                           "1/2T", "1/4D", "1/2"};
static const double kDlyDivBeats[] = {0.125, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.375, 0.5, 2.0 / 3.0, 0.75, 1.0, 4.0 / 3.0, 1.5, 2.0};
constexpr int kNumDlyDivs = 12;

// unison layouts: detune offsets and pan positions for 1..4 copies
static const float kUniOff[5][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {-1, 1, 0, 0}, {-1, 0, 1, 0}, {-1, -0.33f, 0.33f, 1}};

// ---- fast control-rate maths ----------------------------------------------------------------------
inline float fastExp2(float x)
{
    x = mpc::dsp::clamp(x, -60.0f, 60.0f);
    int i = (int)x; if ((float)i > x) --i;
    const float f = x - (float)i;
    float p = 1.0f + f * (0.693147181f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f
            + f * (0.00133335581f + f * 0.000154035304f)))));
    int32_t bits; std::memcpy(&bits, &p, 4); bits += i << 23; std::memcpy(&p, &bits, 4);
    return p;
}
// tan(x) for 0 <= x <= ~1.45 (Pade 7/6)
inline float fastTan(float x)
{
    const float x2 = x * x;
    return x * (135135.0f - x2 * (17325.0f - x2 * (378.0f - x2))) / (135135.0f - x2 * (62370.0f - x2 * (3150.0f - 28.0f * x2)));
}
inline float wrap01(float p) { const int i = (int)(p + 16.0f); return p + 16.0f - (float)i; }
inline float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }
inline uint32_t hashU(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
inline float hashBi(uint32_t x) { return (float)(hashU(x) & 0xFFFFFF) / 8388608.0f - 1.0f; }

struct Rng
{
    uint32_t s = 1;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float bi() { return (float)(next() & 0xFFFFFF) / 8388608.0f - 1.0f; }     // -1..1
    float uni() { return (float)(next() & 0xFFFFFF) / 16777216.0f; }         // 0..1
};

// A slow random wander: a new target every 1/rate seconds, smoothstepped between.
struct Wander
{
    float a = 0.0f, b = 0.0f, ph = 0.0f, rate = 0.5f;
    void init(Rng& r) { a = r.bi(); b = r.bi(); ph = r.uni(); rate = 0.25f + 0.5f * r.uni(); }
    float step(float dt, Rng& r)
    {
        ph += rate * dt;
        if (ph >= 1.0f) { ph -= 1.0f; if (ph >= 1.0f) ph = 0.0f; a = b; b = r.bi(); }
        return a + (b - a) * smoothstep(ph);
    }
};

// LFO waveform at phase p in [0,1); r0 -> r1 are this cycle's random endpoints.
inline float lfoShape(int shape, float p, float r0, float r1)
{
    switch (shape)
    {
        case kShSine:    return mpc::dsp::fastSin01(p);
        case kShTri:     { const float q = wrap01(p + 0.25f); return 1.0f - 4.0f * std::fabs(q - 0.5f); }
        case kShSawUp:   return 2.0f * p - 1.0f;
        case kShSawDown: return 1.0f - 2.0f * p;
        case kShSquare:  return p < 0.5f ? 1.0f : -1.0f;
        case kShSH:      return r1;
        case kShSmooth:  return r0 + (r1 - r0) * smoothstep(p);
        default:         return 0.0f;
    }
}

constexpr float kSqDelta = 0.04f;     // half-width of Squash's rounded corners (in cycles)

struct WarpK
{
    int mode = kWBend;
    float a = 0.0f;                       // bend (phase pushed by a sine: smooth across the wrap)
    float ws = 0.5f, sA = 1.0f, sB = 1.0f, qA = 0.0f, qB = 0.0f;   // squash
    float k = 1.0f, win = 0.0f;           // sync
    float d = 0.0f;                       // fm depth (cycles)
    float factor = 1.0f;                  // highest local speed-up (for choosing the mip level)
    bool active = false;

    void set(int m, float w)
    {
        mode = m; w = mpc::dsp::clamp(w, 0.0f, 1.0f); active = w > 0.0005f;
        switch (m)
        {
            case kWBend:   a = 0.155f * w; factor = 1.0f + w; break;
            case kWSquash:
                ws = 0.5f - 0.40f * w; sA = 0.5f / ws; sB = 0.5f / (1.0f - ws); factor = sA;
                qA = (sB - sA) * (0.25f / kSqDelta); qB = (sA - sB) * (0.25f / kSqDelta);
                break;
            case kWSync:   k = 1.0f + 7.0f * w; win = std::min(1.0f, w * 5.0f); factor = 1.15f * k + 1.5f; break;
            case kWFM:     d = w * w * 1.2f; factor = 1.0f + 12.0f * d; break;
            default: break;
        }
    }
    // returns the warped phase; for Sync also sets the window gain
    inline float apply(float p, float fm, float& gain) const
    {
        switch (mode)
        {
            case kWBend:   return wrap01(p + a * mpc::dsp::fastSin01(p));
            case kWSquash:
            {
                // Casio-style phase distortion: the first half of the cycle is read at slope sA in
                // [0, ws), the second half at sB. Both corners (at ws, and at the wrap) are rounded
                // with a quadratic over +-kSqDelta so the slope changes smoothly (much less aliasing).
                if (p >= ws + kSqDelta && p < 1.0f - kSqDelta) return 0.5f + (p - ws) * sB;
                if (p >= kSqDelta && p < ws - kSqDelta) return p * sA;
                if (p >= ws - kSqDelta && p < ws + kSqDelta)
                {
                    const float u = p - ws + kSqDelta;
                    return 0.5f - sA * kSqDelta + sA * u + qA * u * u;
                }
                const float u = (p < 0.5f ? p : p - 1.0f) + kSqDelta;      // around the wrap
                const float y = -sB * kSqDelta + sB * u + qB * u * u;
                return y < 0.0f ? y + 1.0f : y;
            }
            case kWSync:
            {
                float s = p * k; s -= (float)(int)s;
                const float u = (2.0f * p - 1.0f) * (2.0f * p - 1.0f);
                const float w = 1.0f - u * u * u;          // flat top, 0 at the cycle's ends,
                gain = 1.0f - win * (1.0f - w * w);        // squared so its slope is 0 there too
                return s;
            }
            case kWFM:     return wrap01(p + d * fm);
            default:       return p;
        }
    }
};

struct Voice
{
    bool active = false, fading = false, held = false, sustained = false;
    float fade = 1.0f, fadeStep = 0.0f;
    int note = 60; float vel = 0.0f; uint32_t order = 0;
    float pitch = 60.0f, target = 60.0f, glideStep = 0.0f;
    float rnd = 0.0f, at = 0.0f, age = 0.0f;
    int ampStage = kEnvOff; float amp = 0.0f;
    int fStage = kEnvOff; float fEnv = 0.0f;
    int mStage = kEnvOff; float mEnv = 0.0f;
    float ph1[kMaxUni] = {}, ph2[kMaxUni] = {}, phSub = 0.0f;
    float inc1[kMaxUni] = {}, inc2[kMaxUni] = {}, incSub = 0.0f;
    float m1 = 0.0f, m2 = 0.0f, dm1 = 0.0f, dm2 = 0.0f;
    bool started = false;                 // control values valid (morph ramps start from target)
    int lvl1 = 0, lvl2 = 0, lvlSub = 0, uni = 1;
    WarpK w1, w2;
    float c1L[kMaxUni] = {}, c1R[kMaxUni] = {}, c2L[kMaxUni] = {}, c2R[kMaxUni] = {};
    float subG = 0.0f, noiseG = 0.0f;
    float gL = 0.0f, gR = 0.0f, dgL = 0.0f, dgR = 0.0f;
    // filter: stage A (all modes), stage B (LP24)
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, kA = 2.0f, b1 = 1.0f, b2 = 0.0f, b3 = 0.0f, bpGain = 1.0f;
    float sL[4] = {}, sR[4] = {};
    bool stereo = false;
    float nz = 0.0f;
    Rng rng;
    // per-voice LFOs (Retrig modes)
    float lph[2] = {}, lr0[2] = {}, lr1[2] = {}, lout[2] = {};
    Wander drift[4];                      // pitch 1, pitch 2, morph, cutoff
};

class Cuttlefish : public mpc::Plugin
{
public:
    enum Params
    {
        pOutput = 0,
        pTable1, pMorph1, pWarpMode1, pWarp1, pOct1, pSemi1, pFine1, pLevel1,
        pTable2, pMorph2, pWarpMode2, pWarp2, pOct2, pSemi2, pFine2, pLevel2,
        pSub, pSubType, pNoise, pNoiseTone,
        pUnison, pDetune, pWidth, pDrift, pPhase,
        pPlayMode, pGlide, pBendRange, pVoices,
        pColour, pColMorph1, pColMorph2, pColFilter,
        pMAttack, pMDecay, pMSustain, pMRelease, pEnvMorph,
        pL1Rate, pL1Div, pL1Shape, pL1Mode, pL1Fade,
        pL2Rate, pL2Div, pL2Shape, pL2Mode,
        pSrc1, pDst1, pAmt1, pSrc2, pDst2, pAmt2, pSrc3, pDst3, pAmt3, pSrc4, pDst4, pAmt4,
        pFilterMode, pCutoff, pReso, pDrive, pEnvAmt, pKeyTrack, pVelFilter, pVelLevel,
        pFAttack, pFDecay, pFSustain, pFRelease,
        pAttack, pDecay, pSustain, pRelease,
        pChorus, pChRate, pChDepth, pChMode,
        pDelay, pDlyTime, pDlyFdbk, pDlyTone,
        pReverb, pRevSize, pRevTone, pRevPre,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Cuttlefish";
        i.vendor = "Groundswell";
        i.uniqueId = MPC_FOURCC('G', 'q', 'J', '3');   // == "uid" in plugin.json. Never change.
        i.version = 1100;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Cuttlefish() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        bank_ = &WavetableBank::get();
        std::vector<std::string> tabs, shapes, srcs, dsts, ldivs, ddivs;
        for (const char* t : kTableNames) tabs.push_back(t);
        for (const char* s : kShapeNames) shapes.push_back(s);
        for (const char* s : kSrcNames) srcs.push_back(s);
        for (const char* d : kDstNames) dsts.push_back(d);
        for (const char* d : kLfoDivNames) ldivs.push_back(d);
        for (const char* d : kDlyDivNames) ddivs.push_back(d);
        const std::vector<std::string> warps = {"Bend", "Squash", "Sync", "FM"};
        const std::vector<std::string> lmodes = {"Free", "Retrig", "Sync", "Sync Rtg"};

        // Output level (parameter 0: the host nudges it on load, so it only sets a gain).
        addParam(floatParam ("out",       "Output", -40.0f, 6.0f, -3.0f, "dB", 1));
        // ---- oscillator 1 ----
        addParam(choiceParam("tab1",      "Table 1", tabs, kTabBasic));
        // Position in the table: 0 % = first frame, 100 % = last.
        addParam(floatParam ("morph1",    "Morph 1", 0.0f, 100.0f, 50.0f, "%", 0));
        // Bend leans the cycle; Squash folds it into its first half (phase distortion); Sync is a
        // hard-sync sweep; FM is phase modulation by oscillator 2.
        addParam(choiceParam("warpmode1", "Warp Mode 1", warps, kWBend));
        addParam(floatParam ("warp1",     "Warp 1", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam   ("oct1",      "Oct 1", -3, 3, 0));
        addParam(intParam   ("semi1",     "Semi 1", -12, 12, 0));
        addParam(floatParam ("fine1",     "Fine 1", -50.0f, 50.0f, 0.0f, "ct", 0));
        addParam(floatParam ("level1",    "Level 1", 0.0f, 100.0f, 80.0f, "%", 0));
        // ---- oscillator 2 (its FM warp is modulated by a sine at oscillator 1's pitch) ----
        addParam(choiceParam("tab2",      "Table 2", tabs, kTabBasic));
        addParam(floatParam ("morph2",    "Morph 2", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(choiceParam("warpmode2", "Warp Mode 2", warps, kWBend));
        addParam(floatParam ("warp2",     "Warp 2", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam   ("oct2",      "Oct 2", -3, 3, 0));
        addParam(intParam   ("semi2",     "Semi 2", -12, 12, 0));
        addParam(floatParam ("fine2",     "Fine 2", -50.0f, 50.0f, 7.0f, "ct", 0));
        addParam(floatParam ("level2",    "Level 2", 0.0f, 100.0f, 0.0f, "%", 0));
        // ---- sub and noise ----
        addParam(floatParam ("sub",       "Sub", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("subtype",   "Sub Type", {"Sine -1", "Sine -2", "Tri -1", "Sqr -1"}, 0));
        addParam(floatParam ("noise",     "Noise", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("noisetone", "Noise Tone", 0.0f, 100.0f, 70.0f, "%", 0));
        // ---- unison and drift ----
        addParam(intParam   ("unison",    "Unison", 1, 4, 1));
        addParam(floatParam ("detune",    "Detune", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam ("width",     "Width", 0.0f, 100.0f, 60.0f, "%", 0));
        // Slow random wander of each oscillator's pitch (up to 12 cents), the morphs and the cutoff.
        addParam(floatParam ("drift",     "Drift", 0.0f, 100.0f, 10.0f, "%", 0));
        // Free: oscillators start each note at a random point. Reset: at a fixed point (tighter attacks).
        addParam(choiceParam("phase",     "Phase", {"Free", "Reset"}, 0));
        // ---- playing ----
        addParam(choiceParam("playmode",  "Play Mode", {"Poly", "Mono", "Legato"}, kPoly));
        addParam(floatParam ("glide",     "Glide", 0.0f, 2000.0f, 0.0f, "", 0, 0.4f));      // text from formatValue
        addParam(intParam   ("bendrange", "Bend Range", 0, 24, 2));
        addParam(intParam   ("voices",    "Voices", 1, kMaxVoices, 12));
        // ---- colour macro ----
        addParam(floatParam ("colour",    "Colour", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("colmorph1", "Col>Morph 1", -100.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("colmorph2", "Col>Morph 2", -100.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("colfilter", "Col>Filter", -100.0f, 100.0f, 30.0f, "%", 0));
        // ---- morph (mod) envelope ----
        addParam(logParam   ("mattack",   "M Attack", 1.0f, 10000.0f, 1.0f, "", 0));     // text from formatValue
        addParam(logParam   ("mdecay",    "M Decay", 1.0f, 10000.0f, 800.0f, "", 0));
        addParam(floatParam ("msustain",  "M Sustain", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("mrelease",  "M Release", 1.0f, 10000.0f, 400.0f, "", 0));
        addParam(floatParam ("envmorph",  "Env>Morph", -100.0f, 100.0f, 0.0f, "%", 0));
        // ---- LFO 1 ----
        addParam(logParam   ("l1rate",    "LFO1 Rate", 0.02f, 30.0f, 5.0f, "Hz", 2));
        addParam(choiceParam("l1div",     "LFO1 Div", ldivs, 10));
        addParam(choiceParam("l1shape",   "LFO1 Shape", shapes, kShSine));
        // Free: one shared LFO at Rate. Retrig: each note its own, from the start. Sync: shared, at Div
        // and locked to the song position. Sync Rtg: each note its own at Div.
        addParam(choiceParam("l1mode",    "LFO1 Mode", lmodes, kLfoFree));
        addParam(floatParam ("l1fade",    "LFO1 Fade", 0.0f, 5000.0f, 0.0f, "", 0, 0.4f)); // text from formatValue
        // ---- LFO 2 ----
        addParam(logParam   ("l2rate",    "LFO2 Rate", 0.02f, 30.0f, 0.3f, "Hz", 2));
        addParam(choiceParam("l2div",     "LFO2 Div", ldivs, 3));
        addParam(choiceParam("l2shape",   "LFO2 Shape", shapes, kShTri));
        addParam(choiceParam("l2mode",    "LFO2 Mode", lmodes, kLfoFree));
        // ---- mod matrix (4 slots). Pitch amounts are curved: 10 % = 0.12 st, 100 % = 12 st. ----
        const int defSrc[4] = {kSrcLfo1MW, kSrcLfo2, kSrcAT, kSrcVel};
        const int defDst[4] = {kDstPitch, kDstMorphBoth, kDstCutoff, kDstMorph1};
        const float defAmt[4] = {15.0f, 0.0f, 0.0f, 0.0f};
        static const char* const ids[4][3] = {{"src1", "dst1", "amt1"}, {"src2", "dst2", "amt2"}, {"src3", "dst3", "amt3"}, {"src4", "dst4", "amt4"}};
        static const char* const names[4][3] = {{"Src 1", "Dst 1", "Amt 1"}, {"Src 2", "Dst 2", "Amt 2"}, {"Src 3", "Dst 3", "Amt 3"}, {"Src 4", "Dst 4", "Amt 4"}};
        for (int s = 0; s < 4; ++s)
        {
            addParam(choiceParam(ids[s][0], names[s][0], srcs, defSrc[s]));
            addParam(choiceParam(ids[s][1], names[s][1], dsts, defDst[s]));
            addParam(floatParam (ids[s][2], names[s][2], -100.0f, 100.0f, defAmt[s], "%", 0));
        }
        // ---- filter ----
        addParam(choiceParam("filtmode",  "Filter", {"LP12", "LP24", "BP", "HP", "Notch"}, kLP24));
        addParam(logParam   ("cutoff",    "Cutoff", 20.0f, 20000.0f, 20000.0f, "", 0));   // text from formatValue
        addParam(floatParam ("reso",      "Reso", 0.0f, 100.0f, 10.0f, "%", 0));
        addParam(floatParam ("drive",     "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        // Filter envelope depth: 100 % = +5 octaves at the envelope's peak.
        addParam(floatParam ("envamt",    "Env Amt", -100.0f, 100.0f, 0.0f, "%", 0));
        // 100 % = the cutoff follows the keyboard one-to-one (around C4).
        addParam(floatParam ("keytrack",  "Key Track", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("velfilter", "Vel>Filter", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("vellevel",  "Vel>Level", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(logParam   ("fattack",   "F Attack", 1.0f, 10000.0f, 1.0f, "", 0));
        addParam(logParam   ("fdecay",    "F Decay", 1.0f, 10000.0f, 500.0f, "", 0));
        addParam(floatParam ("fsustain",  "F Sustain", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(logParam   ("frelease",  "F Release", 1.0f, 10000.0f, 400.0f, "", 0));
        // ---- amp ----
        addParam(logParam   ("attack",    "Attack", 1.0f, 10000.0f, 2.0f, "", 0));
        addParam(logParam   ("decay",     "Decay", 1.0f, 10000.0f, 600.0f, "", 0));
        addParam(floatParam ("sustain",   "Sustain", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(logParam   ("release",   "Release", 1.0f, 10000.0f, 250.0f, "", 0));
        // ---- effects ----
        addParam(floatParam ("chorus",    "Chorus", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("chrate",    "Ch Rate", 0.05f, 5.0f, 0.6f, "Hz", 2));
        addParam(floatParam ("chdepth",   "Ch Depth", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(choiceParam("chmode",    "Ch Mode", {"Chorus", "Ensemble", "Flange"}, 0));
        addParam(floatParam ("delay",     "Delay", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("dlytime",   "Dly Time", ddivs, 7));
        addParam(floatParam ("dlyfdbk",   "Dly Fdbk", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam(floatParam ("dlytone",   "Dly Tone", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("reverb",    "Reverb", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("revsize",   "Rev Size", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("revtone",   "Rev Tone", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("revpre",    "Rev Pre", 0.0f, 100.0f, 10.0f, "ms", 0));

        if (numParams() != kNumParams) mpc::log("Cuttlefish: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        addPrograms();
    }

    // ----- lifecycle ------------------------------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        chorus_.prepare(sr_); delay_.prepare(sr_); reverb_.prepare(sr_);
        mipLimit_ = mpc::dsp::clamp((sr_ - 19000.0f) / sr_, 0.45f, 0.85f);
    }

    void reset() override
    {
        for (Voice& v : voices_) { v.active = false; v.fading = false; v.held = v.sustained = false; v.ampStage = kEnvOff; }
        numStack_ = 0; monoVoice_ = -1; pedal_ = false;
        wheel_ = wheelT_ = at_ = atT_ = bend_ = bendT_ = 0.0f;
        hasLast_ = false; lastPitch_ = 60.0f;
        ctrlLeft_ = 0; orderCounter_ = 0;
        for (int l = 0; l < 2; ++l) { gph_[l] = 0.0f; gcycle_[l] = 0; gr0_[l] = 0.0f; gr1_[l] = 0.0f; gout_[l] = 0.0f; }
        rng_.s = 0x9E3779B9u;
        chorus_.reset(); delay_.reset(); reverb_.reset();
        fxOn_[0] = fxOn_[1] = fxOn_[2] = false;
        readSettings(nullptr);
        snapGlobals();
        outGain_ = targetOut_;
        playModeWas_ = (int)set_.playMode;
    }

    // ----- value text ----------------------------------------------------------------------------
    bool formatValue(int index, float v, std::string& out) override
    {
        char b[32];
        auto timeText = [&](float ms) {
            if (ms < 1000.0f) std::snprintf(b, sizeof(b), "%.0f ms", ms); else std::snprintf(b, sizeof(b), "%.2f s", ms * 0.001f);
            out = b; };
        switch (index)
        {
            case pMAttack: case pMDecay: case pMRelease: case pFAttack: case pFDecay: case pFRelease:
            case pAttack: case pDecay: case pRelease:
                timeText(v); return true;
            case pGlide: case pL1Fade:
                if (v < 0.5f) out = "Off"; else timeText(v);
                return true;
            case pCutoff:
                if (v >= 19900.0f) out = "Open";
                else { if (v < 1000.0f) std::snprintf(b, sizeof(b), "%.0f Hz", v); else std::snprintf(b, sizeof(b), "%.1f kHz", v * 0.001f); out = b; }
                return true;
            case pOct1: case pOct2: case pSemi1: case pSemi2:
            {
                const int n = (int)std::lround(v);
                std::snprintf(b, sizeof(b), n > 0 ? "+%d" : "%d", n); out = b; return true;
            }
            case pFine1: case pFine2:
                std::snprintf(b, sizeof(b), v > 0.5f ? "+%.0f" : "%.0f", v); out = b; return true;
            case pUnison:
            {
                const int n = (int)std::lround(v);
                if (n <= 1) out = "Off"; else { std::snprintf(b, sizeof(b), "%d voices", n); out = b; }
                return true;
            }
            case pBendRange: std::snprintf(b, sizeof(b), "%d st", (int)std::lround(v)); out = b; return true;
            default: return false;
        }
    }

    // ----- audio ----------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(&ctx);
        if ((int)set_.playMode != playModeWas_) { allNotesOff(); playModeWas_ = (int)set_.playMode; }

        const int n = ctx.numFrames;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        float* rTmp = R != nullptr ? R : monoScratch_;
        if (R == nullptr && n > (int)(sizeof(monoScratch_) / sizeof(float))) return;
        for (int i = 0; i < n; ++i) { L[i] = 0.0f; rTmp[i] = 0.0f; }

        // transport for the synced LFOs
        bpm_ = (ctx.transport.valid && ctx.transport.tempoValid && ctx.transport.bpm > 1.0) ? ctx.transport.bpm : 120.0;
        locked_ = ctx.transport.valid && ctx.transport.playing && ctx.transport.ppqValid;
        blockPpq_ = ctx.transport.ppqPos;

        int pos = 0, ev = 0;
        while (pos < n)
        {
            if (ctrlLeft_ <= 0) { tick(pos); ctrlLeft_ = kCtrl; }
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= pos) handleMidi(ctx.midi[ev++]);
            int len = std::min(ctrlLeft_, n - pos);
            if (ev < ctx.numMidiEvents) len = std::min(len, ctx.midi[ev].sampleOffset - pos);
            if (len <= 0) len = 1;
            for (Voice& v : voices_) if (v.active) renderVoice(v, L + pos, rTmp + pos, len);
            pos += len; ctrlLeft_ -= len;
        }

        // effects
        runFx(0, set_.chorus, L, rTmp, n);
        runFx(1, set_.delay, L, rTmp, n);
        runFx(2, set_.reverb, L, rTmp, n);

        // output gain + soft limiter
        const float g0 = outGain_, dg = (targetOut_ - outGain_) / (float)n;
        float g = g0;
        for (int i = 0; i < n; ++i)
        {
            g += dg;
            L[i] = limit(L[i] * g);
            rTmp[i] = limit(rTmp[i] * g);
        }
        outGain_ = targetOut_;
        if (R == nullptr) for (int i = 0; i < n; ++i) L[i] = 0.5f * (L[i] + rTmp[i]);
    }

private:
    // ----- settings read once per block -------------------------------------------------------------
    struct Settings
    {
        int table1, table2, warpMode1, warpMode2, subType, unison, phaseReset, playMode, voices, filtMode;
        int l1shape, l2shape, l1mode, l2mode, chMode;
        int src[4], dst[4];
        float amt[4];
        float semi1, semi2;                  // oct*12 + semi + fine/100
        float morph1, morph2, warp1, warp2, level1, level2, sub, noise, noiseC, detune, width, drift;
        float glideMs, bendRange, colour, colM1, colM2, colF, envMorph;
        float l1rate, l2rate, l1beats, l2beats, l1fade;
        float cutoffSt, reso, drive, envAmt, keyTrack, velFilter, velLevel;
        // envelopes: amp per sample, filter/mod per control tick
        float aAtt, aDec, aSus, aRel;
        float fAtt, fDec, fSus, fRel, mAtt, mDec, mSus, mRel;
        float chorus, chRate, chDepth, delay, dlyBeats, dlyFdbk, dlyTone, reverb, revSize, revTone, revPre;
    };

    static float hzToSt(float hz) { return 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f); }

    void readSettings(const mpc::ProcessContext* /*ctx*/)
    {
        Settings& s = set_;
        const float sr = sr_;
        s.table1 = getInt(pTable1); s.table2 = getInt(pTable2);
        s.warpMode1 = getInt(pWarpMode1); s.warpMode2 = getInt(pWarpMode2);
        s.subType = getInt(pSubType); s.unison = mpc::dsp::clamp(getInt(pUnison), 1, kMaxUni);
        s.phaseReset = getInt(pPhase); s.playMode = getInt(pPlayMode); s.voices = mpc::dsp::clamp(getInt(pVoices), 1, kMaxVoices);
        s.filtMode = getInt(pFilterMode);
        s.l1shape = getInt(pL1Shape); s.l2shape = getInt(pL2Shape); s.l1mode = getInt(pL1Mode); s.l2mode = getInt(pL2Mode);
        s.chMode = getInt(pChMode);
        for (int k = 0; k < 4; ++k)
        {
            s.src[k] = getInt(pSrc1 + 3 * k); s.dst[k] = getInt(pDst1 + 3 * k);
            const float a = get(pAmt1 + 3 * k) * 0.01f;
            s.amt[k] = (s.dst[k] == kDstPitch || s.dst[k] == kDstPitch2) ? 12.0f * a * std::fabs(a) : a;
        }
        s.semi1 = 12.0f * (float)getInt(pOct1) + (float)getInt(pSemi1) + 0.01f * get(pFine1);
        s.semi2 = 12.0f * (float)getInt(pOct2) + (float)getInt(pSemi2) + 0.01f * get(pFine2);
        s.morph1 = get(pMorph1) * 0.01f; s.morph2 = get(pMorph2) * 0.01f;
        s.warp1 = get(pWarp1) * 0.01f; s.warp2 = get(pWarp2) * 0.01f;
        s.level1 = get(pLevel1) * 0.01f; s.level2 = get(pLevel2) * 0.01f;
        s.sub = get(pSub) * 0.01f; s.noise = get(pNoise) * 0.01f;
        const float ntHz = 400.0f * std::pow(50.0f, get(pNoiseTone) * 0.01f);       // 400 Hz .. 20 kHz
        s.noiseC = std::min(1.0f, 1.0f - std::exp(-6.2831853f * ntHz / sr));
        s.detune = get(pDetune) * 0.01f; s.width = get(pWidth) * 0.01f; s.drift = get(pDrift) * 0.01f;
        s.glideMs = get(pGlide); s.bendRange = (float)getInt(pBendRange);
        s.colour = get(pColour) * 0.01f;
        s.colM1 = get(pColMorph1) * 0.01f; s.colM2 = get(pColMorph2) * 0.01f; s.colF = get(pColFilter) * 0.01f;
        s.envMorph = get(pEnvMorph) * 0.01f;
        s.l1rate = get(pL1Rate); s.l2rate = get(pL2Rate);
        s.l1beats = (float)kLfoDivBeats[mpc::dsp::clamp(getInt(pL1Div), 0, kNumLfoDivs - 1)];
        s.l2beats = (float)kLfoDivBeats[mpc::dsp::clamp(getInt(pL2Div), 0, kNumLfoDivs - 1)];
        s.l1fade = get(pL1Fade) * 0.001f;
        s.cutoffSt = hzToSt(get(pCutoff)); s.reso = get(pReso) * 0.01f; s.drive = get(pDrive) * 0.01f;
        s.envAmt = get(pEnvAmt) * 0.01f; s.keyTrack = get(pKeyTrack) * 0.01f;
        s.velFilter = get(pVelFilter) * 0.01f; s.velLevel = get(pVelLevel) * 0.01f;

        const float ct = (float)kCtrl;
        s.aAtt = 1.0f / std::max(1.0f, get(pAttack) * 0.001f * sr);
        s.aDec = std::exp(-4.6f / std::max(1.0f, get(pDecay) * 0.001f * sr));
        s.aSus = get(pSustain) * 0.01f;
        s.aRel = std::exp(-6.9f / std::max(1.0f, get(pRelease) * 0.001f * sr));
        s.fAtt = std::min(1.0f, ct / std::max(1.0f, get(pFAttack) * 0.001f * sr));
        s.fDec = std::exp(-4.6f * ct / std::max(1.0f, get(pFDecay) * 0.001f * sr));
        s.fSus = get(pFSustain) * 0.01f;
        s.fRel = std::exp(-6.9f * ct / std::max(1.0f, get(pFRelease) * 0.001f * sr));
        s.mAtt = std::min(1.0f, ct / std::max(1.0f, get(pMAttack) * 0.001f * sr));
        s.mDec = std::exp(-4.6f * ct / std::max(1.0f, get(pMDecay) * 0.001f * sr));
        s.mSus = get(pMSustain) * 0.01f;
        s.mRel = std::exp(-6.9f * ct / std::max(1.0f, get(pMRelease) * 0.001f * sr));

        s.chorus = get(pChorus) * 0.01f; s.chRate = get(pChRate); s.chDepth = get(pChDepth) * 0.01f;
        s.delay = get(pDelay) * 0.01f;
        s.dlyBeats = (float)kDlyDivBeats[mpc::dsp::clamp(getInt(pDlyTime), 0, kNumDlyDivs - 1)];
        s.dlyFdbk = get(pDlyFdbk) * 0.01f; s.dlyTone = get(pDlyTone) * 0.01f;
        s.reverb = get(pReverb) * 0.01f; s.revSize = get(pRevSize) * 0.01f; s.revTone = get(pRevTone) * 0.01f;
        s.revPre = get(pRevPre);
        targetOut_ = mpc::dsp::dbToGain(get(pOutput));
    }

    // Values that move during play are smoothed once per control tick (~15 ms) so knob moves never click.
    struct Glob { float morph1, morph2, warp1, warp2, level1, level2, sub, noise, detune, width, colour, cutoffSt, reso, drive; };
    void snapGlobals()
    {
        const Settings& s = set_;
        g_ = {s.morph1, s.morph2, s.warp1, s.warp2, s.level1, s.level2, s.sub, s.noise, s.detune, s.width, s.colour, s.cutoffSt, s.reso, s.drive};
    }
    void smoothGlobals()
    {
        const Settings& s = set_;
        const float c = 1.0f - std::exp(-(float)kCtrl / (0.015f * sr_));
        auto sm = [c](float& v, float t) { v += c * (t - v); };
        sm(g_.morph1, s.morph1); sm(g_.morph2, s.morph2); sm(g_.warp1, s.warp1); sm(g_.warp2, s.warp2);
        sm(g_.level1, s.level1); sm(g_.level2, s.level2); sm(g_.sub, s.sub); sm(g_.noise, s.noise);
        sm(g_.detune, s.detune); sm(g_.width, s.width); sm(g_.colour, s.colour); sm(g_.cutoffSt, s.cutoffSt);
        sm(g_.reso, s.reso); sm(g_.drive, s.drive);
        const float cm = 1.0f - std::exp(-(float)kCtrl / (0.008f * sr_));
        wheel_ += cm * (wheelT_ - wheel_); at_ += cm * (atT_ - at_); bend_ += cm * (bendT_ - bend_);
    }

    // ----- control tick ------------------------------------------------------------------------------
    void tick(int blockPos)
    {
        smoothGlobals();
        const float dt = (float)kCtrl / sr_;
        const double ppq = blockPpq_ + (double)blockPos / (double)sr_ * bpm_ / 60.0;
        for (int l = 0; l < 2; ++l)
        {
            const int mode = l == 0 ? set_.l1mode : set_.l2mode;
            const int shape = l == 0 ? set_.l1shape : set_.l2shape;
            const float rate = l == 0 ? set_.l1rate : set_.l2rate;
            const float beats = l == 0 ? set_.l1beats : set_.l2beats;
            if (mode == kLfoSync && locked_)
            {
                const double c = ppq / (double)beats;
                const double fl = std::floor(c);
                gph_[l] = (float)(c - fl);
                gcycle_[l] = (int64_t)fl;
            }
            else
            {
                const float hz = (mode == kLfoSync || mode == kLfoSyncRtg) ? (float)(bpm_ / 60.0) / beats : rate;
                gph_[l] += hz * dt;
                if (gph_[l] >= 1.0f) { const int w = (int)gph_[l]; gph_[l] -= (float)w; gcycle_[l] += w; }
            }
            gr0_[l] = hashBi((uint32_t)(gcycle_[l] - 1) * 2654435761u + (uint32_t)l * 977u);
            gr1_[l] = hashBi((uint32_t)gcycle_[l] * 2654435761u + (uint32_t)l * 977u);
            const float raw = lfoShape(shape, gph_[l], gr0_[l], gr1_[l]);
            gout_[l] += lfoSmoothC() * (raw - gout_[l]);
        }
        for (Voice& v : voices_) if (v.active) voiceControl(v, kCtrl, true);
    }
    float lfoSmoothC() const { return 1.0f - std::exp(-(float)kCtrl / (0.002f * sr_)); }

    static void envStep(int& stage, float& e, float att, float dec, float sus, float rel)
    {
        switch (stage)
        {
            case kEnvAttack: e += att; if (e >= 1.0f) { e = 1.0f; stage = kEnvDecay; } break;
            case kEnvDecay:  e = sus + (e - sus) * dec; break;
            case kEnvRelease: e *= rel; if (e < 1.0e-5f) { e = 0.0f; stage = kEnvOff; } break;
            default: break;
        }
    }

    int mipLevel(float inc) const
    {
        int L = 0;
        while (L < kNumLevels - 1 && (float)WavetableBank::levelHarm(L) * inc > mipLimit_) ++L;
        return L;
    }

    // Recompute one voice's per-tick values. n = samples until the next tick (ramp length).
    void voiceControl(Voice& v, int n, bool advance)
    {
        const Settings& s = set_;
        const float dt = (float)n / sr_;
        if (advance)
        {
            envStep(v.fStage, v.fEnv, s.fAtt, s.fDec, s.fSus, s.fRel);
            envStep(v.mStage, v.mEnv, s.mAtt, s.mDec, s.mSus, s.mRel);
            v.age += dt;
            if (v.pitch != v.target)
            {
                if (std::fabs(v.target - v.pitch) <= v.glideStep || v.glideStep <= 0.0f) v.pitch = v.target;
                else v.pitch += v.target > v.pitch ? v.glideStep : -v.glideStep;
            }
        }
        // per-voice LFOs
        for (int l = 0; l < 2; ++l)
        {
            const int mode = l == 0 ? s.l1mode : s.l2mode;
            if (mode != kLfoRetrig && mode != kLfoSyncRtg) continue;
            if (advance)
            {
                const float hz = mode == kLfoSyncRtg ? (float)(bpm_ / 60.0) / (l == 0 ? s.l1beats : s.l2beats) : (l == 0 ? s.l1rate : s.l2rate);
                v.lph[l] += hz * dt;
                if (v.lph[l] >= 1.0f) { v.lph[l] -= (float)(int)v.lph[l]; v.lr0[l] = v.lr1[l]; v.lr1[l] = v.rng.bi(); }
            }
            const float raw = lfoShape(l == 0 ? s.l1shape : s.l2shape, v.lph[l], v.lr0[l], v.lr1[l]);
            if (advance) v.lout[l] += lfoSmoothC() * (raw - v.lout[l]); else v.lout[l] = raw;
        }
        auto lfoVal = [&](int l) {
            const int mode = l == 0 ? s.l1mode : s.l2mode;
            return (mode == kLfoRetrig || mode == kLfoSyncRtg) ? v.lout[l] : gout_[l];
        };
        float fade = 1.0f;
        if (s.l1fade > 0.0005f) fade = std::min(1.0f, v.age / s.l1fade);

        float src[kNumSrc];
        src[kSrcOff] = 0.0f;
        src[kSrcLfo1] = lfoVal(0) * fade;
        src[kSrcLfo2] = lfoVal(1);
        src[kSrcModEnv] = v.mEnv;
        src[kSrcFiltEnv] = v.fEnv;
        src[kSrcAmpEnv] = v.amp;
        src[kSrcVel] = v.vel;
        src[kSrcWheel] = wheel_;
        src[kSrcAT] = std::max(at_, v.at);
        src[kSrcKey] = mpc::dsp::clamp(((float)v.note - 60.0f) / 48.0f, -1.5f, 1.5f);
        src[kSrcLfo1MW] = src[kSrcLfo1] * wheel_;
        src[kSrcRandom] = v.rnd;
        src[kSrcColour] = g_.colour;
        float d[kNumDst] = {};
        for (int k = 0; k < 4; ++k) if (s.src[k] != kSrcOff) d[mpc::dsp::clamp(s.dst[k], 0, kNumDst - 1)] += s.amt[k] * src[mpc::dsp::clamp(s.src[k], 0, kNumSrc - 1)];

        // drift
        float dr[4] = {0, 0, 0, 0};
        if (s.drift > 0.0005f) for (int k = 0; k < 4; ++k) dr[k] = s.drift * v.drift[k].step(dt, v.rng);
        else if (advance) for (int k = 0; k < 4; ++k) v.drift[k].step(dt, v.rng);

        // pitch -> increments
        const float base = v.pitch + bend_ * s.bendRange + d[kDstPitch];
        const float o1 = base + s.semi1 + 0.12f * dr[0];
        const float o2 = base + s.semi2 + d[kDstPitch2] + 0.12f * dr[1];
        const float det = mpc::dsp::clamp(g_.detune + d[kDstDetune], 0.0f, 1.0f);
        const float spread = 0.6f * det * std::sqrt(det);
        const int U = s.unison;
        const float k440 = 440.0f / sr_;
        float max1 = 0.0f, max2 = 0.0f;
        for (int u = 0; u < U; ++u)
        {
            const float off = kUniOff[U][u] * spread;
            v.inc1[u] = std::min(0.49f, k440 * fastExp2((o1 + off - 69.0f) * (1.0f / 12.0f)));
            v.inc2[u] = std::min(0.49f, k440 * fastExp2((o2 + 0.9f * off - 69.0f) * (1.0f / 12.0f)));
            max1 = std::max(max1, v.inc1[u]); max2 = std::max(max2, v.inc2[u]);
        }
        v.uni = U;
        const float subSt = s.subType == 1 ? 24.0f : 12.0f;
        v.incSub = std::min(0.49f, k440 * fastExp2((base + s.semi1 - subSt - 69.0f) * (1.0f / 12.0f)));

        // warp, mip level
        v.w1.set(s.warpMode1, g_.warp1 + d[kDstWarp1]);
        v.w2.set(s.warpMode2, g_.warp2 + d[kDstWarp2]);
        v.lvl1 = mipLevel(max1 * (v.w1.active ? v.w1.factor : 1.0f));
        v.lvl2 = mipLevel(max2 * (v.w2.active ? v.w2.factor : 1.0f));
        v.lvlSub = mipLevel(v.incSub);

        // morph targets (in frames)
        const float col = g_.colour;
        const float mb = d[kDstMorphBoth] + s.envMorph * v.mEnv + 0.05f * dr[2];
        const float t1 = mpc::dsp::clamp(g_.morph1 + col * s.colM1 + mb + d[kDstMorph1], 0.0f, 1.0f) * (float)(kNumFrames - 1);
        const float t2 = mpc::dsp::clamp(g_.morph2 + col * s.colM2 + mb + d[kDstMorph2], 0.0f, 1.0f) * (float)(kNumFrames - 1);
        if (!v.started || CUTTLEFISH_NORAMP) { v.m1 = t1; v.m2 = t2; v.dm1 = v.dm2 = 0.0f; }
        else { v.dm1 = (t1 - v.m1) / (float)n; v.dm2 = (t2 - v.m2) / (float)n; }

        // levels and stereo
        const float mix = mpc::dsp::clamp(d[kDstOscMix], -1.0f, 1.0f);
        const float l1 = g_.level1 * (1.0f - std::max(0.0f, mix));
        const float l2 = g_.level2 * (1.0f + std::min(0.0f, mix));
        const float norm = 1.0f / std::sqrt((float)U);
        const float width = U > 1 ? g_.width : 0.0f;
        const bool stereo = width > 0.001f;
        if (stereo && !v.stereo) for (int k = 0; k < 4; ++k) v.sR[k] = v.sL[k];
        v.stereo = stereo;
        for (int u = 0; u < U; ++u)
        {
            float pl = 0.70710678f, pr = 0.70710678f;
            if (stereo) { const float p = kUniOff[U][u] * width; pl = std::sqrt(0.5f * (1.0f - p)); pr = std::sqrt(0.5f * (1.0f + p)); }
            v.c1L[u] = l1 * norm * pl; v.c1R[u] = l1 * norm * pr;
            v.c2L[u] = l2 * norm * pr; v.c2R[u] = l2 * norm * pl;     // osc 2 mirrored
        }
        static const float subGain[4] = {1.0f, 1.0f, 0.9f, 0.6f};
        v.subG = g_.sub * subGain[mpc::dsp::clamp(s.subType, 0, 3)] * 0.9f;
        v.noiseG = mpc::dsp::clamp(g_.noise + d[kDstNoise], 0.0f, 1.0f) * 0.6f;

        // filter
        float st = g_.cutoffSt + s.envAmt * v.fEnv * 60.0f + s.keyTrack * ((float)v.note - 60.0f)
                 + s.velFilter * (v.vel - 0.6f) * 48.0f + col * s.colF * 60.0f + d[kDstCutoff] * 60.0f + 1.5f * dr[3];
        float fc = 440.0f * fastExp2((mpc::dsp::clamp(st, 0.0f, 140.0f) - 69.0f) * (1.0f / 12.0f));
        fc = mpc::dsp::clamp(fc, 16.0f, 0.45f * sr_);
        const float g = fastTan(3.14159265f * fc / sr_);
        const float r = mpc::dsp::clamp(g_.reso + d[kDstReso], 0.0f, 1.0f);
        float kA, kB = 2.0f;
        if (s.filtMode == kLP24) { kA = 1.85f - 1.0f * r; kB = 2.0f - 1.94f * r; }
        else kA = 2.0f - 1.96f * r;
        v.kA = kA;
        v.a1 = 1.0f / (1.0f + g * (g + kA)); v.a2 = g * v.a1; v.a3 = g * v.a2;
        v.b1 = 1.0f / (1.0f + g * (g + kB)); v.b2 = g * v.b1; v.b3 = g * v.b2;
        v.bpGain = std::sqrt(kA);

        // amplitude and pan (ramped across the tick)
        const float vg = 1.0f - s.velLevel * (1.0f - v.vel * v.vel);
        const float lev = std::max(0.0f, 1.0f + d[kDstLevel]) * vg * kVoiceGain;
        const float pan = mpc::dsp::clamp(d[kDstPan], -1.0f, 1.0f);
        const float tl = lev * (pan > 0.0f ? 1.0f - pan : 1.0f), tr = lev * (pan < 0.0f ? 1.0f + pan : 1.0f);
        if (!v.started) { v.gL = tl; v.gR = tr; v.dgL = v.dgR = 0.0f; }
        else { v.dgL = (tl - v.gL) / (float)n; v.dgR = (tr - v.gR) / (float)n; }
        v.started = true;
    }

    // ----- per-sample voice rendering --------------------------------------------------------------
    static inline float readWT(const float* A, const float* B, float t, float q, float fN, int mask)
    {
        const float x = q * fN; int i = (int)x; const float fr = x - (float)i; i &= mask;
        const float a = A[i] + fr * (A[i + 1] - A[i]);
        const float b = B[i] + fr * (B[i + 1] - B[i]);
        return a + t * (b - a);
    }
    static inline void svf(float x, float* s, float a1, float a2, float a3, float& lp, float& bp)
    {
        const float v3 = x - s[1];
        const float v1 = a1 * s[0] + a2 * v3;
        const float v2 = s[1] + a2 * s[0] + a3 * v3;
        s[0] = 2.0f * v1 - s[0]; s[1] = 2.0f * v2 - s[1];
        lp = v2; bp = v1;
    }
    inline float filt(int mode, float x, float* s, const Voice& v) const
    {
        float lp, bp;
        svf(x, s, v.a1, v.a2, v.a3, lp, bp);
        switch (mode)
        {
            case kLP12: return lp;
            case kLP24: { float lp2, bp2; svf(lp, s + 2, v.b1, v.b2, v.b3, lp2, bp2); return lp2; }
            case kBP:   return bp * v.bpGain;
            case kHP:   return x - v.kA * bp - lp;
            default:    return x - v.kA * bp;    // notch
        }
    }

    void renderVoice(Voice& v, float* L, float* R, int n)
    {
        const Settings& s = set_;
        const WavetableBank& bk = *bank_;
        const int stride = bk.stride();
        const float* tab1 = bk.tableBase(mpc::dsp::clamp(s.table1, 0, kNumTables - 1)) + bk.levelOffset(v.lvl1);
        const float* tab2 = bk.tableBase(mpc::dsp::clamp(s.table2, 0, kNumTables - 1)) + bk.levelOffset(v.lvl2);
        const int N1 = WavetableBank::levelSize(v.lvl1), N2 = WavetableBank::levelSize(v.lvl2);
        const float fN1 = (float)N1, fN2 = (float)N2;
        const int mask1 = N1 - 1, mask2 = N2 - 1;
        static const int subMap[4] = {kSubSine, kSubSine, kSubTri, kSubSquare};
        const float* subTab = bk.sub(subMap[mpc::dsp::clamp(s.subType, 0, 3)], v.lvlSub);
        const int Ns = WavetableBank::levelSize(v.lvlSub);
        const float fNs = (float)Ns; const int maskS = Ns - 1;
        const bool run1 = v.c1L[0] > 1.0e-6f || v.c1R[0] > 1.0e-6f;
        const bool run2 = v.c2L[0] > 1.0e-6f || v.c2R[0] > 1.0e-6f || (run1 && v.w1.mode == kWFM && v.w1.active);
        const bool fm2 = v.w2.mode == kWFM && v.w2.active;
        const bool doSub = v.subG > 1.0e-5f, doNoise = v.noiseG > 1.0e-5f;
        const float drive = g_.drive;
        const bool doDrive = drive > 0.005f;
        const float dg = 1.0f + 9.0f * drive * drive, dMake = 1.0f / mpc::dsp::fastTanh(dg) * (0.6f + 0.4f / dg);
        const int fmode = s.filtMode;
        const int U = v.uni;
        const float lastF = (float)(kNumFrames - 1);
        const float aAtt = s.aAtt, aDec = s.aDec, aSus = s.aSus, aRel = s.aRel;

        for (int i = 0; i < n; ++i)
        {
            // morph position -> frame pair
            v.m1 += v.dm1; v.m2 += v.dm2;
            float mm1 = mpc::dsp::clamp(v.m1, 0.0f, lastF), mm2 = mpc::dsp::clamp(v.m2, 0.0f, lastF);
            int f1 = (int)mm1; if (f1 > kNumFrames - 2) f1 = kNumFrames - 2;
            int f2 = (int)mm2; if (f2 > kNumFrames - 2) f2 = kNumFrames - 2;
            const float t1 = mm1 - (float)f1, t2 = mm2 - (float)f2;
            const float* A1 = tab1 + (size_t)f1 * (size_t)stride; const float* B1 = A1 + stride;
            const float* A2 = tab2 + (size_t)f2 * (size_t)stride; const float* B2 = A2 + stride;

            float sumL = 0.0f, sumR = 0.0f;
            for (int u = 0; u < U; ++u)
            {
                float p1 = v.ph1[u] + v.inc1[u]; if (p1 >= 1.0f) p1 -= 1.0f; v.ph1[u] = p1;
                float p2 = v.ph2[u] + v.inc2[u]; if (p2 >= 1.0f) p2 -= 1.0f; v.ph2[u] = p2;
                float s2 = 0.0f;
                if (run2)
                {
                    float gw = 1.0f;
                    const float q2 = v.w2.active ? v.w2.apply(p2, fm2 ? mpc::dsp::fastSin01(p1) : 0.0f, gw) : p2;
                    s2 = readWT(A2, B2, t2, q2, fN2, mask2) * gw;
                    sumL += s2 * v.c2L[u]; sumR += s2 * v.c2R[u];
                }
                if (run1)
                {
                    float gw = 1.0f;
                    const float q1 = v.w1.active ? v.w1.apply(p1, s2, gw) : p1;
                    const float s1 = readWT(A1, B1, t1, q1, fN1, mask1) * gw;
                    sumL += s1 * v.c1L[u]; sumR += s1 * v.c1R[u];
                }
            }
            float c = 0.0f;
            if (doSub)
            {
                float ps = v.phSub + v.incSub; if (ps >= 1.0f) ps -= 1.0f; v.phSub = ps;
                const float x = ps * fNs; int k = (int)x; const float fr = x - (float)k; k &= maskS;
                c += v.subG * (subTab[k] + fr * (subTab[k + 1] - subTab[k]));
            }
            if (doNoise)
            {
                v.nz += s.noiseC * (v.rng.bi() - v.nz);
                c += v.noiseG * v.nz;
            }
            float yL, yR;
            if (v.stereo)
            {
                float xl = sumL + c, xr = sumR + c;
                if (doDrive) { xl = mpc::dsp::fastTanh(xl * dg) * dMake; xr = mpc::dsp::fastTanh(xr * dg) * dMake; }
                yL = filt(fmode, xl, v.sL, v); yR = filt(fmode, xr, v.sR, v);
            }
            else
            {
                float x = sumL + c;
                if (doDrive) x = mpc::dsp::fastTanh(x * dg) * dMake;
                yL = yR = filt(fmode, x, v.sL, v);
            }
            // amp envelope
            switch (v.ampStage)
            {
                case kEnvAttack: v.amp += aAtt; if (v.amp >= 1.0f) { v.amp = 1.0f; v.ampStage = kEnvDecay; } break;
                case kEnvDecay:  v.amp = aSus + (v.amp - aSus) * aDec; break;
                case kEnvRelease: v.amp *= aRel; break;
                default: break;
            }
            float a = v.amp;
            if (v.fading) { v.fade -= v.fadeStep; if (v.fade < 0.0f) v.fade = 0.0f; a *= v.fade * v.fade * (3.0f - 2.0f * v.fade); }
            v.gL += v.dgL; v.gR += v.dgR;
            L[i] += yL * a * v.gL;
            R[i] += yR * a * v.gR;
        }
        // end of voice
        if ((v.ampStage == kEnvRelease && v.amp < 1.0e-4f) || (v.fading && v.fade <= 0.0f) || v.ampStage == kEnvOff
            || (v.ampStage == kEnvDecay && aSus <= 0.0f && v.amp < 1.0e-4f))
        {
            v.active = false; v.ampStage = kEnvOff; v.fading = false;
            if (monoVoice_ >= 0 && &voices_[monoVoice_] == &v) monoVoice_ = -1;
        }
    }

    // ----- voices ------------------------------------------------------------------------------------
    int liveCount() const
    {
        int c = 0;
        for (const Voice& v : voices_) if (v.active && !v.fading) ++c;
        return c;
    }
    void fadeOut(Voice& v) { if (!v.fading) { v.fading = true; v.fade = 1.0f; v.fadeStep = 1.0f / (0.005f * sr_); } v.held = false; v.sustained = false; }

    int allocSlot()
    {
        for (int i = 0; i < kNumSlots; ++i) if (!voices_[i].active) return i;
        // all busy: reuse the quietest fading voice (nearly silent already)
        int best = -1; float bf = 2.0f;
        for (int i = 0; i < kNumSlots; ++i) if (voices_[i].fading && voices_[i].fade < bf) { bf = voices_[i].fade; best = i; }
        if (best >= 0) return best;
        // nothing fading (cannot happen with kNumSlots > kMaxVoices): oldest voice
        uint32_t bo = 0xFFFFFFFFu;
        for (int i = 0; i < kNumSlots; ++i) if (voices_[i].order < bo) { bo = voices_[i].order; best = i; }
        return best < 0 ? 0 : best;
    }
    void stealIfNeeded(int maxLive)
    {
        while (liveCount() >= maxLive)
        {
            Voice* victim = nullptr;
            for (Voice& v : voices_)       // oldest released voice first
                if (v.active && !v.fading && !v.held && !v.sustained && (!victim || v.order < victim->order)) victim = &v;
            if (!victim)
                for (Voice& v : voices_) if (v.active && !v.fading && (!victim || v.order < victim->order)) victim = &v;
            if (!victim) break;
            fadeOut(*victim);
        }
    }

    void startVoice(int slot, int note, float vel, float fromPitch, bool glide)
    {
        const Settings& s = set_;
        Voice& v = voices_[slot];
        v.active = true; v.fading = false; v.fade = 1.0f; v.held = true; v.sustained = false;
        v.note = note; v.vel = vel; v.order = ++orderCounter_;
        v.target = (float)note;
        v.pitch = glide ? fromPitch : (float)note;
        setGlide(v);
        v.rng.s = hashU(orderCounter_ * 747796405u + (uint32_t)note) | 1u;
        v.rnd = v.rng.bi(); v.at = 0.0f; v.age = 0.0f;
        v.amp = 0.0f; v.ampStage = kEnvAttack;
        v.fEnv = 0.0f; v.fStage = kEnvAttack;
        v.mEnv = 0.0f; v.mStage = kEnvAttack;
        for (int u = 0; u < kMaxUni; ++u)
        {
            if (s.phaseReset) { v.ph1[u] = 0.37f * (float)u; v.ph2[u] = 0.37f * (float)u + 0.11f * (u > 0); }
            else { v.ph1[u] = v.rng.uni(); v.ph2[u] = v.rng.uni(); }
            v.ph1[u] -= (float)(int)v.ph1[u]; v.ph2[u] -= (float)(int)v.ph2[u];
        }
        v.phSub = s.phaseReset ? 0.0f : v.rng.uni();
        for (int l = 0; l < 2; ++l) { v.lph[l] = 0.0f; v.lr0[l] = v.rng.bi(); v.lr1[l] = v.rng.bi(); v.lout[l] = 0.0f; }
        for (int k = 0; k < 4; ++k) v.drift[k].init(v.rng);
        for (int k = 0; k < 4; ++k) { v.sL[k] = 0.0f; v.sR[k] = 0.0f; }
        v.nz = 0.0f; v.stereo = false;
        v.started = false;
        voiceControl(v, std::max(1, ctrlLeft_), false);
    }
    void setGlide(Voice& v)
    {
        const float ms = set_.glideMs;
        if (ms < 0.5f || v.pitch == v.target) { v.glideStep = 0.0f; v.pitch = ms < 0.5f ? v.target : v.pitch; return; }
        v.glideStep = std::fabs(v.target - v.pitch) * (float)kCtrl / (ms * 0.001f * sr_);
    }
    void retrigger(Voice& v)
    {
        v.ampStage = kEnvAttack; v.fStage = kEnvAttack; v.mStage = kEnvAttack;
        v.age = 0.0f;
        for (int l = 0; l < 2; ++l) v.lph[l] = 0.0f;
    }
    void releaseVoice(Voice& v)
    {
        v.held = false; v.sustained = false;
        if (v.ampStage != kEnvOff) v.ampStage = kEnvRelease;
        if (v.fStage != kEnvOff) v.fStage = kEnvRelease;
        if (v.mStage != kEnvOff) v.mStage = kEnvRelease;
    }

    void noteOn(int note, int velocity)
    {
        const Settings& s = set_;
        const float vel = (float)velocity / 127.0f;
        const bool glideOn = s.glideMs >= 0.5f;
        if (s.playMode == kPoly)
        {
            for (Voice& v : voices_) if (v.active && !v.fading && v.note == note) fadeOut(v);   // same key again
            stealIfNeeded(s.voices);
            const int slot = allocSlot();
            startVoice(slot, note, vel, lastPitch_, glideOn && hasLast_);
        }
        else
        {
            // note stack
            int w = 0;
            for (int i = 0; i < numStack_; ++i) if (stack_[i] != note) stack_[w++] = stack_[i];
            numStack_ = w;
            if (numStack_ < kMaxStack) stack_[numStack_++] = note;
            Voice* v = (monoVoice_ >= 0 && voices_[monoVoice_].active && !voices_[monoVoice_].fading) ? &voices_[monoVoice_] : nullptr;
            if (v == nullptr)
            {
                for (Voice& o : voices_) if (o.active && !o.fading) fadeOut(o);
                const int slot = allocSlot();
                startVoice(slot, note, vel, lastPitch_, glideOn && hasLast_ && s.playMode == kMono);
                monoVoice_ = slot;
            }
            else
            {
                const bool overlapping = v->held;
                v->note = note; v->target = (float)note; v->vel = vel;
                v->held = true; v->sustained = false;
                if (glideOn && (s.playMode == kMono || overlapping)) setGlide(*v);
                else { v->pitch = v->target; v->glideStep = 0.0f; }
                if (s.playMode == kMono || !overlapping) retrigger(*v);
            }
        }
        lastPitch_ = (float)note; hasLast_ = true;
    }

    void noteOff(int note)
    {
        const Settings& s = set_;
        if (s.playMode == kPoly)
        {
            for (Voice& v : voices_)
                if (v.active && !v.fading && v.held && v.note == note)
                {
                    if (pedal_) { v.held = false; v.sustained = true; }
                    else releaseVoice(v);
                }
            return;
        }
        int w = 0;
        for (int i = 0; i < numStack_; ++i) if (stack_[i] != note) stack_[w++] = stack_[i];
        numStack_ = w;
        Voice* v = (monoVoice_ >= 0 && voices_[monoVoice_].active && !voices_[monoVoice_].fading) ? &voices_[monoVoice_] : nullptr;
        if (v == nullptr || v->note != note) return;
        if (numStack_ > 0)
        {
            const int top = stack_[numStack_ - 1];
            v->note = top; v->target = (float)top;
            if (s.glideMs >= 0.5f) setGlide(*v); else { v->pitch = v->target; v->glideStep = 0.0f; }
            lastPitch_ = (float)top;
        }
        else if (pedal_) { v->held = false; v->sustained = true; }
        else releaseVoice(*v);
    }

    void allNotesOff()
    {
        for (Voice& v : voices_) if (v.active && !v.fading) releaseVoice(v);
        numStack_ = 0; pedal_ = false;
    }

    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn()) noteOn(e.note(), e.velocity());
        else if (e.isNoteOff()) noteOff(e.note());
        else if (e.isAllNotesOff()) allNotesOff();
        else if (e.isController())
        {
            if (e.controller() == 1) wheelT_ = (float)e.controllerValue() / 127.0f;
            else if (e.controller() == 64)
            {
                const bool down = e.controllerValue() >= 64;
                if (!down && pedal_)
                    for (Voice& v : voices_) if (v.active && v.sustained && !v.held) releaseVoice(v);
                pedal_ = down;
            }
        }
        else if (e.isPitchBend()) bendT_ = (float)e.pitchBend() / 8192.0f;
        else if (e.isAftertouch()) atT_ = (float)e.data[1] / 127.0f;
        else if (e.status() == 0xA0)
            for (Voice& v : voices_) if (v.active && v.note == e.data[1]) v.at = (float)e.data[2] / 127.0f;
    }

    // ----- effects -----------------------------------------------------------------------------------
    void runFx(int which, float mix, float* L, float* R, int n)
    {
        if (mix <= 0.001f)
        {
            if (fxOn_[which]) { fxIdle_[which] += n; if (fxIdle_[which] > (int)(sr_ * 0.25f)) fxOn_[which] = false; }
            if (!fxOn_[which]) return;
            mix = 0.0f;
        }
        else
        {
            if (!fxOn_[which]) { if (which == 0) chorus_.reset(); else if (which == 1) delay_.reset(); else reverb_.reset(); }
            fxOn_[which] = true; fxIdle_[which] = 0;
        }
        const Settings& s = set_;
        if (which == 0) chorus_.process(L, R, n, mix, s.chRate, s.chDepth, s.chMode);
        else if (which == 1) delay_.process(L, R, n, mix, (float)(s.dlyBeats * 60.0 / std::max(40.0, bpm_)), s.dlyFdbk, s.dlyTone);
        else reverb_.process(L, R, n, mix, s.revSize, s.revTone, s.revPre);
    }

    static inline float limit(float x)
    {
        const float a = std::fabs(x);
        if (a <= 0.7f) return x;
        const float y = 0.7f + 0.3f * mpc::dsp::fastTanh((a - 0.7f) / 0.3f);
        return x < 0.0f ? -y : y;
    }

    // ----- programs ------------------------------------------------------------------------------
    void addPrograms();

    // ----- state -------------------------------------------------------------------------------------
    const WavetableBank* bank_ = nullptr;
    float sr_ = 44100.0f, mipLimit_ = 0.55f;
    Settings set_ {};
    Glob g_ {};
    Voice voices_[kNumSlots];
    int stack_[kMaxStack] = {}, numStack_ = 0, monoVoice_ = -1, playModeWas_ = 0;
    bool pedal_ = false, hasLast_ = false;
    float lastPitch_ = 60.0f;
    float wheel_ = 0, wheelT_ = 0, at_ = 0, atT_ = 0, bend_ = 0, bendT_ = 0;
    int ctrlLeft_ = 0; uint32_t orderCounter_ = 0;
    double bpm_ = 120.0, blockPpq_ = 0.0; bool locked_ = false;
    float gph_[2] = {}, gr0_[2] = {}, gr1_[2] = {}, gout_[2] = {}; int64_t gcycle_[2] = {};
    Rng rng_;
    Chorus chorus_; PingPongDelay delay_; SmallReverb reverb_;
    bool fxOn_[3] = {}; int fxIdle_[3] = {};
    float outGain_ = 1.0f, targetOut_ = 1.0f;
    float monoScratch_[4096];
};

#include "Programs.h"

} // namespace cuttlefish

MPC_PLUGIN_ENTRY(cuttlefish::Cuttlefish)
