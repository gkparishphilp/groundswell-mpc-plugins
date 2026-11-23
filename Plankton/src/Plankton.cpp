// Plankton - a granular texture engine for live input, for the MPC, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// Plankton keeps the last few seconds of the track in a stereo ring buffer and plays it back as a
// cloud of tiny overlapping grains: each grain is a short, windowed slice read from somewhere back in
// the buffer, at its own pitch, direction and place in the stereo field. Freeze stops the buffer
// recording, so the cloud plays from that captured moment for as long as you like; Drift keeps a frozen
// cloud wandering so it never sits still.
//
// SIGNAL FLOW
//
//   in L/R -+------------------------------------------------------------------------ dry ---+
//           |                                                                                |
//           +-> (+ Feedback x the cloud, DC-blocked, level-guarded) -> soft ceiling ->       |
//               x release fade-in -> RING BUFFER (stereo, >= 6 s; Freeze / Catch stop        |
//               writing)                                                                     |
//                   |                                                                        |
//                   +-> GRAIN SCHEDULER: Density grains per second (with +-20 % timing       |
//                   |   jitter), or one grain per Division locked to the song when Sync is on |
//                   |   (free-running at the song tempo while the transport is stopped)      |
//                   |                                                                        |
//                   +-> up to 48 GRAINS, each: start = Position + Spray x random + Drift     |
//                       back from the write head; pitch = Pitch + Scatter x random (snapped  |
//                       to octaves / fifths / a pentatonic scale) + Drift; reversed with     |
//                       probability Reverse; window from Shape (Hann -> flat-top -> struck); |
//                       4-point Hermite read; panned by Spread                               |
//                           |                                                                |
//                           +-> x density normalisation -> SMEAR (6 allpasses per side) ->   |
//                               TONE (low-pass left of centre, high-pass right) -- wet ------+-> Mix -> Output
//                                                                                                 -> soft ceiling
//
// CLICK-FREE FREEZE: the buffer's write head is a seam (newest sample next to the oldest). While live,
// grains are kept a few ms behind the head and well clear of the oldest material. On Freeze, the
// newest and oldest 8 ms either side of the seam are faded to silence in place, so a grain that runs
// into the seam fades through a short dip instead of a jump. On release, writing resumes with an 8 ms
// fade-in from the faded seam, and frozen grains never read the oldest 0.6 s (the part the writer
// reaches first), so nothing a grain is playing gets overwritten underneath it.
//
// CPU: grains are rendered grain by grain over the block (state stays in registers); per grain-sample
// one window-table lookup, two 4-point Hermite reads (stereo, interleaved buffer) and a 2x2 pan
// matrix. pow/exp/cos run once per grain at its birth, coefficients once per block. Window shapes are
// tables, blended into one table only when Shape moves. At most 48 grains sound at once; extra
// triggers are skipped (nothing is cut off), so extreme settings thin out rather than overload.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/tempo.h"
#include "dsp/noise.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Test hooks (tests/planktontest.cpp defines these to log what the scheduler does).
#ifndef PLANKTON_GRAIN_HOOK
#define PLANKTON_GRAIN_HOOK(offset, lag, rate, reversed, len) ((void)0)
#endif

using namespace mpc::dsp;

