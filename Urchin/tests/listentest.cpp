// Urchin's render test: does each Spine have its own harmonic signature, does the anti-aliasing hold
// at full Drive on a high note, do Blend and Low Keep keep the low end, does the gate gate (and
// chop), does the level stay sane across Drive and Spike, is it clean (no NaN, no denormals) and
// cheap? Also renders a demo WAV.
//
//   mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework -o tests/build/listentest tests/listentest.cpp
//   tests/build/listentest [demo.wav]          (run from plugins/Urchin)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#define private public
#define protected public
#include "../src/Urchin.cpp"
#undef private
#undef protected

static double SR = 44100.0;
static int failures = 0, passes = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) ++passes; else ++failures;
}

typedef std::vector<float> Buf;

// ---------------------------------------------------------------------------------------------
// Test material: drums, bass, a synth line, a plucked guitar
// ---------------------------------------------------------------------------------------------
static unsigned g_rs = 12345;
static float rnd() { g_rs = g_rs * 1664525u + 1013904223u; return (float)(g_rs >> 8) / 16777216.0f * 2.0f - 1.0f; }
static float midiHz(int n) { return 440.0f * std::pow(2.0f, (n - 69) / 12.0f); }

static Buf drums(double bpm, int bars)
{
    const double spb = SR * 60.0 / bpm, step = spb / 4.0;
    Buf o((size_t)(bars * 4 * spb), 0.0f);
    const long long total = (long long)o.size();
    for (int b = 0; b < bars; ++b)
        for (int k = 0; k < 16; ++k)
        {
            const long long t0 = (long long)((b * 16 + k) * step);
            const bool kick = (k == 0 || k == 6 || k == 8 || k == 11);
            const bool snare = (k == 4 || k == 12);
            if (kick)
            {
                double ph = 0.0;
                for (long long i = 0; i < (long long)(0.4 * SR) && t0 + i < total; ++i)
                {
                    const double t = i / SR, hz = 50.0 + 120.0 * std::exp(-t * 30.0);
                    ph += hz / SR;
                    o[(size_t)(t0 + i)] += (float)(0.85 * std::sin(6.2831853 * ph) * std::exp(-t * 7.0));
                }
            }
            if (snare)
                for (long long i = 0; i < (long long)(0.25 * SR) && t0 + i < total; ++i)
                {
                    const double t = i / SR;
                    float v = rnd() * (float)std::exp(-t * 18.0) * 0.45f;
                    v += (float)(0.3 * std::sin(6.2831853 * 185.0 * t) * std::exp(-t * 25.0));
                    o[(size_t)(t0 + i)] += v;
                }
            float hp = 0.0f, prev = 0.0f;
            for (long long i = 0; i < (long long)(0.05 * SR) && t0 + i < total; ++i)
            {
                const double t = i / SR;
                const float w = rnd();
                hp = 0.6f * (hp + w - prev); prev = w;
                o[(size_t)(t0 + i)] += hp * (float)std::exp(-t * 70.0) * ((k % 2) ? 0.08f : 0.16f);
            }
        }
    for (float& v : o) v *= 0.5f;                     // peaks around -6 dBFS
    return o;
}

static Buf bass(double bpm, int bars)
{
    const double spb = SR * 60.0 / bpm, step = spb / 4.0;
    Buf o((size_t)(bars * 4 * spb), 0.0f);
    const long long total = (long long)o.size();
    const int notes[16] = {33, -1, 33, 45, -1, 33, 36, -1, 31, -1, 31, 43, 38, -1, 36, 35};
    float lp = 0.0f;
    for (int b = 0; b < bars; ++b)
        for (int k = 0; k < 16; ++k)
        {
            if (notes[k] < 0) continue;
            const long long t0 = (long long)((b * 16 + k) * step);
            const long long len = (long long)(step * 1.6);
            const double hz = midiHz(notes[k]);
            double ph = 0.0;
            for (long long i = 0; i < len && t0 + i < total; ++i)
            {
                ph += hz / SR; ph -= std::floor(ph);
                const float saw = (float)(2.0 * ph - 1.0);
                const float env = (float)(std::exp(-(double)i / SR * 4.0) * std::fmin(1.0, (len - i) / (0.005 * SR)));
                const float cut = 0.05f + 0.25f * env;
                lp += cut * (saw - lp);
                o[(size_t)(t0 + i)] += lp * env * ((k % 4 == 0) ? 0.55f : 0.4f);
            }
        }
    return o;
}

// A detuned two-saw synth line with a plucky filter envelope.
static Buf synth(double bpm, int bars)
{
    const double spb = SR * 60.0 / bpm, step = spb / 4.0;
    Buf o((size_t)(bars * 4 * spb), 0.0f);
    const long long total = (long long)o.size();
    const int mel[16] = {57, -1, 60, 64, -1, 62, 60, -1, 69, -1, 67, 64, -1, 62, 64, -1};
    for (int b = 0; b < bars; ++b)
        for (int k = 0; k < 16; ++k)
        {
            const int note = mel[k] < 0 ? -1 : mel[k] + ((b % 2 == 1 && k >= 8) ? -2 : 0);
            if (note < 0) continue;
            const long long t0 = (long long)((b * 16 + k) * step), len = (long long)(step * 1.8);
            const double hz = midiHz(note);
            double p1 = 0.0, p2 = 0.3;
            float lp1 = 0.0f, lp2 = 0.0f;
            for (long long i = 0; i < len && t0 + i < total; ++i)
            {
                p1 += hz / SR; p1 -= std::floor(p1);
                p2 += hz * 1.006 / SR; p2 -= std::floor(p2);
                const float s = (float)(p1 + p2 - 1.0);
                const float fe = (float)std::exp(-(double)i / SR * 12.0);
                const float c = 0.04f + 0.4f * fe;
                lp1 += c * (s - lp1); lp2 += c * (lp1 - lp2);
                const float env = (float)std::fmin(1.0, std::fmin(i / (0.003 * SR), (len - i) / (0.01 * SR)));
                o[(size_t)(t0 + i)] += lp2 * env * 0.3f;
            }
        }
    return o;
}

