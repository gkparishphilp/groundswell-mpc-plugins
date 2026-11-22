// Spring.h - Breakwater's spring reverb: two "springs" (left and right), each a feedback loop
// around a dispersive allpass chain.
//
//   in -> driver (high-pass, soft saturation pushed by Dwell) --+
//                                                               v
//        +-----> delay (34 / 41 ms, slow wobble) -> 40 stretched allpasses -> low-pass (~4.3 kHz)
//        |                                                         -> high-pass -> out
//        +------------------- x feedback (decay set by Dwell) ----------------------+
//                (the two loops are coupled by a small rotation)
//
// The chirp: a "stretched" first-order allpass, H(z) = (a + z^-K) / (1 + a z^-K), delays different
// frequencies by different amounts. Forty in a row smear each echo into the falling/rising "drip"
// a real spring makes, and because the chain sits inside the loop, every echo is smeared further
// than the last, as on a real tank. K is chosen so the dispersion covers 0..~4.3 kHz (the
// spring's transition frequency); the low-pass removes the mirrored copies above it.
// After J. Parker and S. Bilbao / V. Valimaki, J. Parker and J. S. Abel, "Parametric spring
// reverberation effect" (JAES 2010): the structure only, written from scratch.
//
// Cost per spring per sample: 40 multiplies in the chain, one biquad, one interpolated delay read.
// The two springs' chains run interleaved (see chains()).
//
// SPDX-License-Identifier: MIT

#ifndef BREAKWATER_SPRING_H
#define BREAKWATER_SPRING_H

#include "AmpParts.h"
#include "dsp/biquad.h"
#include "dsp/math.h"

#include <cmath>
#include <vector>

namespace bw {

constexpr int kSpringAllpasses = 40;
constexpr int kSpringMaxK = 16;
constexpr float kSpringFc = 4300.0f;

struct SpringLine
{
    // [ring index][section]: one sample's state for every section sits together in memory.
    float xs[kSpringMaxK][kSpringAllpasses] = {};
    float ys[kSpringMaxK][kSpringAllpasses] = {};
    int K = 5, idx = 0;
    float a = 0.6f;

    std::vector<float> buf;               // sized in prepare()
    int mask = 0, wpos = 0;
    float delay = 1500.0f;                // samples
    float modDepth = 0.0f;                // samples
    double modPhase = 0.0, modInc = 0.0;

    mpc::dsp::Biquad lp;
    OnePoleHP hp;
    float last = 0.0f;

    void prepare(float sr, float delayMs, float coeff, float modHz, float phase0)
    {
        K = mpc::dsp::clamp((int)std::lround(sr / (2.0f * kSpringFc)), 1, kSpringMaxK);
        a = coeff;
        delay = delayMs * 0.001f * sr;
        modDepth = 0.0004f * sr;          // +/- 0.4 ms: the spring never sits quite still
        modInc = modHz / sr;
        modPhase = phase0;
        int size = 1;
        while (size < (int)(delay + modDepth) + 256 + 8) size <<= 1;   // room for a whole block
        buf.assign((size_t)size, 0.0f);
        mask = size - 1;
        lp.setCoeffs(mpc::dsp::BiquadCoeffs::lowpass(sr, kSpringFc, 0.6f));
        hp.set(120.0f, sr);
        reset();
    }

    void reset()
    {
        for (auto& row : xs) for (float& v : row) v = 0.0f;
        for (auto& row : ys) for (float& v : row) v = 0.0f;
        for (float& v : buf) v = 0.0f;
        idx = 0; wpos = 0; last = 0.0f;
        lp.reset(); hp.reset();
    }

    // Read the wobbling delay j samples into the block (linear interpolation). Everything a block
    // reads was written before the block started, because the delay is longer than a block.
    float readAhead(int j)
    {
        modPhase += modInc; if (modPhase >= 1.0) modPhase -= 1.0;
        const float d = delay + modDepth * mpc::dsp::fastSin01((float)modPhase);
        const float rp = (float)(wpos + j) - d;
        const int i0 = (int)std::floor(rp);
        const float fr = rp - (float)i0;
        const float s0 = buf[(size_t)(i0 & mask)], s1 = buf[(size_t)((i0 + 1) & mask)];
        return s0 + fr * (s1 - s0);
    }
    void write(int j, float v) { buf[(size_t)((wpos + j) & mask)] = v; }
    void advance(int n) { wpos = (wpos + n) & mask; }

    float post(float x)
    {
        x = lp.process(x);
        x = hp.process(x);
        last = mpc::dsp::flushDenormal(x);
        return last;
    }

    int nextIdx(int i) const { return (i + 1 == K) ? 0 : i + 1; }

