// Galactic - a huge, dark, shimmering ambient reverb for the MPC, by Groundswell.
//
// A port of Airwindows Galactic by Chris Johnson (MIT licence, see README.md),
// moved onto the MPC Plugin SDK framework. The reverb algorithm is unchanged;
// the changes are listed under "Port notes" below.
//
// SPDX-License-Identifier: MIT
//
// Signal flow (per channel, the two channels cross-feed each other):
//
//   in -> x attenuate -> vibrato pre-delay (256 samples, swept by a slow sine,
//         L and R 90 degrees apart: this is the "Detune" shimmer)
//      -> one-pole lowpass ("Brightness")
//      -> block 1: 4 delays (3407/1823/859/331 x size)  -+
//      -> block 2: 4 delays (4801/2909/1153/461 x size)  |  each block mixes its four
//      -> block 3: 4 delays (7607/4217/2269/1597 x size) |  outputs with a -1 matrix
//      -> sum of block 3 / 8 = reverb                   -+  (a feedback delay network)
//         block 3 outputs, x regen, feed back into block 1 of the OTHER channel
//      -> one-pole lowpass ("Brightness" again)
//      -> dry/wet mix -> output gain
//
// At sample rates above ~88 kHz the network runs once every 2-4 samples and the
// output is interpolated between runs, so the sound (and CPU) stays the same as at 44.1k.
//
// Port notes (what differs from upstream Airwindows Galactic):
//   * Delay memory is float rather than double (half the memory, ~0.5 MB per instance).
//   * The vibrato sine is a rotating phasor (no sin() per sample) - same shape and speed.
//   * Airwindows' 32-bit floating point dither on the output is left out; the MPC mixes
//     in float anyway. The denormal-avoidance noise is left out because the framework
//     flushes denormals to zero.
//   * New: an Output level (parameter 0, the host nudges it on load) and smoothing on
//     Mix and Output so moving them never zippers.
//
// Parameters (ids are permanent once released - never rename or reorder):
//   0 Output   dB   -24..+6, 0     output level after the mix
//   1 Replace  %    0..100,  50    how fast new sound replaces the tail: low = long decay,
//                                  0 = freeze (input is shut off and the tail holds)
//   2 Bright   %    0..100,  50    lowpass before and after the tank: low = dark, 100 = open
//   3 Detune   %    0..100,  50    vibrato speed of the pre-delay: the shimmer / pitch drift
//   4 Bigness  %    0..100,  100   size of all twelve delays
//   5 Mix      %    0..100,  100   dry/wet on Airwindows' curve (wet = 1-(1-mix)^3), so the
//                                  reverb is already loud at 30-40%

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/smoother.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

using namespace mpc::dsp;

namespace {

enum ParamIndex { kOutput = 0, kReplace, kBright, kDetune, kBigness, kMix, kNumParams };

constexpr double kTwoPiD = 6.283185307179586;   // double: the vibrato phase is accumulated in double, as upstream

// One circular delay line with Airwindows' indexing: write at `count`, step,
// read at the new `count`, so the delay is `len + 1` samples. `len` may change
// every block (Bigness); the buffer is sized for the largest it can be.
struct Line
{
    std::vector<float> buf;
    int count = 1;
    int len = 0;

    void allocate(int size) { buf.assign((size_t)size, 0.0f); }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); count = 1; }
    float writeRead(float x)
    {
        buf[(size_t)count] = x;
        ++count; if (count < 0 || count > len) count = 0;
        return buf[(size_t)count];
    }
};

// Buffer sizes and base lengths from upstream, in network order I J K L / A B C D / E F G H.
constexpr int kBufSize[12] = { 6480, 3660, 1720, 680,  9700, 6000, 2320, 940,  15220, 8460, 4540, 3200 };
constexpr double kBaseLen[12] = { 3407, 1823, 859, 331,  4801, 2909, 1153, 461,  7607, 4217, 2269, 1597 };
constexpr int kVibLen = 256;    // upstream delayM
constexpr int kVibBuf = 520;    // room for count + max offset (256 + 255) + 1

struct Channel
{
    Line lines[12];
    float vib[kVibBuf] = {};
    float iirA = 0.0f, iirB = 0.0f;   // the two Brightness lowpasses
    float feedback[4] = {};           // block 3 outputs, sent to the other channel
    float lastRef[7] = {};            // interpolation points for high sample rates

    void allocate() { for (int i = 0; i < 12; ++i) lines[i].allocate(kBufSize[i]); }
    void clear()
    {
        for (Line& l : lines) l.clear();
        std::fill(std::begin(vib), std::end(vib), 0.0f);
        iirA = iirB = 0.0f;
        std::fill(std::begin(feedback), std::end(feedback), 0.0f);
        std::fill(std::begin(lastRef), std::end(lastRef), 0.0f);
    }
};

// One 4x4 block of the network: each output = own line minus the other three.
inline void mixBlock(const float in[4], float out[4])
{
    const float sum = in[0] + in[1] + in[2] + in[3];
    for (int k = 0; k < 4; ++k) out[k] = in[k] - (sum - in[k]);
}

} // namespace

