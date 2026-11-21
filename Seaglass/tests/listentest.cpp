// Does Seaglass freeze cleanly, shimmer upward without running away, swell in the gaps, and stay
// click-free and under 0 dBFS? Feeds stereo material (a synthesised pad chord, a sine and a drum
// loop), measures the output, and renders a demo WAV.
//
//   mkdir -p tests/build
//   clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
//   tests/build/listentest build/seaglass-demo.wav
//
// (Run from plugins/Seaglass/.) The harness and click detector follow Abyss's tests/listentest.cpp.
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <complex>
#include <string>
#include <memory>
#include <chrono>
#include <algorithm>

#define private public
#define protected public
#include "../src/Seaglass.cpp"
#undef private
#undef protected

static const double SR = 44100.0;
static int failures = 0;
static void check(bool ok, const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", buf);
    if (!ok) ++failures;
}

static void writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r)
{
    FILE* f = std::fopen(path, "wb"); if (!f) { std::printf("could not write %s\n", path); return; }
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

// ---- test material (stereo) -----------------------------------------------------------------------
struct Stereo { std::vector<float> L, R; };
static Stereo silence(double seconds) { Stereo s; s.L.assign((size_t)(seconds * SR), 0.0f); s.R = s.L; return s; }

static float envAt(double t, double t0, double t1, double att, double rel);
// Steady stereo noise (independent left and right, gently low-passed), for level measurements.
static void addNoise(Stereo& s, double t0, double t1, float level = 0.1f)
{
    unsigned a = 4242, b = 9191;
    float lpL = 0.0f, lpR = 0.0f;
    const size_t i0 = (size_t)(t0 * SR), i1 = std::min(s.L.size(), (size_t)((t1 + 0.2) * SR) + 1);
    for (size_t i = i0; i < i1; ++i)
    {
        a = a * 1664525u + 1013904223u; b = b * 22695477u + 1u;
        const float nl = (float)((a >> 8) & 0xFFFF) / 32768.0f - 1.0f, nr = (float)((b >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        lpL += 0.3f * (nl - lpL); lpR += 0.3f * (nr - lpR);
        const float e = envAt(i / SR, t0, t1, 0.1, 0.2) * level;
        s.L[i] += e * lpL; s.R[i] += e * lpR;
    }
}

// A smooth envelope: raised-cosine attack and release, so the material itself never clicks.
static float envAt(double t, double t0, double t1, double att, double rel)
{
    if (t < t0 || t > t1 + rel) return 0.0f;
    double a = t < t0 + att ? 0.5 - 0.5 * std::cos(M_PI * (t - t0) / att) : 1.0;
    if (t > t1) a *= 0.5 + 0.5 * std::cos(M_PI * (t - t1) / rel);
    return (float)a;
}
static double midiHz(int n) { return 440.0 * std::pow(2.0, (n - 69) / 12.0); }

// A soft pad chord: six harmonics per note, detuned a few cents apart between left and right.
static void addChord(Stereo& s, std::initializer_list<int> notes, double t0, double t1, float level = 0.07f,
                     double att = 0.25, double rel = 0.4)
{
    for (int note : notes)
    {
        const double f = midiHz(note);
        const double fl = f * std::pow(2.0, 3.0 / 1200.0), fr = f * std::pow(2.0, -3.0 / 1200.0);
        const size_t i0 = (size_t)(t0 * SR), i1 = std::min(s.L.size(), (size_t)((t1 + rel) * SR) + 1);
        for (size_t i = i0; i < i1; ++i)
        {
            const double t = i / SR;
            const float e = envAt(t, t0, t1, att, rel) * level;
            double l = 0.0, r = 0.0;
            for (int h = 1; h <= 6; ++h)
            {
                if (f * h > 12000.0) break;
                l += std::sin(2.0 * M_PI * fl * h * t) / h;
                r += std::sin(2.0 * M_PI * fr * h * t + 0.3 * h) / h;
            }
            s.L[i] += e * (float)l; s.R[i] += e * (float)r;
        }
    }
}

static void addSine(Stereo& s, double hz, double t0, double t1, float level = 0.25f)
{
    const size_t i0 = (size_t)(t0 * SR), i1 = std::min(s.L.size(), (size_t)((t1 + 0.1) * SR) + 1);
    for (size_t i = i0; i < i1; ++i)
    {
        const double t = i / SR;
        const float e = envAt(t, t0, t1, 0.05, 0.1) * level;
        s.L[i] += e * (float)std::sin(2.0 * M_PI * hz * t);
        s.R[i] += e * (float)std::sin(2.0 * M_PI * hz * t + 0.5);
    }
}

// A drum loop: kick on every beat, snare on 2 and 4, eighth-note hats panned left and right.
static void addDrums(Stereo& s, double t0, double t1, double bpm = 96.0, float level = 1.0f)
{
    unsigned rs = 777;
    auto noise = [&]() { rs = rs * 1664525u + 1013904223u; return (float)((rs >> 8) & 0xFFFF) / 32768.0f - 1.0f; };
    const double eighth = 30.0 / bpm;
    int step = 0;
    for (double t = t0; t < t1 - 1e-9; t += eighth, ++step)
    {
        const size_t i0 = (size_t)(t * SR);
        const bool beat = step % 2 == 0, back = step % 4 == 2;
        float hp = 0.0f, prev = 0.0f;
        for (size_t k = 0; k < (size_t)(0.4 * SR) && i0 + k < s.L.size(); ++k)
        {
            const double dt = k / SR;
            const float att = (float)std::min(1.0, dt / 0.0015);   // 1.5 ms attack: a hit, not a click
            float l = 0.0f, r = 0.0f;
            if (beat)
            {
                const double ph = 2.0 * M_PI * (45.0 * dt + (120.0 - 45.0) * 0.04 * (1.0 - std::exp(-dt / 0.04)));
                const float k1 = 0.55f * (float)std::exp(-dt / 0.22) * (float)std::sin(ph);
                l += k1; r += k1;
            }
            const float nz = noise();
            if (back)
            {
                const float sn = (float)std::exp(-dt / 0.11) * (0.22f * nz + 0.18f * (float)std::sin(2.0 * M_PI * 185.0 * dt));
                l += sn; r += sn;
            }
            hp = nz - prev; prev = nz;
            const float hat = 0.08f * (float)std::exp(-dt / 0.03) * hp;
            if (step % 2 == 0) { l += hat; r += 0.4f * hat; } else { l += 0.4f * hat; r += hat; }
            s.L[i0 + k] += att * l * level; s.R[i0 + k] += att * r * level;
        }
    }
}

// ---- rendering ------------------------------------------------------------------------------------
struct Pc { double t; int param; float plain; };

// Processes `in` in place in random blocks of 32..512 frames (like the MPC), applying parameter
// changes between blocks.
static Stereo render(Seaglass& p, const Stereo& in, std::vector<Pc> pcs = {}, double* cpuSec = nullptr)
{
    std::sort(pcs.begin(), pcs.end(), [](const Pc& a, const Pc& b) { return a.t < b.t; });
    Stereo out = in;
    const int total = (int)in.L.size();
    unsigned rs = 12345;
    size_t pi = 0;
    double cpu = 0.0;
    for (int pos = 0; pos < total;)
    {
        rs = rs * 1664525u + 1013904223u;
        const int block = std::min(32 + (int)((rs >> 8) % 481), total - pos);
        while (pi < pcs.size() && (int)(pcs[pi].t * SR) <= pos) { p.set(pcs[pi].param, pcs[pi].plain); ++pi; }
        float* bufs[2] = {out.L.data() + pos, out.R.data() + pos};
        mpc::ProcessContext ctx;
        ctx.inputs = bufs; ctx.outputs = bufs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = block;
        ctx.sampleRate = SR;
        const auto t0 = std::chrono::steady_clock::now();
        p.process(ctx);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        pos += block;
    }
    if (cpuSec) *cpuSec = cpu;
    return out;
}

static void loadProgram(Seaglass& p, int prog)
{
    const mpc::Program& pr = p.programs_[(size_t)prog];
    for (int i = 0; i < p.numParams(); ++i) p.set(i, pr.plainValues[(size_t)i]);
}

static std::unique_ptr<Seaglass> make(int prog = 0, std::initializer_list<std::pair<int, float>> overrides = {})
{
    auto p = std::make_unique<Seaglass>();
    p->prepare(SR, 512);
    loadProgram(*p, prog);
    for (const auto& o : overrides) p->set(o.first, o.second);
    p->reset();
    return p;
}

// ---- analysis -------------------------------------------------------------------------------------
static void fft(std::vector<std::complex<float>>& a)
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
        const float ang = -6.2831853f / (float)len;
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Averaged power spectrum of the mono mix over [t0, t1): frames of 8192 (5.4 Hz bins), Hann window.
static std::vector<double> spectrum(const Stereo& s, double t0, double t1)
{
    const int N = 8192;
    std::vector<double> power(N / 2, 0.0);
    for (long start = (long)(t0 * SR); start + N <= (long)(t1 * SR) && start + N <= (long)s.L.size(); start += N / 2)
    {
        std::vector<std::complex<float>> a(N);
        for (int i = 0; i < N; ++i)
        {
            const float x = 0.5f * (s.L[(size_t)(start + i)] + s.R[(size_t)(start + i)]);
            const float w = 0.5f - 0.5f * std::cos(6.2831853f * (float)i / (float)N);
            a[i] = std::complex<float>(x * w, 0.0f);
        }
        fft(a);
        for (int k = 0; k < N / 2; ++k) power[k] += std::norm(a[k]);
    }
    return power;
}
static double bandDb(const std::vector<double>& pw, double lo, double hi)
{
    const int N = 8192;
    double e = 1e-20;
    for (int k = std::max(1, (int)(lo * N / SR)); k <= (int)(hi * N / SR) && k < N / 2; ++k) e += pw[(size_t)k];
    return 10.0 * std::log10(e);
}

// Third-octave spectral shape (dB relative to the total) and its distance to another.
static std::vector<float> shapeOf(const Stereo& s, double t0, double t1)
{
    const auto pw = spectrum(s, t0, t1);
    double tot = 1e-30; for (double v : pw) tot += v;
    std::vector<float> bands;
    for (double fc = 50.0; fc < 14000.0; fc *= 1.259921)
        bands.push_back((float)(bandDb(pw, fc / 1.122462, fc * 1.122462) - 10.0 * std::log10(tot)));
    return bands;
}
static float shapeDistance(const std::vector<float>& a, const std::vector<float>& b)
{
    float top = -1e9f;
    for (size_t i = 0; i < a.size(); ++i) top = std::max(top, std::max(a[i], b[i]));
    float sum = 0.0f; int n = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::max(a[i], b[i]) < top - 40.0f) continue;
        sum += std::fabs(std::max(a[i], top - 40.0f) - std::max(b[i], top - 40.0f)); ++n;
    }
    return n ? sum / (float)n : 0.0f;
}

static float db(double x) { return (float)(20.0 * std::log10(std::max(x, 1e-10))); }
static double rmsOf(const Stereo& s, double t0, double t1)
{
    double e = 0.0; long n = 0;
    for (size_t i = (size_t)(t0 * SR); i < (size_t)(t1 * SR) && i < s.L.size(); ++i) { e += (double)s.L[i] * s.L[i] + (double)s.R[i] * s.R[i]; n += 2; }
    return n ? std::sqrt(e / (double)n) : 0.0;
}
static float peakOf(const Stereo& s)
{
    float m = 0.0f;
    for (size_t i = 0; i < s.L.size(); ++i) m = std::max(m, std::max(std::fabs(s.L[i]), std::fabs(s.R[i])));
    return m;
}
static bool finite(const Stereo& s)
{
    for (size_t i = 0; i < s.L.size(); ++i) if (!std::isfinite(s.L[i]) || !std::isfinite(s.R[i])) return false;
    return true;
}
static double diffRatioDb(const Stereo& a, const Stereo& b, double t0, double t1)
{
    double e = 0.0, d = 0.0;
    for (size_t i = (size_t)(t0 * SR); i < (size_t)(t1 * SR) && i < a.L.size(); ++i)
    {
        e += (double)a.L[i] * a.L[i] + (double)a.R[i] * a.R[i];
        const double x = a.L[i] - b.L[i], y = a.R[i] - b.R[i];
        d += x * x + y * y;
    }
    return 10.0 * std::log10((d + 1e-30) / (e + 1e-30));
}

// Clicks: a click is a jump in value or slope, which shows as a spike in the second difference.
// Compare each 2 ms window's largest second difference against the median of its neighbours over
// +-50 ms; 3x counts as a click (from Abyss). Use it on smooth material (pads, tails), not drums.
static int countClicks(const std::vector<float>& x, double& worstT, float& worstRatio, double from = 0.0)
{
    const int W = (int)(0.002 * SR);
    const int nw = (int)x.size() / W;
    std::vector<float> mx((size_t)nw, 0.0f);
    for (int w = 0; w < nw; ++w)
        for (int i = std::max(2, w * W); i < (w + 1) * W; ++i)
            mx[(size_t)w] = std::max(mx[(size_t)w], std::fabs(x[(size_t)i] - 2.0f * x[(size_t)i - 1] + x[(size_t)i - 2]));
    int clicks = 0; worstRatio = 0.0f; worstT = 0.0;
    std::vector<float> nb;
    for (int w = (int)(from * SR / W); w < nw; ++w)
    {
        nb.clear();
        for (int j = std::max(0, w - 25); j <= std::min(nw - 1, w + 25); ++j) if (std::abs(j - w) > 1) nb.push_back(mx[(size_t)j]);
        if (nb.empty()) continue;
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end());
        const float med = nb[nb.size() / 2];
        const float ratio = mx[(size_t)w] / std::max(med, 1e-9f);
        if (mx[(size_t)w] > 2e-4f && ratio > worstRatio) { worstRatio = ratio; worstT = (double)w * W / SR; }
        if (mx[(size_t)w] > 2e-4f && ratio > 3.0f) ++clicks;
    }
    return clicks;
}
static void checkClicks(const Stereo& s, const char* what, double from = 0.0)
{
    double tl, tr; float rl, rr;
    const int cl = countClicks(s.L, tl, rl, from), cr = countClicks(s.R, tr, rr, from);
    check(cl + cr == 0, "%s: no clicks (worst jump %.1fx its surroundings at %.2f s)", what,
          rl > rr ? rl : rr, rl > rr ? tl : tr);
}