    // The dispersive chains of both springs over a block. Each chain is a serial dependency through
    // its 40 sections, which makes it slow on its own. But a section's z^-K means samples j and j+1
    // never touch the same state when K >= 2, so two samples of each spring - four independent
    // chains - run side by side. The result is identical to running one sample at a time.
    static void chains(SpringLine& A, SpringLine& B, float* xa, float* xb, int n)
    {
        const float aa = A.a, ab = B.a;
        int j = 0;
        if (A.K >= 2)
        {
            for (; j + 1 < n; j += 2)
            {
                const int i0 = A.idx, i1 = A.nextIdx(i0);
                float *xa0 = A.xs[i0], *ya0 = A.ys[i0], *xa1 = A.xs[i1], *ya1 = A.ys[i1];
                float *xb0 = B.xs[i0], *yb0 = B.ys[i0], *xb1 = B.xs[i1], *yb1 = B.ys[i1];
                float ua0 = xa[j], ua1 = xa[j + 1], ub0 = xb[j], ub1 = xb[j + 1];
                for (int m = 0; m < kSpringAllpasses; ++m)
                {
                    const float va0 = aa * (ua0 - ya0[m]) + xa0[m];
                    const float va1 = aa * (ua1 - ya1[m]) + xa1[m];
                    const float vb0 = ab * (ub0 - yb0[m]) + xb0[m];
                    const float vb1 = ab * (ub1 - yb1[m]) + xb1[m];
                    xa0[m] = ua0; ya0[m] = va0; ua0 = va0;
                    xa1[m] = ua1; ya1[m] = va1; ua1 = va1;
                    xb0[m] = ub0; yb0[m] = vb0; ub0 = vb0;
                    xb1[m] = ub1; yb1[m] = vb1; ub1 = vb1;
                }
                xa[j] = ua0; xa[j + 1] = ua1; xb[j] = ub0; xb[j + 1] = ub1;
                A.idx = B.idx = A.nextIdx(i1);
            }
        }
        for (; j < n; ++j)
        {
            const int i = A.idx;
            float *xsa = A.xs[i], *ysa = A.ys[i], *xsb = B.xs[i], *ysb = B.ys[i];
            float ua = xa[j], ub = xb[j];
            for (int m = 0; m < kSpringAllpasses; ++m)
            {
                const float va = aa * (ua - ysa[m]) + xsa[m];
                const float vb = ab * (ub - ysb[m]) + xsb[m];
                xsa[m] = ua; ysa[m] = va; ua = va;
                xsb[m] = ub; ysb[m] = vb; ub = vb;
            }
            xa[j] = ua; xb[j] = ub;
            A.idx = B.idx = A.nextIdx(i);
        }
    }

    // Loop length used to turn a decay time into a feedback gain.
    float loopSamples() const { return delay + (float)(kSpringAllpasses * K); }
};

struct SpringReverb
{
    SpringLine s[2];
    OnePoleHP driverHp;
    OnePoleLP toneL, toneR;
    float g = 0.7f;                       // feedback, per spring
    // The two springs feed back through a small rotation (energy preserving, so coupling them does
    // not shorten the decay): cos/sin of about 14 degrees.
    float rc = 0.9703f, rs = 0.2419f;
    float drive = 1.0f, driveNorm = 1.0f;
    float sr = 44100.0f;

    void prepare(float sampleRate)
    {
        sr = sampleRate;
        s[0].prepare(sr, 33.9f, 0.62f, 0.31f, 0.0);
        s[1].prepare(sr, 40.7f, 0.57f, 0.23f, 0.5);
        driverHp.set(220.0f, sr);
        reset();
    }
    void reset() { s[0].reset(); s[1].reset(); driverHp.reset(); toneL.reset(); toneR.reset(); }

    // dwell 0..1: drives the tank harder (more splash) and lengthens the decay (0.7 s .. 4 s).
    // tone 0..1: the wet signal's low-pass, 1.4 kHz .. 9 kHz.
    void set(float dwell, float tone)
    {
        const float rt = 0.7f * std::pow(4.0f / 0.7f, dwell);
        g = std::pow(10.0f, -3.0f * s[0].loopSamples() / (rt * sr));
        g = mpc::dsp::clamp(g, 0.0f, 0.97f);
        // The driver: unity gain for small signals, but pushed harder into saturation as Dwell
        // rises, so big hits splash and compress in the tank instead of just getting louder.
        drive = 1.0f + 5.0f * dwell * dwell;
        driveNorm = 1.0f / drive;
        const float hz = 1400.0f * std::pow(9000.0f / 1400.0f, tone);
        toneL.set(hz, sr); toneR.set(hz, sr);
    }

    static constexpr int kBlock = 256;    // shorter than the shortest delay at any sample rate
    float bl[kBlock], br[kBlock];

    // n <= kBlock. in: the amp's mono output; out: the two springs (wet only).
    void processBlock(const float* in, float* outL, float* outR, int n)
    {
        for (int j = 0; j < n; ++j) { bl[j] = s[0].readAhead(j); br[j] = s[1].readAhead(j); }
        SpringLine::chains(s[0], s[1], bl, br, n);
        for (int j = 0; j < n; ++j)
        {
            // This sample's delay input uses the previous sample's outputs, as in a per-sample loop.
            const float x = fastTanh(drive * driverHp.process(in[j])) * driveNorm;
            const float l0 = s[0].last, r0 = s[1].last;
            s[0].write(j, x + g * (rc * l0 + rs * r0));
            s[1].write(j, x + g * (rc * r0 - rs * l0));
            outL[j] = toneL.process(s[0].post(bl[j]));
            outR[j] = toneR.process(s[1].post(br[j]));
        }
        s[0].advance(n); s[1].advance(n);
    }
};

} // namespace bw

#endif // BREAKWATER_SPRING_H