class Galactic : public mpc::Plugin
{
public:
    Galactic() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("output",  "Output",  -24.0f, 6.0f,   0.0f,   "dB", 1));
        addParam(mpc::floatParam("replace", "Replace", 0.0f,   100.0f, 50.0f,  "%",  0));
        addParam(mpc::floatParam("bright",  "Bright",  0.0f,   100.0f, 50.0f,  "%",  0));
        addParam(mpc::floatParam("detune",  "Detune",  0.0f,   100.0f, 50.0f,  "%",  0));
        addParam(mpc::floatParam("bigness", "Bigness", 0.0f,   100.0f, 100.0f, "%",  0));
        addParam(mpc::floatParam("mix",     "Mix",     0.0f,   100.0f, 100.0f, "%",  0));
        if (numParams() != kNumParams) mpc::log("Galactic: parameter count mismatch");

        //                          Output Replace Bright Detune Bigness Mix
        addProgram("Galactic",      { 0.0f, 50.0f, 50.0f, 50.0f, 100.0f, 100.0f });  // upstream defaults, for a return track
        addProgram("Insert Wash",   { 0.0f, 50.0f, 40.0f, 40.0f, 100.0f,  30.0f });  // on a track's insert
        addProgram("Dark Matter",   { 0.0f, 20.0f, 18.0f, 55.0f, 100.0f, 100.0f });  // long and murky
        addProgram("Nebula Shimmer",{ 0.0f, 30.0f, 85.0f, 95.0f, 100.0f, 100.0f });  // bright, wobbling tail
        addProgram("Small Orbit",   { 0.0f, 85.0f, 60.0f, 10.0f,  20.0f,  25.0f });  // short, roomy, on drums
        addProgram("Event Horizon", { 0.0f,  4.0f, 35.0f, 70.0f, 100.0f, 100.0f });  // near-endless drone pad

        for (Channel& c : ch_) c.allocate();   // all memory is allocated here, once
        rng_ = 0x9E3779B9u;
        reset();
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sampleRate_ = sampleRate;
        output_.setTime(30.0f, sampleRate);
        wet_.setTime(30.0f, sampleRate);
    }

    void reset() override
    {
        for (Channel& c : ch_) c.clear();
        cycle_ = 0;
        vibCount_ = 1;
        // Upstream start values: phase 3.0 and a deliberately huge first speed, so with any
        // Detune the first sample wraps to phase 0 and draws a real speed; with Detune at 0
        // the taps stay put at sin(3.0) / cos(3.0).
        vibPhase_ = 3.0;
        vibSin_ = std::sin(3.0); vibCos_ = std::cos(3.0);
        vibSpeed_ = 429496.7295;
        output_.snap(dbToGain(get(kOutput)));
        wet_.snap(wetAmount());
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // ----- per block: parameters -> coefficients (upstream math, A..E = 0..1) -----
        const double overallscale = sampleRate_ / 44100.0;
        int cycleEnd = (int)std::floor(overallscale);
        cycleEnd = mpc::clamp(cycleEnd, 1, 4);
        if (cycle_ > cycleEnd - 1) cycle_ = cycleEnd - 1;

        const double A = get(kReplace) * 0.01, B = get(kBright) * 0.01;
        const double C = get(kDetune) * 0.01, D = get(kBigness) * 0.01;
        const float regen = (float)(0.0625 + (1.0 - A) * 0.0625);
        const float attenuate = (float)((1.0 - (regen / 0.125)) * 1.333);
        const float lowpass = (float)(std::pow(1.00001 - (1.0 - B), 2.0) / std::sqrt(overallscale));
        const double drift = C * C * C * 0.001;
        const double size = D * 1.77 + 0.1;
        for (Channel& c : ch_)
            for (int i = 0; i < 12; ++i) c.lines[i].len = (int)(kBaseLen[i] * size);

        // Vibrato: the phase advances by vibSpeed_ * drift per sample. Rotate a sin/cos
        // pair by that angle instead of calling sin() every sample.
        double step = vibSpeed_ * drift;
        double rotC = std::cos(step), rotS = std::sin(step);

        output_.setTarget(dbToGain(get(kOutput)));
        wet_.setTarget(wetAmount());

        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        Channel& L = ch_[0];
        Channel& R = ch_[1];

        for (int n = 0; n < ctx.numFrames; ++n)
        {
            const float dryL = inL[n], dryR = inR[n];

            // --- vibrato pre-delay ---
            vibPhase_ += step;
            if (vibPhase_ > kTwoPiD)
            {
                vibPhase_ = 0.0; vibSin_ = 0.0; vibCos_ = 1.0;
                nextVibSpeed();                      // upstream picks a new random speed each turn
                step = vibSpeed_ * drift;
                rotC = std::cos(step); rotS = std::sin(step);   // once per vibrato cycle, not per sample
            }
            else
            {
                const double s = vibSin_ * rotC + vibCos_ * rotS;
                vibCos_ = vibCos_ * rotC - vibSin_ * rotS;
                vibSin_ = s;
            }

            L.vib[vibCount_] = dryL * attenuate;
            R.vib[vibCount_] = dryR * attenuate;
            ++vibCount_; if (vibCount_ < 0 || vibCount_ > kVibLen) vibCount_ = 0;

            float sampleL = readVib(L.vib, (vibSin_ + 1.0) * 127.0);   // sin(phase)
            float sampleR = readVib(R.vib, (vibCos_ + 1.0) * 127.0);   // sin(phase + 90 degrees)

            L.iirA += (sampleL - L.iirA) * lowpass; sampleL = L.iirA;
            R.iirA += (sampleR - R.iirA) * lowpass; sampleR = R.iirA;

            // --- the delay network, once every cycleEnd samples ---
            ++cycle_;
            if (cycle_ == cycleEnd)
            {
                // Both channels must read the OTHER channel's feedback from the previous run,
                // so take copies before either run overwrites its own.
                float fbL[4], fbR[4];
                for (int k = 0; k < 4; ++k) { fbL[k] = L.feedback[k]; fbR[k] = R.feedback[k]; }
                const float outLsum = runNetwork(L, sampleL, fbR, regen);
                const float outRsum = runNetwork(R, sampleR, fbL, regen);
                storeRefs(L.lastRef, outLsum, cycleEnd);
                storeRefs(R.lastRef, outRsum, cycleEnd);
                cycle_ = 0;
            }
            sampleL = L.lastRef[cycle_];
            sampleR = R.lastRef[cycle_];

            L.iirB += (sampleL - L.iirB) * lowpass; sampleL = L.iirB;
            R.iirB += (sampleR - R.iirB) * lowpass; sampleR = R.iirB;

            // --- mix and output ---
            const float wet = wet_.process();
            const float gain = output_.process();
            outL[n] = (dryL + (sampleL - dryL) * wet) * gain;
            outR[n] = (dryR + (sampleR - dryR) * wet) * gain;
        }

        // Keep the phasor on the unit circle (float rounding slowly changes its length).
        const double mag = std::sqrt(vibSin_ * vibSin_ + vibCos_ * vibCos_);
        if (mag > 0.0) { vibSin_ /= mag; vibCos_ /= mag; }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Galactic";
        info.vendor       = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;            // 1.0.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 0;               // 0 = unknown: the tail depends on Replace (and is endless at 0)
        return info;
    }

    static constexpr char kUid[5] = "GFVZ";

    // Airwindows' dry/wet curve.
    float wetAmount() const
    {
        const float e = 1.0f - get(kMix) * 0.01f;
        return 1.0f - e * e * e;
    }

    // A fresh vibrato speed in [0.43, 0.69], as upstream derives it from its noise generator.
    void nextVibSpeed()
    {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        vibSpeed_ = 0.4294967295 + (double)rng_ * 0.0000000000618;
    }

    // Linear-interpolated read from the vibrato pre-delay, `offset` samples after the write head.
    float readVib(const float* buf, double offset) const
    {
        const int whole = (int)offset;
        const float frac = (float)(offset - whole);
        int a = vibCount_ + whole;     if (a > kVibLen) a -= kVibLen + 1;
        int b = vibCount_ + whole + 1; if (b > kVibLen) b -= kVibLen + 1;
        return buf[a] * (1.0f - frac) + buf[b] * frac;
    }

    // Three 4x4 blocks; returns the channel's reverb sample and updates its feedback.
    static float runNetwork(Channel& c, float in, const float* otherFeedback, float regen)
    {
        float a[4], b[4];
        for (int k = 0; k < 4; ++k) a[k] = c.lines[k].writeRead(in + otherFeedback[k] * regen);
        mixBlock(a, b);
        for (int k = 0; k < 4; ++k) a[k] = c.lines[4 + k].writeRead(b[k]);
        mixBlock(a, b);
        for (int k = 0; k < 4; ++k) a[k] = c.lines[8 + k].writeRead(b[k]);
        mixBlock(a, c.feedback);
        return (a[0] + a[1] + a[2] + a[3]) * 0.125f;
    }

    // Interpolation points between network runs when it runs every 2-4 samples.
    static void storeRefs(float* r, float x, int cycleEnd)
    {
        switch (cycleEnd)
        {
            case 4: r[0] = r[4]; r[2] = (r[0] + x) * 0.5f; r[1] = (r[0] + r[2]) * 0.5f;
                    r[3] = (r[2] + x) * 0.5f; r[4] = x; break;
            case 3: r[0] = r[3]; r[2] = (r[0] + r[0] + x) / 3.0f; r[1] = (r[0] + x + x) / 3.0f;
                    r[3] = x; break;
            case 2: r[0] = r[2]; r[1] = (r[0] + x) * 0.5f; r[2] = x; break;
            default: r[0] = x; break;
        }
    }

    Channel ch_[2];
    double sampleRate_ = 44100.0;
    int cycle_ = 0;
    int vibCount_ = 1;
    double vibPhase_ = 0.0, vibSin_ = 0.0, vibCos_ = 1.0, vibSpeed_ = 0.43;
    uint32_t rng_ = 1;
    Smoother output_, wet_;
};

MPC_PLUGIN_ENTRY(Galactic)
