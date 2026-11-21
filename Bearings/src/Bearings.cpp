// Bearings - place a sound in the stereo field and move it there, by Groundswell.
//
// SPDX-License-Identifier: MIT
//
// One instance per track (or per pad, where the MPC offers a pad insert) puts that sound somewhere on a plane in
// front of the listener and, if you like, sends it round a path: a circle, a figure 8, a pendulum, a ping-pong, a
// slow random drift, a tide that breathes in and out, or a step / scatter that jumps on every hit. Locked to the
// song position, several instances with different Phase settings move in formation.
//
// Signal flow:
//
//   in L+R ─► mono ─┬─────────────────────────────────── (Split Off) ──────────────► EMITTER 0 ─┐
//                   └─► crossover (Split On): LOW ───────────────────────────────► EMITTER 0 ─┤
//                                             MID ───────────────────────────────► EMITTER 1 ─┼─► wet L/R ─► soft knee ─► mix ─► Output
//                                             HIGH ──────────────────────────────► EMITTER 2 ─┤        ▲      (dry = the
//                                                                                              │        │       untouched
//                                          each emitter's room send (pre-delayed, panned) ───► ROOM ────┘       stereo input)
//
//   An EMITTER is one sound at a point (x, y) on the plane: x = left..right (-1..1), y = near..far (0..1).
//   From (x, y) it works out, every 16 samples, and ramps towards over the next 16:
//     ears      constant-power pan on x, with the far ear still hearing it round the head (-12 dB at the side),
//               up to 0.66 ms later (interaural time difference) and through a low-pass (head shadow). These are
//               the head cues from Tidepools 1.1's Swirl. Mono Safe drops the time difference so a mono sum
//               cannot comb.
//     distance  quieter (up to -18 dB) and duller (up to 3 octaves of high cut) with y, scaled by Depth; a
//               propagation delay of up to 20 ms with y, scaled by Doppler, so movement towards or away bends
//               the pitch a little (slew-limited so a jump cannot chirp); and more of it goes to the room.
//     room      a send whose level rises with y (direct-to-reverb ratio is the ear's main distance cue), pre-
//               delayed by 25 ms when near and 3 ms when far, panned like the source. One shared four-line FDN.
//
//   The plane position:
//     x = Pan      + Size X * size * u          (clamped -1..1)
//     y = Distance + Size Y * size * v / 2 + push   (clamped 0..1)
//   where (u, v) in -1..1 is the path at the current phase (v = -1 near, +1 far), smoothed by Glide;
//   size = band size * (1 + Fol>Size * follower) * (1 + throw), push = Fol>Push * follower * 0.6 + throw / 2.
//
//   Paths (u, v at phase p in turns; all start in front, centre):
//     Fixed      0, 0
//     Circle     sin, -cos           front, right, behind, left (Counter mirrors it)
//     Figure 8   sin p, sin 2p        two loops, left and right
//     Pendulum   sin p, 2u^2 - 1      a swing that is nearest in the middle (Size Y sets the arc)
//     Ping-Pong  -1 / +1              hard jumps; Glide rounds them off
//     Drift      smooth random walk   one wander per cycle, two octaves of it
//     Tide       0, -cos              straight towards you and away
//     Step       a circle, but it moves Step Angle further round on each hit (onset detector) instead of in time
//     Scatter    a new random point inside the path area on each hit
//
//   Split (band mode): Linkwitz-Riley 4th-order crossovers at Low Xover and High Xover, the low band phase-aligned
//   with an allpass so the three bands add back up flat. Each band is its own emitter on the same path, offset by
//   Mid Phase / High Phase and scaled by its Size. Low Size 0 (the default) keeps the bass still at Pan / Distance.
//
//   Clock: in Sync the phase is the song position divided by the cycle length, plus Phase, so every instance on
//   the same Rate and Phase is in the same place on the same beat. Free runs in Hz. Fol>Rate adds phase on top
//   (louder = further ahead), which in Sync means it no longer lands on the same spot each bar.
//
// Parameters (ids are permanent once released - never rename or reorder; append new ones at the end):
//    0 Output      dB  -24..+12   0      level after everything (the host nudges parameter 0 on load)
//    1 Mix         %   0..100     100    dry stereo / placed
//    2 Pan         -100..100      C      where the path is centred, left..right
//    3 Distance    %   0..100     25     where the path is centred, near..far
//    4 Depth       %   0..100     60     how strongly distance changes level and tone
//    5 Doppler     %   0..100     25     pitch bend from movement towards / away (0 = none)
//    6 Room        %   0..100     20     room send; the farther the sound, the more of it
//    7 Room Size   %   0..100     40     small booth .. hall (also the decay)
//    8 Mono Safe   Off/On         Off    no interaural delay: nothing can comb when summed to mono
//    9 Path        Fixed, Circle, Figure 8, Pendulum, Ping-Pong, Drift, Tide, Step, Scatter
//   10 Sync        Free / Sync    Sync
//   11 Rate        0..1           2 bars one control for both modes: 0.01..10 Hz, or 16 bars .. 1/16
//   12 Direction   Clockwise / Counter
//   13 Size X      %   0..100     70     how far the path reaches left and right
//   14 Size Y      %   0..100     30     how far it reaches towards you and away
//   15 Phase       deg 0..360     0      where on the path this instance is (formation offset)
//   16 Glide       ms  5..2000    40     smoothing on every move; rounds off Ping-Pong, Step and Scatter
//   17 Step Angle  deg 15..180    90     Step: how far round each hit moves it
//   18 Hit Sens    %   0..100     50     Step / Scatter: how easily a hit is detected
//   19 Fol Attack  ms  1..1000    20     loudness follower
//   20 Fol Release ms  10..5000   400
//   21 Fol Gain    dB  -12..+36   12     raise for quiet sources
//   22 Fol>Size    %   -100..100  0      louder = wider path (+) or tighter (-)
//   23 Fol>Rate    %   -100..100  0      louder = moves faster (+) or slower / backwards (-)
//   24 Fol>Push    %   -100..100  0      louder = pushed away (+) or pulled close (-)
//   25 Throw       %   0..100     0      manual fling for a Q-Link: out fast, drifts back over ~1.5 s
//   26 Split       Off / On       Off    three-band mode
//   27 Low Xover   Hz  60..1000   200
//   28 High Xover  Hz  1000..10000 2500
//   29 Low Size    %   0..100     0      each band's share of Size X / Size Y (0 = the bass stays put)
//   30 Mid Size    %   0..100     100
//   31 High Size   %   0..100     100
//   32 Mid Phase   deg 0..360     120    each band's offset along the path
//   33 High Phase  deg 0..360     240
//   34 X Now       meter (not saved, not automatable): where the sound is, -100..100 (mid band in Split)
//   35 Y Now       meter: 0..100
//   36..43 Dot 1..8  meters for the skin's globe: the plane cut into 8 rows from near to far; the row the sound is
//                  in holds its left-right position as (column + 1) / 32, every other row holds 0 (no dot)
//
// CPU: per emitter a 4096..16384-sample delay with three linear taps, two one-pole filters and nine ramps; the
// room is four delay lines and four allpasses; Split adds nine biquads. Control maths (a few sin/cos/exp per
// emitter) runs every 16 samples. With Room at 0 the reverb is skipped once its tail has died.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/biquad.h"
#include "dsp/delay_line.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"
#include "dsp/tempo.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace mpc::dsp;

