// Harmony.h - hears the key and the current chord in a (polyphonic) mix.
//
// SPDX-License-Identifier: MIT
//
// How it works
// ------------
//   input -> high-pass 50 Hz -> anti-alias low-pass -> decimate to ~11 kHz -> ring buffer (8192)
//
// Every ~93 ms (1024 decimated samples) it measures 48 semitones, C2 to B5, each with its own
// Goertzel filter over a Hann window whose length is about 34 periods of that note. That gives
// every bin the same semitone resolution (a constant-Q filter bank) for roughly 100k
// multiply-adds per frame. The frame is spread over several process() calls, 8 bins at a time,
// so no single audio block pays for all of it.
//
// Drums smear energy over every bin; notes stand out as peaks. So each bin has the average of its
// six neighbours subtracted. What survives is folded into twelve pitch classes, and the fraction
// that survived ("tonality") weights the frame's vote: a frame of hi-hats barely counts, a frame of
// chords counts fully.
//
// Key:   a pitch-class histogram with a ~20 s memory, correlated against the Krumhansl-Kessler
//        major and minor key profiles (published listening-test data, Krumhansl & Kessler 1982)
//        for all 24 keys. Confidence is how far the best key beats the best key on another root.
// Chord: the same, with a ~0.35 s memory, scored against the seven triads of the key in use, with
//        an extra vote for whichever triad root is loudest in the bass octaves (C2-B3). It changes
//        chord only when a new one has won two frames running by a clear margin.
//
// Notes take about a third of a second to register, so the first note after a chord change can
// still be the old root. That is the price of hearing chords instead of being told them.

#pragma once

#include "dsp/biquad.h"
#include "dsp/math.h"

#include <cmath>
#include <cstdint>

struct ScaleDef { const char* name; int notes[7]; bool minorish; };

// Order is the order of the Scale parameter's choices. Never reorder.
static const ScaleDef kScales[] = {
    {"Minor",    {0, 2, 3, 5, 7, 8, 10}, true},
    {"Major",    {0, 2, 4, 5, 7, 9, 11}, false},
    {"Dorian",   {0, 2, 3, 5, 7, 9, 10}, true},
    {"Phrygian", {0, 1, 3, 5, 7, 8, 10}, true},
    {"Mixolyd",  {0, 2, 4, 5, 7, 9, 10}, false},
};
constexpr int kNumScales = (int)(sizeof(kScales) / sizeof(kScales[0]));

class Harmony
{
public:
    static constexpr int kLowMidi = 36;     // C2
    static constexpr int kNumBins = 48;     // four octaves
    static constexpr int kBassBins = 24;    // C2..B3 count as "bass" for the chord root vote
    static constexpr int kBuf = 8192;       // power of two
    static constexpr int kHop = 1024;
    static constexpr int kBinsPerCall = 8;
    static constexpr int kWin = 1024;       // Hann table resolution

    void prepare(double sampleRate)
    {
        dec_ = (int)std::lround(sampleRate / 11025.0);
        if (dec_ < 1) dec_ = 1;
        fsd_ = (float)(sampleRate / dec_);
        using mpc::dsp::BiquadCoeffs;
        hp_.setCoeffs(BiquadCoeffs::highpass(sampleRate, 50.0f));
        const BiquadCoeffs aa = BiquadCoeffs::lowpass(sampleRate, 0.42f * fsd_, 0.7071f);
        aa1_.setCoeffs(aa);
        aa2_.setCoeffs(aa);
        for (int k = 0; k < kNumBins; ++k)
        {
            const float hz = 440.0f * std::pow(2.0f, (float)(kLowMidi + k - 69) / 12.0f);
            int n = (int)std::lround(34.0f * fsd_ / hz);
            n = mpc::dsp::clamp(n, 256, kBuf - 2 * kHop);
            binLen_[k] = n;
            binCoef_[k] = 2.0f * std::cos(2.0f * 3.14159265f * hz / fsd_);
        }
        for (int i = 0; i <= kWin; ++i)
            hann_[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * (float)i / (float)kWin);
        const float hopSec = (float)kHop / fsd_;
        keyDecay_ = std::exp(-hopSec / 20.0f);
        chordDecay_ = std::exp(-hopSec / 0.35f);
        tonalDecay_ = std::exp(-hopSec / 2.0f);
        reset();
    }

    void reset()
    {
        hp_.reset(); aa1_.reset(); aa2_.reset();
        for (float& v : ring_) v = 0.0f;
        writePos_ = 0; decCount_ = 0; sinceHop_ = 0; filled_ = 0;
        busy_ = false; nextBin_ = 0; frameEnd_ = 0;
        for (int i = 0; i < 12; ++i) { keyHist_[i] = 0.0f; chordMid_[i] = 0.0f; chordBass_[i] = 0.0f; }
        keyMass_ = 0.0f; tonal_ = 0.0f;
        foundRoot_ = 0; foundMinor_ = true; confidence_ = 0.0f;
        chordDegree_ = 0; chordValid_ = false; candidate_ = -1; candidateFrames_ = 0;
    }

