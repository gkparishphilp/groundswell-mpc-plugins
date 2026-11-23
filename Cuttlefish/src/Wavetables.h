// Wavetables.h - Cuttlefish's wavetables, generated in code on first use.
//
// SPDX-License-Identifier: MIT
//
// A wavetable here is kNumFrames single-cycle waveforms ("frames"). Morph reads between two
// neighbouring frames, so sweeping it changes the timbre smoothly. Every frame is stored nine times
// ("mip levels"), each band-limited for one octave of playback pitch, so high notes don't alias:
//
//   level   0     1     2    3    4    5    6    7    8
//   harms   256   128   64   32   16   8    4    2    1      (highest harmonic kept)
//   size    2048  1024  512  512  512  512  512  512  512    (samples; >= 8 per harmonic and at least
//                                                             512, so linear interpolation's images stay
//                                                             far down, -47 dB or more below the top
//                                                             harmonic on the high-note levels)
//
// The oscillator picks the level whose top harmonic stays below (sampleRate - 19 kHz), i.e. anything
// that folds back lands above 19 kHz. The top 40 % of each level's harmonics is gently tapered (to
// 40 % at the very top) so the change from one level to the next is a small step, not a jump.
//
// Each frame is described as a harmonic spectrum - either directly (additive tables) or by drawing
// the waveform at 4096 points and taking its FFT (drawn tables) - and every level is made from that
// spectrum with an inverse FFT. Frames are normalised to the same loudness (RMS 0.35, peak <= 0.95),
// with one gain for all levels of a frame, so Morph and pitch don't change the volume.
//
// The bank is built once per process (a function-local static, ~15 MB) and shared by every instance.
// It never allocates or computes anything after construction.

#ifndef CUTTLEFISH_WAVETABLES_H
#define CUTTLEFISH_WAVETABLES_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cuttlefish {

constexpr int kNumFrames = 32;
constexpr int kNumLevels = 9;
constexpr int kMaxHarm   = 256;          // harmonics at level 0
constexpr int kDrawSize  = 4096;         // drawn waveforms are sampled at this many points

enum TableId
{
    kTabBasic = 0, kTabPWM, kTabSync, kTabReso, kTabVowel, kTabFormant, kTabOrgan, kTabTine,
    kTabGlass, kTabMetal, kTabFM, kTabFold, kTabDigital, kTabHollow, kTabBrass, kTabSpectra, kTabComb,
    kNumTables
};

static const char* const kTableNames[kNumTables] = {
    "Basic", "PWM", "Sync", "Reso", "Vowel", "Formant", "Organ", "Tine",
    "Glass", "Metal", "FM", "Fold", "Digital", "Hollow", "Brass", "Spectra", "Comb"
};

enum SubWave { kSubSine = 0, kSubTri, kSubSquare, kNumSubWaves };

class WavetableBank
{
public:
    static const WavetableBank& get() { static WavetableBank bank; return bank; }

    // Level L of frame f of table t: levelSize(L) samples plus one guard sample (== sample 0).
    const float* frame(int t, int f, int L) const
    {
        return data_.data() + ((size_t)t * kNumFrames + (size_t)f) * (size_t)stride_ + (size_t)offset_[L];
    }
    const float* tableBase(int t) const { return data_.data() + (size_t)t * kNumFrames * (size_t)stride_; }
    const float* sub(int w, int L) const { return sub_.data() + (size_t)w * (size_t)stride_ + (size_t)offset_[L]; }
    int stride() const { return stride_; }
    int levelOffset(int L) const { return offset_[L]; }
    static int levelSize(int L) { const int n = 8 * (kMaxHarm >> L); return n < 512 ? 512 : n; }
    static int levelHarm(int L) { return kMaxHarm >> L; }

private:
    struct Spectrum { double re[kMaxHarm + 1]; double im[kMaxHarm + 1]; };   // x(p) = sum 2 Re(c_h e^{i2pi h p})

    std::vector<float> data_, sub_;
    int offset_[kNumLevels] = {};
    int stride_ = 0;

