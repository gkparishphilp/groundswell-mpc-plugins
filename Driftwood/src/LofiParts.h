// LofiParts.h - Driftwood's output stage: chorus, a small room, tape wobble and dust.
// No allocation outside prepare(), no transcendental functions per sample.
//
// SPDX-License-Identifier: MIT

#ifndef DRIFTWOOD_LOFIPARTS_H
#define DRIFTWOOD_LOFIPARTS_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace driftwood {

constexpr float kTwoPi = 6.28318530718f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// sin(2*pi*ph), parabolic approximation with one correction step (error < 0.1 %).
inline float fastSin(float ph)
{
    ph -= std::floor(ph);
    const float t = 2.0f * ph - 1.0f;
    float y = 4.0f * t * (1.0f - std::fabs(t));
    y = 0.225f * (y * std::fabs(y) - y) + y;
    return -y;
}

struct Rng
{
    uint32_t s = 0x2545F491u;
    float next() { uint32_t x = s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s = x; return (float)(x >> 8) * (1.0f / 16777216.0f); }
    float bi() { return next() * 2.0f - 1.0f; }
};

// A power-of-two delay line with a linearly interpolated read.
struct Delay
{
    std::vector<float> b;
    int mask = 0, w = 0;
    void alloc(int minLen) { int n = 1; while (n < minLen) n <<= 1; b.assign((size_t)n, 0.0f); mask = n - 1; w = 0; }
    void clear() { std::fill(b.begin(), b.end(), 0.0f); w = 0; }
    void push(float x) { b[(size_t)w] = x; w = (w + 1) & mask; }
    // d samples behind the most recent push (d >= 1).
    float read(float d) const
    {
        float rp = (float)w - d;
        int i0 = (int)std::floor(rp);
        const float fr = rp - (float)i0;
        const float a = b[(size_t)(i0 & mask)], c = b[(size_t)((i0 + 1) & mask)];
        return a + fr * (c - a);
    }
    float tap(int d) const { return b[(size_t)((w - d) & mask)]; }
};

// Stereo chorus: one modulated delay per side, the two LFOs a quarter cycle apart.
struct Chorus
{
    Delay dl, dr;
    float ph = 0.0f, sr = 44100.0f;
    void prepare(float sampleRate) { sr = sampleRate; dl.alloc((int)(0.04f * sr) + 4); dr.alloc((int)(0.04f * sr) + 4); }
    void reset() { dl.clear(); dr.clear(); ph = 0.0f; }
    // amt 0..1: wet level and depth.
    void process(float& l, float& r, float amt, float inc)
    {
        dl.push(l); dr.push(r);
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
        const float base = 0.009f * sr, dep = 0.0035f * sr;
        const float wl = dl.read(base + dep * fastSin(ph));
        const float wr = dr.read(base + dep * fastSin(ph + 0.25f));
        const float wet = 0.55f * amt, dry = 1.0f - 0.25f * amt;
        l = dry * l + wet * wl;
        r = dry * r + wet * wr;
    }
};

// A small room: 4 delay lines (23..42 ms) in a Hadamard feedback network, damped by a one-pole low
// pass in each loop. T60 about 0.8 s: enough to put the instrument in a space, not a hall.
struct Room
{
    Delay d[4];
    int len[4] = {1031, 1327, 1523, 1871};
    float lp[4] = {0, 0, 0, 0};
    void prepare(float sr)
    {
        static const int kLen[4] = {1031, 1327, 1523, 1871};
        for (int i = 0; i < 4; ++i) { len[i] = (int)((float)kLen[i] * sr / 44100.0f); d[i].alloc(len[i] + 2); }
    }
    void reset() { for (int i = 0; i < 4; ++i) { d[i].clear(); lp[i] = 0.0f; } }
    void process(float& l, float& r, float amt)
    {
        float y[4];
        for (int i = 0; i < 4; ++i)
        {
            const float x = d[i].tap(len[i]);
            lp[i] += 0.45f * (x - lp[i]);
            y[i] = lp[i];
        }
        const float in = 0.35f * (l + r);
        const float g = 0.5f * 0.80f;   // Hadamard normalisation x loop gain
        const float h0 = (y[0] + y[1] + y[2] + y[3]) * g, h1 = (y[0] - y[1] + y[2] - y[3]) * g;
        const float h2 = (y[0] + y[1] - y[2] - y[3]) * g, h3 = (y[0] - y[1] - y[2] + y[3]) * g;
        d[0].push(h0 + in); d[1].push(h1 + in); d[2].push(h2 - in); d[3].push(h3 - in);
        const float wet = 0.55f * amt, dry = 1.0f - 0.2f * amt;
        l = dry * l + wet * (y[0] + 0.6f * y[2]);
        r = dry * r + wet * (y[1] + 0.6f * y[3]);
    }
};