namespace {

enum ParamIndex
{
    kOutput = 0, kMix, kPan, kDistance, kDepth, kDoppler, kRoom, kRoomSize, kMonoSafe,
    kPath, kSync, kRate, kDirection, kSizeX, kSizeY, kPhase, kGlide, kStepAngle, kHitSens,
    kFolAttack, kFolRelease, kFolGain, kFolSize, kFolRate, kFolPush, kThrow,
    kSplit, kLowXover, kHighXover, kLowSize, kMidSize, kHighSize, kMidPhase, kHighPhase,
    kXNow, kYNow,
    kDot0, kDotLast = kDot0 + 7,
    kNumParams
};

enum Path { kFixed = 0, kCircle, kFigure8, kPendulum, kPingPong, kDrift, kTide, kStep, kScatter };

constexpr int kCtrl = 16;              // control-rate interval in samples
constexpr int kDotRows = 8, kDotCols = 32;   // the skin's globe: rows near..far, columns left..right
constexpr int kMaxEmitters = 3;

// Rate in Sync mode: slowest to fastest, so turning the knob up always speeds it up.
struct SyncDiv { const char* name; float beats; };
constexpr SyncDiv kSyncDivs[] = {
    {"16 bars", 64.0f}, {"8 bars", 32.0f}, {"4 bars", 16.0f}, {"2 bars", 8.0f}, {"1 bar", 4.0f}, {"1/2D", 3.0f},
    {"1/2", 2.0f}, {"1/4D", 1.5f}, {"1/4", 1.0f}, {"1/8D", 0.75f}, {"1/4T", 0.66667f}, {"1/8", 0.5f},
    {"1/8T", 0.33333f}, {"1/16", 0.25f}
};
constexpr int kNumSyncDivs = (int)(sizeof(kSyncDivs) / sizeof(kSyncDivs[0]));
inline int syncIndex(float rate01) { return mpc::clamp((int)std::lround(rate01 * (kNumSyncDivs - 1)), 0, kNumSyncDivs - 1); }
inline float syncPos(int index) { return (float)index / (float)(kNumSyncDivs - 1); }   // for programs
inline float freeHz(float rate01) { return 0.01f * std::pow(1000.0f, mpc::clamp(rate01, 0.0f, 1.0f)); }
inline float freePos(float hz) { return std::log(hz / 0.01f) / std::log(1000.0f); }       // for programs

// The head and the room (see the top of the file).
constexpr float kItdSeconds = 0.00066f;     // the far ear hears a source at the side this much later
constexpr float kFarFloor = 0.25f;          // ...and still hears it, round the head, at -12 dB
constexpr float kShadowOct = 2.5f;          // head shadow: far ear low-passed by up to 2.5 octaves
constexpr float kAirOct = 3.0f;             // distance: up to 3 octaves of high cut at Depth 100, far away
constexpr float kFarDb = 18.0f;             // distance: up to 18 dB quieter at Depth 100, far away
constexpr float kMaxPropSeconds = 0.020f;   // Doppler 100: far away is 20 ms later than near
constexpr float kPreNearSeconds = 0.025f;   // room pre-delay for a near source...
constexpr float kPreFarSeconds = 0.003f;    // ...and a far one
// Ear gains are normalised so a centred source comes out at unity in each ear (a centred mono input keeps its
// level). Hard to one side the near ear is +2.2 dB, the far ear -9.9 dB.
constexpr float kEarNorm = 1.0f / (0.70710678f + kFarFloor * (1.0f - 0.70710678f));

// Deterministic random value in -1..1 for step n, so a synced Drift walks the same way each time the song plays.
inline float hashBipolar(int64_t n, uint32_t seed)
{
    uint32_t x = (uint32_t)n * 0x9E3779B1u ^ (uint32_t)(n >> 32) ^ seed;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
}
inline float smoothRandom(double cycles, uint32_t seed)
{
    const double fl = std::floor(cycles);
    const float t = (float)(cycles - fl);
    const float s = t * t * (3.0f - 2.0f * t);
    const int64_t n = (int64_t)fl;
    return lerp(hashBipolar(n, seed), hashBipolar(n + 1, seed), s);
}

// A time-based path at absolute position `cycles` (turns). Step and Scatter are handled by the caller.
inline void pathAt(int path, double cycles, float& u, float& v)
{
    const float p = (float)(cycles - std::floor(cycles));
    const float a = kTwoPi * p;
    switch (path)
    {
        case kCircle:   u = std::sin(a); v = -std::cos(a); break;
        case kFigure8:  u = std::sin(a); v = std::sin(2.0f * a); break;
        case kPendulum: u = std::sin(a); v = 2.0f * u * u - 1.0f; break;
        case kPingPong: u = p < 0.5f ? -1.0f : 1.0f; v = 0.0f; break;
        case kDrift:
            u = 0.75f * smoothRandom(cycles, 0x68E31DA4u) + 0.25f * smoothRandom(cycles * 2.37, 0x1B56C4E9u);
            v = 0.75f * smoothRandom(cycles, 0x2545F491u) + 0.25f * smoothRandom(cycles * 2.37, 0x7A3F9D21u);
            u = clamp(u * 1.3f, -1.0f, 1.0f); v = clamp(v * 1.3f, -1.0f, 1.0f);   // a walk rarely reaches the edges
            break;
        case kTide:     u = 0.0f; v = -std::cos(a); break;
        case kFixed:
        default:        u = 0.0f; v = 0.0f; break;
    }
}

// Soft knee above -3 dBFS so a hard-panned loud source cannot clip harshly. Transparent below 0.7.
inline float softKnee(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.7f) return x;
    const float y = 0.7f + 0.3f * fastTanh((a - 0.7f) * (1.0f / 0.3f));
    return x < 0.0f ? -y : y;
}