    // scratch (construction only)
    std::vector<double> fre_, fim_, draw_, lvl0_;

    WavetableBank()
    {
        int off = 0;
        for (int L = 0; L < kNumLevels; ++L) { offset_[L] = off; off += levelSize(L) + 1; }
        stride_ = off;
        data_.assign((size_t)kNumTables * kNumFrames * (size_t)stride_, 0.0f);
        sub_.assign((size_t)kNumSubWaves * (size_t)stride_, 0.0f);
        fre_.resize(kDrawSize); fim_.resize(kDrawSize); draw_.resize(kDrawSize); lvl0_.resize((size_t)levelSize(0));

        Spectrum s;
        for (int t = 0; t < kNumTables; ++t)
            for (int f = 0; f < kNumFrames; ++f)
            {
                spectrumFor(t, (double)f / (double)(kNumFrames - 1), s);
                build(s, data_.data() + ((size_t)t * kNumFrames + (size_t)f) * (size_t)stride_);
            }
        for (int w = 0; w < kNumSubWaves; ++w)
        {
            clear(s);
            for (int h = 1; h <= kMaxHarm; ++h)
            {
                double a = 0.0;
                if (w == kSubSine) a = h == 1 ? 1.0 : 0.0;
                else if (w == kSubTri) a = (h & 1) ? ((((h - 1) / 2) & 1) ? -1.0 : 1.0) / (double)(h * h) : 0.0;
                else a = (h & 1) ? 1.0 / (double)h : 0.0;
                sine(s, h, a);
            }
            build(s, sub_.data() + (size_t)w * (size_t)stride_);
        }
    }