// A plucked string (Karplus-Strong with a fractional-delay allpass and pick position), from Breakwater.
static void pluck(Buf& buf, long long t0, float hz, float vel, float durS, bool mute = false)
{
    const double period = SR / hz;
    const int N = (int)std::floor(period - 0.5);
    const float frac = (float)(period - 0.5 - N);
    const float apc = (1.0f - frac) / (1.0f + frac);
    std::vector<float> line((size_t)N + 2, 0.0f), ex((size_t)N, 0.0f);
    float lp = 0.0f; const float bright = 0.25f + 0.7f * vel;
    for (int i = 0; i < N; ++i) { lp += bright * (rnd() - lp); ex[(size_t)i] = lp; }
    const int pp = std::max(1, (int)(N * 0.13));
    for (int i = N - 1; i >= pp; --i) ex[(size_t)i] -= ex[(size_t)(i - pp)];
    for (int i = 0; i < N; ++i) line[(size_t)i] = ex[(size_t)i] * vel;
    const float loss = mute ? 0.93f : (0.9965f - 0.0000045f * hz);
    const int len = (int)(durS * SR);
    int w = 0; float apz = 0.0f, prev = 0.0f, hp = 0.0f, hpx = 0.0f;
    for (int i = 0; i < len && t0 + i < (long long)buf.size(); ++i)
    {
        const float a = line[(size_t)w];
        const float avg = 0.5f * (a + prev); prev = a;
        const float y = apc * avg + apz; apz = avg - apc * y;
        line[(size_t)w] = y * loss;
        w = (w + 1) % N;
        float env = 1.0f; const int rel = (int)(0.02 * SR);
        if (i > len - rel) env = (float)(len - i) / (float)rel;
        const float o = a * env;
        hp = 0.995f * (hp + o - hpx); hpx = o;
        buf[(size_t)(t0 + i)] += hp * 0.16f;          // a guitar DI: a hard strum peaks around -8 dBFS
    }
}

// A riff: power chords and single-note fills, 8th notes.
static Buf guitar(double bpm, int bars)
{
    const double spb = SR * 60.0 / bpm;
    Buf o((size_t)(bars * 4 * spb), 0.0f);
    const int roots[4] = {40, 43, 45, 38};
    for (int b = 0; b < bars; ++b)
    {
        const int r = roots[b % 4];
        for (int e = 0; e < 8; ++e)
        {
            const long long t0 = (long long)((b * 4 + e * 0.5) * spb);
            if (e < 5)
            {
                const bool mute = (e == 1 || e == 3);
                const float v = mute ? 0.55f : 0.9f;
                for (int k = 0; k < 3; ++k)
                    pluck(o, t0 + (long long)(k * 0.008 * SR), midiHz(r + (k == 0 ? 0 : (k == 1 ? 7 : 12))), v, mute ? 0.15f : 0.5f, mute);
            }
            else
            {
                const int lick[3] = {12, 15, 17};
                pluck(o, t0, midiHz(r + 12 + lick[e - 5]), 0.85f, 0.45f);
            }
        }
    }
    return o;
}

static Buf sine(double hz, float amp, double secs)
{
    Buf o((size_t)(secs * SR));
    for (size_t i = 0; i < o.size(); ++i) o[i] = amp * (float)std::sin(6.283185307179586 * hz * (double)i / SR);
    return o;
}

static Buf sum4(const Buf& a, const Buf& b, const Buf& c, const Buf& d, float ga, float gb, float gc, float gd)
{
    Buf o(std::min(std::min(a.size(), b.size()), std::min(c.size(), d.size())));
    for (size_t i = 0; i < o.size(); ++i) o[i] = ga * a[i] + gb * b[i] + gc * c[i] + gd * d[i];
    return o;
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------
struct Settings
{
    int spine = kGated, focus = kFlat, stereo = 0, spineR = 0;   // spineR: 0 = Same, 1 + Spine = Split
    float drive = 50, spike = 0, tone = 0, bias = 25, bloom = 0, gate = 0, blend = 100, lowKeep = 20, level = 0, output = 0, width = 100;
};

static void apply(Urchin& p, const Settings& s)
{
    p.set(kOutput, s.output); p.set(kSpine, (float)s.spine); p.set(kDrive, s.drive); p.set(kSpike, s.spike);
    p.set(kTone, s.tone); p.set(kFocus, (float)s.focus); p.set(kBias, s.bias); p.set(kBloom, s.bloom);
    p.set(kGate, s.gate); p.set(kBlend, s.blend); p.set(kLowKeep, s.lowKeep); p.set(kLevel, s.level);
    p.set(kStereo, (float)s.stereo); p.set(kSpineR, (float)s.spineR); p.set(kWidth, s.width);
}

typedef void (*Automation)(Urchin& p, size_t pos, size_t total);

static void render(Urchin& p, const Buf& inL, const Buf& inR, int block, Buf& outL, Buf& outR, Automation aut = nullptr)
{
    const size_t total = inL.size();
    outL.assign(total, 0.0f); outR.assign(total, 0.0f);
    Buf l((size_t)block), r((size_t)block);
    mpc::ScopedDenormalsOff ftz;
    for (size_t pos = 0; pos < total; pos += (size_t)block)
    {
        const int n = (int)std::min((size_t)block, total - pos);
        if (aut) aut(p, pos, total);
        for (int i = 0; i < n; ++i) { l[(size_t)i] = inL[pos + (size_t)i]; r[(size_t)i] = inR[pos + (size_t)i]; }
        const float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {l.data(), r.data()};
        mpc::ProcessContext ctx;
        ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = n;
        ctx.sampleRate = SR;
        p.process(ctx);
        for (int i = 0; i < n; ++i) { outL[pos + (size_t)i] = l[(size_t)i]; outR[pos + (size_t)i] = r[(size_t)i]; }
    }
}

static void run(const Settings& s, const Buf& in, Buf& outL, Buf& outR, int block = 64, Automation aut = nullptr)
{
    Urchin p;
    p.prepare(SR, 512);
    apply(p, s);
    p.reset();
    render(p, in, in, block, outL, outR, aut);
}

static Buf runL(const Settings& s, const Buf& in, int block = 64)
{
    Buf l, r; run(s, in, l, r, block); return l;
}

// ---------------------------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------------------------
static double rmsDb(const Buf& x, size_t a, size_t b)
{
    double s = 0; b = std::min(b, x.size());
    for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i];
    return 10.0 * std::log10(s / (double)(b - a) + 1e-30);
}
static double peakDb(const Buf& x, size_t a = 0, size_t b = (size_t)-1)
{
    float pk = 0.0f;
    for (size_t i = a; i < std::min(b, x.size()); ++i) pk = std::max(pk, std::fabs(x[i]));
    return 20.0 * std::log10(pk + 1e-30);
}
// Amplitude of the component at hz over [a, b) (windowed Goertzel-style fit), in dB re full scale.
static double toneDb(const Buf& x, size_t a, size_t b, double hz)
{
    double c = 0, s = 0, wsum = 0;
    for (size_t i = a; i < b; ++i)
    {
        const double t = (double)(i - a) / (double)(b - a);
        const double w = 0.5 - 0.5 * std::cos(6.283185307179586 * t);       // Hann
        const double ph = 6.283185307179586 * hz * (double)i / SR;
        c += w * x[i] * std::cos(ph); s += w * x[i] * std::sin(ph); wsum += w;
    }
    const double amp = 2.0 * std::sqrt(c * c + s * s) / wsum;
    return 20.0 * std::log10(amp + 1e-12);
}
// Energy below `hz` (4th-order Butterworth low-pass, in double), dB.
static double lowDb(const Buf& x, size_t a, size_t b, double hz)
{
    const double w0 = 6.283185307179586 * hz / SR, cw = std::cos(w0), al = std::sin(w0) / (2.0 * 0.70710678);
    const double b0 = (1 - cw) / 2 / (1 + al), b1 = (1 - cw) / (1 + al), a1 = -2 * cw / (1 + al), a2 = (1 - al) / (1 + al);
    double z[2][2] = {{0, 0}, {0, 0}}, s = 0;
    for (size_t i = 0; i < b && i < x.size(); ++i)
    {
        double v = x[i];
        for (int k = 0; k < 2; ++k)
        {
            const double y = b0 * v + z[k][0];
            z[k][0] = b1 * v - a1 * y + z[k][1];
            z[k][1] = b0 * v - a2 * y;
            v = y;
        }
        if (i >= a) s += v * v;
    }
    return 10.0 * std::log10(s / (double)(b - a) + 1e-30);
}
static bool cleanBuf(const Buf& x, int& bad, int& subnormal)
{
    bad = subnormal = 0;
    for (float v : x)
    {
        if (!std::isfinite(v)) ++bad;
        else if (v != 0.0f && std::fabs(v) < 1.17549435e-38f) ++subnormal;
    }
    return bad == 0 && subnormal == 0;
}