// A Schroeder allpass: diffuses the room's input so the tail starts dense instead of as a flutter.
struct Allpass
{
    std::vector<float> buf;
    int len = 1, pos = 0;
    void prepare(int n) { len = n < 1 ? 1 : n; buf.assign((size_t)len, 0.0f); pos = 0; }
    void reset() { for (float& b : buf) b = 0.0f; pos = 0; }
    float process(float x, float g)
    {
        const float d = buf[(size_t)pos];
        const float w = x + g * d;
        buf[(size_t)pos] = flushDenormal(w);
        if (++pos >= len) pos = 0;
        return d - g * w;
    }
};

// The room: a four-line feedback delay network (Hadamard mixing), as in Tidepools, with Size scaling the line
// lengths and the decay. Left input feeds lines 0 and 3, right feeds 1 and 2, so the first reflections come from
// the source's side.
class Room
{
public:
    static constexpr int kLines = 4;
    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        const float scale = sr_ / 44100.0f;
        static constexpr int kLengths[kLines] = { 1931, 2749, 3671, 4493 };
        for (int i = 0; i < kLines; ++i)
        {
            maxLen_[i] = (float)kLengths[i] * scale;
            line_[i].prepare((int)maxLen_[i] + 8);
            damp_[i].setCutoff(6500.0f, sampleRate);
        }
        static constexpr int kDiff[4] = { 142, 379, 107, 277 };
        for (int i = 0; i < 4; ++i) diff_[i].prepare((int)((float)kDiff[i] * scale));
        size_.snap(0.4f);
        reset();
    }
    void reset()
    {
        for (int i = 0; i < kLines; ++i) { line_[i].reset(); damp_[i].reset(); }
        for (Allpass& a : diff_) a.reset();
    }
    // Once per block: Size 0..1 sets the line lengths (glided, so turning it only bends the tail) and the decay.
    void setSize(float size01, int numFrames)
    {
        size_.setTarget(clamp(size01, 0.0f, 1.0f), numFrames);
        const float s = size_.target();
        const float rt60 = 0.35f + 2.4f * s * s;   // seconds
        const float lenScale = 0.25f + 0.75f * s;
        for (int i = 0; i < kLines; ++i)
            fb_[i] = std::pow(10.0f, -3.0f * (maxLen_[i] * lenScale) / (rt60 * sr_));
    }
    void process(float inL, float inR, float& outL, float& outR)
    {
        const float lenScale = 0.25f + 0.75f * size_.next();
        float v[kLines];
        for (int i = 0; i < kLines; ++i) v[i] = line_[i].readLinear(maxLen_[i] * lenScale);
        const float a = v[0] + v[1], b = v[2] + v[3], c = v[0] - v[1], d = v[2] - v[3];
        const float m[kLines] = { 0.5f * (a + b), 0.5f * (c + d), 0.5f * (a - b), 0.5f * (c - d) };
        const float dl = diff_[2].process(diff_[0].process(inL, 0.6f), 0.6f);
        const float dr = diff_[3].process(diff_[1].process(inR, 0.6f), 0.6f);
        const float in[kLines] = { dl, dr, dr, dl };
        for (int i = 0; i < kLines; ++i)
            line_[i].write(flushDenormal(in[i] + damp_[i].process(m[i]) * fb_[i]));
        outL = 0.5f * (v[0] + v[2] - v[3]);
        outR = 0.5f * (v[1] + v[3] - v[2]);
    }

private:
    DelayLine line_[kLines];
    OnePoleLP damp_[kLines];
    Allpass diff_[4];
    float maxLen_[kLines] = {}, fb_[kLines] = {};
    LinearRamp size_;
    float sr_ = 44100.0f;
};

// One sound at a point on the plane. Per ear: a delay tap (propagation + interaural), a one-pole low-pass (air and
// head shadow) and a gain; plus a pre-delayed, panned send to the room. Targets are set every 16 samples and
// ramped towards, so the per-sample work is a few multiplies and three interpolated reads.
struct Emitter
{
    std::vector<float> buf;
    int mask = 0, pos = 0, maxTap = 0;
    float gain[2] = {}, gainInc[2] = {};
    float tap[2] = {}, tapInc[2] = {};
    float coef[2] = { 1.0f, 1.0f }, coefInc[2] = {};
    float lp[2] = {};
    float roomTap = 0.0f, roomTapInc = 0.0f, roomG[2] = {}, roomGInc[2] = {};
    // Slewed delay targets (samples) and the smoothed position.
    float prop = 0.0f, pre = 0.0f, itd[2] = {};
    float u = 0.0f, v = 0.0f;
    float x = 0.0f, y = 0.25f;