    // ---- FFT (radix 2, in place, construction only) ----------------------------------------------
    static void fft(double* re, double* im, int n, bool inverse)
    {
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const double ang = 6.283185307179586 / (double)len * (inverse ? 1.0 : -1.0);
            const double wr = std::cos(ang), wi = std::sin(ang);
            for (int i = 0; i < n; i += len)
            {
                double cr = 1.0, ci = 0.0;
                for (int k = 0; k < len / 2; ++k)
                {
                    const int a = i + k, b = i + k + len / 2;
                    const double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                    const double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
                }
            }
        }
    }

    static void clear(Spectrum& s) { std::memset(&s, 0, sizeof(s)); }
    // add a sine-phase harmonic of amplitude a (a sin(2 pi h p))
    static void sine(Spectrum& s, int h, double a) { if (h >= 1 && h <= kMaxHarm) s.im[h] += -0.5 * a; }
    // harmonic of amplitude a with phase phi: a sin(2 pi h p + phi)
    static void sinePhase(Spectrum& s, int h, double a, double phi)
    {
        if (h < 1 || h > kMaxHarm) return;
        s.re[h] += 0.5 * a * std::sin(phi); s.im[h] += -0.5 * a * std::cos(phi);
    }

    // Sample a waveform function at kDrawSize points and take its spectrum.
    template <typename F> void drawn(Spectrum& s, F fn)
    {
        for (int n = 0; n < kDrawSize; ++n) { fre_[(size_t)n] = fn((double)n / (double)kDrawSize); fim_[(size_t)n] = 0.0; }
        fft(fre_.data(), fim_.data(), kDrawSize, false);
        for (int h = 0; h <= kMaxHarm; ++h) { s.re[h] = fre_[(size_t)h] / kDrawSize; s.im[h] = fim_[(size_t)h] / kDrawSize; }
        s.re[0] = s.im[0] = 0.0;
    }

    // One level from the spectrum into dst (size + guard). Returns nothing; gain applied.
    void synthLevel(const Spectrum& s, int L, double gain, double* out)
    {
        const int n = levelSize(L), H = levelHarm(L);
        for (int i = 0; i < n; ++i) { fre_[(size_t)i] = 0.0; fim_[(size_t)i] = 0.0; }
        for (int h = 1; h <= H && h < n / 2; ++h)
        {
            double g = gain;
            const double x = ((double)h - 0.6 * H) / (0.4 * H);
            if (h > 1 && x > 0.0) g *= 1.0 - 0.6 * x * x;
            fre_[(size_t)h] = s.re[h] * g; fim_[(size_t)h] = s.im[h] * g;
        }
        fft(fre_.data(), fim_.data(), n, true);
        for (int i = 0; i < n; ++i) out[i] = 2.0 * fre_[(size_t)i];
    }

    void build(const Spectrum& s, float* dst)
    {
        // level 0 at unit gain to measure loudness
        synthLevel(s, 0, 1.0, lvl0_.data());
        const int n0 = levelSize(0);
        double sum = 0.0, peak = 0.0;
        for (int i = 0; i < n0; ++i) { sum += lvl0_[(size_t)i] * lvl0_[(size_t)i]; peak = std::fmax(peak, std::fabs(lvl0_[(size_t)i])); }
        const double rms = std::sqrt(sum / n0);
        double gain = 1.0;
        if (rms > 1.0e-9) gain = std::fmin(0.35 / rms, 0.95 / peak);
        std::vector<double>& tmp = draw_;
        for (int L = 0; L < kNumLevels; ++L)
        {
            synthLevel(s, L, gain, tmp.data());
            const int n = levelSize(L);
            float* d = dst + offset_[L];
            for (int i = 0; i < n; ++i) d[i] = (float)tmp[(size_t)i];
            d[n] = d[0];
        }
    }

    // ---- helpers for table design ----------------------------------------------------------------
    static double frac(double x) { return x - std::floor(x); }
    static double lerp(double a, double b, double t) { return a + (b - a) * t; }
    static double hash01(uint32_t x)
    {
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return (double)(x & 0xFFFFFF) / 16777216.0;
    }
    static double tri(double p) { const double q = frac(p + 0.25); return 1.0 - 4.0 * std::fabs(q - 0.5); }
    static double saw(double p) { return 1.0 - 2.0 * frac(p); }
    static double sqr(double p, double w = 0.5) { return frac(p) < w ? 1.0 : -1.0; }
    static double sn(double p) { return std::sin(6.283185307179586 * p); }
    // resonance magnitude around a centre frequency (Lorentzian-ish)
    static double peak(double f, double centre, double bw) { const double d = (f - centre) / (0.5 * bw); return 1.0 / std::sqrt(1.0 + d * d); }
    // keyframe interpolation over n keyframes, smooth (cosine) between neighbours
    static void key(double x, int n, int& i, double& t)
    {
        const double pos = x * (double)(n - 1);
        i = (int)std::floor(pos); if (i >= n - 1) i = n - 2; if (i < 0) i = 0;
        t = pos - (double)i; t = 0.5 - 0.5 * std::cos(3.141592653589793 * t);
    }

    void spectrumFor(int t, double x, Spectrum& s)
    {
        clear(s);
        switch (t)
        {
            case kTabBasic:     // sine -> triangle -> saw -> square -> narrowing pulse
                drawn(s, [x](double p) {
                    const double seg = x * 4.0; const int i = seg >= 4.0 ? 3 : (int)seg; const double u = seg - i;
                    switch (i)
                    {
                        case 0: return lerp(sn(p), tri(p), u);
                        case 1: return lerp(tri(p), saw(p), u);
                        case 2: return lerp(saw(p), sqr(p), u);
                        default: return sqr(p, 0.5 - 0.38 * u);
                    }
                });
                break;
            case kTabPWM:       // pulse width 50 % -> 4 %
                drawn(s, [x](double p) { return sqr(p, 0.5 - 0.46 * x); });
                break;
            case kTabSync:      // hard-synced saw, slave ratio 1 -> 7
            {
                const double r = 1.0 + 6.0 * x;
                drawn(s, [r](double p) { return saw(p * r); });
                break;
            }
            case kTabReso:      // saw through a resonant low pass sweeping harmonic 1.5 -> 48
            {
                const double hc = 1.5 * std::pow(2.0, x * 5.0), Q = 5.0;
                for (int h = 1; h <= kMaxHarm; ++h)
                {
                    const double r = h / hc;
                    const double H = 1.0 / std::sqrt((1.0 - r * r) * (1.0 - r * r) + (r / Q) * (r / Q));
                    sine(s, h, H / std::sqrt(1.0 + r * r) / h);
                }
                break;
            }
            case kTabVowel:     // A - E - I - O - U (formants on a 130 Hz voice)
            {
                static const double F[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
                static const double B[3] = {90, 110, 160}, G[3] = {1.0, 0.55, 0.3};
                int i; double u; key(x, 5, i, u);
                double f[3]; for (int k = 0; k < 3; ++k) f[k] = lerp(F[i][k], F[i + 1][k], u);
                for (int h = 1; h <= kMaxHarm; ++h)
                {
                    const double fh = 130.0 * h;
                    double r = 0.03; for (int k = 0; k < 3; ++k) r += G[k] * peak(fh, f[k], B[k]);
                    sine(s, h, r / h);
                }
                break;
            }
            case kTabFormant:   // one bright formant (and a softer one above it) sweeping 250 Hz -> 4 kHz
            {
                const double f1 = 250.0 * std::pow(2.0, x * 4.0), f2 = f1 * 2.3;
                for (int h = 1; h <= kMaxHarm; ++h)
                {
                    const double fh = 110.0 * h;
                    const double r = peak(fh, f1, 60.0 + 0.12 * f1) + 0.45 * peak(fh, f2, 90.0 + 0.12 * f2) + (h == 1 ? 0.5 : 0.0);
                    sine(s, h, r / std::sqrt((double)h));
                }
                break;
            }
            case kTabOrgan:     // drawbar registrations 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'
            {
                static const int harm[7] = {1, 2, 3, 4, 5, 6, 8};
                static const double reg[7][7] = {
                    {8, 0, 0, 0, 0, 0, 0}, {8, 6, 0, 0, 0, 0, 0}, {8, 8, 6, 0, 0, 0, 0}, {8, 8, 8, 4, 0, 0, 0},
                    {8, 6, 8, 6, 2, 4, 0}, {8, 8, 8, 8, 6, 6, 4}, {8, 8, 8, 8, 8, 8, 8}};
                int i; double u; key(x, 7, i, u);
                for (int k = 0; k < 7; ++k)
                {
                    auto amp = [](double n) { return n <= 0.0 ? 0.0 : std::pow(10.0, -3.0 * (8.0 - n) / 20.0); };
                    sine(s, harm[k], lerp(amp(reg[i][k]), amp(reg[i + 1][k]), u));
                }
                break;
            }
            case kTabTine:      // electric-piano tine: soft -> bark -> bell
            {
                static const int H[10] = {1, 2, 3, 4, 5, 7, 9, 14, 21, 28};
                static const double K[4][10] = {
                    {1.0, 0.08, 0.02, 0, 0, 0, 0, 0, 0, 0},
                    {1.0, 0.35, 0.15, 0.08, 0.05, 0.12, 0.03, 0, 0, 0},
                    {1.0, 0.5, 0.3, 0.2, 0.15, 0.3, 0.12, 0.25, 0.08, 0.02},
                    {0.8, 0.1, 0.2, 0.02, 0, 0.5, 0.05, 0.45, 0.3, 0.15}};
                int i; double u; key(x, 4, i, u);
                for (int k = 0; k < 10; ++k) sine(s, H[k], lerp(K[i][k], K[i + 1][k], u));
                break;
            }
            case kTabGlass:     // a sine growing sparse high partials at the square numbers
            {
                sine(s, 1, 1.0);
                for (int k = 2; k <= 9; ++k)
                {
                    const double on = std::fmin(1.0, std::fmax(0.0, x * 9.0 - (k - 2) * 0.9));
                    const double a = on * 0.75 / std::sqrt((double)k);
                    sine(s, k * k, a);
                    sinePhase(s, k * k + 1, 0.3 * a, 1.0);
                }
                break;
            }
            case kTabMetal:     // clangorous sparse partials, more and brighter as it morphs
            {
                sine(s, 1, 1.0);
                for (int h = 2; h <= 160; ++h)
                {
                    if (hash01((uint32_t)h * 7919u + 3u) > 0.32) continue;
                    const double th = hash01((uint32_t)h * 104729u + 11u) * 0.85;
                    const double on = std::fmin(1.0, std::fmax(0.0, (x * 1.15 - th) * 5.0));
                    const double w = (0.35 + 0.65 * hash01((uint32_t)h * 1299709u)) / std::pow((double)h, 0.75 - 0.35 * x);
                    sinePhase(s, h, on * w * 1.6, 6.283185307179586 * hash01((uint32_t)h * 15485863u));
                }
                break;
            }
            case kTabFM:        // 2-operator phase modulation, ratio 1:1, index 0 -> 7
            {
                const double I = 7.0 * x * x;
                drawn(s, [I](double p) { return std::sin(6.283185307179586 * p + I * std::sin(6.283185307179586 * p)); });
                break;
            }
            case kTabFold:      // a sine driven into a wavefolder, gain 1 -> 7
            {
                const double g = 1.0 + 6.0 * x;
                drawn(s, [g](double p) {
                    const double v = g * sn(p) + 0.08 * (g - 1.0);
                    const double q = frac((v + 1.0) * 0.25);
                    return 1.0 - 4.0 * std::fabs(q - 0.5);
                });
                break;
            }
            case kTabDigital:   // a sine with fewer and fewer steps per cycle and levels
            {
                const double S = 48.0 - 44.0 * x, Q = 16.0 - 14.0 * x;
                drawn(s, [S, Q](double p) {
                    const double held = std::floor(p * S) / S;
                    return std::round(std::sin(6.283185307179586 * held) * Q) / Q;
                });
                break;
            }
            case kTabHollow:    // odd harmonics (hollow) -> all (saw) -> fundamental + even (octave-bright)
                for (int h = 1; h <= kMaxHarm; ++h)
                {
                    double g;
                    if (h == 1) g = 1.0;
                    else if (h & 1) g = x < 0.5 ? 1.0 : 1.0 - 2.0 * (x - 0.5);
                    else g = x < 0.5 ? 2.0 * x : 1.0;
                    sine(s, h, g / h);
                }
                break;
            case kTabBrass:     // saw with a low pass opening, and a brassy bump around 1.2 kHz
            {
                const double hc = 1.2 * std::pow(2.0, x * 5.0);
                for (int h = 1; h <= kMaxHarm; ++h)
                {
                    const double r = h / hc;
                    const double bump = 1.0 + 0.8 * std::exp(-std::pow((110.0 * h - 1200.0) / 600.0, 2.0));
                    sine(s, h, bump / (1.0 + std::pow(r, 2.5)) / h);
                }
                break;
            }
            case kTabSpectra:   // five random spectra, blended (each harmonic keeps its own phase)
            {
                int i; double u; key(x, 5, i, u);
                for (int h = 1; h <= 96; ++h)
                {
                    auto amp = [h](int k) { const double r = hash01((uint32_t)(h * 977 + k * 7187 + 1)); return r * r; };
                    const double a = lerp(amp(i), amp(i + 1), u) / std::pow((double)h, 0.8) + (h == 1 ? 0.4 : 0.0);
                    sinePhase(s, h, a, 6.283185307179586 * hash01((uint32_t)h * 31337u));
                }
                break;
            }
            case kTabComb:      // saw through a moving comb (phaser / flanger colours)
            {
                const double d = 0.012 + 0.24 * x;
                for (int h = 1; h <= kMaxHarm; ++h)
                    sine(s, h, (0.12 + 0.88 * std::fabs(std::cos(3.141592653589793 * h * d))) / h);
                break;
            }
            default:
                sine(s, 1, 1.0);
                break;
        }
    }
};

} // namespace cuttlefish

#endif