// Largest change of level between consecutive 100 ms windows in [t0, t1).
static float maxStepDb(const Stereo& s, double t0, double t1, double win = 0.1)
{
    float worst = 0.0f;
    float prev = db(rmsOf(s, t0, t0 + win));
    for (double t = t0 + win; t + win <= t1; t += win)
    {
        const float cur = db(rmsOf(s, t, t + win));
        worst = std::max(worst, std::fabs(cur - prev));
        prev = cur;
    }
    return worst;
}

using SG = Seaglass;

int main(int argc, char** argv)
{
    Seaglass probe; probe.prepare(SR, 512);
    const int numPrograms = (int)probe.programs_.size();
    std::printf("Seaglass listen test: %d parameters, %d programs\n\n", probe.numParams(), numPrograms);

    // A shared piece of input: 6 s of drums and a pad chord, then 4 s of silence.
    Stereo mixIn = silence(10.0);
    addDrums(mixIn, 0.0, 6.0, 96.0, 0.6f);
    addChord(mixIn, {48, 55, 59, 62, 64}, 0.0, 5.6, 0.045f);
    std::printf("INFO  test mix (drums + chord) peaks at %.2f dBFS\n", db(peakOf(mixIn)));

    // ---- 1. Every program: finite, under 0 dBFS, and a tail after the input stops -------------------
    std::printf("== 1. Programs with drums and a chord ==\n");
    for (int pr = 0; pr < numPrograms; ++pr)
    {
        auto p = make(pr);
        Stereo out = render(*p, mixIn);
        const float tailDb = db(rmsOf(out, 6.7, 7.2));
        check(finite(out) && peakOf(out) < 1.0f && tailDb > -60.0f,
              "program %-14s finite, peak %.2f dBFS, tail %.1f dBFS 1 s after the input stops",
              p->programs_[(size_t)pr].name.c_str(), db(peakOf(out)), tailDb);
    }

    // ---- 2. Every control on its own changes the sound --------------------------------------------
    std::printf("\n== 2. Every control does something on its own ==\n");
    {
        // pcsB: parameter changes for the second render only (for Add, which acts on a change, not a value).
        struct Ctl { int param; float a, b; std::vector<std::pair<int, float>> ctx; std::vector<Pc> pcs; std::vector<Pc> pcsB = {}; };
        const std::vector<Ctl> ctls = {
            {SG::kMix, 10, 90, {}, {}},
            {SG::kDecay, 1, 15, {{SG::kMix, 60}}, {}},
            {SG::kPreDelay, 0, 200, {{SG::kMix, 60}}, {}},
            {SG::kTone, 1500, 14000, {{SG::kMix, 60}}, {}},
            {SG::kLowCut, 20, 800, {{SG::kMix, 60}}, {}},
            {SG::kMotion, 0, 100, {{SG::kMix, 60}}, {}},
            {SG::kWidth, 0, 100, {{SG::kMix, 60}}, {}},
            {SG::kOutput, -12, 0, {}, {}},
            {SG::kFreeze, 0, 0, {{SG::kMix, 60}, {SG::kLive, 0}}, {}, {{3.0, SG::kFreeze, 1}}},
            {SG::kFade, 20, 3000, {{SG::kMix, 60}}, {{3.0, SG::kFreeze, 1}}},
            {SG::kShimmer, 0, 80, {{SG::kMix, 60}}, {}},
            {SG::kInterval, kIvOctave, kIvFifth, {{SG::kMix, 60}, {SG::kShimmer, 70}}, {}},
            {SG::kSwell, 0, 100, {{SG::kMix, 60}}, {}},
            {SG::kRise, 0.2f, 6.0f, {{SG::kMix, 60}, {SG::kSwell, 100}}, {}},
            {SG::kSense, -60, -20, {{SG::kMix, 60}, {SG::kSwell, 100}}, {}},
            {SG::kAdd, 0, 0, {{SG::kMix, 60}, {SG::kLive, 0}}, {{1.0, SG::kFreeze, 1}}, {{4.0, SG::kAdd, 1}}},
            {SG::kLive, 0, 100, {{SG::kMix, 60}}, {{1.0, SG::kFreeze, 1}}},
            {SG::kShift, 0.0f, shiftForSemis(7), {{SG::kMix, 60}, {SG::kShimmer, 70}}, {}},
        };
        check((int)ctls.size() == SG::kNumParams, "the control list covers all %d parameters", (int)SG::kNumParams);
        for (const Ctl& c : ctls)
        {
            Stereo outs[2];
            for (int side = 0; side < 2; ++side)
            {
                auto p = make(0);
                for (const auto& o : c.ctx) p->set(o.first, o.second);
                p->set(c.param, side ? c.b : c.a);
                p->reset();
                std::vector<Pc> pcs = c.pcs;
                if (side) pcs.insert(pcs.end(), c.pcsB.begin(), c.pcsB.end());
                outs[side] = render(*p, mixIn, pcs);
            }
            const double d = diffRatioDb(outs[0], outs[1], 0.0, 10.0);
            if (c.pcsB.empty())
                check(d > -30.0, "%-10s %g -> %g changes the output by %.1f dB (relative)", probe.param(c.param).name.c_str(),
                      (double)c.a, (double)c.b, d);
            else
                check(d > -30.0, "%-10s switched during the render changes the output by %.1f dB (relative)",
                      probe.param(c.param).name.c_str(), d);
        }
    }

    // ---- 3. Freeze -----------------------------------------------------------------------------------
    std::printf("\n== 3. Freeze ==\n");
    {
        // Wet only, Live 0 % (only the pad is heard while frozen, as in 1.0). A chord plays from 0 to 6 s;
        // Freeze goes on at 3 s and off at 36 s.
        Stereo in = silence(46.0);
        addChord(in, {50, 57, 60, 65, 69}, 0.0, 6.0, 0.08f);
        auto run = [&](float fadeMs, std::vector<Pc> extra, const Stereo& input) {
            auto p = make(0, {{SG::kMix, 100}, {SG::kFade, fadeMs}, {SG::kLive, 0}});
            std::vector<Pc> pcs = {{3.0, SG::kFreeze, 1}, {36.0, SG::kFreeze, 0}};
            for (const Pc& e : extra) pcs.push_back(e);
            return render(*p, input, pcs);
        };
        Stereo out = run(250, {}, in);
        check(finite(out) && peakOf(out) < 1.0f, "frozen chord finite, peak %.2f dBFS", db(peakOf(out)));
        // Holds steady for 30 s: 1 s windows from 4 s to 35 s.
        float lo = 1e9f, hi = -1e9f;
        for (double t = 4.0; t + 1.0 <= 35.0; t += 1.0) { const float v = db(rmsOf(out, t, t + 1.0)); lo = std::min(lo, v); hi = std::max(hi, v); }
        check(hi - lo < 1.5f, "holds its level for 31 s (1 s windows within %.2f dB: %.1f .. %.1f dBFS)", hi - lo, lo, hi);
        const float drift = db(rmsOf(out, 33.0, 35.0)) - db(rmsOf(out, 4.0, 6.0));
        check(std::fabs(drift) < 1.0f, "no drift: 33-35 s is %+.2f dB against 4-6 s", drift);
        const float sd = shapeDistance(shapeOf(out, 4.0, 7.0), shapeOf(out, 32.0, 35.0));
        check(sd < 1.5f, "the spectrum holds too (third-octave shape moves %.2f dB on average)", sd);
        // Engaging and releasing: no level jump. Steady noise plays straight through, so the reverb
        // sits at a steady level. With Live 0 freezing holds that level; releasing brings the live
        // reverb (which kept listening) back up while the pad dies away, so the level may rise by up to
        // 3 dB (two uncorrelated reverbs) before it settles - smoothly. With Live 100 the live reverb
        // refills beside the pad, so the level rises smoothly by about 3 dB.
        {
            Stereo nz = silence(40.0);
            addNoise(nz, 0.0, 39.0);
            for (float fadeMs : {250.0f, 20.0f})
            {
                auto pn = make(0, {{SG::kMix, 100}, {SG::kFade, fadeMs}, {SG::kLive, 0}});
                Stereo o = render(*pn, nz, {{10.0, SG::kFreeze, 1}, {30.0, SG::kFreeze, 0}});
                const float ref = db(rmsOf(o, 6.0, 10.0));
                float dev = 0.0f, lo = 0.0f, hi = 0.0f;
                for (double t = 8.0; t + 0.1 <= 14.0; t += 0.1) dev = std::max(dev, std::fabs(db(rmsOf(o, t, t + 0.1)) - ref));
                for (double t = 26.0; t + 0.1 <= 34.0; t += 0.1) { const float v = db(rmsOf(o, t, t + 0.1)) - ref; lo = std::min(lo, v); hi = std::max(hi, v); }
                const float settled = db(rmsOf(o, 36.0, 39.0)) - ref;
                const float steady = maxStepDb(o, 15.0, 25.0), eng = maxStepDb(o, 9.0, 12.0), rel = maxStepDb(o, 29.0, 32.0);
                check(dev < 1.5f && eng < steady + 0.5f,
                      "Fade %4.0f ms, Live 0, noise playing through: engaging holds the level (100 ms windows within %.2f dB; largest step %.2f, while held %.2f dB)",
                      fadeMs, dev, eng, steady);
                // (Over a 20 ms Fade the returning live reverb rises within one window: a fade, not a click.)
                check(rel < (fadeMs > 100.0f ? 1.5f : 3.5f) && lo > -1.5f && hi < 3.5f && std::fabs(settled) < 1.5f,
                      "Fade %4.0f ms, Live 0: releasing is smooth (largest step %.2f dB, level %+.1f .. %+.1f dB, settles at %+.1f dB)",
                      fadeMs, rel, lo, hi, settled);
                checkClicks(o, fadeMs > 100.0f ? "freeze in and out with noise playing (Fade 250 ms, Live 0)" : "freeze in and out with noise playing (Fade 20 ms, Live 0)");
            }
            auto pl = make(0, {{SG::kMix, 100}});
            Stereo o = render(*pl, nz, {{10.0, SG::kFreeze, 1}, {30.0, SG::kFreeze, 0}});
            const float ref = db(rmsOf(o, 6.0, 10.0)), both = db(rmsOf(o, 14.0, 29.0)) - ref;
            const float eng = maxStepDb(o, 9.0, 14.0), rel = maxStepDb(o, 29.0, 34.0);
            check(eng < 1.5f && rel < 1.5f && both > 2.0f && both < 4.0f,
                  "Live 100, noise playing through: pad + live reverb sit %+.1f dB over the reverb alone, reached smoothly (largest step %.2f dB in, %.2f dB out)",
                  both, eng, rel);
            checkClicks(o, "freeze in and out with noise playing (Live 100)");
        }
        // Releases: the pad then decays with the Decay time (5 s RT60).
        const float fall = db(rmsOf(out, 41.0, 42.0)) - db(rmsOf(out, 35.0, 36.0));
        check(fall < -30.0f && fall > -80.0f, "lets go: 5 s after release it has fallen %.1f dB (5 s RT60 => about -60)", fall);
        checkClicks(out, "freeze in, 30 s hold, release (Fade 250 ms)");
        // A hard 20 ms Fade is still click-free.
        Stereo fast = run(20, {}, in);
        checkClicks(fast, "freeze with Fade at 20 ms");
        // While frozen, the input is shut out of the pad: with Live 0, drums from 10 to 20 s leave it untouched.
        Stereo withDrums = in; addDrums(withDrums, 10.0, 20.0);
        Stereo out2 = run(250, {}, withDrums);
        const double leak = diffRatioDb(out, out2, 10.0, 34.0);
        check(leak < -60.0, "the pad ignores new input (drums change the frozen pad by %.1f dB)", leak);
        // Freeze with Shimmer up: the shimmer is muted inside the frozen loop, so it holds too.
        auto ps = make(0, {{SG::kMix, 100}, {SG::kShimmer, 70}, {SG::kLive, 0}});
        Stereo outS = render(*ps, in, {{3.0, SG::kFreeze, 1}, {36.0, SG::kFreeze, 0}});
        lo = 1e9f; hi = -1e9f;
        for (double t = 5.0; t + 1.0 <= 35.0; t += 1.0) { const float v = db(rmsOf(outS, t, t + 1.0)); lo = std::min(lo, v); hi = std::max(hi, v); }
        check(hi - lo < 2.0f && finite(outS), "frozen with Shimmer 70%% holds (1 s windows within %.2f dB)", hi - lo);
        checkClicks(outS, "freeze with Shimmer 70%");
        // Mixed with the dry signal at 35 %, engaging and releasing under a playing chord.
        Stereo in3 = silence(16.0);
        addChord(in3, {45, 52, 57, 60, 64}, 0.0, 14.0, 0.08f);
        auto pm = make(0);
        Stereo out3 = render(*pm, in3, {{4.0, SG::kFreeze, 1}, {9.0, SG::kFreeze, 0}});
        checkClicks(out3, "freeze on and off under a playing chord, Mix 35%");

        // The click detector must notice small steps in this same frozen audio: 0.03 (-30 dBFS) in the
        // held pad at 15 s (a bright chord at about -20 dBFS RMS, whose own top end hides anything much
        // smaller from the detector and the ear), and 0.002 (-54 dBFS) in the release tail at 40 s.
        const double at[2] = {15.0, 40.0};
        const float size[2] = {0.03f, 0.002f};
        for (int k = 0; k < 2; ++k)
        {
            std::vector<float> bad = out.L;
            for (size_t i = (size_t)(at[k] * SR); i < bad.size(); ++i) bad[i] += size[k];
            double t; float ratio;
            const int nc = countClicks(bad, t, ratio);
            check(nc > 0 && std::fabs(t - at[k]) < 0.01, "click detector self-test: a %.3f step at %.0f s is caught (%.1fx at %.2f s)",
                  size[k], at[k], ratio, t);
        }
        // Loaded with Freeze already on: silence in, silence out.
        auto pz = make(0, {{SG::kFreeze, 1}});
        Stereo z = render(*pz, silence(2.0));
        check(finite(z) && peakOf(z) == 0.0f, "loaded with Freeze on: silent and finite");
    }

    // ---- 3b. Freeze + Add: a frozen chord, one note at a time ------------------------------------------
    std::printf("\n== 3b. Freeze + Add ==\n");
    {
        // C4, then E4, then G4, each played alone for 1.6 s. Freeze at 1.2 s, Add at 3.2 s and at 5.2 s
        // (the switch goes on, then off: every change adds).
        Stereo in = silence(30.0);
        addChord(in, {60}, 0.0, 1.6, 0.15f);
        addChord(in, {64}, 2.0, 3.6, 0.15f);
        addChord(in, {67}, 4.0, 5.6, 0.15f);
        const std::vector<Pc> build = {{1.2, SG::kFreeze, 1}, {3.2, SG::kAdd, 1}, {5.2, SG::kAdd, 0}};
        auto pc = make(0, {{SG::kMix, 100}, {SG::kLive, 0}});
        Stereo chord = render(*pc, in, build);
        auto p1 = make(0, {{SG::kMix, 100}, {SG::kLive, 0}});
        Stereo single = render(*p1, in, {{1.2, SG::kFreeze, 1}});
        const auto sc = spectrum(chord, 8.0, 28.0), s1 = spectrum(single, 8.0, 28.0);
        const double hz[3] = {261.6, 329.6, 392.0};
        const int notes[3] = {60, 64, 67};
        const double at[3] = {1.2, 3.2, 5.2};
        double lv[3], lv1[3], alone[3];
        for (int k = 0; k < 3; ++k)
        {
            lv[k] = bandDb(sc, hz[k] * 0.98, hz[k] * 1.02); lv1[k] = bandDb(s1, hz[k] * 0.98, hz[k] * 1.02);
            // The same note frozen on its own at the same moment. (Notes differ among themselves by several
            // dB even in the plain reverb: an 8-line network colours pure tones. What matters is that Add
            // holds each note as strongly as freezing it alone would.)
            Stereo one = silence(30.0);
            addChord(one, {notes[k]}, at[k] - 1.2, at[k] + 0.4, 0.15f);
            auto pa = make(0, {{SG::kMix, 100}, {SG::kLive, 0}});
            alone[k] = bandDb(spectrum(render(*pa, one, {{at[k], SG::kFreeze, 1}}), 8.0, 28.0), hz[k] * 0.98, hz[k] * 1.02);
        }
        float worst = 0.0f;
        for (int k = 0; k < 3; ++k) worst = std::max(worst, (float)std::fabs(lv[k] - alone[k]));
        check(worst < 3.0f, "three notes frozen one at a time are each held as strongly as frozen alone (C %+.1f, E %+.1f, G %+.1f dB)",
              lv[0] - alone[0], lv[1] - alone[1], lv[2] - alone[2]);
        check(lv[1] - lv1[1] > 25.0 && lv[2] - lv1[2] > 25.0,
              "without Add only the first note is held (E and G are %.0f / %.0f dB weaker)", lv[1] - lv1[1], lv[2] - lv1[2]);
        float lo = 1e9f, hi = -1e9f;
        for (double t = 7.0; t + 1.0 <= 29.0; t += 1.0) { const float v = db(rmsOf(chord, t, t + 1.0)); lo = std::min(lo, v); hi = std::max(hi, v); }
        check(hi - lo < 1.5f, "the frozen chord holds for 22 s (1 s windows within %.2f dB)", hi - lo);
        check(finite(chord) && peakOf(chord) < 1.0f, "frozen chord finite, peak %.2f dBFS", db(peakOf(chord)));
        checkClicks(chord, "building a chord with Freeze + Add (Live 0)");
        std::string txt; pc->formatValue(SG::kAdd, 0.0f, txt);
        check(pc->layers_.load() == 3 && txt == "3 layers", "Add's display counts the layers (\"%s\")", txt.c_str());

        // The same with Live 100: the reverb keeps playing beside the pad.
        auto pl = make(0, {{SG::kMix, 100}});
        Stereo chordL = render(*pl, in, build);
        checkClicks(chordL, "building a chord with Freeze + Add (Live 100)");
        check(finite(chordL) && peakOf(chordL) < 1.0f, "Live 100 chord finite, peak %.2f dBFS", db(peakOf(chordL)));

        // Live: over the frozen chord, a new note (A4) at 10 s is heard through the reverb in proportion to Live.
        Stereo over = in; addChord(over, {69}, 10.0, 11.0, 0.15f);
        double a440[3];
        const float lives[3] = {0.0f, 50.0f, 100.0f};
        for (int k = 0; k < 3; ++k)
        {
            auto pv = make(0, {{SG::kMix, 100}, {SG::kLive, lives[k]}});
            a440[k] = bandDb(spectrum(render(*pv, over, build), 11.6, 13.6), 431.0, 449.0);
        }
        check(a440[2] - a440[0] > 20.0 && a440[1] > a440[0] + 6.0 && a440[1] < a440[2] - 3.0,
              "Live sets how loud the reverb of new playing is over the pad (A4 tail: 0%% %.1f, 50%% %+.1f, 100%% %+.1f dB)",
              0.0, a440[1] - a440[0], a440[2] - a440[0]);

        // Add does nothing while Freeze is off.
        auto pa = make(0, {{SG::kMix, 100}}), pb = make(0, {{SG::kMix, 100}});
        const Stereo oa = render(*pa, in, {{2.0, SG::kAdd, 1}, {4.0, SG::kAdd, 0}}), ob = render(*pb, in);
        check(diffRatioDb(oa, ob, 0.0, 30.0) < -200.0, "Add while Freeze is off changes nothing");

        // Taps in quick succession, while a capture is still fading in (Fade 1 s): each waits for the one
        // before to finish, and taps that pile up meanwhile count once. Smooth and finite.
        auto pq = make(0, {{SG::kMix, 100}, {SG::kFade, 1000}});
        Stereo quick = render(*pq, in, {{1.2, SG::kFreeze, 1}, {2.3, SG::kAdd, 1}, {2.35, SG::kAdd, 0}, {2.45, SG::kAdd, 1}, {4.3, SG::kAdd, 0}, {4.32, SG::kAdd, 1}});
        checkClicks(quick, "quick taps while captures are still fading in");
        check(finite(quick) && peakOf(quick) < 1.0f && pq->layers_.load() >= 4 && pq->layers_.load() <= 5,
              "quick taps: finite, peak %.2f dBFS, %d layers (6 taps; those during a capture wait and pile up as one)",
              db(peakOf(quick)), pq->layers_.load());

        // Loudness guard: a loud chord with Add tapped every half second, ten times.
        Stereo loud = silence(20.0);
        addChord(loud, {48, 55, 60, 64, 67, 72}, 0.0, 7.0, 0.09f);
        std::vector<Pc> taps = {{1.0, SG::kFreeze, 1}};
        for (int k = 0; k < 10; ++k) taps.push_back({1.5 + 0.5 * k, SG::kAdd, (float)((k + 1) % 2)});
        auto pg = make(0, {{SG::kMix, 100}, {SG::kLive, 0}});
        Stereo g = render(*pg, loud, taps);
        float gTop = -200.0f;
        for (double t = 9.0; t + 1.0 <= 20.0; t += 1.0) gTop = std::max(gTop, db(rmsOf(g, t, t + 1.0)));
        const float gEnd = db(rmsOf(g, 18.0, 20.0));
        check(finite(g) && peakOf(g) < 1.0f && gTop < -7.0f && gEnd > -16.0f,
              "eleven stacked layers of a loud chord are held down (loudest second %.1f dBFS, 18-20 s %.1f dBFS, peak %.2f dBFS)",
              gTop, gEnd, db(peakOf(g)));

        // After Freeze off the pad dies away and stops running.
        auto ps = make(0, {{SG::kMix, 100}, {SG::kDecay, 2}});
        Stereo sd = render(*ps, in, {{1.2, SG::kFreeze, 1}, {6.0, SG::kFreeze, 0}});
        check(!ps->pad_.active && !ps->stage_.active, "once released and silent, the pad network stops running");
        // A project loaded with Freeze and Add already on holds an empty pad and adds nothing.
        auto pz = make(0, {{SG::kFreeze, 1}, {SG::kAdd, 1}});
        Stereo z = render(*pz, silence(1.0));
        check(pz->layers_.load() == 0 && peakOf(z) == 0.0f, "loaded with Freeze and Add on: nothing added, silent");
    }

    // ---- 4. Shimmer ----------------------------------------------------------------------------------
    std::printf("\n== 4. Shimmer ==\n");
    {
        // A 220 Hz sine for 3 s; measure the tail from 3.5 to 6 s.
        Stereo in = silence(7.0);
        addSine(in, 220.0, 0.0, 3.0);
        auto tail = [&](float shimmer, int interval) {
            auto p = make(0, {{SG::kMix, 100}, {SG::kDecay, 8}, {SG::kShimmer, shimmer}, {SG::kInterval, (float)interval}});
            return spectrum(render(*p, in), 3.5, 6.5);
        };
        const auto off = tail(0, kIvOctave);
        const double base = bandDb(off, 200, 240);
        struct Iv { int iv; const char* name; double hz; };
        for (const Iv& v : {Iv{kIvOctave, "Octave", 440}, Iv{kIvFifth, "Fifth", 330}, Iv{kIvTwoOct, "2 Oct", 880}, Iv{kIvOctDown, "Oct Down", 110}})
        {
            const auto on = tail(70, v.iv);
            const double gainOff = bandDb(off, v.hz * 0.95, v.hz * 1.05) - base;
            const double gainOn = bandDb(on, v.hz * 0.95, v.hz * 1.05) - bandDb(on, 200, 240);
            check(gainOn - gainOff > 12.0, "Shimmer 70%%, %-8s: energy at %.0f Hz rises %.1f dB relative to the 220 Hz tone",
                  v.name, v.hz, gainOn - gainOff);
        }
        // Octave: the next octave (880 Hz) grows too, so the tail keeps climbing.
        const auto on = tail(70, kIvOctave);
        const double up2 = (bandDb(on, 836, 924) - bandDb(on, 200, 240)) - (bandDb(off, 836, 924) - base);
        check(up2 > 12.0, "Shimmer 70%%, Octave: the tail climbs on to 880 Hz too (+%.1f dB)", up2);

        // Shift (1.2). First the pitch shifter on its own, fed a pure sine: a two-grain shifter's output
        // comes in lines spaced at its grain rate, the strongest within about half a grain rate of the
        // exact interval (which line depends on the input frequency). So the check is: the energy centre
        // around the target is within 0.7 grain rates (+1 Hz) of it, which also proves the dial's
        // semitones and cents reach the shifter correctly.
        auto centreAround = [](const std::vector<double>& pw, double lo, double hi) {
            const int N = 8192; double e = 0.0, ef = 0.0;
            for (int k = (int)(lo * N / SR); k <= (int)(hi * N / SR); ++k) { e += pw[(size_t)k]; ef += pw[(size_t)k] * k * SR / N; }
            return ef / (e + 1e-30);
        };
        for (float u : {shiftForSemis(3), shiftForSemis(5), shiftForSemis(7), shiftForSemis(12), shiftForSemis(19),
                        shiftForSemis(-5), shiftForSemis(-7), shiftForSemis(-12), 0.1414f, -0.1414f})
        {
            Seaglass q; q.prepare(SR, 512); q.set(SG::kShift, u); q.reset();
            const float ratio = q.shiftRatio(), W = q.windowFor(ratio);
            Shifter sh; sh.r.allocate((int)W + 16);
            Stereo o = silence(12.0);
            for (size_t i = 0; i < o.L.size(); ++i)
                o.L[i] = o.R[i] = sh.process((float)std::sin(2.0 * M_PI * 220.0 * i / SR), (1.0f - ratio) / W, W);
            const double target = 220.0 * ratio, grain = std::fabs(1.0 - ratio) / W * SR;
            const double got = centreAround(spectrum(o, 2.0, 12.0), target * 0.95, target * 1.05);
            std::string txt; q.formatValue(SG::kShift, u, txt);
            check(std::fabs(got - target) <= 0.7 * grain + 1.0, "shifter at Shift %-7s: %.1f Hz from 220 Hz (exact %.1f Hz, grain rate %.1f Hz)",
                  txt.c_str(), got, target, grain);
        }
        // Then in the reverb: from a 220 Hz tone, energy builds up at the dialled interval.
        for (int st : {7, 3, -5, 19})
        {
            auto ps = make(0, {{SG::kMix, 100}, {SG::kDecay, 8}, {SG::kShimmer, 70}, {SG::kShift, shiftForSemis(st)}});
            const auto pw = spectrum(render(*ps, in), 3.5, 6.5);
            const double target = 220.0 * std::pow(2.0, st / 12.0);
            const double gain = (bandDb(pw, target * 0.97, target * 1.03) - bandDb(pw, 200, 240)) - (bandDb(off, target * 0.97, target * 1.03) - base);
            check(gain > 12.0, "Shimmer 70%%, Shift %+3d st: energy at %.0f Hz rises %.1f dB relative to the 220 Hz tone", st, target, gain);
        }
        // Cents, in the reverb: on a rich chord the tail's brightness drifts up or down with the dial.
        {
            Stereo pad = silence(9.0);
            addChord(pad, {48, 55, 60, 64, 67}, 0.0, 2.5, 0.07f);
            auto centroidOf = [&](float u) {
                auto ps = make(0, {{SG::kMix, 100}, {SG::kDecay, 8}, {SG::kShimmer, 70}, {SG::kShift, u}});
                const auto pw = spectrum(render(*ps, pad), 4.0, 8.0);
                return centreAround(pw, 80.0, 6000.0);
            };
            const double down = centroidOf(-0.1414f), zero = centroidOf(0.0f), up = centroidOf(0.1414f);
            check(down < zero && zero < up, "Shift -50 / 0 / +50 ct: the tail's spectral centre drifts down and up (%.0f / %.0f / %.0f Hz)",
                  down, zero, up);
        }
        auto peakNear = [&](const std::vector<double>& pw, double hz) { return centreAround(pw, hz * 0.95, hz * 1.05); };
        // The dial's text, and the 1.0/1.1 Interval: it still applies while Shift is at its default, and not otherwise.
        {
            Seaglass q; std::string t[6];
            const float us[6] = {0.0f, 0.1f, shiftForSemis(7), shiftForSemis(-12), 1.0f, -1.0f};
            for (int k = 0; k < 6; ++k) q.formatValue(SG::kShift, us[k], t[k]);
            check(t[0] == "0 ct" && t[1] == "+25 ct" && t[2] == "+7 st" && t[3] == "-12 st" && t[4] == "+24 st" && t[5] == "-24 st",
                  "Shift reads %s, %s, %s, %s, %s, %s", t[0].c_str(), t[1].c_str(), t[2].c_str(), t[3].c_str(), t[4].c_str(), t[5].c_str());
            auto pl = make(0, {{SG::kMix, 100}, {SG::kDecay, 8}, {SG::kShimmer, 70}, {SG::kInterval, kIvFifth}, {SG::kShift, shiftForSemis(3)}});
            const double got = peakNear(spectrum(render(*pl, in), 3.5, 6.5), 261.6);
            check(std::fabs(got / 261.6 - 1.0) < 0.03 && std::fabs(got / 329.6 - 1.0) > 0.1,
                  "a Shift away from its default wins over an old Interval (Fifth + Shift +3 st: the shimmer is around %.1f Hz, not 330)", got);
        }

        // Bounded at maximum decay and shimmer, for every interval, fed a loud chord and drums.
        Stereo loud = silence(60.0);
        addChord(loud, {48, 55, 60, 64, 67, 71}, 0.0, 5.0, 0.06f);
        addDrums(loud, 0.0, 5.0, 96.0, 0.7f);
        std::printf("INFO  loud material peaks at %.2f dBFS\n", db(peakOf(loud)));
        struct Sh { const char* name; float u; };
        const Sh shifts[] = {{"Octave", shiftForSemis(12)}, {"+7 st", shiftForSemis(7)}, {"+24 st", 1.0f}, {"-24 st", -1.0f},
                             {"-12 st", shiftForSemis(-12)}, {"-1 st", shiftForSemis(-1)}, {"+10 ct", 0.0632f}, {"0 ct", 0.0f}};
        for (const Sh& sh : shifts)
        {
            auto p = make(0, {{SG::kMix, 100}, {SG::kDecay, 30}, {SG::kShimmer, 100}, {SG::kShift, sh.u}});
            Stereo out = render(*p, loud);
            float top = -200.0f;
            for (double t = 5.0; t + 1.0 <= 60.0; t += 1.0) top = std::max(top, db(rmsOf(out, t, t + 1.0)));
            const float late = db(rmsOf(out, 50.0, 60.0)), mid = db(rmsOf(out, 30.0, 40.0));
            check(finite(out) && peakOf(out) < 1.0f && top < -3.0f && late < mid + 1.0f,
                  "Decay 30 s + Shimmer 100%%, Shift %-7s: bounded (peak %.2f dBFS, loudest second %.1f dBFS, 50-60 s %.1f vs 30-40 s %.1f dBFS)",
                  sh.name, db(peakOf(out)), top, late, mid);
        }
        // Shimmer on a pad tail stays click-free.
        Stereo padIn = silence(14.0);
        addChord(padIn, {52, 59, 64, 66, 71}, 0.0, 4.0, 0.08f);
        auto p = make(0, {{SG::kMix, 100}, {SG::kDecay, 10}, {SG::kShimmer, 60}});
        checkClicks(render(*p, padIn), "shimmer tail on a pad");
        // Turning Shift through cents and semitones while the tail rings (a change every 100 ms).
        auto pt = make(0, {{SG::kMix, 100}, {SG::kDecay, 10}, {SG::kShimmer, 60}});
        std::vector<Pc> turn;
        for (int k = 0; k < 80; ++k) turn.push_back({2.0 + 0.1 * k, SG::kShift, -0.3f + 1.3f * (float)k / 79.0f});
        for (int k = 0; k < 40; ++k) turn.push_back({10.2 + 0.05 * k, SG::kShift, 1.0f - 2.0f * (float)k / 39.0f});
        Stereo tt = render(*pt, padIn, turn);
        checkClicks(tt, "turning Shift from -6 st through cents to +24 st and back to -24 st while the tail rings");
        check(finite(tt) && peakOf(tt) < 1.0f, "...finite, peak %.2f dBFS", db(peakOf(tt)));
    }

    // ---- 5. Swell ------------------------------------------------------------------------------------
    std::printf("\n== 5. Swell ==\n");
    {
        // A chord plays to 6 s (released by 6.4 s), then silence.
        Stereo in = silence(14.0);
        addChord(in, {48, 55, 60, 64, 67}, 0.0, 6.0, 0.08f);
        auto run = [&](float swell) {
            auto p = make(0, {{SG::kMix, 100}, {SG::kDecay, 6}, {SG::kSwell, swell}, {SG::kRise, 1.5f}});
            return render(*p, in);
        };
        const Stereo dry = run(0), sw = run(100), half = run(50);
        const float playOff = db(rmsOf(dry, 3.0, 5.5)), playOn = db(rmsOf(sw, 3.0, 5.5)), playHalf = db(rmsOf(half, 3.0, 5.5));
        check(playOn < playOff - 20.0f, "while playing, Swell 100%% keeps the tail down: %.1f dBFS against %.1f without", playOn, playOff);
        check(playHalf < playOff - 3.0f && playHalf > playOn + 3.0f, "Swell 50%% ducks part way (%.1f dBFS)", playHalf);
        const float before = db(rmsOf(sw, 5.0, 5.5)), after = db(rmsOf(sw, 8.0, 8.5));
        check(after > before + 10.0f, "after the input stops the tail blooms: %.1f dBFS at 5 s -> %.1f dBFS at 8 s", before, after);
        const float vsDry = after - db(rmsOf(dry, 8.0, 8.5));
        check(std::fabs(vsDry) < 2.0f, "by 8 s it has come all the way back (%+.1f dB against no Swell)", vsDry);
        // It rises over about Rise seconds, not at once: halfway through it is clearly below full.
        const float early = db(rmsOf(sw, 6.6, 6.8)) - db(rmsOf(dry, 6.6, 6.8));
        check(early < -3.0f, "the bloom takes time (%.1f dB below full 0.3 s after the release)", early);
        checkClicks(sw, "swell duck and bloom");
        // In the gaps between drum hits: Swell lowers the tail under the hits but lets it up between bars.
        Stereo gaps = silence(12.0);
        for (int b = 0; b < 4; ++b) addDrums(gaps, b * 3.0, b * 3.0 + 1.25);   // one bar, then a gap
        auto pg = make(0, {{SG::kMix, 100}, {SG::kDecay, 4}, {SG::kSwell, 100}, {SG::kRise, 0.6f}});
        auto pn = make(0, {{SG::kMix, 100}, {SG::kDecay, 4}});
        const Stereo g1 = render(*pg, gaps), g0 = render(*pn, gaps);
        const float underHits = db(rmsOf(g1, 3.3, 4.2)) - db(rmsOf(g0, 3.3, 4.2));
        const float inGap = db(rmsOf(g1, 5.3, 5.9)) - db(rmsOf(g0, 5.3, 5.9));
        check(underHits < -10.0f && inGap > -3.0f, "drums with gaps: tail %.1f dB under the hits, %.1f dB in the gaps", underHits, inGap);
    }

    // ---- 6. Abuse: random parameter changes every 50 ms over drums and a chord -----------------------
    std::printf("\n== 6. Abuse ==\n");
    {
        Stereo in = silence(20.0);
        addDrums(in, 0.0, 18.0, 96.0, 0.6f);
        addChord(in, {50, 57, 62, 65, 69}, 0.0, 18.0, 0.05f);
        auto p = make(0);
        std::vector<Pc> pcs;
        unsigned rs = 99;
        for (double t = 0.0; t < 20.0; t += 0.05)
        {
            rs = rs * 1664525u + 1013904223u;
            const int prm = (int)((rs >> 8) % SG::kNumParams);
            rs = rs * 1664525u + 1013904223u;
            const float u = (float)((rs >> 8) & 0xFFFF) / 65535.0f;
            pcs.push_back({t, prm, p->param(prm).toPlain(u)});
        }
        Stereo out = render(*p, in, pcs);
        check(finite(out) && peakOf(out) < 1.0f, "random changes every 50 ms: finite, peak %.2f dBFS", db(peakOf(out)));
        // Sample rates other than 44.1 kHz.
        for (double rate : {48000.0, 96000.0})
        {
            Seaglass q; q.prepare(rate, 512); loadProgram(q, 3); q.reset();
            Stereo o = render(q, mixIn);   // the material is just numbers here
            check(finite(o) && peakOf(o) < 1.0f, "prepared at %.0f Hz: finite, peak %.2f dBFS", rate, db(peakOf(o)));
        }
    }

    // ---- 7. Demo -------------------------------------------------------------------------------------
    std::printf("\n== 7. Demo ==\n");
    {
        // 0-13 s   drums and chords through the default program (Mix 40 %)
        // 14-23 s  a chord built one note at a time: C, E, G, B, each frozen in turn (Freeze, then Add)
        // 23-31 s  a melody over the frozen chord, with the live reverb (Live 60 %) still on it
        // 32 s     Freeze off: the chord lets go
        // 35-47 s  shimmer (Octave 55 %, Decay 10 s) on slow chords
        // 48-64 s  swell (100 %, Rise 2 s) on chord stabs: the tail blooms in the gaps
        // 66-80 s  Shift +14 ct (the Drift Glass program's wash) on a held chord: a slow, rising smear
        Stereo in = silence(80.0);
        addDrums(in, 0.0, 13.0, 92.0, 0.8f);
        addChord(in, {48, 55, 59, 62, 64}, 0.0, 4.2, 0.06f);
        addChord(in, {45, 52, 57, 60, 64}, 4.4, 8.6, 0.06f);
        addChord(in, {41, 48, 55, 57, 64}, 8.8, 12.6, 0.06f);
        const int build[4] = {48, 52, 55, 59};
        for (int k = 0; k < 4; ++k) addChord(in, {build[k]}, 14.0 + 2.2 * k, 15.4 + 2.2 * k, 0.13f, 0.15, 0.4);
        const int melody[8] = {67, 71, 72, 74, 72, 71, 67, 64};
        for (int k = 0; k < 8; ++k) addChord(in, {melody[k]}, 23.5 + 0.9 * k, 24.0 + 0.9 * k, 0.08f, 0.04, 0.3);
        addChord(in, {52, 59, 64, 66, 71}, 35.0, 39.0, 0.07f, 0.4, 0.8);
        addChord(in, {49, 56, 61, 64, 68}, 40.0, 43.0, 0.07f, 0.4, 0.8);
        for (int k = 0; k < 6; ++k)
        {
            const int root = (k % 2) ? 45 : 48;
            addChord(in, {root, root + 7, root + 12, root + 16}, 48.0 + k * 2.6, 48.0 + k * 2.6 + 0.5, 0.08f, 0.03, 0.15);
        }
        addChord(in, {50, 57, 62, 66, 69}, 66.0, 70.0, 0.07f, 0.6, 1.0);
        auto p = make(0, {{SG::kMix, 40}, {SG::kDecay, 6}});
        std::vector<Pc> pcs = {
            {13.5, SG::kMix, 75}, {13.5, SG::kLive, 60}, {13.5, SG::kFade, 400},
            {15.2, SG::kFreeze, 1}, {17.4, SG::kAdd, 1}, {19.6, SG::kAdd, 0}, {21.8, SG::kAdd, 1},
            {32.0, SG::kFreeze, 0},
            {34.0, SG::kShimmer, 55}, {34.0, SG::kDecay, 10}, {34.0, SG::kMix, 50},
            {47.0, SG::kShimmer, 15}, {47.0, SG::kSwell, 100}, {47.0, SG::kRise, 2.0f}, {47.0, SG::kDecay, 8},
            {64.0, SG::kSwell, 0}, {64.0, SG::kShimmer, 70}, {64.0, SG::kShift, 0.075f}, {64.0, SG::kDecay, 16}, {64.0, SG::kMix, 55},
        };
        double cpu = 0.0;
        Stereo out = render(*p, in, pcs, &cpu);
        const char* path = argc > 1 ? argv[1] : "build/seaglass-demo.wav";
        writeWav(path, out.L, out.R);
        std::printf("INFO  demo: %s (80 s, peak %.2f dBFS)\n", path, db(peakOf(out)));
        std::printf("INFO  CPU: 80 s rendered in %.3f s on this computer (%.2f%% of real time, desktop; the MPC is several times slower)\n",
                    cpu, 100.0 * cpu / 80.0);
        check(finite(out) && peakOf(out) < 1.0f, "demo under 0 dBFS, no NaN");
        checkClicks(out, "demo after the drums stop (chord built with Add, melody over it, release, shimmer, swell, cents wash)", 13.5);

        // Worst case for CPU: all three networks running (a capture fading in over a held pad) with Shimmer on.
        auto pw = make(0, {{SG::kShimmer, 50}, {SG::kFade, 4000}});
        Stereo nz = silence(12.0); addNoise(nz, 0.0, 12.0);
        double cpuW = 0.0;
        render(*pw, nz, {{1.0, SG::kFreeze, 1}, {5.0, SG::kAdd, 1}, {9.0, SG::kAdd, 0}}, &cpuW);
        std::printf("INFO  CPU worst case (live + pad + capture + shimmer): %.2f%% of real time on this computer\n", 100.0 * cpuW / 12.0);
    }

    std::printf("\n%s  (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