    // The key the chord detector should think in (the plugin's active key).
    void setKey(int root, int scaleIdx)
    {
        keyRoot_ = ((root % 12) + 12) % 12;
        scale_ = mpc::dsp::clamp(scaleIdx, 0, kNumScales - 1);
    }

    // One input sample (mono).
    void push(float x)
    {
        const float y = aa2_.process(aa1_.process(hp_.process(x)));
        if (++decCount_ < dec_) return;
        decCount_ = 0;
        ring_[writePos_] = y;
        writePos_ = (writePos_ + 1) & (kBuf - 1);
        ++sinceHop_;
        if (filled_ < kBuf) ++filled_;
    }

    // Call once per audio block, after the block's samples were pushed. Does a slice of analysis.
    void work()
    {
        if (!busy_)
        {
            if (sinceHop_ < kHop || filled_ < kBuf / 2) return;
            sinceHop_ = 0;
            busy_ = true;
            nextBin_ = 0;
            frameEnd_ = writePos_;
        }
        const int end = mpc::dsp::clamp(nextBin_ + kBinsPerCall, 0, kNumBins);
        for (int k = nextBin_; k < end; ++k) amp_[k] = goertzel(k);
        nextBin_ = end;
        if (nextBin_ >= kNumBins) { busy_ = false; finishFrame(); }
    }