namespace plankton {

constexpr int   kPool = 48;               // grain slots: the most grains that can sound at once
constexpr int   kWinLen = 1024;           // window table length (plus one guard point)
constexpr float kBufSec = 12.0f;          // at least this much history in the ring buffer (Tail reaches far back)
constexpr float kMaxReadSec = 2.0f;       // a grain never reads more than this much material (pitch x size)
constexpr float kSeamSec = 0.008f;        // fade either side of the write head on Freeze, and fade-in on release
constexpr float kFrozenGuardSec = 0.6f;   // while frozen, the oldest 0.6 s is never read (see the header)
constexpr float kDriftPosMs = 750.0f;     // Drift 100 %: position wanders up to 750 ms further back
constexpr float kDriftPitchSt = 1.0f;     // Drift 100 %: pitch wanders up to +-1 semitone
constexpr float kFreeJitter = 0.2f;       // free-running grains: +-20 % timing jitter
constexpr int   kMaxTriggersPerBlock = 96;
constexpr float kTailRelDb = -20.0f;      // Tail: the input counts as "playing" while within 20 dB of its recent peak...
constexpr float kTailFloor = 0.0018f;     // ...and above -55 dBFS
constexpr float kTailFollowSec = 0.05f;   // level follower release
constexpr float kTailPeakSec = 1.5f;      // how fast the recent peak forgets
constexpr float kTailGapSec = 0.05f;      // a quiet gap longer than this starts a new "thing you played"
constexpr float kTailWindowSec = 1.0f;    // tail grains come from (at most) the last second of it
constexpr int   kEnvShift = 6;            // the loudness map keeps one peak per 64 frames of input
constexpr int   kTailTries = 4;           // a Tail grain takes the loudest of this many random spots
constexpr float kFbMax = 0.92f;           // feedback at 100 %
constexpr float kFbCeiling = 0.45f;       // cloud peak above which the feedback guard eases off

// Pitch snap for Scatter. Internal names carry the kSn prefix so they never collide with parameters.
enum SnapChoice { kSnFree = 0, kSnOctave, kSnFifth, kSnScale };
static const float kOctaveSet[] = {-24, -12, 0, 12, 24};
static const float kFifthSet[]  = {-24, -19, -12, -7, 0, 7, 12, 19, 24};
static const float kScaleSet[]  = {-24, -22, -19, -17, -15, -12, -10, -7, -5, -3, 0,
                                   3, 5, 7, 10, 12, 15, 17, 19, 22, 24};   // minor pentatonic, +-2 oct

// Drift periods when Sync is on (beats), slowest first, and their names.
static const float kDriftSyncBeats[] = {32.0f, 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f};
static const char* const kDriftSyncNames[] = {"8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16"};
constexpr int kNumDriftSync = 8;

// Smear: allpass lengths in ms per side (mutually prime-ish), so a cloud can melt into a wash.
static const float kSmearMsL[6] = {3.1f, 7.3f, 13.7f, 23.3f, 41.9f, 67.1f};
static const float kSmearMsR[6] = {3.7f, 8.1f, 14.9f, 25.7f, 44.3f, 71.9f};

inline float smoothstep01(float x)
{
    const float t = mpc::clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Bend above 0.8 smoothly toward 1.0: used on the output and on what is written into the buffer.
inline float ceiling(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.8f) return x;
    const float y = 0.8f + 0.2f * fastTanh((a - 0.8f) * 5.0f);
    return x < 0.0f ? -y : y;
}

// Integer hash -> [-1, 1]: Drift's random targets, so a synced drift repeats with the song.
inline float hashBipolar(int64_t seg, uint32_t salt)
{
    uint32_t x = (uint32_t)(seg * 0x9E3779B1LL) ^ (salt * 0x85EBCA77u);
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// 4-point, 3rd-order Hermite interpolation.
inline float hermite(float xm1, float x0, float x1, float x2, float f)
{
    const float c = (x1 - xm1) * 0.5f;
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + (x2 - x0) * 0.5f;
    const float b = w + a;
    return ((a * f - b) * f + c) * f + x0;
}

struct Allpass
{
    std::vector<float> buf;
    int len = 1, pos = 0;
    void allocate(int n) { len = std::max(1, n); buf.assign((size_t)len, 0.0f); pos = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
    float process(float x, float g)
    {
        const float d = buf[(size_t)pos];
        const float v = x - g * d;
        buf[(size_t)pos] = v;
        if (++pos >= len) pos = 0;
        return d + g * v;
    }
};

struct Grain
{
    bool   active = false;
    double pos = 0.0;          // read position in the buffer (frames), kept positive, wrapped by mask
    float  inc = 1.0f;         // frames per sample: +rate forward, -rate reversed
    float  wph = 0.0f;         // window phase, 0..kWinLen
    float  winc = 1.0f;        // window phase per sample
    float  mLL = 1, mRL = 0, mLR = 0, mRR = 1;   // pan matrix: outL = mLL*l + mRL*r, outR = mLR*l + mRR*r
    int    life = -1;          // samples until a forced stop (-1: none). Set on Freeze for grains that
                               // would run through the seam; they stop where the faded seam is silent.
};

} // namespace plankton

using namespace plankton;

class Plankton : public mpc::Plugin
{
public:
    // Parameter order and ids are permanent once users have projects: append new ones at the end.
    enum Params
    {
        kOutput = 0,
        kMix, kFreeze, kCatch,
        kSize, kDensity, kPosition, kSpray,
        kPitch, kScatter, kSnap, kReverse,
        kShape, kSpread,
        kDrift, kDriftRate,
        kFeedback, kSmear, kTone,
        kSync, kDivision,
        // appended after the first install: keep it last
        kLinger,
        kTail,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Plankton";
        i.vendor = "Groundswell";
        i.uniqueId = MPC_FOURCC('G', '3', 'D', 'd');   // == "uid" in plugin.json. Never change.
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.tailSamples = 0;   // unknown: Freeze is endless
        return i;
    }

    Plankton() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        // Parameter 0: the host nudges it on every load, which is harmless for a level.
        addParam(floatParam ("output",   "Output", -24.0f, 6.0f, 0.0f, "dB", 1));
        // Dry / cloud balance (equal power).
        addParam(floatParam ("mix",      "Mix", 0.0f, 100.0f, 50.0f, "%", 0));
        // Stop recording: the cloud plays from the captured moment until Freeze goes off. A toggle.
        addParam(boolParam  ("freeze",   "Freeze", false));
        // The same, but held: frozen only while Catch is on (on screen it springs back when you let go).
        addParam(boolParam  ("catch",    "Catch", false));
        // Grain length.
        addParam(logParam   ("size",     "Size", 5.0f, 500.0f, 90.0f, "", 0));        // text from formatValue
        // Grains per second (free mode; Sync uses Division instead).
        addParam(logParam   ("density",  "Density", 1.0f, 200.0f, 16.0f, "/s", 1));
        // How far back in the buffer the grains play from (the delay at each grain's centre).
        addParam(floatParam ("position", "Position", 0.0f, 3000.0f, 150.0f, "", 0, 0.4f)); // text from formatValue
        // Random extra distance back, per grain.
        addParam(floatParam ("spray",    "Spray", 0.0f, 1000.0f, 80.0f, "", 0, 0.4f));      // text from formatValue
        // Transposition of every grain.
        addParam(floatParam ("pitch",    "Pitch", -24.0f, 24.0f, 0.0f, "st", 1));
        // Random transposition per grain, up to this many semitones either way...
        addParam(floatParam ("scatter",  "Scatter", 0.0f, 24.0f, 0.0f, "st", 1));
        // ...free, or snapped to octaves, fifths and octaves, or a minor pentatonic scale.
        addParam(choiceParam("snap",     "Snap", {"Free", "Oct", "5th", "Scale"}, kSnFree));
        // Chance that a grain plays backwards.
        addParam(floatParam ("reverse",  "Reverse", 0.0f, 100.0f, 0.0f, "%", 0));
        // Grain window: 0 smooth (Hann), 50 flat-topped, 100 struck (sharp attack, decaying).
        addParam(floatParam ("shape",    "Shape", 0.0f, 100.0f, 20.0f, "%", 0));
        // Random stereo placement per grain.
        addParam(floatParam ("spread",   "Spread", 0.0f, 100.0f, 60.0f, "%", 0));
        // Slow wander of position (further back) and pitch, so a frozen cloud keeps evolving.
        addParam(floatParam ("drift",    "Drift", 0.0f, 100.0f, 20.0f, "%", 0));
        // How fast Drift wanders: Hz free, or a musical period when Sync is on.
        addParam(logParam   ("driftrate","Drift Rate", 0.05f, 4.0f, 0.25f, "", 2));  // text from formatValue
        // The cloud written back into the buffer: growing, smearing textures (guarded, never runs away).
        addParam(floatParam ("feedback", "Feedback", 0.0f, 100.0f, 0.0f, "%", 0));
        // Allpass blur after the grains: 0 crisp, 100 a wash.
        addParam(floatParam ("smear",    "Smear", 0.0f, 100.0f, 25.0f, "%", 0));
        // Left of centre darker (low-pass down to 300 Hz), right thinner (high-pass up to 2 kHz).
        addParam(floatParam ("tone",     "Tone", -100.0f, 100.0f, 0.0f, "", 0));     // text from formatValue
        // Lock grain triggers and Drift to the song (free at the song tempo while stopped).
        addParam(boolParam  ("sync",     "Sync", false));
        // One grain per Division when Sync is on.
        addParam(choiceParam("division", "Division", divisionNames(), kDivisionSixteenth));
        // Release for Freeze and Catch: after you let go, the caught cloud keeps playing for this long and
        // gives way to the live input grain by grain. 0 = let go at once.
        addParam(floatParam ("linger",   "Linger", 0.0f, 10000.0f, 0.0f, "", 0, 0.4f));    // text from formatValue
        // Automatic tail: when the input goes quiet, the grains keep playing the last thing you played and
        // fade out, reaching -60 dB after this long. Play again and they move to the new sound. 0 = off.
        addParam(floatParam ("tail",     "Tail", 0.0f, 10000.0f, 0.0f, "", 0, 0.4f));      // text from formatValue

        if (numParams() != kNumParams) mpc::log("Plankton: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Soft Cloud", {{kTail, 3000}, {kLinger, 1500}, {kSize, 120}, {kDensity, 18}, {kPosition, 150}, {kSpray, 120}, {kShape, 15},
                               {kSpread, 60}, {kDrift, 25}, {kSmear, 35}, {kTone, -15}, {kMix, 50}});
        program("Frozen Pad", {{kTail, 6000}, {kLinger, 4000}, {kSize, 320}, {kDensity, 30}, {kPosition, 400}, {kSpray, 600}, {kShape, 0},
                               {kSpread, 85}, {kDrift, 55}, {kDriftRate, 0.12f}, {kSmear, 70}, {kTone, -20}, {kMix, 75}});
        program("Shimmer Dust", {{kTail, 5000}, {kLinger, 2500}, {kSize, 90}, {kDensity, 40}, {kPosition, 120}, {kSpray, 300}, {kPitch, 12},
                                 {kScatter, 12}, {kSnap, kSnOctave}, {kShape, 10}, {kSpread, 95}, {kDrift, 20},
                                 {kFeedback, 25}, {kSmear, 55}, {kTone, 15}, {kMix, 45}});
        program("Reverse Swarm", {{kTail, 3000}, {kLinger, 1500}, {kSize, 180}, {kDensity, 26}, {kPosition, 300}, {kSpray, 500}, {kScatter, 0.3f},
                                  {kReverse, 100}, {kShape, 25}, {kSpread, 80}, {kDrift, 30}, {kSmear, 25}, {kMix, 55}});
        program("Rhythmic Grains", {{kLinger, 500}, {kSync, 1}, {kDivision, kDivisionSixteenth}, {kSize, 70}, {kPosition, 250},
                                    {kSpray, 0}, {kScatter, 12}, {kSnap, kSnOctave}, {kReverse, 25}, {kShape, 75},
                                    {kSpread, 50}, {kDrift, 15}, {kDriftRate, 0.5f}, {kSmear, 5}, {kMix, 60}});
        program("Smear Wash", {{kTail, 8000}, {kLinger, 3000}, {kSize, 300}, {kDensity, 50}, {kPosition, 500}, {kSpray, 800}, {kScatter, 0.2f},
                               {kShape, 0}, {kSpread, 90}, {kDrift, 40}, {kFeedback, 40}, {kSmear, 100}, {kTone, -30}, {kMix, 65}});
        program("Glitch Plankton", {{kSize, 14}, {kDensity, 60}, {kPosition, 80}, {kSpray, 900}, {kScatter, 12},
                                    {kSnap, kSnScale}, {kReverse, 50}, {kShape, 90}, {kSpread, 100}, {kDrift, 0},
                                    {kSmear, 0}, {kMix, 50}});
        program("Deep Sea", {{kTail, 6000}, {kLinger, 3000}, {kSize, 250}, {kDensity, 22}, {kPosition, 350}, {kSpray, 400}, {kPitch, -12},
                             {kScatter, 7}, {kSnap, kSnFifth}, {kShape, 5}, {kSpread, 70}, {kDrift, 40},
                             {kFeedback, 20}, {kSmear, 60}, {kTone, -60}, {kMix, 60}});

        buildBaseWindows();
        rebuildWindow(get(kShape) * 0.01f);
    }

    // ----- lifecycle ------------------------------------------------------------------------------
    void prepare(double sampleRate, int maxBlockSize) override
    {
        sr_ = (float)mpc::clamp(sampleRate, 8000.0, 192000.0);
        maxBlock_ = std::max(32, std::min(maxBlockSize, 8192));
        // Ring buffer: a power of two, at least kBufSec of history plus a block.
        int n = 1 << 14;
        const int need = (int)(kBufSec * sr_) + maxBlock_ + 16;
        while (n < need) n <<= 1;
        bufFrames_ = n;
        mask_ = n - 1;
        envMask_ = (n >> kEnvShift) - 1;
        buf_.assign((size_t)n * 2, 0.0f);              // interleaved L/R
        envMap_.assign((size_t)(n >> kEnvShift), 0.0f);
        wetL_.assign((size_t)maxBlock_, 0.0f);
        wetR_.assign((size_t)maxBlock_, 0.0f);
        fbL_.assign((size_t)maxBlock_, 0.0f);
        fbR_.assign((size_t)maxBlock_, 0.0f);
        for (int j = 0; j < 6; ++j)
        {
            smL_[j].allocate((int)std::lround(kSmearMsL[j] * 0.001f * sr_));
            smR_[j].allocate((int)std::lround(kSmearMsR[j] * 0.001f * sr_));
        }
        for (SVF* f : {&lpL_, &lpR_, &hpL_, &hpR_}) f->setSampleRate(sr_);
        seam_ = std::max(16, (int)(kSeamSec * sr_));
        reset();
    }

    void reset() override
    {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        std::fill(envMap_.begin(), envMap_.end(), 0.0f);
        std::fill(fbL_.begin(), fbL_.end(), 0.0f);
        std::fill(fbR_.begin(), fbR_.end(), 0.0f);
        for (Grain& g : grains_) g.active = false;
        for (int j = 0; j < 6; ++j) { smL_[j].clear(); smR_[j].clear(); }
        for (SVF* f : {&lpL_, &lpR_, &hpL_, &hpR_}) f->reset();
        head_ = 0;
        filled_ = 0;
        frozen_ = false;
        lingerActive_ = false;
        lingerSpawns_ = 0;
        follow_ = peakRef_ = 0.0f;
        lastLoud_ = segStart_ = 0;
        releaseCount_ = seam_;     // no fade-in pending
        nextTrig_ = 0.0;
        lastSyncIdx_ = INT64_MIN;
        wasPlaying_ = false;
        wasSync_ = false;
        driftPhase_ = 0.0;
        driftSeg_ = 0;
        driftPos_ = driftPitch_ = 0.0f;
        driftPrimed_ = false;
        dcInL_ = dcInR_ = dcOutL_ = dcOutR_ = 0.0f;
        fbEnv_ = 0.0f;
        fbGuard_ = 1.0f;
        normCur_ = -1.0f;
        dryCur_ = wetCur_ = outCur_ = -1.0f;
        skipped_ = 0;
        rng_.reseed(0x51A7C0DEu);
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        char buf[32];
        switch (index)
        {
        case kSize:
        case kPosition:
        case kSpray:
            if (v < 1000.0f) std::snprintf(buf, sizeof(buf), "%.0f ms", (double)v);
            else std::snprintf(buf, sizeof(buf), "%.2f s", (double)v * 0.001);
            break;
        case kLinger:
        case kTail:
            if (v < 1.0f) std::snprintf(buf, sizeof(buf), "Off");
            else if (v < 1000.0f) std::snprintf(buf, sizeof(buf), "%.0f ms", (double)v);
            else std::snprintf(buf, sizeof(buf), "%.1f s", (double)v * 0.001);
            break;
        case kDriftRate:
            if (getBool(kSync)) std::snprintf(buf, sizeof(buf), "%s", kDriftSyncNames[driftSyncIndex()]);
            else std::snprintf(buf, sizeof(buf), v < 1.0f ? "%.2f Hz" : "%.1f Hz", (double)v);
            break;
        case kTone:
        {
            const int t = (int)std::lround(v);
            if (t == 0) std::snprintf(buf, sizeof(buf), "Flat");
            else if (t < 0) std::snprintf(buf, sizeof(buf), "Dark %d", -t);
            else std::snprintf(buf, sizeof(buf), "Thin %d", t);
            break;
        }
        case kPitch:
            std::snprintf(buf, sizeof(buf), "%+.1f", (double)v);            // the unit "st" is added by the host
            if (std::fabs(v) < 0.05f) std::snprintf(buf, sizeof(buf), "0.0");
            break;
        default:
            return false;
        }
        out = buf;
        return true;
    }

    // ----- audio ----------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        if (buf_.empty() || ctx.numOutputs < 1) return;
        // Treat maxBlock as a maximum, but stay safe if a host sends more: work in chunks.
        int done = 0;
        while (done < ctx.numFrames)
        {
            const int n = std::min(maxBlock_, ctx.numFrames - done);
            processChunk(ctx, done, n);
            done += n;
        }
    }

private:
    // ----- window tables --------------------------------------------------------------------------
    void buildBaseWindows()
    {
        for (int i = 0; i <= kWinLen; ++i)
        {
            const float x = (float)i / (float)kWinLen;                 // 0..1
            // Hann
            winHann_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * x);
            // Flat top (Tukey, 25 % taper each end)
            const float t = 0.25f;
            if (x < t) winFlat_[i] = 0.5f - 0.5f * std::cos(kPi * x / t);
            else if (x > 1.0f - t) winFlat_[i] = 0.5f - 0.5f * std::cos(kPi * (1.0f - x) / t);
            else winFlat_[i] = 1.0f;
            // Struck: 2 % raised-cosine attack, exponential decay, 6 % taper to zero at the end
            float s;
            if (x < 0.02f) s = 0.5f - 0.5f * std::cos(kPi * x / 0.02f);
            else s = std::exp(-5.0f * (x - 0.02f) / 0.98f);
            if (x > 0.94f) s *= 0.5f + 0.5f * std::cos(kPi * (x - 0.94f) / 0.06f);
            winPerc_[i] = s;
        }
        winHann_[0] = winFlat_[0] = winPerc_[0] = 0.0f;
        winHann_[kWinLen] = winFlat_[kWinLen] = winPerc_[kWinLen] = 0.0f;
    }

    // Blend the three base shapes into the live table (only when Shape moves) and measure its power.
    void rebuildWindow(float shape)
    {
        shape = mpc::clamp(shape, 0.0f, 1.0f);
        const float* a; const float* b; float t;
        if (shape < 0.5f) { a = winHann_; b = winFlat_; t = shape * 2.0f; }
        else              { a = winFlat_; b = winPerc_; t = (shape - 0.5f) * 2.0f; }
        double ms = 0.0;
        for (int i = 0; i <= kWinLen; ++i)
        {
            win_[i] = a[i] + t * (b[i] - a[i]);
            if (i < kWinLen) ms += (double)win_[i] * win_[i];
        }
        winMs_ = (float)(ms / kWinLen);
        winShape_ = shape;
    }

    int driftSyncIndex() const
    {
        return mpc::clamp((int)std::lround(getNormalised(kDriftRate) * (float)(kNumDriftSync - 1)), 0, kNumDriftSync - 1);
    }

    // ----- pitch scatter --------------------------------------------------------------------------
    float scatterOffset(float range, int snap)
    {
        if (range <= 0.001f) return 0.0f;
        const float o = range * rng_.nextBipolar();
        if (snap == kSnFree) return o;
        const float* set; int count;
        if (snap == kSnOctave) { set = kOctaveSet; count = 5; }
        else if (snap == kSnFifth) { set = kFifthSet; count = 9; }
        else { set = kScaleSet; count = 21; }
        float best = 0.0f, bestD = 1e9f;
        for (int i = 0; i < count; ++i)
        {
            if (std::fabs(set[i]) > range + 0.001f) continue;
            const float d = std::fabs(set[i] - o);
            if (d < bestD) { bestD = d; best = set[i]; }
        }
        return best;
    }

    // ----- grain birth ----------------------------------------------------------------------------
    struct BlockParams
    {
        float pitch, scatter, reverseProb, spread, posSamp, spraySamp, sizeSamp;
        int   snap;
    };

    // fromCaught: read the caught moment of a Freeze/Catch that has been let go (Linger), if it still fits.
    // tailCentre >= 0: a Tail grain, centred that many frames back (instead of Position + Spray), at level gain.
    void spawn(int offset, const BlockParams& bp, bool fromCaught = false, float tailCentre = -1.0f, float gain = 1.0f)
    {
        int slot = -1;
        for (int i = 0; i < kPool; ++i) if (!grains_[i].active) { slot = i; break; }
        if (slot < 0) { ++skipped_; return; }
        Grain& g = grains_[slot];

        float semis = bp.pitch + scatterOffset(bp.scatter, bp.snap) + driftPitch_;
        semis = mpc::clamp(semis, -36.0f, 36.0f);
        const float r = std::exp2(semis * (1.0f / 12.0f));
        float len = bp.sizeSamp;
        const float maxRead = kMaxReadSec * sr_;
        if (r * len > maxRead) len = maxRead / r;
        len = std::max(len, 16.0f);
        const bool rev = rng_.nextFloat() < bp.reverseProb;

        // Delay at the grain's centre.
        float c = bp.posSamp + driftPos_ + bp.spraySamp * rng_.nextFloat();
        const float ext = r * len;                       // material read, in frames
        const double N = (double)bufFrames_;
        double L0, lagMin, lagMax;
        int64_t H;                                        // the frame at lag 0 (this sample's input)
        bool caught = frozen_;
        if (fromCaught && !frozen_)
        {
            // Lingering: lags are measured back from the moment caught by the last Freeze/Catch. The recorder
            // has been overwriting that buffer from its oldest end since the release (w frames so far, the
            // whole chunk already written), so keep the grain clear of what it overwrites during the grain.
            H = lingerHead_ - 1;
            const double w = (double)(head_ + curN_ - lingerHead_);
            const double guard = kFrozenGuardSec * sr_;
            const double edge = std::min((double)lingerFilled_, N) - guard;
            const double ow = N - w - guard;
            if (!rev)
            {
                L0 = c + ext * 0.5f;
                lagMin = seam_ + 4.0 + ext;
                lagMax = std::min(edge, ow - std::max(0.0f, (1.0f - r) * len));
            }
            else
            {
                L0 = c - ext * 0.5f;
                lagMin = seam_ + 4.0;
                lagMax = std::min(edge - ext, ow - (1.0f + r) * len);
            }
            if (lagMax >= lagMin) caught = true;          // otherwise the caught moment is gone: a live grain
            else fromCaught = false;
        }
        if (fromCaught && caught) ++lingerSpawns_;        // bounds set above
        else if (!frozen_)
        {
            if (tailCentre >= 0.0f) c = tailCentre + driftPos_;
            H = head_ + offset;
            const double margin = seam_ + 4.0;
            const double oldEdge = N - maxBlock_ - seam_ - 8.0;
            if (!rev)
            {
                L0 = c + (r - 1.0f) * len * 0.5f;
                lagMin = margin + std::max(0.0f, (r - 1.0f) * len);
                lagMax = oldEdge - std::max(0.0f, (1.0f - r) * len);
            }
            else
            {
                L0 = c - (1.0f + r) * len * 0.5f;
                lagMin = margin;
                lagMax = oldEdge - (1.0f + r) * len;
            }
        }
        else
        {
            H = frozenHead_ - 1;                          // the newest frame captured
            const double cap = std::min((double)filled_, N) - kFrozenGuardSec * sr_;
            if (!rev)
            {
                L0 = c + ext * 0.5f;
                lagMin = seam_ + 4.0 + ext;
                lagMax = cap;
            }
            else
            {
                L0 = c - ext * 0.5f;
                lagMin = seam_ + 4.0;
                lagMax = cap - ext;
            }
        }
        if (L0 > lagMax) L0 = lagMax;
        if (L0 < lagMin) L0 = lagMin;

        g.pos = (double)H - L0;
        g.pos = std::fmod(g.pos, N);
        if (g.pos < 0.0) g.pos += N;
        g.pos += 2.0 * N;                                 // stays positive for any reverse run
        g.inc = rev ? -r : r;
        g.wph = 0.0f;
        g.winc = (float)kWinLen / len;
        g.life = -1;

        // Stereo: narrow each grain toward mono by Spread, then pan it by Spread x random.
        const float a = bp.spread * 0.5f;
        const float p = bp.spread * rng_.nextBipolar();
        const float th = (p + 1.0f) * 0.25f * kPi;
        float gl = std::cos(th) * 1.41421356f, gr = std::sin(th) * 1.41421356f;
        if (!(fromCaught || frozen_) && gain < 1.0f) { gl *= gain; gr *= gain; }   // Tail's fade (in the pan matrix: free)
        g.mLL = gl * (1.0f - a); g.mRL = gl * a;
        g.mLR = gr * a;          g.mRR = gr * (1.0f - a);
        g.active = true;
        PLANKTON_GRAIN_HOOK(offset, (float)L0, r, rev, len);
        render(g, offset, curN_);
    }

    // Render one grain into wetL_/wetR_ from sample s0 to s1.
    void render(Grain& g, int s0, int s1)
    {
        const float* b = buf_.data();
        const int64_t m = mask_;
        float* wl = wetL_.data();
        float* wr = wetR_.data();
        double pos = g.pos;
        float wph = g.wph;
        const float inc = g.inc, winc = g.winc;
        if (g.life >= 0)
        {
            if (s1 - s0 >= g.life) { s1 = s0 + g.life; g.active = false; }   // stops inside the faded seam
            g.life -= (s1 - s0);
        }
        for (int k = s0; k < s1; ++k)
        {
            const int wi = (int)wph;
            const float wf = wph - (float)wi;
            const float w = win_[wi] + wf * (win_[wi + 1] - win_[wi]);
            const int64_t ip = (int64_t)pos;
            const float f = (float)(pos - (double)ip);
            const float* p0 = b + ((ip - 1) & m) * 2;
            const float* p1 = b + (ip & m) * 2;
            const float* p2 = b + ((ip + 1) & m) * 2;
            const float* p3 = b + ((ip + 2) & m) * 2;
            const float l = hermite(p0[0], p1[0], p2[0], p3[0], f) * w;
            const float r = hermite(p0[1], p1[1], p2[1], p3[1], f) * w;
            wl[k] += g.mLL * l + g.mRL * r;
            wr[k] += g.mLR * l + g.mRR * r;
            pos += inc;
            wph += winc;
            if (wph >= (float)kWinLen) { g.active = false; break; }
        }
        g.pos = pos;
        g.wph = wph;
    }

    // ----- freeze transitions ---------------------------------------------------------------------
    // Fade the frames either side of the write head to silence, so the frozen buffer has a soft seam.
    void fadeSeam()
    {
        float* b = buf_.data();
        for (int j = 0; j < seam_; ++j)
        {
            const float gn = smoothstep01(((float)j + 0.5f) / (float)seam_);
            const int64_t newest = (head_ - 1 - j) & mask_;
            const int64_t oldest = (head_ + j) & mask_;
            b[newest * 2] *= gn; b[newest * 2 + 1] *= gn;
            b[oldest * 2] *= gn; b[oldest * 2 + 1] *= gn;
        }
    }

    // ----- one chunk ------------------------------------------------------------------------------
    void processChunk(mpc::ProcessContext& ctx, int off, int n)
    {
        curN_ = n;
        const float* inL = (ctx.inputs && ctx.numInputs > 0) ? ctx.inputs[0] + off : nullptr;
        const float* inR = (ctx.inputs && ctx.numInputs > 1) ? ctx.inputs[1] + off : inL;
        float* outL = ctx.outputs[0] + off;
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] + off : nullptr;

        // ---- clock --------------------------------------------------------------------------------
        const mpc::Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        const double bpm = (tr.valid && tr.tempoValid) ? (double)safeBpm(tr.bpm) : 120.0;
        const double spb = (double)sr_ * 60.0 / bpm;        // samples per beat
        const double ppq0 = tr.ppqPos + (double)off / spb;

        // ---- parameters, once per chunk -----------------------------------------------------------
        const bool sync = getBool(kSync);
        const float shape = get(kShape) * 0.01f;
        if (std::fabs(shape - winShape_) > 0.0005f) rebuildWindow(shape);

        BlockParams bp;
        bp.pitch = get(kPitch);
        bp.scatter = get(kScatter);
        bp.snap = getInt(kSnap);
        bp.reverseProb = get(kReverse) * 0.01f;
        bp.spread = get(kSpread) * 0.01f;
        bp.posSamp = get(kPosition) * 0.001f * sr_;
        bp.spraySamp = get(kSpray) * 0.001f * sr_;
        bp.sizeSamp = get(kSize) * 0.001f * sr_;
        const float divBeats = divisionBeats(getInt(kDivision));
        const double divSamp = std::max(16.0, (double)divBeats * spb);
        const float density = get(kDensity);

        // ---- freeze -------------------------------------------------------------------------------
        const bool wantFrozen = getBool(kFreeze) || getBool(kCatch);
        if (wantFrozen && !frozen_)
        {
            fadeSeam();
            frozen_ = true;
            frozenHead_ = head_;
            lingerActive_ = false;                // a new catch replaces any moment still lingering
            // Grains started live and pitched up would now run into the seam (the head no longer moves
            // away from them). Stop each one where it reaches the faded, silent part of the seam, so it
            // can never be reading the oldest frames when the writer comes back to them on release.
            const double N = (double)bufFrames_;
            const double H = (double)(frozenHead_ - 1);
            for (Grain& g : grains_)
            {
                if (!g.active || g.inc <= 0.0f) continue;
                double lag = std::fmod(H - g.pos, N);
                if (lag < 0.0) lag += N;
                const int life = std::max(0, (int)((lag - 3.0) / (double)g.inc));
                const int remaining = (int)(((float)kWinLen - g.wph) / g.winc) + 1;
                if (life < remaining) g.life = life;
            }
        }
        else if (!wantFrozen && frozen_)
        {
            frozen_ = false;
            releaseCount_ = 0;            // fade the input in from the (already faded) seam
            // Linger: keep drawing grains from the caught moment for a while after letting go.
            lingerActive_ = get(kLinger) >= 1.0f;
            lingerHead_ = frozenHead_;
            lingerFilled_ = filled_;
            lingerElapsed_ = 0;
        }

        // ---- drift (evaluated once per chunk, smoothed) ------------------------------------------
        {
            const float amt = get(kDrift) * 0.01f;
            float u, v;
            if (sync)
            {
                const float periodBeats = kDriftSyncBeats[driftSyncIndex()];
                if (playing)
                {
                    const double x = ppq0 / (double)periodBeats;
                    const double fl = std::floor(x);
                    driftSeg_ = (int64_t)fl;
                    driftPhase_ = x - fl;
                }
                else advanceDrift((double)n / ((double)periodBeats * spb));
            }
            else advanceDrift((double)n * (double)get(kDriftRate) / (double)sr_);
            const float t = smoothstep01((float)driftPhase_);
            u = hashBipolar(driftSeg_, 1u) + t * (hashBipolar(driftSeg_ + 1, 1u) - hashBipolar(driftSeg_, 1u));
            v = hashBipolar(driftSeg_, 2u) + t * (hashBipolar(driftSeg_ + 1, 2u) - hashBipolar(driftSeg_, 2u));
            const float targetPos = amt * kDriftPosMs * 0.001f * sr_ * 0.5f * (u + 1.0f);
            const float targetPitch = amt * kDriftPitchSt * v;
            if (!driftPrimed_) { driftPos_ = targetPos; driftPitch_ = targetPitch; driftPrimed_ = true; }
            const float k = 1.0f - std::exp(-(float)n / (0.15f * sr_));   // ~150 ms glide (sync jumps)
            driftPos_ += k * (targetPos - driftPos_);
            driftPitch_ += k * (targetPitch - driftPitch_);
        }

        // ---- Tail: follow the input level and remember the last frame where it was "playing" ---------
        {
            const float rel = std::exp(-1.0f / (kTailFollowSec * sr_));
            const float pkRel = std::exp(-1.0f / (kTailPeakSec * sr_));
            const float relGain = std::pow(10.0f, kTailRelDb / 20.0f);
            for (int k = 0; k < n; ++k)
            {
                const float x = std::max(std::fabs(inL ? inL[k] : 0.0f), std::fabs(inR ? inR[k] : 0.0f));
                if (!frozen_)
                {
                    // loudness map: the peak of each 64-frame cell (cleared as the cell is first written)
                    const int64_t f = head_ + k;
                    float& cell = envMap_[(size_t)((f >> kEnvShift) & envMask_)];
                    if ((f & ((1 << kEnvShift) - 1)) == 0) cell = 0.0f;
                    if (x > cell) cell = x;
                }
                follow_ = x > follow_ ? x : follow_ * rel;
                peakRef_ = follow_ > peakRef_ ? follow_ : peakRef_ * pkRel;
                if (follow_ > kTailFloor && follow_ >= relGain * peakRef_)
                {
                    if (head_ + k - lastLoud_ > (int64_t)(kTailGapSec * sr_)) segStart_ = head_ + k;   // a new event
                    lastLoud_ = head_ + k;
                }
            }
        }

        // ---- write the input into the buffer (whole chunk first, so grains can read any of it) ----
        if (!frozen_)
        {
            const float fbAmt = kFbMax * get(kFeedback) * 0.01f * fbGuard_;
            float* b = buf_.data();
            for (int k = 0; k < n; ++k)
            {
                float l = inL ? inL[k] : 0.0f;
                float r = inR ? inR[k] : 0.0f;
                if (fbAmt > 0.0f)
                {
                    // DC blocker on the fed-back cloud
                    const float xl = fbL_[(size_t)k], xr = fbR_[(size_t)k];
                    dcOutL_ = xl - dcInL_ + 0.995f * dcOutL_; dcInL_ = xl;
                    dcOutR_ = xr - dcInR_ + 0.995f * dcOutR_; dcInR_ = xr;
                    l += fbAmt * dcOutL_;
                    r += fbAmt * dcOutR_;
                }
                l = ceiling(l); r = ceiling(r);
                if (releaseCount_ < seam_)
                {
                    const float gIn = smoothstep01(((float)releaseCount_ + 0.5f) / (float)seam_);
                    l *= gIn; r *= gIn;
                    ++releaseCount_;
                }
                const int64_t i = (head_ + k) & mask_;
                b[i * 2] = l;
                b[i * 2 + 1] = r;
            }
        }

        // ---- render the grains already sounding --------------------------------------------------
        std::fill(wetL_.begin(), wetL_.begin() + n, 0.0f);
        std::fill(wetR_.begin(), wetR_.begin() + n, 0.0f);
        for (Grain& g : grains_) if (g.active) render(g, 0, n);

        // ---- schedule new grains in this chunk ---------------------------------------------------
        int trig[kMaxTriggersPerBlock];
        int nt = 0;
        if (sync != wasSync_ || (sync && playing != wasPlaying_)) { nextTrig_ = 0.0; lastSyncIdx_ = INT64_MIN; }
        wasSync_ = sync;
        wasPlaying_ = playing;
        if (sync && playing)
        {
            // Grains on every Division boundary of the song position, on the nearest sample. A boundary
            // that rounds to the next chunk is left for it (the search starts half a sample early, and
            // lastSyncIdx_ stops a boundary firing twice).
            const double bps = 1.0 / spb;
            int64_t idx = (int64_t)std::ceil((ppq0 - 0.5 * bps) / (double)divBeats - 1e-9);
            for (;;)
            {
                const long at = std::lround(((double)idx * (double)divBeats - ppq0) / bps);
                if (at >= (long)n || nt >= kMaxTriggersPerBlock) break;
                if (idx != lastSyncIdx_ && at >= 0) { trig[nt++] = (int)at; lastSyncIdx_ = idx; }
                ++idx;
            }
        }
        else
        {
            // Free: Density per second with jitter, or (Sync, stopped) the Division at the song tempo.
            const double interval = sync ? divSamp : (double)sr_ / (double)std::max(0.1f, density);
            while (nextTrig_ < (double)n && nt < kMaxTriggersPerBlock)
            {
                trig[nt++] = std::max(0, (int)nextTrig_);
                const double j = sync ? 1.0 : 1.0 + (double)kFreeJitter * (double)rng_.nextBipolar();
                nextTrig_ += std::max(1.0, interval * j);
            }
            nextTrig_ -= (double)n;
            if (nextTrig_ < -(double)n) nextTrig_ = 0.0;
        }
        // Each grain comes from the caught moment with a probability that falls from 1 to 0 over Linger (a
        // raised-cosine release, so the cloud stays caught for a while and then lets go smoothly), or from
        // the live buffer otherwise: the caught cloud gives way to the live one grain by grain.
        const float lingerLen = get(kLinger) * 0.001f * sr_;
        const float tailLen = get(kTail) * 0.001f * sr_;
        if (lingerActive_ && lingerLen < 1.0f) lingerActive_ = false;
        for (int t = 0; t < nt; ++t)
        {
            bool fromCaught = false;
            if (lingerActive_)
            {
                const float x = (float)(lingerElapsed_ + trig[t]) / lingerLen;
                const float env = x >= 1.0f ? 0.0f : 0.5f + 0.5f * std::cos(kPi * x);
                fromCaught = rng_.nextFloat() < env;
            }
            float tailCentre = -1.0f, gain = 1.0f;
            if (!fromCaught && !frozen_ && tailLen >= 1.0f)
            {
                // The input has been quiet for `since`. Live grains keep reading the last thing you played
                // until they run past it (Position back); after that, Tail grains take over: each is
                // centred at random inside that last event (from where it got loud to where it got quiet,
                // its last second at most), and they fade, -60 dB after Tail. Below that, no grain starts.
                const float since = (float)std::max<int64_t>(0, head_ + trig[t] - lastLoud_);
                const float over = since - bp.posSamp;
                if (over > 0.0f)
                {
                    gain = std::exp(-6.9078f * over / tailLen);
                    if (gain < 0.001f) continue;
                    // Weighted toward the loud parts: the loudest of a few random spots in the event.
                    const float w = std::max(0.0f, std::min((float)(lastLoud_ - segStart_), kTailWindowSec * sr_));
                    float best = -1.0f;
                    for (int tr = 0; tr < kTailTries; ++tr)
                    {
                        const float back = w * rng_.nextFloat();
                        const int64_t f = lastLoud_ - (int64_t)back;
                        const float e = envMap_[(size_t)((f >> kEnvShift) & envMask_)];
                        if (e > best) { best = e; tailCentre = since + back; }
                    }
                }
            }
            spawn(trig[t], bp, fromCaught, tailCentre, gain);
        }
        if (lingerActive_)
        {
            lingerElapsed_ += n;
            if ((float)lingerElapsed_ >= lingerLen) lingerActive_ = false;
        }

        // ---- level normalisation from the expected overlap ---------------------------------------
        const double rate = sync ? (double)sr_ / divSamp : (double)density;
        const float overlap = (float)std::min((double)kPool, rate * (double)bp.sizeSamp / (double)sr_);
        const float normTarget = 1.0f / std::sqrt(std::max(1.0f, overlap * winMs_));
        if (normCur_ < 0.0f) normCur_ = normTarget;
        const float normStep = (normTarget - normCur_) / (float)n;

        // ---- smear, tone ----------------------------------------------------------------------------
        const float smear = get(kSmear) * 0.01f;
        const float sg = 0.4f + 0.3f * smear;
        const float tone = get(kTone) * 0.01f;
        const float lpHz = tone < 0.0f ? 18000.0f * std::pow(300.0f / 18000.0f, -tone) : 20000.0f;
        const float hpHz = tone > 0.0f ? 20.0f * std::pow(2000.0f / 20.0f, tone) : 10.0f;
        lpL_.set(lpHz, 0.1f); lpR_.set(lpHz, 0.1f);
        hpL_.set(hpHz, 0.1f); hpR_.set(hpHz, 0.1f);
        const bool useLp = tone < -0.001f, useHp = tone > 0.001f;

        // ---- mix and output ramps ----------------------------------------------------------------
        const float mix = get(kMix) * 0.01f;
        const float dryT = std::cos(mix * 0.5f * kPi), wetT = std::sin(mix * 0.5f * kPi);
        const float outT = dbToGain(get(kOutput));
        if (dryCur_ < 0.0f) { dryCur_ = dryT; wetCur_ = wetT; outCur_ = outT; }
        const float inv = 1.0f / (float)n;
        const float dStep = (dryT - dryCur_) * inv, wStep = (wetT - wetCur_) * inv, oStep = (outT - outCur_) * inv;

        float peak = 0.0f;
        for (int k = 0; k < n; ++k)
        {
            normCur_ += normStep;
            float l = wetL_[(size_t)k] * normCur_;
            float r = wetR_[(size_t)k] * normCur_;
            if (smear > 0.001f)
            {
                float dl = l, dr = r;
                for (int j = 0; j < 6; ++j) { dl = smL_[j].process(dl, sg); dr = smR_[j].process(dr, sg); }
                l += smear * (dl - l);
                r += smear * (dr - r);
            }
            if (useLp) { l = lpL_.process(l, SVF::Mode::Lowpass); r = lpR_.process(r, SVF::Mode::Lowpass); }
            if (useHp) { l = hpL_.process(l, SVF::Mode::Highpass); r = hpR_.process(r, SVF::Mode::Highpass); }
            fbL_[(size_t)k] = l;
            fbR_[(size_t)k] = r;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));

            const float xl = inL ? inL[k] : 0.0f;          // read the input before writing the output
            const float xr = inR ? inR[k] : 0.0f;
            dryCur_ += dStep; wetCur_ += wStep; outCur_ += oStep;
            const float yl = (xl * dryCur_ + l * wetCur_) * outCur_;
            const float yr = (xr * dryCur_ + r * wetCur_) * outCur_;
            outL[k] = ceiling(yl);
            if (outR) outR[k] = ceiling(yr);
        }
        normCur_ = normTarget;
        dryCur_ = dryT; wetCur_ = wetT; outCur_ = outT;
        // A chunk shorter than the feedback buffer leaves stale samples past n: clear them.
        if (n < maxBlock_)
        {
            std::fill(fbL_.begin() + n, fbL_.end(), 0.0f);
            std::fill(fbR_.begin() + n, fbR_.end(), 0.0f);
        }

        // ---- feedback guard: ease the feedback off as the cloud gets loud ------------------------
        fbEnv_ = std::max(peak, fbEnv_ * std::exp(-(float)n / (0.3f * sr_)));
        const float guardT = fbEnv_ > kFbCeiling ? kFbCeiling / fbEnv_ : 1.0f;
        fbGuard_ += (guardT < fbGuard_ ? 0.5f : 0.05f) * (guardT - fbGuard_);

        // ---- advance the write head --------------------------------------------------------------
        if (!frozen_)
        {
            head_ += n;
            filled_ = std::min<int64_t>((int64_t)bufFrames_, filled_ + n);
        }
    }

    void advanceDrift(double dPhase)
    {
        driftPhase_ += dPhase;
        if (driftPhase_ >= 1.0)
        {
            const double fl = std::floor(driftPhase_);
            driftSeg_ += (int64_t)fl;
            driftPhase_ -= fl;
        }
    }

    // ----- state ----------------------------------------------------------------------------------
    float sr_ = 44100.0f;
    int maxBlock_ = 512;
    int curN_ = 0;
    std::vector<float> buf_;          // interleaved stereo ring buffer
    int bufFrames_ = 0;
    int64_t mask_ = 0;
    int64_t head_ = 0;                // next frame to write (monotonic; masked on use)
    int64_t frozenHead_ = 0;
    int64_t filled_ = 0;
    int seam_ = 352;
    int releaseCount_ = 0;
    bool frozen_ = false;
    bool lingerActive_ = false;       // drawing grains from the last caught moment after a release
    int64_t lingerHead_ = 0, lingerFilled_ = 0, lingerElapsed_ = 0;
    int lingerSpawns_ = 0;            // grains drawn from a lingering moment (tests read it)
    float follow_ = 0.0f, peakRef_ = 0.0f;   // Tail's input level follower and its recent peak
    int64_t lastLoud_ = 0;                   // the last frame where the input counted as playing
    int64_t segStart_ = 0;                   // where that stretch of playing began
    std::vector<float> envMap_;              // input loudness, one peak per 64 frames, aligned with the buffer
    int64_t envMask_ = 0;

    Grain grains_[kPool];
    int skipped_ = 0;                 // triggers dropped because all grains were busy (tests read it)
    Xorshift32 rng_;

    float winHann_[kWinLen + 1], winFlat_[kWinLen + 1], winPerc_[kWinLen + 1];
    float win_[kWinLen + 1];
    float winMs_ = 0.375f;
    float winShape_ = -1.0f;

    double nextTrig_ = 0.0;
    int64_t lastSyncIdx_ = INT64_MIN;
    bool wasPlaying_ = false, wasSync_ = false;

    double driftPhase_ = 0.0;
    int64_t driftSeg_ = 0;
    float driftPos_ = 0.0f, driftPitch_ = 0.0f;
    bool driftPrimed_ = false;

    std::vector<float> wetL_, wetR_, fbL_, fbR_;
    float dcInL_ = 0, dcInR_ = 0, dcOutL_ = 0, dcOutR_ = 0;
    float fbEnv_ = 0.0f, fbGuard_ = 1.0f;
    Allpass smL_[6], smR_[6];
    SVF lpL_, lpR_, hpL_, hpR_;
    float normCur_ = -1.0f, dryCur_ = -1.0f, wetCur_ = -1.0f, outCur_ = -1.0f;
};

MPC_PLUGIN_ENTRY(Plankton)