    void prepare(int maxTapSamples)
    {
        int size = 16;
        while (size < maxTapSamples + 4) size <<= 1;
        buf.assign((size_t)size, 0.0f);
        mask = size - 1;
        maxTap = maxTapSamples;
        pos = 0;
    }
    void clear()
    {
        for (float& b : buf) b = 0.0f;
        pos = 0;
        lp[0] = lp[1] = 0.0f;
        for (int e = 0; e < 2; ++e) gainInc[e] = tapInc[e] = coefInc[e] = roomGInc[e] = 0.0f;
        roomTapInc = 0.0f;
    }
    float read(float d) const
    {
        d = d < 1.0f ? 1.0f : (d > (float)maxTap ? (float)maxTap : d);
        const int i = (int)d;
        const float f = d - (float)i;
        const float a = buf[(size_t)((pos - i) & mask)], b = buf[(size_t)((pos - i - 1) & mask)];
        return a + (b - a) * f;
    }
    void process(float in, bool withRoom, float& l, float& r, float& rl, float& rr)
    {
        buf[(size_t)pos] = in;
        float o[2];
        for (int e = 0; e < 2; ++e)
        {
            gain[e] += gainInc[e];
            tap[e] += tapInc[e];
            coef[e] += coefInc[e];
            lp[e] = flushDenormal(lp[e] + coef[e] * (read(tap[e]) - lp[e]));
            o[e] = lp[e] * gain[e];
        }
        l += o[0];
        r += o[1];
        roomTap += roomTapInc;
        roomG[0] += roomGInc[0];
        roomG[1] += roomGInc[1];
        if (withRoom)
        {
            const float s = read(roomTap);
            rl += s * roomG[0];
            rr += s * roomG[1];
        }
        pos = (pos + 1) & mask;
    }
};

std::vector<std::string> pathNames()
{
    return {"Fixed", "Circle", "Figure 8", "Pendulum", "Ping-Pong", "Drift", "Tide", "Step", "Scatter"};
}

} // namespace