// Tape wobble: a modulated delay shared by both sides (the whole tape slows down together).
// Wow 0.5 Hz with a slowly wandering rate and depth, flutter 6.5 Hz. At 100 % the wow swings the
// pitch about +-10 cents. At 0 the delay is a fixed 1 sample, so nothing changes.
struct Wobble
{
    Delay dl, dr;
    float wowPh = 0.0f, flutPh = 0.0f, wander = 0.0f, wanderT = 0.0f, sr = 44100.0f;
    int wanderCount = 0;
    Rng rng;
    void prepare(float sampleRate) { sr = sampleRate; dl.alloc((int)(0.02f * sr) + 8); dr.alloc((int)(0.02f * sr) + 8); }
    void reset() { dl.clear(); dr.clear(); wowPh = flutPh = wander = wanderT = 0.0f; wanderCount = 0; rng.s = 0x1234567u; }
    // Called once per chunk: returns per-sample increments and depths in samples.
    void process(float* L, float* R, int n, float amt)
    {
        if (--wanderCount <= 0) { wanderCount = (int)(0.25f * sr / 32.0f); wanderT = rng.bi(); }
        wander += (wanderT - wander) * 0.02f;
        const float wowHz = 0.5f * (1.0f + 0.3f * wander);
        const float wowA = amt * (1.0f + 0.35f * wander) * sr * 0.0058f / (kTwoPi * wowHz);   // +-0.58 % speed
        const float flutA = amt * sr * 0.0012f / (kTwoPi * 6.5f);                            // +-0.12 % speed
        const float wInc = wowHz / sr, fInc = 6.5f / sr;
        for (int i = 0; i < n; ++i)
        {
            dl.push(L[i]); dr.push(R[i]);
            wowPh += wInc; if (wowPh >= 1.0f) wowPh -= 1.0f;
            flutPh += fInc; if (flutPh >= 1.0f) flutPh -= 1.0f;
            const float d = 1.0f + wowA * (1.0f + fastSin(wowPh)) + flutA * (1.0f + fastSin(flutPh));
            L[i] = dl.read(d); R[i] = dr.read(d);
        }
    }
};

// Dust: record crackle and tape hiss that ride on the sound. A slow follower of the instrument's
// level fades the dust in with the notes and out about two seconds after the last tail, so an idle
// Driftwood is silent. Dust also rolls off the top end a little (a worn medium).
struct Dust
{
    Rng rng;
    float follow = 0.0f, click = 0.0f, clickPan = 0.0f, hissL = 0.0f, hissR = 0.0f, hpL = 0.0f, hpR = 0.0f;
    float lpL = 0.0f, lpR = 0.0f;
    float relCoef = 0.9999f, attCoef = 0.99f, clickCoef = 0.6f, sr = 44100.0f;
    void prepare(float sampleRate)
    {
        sr = sampleRate;
        relCoef = std::exp(-1.0f / (0.7f * sr));
        attCoef = std::exp(-1.0f / (0.01f * sr));
        clickCoef = std::exp(-1.0f / (0.00012f * sr));
    }
    void reset() { follow = click = hissL = hissR = hpL = hpR = lpL = lpR = 0.0f; rng.s = 0x9E3779B9u; }
    void process(float& l, float& r, float amt, float rollA)
    {
        // Top-end roll-off (rollA = 1 means none).
        lpL += rollA * (l - lpL); lpR += rollA * (r - lpR);
        l = lpL; r = lpR;
        const float lev = std::fabs(l) + std::fabs(r);
        follow = lev > follow ? lev + attCoef * (follow - lev) : lev + relCoef * (follow - lev);
        const float gate = clampf(follow * 25.0f, 0.0f, 1.0f);
        if (amt <= 0.0f || gate < 1.0e-4f) { click *= clickCoef; return; }
        // Crackle: 2..40 pops a second, mostly tiny, a few loud.
        const float rate = 2.0f + 38.0f * amt * amt;
        if (rng.next() < rate / sr)
        {
            const float a = rng.next();
            click += (rng.next() < 0.5f ? -1.0f : 1.0f) * (0.02f + 0.25f * a * a * a) * amt;
            clickPan = rng.bi() * 0.6f;
        }
        click *= clickCoef;
        // Hiss: white noise, low-passed around 5 kHz.
        hissL += 0.5f * (rng.bi() - hissL);
        hissR += 0.5f * (rng.bi() - hissR);
        const float hiss = 0.006f * amt;
        const float cl = click * (1.0f - clickPan), cr = click * (1.0f + clickPan);
        // Pops through a differentiator so they are clicks, not thumps.
        const float pl = cl - hpL, pr = cr - hpR; hpL = cl; hpR = cr;
        l += gate * (pl + hiss * hissL);
        r += gate * (pr + hiss * hissR);
    }
};

} // namespace driftwood

#endif // DRIFTWOOD_LOFIPARTS_H