    int   foundRoot() const { return foundRoot_; }
    bool  foundMinor() const { return foundMinor_; }
    float confidence() const { return confidence_; }      // 0..1
    float tonality() const { return tonal_; }             // smoothed 0..1: is there harmony to hear?
    bool  chordValid() const { return chordValid_; }
    int   chordDegree() const { return chordDegree_; }    // 0..6 in the key passed to setKey()

private:
    float goertzel(int k) const
    {
        const int n = binLen_[k];
        const float c = binCoef_[k];
        const float step = (float)kWin / (float)n;
        int idx = (frameEnd_ - n) & (kBuf - 1);
        float s1 = 0.0f, s2 = 0.0f, w = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float s0 = ring_[idx] * hann_[(int)w] + c * s1 - s2;
            s2 = s1; s1 = s0;
            idx = (idx + 1) & (kBuf - 1);
            w += step;
        }
        float p = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (p < 0.0f) p = 0.0f;
        return std::sqrt(p) / (float)n;
    }

    void finishFrame()
    {
        float total = 0.0f;
        for (int k = 0; k < kNumBins; ++k) total += amp_[k];
        if (total < 1e-5f)                      // silence: forget nothing, learn nothing
        {
            tonal_ *= tonalDecay_;
            return;
        }

        // Peaks above the local floor.
        float peaks[kNumBins];
        float kept = 0.0f;
        for (int k = 0; k < kNumBins; ++k)
        {
            float sum = 0.0f; int cnt = 0;
            for (int j = k - 3; j <= k + 3; ++j)
                if (j != k && j >= 0 && j < kNumBins) { sum += amp_[j]; ++cnt; }
            const float v = amp_[k] - (cnt > 0 ? sum / (float)cnt : 0.0f);
            peaks[k] = v > 0.0f ? v : 0.0f;
            kept += peaks[k];
        }
        const float tonality = mpc::dsp::clamp(kept / total, 0.0f, 1.0f);
        tonal_ = tonal_ * tonalDecay_ + tonality * (1.0f - tonalDecay_);
        if (kept <= 1e-9f) return;

        float mid[12] = {}, bass[12] = {};
        float midSum = 0.0f, bassSum = 0.0f;
        for (int k = 0; k < kNumBins; ++k)
        {
            const int pc = (kLowMidi + k) % 12;
            mid[pc] += peaks[k]; midSum += peaks[k];
            if (k < kBassBins) { bass[pc] += peaks[k]; bassSum += peaks[k]; }
        }
        const float vote = tonality * tonality;
        for (int i = 0; i < 12; ++i)
        {
            const float m = mid[i] / midSum;
            keyHist_[i] = keyHist_[i] * keyDecay_ + vote * m;
            chordMid_[i] = chordMid_[i] * chordDecay_ + vote * m;
            chordBass_[i] = chordBass_[i] * chordDecay_ + (bassSum > 0.0f ? vote * bass[i] / bassSum : 0.0f);
        }
        keyMass_ = keyMass_ * keyDecay_ + vote;

        findKey();
        findChord();
    }

    void findKey()
    {
        // Krumhansl-Kessler probe-tone profiles.
        static const float kMajor[12] = {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};
        static const float kMinor[12] = {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};
        float hMean = 0.0f;
        for (float v : keyHist_) hMean += v;
        hMean /= 12.0f;
        float bestPerRoot[12];
        bool minorPerRoot[12];
        for (int r = 0; r < 12; ++r)
        {
            bestPerRoot[r] = -2.0f;
            for (int mode = 0; mode < 2; ++mode)
            {
                const float* prof = mode == 0 ? kMajor : kMinor;
                float pMean = 0.0f;
                for (int i = 0; i < 12; ++i) pMean += prof[i];
                pMean /= 12.0f;
                float num = 0.0f, dh = 0.0f, dp = 0.0f;
                for (int i = 0; i < 12; ++i)
                {
                    const float h = keyHist_[(i + r) % 12] - hMean;
                    const float p = prof[i] - pMean;
                    num += h * p; dh += h * h; dp += p * p;
                }
                const float corr = (dh > 1e-12f) ? num / std::sqrt(dh * dp) : 0.0f;
                if (corr > bestPerRoot[r]) { bestPerRoot[r] = corr; minorPerRoot[r] = (mode == 1); }
            }
        }
        int best = 0;
        for (int r = 1; r < 12; ++r) if (bestPerRoot[r] > bestPerRoot[best]) best = r;
        float second = -2.0f;
        for (int r = 0; r < 12; ++r) if (r != best && bestPerRoot[r] > second) second = bestPerRoot[r];
        foundRoot_ = best;
        foundMinor_ = minorPerRoot[best];
        const float gap = mpc::dsp::clamp((bestPerRoot[best] - second) / 0.12f, 0.0f, 1.0f);
        const float enough = mpc::dsp::clamp(keyMass_ / 6.0f, 0.0f, 1.0f);   // ~6 good frames
        confidence_ = gap * enough * (bestPerRoot[best] > 0.3f ? 1.0f : 0.0f);
    }

    void findChord()
    {
        float midSum = 0.0f, bassSum = 0.0f;
        for (int i = 0; i < 12; ++i) { midSum += chordMid_[i]; bassSum += chordBass_[i]; }
        if (midSum < 0.05f || tonal_ < 0.08f) return;   // nothing harmonic to hear: keep the last chord
        const ScaleDef& sc = kScales[scale_];
        float score[7];
        int best = 0;
        for (int d = 0; d < 7; ++d)
        {
            const int r = (keyRoot_ + sc.notes[d]) % 12;
            const int t = (keyRoot_ + sc.notes[(d + 2) % 7]) % 12;
            const int f = (keyRoot_ + sc.notes[(d + 4) % 7]) % 12;
            float in = chordMid_[r] + 0.8f * chordMid_[t] + 0.9f * chordMid_[f];
            float out = midSum - chordMid_[r] - chordMid_[t] - chordMid_[f];
            float s = (in - 0.45f * out / 9.0f) / midSum;
            if (bassSum > 0.02f) s += 0.9f * chordBass_[r] / bassSum;
            score[d] = s;
            if (s > score[best]) best = d;
        }
        if (!chordValid_) { chordDegree_ = best; chordValid_ = true; candidate_ = -1; return; }
        if (best == chordDegree_) { candidate_ = -1; candidateFrames_ = 0; return; }
        if (score[best] < score[chordDegree_] * 1.08f + 0.01f) { candidate_ = -1; return; }
        if (best != candidate_) { candidate_ = best; candidateFrames_ = 1; return; }
        if (++candidateFrames_ >= 2) { chordDegree_ = best; candidate_ = -1; candidateFrames_ = 0; }
    }

    mpc::dsp::Biquad hp_, aa1_, aa2_;
    int dec_ = 4;
    float fsd_ = 11025.0f;
    float ring_[kBuf] = {};
    int writePos_ = 0, decCount_ = 0, sinceHop_ = 0, filled_ = 0;
    bool busy_ = false;
    int nextBin_ = 0, frameEnd_ = 0;
    int binLen_[kNumBins] = {};
    float binCoef_[kNumBins] = {};
    float amp_[kNumBins] = {};
    float hann_[kWin + 1] = {};

    float keyDecay_ = 0.995f, chordDecay_ = 0.77f, tonalDecay_ = 0.95f;
    float keyHist_[12] = {}, chordMid_[12] = {}, chordBass_[12] = {};
    float keyMass_ = 0.0f, tonal_ = 0.0f;
    int foundRoot_ = 0; bool foundMinor_ = true; float confidence_ = 0.0f;

    int keyRoot_ = 0, scale_ = 0;
    int chordDegree_ = 0; bool chordValid_ = false;
    int candidate_ = -1, candidateFrames_ = 0;
};