class Bearings : public mpc::Plugin
{
public:
    Bearings() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("output",    "Output",      -24.0f, 12.0f,   0.0f,   "dB", 1));
        addParam(mpc::floatParam ("mix",       "Mix",         0.0f,   100.0f,  100.0f, "%",  0));
        addParam(mpc::floatParam ("pan",       "Pan",         -100.0f, 100.0f, 0.0f,   "",   0));   // text from formatValue
        addParam(mpc::floatParam ("distance",  "Distance",    0.0f,   100.0f,  25.0f,  "%",  0));
        addParam(mpc::floatParam ("depth",     "Depth",       0.0f,   100.0f,  60.0f,  "%",  0));
        addParam(mpc::floatParam ("doppler",   "Doppler",     0.0f,   100.0f,  25.0f,  "%",  0));
        addParam(mpc::floatParam ("room",      "Room",        0.0f,   100.0f,  20.0f,  "%",  0));
        addParam(mpc::floatParam ("room_size", "Room Size",   0.0f,   100.0f,  40.0f,  "%",  0));
        addParam(mpc::boolParam  ("mono_safe", "Mono Safe",   false));
        addParam(mpc::choiceParam("path",      "Path",        pathNames(), kCircle));
        addParam(mpc::choiceParam("sync",      "Sync",        {"Free", "Sync"}, 1));
        addParam(mpc::floatParam ("rate",      "Rate",        0.0f,   1.0f,    syncPos(3), "", 3));   // text from formatValue
        addParam(mpc::choiceParam("direction", "Direction",   {"Clockwise", "Counter"}, 0));
        addParam(mpc::floatParam ("size_x",    "Size X",      0.0f,   100.0f,  70.0f,  "%",  0));
        addParam(mpc::floatParam ("size_y",    "Size Y",      0.0f,   100.0f,  30.0f,  "%",  0));
        addParam(mpc::floatParam ("phase",     "Phase",       0.0f,   360.0f,  0.0f,   "deg", 0));
        addParam(mpc::logParam   ("glide",     "Glide",       5.0f,   2000.0f, 40.0f,  "",   0));   // text from formatValue
        addParam(mpc::floatParam ("step",      "Step Angle",  15.0f,  180.0f,  90.0f,  "deg", 0));
        addParam(mpc::floatParam ("hit_sens",  "Hit Sens",    0.0f,   100.0f,  50.0f,  "%",  0));
        addParam(mpc::logParam   ("fol_att",   "Fol Attack",  1.0f,   1000.0f, 20.0f,  "ms", 0));
        addParam(mpc::logParam   ("fol_rel",   "Fol Release", 10.0f,  5000.0f, 400.0f, "ms", 0));
        addParam(mpc::floatParam ("fol_gain",  "Fol Gain",    -12.0f, 36.0f,   12.0f,  "dB", 1));
        addParam(mpc::floatParam ("fol_size",  "Fol>Size",    -100.0f, 100.0f, 0.0f,   "",   0));   // text from formatValue
        addParam(mpc::floatParam ("fol_rate",  "Fol>Rate",    -100.0f, 100.0f, 0.0f,   "",   0));
        addParam(mpc::floatParam ("fol_push",  "Fol>Push",    -100.0f, 100.0f, 0.0f,   "",   0));
        addParam(mpc::floatParam ("throw",     "Throw",       0.0f,   100.0f,  0.0f,   "%",  0));
        addParam(mpc::boolParam  ("split",     "Split",       false));
        addParam(mpc::logParam   ("low_x",     "Low Xover",   60.0f,  1000.0f, 200.0f, "", 0));     // text from formatValue
        addParam(mpc::logParam   ("high_x",    "High Xover",  1000.0f, 10000.0f, 2500.0f, "", 0));
        addParam(mpc::floatParam ("low_size",  "Low Size",    0.0f,   100.0f,  0.0f,   "%",  0));
        addParam(mpc::floatParam ("mid_size",  "Mid Size",    0.0f,   100.0f,  100.0f, "%",  0));
        addParam(mpc::floatParam ("high_size", "High Size",   0.0f,   100.0f,  100.0f, "%",  0));
        addParam(mpc::floatParam ("mid_phase", "Mid Phase",   0.0f,   360.0f,  120.0f, "deg", 0));
        addParam(mpc::floatParam ("high_phase","High Phase",  0.0f,   360.0f,  240.0f, "deg", 0));
        auto meter = [this](mpc::Param p) { p.automatable = false; p.saved = false; addParam(p); };
        meter(mpc::floatParam("x_now", "X Now", -100.0f, 100.0f, 0.0f, "", 0));
        meter(mpc::floatParam("y_now", "Y Now", 0.0f, 100.0f, 25.0f, "%", 0));
        for (int r = 0; r < kDotRows; ++r)
        {
            const std::string id = "dot" + std::to_string(r + 1), name = "Dot " + std::to_string(r + 1);
            meter(mpc::floatParam(id.c_str(), name.c_str(), 0.0f, 1.0f, 0.0f, "", 3));
        }

        // Programs: every parameter from its default, then the few each program changes.
        std::vector<float> defaults;
        for (int i = 0; i < numParams(); ++i) defaults.push_back(param(i).def);
        auto prog = [this, &defaults](const char* name, std::initializer_list<std::pair<int, float>> changes)
        {
            std::vector<float> v = defaults;
            for (const auto& c : changes) v[(size_t)c.first] = c.second;
            addProgram(name, v);
        };
        prog("Slow Orbit",    {});
        prog("Centre Stage",  {{kPath, kFixed}, {kDistance, 15}, {kRoom, 10}});
        prog("Halo",          {{kSizeX, 85}, {kSizeY, 60}, {kRate, syncPos(2)}, {kRoom, 30}, {kRoomSize, 55}, {kGlide, 120}});
        prog("Hat Orbit",     {{kRate, syncPos(4)}, {kSizeX, 70}, {kSizeY, 15}, {kDepth, 40}, {kRoom, 8}, {kDistance, 20}});
        prog("Pendulum Keys", {{kPath, kPendulum}, {kRate, syncPos(3)}, {kSizeX, 60}, {kSizeY, 25}, {kRoom, 15}});
        prog("Ping-Pong",     {{kPath, kPingPong}, {kRate, syncPos(6)}, {kSizeX, 80}, {kSizeY, 0}, {kGlide, 20}, {kRoom, 12}});
        prog("Drift Away",    {{kPath, kDrift}, {kSync, 0}, {kRate, freePos(0.08f)}, {kSizeX, 65}, {kSizeY, 55},
                               {kDistance, 40}, {kRoom, 35}, {kRoomSize, 60}, {kGlide, 200}});
        prog("Tide",          {{kPath, kTide}, {kRate, syncPos(3)}, {kSizeY, 95}, {kDistance, 50}, {kDepth, 80},
                               {kRoom, 40}, {kRoomSize, 55}});
        prog("Kick Push",     {{kPath, kFixed}, {kDistance, 10}, {kFolPush, 80}, {kFolAttack, 3}, {kFolRelease, 250},
                               {kRoom, 30}, {kDepth, 75}});
        prog("Step Around",   {{kPath, kStep}, {kStepAngle, 90}, {kSizeX, 80}, {kSizeY, 40}, {kGlide, 60}, {kRoom, 15}});
        prog("Scatter Percs", {{kPath, kScatter}, {kSizeX, 90}, {kSizeY, 50}, {kGlide, 12}, {kRoom, 20}, {kDistance, 30}});
        prog("Prism Pad",     {{kSplit, 1}, {kRate, syncPos(2)}, {kSizeX, 85}, {kSizeY, 50}, {kRoom, 30}, {kRoomSize, 60},
                               {kDistance, 35}, {kGlide, 150}});
        prog("Formation A",   {{kRate, syncPos(3)}, {kSizeX, 80}, {kSizeY, 40}, {kPhase, 0}});
        prog("Formation B",   {{kRate, syncPos(3)}, {kSizeX, 80}, {kSizeY, 40}, {kPhase, 120}});
        prog("Formation C",   {{kRate, syncPos(3)}, {kSizeX, 80}, {kSizeY, 40}, {kPhase, 240}});
        prog("Far Away",      {{kPath, kFixed}, {kDistance, 85}, {kDepth, 80}, {kRoom, 50}, {kRoomSize, 65}});
        prog("Up Close",      {{kPath, kFixed}, {kDistance, 0}, {kRoom, 5}, {kDoppler, 0}});
        prog("Swell Orbit",   {{kRate, syncPos(3)}, {kSizeX, 35}, {kSizeY, 20}, {kFolSize, 90}, {kFolAttack, 60},
                               {kFolRelease, 900}, {kRoom, 20}});
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = (float)sampleRate;
        const int maxTap = (int)std::ceil((kMaxPropSeconds + kPreNearSeconds + kItdSeconds + 0.002f) * sr_);
        for (Emitter& e : em_) e.prepare(maxTap);
        room_.prepare(sampleRate);
        powCoef_ = onePoleCoeff(10.0f, sampleRate);
        earW0_ = kTwoPi * std::fmin(20000.0f, 0.45f * sr_) / sr_;
        earNorm_ = 1.0f / (1.0f - std::exp(-earW0_));
        lastLowX_ = lastHighX_ = -1.0f;
        reset();
    }

    void reset() override
    {
        for (Emitter& e : em_) e.clear();
        for (Biquad& b : xo_) b.reset();
        room_.reset();
        roomTail_ = 0.0f;
        power_ = peak_ = 0.0f; fol_ = 0.0f; fast_ = slow_ = 0.0f; holdoff_ = 0; hits_ = 0;
        stepCur_ = stepTarget_ = 0.0f; throw_ = get(kThrow) * 0.01f;
        cycles_ = 0.0; rateOffset_ = 0.0;
        ctrlCount_ = 0; snap_ = true;
        meterWait_ = 0; lastX_ = lastY_ = -1000.0f;
        for (float& d : lastDot_) d = -1.0f;
        mix_.snap(get(kMix) * 0.01f);
        out_.snap(dbToGain(get(kOutput)));
        roomAmt_.snap(get(kRoom) * 0.01f);
        splitWas_ = getBool(kSplit);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const int nch = mpc::clamp(ctx.numOutputs, 0, 2);
        if (nch == 0) return;
        const bool haveIn = ctx.inputs != nullptr && ctx.numInputs > 0;

        // ---- per-block reads ------------------------------------------------------------------------------
        const bool split = getBool(kSplit);
        if (split != splitWas_)
        {
            // Bands on or off: start the newly used emitters (and the crossover) clean.
            for (int b = 1; b < kMaxEmitters; ++b) em_[b].clear();
            for (Biquad& q : xo_) q.reset();
            em_[0].u = em_[0].v = 0.0f;
            splitWas_ = split;
        }
        numEm_ = split ? 3 : 1;
        if (split) updateCrossover();

        mix_.setTarget(get(kMix) * 0.01f, n);
        out_.setTarget(dbToGain(get(kOutput)), n);
        roomAmt_.setTarget(get(kRoom) * 0.01f, n);
        room_.setSize(get(kRoomSize) * 0.01f, n);
        const bool roomOn = roomAmt_.target() > 0.0f || roomAmt_.value() > 0.0f || roomTail_ > 1.0e-5f;

        updateClock(ctx);

        const float* inL = haveIn ? ctx.inputs[0] : nullptr;
        const float* inR = haveIn ? ctx.inputs[ctx.numInputs > 1 ? 1 : 0] : nullptr;
        float* outL = ctx.outputs[0];
        float* outR = nch > 1 ? ctx.outputs[1] : nullptr;
        float tail = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            if (ctrlCount_ == 0) controlTick();
            if (++ctrlCount_ >= kCtrl) ctrlCount_ = 0;

            const float xl = (inL != nullptr && inL[i] == inL[i]) ? inL[i] : 0.0f;
            const float xr = (inR != nullptr && inR[i] == inR[i]) ? inR[i] : xl;
            const float m = 0.5f * (xl + xr);

            // Follower and hit detector listen to the mono input.
            power_ += powCoef_ * (m * m - power_);
            const float pk = std::fabs(m);
            if (pk > peak_) peak_ = pk;

            float band[kMaxEmitters] = { m, 0.0f, 0.0f };
            if (split)
            {
                const float low = xo_[4].process(xo_[1].process(xo_[0].process(m)));   // LR4 low, allpassed at f2
                const float rest = xo_[3].process(xo_[2].process(m));
                band[0] = low;
                band[1] = xo_[6].process(xo_[5].process(rest));
                band[2] = xo_[8].process(xo_[7].process(rest));
            }

            float wl = 0.0f, wr = 0.0f, sl = 0.0f, sr = 0.0f;
            for (int b = 0; b < numEm_; ++b) em_[b].process(band[b], roomOn, wl, wr, sl, sr);

            const float ra = roomAmt_.next();
            if (roomOn)
            {
                float rl, rr;
                room_.process(sl * ra, sr * ra, rl, rr);
                wl += rl; wr += rr;
                tail = std::fmax(tail, std::fmax(std::fabs(rl), std::fabs(rr)));
            }

            const float mix = mix_.next();
            const float og = out_.next();
            float yl = xl + (softKnee(wl) - xl) * mix;
            float yr = xr + (softKnee(wr) - xr) * mix;
            yl *= og; yr *= og;
            if (!std::isfinite(yl) || !std::isfinite(yr)) { yl = yr = 0.0f; for (Emitter& e : em_) e.clear(); room_.reset(); }
            if (outR != nullptr) { outL[i] = yl; outR[i] = yr; }
            else outL[i] = 0.5f * (yl + yr);
        }
        roomTail_ = roomOn ? tail : 0.0f;

        // Meters, at most every ~30 ms and only when they have moved a step.
        meterWait_ -= n;
        if (meterWait_ <= 0)
        {
            meterWait_ = (int)(0.03f * sr_);
            const Emitter& e = em_[split ? 1 : 0];
            sendMeter(kXNow, std::round(e.x * 64.0f) / 64.0f * 100.0f, lastX_);
            sendMeter(kYNow, std::round(e.y * 64.0f) / 64.0f * 100.0f, lastY_);
            const int row = mpc::clamp((int)(e.y * kDotRows), 0, kDotRows - 1);
            const int col = mpc::clamp((int)std::lround((e.x + 1.0f) * 0.5f * (kDotCols - 1)), 0, kDotCols - 1);
            for (int r = 0; r < kDotRows; ++r)
                sendMeter(kDot0 + r, r == row ? (float)(col + 1) / (float)kDotCols : 0.0f, lastDot_[r]);
        }
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case kPan: case kXNow:
                if (std::fabs(v) < 0.5f) out = "C";
                else { std::snprintf(b, sizeof(b), "%s %.0f", v < 0.0f ? "L" : "R", (double)std::fabs(v)); out = b; }
                return true;
            case kRate:
                if (getInt(kSync) == 1) out = kSyncDivs[syncIndex(v)].name;
                else { const float hz = freeHz(v); std::snprintf(b, sizeof(b), hz < 1.0f ? "%.3f Hz" : "%.2f Hz", (double)hz); out = b; }
                return true;
            case kGlide:
                if (v >= 1000.0f) { std::snprintf(b, sizeof(b), "%.2f s", (double)(v * 0.001f)); out = b; }
                else                 { std::snprintf(b, sizeof(b), "%.0f ms", (double)v); out = b; }
                return true;
            case kLowXover: case kHighXover:
                if (v >= 1000.0f) std::snprintf(b, sizeof(b), "%.2f kHz", (double)(v * 0.001f));
                else              std::snprintf(b, sizeof(b), "%.0f Hz", (double)v);
                out = b; return true;
            case kFolSize: case kFolRate: case kFolPush:
                if (std::fabs(v) < 0.5f) out = "0 %";
                else { std::snprintf(b, sizeof(b), "%+.0f %%", (double)v); out = b; }
                return true;
            default: return false;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Bearings";
        info.vendor       = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;           // 1.0.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 4 * 48000;      // the room rings for up to ~3 s
        return info;
    }
    static constexpr char kUid[5] = "GDHt";

    void sendMeter(int index, float plain, float& last)
    {
        if (plain == last) return;
        last = plain;
        set(index, plain);
        notifyHostParameterChanged(index);
    }

    // Linkwitz-Riley 4th order: two Butterworth sections per side. The low band also passes an allpass at the
    // upper crossover, so low + mid + high has a flat magnitude (the phase of a single LR4 split, twice).
    void updateCrossover()
    {
        const float f1 = clamp(get(kLowXover), 40.0f, 0.2f * sr_);
        const float f2 = clamp(std::fmax(get(kHighXover), f1 * 1.5f), 60.0f, 0.4f * sr_);
        if (f1 == lastLowX_ && f2 == lastHighX_) return;
        lastLowX_ = f1; lastHighX_ = f2;
        const double sr = sr_;
        const BiquadCoeffs lp1 = BiquadCoeffs::lowpass(sr, f1), hp1 = BiquadCoeffs::highpass(sr, f1);
        const BiquadCoeffs lp2 = BiquadCoeffs::lowpass(sr, f2), hp2 = BiquadCoeffs::highpass(sr, f2);
        xo_[0].setCoeffs(lp1); xo_[1].setCoeffs(lp1);
        xo_[2].setCoeffs(hp1); xo_[3].setCoeffs(hp1);
        xo_[4].setCoeffs(BiquadCoeffs::allpass(sr, f2, 0.70710678f));
        xo_[5].setCoeffs(lp2); xo_[6].setCoeffs(lp2);
        xo_[7].setCoeffs(hp2); xo_[8].setCoeffs(hp2);
    }

    // Where the path is: locked to the song position while synced and playing, free-running otherwise.
    void updateClock(const mpc::ProcessContext& ctx)
    {
        const float rate = get(kRate);
        if (getInt(kSync) == 1)
        {
            const float beats = kSyncDivs[syncIndex(rate)].beats;
            inc_ = (double)beatsToHz(beats, ctx.transport.bpm) / sr_;
            if (ctx.transport.playing && ctx.transport.ppqValid)
            {
                cycles_ = ctx.transport.ppqPos / (double)beats;
                ctrlCount_ = 0;   // start a control tick on the block boundary so the phase lands exactly
            }
        }
        else
            inc_ = (double)freeHz(rate) / sr_;
    }

    // Everything that moves, once every kCtrl samples.
    void controlTick()
    {
        const float dt = (float)kCtrl;

        // Follower: RMS -> dB -> 0..1 over a 36 dB window, then attack / release.
        const float amp = std::sqrt(power_ > 0.0f ? power_ : 0.0f);
        const float db = gainToDb(amp, -120.0f) + get(kFolGain);
        const float target = clamp((db + 36.0f) * (1.0f / 36.0f), 0.0f, 1.0f);
        const float att = 1.0f - std::exp(-dt / (get(kFolAttack) * 0.001f * sr_));
        const float rel = 1.0f - std::exp(-dt / (get(kFolRelease) * 0.001f * sr_));
        fol_ += (target > fol_ ? att : rel) * (target - fol_);

        // Hit detector (Step, Scatter): a fast peak follower jumping well above a slow one.
        const float pk = peak_; peak_ = 0.0f;
        const float sens = get(kHitSens) * 0.01f;
        fast_ += (pk > fast_ ? 0.7f : 0.06f) * (pk - fast_);
        slow_ += (fast_ > slow_ ? 0.012f : 0.004f) * (fast_ - slow_);
        const float ratio = 1.3f + 2.7f * (1.0f - sens);
        const float floorAmp = dbToGain(-54.0f + 24.0f * (1.0f - sens));
        if (holdoff_ > 0) --holdoff_;
        if (holdoff_ == 0 && fast_ > floorAmp && fast_ > slow_ * ratio)
        {
            ++hits_;
            stepTarget_ += get(kStepAngle) / 360.0f;   // read at the hit, so turning Step Angle moves later hits only
            holdoff_ = (int)(0.05f * sr_ / dt);   // 50 ms before the next hit counts
        }

        // Throw: out in ~30 ms, back over ~1.5 s, so a quick flick of the Q-Link flings it and lets it drift home.
        const float th = get(kThrow) * 0.01f;
        throw_ += (th > throw_ ? 1.0f - std::exp(-dt / (0.03f * sr_)) : 1.0f - std::exp(-dt / (0.5f * sr_))) * (th - throw_);

        // Clock: time-based paths advance; Fol>Rate adds phase on top.
        const double cyc = cycles_ + rateOffset_ + get(kPhase) / 360.0;
        cycles_ += inc_ * kCtrl;
        if (cycles_ > 1.0e7) cycles_ -= 1.0e7;
        rateOffset_ += inc_ * kCtrl * 2.0 * (double)(get(kFolRate) * 0.01f * fol_);
        if (rateOffset_ > 1.0e6 || rateOffset_ < -1.0e6) rateOffset_ = 0.0;

        const int path = getInt(kPath);
        const float glideMs = get(kGlide);
        const float glide = snap_ ? 1.0f : 1.0f - std::exp(-dt / (std::fmax(glideMs, 5.0f) * 0.001f * sr_));
        if (path == kStep) stepCur_ += glide * (stepTarget_ - stepCur_);
        else stepCur_ = stepTarget_;   // so switching to Step starts where it is
        if (stepCur_ > 1000.0f) { stepCur_ -= 1000.0f; stepTarget_ -= 1000.0f; }   // whole turns: keep float precision

        const float mirror = getInt(kDirection) == 1 ? -1.0f : 1.0f;
        const float sizeMul = std::fmax(0.0f, 1.0f + get(kFolSize) * 0.01f * fol_) * (1.0f + throw_);
        const float push = get(kFolPush) * 0.01f * fol_ * 0.6f + 0.5f * throw_;
        const float sx = get(kSizeX) * 0.01f, sy = get(kSizeY) * 0.01f;
        const float panC = get(kPan) * 0.01f, distC = get(kDistance) * 0.01f;
        const bool split = numEm_ > 1;
        const float bandSize[kMaxEmitters] = { get(kLowSize) * 0.01f, get(kMidSize) * 0.01f, get(kHighSize) * 0.01f };
        const float bandPhase[kMaxEmitters] = { 0.0f, get(kMidPhase) / 360.0f, get(kHighPhase) / 360.0f };

        for (int b = 0; b < numEm_; ++b)
        {
            Emitter& e = em_[b];
            const float bs = split ? bandSize[b] : 1.0f;
            const bool still = bs <= 0.0f;   // a band at Size 0 stays put (and needs no path maths)
            const float offset = get(kPhase) / 360.0f + (split ? bandPhase[b] : 0.0f);
            const double bc = cyc + (split ? (double)bandPhase[b] : 0.0);
            float tu = 0.0f, tv = 0.0f;
            if (!still)
            {
                if (path == kStep)
                {
                    const float a = kTwoPi * (stepCur_ + offset);   // Phase (and a band's phase) still offsets it
                    tu = std::sin(a); tv = -std::cos(a);
                }
                else if (path == kScatter)
                {
                    const uint32_t seed = 0xA511E9B3u + 0x2F1Bu * (uint32_t)b;
                    tu = hashBipolar(hits_, seed);
                    tv = hashBipolar(hits_, seed ^ 0x5bd1e995u);
                }
                else pathAt(path, bc, tu, tv);
                tu *= mirror;
            }
            const float g = path == kStep ? 1.0f : glide;   // Step glides round the circle, not across it
            e.u += g * (tu - e.u);
            e.v += g * (tv - e.v);

            e.x = clamp(panC + sx * sizeMul * bs * e.u, -1.0f, 1.0f);
            e.y = clamp(distC + 0.5f * sy * sizeMul * bs * e.v + push, 0.0f, 1.0f);
            aim(e);
        }
        snap_ = false;
    }

    // Turn an emitter's (x, y) into ear gains, delays, filters and a room send, reached over the next kCtrl samples.
    void aim(Emitter& e)
    {
        const float invN = 1.0f / (float)kCtrl;
        const float depth = get(kDepth) * 0.01f;
        const float dop = get(kDoppler) * 0.01f;
        const bool monoSafe = getBool(kMonoSafe);

        // Distance: level, air, propagation delay (slew-limited: at most 0.4..4.4 % pitch, so a jump glides).
        const float distGain = dbToGain(-kFarDb * depth * e.y);
        const float airOct = kAirOct * depth * e.y;
        const float propT = dop * kMaxPropSeconds * sr_ * e.y;
        const float preT = (kPreFarSeconds + (kPreNearSeconds - kPreFarSeconds) * (1.0f - e.y)) * sr_;
        if (snap_) { e.prop = propT; e.pre = preT; }
        else
        {
            const float maxStep = (float)kCtrl * (0.004f + 0.04f * dop);
            e.prop += clamp(propT - e.prop, -maxStep, maxStep);
            const float maxPre = (float)kCtrl * 0.05f;
            e.pre += clamp(preT - e.pre, -maxPre, maxPre);
        }

        float g[2], d[2], c[2], rg[2];
        for (int ear = 0; ear < 2; ++ear)
        {
            const float p = ear == 0 ? -e.x : e.x;                    // 1 = the source is at this ear, -1 = the other
            const float far = std::fmax(0.0f, -p);
            const float cp = std::cos(0.78539816f * (1.0f - p));      // constant-power pan
            g[ear] = (cp + kFarFloor * (1.0f - cp)) * kEarNorm * distGain;
            const float oct = airOct + kShadowOct * far;
            c[ear] = oct < 1.0e-4f ? 1.0f : (1.0f - std::exp(-earW0_ * std::exp2(-oct))) * earNorm_;
            // Interaural delay, slew-limited like the propagation delay (5 % pitch at most), so a jump from one
            // side to the other cannot click however short Glide is.
            const float itdT = monoSafe ? 0.0f : kItdSeconds * sr_ * far;
            if (snap_) e.itd[ear] = itdT;
            else { const float maxItd = (float)kCtrl * 0.05f; e.itd[ear] += clamp(itdT - e.itd[ear], -maxItd, maxItd); }
            d[ear] = 1.0f + e.prop + e.itd[ear];
            rg[ear] = cp * (0.7f + 0.3f * e.y) * 2.0f;                 // room send rises with distance
        }
        const float rt = 1.0f + e.prop + e.pre;

        if (snap_)
        {
            for (int ear = 0; ear < 2; ++ear)
            {
                e.gain[ear] = g[ear]; e.tap[ear] = d[ear]; e.coef[ear] = c[ear]; e.roomG[ear] = rg[ear];
                e.gainInc[ear] = e.tapInc[ear] = e.coefInc[ear] = e.roomGInc[ear] = 0.0f;
            }
            e.roomTap = rt; e.roomTapInc = 0.0f;
            return;
        }
        for (int ear = 0; ear < 2; ++ear)
        {
            e.gainInc[ear] = (g[ear] - e.gain[ear]) * invN;
            e.tapInc[ear] = (d[ear] - e.tap[ear]) * invN;
            e.coefInc[ear] = (c[ear] - e.coef[ear]) * invN;
            e.roomGInc[ear] = (rg[ear] - e.roomG[ear]) * invN;
        }
        e.roomTapInc = (rt - e.roomTap) * invN;
    }

    Emitter em_[kMaxEmitters];
    int numEm_ = 1;
    Biquad xo_[9];
    float lastLowX_ = -1.0f, lastHighX_ = -1.0f;
    bool splitWas_ = false;
    Room room_;
    float roomTail_ = 0.0f;
    float sr_ = 44100.0f;
    float earW0_ = 1.0f, earNorm_ = 1.0f;
    float powCoef_ = 0.0f, power_ = 0.0f, peak_ = 0.0f;
    float fol_ = 0.0f, fast_ = 0.0f, slow_ = 0.0f;
    int holdoff_ = 0;
    int64_t hits_ = 0;
    float stepCur_ = 0.0f, stepTarget_ = 0.0f, throw_ = 0.0f;
    double cycles_ = 0.0, inc_ = 0.0, rateOffset_ = 0.0;
    int ctrlCount_ = 0;
    bool snap_ = true;
    int meterWait_ = 0;
    float lastX_ = -1000.0f, lastY_ = -1000.0f, lastDot_[kDotRows] = {};
    LinearRamp mix_, out_, roomAmt_;
};

MPC_PLUGIN_ENTRY(Bearings)