// In-place radix-2 FFT.
static void fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -6.283185307179586 / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Aliasing: power that is not at a harmonic of f0 (or DC), relative to the total, in dB, from a
// 4-term Blackman-Harris windowed FFT of x[a, a + N). Band: 20 Hz to 20 kHz.
static double aliasDb(const Buf& x, size_t a, double f0, double* worstDb = nullptr)
{
    const size_t N = 65536;
    std::vector<std::complex<double>> v(N);
    for (size_t i = 0; i < N; ++i)
    {
        const double t = 6.283185307179586 * (double)i / (double)(N - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        v[i] = w * (double)x[a + i];
    }
    fft(v);
    const double binHz = SR / (double)N;
    double tot = 0, ali = 0, worst = 0;
    std::vector<char> harm(N / 2, 0);
    for (int k = 1; k * f0 < SR / 2; ++k)
    {
        const long c = std::lround(k * f0 / binHz);
        for (long d = -8; d <= 8; ++d) if (c + d > 0 && c + d < (long)(N / 2)) harm[(size_t)(c + d)] = 1;
    }
    for (size_t i = (size_t)(20.0 / binHz); i < (size_t)(20000.0 / binHz); ++i)
    {
        const double p = std::norm(v[i]);
        tot += p;
        if (!harm[i]) { ali += p; worst = std::max(worst, p); }
    }
    if (worstDb) *worstDb = 10.0 * std::log10(worst / (tot + 1e-30) + 1e-30);
    return 10.0 * std::log10(ali / (tot + 1e-30) + 1e-30);
}

static void writeWav(const char* path, const Buf& l, const Buf& r)
{
    FILE* f = std::fopen(path, "wb"); if (!f) return;
    const uint32_t n = (uint32_t)l.size(), bytes = n * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32((uint32_t)SR); u32((uint32_t)SR * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; ++i)
    {
        int16_t a = (int16_t)std::lround(std::fmax(-1.0, std::fmin(1.0, l[i])) * 32767.0);
        int16_t b = (int16_t)std::lround(std::fmax(-1.0, std::fmin(1.0, r[i])) * 32767.0);
        std::fwrite(&a, 2, 1, f); std::fwrite(&b, 2, 1, f);
    }
    std::fclose(f);
}

// Harmonic levels H1..H15 of a sine at f0 through a setting, in dB re full scale.
struct Harm { double h[16]; double thd; };
static Harm harmonics(const Settings& s, double f0, float amp)
{
    const Buf in = sine(f0, amp, 1.5);
    const Buf o = runL(s, in);
    const size_t a = (size_t)(0.5 * SR), b = in.size();
    Harm r{};
    double hsum = 0;
    for (int k = 1; k <= 15; ++k)
    {
        r.h[k] = (k * f0 < SR / 2) ? toneDb(o, a, b, k * f0) : -200.0;
        if (k >= 2) hsum += std::pow(10.0, r.h[k] / 10.0);
    }
    r.thd = 100.0 * std::sqrt(hsum) / std::pow(10.0, r.h[1] / 20.0);
    return r;
}
static double rel(const Harm& h, int k) { return std::max(h.h[k] - h.h[1], -90.0); }   // dB re H1, floored

int main(int argc, char** argv)
{
    const double bpm = 100.0;
    const int bars = 4;
    const Buf dr = drums(bpm, bars), bs = bass(bpm, bars), sy = synth(bpm, bars), gt = guitar(bpm, bars);
    const Buf mx = sum4(dr, bs, sy, gt, 0.7f, 0.6f, 0.6f, 0.8f);
    struct Src { const char* name; const Buf* b; };
    const Src srcs[5] = {{"drums", &dr}, {"bass", &bs}, {"synth", &sy}, {"guitar", &gt}, {"mix", &mx}};
    std::printf("material (RMS / peak dBFS):");
    for (const Src& s : srcs) std::printf("  %s %.1f/%.1f", s.name, rmsDb(*s.b, 0, s.b->size()), peakDb(*s.b));
    std::printf("\n");

    // ---- 1: harmonic signatures ----
    std::printf("\n== Harmonic signature: 220 Hz sine at -12 dBFS, Drive 50, Bloom 0 (dB re H1) ==\n");
    struct Sig { const char* name; int spine; float bias; float drive; };
    const Sig sigs[] = {
        {"Gated Fuzz, Starve 0", kGated, 0, 50}, {"Gated Fuzz, Starve 70", kGated, 70, 50},
        {"Octave Fuzz, Oct 80", kOctave, 80, 50}, {"Torn Speaker, Tear 60", kSpeaker, 60, 50},
        {"Rectifier, Half 0", kRectifier, 0, 50}, {"Rectifier, Full 50", kRectifier, 50, 50},
        {"Wavefolder, Symm 0", kFolder, 0, 50}, {"Overdrive, Asym 0", kOverdrive, 0, 50},
    };
    const int nSig = (int)(sizeof(sigs) / sizeof(sigs[0]));
    Harm H[8];
    std::printf("%-24s  H1(dBFS)   H2     H3     H4     H5     H6     H7     H9    H11    H13   THD%%\n", "");
    for (int i = 0; i < nSig; ++i)
    {
        Settings s; s.spine = sigs[i].spine; s.bias = sigs[i].bias; s.drive = sigs[i].drive;
        H[i] = harmonics(s, 220.0, 0.25f);
        std::printf("%-24s  %6.1f  %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.0f\n", sigs[i].name, H[i].h[1],
                    rel(H[i], 2), rel(H[i], 3), rel(H[i], 4), rel(H[i], 5), rel(H[i], 6), rel(H[i], 7), rel(H[i], 9), rel(H[i], 11), rel(H[i], 13), H[i].thd);
    }
    {
        const Harm& gf = H[0]; const Harm& gs = H[1]; const Harm& oc = H[2]; const Harm& rh = H[4];
        const Harm& rf = H[5]; const Harm& wf = H[6]; const Harm& od = H[7];
        check(rel(gf, 3) > rel(gf, 2) + 15.0 && gf.thd > 30.0, "Gated Fuzz (Starve 0): hard, odd-harmonic fuzz (H3 >> H2, THD > 30 %)");
        check(rel(gs, 2) > rel(gf, 2) + 15.0 && rel(gs, 2) > -20.0, "Gated Fuzz starved: the second harmonic comes up (asymmetry) by > 15 dB");
        bool h2Top = true;
        for (int k = 3; k <= 13; ++k) if (oc.h[k] > oc.h[2]) h2Top = false;
        check(h2Top && rel(oc, 2) > -6.0, "Octave Fuzz: a strong second harmonic (within 6 dB of H1, above every higher one)");
        check(rel(rf, 2) > 15.0 && rel(rf, 3) < rel(rf, 2) - 20.0 && rel(rf, 5) < rel(rf, 2) - 20.0,
              "Rectifier full-wave: the fundamental all but vanishes (H2 > H1 + 15 dB), odd harmonics 20 dB under H2");
        check(rel(rh, 2) > -12.0 && rel(rh, 2) < 6.0, "Rectifier half-wave: fundamental and octave together (H2 within 12 dB of H1)");
        double folderTop = -200; for (int k = 2; k <= 15; ++k) folderTop = std::max(folderTop, wf.h[k]);
        check(folderTop > wf.h[1] - 6.0, "Wavefolder: the fold pushes energy up (an overtone within 6 dB of H1; no clipper gets past -9.5 dB)");
        double odHi = 0, gfHi = 0;
        for (int k = 7; k <= 13; ++k) { odHi += std::pow(10.0, rel(od, k) / 10.0); gfHi += std::pow(10.0, rel(gf, k) / 10.0); }
        check(od.thd < gf.thd * 0.6 && 10.0 * std::log10(odHi / gfHi) < -10.0,
              "Overdrive: smoother than the fuzz (lower THD, high harmonics H7-H13 > 10 dB lower)");
        // Every pair differs: RMS distance of the H2..H13 profiles (dB re H1).
        double minD = 1e9; int ma = 0, mb = 0;
        for (int i = 0; i < nSig; ++i)
            for (int j = i + 1; j < nSig; ++j)
            {
                double d = 0;
                for (int k = 2; k <= 13; ++k) { const double e = rel(H[i], k) - rel(H[j], k); d += e * e; }
                d = std::sqrt(d / 12.0);
                if (d < minD) { minD = d; ma = i; mb = j; }
            }
        std::printf("closest pair: %s vs %s, %.1f dB RMS apart\n", sigs[ma].name, sigs[mb].name, minD);
        check(minD > 5.0, "every Spine setting has its own harmonic profile (all pairs > 5 dB RMS apart)");
    }
    // Torn Speaker: level-dependent and asymmetric (60 Hz, quiet vs loud).
    {
        Settings s; s.spine = kSpeaker; s.bias = 60; s.drive = 30;
        const Harm q = harmonics(s, 80.0, 0.03f), l = harmonics(s, 80.0, 0.5f);
        std::printf("Torn Speaker at 80 Hz: THD %.1f %% at -30 dBFS, %.1f %% at -6 dBFS; H2 %.1f dB re H1 when loud\n", q.thd, l.thd, rel(l, 2));
        check(l.thd > 4.0 * q.thd && rel(l, 2) > -25.0, "Torn Speaker: level dependent (THD x4 louder) and asymmetric (H2 > -25 dB when loud)");
    }

    // ---- 1b: Starve is audible: a dying battery gates and sputters the decays ----
    std::printf("\n== Starve on the guitar (Gated Fuzz, Drive 50, Bloom 25): how much of each note survives ==\n");
    {
        const double spb = SR * 60.0 / bpm;
        double sil[4] = {}, diffDb[4] = {};
        Buf ref;
        const float starves[4] = {0, 35, 70, 100};
        for (int k = 0; k < 4; ++k)
        {
            Settings s; s.spine = kGated; s.drive = 50; s.bias = starves[k]; s.bloom = 25;
            const Buf o = runL(s, gt);
            if (k == 0) ref = o;
            size_t silent = 0, frames = 0;
            const size_t w = (size_t)(0.005 * SR);
            for (size_t i = (size_t)(2.0 * SR); i + w < o.size(); i += w)
            {
                double e = 0; for (size_t j = 0; j < w; ++j) e += (double)o[i + j] * o[i + j];
                ++frames; if (e / (double)w < 1e-6) ++silent;          // under -60 dBFS
            }
            sil[k] = (double)silent / (double)frames;
            double num = 0, den = 0;
            for (size_t i = (size_t)(2.0 * SR); i < o.size(); ++i) { const double d = o[i] - ref[i]; num += d * d; den += (double)ref[i] * ref[i]; }
            diffDb[k] = 10.0 * std::log10(num / (den + 1e-30) + 1e-30);
            std::printf("Starve %3.0f: silent %4.1f %% of the time, difference from Starve 0 %6.1f dB\n", starves[k], 100 * sil[k], diffDb[k]);
        }
        (void)spb;
        check(sil[2] > sil[0] + 0.10 && sil[3] > sil[2], "Starve gates: at 70 the decays are cut (10 % more silence than at 0), more again at 100");
        check(diffDb[1] > -12.0 && diffDb[2] > -6.0, "Starve changes the sound (35 vs 0: residual within 12 dB of the signal; 70 vs 0: within 6 dB)");
    }

    // ---- 2: aliasing ----
    std::printf("\n== Aliasing at Drive 100, -6 dBFS sine (non-harmonic power re total, 20 Hz-20 kHz) ==\n");
    {
        struct A { const char* name; int spine; float bias; double limit6k; };
        // The octave fuzz and the full-wave rectifier double the frequency, so a 6 kHz note is a
        // 12 kHz wave inside them: their 6 kHz limit is looser.
        const A as[] = {
            {"Gated Fuzz, Starve 0", kGated, 0, -40}, {"Gated Fuzz, Starve 70", kGated, 70, -40},
            {"Octave Fuzz, Oct 80", kOctave, 80, -30}, {"Torn Speaker, Tear 60", kSpeaker, 60, -40},
            {"Rectifier, Half 0", kRectifier, 0, -30}, {"Rectifier, Full 50", kRectifier, 50, -30},
            {"Rectifier, Fold 100", kRectifier, 100, -30}, {"Wavefolder, Symm 0", kFolder, 0, -40},
            {"Overdrive, Asym 0", kOverdrive, 0, -40},
        };
        bool ok6 = true, ok2 = true, better = true;
        std::printf("%-24s  %-26s  %-14s  %s\n", "", "6123 Hz: Urchin (worst bin)", "naive", "1987 Hz: Urchin");
        for (const A& a : as)
        {
            Settings s; s.spine = a.spine; s.bias = a.bias; s.drive = 100;
            double res[2], worstBin = 0, naive = 0;
            const double fs[2] = {6123.0, 1987.0};
            for (int fi = 0; fi < 2; ++fi)
            {
                const Buf in = sine(fs[fi], 0.5f, 2.2);
                const Buf o = runL(s, in);
                double wb;
                res[fi] = aliasDb(o, (size_t)(0.3 * SR), fs[fi], &wb);
                if (fi == 0)
                {
                    worstBin = wb;
                    // A naive reference: the same static curve at the base rate, no oversampling, no ADAA.
                    Urchin p; p.prepare(SR, 512); apply(p, s); p.reset();
                    float dc = 0.0f;
                    if (a.spine == kOctave)
                    {
                        double m = 0; for (size_t i = 0; i < in.size(); ++i) m += Urchin::shapeStatic(p.circ_[0], in[i], p.circ_[0].driveT, 0.0f, true);
                        dc = (float)(m / (double)in.size());
                    }
                    Buf nv(in.size());
                    for (size_t i = 0; i < in.size(); ++i) nv[i] = Urchin::shapeStatic(p.circ_[0], in[i], p.circ_[0].driveT, dc);
                    naive = aliasDb(nv, (size_t)(0.3 * SR), fs[fi]);
                }
            }
            std::printf("%-24s  %6.1f dB (%6.1f)            %6.1f dB      %6.1f dB\n", a.name, res[0], worstBin, naive, res[1]);
            if (res[0] > a.limit6k) ok6 = false;
            if (res[1] > -45.0) ok2 = false;
            if (res[0] > naive - 15.0) better = false;
        }
        check(ok6, "6 kHz at full Drive: aliasing under -40 dB (clippers, folder) / -30 dB (frequency doublers)");
        check(ok2, "2 kHz at full Drive: aliasing under -45 dB on every Spine");
        check(better, "at least 15 dB less aliasing than the same curves run naively");
    }

    // ---- 3: low end ----
    // A bass note: a 55 Hz fundamental with a 330 Hz partial on top. What happens to the 55 Hz?
    std::printf("\n== Low end: the 55 Hz fundamental of a bass note, change vs the input (dB) ==\n");
    {
        Buf in((size_t)(4.0 * SR));
        for (size_t i = 0; i < in.size(); ++i)
        {
            const double t = (double)i / SR;
            in[i] = (float)(0.35 * std::sin(6.283185307 * 55.0 * t) + 0.12 * std::sin(6.283185307 * 330.0 * t));
        }
        const size_t a = (size_t)(2.0 * SR);
        const double inF = toneDb(in, a, in.size(), 55.0);
        bool keepOk = true, blendOk = true, lostSomewhere = false;
        for (int sp = 0; sp < kNumSpines; ++sp)
        {
            Settings s; s.spine = sp; s.drive = 75; s.bias = sp == kRectifier ? 50.0f : 40.0f;
            const double plain = toneDb(runL(s, in), a, in.size(), 55.0) - inF;
            Settings k = s; k.lowKeep = 150;
            const double keep = toneDb(runL(k, in), a, in.size(), 55.0) - inF;
            Settings bl = s; bl.blend = 50;
            const double blend = toneDb(runL(bl, in), a, in.size(), 55.0) - inF;
            std::printf("%-12s  fuzz only %+6.1f   Low Keep 150 Hz %+6.1f   Blend 50 %+6.1f\n", kSpineNames[sp], plain, keep, blend);
            if (std::fabs(keep) > 1.5) keepOk = false;
            if (blend < -1.5) blendOk = false;
            if (plain < -6.0) lostSomewhere = true;
        }
        check(lostSomewhere, "without help, some Spines lose the bass fundamental (so the next checks mean something)");
        check(keepOk, "Low Keep 150 Hz: the 55 Hz fundamental comes through within 1.5 dB on every Spine");
        check(blendOk, "Blend 50: the 55 Hz fundamental is never lost (no more than 1.5 dB down) on every Spine");
        // For information: the kick drum's energy under 100 Hz.
        const size_t a2 = (size_t)(1.0 * SR);
        const double inLow = lowDb(dr, a2, dr.size(), 100.0);
        for (int sp : {kGated, kOctave, kRectifier})
        {
            Settings s; s.spine = sp; s.drive = 75; s.bias = 40;
            Settings k = s; k.lowKeep = 150; Settings bl = s; bl.blend = 50;
            std::printf("drums %-12s under 100 Hz: fuzz only %+5.1f, Low Keep 150 %+5.1f, Blend 50 %+5.1f dB\n", kSpineNames[sp],
                        lowDb(runL(s, dr), a2, dr.size(), 100.0) - inLow, lowDb(runL(k, dr), a2, dr.size(), 100.0) - inLow,
                        lowDb(runL(bl, dr), a2, dr.size(), 100.0) - inLow);
        }
    }

    // ---- 4: gate ----
    std::printf("\n== Gate ==\n");
    {
        // Guitar with a -66 dBFS hum-and-hiss floor between the notes; the fuzz makes it loud.
        Buf g = gt;
        for (size_t i = 0; i < g.size(); ++i) g[i] += 0.0005f * rnd() + 0.0003f * (float)std::sin(6.2831853 * 60.0 * (double)i / SR);
        // Silence the riff on beats 3-4 of every bar, so there are real gaps.
        const double spb = SR * 60.0 / bpm;
        for (size_t i = 0; i < g.size(); ++i)
        {
            const double beat = std::fmod((double)i / spb, 4.0);
            if (beat >= 2.6) g[i] = 0.0005f * rnd() + 0.0003f * (float)std::sin(6.2831853 * 60.0 * (double)i / SR);
        }
        auto stats = [&](float gate, double& gapDb, double& silentFrac, double& closesPerS)
        {
            Settings s; s.spine = kGated; s.drive = 70; s.bias = 0; s.gate = gate;
            const Buf o = runL(s, g);
            // gap level: beat 3.3-3.9 of each bar
            double gs = 0; size_t gn = 0, silent = 0, frames = 0; int closes = 0; bool wasOpen = true;
            const size_t w = (size_t)(0.005 * SR);
            for (size_t i = (size_t)SR; i + w < o.size(); i += w)
            {
                double e = 0; for (size_t j = 0; j < w; ++j) e += (double)o[i + j] * o[i + j];
                e /= (double)w;
                const double beat = std::fmod((double)i / spb, 4.0);
                if (beat > 3.3 && beat < 3.9) { gs += e; ++gn; }
                const bool isSilent = e < 1e-8;                     // -80 dBFS
                if (beat < 2.5) { ++frames; if (isSilent) ++silent; }   // inside the phrases
                if (wasOpen && isSilent) ++closes;
                wasOpen = !isSilent;
            }
            gapDb = 10.0 * std::log10(gs / (double)std::max<size_t>(gn, 1) + 1e-30);
            silentFrac = (double)silent / (double)frames;
            closesPerS = closes / ((double)(o.size() - SR) / SR);
        };
        double g0, s0, c0, g1, s1, c1, g2, s2, c2;
        stats(0, g0, s0, c0); stats(35, g1, s1, c1); stats(100, g2, s2, c2);
        std::printf("Gate Off:      gaps %6.1f dBFS, silent %4.0f %% of the phrases, %.1f closes/s\n", g0, 100 * s0, c0);
        std::printf("Gate 35 %%:     gaps %6.1f dBFS, silent %4.0f %% of the phrases, %.1f closes/s\n", g1, 100 * s1, c1);
        std::printf("Gate Chop 100: gaps %6.1f dBFS, silent %4.0f %% of the phrases, %.1f closes/s\n", g2, 100 * s2, c2);
        check(g0 > -70.0, "without the gate the hiss is there in the gaps (louder than -70 dBFS)");
        check(g1 < -90.0, "Gate 35 %: the gaps between phrases are silent (< -90 dBFS)");
        check(s2 > s1 + 0.2 && c2 > 1.5 * c1 && c2 > 1.5, "Gate Chop: it chops inside the phrases (20 % more silence there, at least 1.5 cuts per second, 1.5x the plain gate)");
    }

    // ---- 5: level across Drive and Spike ----
    std::printf("\n== Level: RMS change vs input (dB) across Drive 0/25/50/75/100 and Spike 100, peak dBFS (from 4 s, once the match settles) ==\n");
    {
        const size_t a = (size_t)(4.0 * SR);
        bool spreadOk = true, peakOk = true, nearOk = true;
        for (int sp = 0; sp < kNumSpines; ++sp)
            for (int si : {1, 3, 4})
            {
                const Buf& in = *srcs[si].b;
                const double inDb = rmsDb(in, a, in.size());
                double lo = 1e9, hi = -1e9, pk = -200;
                std::printf("%-12s %-6s", kSpineNames[sp], srcs[si].name);
                for (float d : {0.0f, 25.0f, 50.0f, 75.0f, 100.0f})
                {
                    Settings s; s.spine = sp; s.drive = d; s.bias = 40; s.bloom = 30;
                    Buf l, r; run(s, in, l, r);
                    const double dl = rmsDb(l, a, in.size()) - inDb;
                    lo = std::min(lo, dl); hi = std::max(hi, dl); pk = std::max(pk, peakDb(l, a));
                    std::printf(" %+5.1f", dl);
                }
                Settings s; s.spine = sp; s.drive = 100; s.spike = 100; s.bias = 40; s.bloom = 30;
                Buf l, r; run(s, in, l, r);
                const double dsp = rmsDb(l, a, in.size()) - inDb;
                lo = std::min(lo, dsp); hi = std::max(hi, dsp); pk = std::max(pk, peakDb(l, a));
                std::printf("  | Spike %+5.1f   spread %4.1f  peak %+5.1f\n", dsp, hi - lo, pk);
                if (hi - lo > 6.0) spreadOk = false;
                if (pk > 6.0) peakOk = false;
                if (lo < -8.0 || hi > 6.0) nearOk = false;
            }
        check(spreadOk, "no runaway: across Drive 0-100 and Spike 100 the level moves by 6 dB or less");
        check(nearOk, "the level stays near the input (-8 to +6 dB) everywhere");
        check(peakOk, "peaks stay under +6 dBFS at any Drive or Spike");
    }

    // ---- 6: programs ----
    std::printf("\n== Programs on the mix (RMS change vs input, peak, from 4 s) ==\n");
    {
        const size_t a = (size_t)(4.0 * SR);
        const double inDb = rmsDb(mx, a, mx.size());
        bool ok = true;
        Urchin q;
        for (int pi = 0; pi < (int)q.programs_.size(); ++pi)
        {
            Urchin p; p.prepare(SR, 512); p.setProgram(pi); p.reset();
            Buf l, r; render(p, mx, mx, 128, l, r);
            int bad, sub; const bool clean = cleanBuf(l, bad, sub) && cleanBuf(r, bad, sub);
            const double d = rmsDb(l, a, mx.size()) - inDb, pk = peakDb(l, a);
            std::printf("%-16s %+5.1f dB  peak %+5.1f dBFS%s\n", q.programs_[(size_t)pi].name.c_str(), d, pk, clean ? "" : "  NOT CLEAN");
            if (!clean || std::fabs(d) > 6.0 || pk > 6.0) ok = false;
        }
        check(ok, "every program is clean, within 6 dB of the input and under +6 dBFS");
    }

    // ---- 7: robustness: NaN, denormals, silence ----
    std::printf("\n== Robustness ==\n");
    {
        // Loud noise, then silence, then a vanishing signal, with every control jumping each block.
        Buf in((size_t)(6.0 * SR), 0.0f);
        for (size_t i = 0; i < (size_t)(2.0 * SR); ++i) in[i] = rnd();
        for (size_t i = (size_t)(4.0 * SR); i < in.size(); ++i) in[i] = 1.0e-30f * (float)std::sin((double)i * 0.01);
        bool allClean = true, silentOk = true, statesOk = true;
        for (int sp = 0; sp < kNumSpines; ++sp)
        {
            Settings s; s.spine = sp; s.drive = 100; s.spike = 100; s.bias = 100; s.bloom = 100; s.gate = 100;
            s.lowKeep = 800; s.focus = kLows; s.tone = 100; s.level = 12; s.output = 12;
            Urchin p; p.prepare(SR, 512); apply(p, s); p.reset();
            Buf l, r;
            render(p, in, in, 97, l, r, [](Urchin& u, size_t pos, size_t total) {
                if (pos < total / 3)
                    for (int k = 1; k < kNumParams; ++k) if (k != kSpine && rnd() > 0.7f) u.setNormalised(k, 0.5f + 0.5f * rnd());
            });
            int bad, sub;
            if (!cleanBuf(l, bad, sub) || !cleanBuf(r, bad, sub)) { allClean = false; std::printf("%s: %d bad, %d subnormal\n", kSpineNames[sp], bad, sub); }
            if (peakDb(l, (size_t)(3.5 * SR)) > -100.0) silentOk = false;
            for (int c = 0; c < 2; ++c)
            {
                const auto& k = p.core_[c];
                for (float v : {k.preHp.y1, k.postHp.y1, k.couple.y1, k.coneHp.y1, k.coneLp.z, k.tiltLp.z})
                    if (!std::isfinite(v) || (v != 0.0f && std::fabs(v) < 1.17549435e-38f)) statesOk = false;
            }
        }
        check(allClean, "no NaN, Inf or denormals at extreme settings with every control jumping");
        check(silentOk, "silence in, silence out (< -100 dBFS) after full-scale noise, on every Spine");
        check(statesOk, "filter states finite and not denormal after the vanishing signal");

        // Switching Spines mid-note does not click: no step bigger than the signal's own.
        Settings s; s.spine = kOverdrive; s.drive = 40;
        const Buf in2 = sine(110.0, 0.25f, 2.0);
        Buf l, r;
        run(s, in2, l, r, 64, [](Urchin& u, size_t pos, size_t) {
            const size_t step = (size_t)(0.25 * SR);
            u.set(kSpine, (float)((pos / step) % kNumSpines));
        });
        float maxStep = 0.0f, steadyStep = 0.0f; size_t worstAt = 0;
        for (size_t i = (size_t)(0.2 * SR); i < l.size(); ++i)
        {
            const float d = std::fabs(l[i] - l[i - 1]);
            const bool nearSwitch = (i % (size_t)(0.25 * SR)) < (size_t)(0.02 * SR);
            if (nearSwitch) { if (d > maxStep) { maxStep = d; worstAt = i; } } else steadyStep = std::max(steadyStep, d);
        }
        std::printf("Spine switching: biggest step at a switch %.4f (%.3f s), elsewhere %.4f\n", maxStep, worstAt / SR, steadyStep);
        check(maxStep <= steadyStep * 1.2f, "switching Spines mid-note is click-free (no step bigger than the signal's own)");

        // Mono: one core, L == R on a mono-compatible input.
        Settings m; m.stereo = 1; m.spine = kGated;
        Buf ml, mr; run(m, gt, ml, mr);
        double diff = 0; for (size_t i = 0; i < ml.size(); ++i) diff = std::max(diff, (double)std::fabs(ml[i] - mr[i]));
        check(diff < 1e-6, "Mono: both sides identical");
    }

    // ---- 7b: Split: a different Spine on each side ----
    std::printf("\n== Split (Spine R) on the guitar ==\n");
    {
        auto rel = [](const Buf& a, const Buf& b, size_t from) {
            double num = 0, den = 0;
            for (size_t i = from; i < a.size(); ++i) { const double d = a[i] - b[i]; num += d * d; den += (double)b[i] * b[i]; }
            return 10.0 * std::log10(num / (den + 1e-30) + 1e-30);
        };
        const size_t from = (size_t)(1.0 * SR);
        Settings sp; sp.spine = kOverdrive; sp.spineR = 1 + kGated; sp.drive = 55; sp.bias = 20; sp.bloom = 25;
        Buf sl, sr; run(sp, gt, sl, sr);
        Settings a = sp; a.spineR = 0; Buf al, ar; run(a, gt, al, ar);            // Overdrive on both sides
        Settings b = sp; b.spine = kGated; b.spineR = 0; Buf bl, br; run(b, gt, bl, br);   // Gated Fuzz on both
        const double dL = rel(sl, al, from), dR = rel(sr, bl, from), dLR = rel(sl, sr, from);
        std::printf("left vs Overdrive alone %.1f dB, right vs Gated Fuzz alone %.1f dB, left vs right %.1f dB\n", dL, dR, dLR);
        check(dL < -60.0 && dR < -60.0, "Split: the left side is exactly Spine, the right side exactly Spine R (residual under -60 dB)");
        check(dLR > -6.0, "Split: the two sides really differ (residual within 6 dB of the signal)");
        const double lvl = rmsDb(sl, from, sl.size()) - rmsDb(sr, from, sr.size());
        std::printf("left minus right level %+.1f dB\n", lvl);
        check(std::fabs(lvl) < 3.0, "Split: the two sides sit within 3 dB of each other");
        Settings w0 = sp; w0.width = 0; Buf wl, wr; run(w0, gt, wl, wr);
        double dmax = 0; for (size_t i = 0; i < wl.size(); ++i) dmax = std::max(dmax, (double)std::fabs(wl[i] - wr[i]));
        check(dmax < 1e-6, "Width 0: both sides identical (the two Spines summed in the middle)");
        Settings w50 = sp; w50.width = 50; Buf hl, hr; run(w50, gt, hl, hr);
        double side100 = 0, side50 = 0;
        for (size_t i = from; i < hl.size(); ++i) { side100 += std::pow(sl[i] - sr[i], 2); side50 += std::pow(hl[i] - hr[i], 2); }
        std::printf("Width 50: side signal %.1f dB re Width 100\n", 10.0 * std::log10(side50 / side100));
        check(std::fabs(10.0 * std::log10(side50 / side100) + 6.02) < 0.5, "Width 50 halves the side signal (-6 dB)");
        // Changing Spine R mid-note is click-free.
        Settings cs; cs.spine = kOverdrive; cs.drive = 40;
        const Buf in2 = sine(110.0, 0.25f, 2.0);
        Buf cl, cr;
        run(cs, in2, cl, cr, 64, [](Urchin& u, size_t pos, size_t) {
            const size_t step = (size_t)(0.25 * SR);
            u.set(kSpineR, (float)((pos / step) % (kNumSpines + 1)));
        });
        float maxStep = 0.0f, steadyStep = 0.0f;
        for (size_t i = (size_t)(0.2 * SR); i < cr.size(); ++i)
        {
            const float d = std::fabs(cr[i] - cr[i - 1]);
            const bool nearSwitch = (i % (size_t)(0.25 * SR)) < (size_t)(0.02 * SR);
            if (nearSwitch) maxStep = std::max(maxStep, d); else steadyStep = std::max(steadyStep, d);
        }
        std::printf("Spine R switching: biggest step at a switch %.4f, elsewhere %.4f\n", maxStep, steadyStep);
        check(maxStep <= steadyStep * 1.2f, "switching Spine R mid-note is click-free");
    }

    // ---- 8: CPU ----
    std::printf("\n== CPU (block 128, 16 s of the mix, stereo input) ==\n");
    {
        Buf big; for (int k = 0; k < 4; ++k) big.insert(big.end(), mx.begin(), mx.end());
        const double audio = (double)big.size() / SR;
        Buf rnd2 = big; for (size_t i = 0; i < rnd2.size(); ++i) rnd2[i] = big[(i + 7919) % big.size()];   // decorrelated R
        double worst = 0, worstMono = 0;
        for (int sp = 0; sp < kNumSpines; ++sp)
            for (int st = 0; st < 2; ++st)
            {
                Settings s; s.spine = sp; s.stereo = st; s.drive = 70; s.bias = 50; s.bloom = 50; s.gate = 30; s.lowKeep = 120; s.focus = kMids; s.blend = 70; s.tone = 20;
                Urchin p; p.prepare(SR, 512); apply(p, s); p.reset();
                Buf l, r;
                const auto t0 = std::chrono::steady_clock::now();
                render(p, big, rnd2, 128, l, r);
                const double pct = 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / audio;
                std::printf("%-12s %-6s %.3f %% of one desktop core\n", kSpineNames[sp], st ? "mono" : "stereo", pct);
                if (st) worstMono = std::max(worstMono, pct); else worst = std::max(worst, pct);
            }
        std::printf("worst stereo %.3f %%, worst mono %.3f %%; MPC estimate (x15 to x30 slower per core): stereo %.1f-%.1f %%, mono %.1f-%.1f %%\n",
                    worst, worstMono, worst * 15, worst * 30, worstMono * 15, worstMono * 30);
        check(worst < 0.45, "CPU under 0.45 % of a desktop core in stereo with everything on, worst Spine");
        check(worstMono < 0.7 * worst, "Mono saves CPU (under 70 % of stereo)");
        double worstSplit = 0;
        for (int sp = 0; sp < kNumSpines; ++sp)
        {
            Settings s; s.spine = sp; s.spineR = 1 + kOctave; s.drive = 70; s.bias = 50; s.bloom = 50; s.gate = 30; s.lowKeep = 120; s.focus = kMids; s.blend = 70; s.tone = 20;
            Urchin p; p.prepare(SR, 512); apply(p, s); p.reset();
            Buf l, r;
            const auto t0 = std::chrono::steady_clock::now();
            render(p, big, rnd2, 128, l, r);
            worstSplit = std::max(worstSplit, 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / audio);
        }
        std::printf("Split (each Spine with the Octave Fuzz on the right): worst %.3f %%\n", worstSplit);
        check(worstSplit < 0.45, "Split costs no more than Stereo (under 0.45 %)");
    }

    // ---- demo ----
    if (argc > 1)
    {
        Buf L, R;
        auto append = [&](const Buf& a, const Buf& b) { L.insert(L.end(), a.begin(), a.end()); R.insert(R.end(), b.begin(), b.end()); };
        auto gap = [&](double s) { Buf z((size_t)(s * SR), 0.0f); append(z, z); };
        const int b2 = 2;
        const Buf d2 = drums(bpm, b2), ba2 = bass(bpm, b2), s2 = synth(bpm, b2), g2 = guitar(bpm, b2);
        const Buf m4 = sum4(drums(bpm, 4), bass(bpm, 4), synth(bpm, 4), guitar(bpm, 4), 0.7f, 0.6f, 0.6f, 0.8f);
        double tcur = 0;
        std::printf("\ndemo sections (%.0f BPM, 2 bars = %.1f s):\n", bpm, d2.size() / SR);
        auto section = [&](const char* what, const Buf& in, int program, Automation aut = nullptr, const Settings* s = nullptr)
        {
            std::printf("  %6.1f s  %s\n", tcur, what);
            if (program < 0 && !s) append(in, in);
            else
            {
                Urchin p; p.prepare(SR, 512);
                if (s) apply(p, *s); else p.setProgram(program);
                p.reset();
                Buf l, r; render(p, in, in, 128, l, r, aut);
                append(l, r);
            }
            tcur += in.size() / SR;
            gap(0.5); tcur += 0.5;
        };
        section("dry: drums, bass, synth and guitar together", m4, -1);
        // One program per Spine, each on the four sources.
        struct P { int prog; const char* spine; };
        const P per[6] = {{2, "Gated Fuzz (Dying Battery)"}, {3, "Octave Fuzz (Octave Spit)"}, {4, "Torn Speaker"},
                          {1, "Rectifier (Bass Grind)"}, {5, "Wavefolder (Folded Synth)"}, {6, "Overdrive (Gentle Hair)"}};
        const Src four[4] = {{"drums", &d2}, {"bass", &ba2}, {"synth", &s2}, {"guitar", &g2}};
        for (const P& p : per)
            for (const Src& s : four)
            {
                const std::string w = std::string(p.spine) + ": " + s.name;
                section(w.c_str(), *s.b, p.prog);
            }
        section("Drum Smasher: drums", d2, 0);
        // Starve: 0 -> 100 over 4 bars of guitar, so the battery dies as you listen.
        auto starveRamp = [](Urchin& u, size_t pos, size_t total) { u.set(kBias, 100.0f * (float)pos / (float)total); };
        Settings st; st.spine = kGated; st.drive = 50; st.bloom = 25; st.tone = -10;
        section("Starve 0 -> 100 on the guitar (Gated Fuzz, Drive 50)", guitar(bpm, 4), -1, starveRamp, &st);
        // Spike: 0 -> 100 over the section, on the mix and on the guitar.
        auto spikeRamp = [](Urchin& u, size_t pos, size_t total) { u.set(kSpike, 100.0f * (float)pos / (float)total); };
        Settings sk; sk.spine = kGated; sk.drive = 45; sk.bias = 20; sk.bloom = 25; sk.tone = -10;
        section("Spike 0 -> 100 on the mix (Gated Fuzz, Drive 45)", m4, -1, spikeRamp, &sk);
        Settings so; so.spine = kOctave; so.drive = 40; so.bias = 60; so.bloom = 20;
        const Buf g4 = guitar(bpm, 4);
        section("Spike 0 -> 100 on the guitar (Octave Fuzz, Drive 40)", g4, -1, spikeRamp, &so);
        const Buf g4b = guitar(bpm, 4);
        section("Split, Twin Amps: guitar (Overdrive left, Gated Fuzz right)", g4b, 8);
        section("Split, Split Spit: guitar (Octave Fuzz left, Torn Speaker right, Width 80)", g4b, 9);
        section("Total Urchin: the mix", m4, 7);
        // One gain for the whole file (sections keep their levels relative to each other): peak -1 dBFS.
        float pk = 0.0f;
        for (size_t i = 0; i < L.size(); ++i) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
        const float g = pk > 0.891f ? 0.891f / pk : 1.0f;
        for (size_t i = 0; i < L.size(); ++i) { L[i] *= g; R[i] *= g; }
        writeWav(argv[1], L, R);
        std::printf("wrote %s (%.1f s; raw peak %+.1f dBFS, scaled by %.1f dB to peak at -1 dBFS)\n", argv[1], L.size() / SR,
                    20.0 * std::log10(pk), 20.0 * std::log10(g));
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
