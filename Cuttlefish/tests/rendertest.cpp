// Does Cuttlefish morph smoothly, stay alias-free, lock to the song, steal voices without clicks, glide,
// stay finite and fit the CPU? Renders the plugin directly (as the MPC would call process()) and checks:
//   tables      built once, finite, equal loudness, mip levels band-limited, build time
//   morph       a slow sweep moves the spectrum smoothly and adds no zipper noise (self-test: the same
//               sweep with per-sample ramping switched off must show it); a knob jump does not click
//   aliasing    inharmonic energy of high notes on every table, with warps (self-test: a naive saw)
//   tempo sync  synced LFOs follow the transport position, jump with it, run at tempo when stopped
//   voices      the voice limit holds, stealing and same-key repeats don't click (self-test: an injected click)
//   glide       legato and mono glide timing, no glide on detached legato notes, poly glide
//   motion      Colour, Drift, mod matrix, morph envelope do what they say
//   safety      no NaN / Inf / denormals on every program, under random knob moves and with long tails
//   levels      every program's peak and loudness for a single note and a 4-note chord
//   CPU         full polyphony at unison 1 and 4, both oscillators, every effect
// Then renders build/cuttlefish-demo.wav (the 1.0 programs, then a slow Colour sweep) and
// build/cuttlefish-demo-1.1.wav (the programs added in 1.1).
//
//   mkdir -p tests/build
//   clang++ -std=c++17 -O2 -I../../framework tests/rendertest.cpp -o tests/build/rendertest
//   tests/build/rendertest build/cuttlefish-demo.wav build/cuttlefish-demo-1.1.wav
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cfloat>
#include <memory>
#include <string>
#include <chrono>
#include <algorithm>
#include <functional>
#include <map>
#include <atomic>

static bool g_noRamp = false;
#define CUTTLEFISH_NORAMP g_noRamp

#define private public
#define protected public
#include "../src/Cuttlefish.cpp"
#undef private
#undef protected

using namespace cuttlefish;
using P = Cuttlefish;

static double SR = 44100.0;
static int failures = 0, passes = 0;
static void check(bool ok, const char* fmt, ...)
{
    char buf[600];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", buf);
    if (ok) ++passes; else ++failures;
}
static void info(const char* fmt, ...)
{
    char buf[600];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::printf("INFO  %s\n", buf);
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

// ---- a running session ----------------------------------------------------------------------------
struct Ev { double t; mpc::MidiEvent e; };
static Ev on(double t, int note, int vel = 100) { return {t, mpc::MidiEvent::noteOn(0, note, vel)}; }
static Ev off(double t, int note) { return {t, mpc::MidiEvent::noteOff(0, note)}; }
struct Auto { double t; int param; float value; };

struct Session
{
    std::unique_ptr<Cuttlefish> p;
    std::vector<float> L, R;
    long long pos = 0;
    bool playing = false;
    double bpm = 120.0, ppqAtStart = 0.0;
    long long playStart = 0;
    int fixedBlock = 0;
    bool ftz = true;
    unsigned rs = 12345;
    int maxLive = 0, maxSlots = 0;
    double cpu = 0.0;
    bool bad = false;
    std::function<void(Session&)> perBlock;

    explicit Session(int prog = 0, std::initializer_list<std::pair<int, float>> overrides = {}, bool clean = false)
    {
        p = std::make_unique<Cuttlefish>();
        p->prepare(SR, 512);
        const mpc::Program& pr = p->programs_[(size_t)prog];
        for (int i = 0; i < p->numParams(); ++i) p->set(i, pr.plainValues[(size_t)i]);
        if (clean)   // a dry, steady single oscillator for measurements
        {
            p->set(P::pDrift, 0); p->set(P::pUnison, 1); p->set(P::pChorus, 0); p->set(P::pDelay, 0); p->set(P::pReverb, 0);
            p->set(P::pLevel2, 0); p->set(P::pSub, 0); p->set(P::pNoise, 0); p->set(P::pCutoff, 20000); p->set(P::pEnvAmt, 0);
            p->set(P::pAmt1, 0); p->set(P::pAmt2, 0); p->set(P::pAmt3, 0); p->set(P::pAmt4, 0); p->set(P::pEnvMorph, 0);
            p->set(P::pColour, 0); p->set(P::pSustain, 100); p->set(P::pAttack, 1); p->set(P::pReso, 0);
            p->set(P::pFilterMode, kLP12); p->set(P::pKeyTrack, 0);
        }
        for (const auto& o : overrides) p->set(o.first, o.second);
        p->reset();
    }
    void set(int param, float v) { p->set(param, v); }
    void play(bool o, double startPpq = 0.0) { playing = o; ppqAtStart = startPpq; playStart = pos; }
    double ppqNow() const { return ppqAtStart + (double)(pos - playStart) / SR * bpm / 60.0; }

    void run(double untilSec, std::vector<Ev> evs = {}, std::vector<Auto> autos = {})
    {
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
        std::stable_sort(autos.begin(), autos.end(), [](const Auto& a, const Auto& b) { return a.t < b.t; });
        const long long end = (long long)(untilSec * SR);
        size_t ei = 0, ai = 0;
        std::vector<mpc::MidiEvent> be;
        while (pos < end)
        {
            rs = rs * 1664525u + 1013904223u;
            const int block = (int)std::min<long long>(fixedBlock > 0 ? fixedBlock : 32 + (int)((rs >> 8) % 481), end - pos);
            while (ai < autos.size() && (long long)std::llround(autos[ai].t * SR) < pos + block) { p->set(autos[ai].param, autos[ai].value); ++ai; }
            if (perBlock) perBlock(*this);
            be.clear();
            while (ei < evs.size() && (long long)std::llround(evs[ei].t * SR) < pos + block)
            {
                mpc::MidiEvent e = evs[ei].e;
                e.sampleOffset = (int)std::max<long long>(0, std::llround(evs[ei].t * SR) - pos);
                be.push_back(e); ++ei;
            }
            L.resize((size_t)(pos + block)); R.resize((size_t)(pos + block));
            float* outs[2] = {L.data() + pos, R.data() + pos};
            mpc::ProcessContext ctx;
            ctx.outputs = outs; ctx.numInputs = 0; ctx.numOutputs = 2; ctx.numFrames = block;
            ctx.midi = be.data(); ctx.numMidiEvents = (int)be.size();
            ctx.sampleRate = SR;
            ctx.transport.valid = ctx.transport.tempoValid = true; ctx.transport.bpm = bpm;
            ctx.transport.playing = playing; ctx.transport.ppqValid = playing;
            ctx.transport.ppqPos = ppqNow();
            const auto t0 = std::chrono::steady_clock::now();
            if (ftz) { mpc::ScopedDenormalsOff d; p->process(ctx); } else p->process(ctx);
            cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            int live = 0, slots = 0;
            for (auto& v : p->voices_) if (v.active) { ++slots; if (!v.fading) ++live; }
            maxLive = std::max(maxLive, live); maxSlots = std::max(maxSlots, slots);
            for (int i = 0; i < block; ++i) if (!std::isfinite(outs[0][i]) || !std::isfinite(outs[1][i])) bad = true;
            pos += block;
        }
    }
};

// ---- measurements ----------------------------------------------------------------------------------
static float peakOf(const std::vector<float>& a, size_t from = 0, size_t to = (size_t)-1)
{
    float m = 0.0f; to = std::min(to, a.size());
    for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(a[i])); return m;
}
static float rmsOf(const std::vector<float>& a, size_t from, size_t to)
{
    double s = 0.0; to = std::min(to, a.size()); if (to <= from) return 0.0f;
    for (size_t i = from; i < to; ++i) s += (double)a[i] * a[i]; return (float)std::sqrt(s / (double)(to - from));
}
static float db(double x) { return (float)(20.0 * std::log10(std::max(x, 1.0e-12))); }
static size_t S(double t) { return (size_t)(t * SR); }

static void fftD(std::vector<double>& re, std::vector<double>& im)
{
    const int n = (int)re.size();
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
        if (i < j) { std::swap(re[(size_t)i], re[(size_t)j]); std::swap(im[(size_t)i], im[(size_t)j]); }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = -6.283185307179586 / len, wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len)
        {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; ++k)
            {
                const size_t a = (size_t)(i + k), b = (size_t)(i + k + len / 2);
                const double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                const double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}
// Power spectrum of N samples from `from`, 4-term Blackman-Harris window (sidelobes -92 dB).
static std::vector<double> power(const std::vector<float>& x, size_t from, int N)
{
    std::vector<double> re((size_t)N), im((size_t)N, 0.0);
    for (int i = 0; i < N; ++i)
    {
        const double t = 6.283185307179586 * i / (N - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        re[(size_t)i] = w * x[from + (size_t)i];
    }
    fftD(re, im);
    std::vector<double> p((size_t)N / 2);
    for (int k = 0; k < N / 2; ++k) p[(size_t)k] = re[(size_t)k] * re[(size_t)k] + im[(size_t)k] * im[(size_t)k];
    return p;
}
// Energy that is not at a harmonic of f0, relative to the harmonic energy, 20 Hz - 18.5 kHz (dB). The tables are
// built so that anything folding back lands above 19 kHz; this measures the band where aliasing is audible.
static double inharmonicDb(const std::vector<float>& x, size_t from, double f0, int N = 16384)
{
    const std::vector<double> p = power(x, from, N);
    const double bin = SR / N;
    double harm = 0.0, inh = 0.0;
    for (int k = (int)(20.0 / bin); k < N / 2 && k * bin <= 18500.0; ++k)
    {
        const double f = k * bin;
        const double h = std::round(f / f0);
        if (h >= 1.0 && std::fabs(f - h * f0) <= 7.0 * bin) harm += p[(size_t)k]; else inh += p[(size_t)k];
    }
    return 10.0 * std::log10(std::max(inh, 1e-30) / std::max(harm, 1e-30));
}
// Energy half-way between harmonics (further than 0.3 f0 from any), relative to the harmonics (dB). A morph
// stepped at the 16-sample control rate puts sidebands there (2756 Hz from each harmonic); the morph's own
// slow movement only widens the harmonics a little, so it does not.
static double betweenDb(const std::vector<float>& x, size_t from, double f0, int N = 16384)
{
    const std::vector<double> p = power(x, from, N);
    const double bin = SR / N;
    double harm = 0.0, mid = 0.0;
    for (int k = (int)(20.0 / bin); k < N / 2 && k * bin <= 20000.0; ++k)
    {
        const double f = k * bin, h = std::round(f / f0), dist = std::fabs(f - h * f0);
        if (h >= 1.0 && dist <= 7.0 * bin) harm += p[(size_t)k]; else if (dist > 0.3 * f0) mid += p[(size_t)k];
    }
    return 10.0 * std::log10(std::max(mid, 1e-30) / std::max(harm, 1e-30));
}
// Spectral centroid weighted by magnitude (not power), so upper harmonics count.
static double centroid(const std::vector<float>& x, size_t from, int N = 2048)
{
    const std::vector<double> p = power(x, from, N);
    double a = 0.0, b = 0.0;
    for (int k = 1; k < N / 2; ++k) { const double m = std::sqrt(p[(size_t)k]); a += k * SR / N * m; b += m; }
    return b > 0.0 ? a / b : 0.0;
}
// Frequency from rising zero crossings (interpolated) over [from, from+len).
static double zcFreq(const std::vector<float>& x, size_t from, size_t len)
{
    double first = -1, last = -1; int count = 0;
    for (size_t i = from + 1; i < from + len && i < x.size(); ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f)
        {
            const double t = (double)(i - 1) + x[i - 1] / (double)(x[i - 1] - x[i]);
            if (first < 0) first = t; else ++count;
            last = t;
        }
    return count > 0 ? SR * count / (last - first) : 0.0;
}
static double stOf(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
static double hzOf(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

// Clicks: spikes in the second difference against the median of the neighbouring 2 ms windows (+-50 ms).
static int countClicks(const std::vector<float>& x, size_t from, size_t to, double& worstT, float& worstRatio, float thr = 4.0f)
{
    const int W = (int)(0.002 * SR);
    to = std::min(to, x.size());
    const int nw = (int)((to - from) / (size_t)W);
    std::vector<float> mx((size_t)std::max(nw, 0), 0.0f);
    for (int w = 0; w < nw; ++w)
        for (size_t i = std::max<size_t>(2, from + (size_t)(w * W)); i < from + (size_t)((w + 1) * W); ++i)
            mx[(size_t)w] = std::max(mx[(size_t)w], std::fabs(x[i] - 2.0f * x[i - 1] + x[i - 2]));
    int clicks = 0; worstRatio = 0.0f; worstT = 0.0;
    std::vector<float> nb;
    for (int w = 0; w < nw; ++w)
    {
        nb.clear();
        for (int j = std::max(0, w - 25); j <= std::min(nw - 1, w + 25); ++j) if (std::abs(j - w) > 1) nb.push_back(mx[(size_t)j]);
        if (nb.empty()) continue;
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end());
        const float med = nb[nb.size() / 2];
        const float ratio = mx[(size_t)w] / std::max(med, 1e-9f);
        if (mx[(size_t)w] > 2e-4f && ratio > worstRatio) { worstRatio = ratio; worstT = (double)(from + (size_t)(w * W)) / SR; }
        if (mx[(size_t)w] > 2e-4f && ratio > thr) ++clicks;
    }
    return clicks;
}
static int subnormals(const float* x, size_t n) { int c = 0; for (size_t i = 0; i < n; ++i) if (std::fpclassify(x[i]) == FP_SUBNORMAL) ++c; return c; }

// =====================================================================================================
int realMain(int argc, char** argv);
#ifndef DBG
int main(int argc,char**argv){return realMain(argc,argv);}
#endif
int realMain(int argc, char** argv)
{
    // ================================================================================================
    std::printf("\n== Wavetables ==\n");
    {
        const auto t0 = std::chrono::steady_clock::now();
        const WavetableBank& bk = WavetableBank::get();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        info("%d tables x %d frames x %d mip levels, %.1f MB, built in %.0f ms on this machine (once per process)",
             kNumTables, kNumFrames, kNumLevels, bk.data_.size() * 4.0 / 1048576.0, ms);
        check(ms < 1500.0, "table build time %.0f ms (expect a few times longer on the MPC)", ms);
        bool finite = true; double minR = 9, maxR = 0, maxPeak = 0, maxDc = 0;
        for (int t = 0; t < kNumTables; ++t)
            for (int f = 0; f < kNumFrames; ++f)
                for (int L = 0; L < kNumLevels; ++L)
                {
                    const float* d = bk.frame(t, f, L); const int n = WavetableBank::levelSize(L);
                    double s = 0, dc = 0, pk = 0;
                    for (int i = 0; i <= n; ++i) { if (!std::isfinite(d[i])) finite = false; }
                    for (int i = 0; i < n; ++i) { s += (double)d[i] * d[i]; dc += d[i]; pk = std::max(pk, (double)std::fabs(d[i])); }
                    if (d[n] != d[0]) finite = false;
                    if (L == 0) { const double r = std::sqrt(s / n); minR = std::min(minR, r); maxR = std::max(maxR, r); }
                    maxPeak = std::max(maxPeak, pk); maxDc = std::max(maxDc, std::fabs(dc / n));
                }
        check(finite, "every frame of every level is finite and its guard sample repeats sample 0");
        check(maxR <= 0.351 && minR > 0.12, "frames are loudness-matched: RMS %.3f..%.3f (0.35 unless the 0.95 peak cap bites)", minR, maxR);
        check(maxPeak < 1.25, "peaks stay below %.2f on every level (band-limiting may overshoot a little)", maxPeak);
        check(maxDc < 1e-4, "no DC in any frame (max %.1e)", maxDc);
        // band-limit of each level: no energy above the level's top harmonic
        double worst = -300;
        for (int t = 0; t < kNumTables; t += 3)
            for (int L = 0; L < kNumLevels; ++L)
            {
                const int n = WavetableBank::levelSize(L), H = WavetableBank::levelHarm(L);
                const float* d = bk.frame(t, kNumFrames - 1, L);
                std::vector<double> re((size_t)n), im((size_t)n, 0.0);
                for (int i = 0; i < n; ++i) re[(size_t)i] = d[i];
                fftD(re, im);
                double in = 0, out = 0;
                for (int k = 1; k < n / 2; ++k) { const double pw = re[(size_t)k] * re[(size_t)k] + im[(size_t)k] * im[(size_t)k]; if (k <= H) in += pw; else out += pw; }
                worst = std::max(worst, 10 * std::log10(std::max(out, 1e-30) / std::max(in, 1e-30)));
            }
        check(worst < -100.0, "each mip level holds only its harmonics (worst out-of-band %.0f dB)", worst);
    }

    // ================================================================================================
    std::printf("\n== Morph: smooth, no zipper ==\n");
    {
        const int note = 57; const double f0 = hzOf(note);   // 220 Hz; the 16-sample tick (2756 Hz) falls between harmonics
        // mode 0: the Morph knob swept 0 -> 100 % over 4 s.  mode 1: an 7 Hz sine LFO swinging Morph 1 by +-50 %.
        auto run = [&](bool noRamp, int table, int mode, std::vector<float>* outL = nullptr) {
            g_noRamp = noRamp;
            Session s(0, {{P::pTable1, (float)table}, {P::pMorph1, mode == 0 ? 0.0f : 50.0f}}, true);
            std::vector<Auto> a;
            if (mode == 0) for (int i = 0; i <= 400; ++i) a.push_back({0.3 + 4.0 * i / 400.0, P::pMorph1, (float)(100.0 * i / 400.0)});
            else { s.set(P::pSrc1, kSrcLfo1); s.set(P::pDst1, kDstMorph1); s.set(P::pAmt1, 50); s.set(P::pL1Rate, 7.0f); s.set(P::pL1Mode, kLfoFree); }
            s.run(4.8, {on(0.05, note)}, a);
            g_noRamp = false;
            double worst = -300;
            for (double t = 0.5; t + 0.4 < 4.4; t += 0.2) worst = std::max(worst, betweenDb(s.L, S(t), f0));
            if (outL) *outL = s.L;
            return worst;
        };
        auto staticWorst = [&](int table) {
            double worst = -300;
            for (int m = 0; m <= 100; m += 10)
            {
                Session s(0, {{P::pTable1, (float)table}, {P::pMorph1, (float)m}}, true);
                s.run(0.7, {on(0.05, note)});
                worst = std::max(worst, betweenDb(s.L, S(0.3), f0));
            }
            return worst;
        };
        const int tabs[4] = {kTabBasic, kTabVowel, kTabDigital, kTabMetal};
        for (int t : tabs)
        {
            std::vector<float> L;
            const double st = staticWorst(t), sw = run(false, t, 0, &L);
            check(sw < st + 12.0 && sw < -50.0, "%-8s Morph knob swept 0-100 %% in 4 s: between-harmonic (zipper) energy %.0f dB (still frames %.0f dB)", kTableNames[t], sw, st);
            // Zipper from a fast LFO: render with and without per-sample ramping (everything else identical);
            // the difference is exactly the stepping error that ramping removes. It must be real (the hook
            // works) and sit at the control rate, ~2.76 kHz away from each harmonic.
            std::vector<float> a, b;
            run(false, t, 1, &a); run(true, t, 1, &b);
            std::vector<float> d(a.size()); for (size_t i = 0; i < a.size(); ++i) d[i] = b[i] - a[i];
            const double rel = db(rmsOf(d, S(0.5), S(4.4)) / std::max(rmsOf(a, S(0.5), S(4.4)), 1e-9f));
            const std::vector<double> pd = power(d, S(1.0), 16384);
            double nearTick = 0, all = 0;
            for (int k = 1; k < 8192; ++k) { const double f = k * SR / 16384; all += pd[(size_t)k];
                for (int m = 1; m <= 3; ++m) { const double ft = m * SR / 16.0; if (std::fabs(std::fmod(std::fabs(f - ft) + 0.5 * f0, f0) - 0.5 * f0) < 30.0) { nearTick += pd[(size_t)k]; break; } } }
            check(rel > -70.0 && rel < -15.0 && nearTick > 0.5 * all, "%-8s 7 Hz LFO on Morph: stepping the morph every 16 samples would add a zipper at %.0f dB (%.0f %% of it at the control-rate sidebands); per-sample ramping removes it",
                  kTableNames[t], rel, 100.0 * nearTick / std::max(all, 1e-30));
            if (t == kTabBasic)
            {
                double c0 = centroid(L, S(0.35)), c1 = centroid(L, S(4.35)), maxJump = 0, prev = c0;
                for (double tt = 0.35; tt < 4.35; tt += 0.025) { const double c = centroid(L, S(tt)); maxJump = std::max(maxJump, std::fabs(c - prev)); prev = c; }
                check(c1 > c0 * 4.0, "Basic: morph moves the brightness from %.0f Hz (sine) to %.0f Hz (narrow pulse)", c0, c1);
                check(maxJump < 0.06 * (c1 - c0), "Basic: the brightness changes smoothly (largest step between 25 ms windows %.0f Hz of %.0f)", maxJump, c1 - c0);
            }
        }
        // a sudden knob jump is smoothed: the waveform's curvature never exceeds what the two still sounds have
        Session j(0, {{P::pTable1, (float)kTabBasic}, {P::pMorph1, 0}, {P::pCutoff, 3000}}, true);
        j.run(1.2, {on(0.05, 45)}, {{0.6, P::pMorph1, 100.0f}});
        auto d2 = [&](double a, double b) { float m = 0; for (size_t i = S(a); i < S(b); ++i) m = std::max(m, std::fabs(j.L[i] - 2 * j.L[i - 1] + j.L[i - 2])); return m; };
        const float before = d2(0.3, 0.58), during = d2(0.58, 0.7), after = d2(0.75, 1.1);
        check(during <= 1.15f * std::max(before, after), "Morph knob jumped 0 -> 100 %%: no click (curvature %.4f during, %.4f/%.4f before/after)", during, before, after);
    }

    std::printf("\n== Aliasing at high notes ==\n");
    {
        // self-test: a naive (not band-limited) saw at the same pitch
        std::vector<float> naive(S(1.0));
        const double f0 = hzOf(100);
        for (size_t i = 0; i < naive.size(); ++i) { double ph = std::fmod(f0 * i / SR, 1.0); naive[i] = (float)(0.3 * (1.0 - 2.0 * ph)); }
        const double nv = inharmonicDb(naive, S(0.2), f0);
        const int notes[4] = {84, 96, 100, 108};
        double worst = -300; int wt = 0, wn = 0, wm = 0;
        for (int t = 0; t < kNumTables; ++t)
            for (int note : notes)
                for (int m = 0; m <= 100; m += 50)
                {
                    Session s(0, {{P::pTable1, (float)t}, {P::pMorph1, (float)m}}, true);
                    s.run(0.7, {on(0.05, note)});
                    const double a = inharmonicDb(s.L, S(0.3), hzOf(note));
                    if (a > worst) { worst = a; wt = t; wn = note; wm = m; }
                }
        check(worst < -60.0, "every table, notes C6-C8, morph 0/50/100: aliasing at most %.0f dB (%s, note %d, morph %d)", worst, kTableNames[wt], wn, wm);
        Session sb(0, {{P::pTable1, (float)kTabBasic}, {P::pMorph1, 50}}, true);
        sb.run(0.7, {on(0.05, 100)});
        const double saw = inharmonicDb(sb.L, S(0.3), f0);
        check(nv > saw + 30.0, "self-test: a naive saw at E7 aliases at %.0f dB, Cuttlefish's saw at %.0f dB", nv, saw);
        const char* wn2[4] = {"Bend", "Squash", "Sync", "FM"};
        for (int w = 0; w < 4; ++w)
        {
            double ww = -300;
            for (int note : {72, 84})
                for (int amt : {30, 70, 100})
                {
                    Session s(0, {{P::pTable1, (float)kTabBasic}, {P::pMorph1, w == kWFM ? 0.0f : 50.0f}, {P::pWarpMode1, (float)w},
                                  {P::pWarp1, (float)amt}, {P::pFine2, 0}, {P::pMorph2, 0}}, true);
                    s.run(0.7, {on(0.05, note)});
                    ww = std::max(ww, inharmonicDb(s.L, S(0.3), hzOf(note)));
                }
            const double lims[4] = {-52.0, -45.0, -55.0, -60.0};
            check(ww < lims[w], "warp %-6s on %s, C5/C6, 30-100 %%: aliasing at most %.0f dB (limit %.0f)", wn2[w],
                  w == kWFM ? "a sine, modulated by osc 2's sine" : "a saw", ww, lims[w]);
        }
    }

    // ================================================================================================
    std::printf("\n== Tempo-synced LFOs ==\n");
    {
        // LFO 1: Sync, 1/4, square -> Level 100 %: the sound is gated on every beat
        Session s(0, {{P::pL1Mode, kLfoSync}, {P::pL1Div, 7}, {P::pL1Shape, kShSquare}, {P::pSrc1, kSrcLfo1}, {P::pDst1, kDstLevel},
                      {P::pAmt1, 100}, {P::pTable1, kTabBasic}, {P::pMorph1, 0}}, true);
        s.bpm = 123.0;
        double maxErr = 0.0; long long skipUntil = 0;
        s.perBlock = [&](Session& se) {
            if (!se.playing || se.pos <= skipUntil) return;     // gph_ is from the previous block's ticks
            const double expect = std::fmod(se.ppqNow() / 1.0, 1.0);
            double err = std::fabs(se.p->gph_[0] - expect); err = std::min(err, 1.0 - err);
            maxErr = std::max(maxErr, err);
        };
        s.play(true, 3.37); skipUntil = s.pos;
        s.run(3.0, {on(0.01, 57)});
        const double tickBeats = 16.0 / SR * s.bpm / 60.0;
        check(maxErr <= tickBeats + 512.0 / SR * s.bpm / 60.0 + 1e-6, "Sync 1/4 at 123 BPM from ppq 3.37: LFO phase follows the song position (max error %.4f beat)", maxErr);
        // audible check: the gate opens on the beat
        int good = 0, total = 0;
        for (double ppq = 4.0; ppq < 3.37 + 3.0 * 123.0 / 60.0 - 1.0; ppq += 1.0)
        {
            const double tb = (ppq - 3.37) * 60.0 / 123.0;
            const float before = rmsOf(s.L, S(tb + 0.6 * 60 / 123.0), S(tb + 0.9 * 60 / 123.0));    // second half of the beat: closed
            const float after = rmsOf(s.L, S(tb + 0.1 * 60 / 123.0), S(tb + 0.4 * 60 / 123.0));     // first half: open
            ++total; if (after > 10.0f * std::max(before, 1e-6f)) ++good;
        }
        check(total > 3 && good == total, "the synced square gate opens at every beat (%d of %d)", good, total);
        // a jump in song position moves the LFO with it
        maxErr = 0.0;
        s.play(true, 17.81); skipUntil = s.pos;
        s.run(4.0);
        check(maxErr <= tickBeats + 512.0 / SR * s.bpm / 60.0 + 1e-6, "after relocating to ppq 17.81 the phase follows at once (max error %.4f beat)", maxErr);
        // stopped: runs at the tempo
        Session f(0, {{P::pL1Mode, kLfoSync}, {P::pL1Div, 10}, {P::pL1Shape, kShSawUp}}, true);
        f.bpm = 90.0; f.fixedBlock = 128;
        std::vector<double> wraps; float last = 0.0f;
        f.perBlock = [&](Session& se) { const float ph = se.p->gph_[0]; if (ph < last) wraps.push_back((double)se.pos / SR); last = ph; };
        f.run(4.0, {on(0.01, 57)});
        double period = wraps.size() > 2 ? (wraps.back() - wraps.front()) / (double)(wraps.size() - 1) : 0.0;
        check(std::fabs(period - 0.5 * 60.0 / 90.0) < 0.004, "transport stopped: Sync 1/8 at 90 BPM cycles every %.4f s (expect %.4f)", period, 0.5 * 60.0 / 90.0);
        // Sync Rtg: per note, restarts with each note at tempo
        Session r(0, {{P::pL1Mode, kLfoSyncRtg}, {P::pL1Div, 7}, {P::pL1Shape, kShSawUp}}, true);
        r.bpm = 120.0; r.fixedBlock = 64;
        r.run(0.3, {on(0.01, 57)});
        const float ph1 = r.p->voices_[0].lph[0];
        check(std::fabs(ph1 - (0.29 * 2.0 - std::floor(0.29 * 2.0))) < 0.02, "Sync Rtg 1/4 at 120 BPM: 0.29 s after the note the phase is %.3f (expect %.3f)", ph1, 0.58);
    }

    // ================================================================================================
    std::printf("\n== Voice limit and stealing ==\n");
    {
        Session s(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pVoices, 8}, {P::pRelease, 3000}, {P::pCutoff, 2500}, {P::pAttack, 5}}, true);
        std::vector<Ev> ev;
        for (int i = 0; i < 24; ++i) { const int nn = 48 + (i * 7) % 25; ev.push_back(on(0.05 + 0.06 * i, nn, 90)); ev.push_back(off(0.1 + 0.06 * i, nn)); }
        s.run(2.6, ev);
        check(s.maxLive <= 8 && s.maxSlots <= kNumSlots, "Voices 8, 24 notes in 1.4 s: at most %d sounding (limit 8), %d slots in use (of %d)", s.maxLive, s.maxSlots, kNumSlots);
        double t; float r;
        const int c = countClicks(s.L, S(0.1), S(2.5), t, r);
        check(c == 0, "stealing fades instead of clicking (worst %.1fx its surroundings at %.2f s)", r, t);
        // self-test: inject a 0.02 step into a copy and it must be found
        std::vector<float> cp = s.L; for (size_t i = S(1.234); i < cp.size(); ++i) cp[i] += 0.02f;
        const int c2 = countClicks(cp, S(0.05), S(2.5), t, r);
        check(c2 >= 1 && std::fabs(t - 1.234) < 0.01, "self-test: an injected 0.02 step is detected (at %.3f s, %.0fx)", t, r);
        // the full 16 voices
        Session f(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pVoices, 16}, {P::pRelease, 3000}, {P::pCutoff, 2500}, {P::pAttack, 5}}, true);
        std::vector<Ev> e2;
        for (int i = 0; i < 40; ++i) { const int nn = 40 + (i * 11) % 37; e2.push_back(on(0.02 + 0.03 * i, nn, 80)); }
        f.run(2.0, e2);
        const int c3 = countClicks(f.L, S(0.05), S(1.9), t, r);
        check(f.maxLive <= 16 && c3 == 0, "Voices 16, 40 held notes: at most %d sounding, no clicks (worst %.1fx)", f.maxLive, r);
        // the same key hammered: each repeat crossfades
        Session h(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pRelease, 800}, {P::pCutoff, 2500}, {P::pAttack, 5}}, true);
        std::vector<Ev> e3;
        for (int i = 0; i < 16; ++i) { e3.push_back(on(0.05 + 0.07 * i, 55, 70 + i)); e3.push_back(off(0.1 + 0.07 * i, 55)); }
        h.run(1.6, e3);
        const int c4 = countClicks(h.L, S(0.08), S(1.5), t, r, 6.0f);
        check(c4 == 0, "the same key 16 times quickly: each repeat crossfades, no clicks (worst %.1fx)", r);
        // mono retriggers
        Session m(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pPlayMode, kMono}, {P::pCutoff, 2500}, {P::pAttack, 5}}, true);
        std::vector<Ev> e4;
        const int mn[4] = {57, 59, 60, 62};
        for (int i = 0; i < 12; ++i) { const int nn = mn[i % 4]; e4.push_back(on(0.05 + 0.1 * i, nn)); e4.push_back(off(0.12 + 0.1 * i, nn)); }
        m.run(1.5, e4);
        const int c5 = countClicks(m.L, S(0.08), S(1.4), t, r, 6.0f);
        check(c5 == 0 && m.maxLive <= 1, "Mono: 12 notes, one voice (%d), no clicks (worst %.1fx)", m.maxLive, r);
    }

    // ================================================================================================
    std::printf("\n== Glide ==\n");
    {
        auto pitchAt = [](const Session& s, double t) { return stOf(zcFreq(s.L, S(t - 0.008), S(0.016))); };
        Session s(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pPlayMode, kLegato}, {P::pGlide, 200}}, true);
        s.run(1.6, {on(0.1, 48), on(0.6, 60), off(1.2, 48), off(1.3, 60)});
        const double a = pitchAt(s, 0.55), b = pitchAt(s, 0.7), c = pitchAt(s, 0.9);
        check(std::fabs(a - 48) < 0.1 && std::fabs(b - 54) < 0.7 && std::fabs(c - 60) < 0.1,
              "Legato 200 ms: overlapping C3 -> C4 slides 48.0 -> %.2f (half way, expect ~54) -> %.2f (from %.2f)", b, c, a);
        // glide time: first moment within 0.1 st of the target
        double reach = 0;
        for (double t = 0.62; t < 1.0; t += 0.004) if (std::fabs(pitchAt(s, t) - 60) < 0.15) { reach = t - 0.6; break; }
        check(reach > 0.17 && reach < 0.24, "the slide takes %.0f ms (Glide 200 ms)", reach * 1000);
        const float r1 = rmsOf(s.L, S(0.5), S(0.58)), r2 = rmsOf(s.L, S(0.62), S(0.7));
        check(std::fabs(db(r2) - db(r1)) < 1.5, "legato: no retrigger, level %.1f -> %.1f dB", db(r1), db(r2));
        Session d(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pPlayMode, kLegato}, {P::pGlide, 200}, {P::pRelease, 1}}, true);
        d.run(1.0, {on(0.1, 48), off(0.5, 48), on(0.6, 60), off(0.95, 60)});
        check(std::fabs(pitchAt(d, 0.63) - 60) < 0.15, "legato: a detached note does not glide (%.2f at +30 ms)", pitchAt(d, 0.63));
        Session m(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pPlayMode, kMono}, {P::pGlide, 200}, {P::pRelease, 1}}, true);
        m.run(1.0, {on(0.1, 48), off(0.5, 48), on(0.6, 60), off(0.95, 60)});
        check(std::fabs(pitchAt(m, 0.7) - 54) < 0.8, "mono: glides even between detached notes (%.2f half way)", pitchAt(m, 0.7));
        Session back(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pPlayMode, kLegato}, {P::pGlide, 100}}, true);
        back.run(1.4, {on(0.1, 48), on(0.4, 55), off(0.7, 55), off(1.3, 48)});
        check(std::fabs(pitchAt(back, 1.0) - 48) < 0.1, "legato: releasing the top note returns to the held one (%.2f)", pitchAt(back, 1.0));
        Session p(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pGlide, 200}}, true);
        p.run(1.0, {on(0.1, 48), on(0.5, 60), off(0.9, 48), off(0.9, 60)});
        check(p.p->voices_[1].active || p.p->voices_[0].active, "poly glide renders");
        double pg = 0; for (auto& v : p.p->voices_) if (v.active && v.note == 60) pg = v.pitch;
        check(std::fabs(pg - 60) < 0.01, "poly glide: the second voice has arrived at its note (%.2f)", pg);
    }

    // ================================================================================================
    std::printf("\n== Motion: Colour, Drift, matrix, envelope ==\n");
    {
        Session c(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 10}, {P::pCutoff, 2000}, {P::pColMorph1, 60}, {P::pColFilter, 40}}, true);
        c.run(4.0, {on(0.05, 57)}, {{1.0, P::pColour, 100.0f}, {2.5, P::pColour, 0.0f}});
        const double b0 = centroid(c.L, S(0.6)), b1 = centroid(c.L, S(2.0)), b2 = centroid(c.L, S(3.5));
        check(b1 > 2.0 * b0 && std::fabs(b2 - b0) < 0.15 * b0, "Colour 0 -> 100 -> 0 brightens %.0f -> %.0f Hz and returns to %.0f Hz", b0, b1, b2);
        auto d2 = [&](double a, double b) { float m = 0; for (size_t i = S(a); i < S(b); ++i) m = std::max(m, std::fabs(c.L[i] - 2 * c.L[i - 1] + c.L[i - 2])); return m; };
        const float before = d2(0.4, 0.98), during = d2(0.98, 1.1), after = d2(1.3, 2.4);
        check(during <= 1.15f * std::max(before, after), "Colour jumped 0 -> 100: no click (curvature %.4f during, %.4f/%.4f before/after)", during, before, after);

        Session dr(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pDrift, 100}}, true);
        dr.p->set(P::pDrift, 100); dr.p->reset();
        dr.run(8.0, {on(0.05, 57)});
        double lo = 1e9, hi = -1e9;
        for (double tt = 0.5; tt < 7.9; tt += 0.25) { const double st = stOf(zcFreq(dr.L, S(tt), S(0.2))); lo = std::min(lo, st); hi = std::max(hi, st); }
        check(hi - lo > 0.02 && hi - lo < 0.3 && std::fabs(0.5 * (hi + lo) - 57) < 0.15, "Drift 100 %% wanders the pitch %.1f cents in 8 s (bounded, centred on the note)", (hi - lo) * 100);

        Session v(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pSrc4, kSrcVel}, {P::pDst4, kDstMorph1}, {P::pAmt4, 80}, {P::pVelLevel, 0}}, true);
        v.run(1.4, {on(0.05, 57, 20), off(0.6, 57), on(0.75, 57, 127), off(1.3, 57)});
        check(centroid(v.L, S(0.9)) > 2.0 * centroid(v.L, S(0.3)), "matrix Velocity -> Morph 1: a hard note is brighter (%.0f vs %.0f Hz)",
              centroid(v.L, S(0.9)), centroid(v.L, S(0.3)));
        Session e(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pEnvMorph, 80}, {P::pMDecay, 300}, {P::pMSustain, 0}}, true);
        e.run(1.5, {on(0.05, 57)});
        check(centroid(e.L, S(0.08)) > 2.0 * centroid(e.L, S(1.2)), "Env>Morph: bright at the strike (%.0f Hz), back to the sine after the decay (%.0f Hz)",
              centroid(e.L, S(0.08)), centroid(e.L, S(1.2)));
        Session w(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pSrc1, kSrcWheel}, {P::pDst1, kDstPitch}, {P::pAmt1, 100}}, true);
        w.run(1.0, {on(0.05, 57), {0.4, mpc::MidiEvent::cc(0, 1, 127)}});
        check(std::fabs(stOf(zcFreq(w.L, S(0.8), S(0.1))) - 69) < 0.1, "Mod Wheel -> Pitch 100 %%: up an octave (%.2f)", stOf(zcFreq(w.L, S(0.8), S(0.1))));
        Session pb(0, {{P::pTable1, kTabBasic}, {P::pMorph1, 0}, {P::pBendRange, 7}}, true);
        mpc::MidiEvent bendUp; bendUp.size = 3; bendUp.data[0] = 0xE0; bendUp.data[1] = 127; bendUp.data[2] = 127;
        pb.run(1.0, {on(0.05, 57), {0.3, bendUp}});
        check(std::fabs(stOf(zcFreq(pb.L, S(0.8), S(0.1))) - 64) < 0.1, "pitch bend, range 7: up a fifth (%.2f)", stOf(zcFreq(pb.L, S(0.8), S(0.1))));
    }

    // ================================================================================================
    std::printf("\n== Safety: NaN, denormals, every program ==\n");
    int numPrograms = 0;
    {
        Cuttlefish probe; numPrograms = (int)probe.programs_.size();
        bool allFinite = true; int subs = 0;
        for (int pr = 0; pr < numPrograms; ++pr)
        {
            Session s(pr);
            s.playing = true;
            std::vector<Ev> ev;
            for (int i = 0; i < 20; ++i) { const int nn = 36 + (i * 13) % 50; ev.push_back(on(0.05 + 0.12 * i, nn, 30 + (i * 37) % 97)); ev.push_back(off(0.3 + 0.12 * i, nn)); }
            ev.push_back({1.0, mpc::MidiEvent::cc(0, 1, 100)});
            s.run(9.0, ev);
            allFinite = allFinite && !s.bad;
            subs += subnormals(s.L.data(), s.L.size()) + subnormals(s.R.data(), s.R.size());
        }
        check(allFinite, "all %d programs: dense playing, then 6 s of tails, no NaN/Inf", numPrograms);
        check(subs == 0, "no denormal output samples (%d)", subs);
        // random knob abuse
        Session z(0);
        unsigned rr = 99;
        z.perBlock = [&](Session& se) {
            for (int k = 0; k < 3; ++k) { rr = rr * 1664525u + 1013904223u; const int pi = 1 + (int)((rr >> 8) % (unsigned)(se.p->numParams() - 1));
                rr = rr * 1664525u + 1013904223u; se.p->setNormalised(pi, (float)((rr >> 8) & 0xFFFF) / 65535.0f); }
        };
        std::vector<Ev> ev;
        for (int i = 0; i < 60; ++i) { const int nn = 30 + (i * 17) % 70; ev.push_back(on(0.03 + 0.1 * i, nn, 127)); ev.push_back(off(0.2 + 0.1 * i, nn)); }
        mpc::MidiEvent at; at.size = 2; at.data[0] = 0xD0; at.data[1] = 100;
        ev.push_back({2.0, at});
        z.run(8.0, ev);
        check(!z.bad && peakOf(z.L) <= 1.0f && peakOf(z.R) <= 1.0f, "random knob moves on every block while playing: finite, peak %.1f dBFS", db(std::max(peakOf(z.L), peakOf(z.R))));
        // long tail without flush-to-zero: internal state
        Session t(6, {{P::pReverb, 80}, {P::pDelay, 60}, {P::pDlyFdbk, 90}}); t.ftz = false;
        t.run(1.0, {on(0.05, 60), off(0.5, 60)});
        t.run(40.0);
        int st = subnormals(t.L.data() + S(30), S(10)) + subnormals(t.R.data() + S(30), S(10));
        info("without flush-to-zero (the framework always sets it), 30-40 s into a feedback tail: %d subnormal output samples", st);
        Session t2(6, {{P::pReverb, 80}, {P::pDelay, 60}, {P::pDlyFdbk, 90}});
        t2.run(1.0, {on(0.05, 60), off(0.5, 60)});
        t2.run(40.0);
        int st2 = subnormals(t2.L.data(), t2.L.size()) + subnormals(t2.R.data(), t2.R.size());
        for (auto& l : t2.p->reverb_.line_) st2 += subnormals(l.data(), l.size());
        st2 += subnormals(t2.p->delay_.l_.data(), t2.p->delay_.l_.size()) + subnormals(t2.p->delay_.r_.data(), t2.p->delay_.r_.size());
        check(st2 == 0, "with the framework's flush-to-zero: no denormals in the output or the delay/reverb lines after 40 s (%d)", st2);
    }

    // ================================================================================================
    std::printf("\n== Levels per program ==\n");
    {
        float worstPk = 0.0f;
        for (int pr = 0; pr < numPrograms; ++pr)
        {
            Session one(pr); one.bpm = 100; one.playing = true;
            one.run(2.5, {on(0.05, 60, 100), off(1.8, 60)});
            const bool mono = one.p->getInt(P::pPlayMode) != kPoly;
            Session ch(pr); ch.bpm = 100; ch.playing = true;
            if (mono) ch.run(2.5, {on(0.05, 36, 127), off(1.8, 36)});
            else ch.run(2.5, {on(0.05, 48, 127), on(0.05, 55, 127), on(0.05, 60, 127), on(0.05, 64, 127), off(1.8, 48), off(1.8, 55), off(1.8, 60), off(1.8, 64)});
            const float p1 = std::max(peakOf(one.L), peakOf(one.R)), r1 = rmsOf(one.L, S(0.3), S(1.7));
            const float p4 = std::max(peakOf(ch.L), peakOf(ch.R));
            worstPk = std::max(worstPk, p4);
            std::printf("      %-20s note: peak %6.1f dBFS rms %6.1f | %s: peak %6.1f dBFS\n", one.p->programs_[(size_t)pr].name.c_str(),
                        db(p1), db(r1), mono ? "low note vel 127" : "4-note chord vel 127", db(p4));
            check(db(p1) > -20.0 && db(p1) < -4.0, "%s: a single note peaks at %.1f dBFS", one.p->programs_[(size_t)pr].name.c_str(), db(p1));
        }
        check(worstPk < 0.99f, "loudest chord peak %.1f dBFS (soft limiter above -3 dBFS)", db(worstPk));
    }

    // ================================================================================================
    std::printf("\n== CPU ==\n");
    {
        auto measure = [&](const char* what, int uni, bool both, bool fx) {
            Session c(1, {{P::pUnison, (float)uni}, {P::pLevel2, both ? 60.0f : 0.0f}, {P::pVoices, 16}, {P::pRelease, 5000},
                          {P::pAttack, 5}, {P::pChorus, fx ? 40.0f : 0.0f}, {P::pDelay, fx ? 30.0f : 0.0f}, {P::pReverb, fx ? 40.0f : 0.0f},
                          {P::pWarpMode1, kWSync}, {P::pWarp1, 30}, {P::pSub, 30}, {P::pNoise, 10}, {P::pDrive, 30}});
            c.fixedBlock = 128;
            std::vector<Ev> ev;
            for (int i = 0; i < 16; ++i) ev.push_back(on(0.001 * i, 40 + 3 * i, 100));
            c.run(0.5, ev);
            const double t0 = c.cpu; const long long p0 = c.pos;
            c.run(6.5);
            const double sec = (double)(c.pos - p0) / SR;
            const double pct = 100.0 * (c.cpu - t0) / sec;
            int live = 0; for (auto& v : c.p->voices_) if (v.active && !v.fading) ++live;
            info("%-44s %2d voices: %.2f %% of one desktop core (%.3f %% per voice)", what, live, pct, pct / std::max(live, 1));
            return pct;
        };
        const double a = measure("unison 1, one osc, no fx (Init-like)", 1, false, false);
        const double b = measure("unison 1, two osc + sub + noise, sync, all fx", 1, true, true);
        const double c = measure("unison 2, two osc + sub + noise, sync, all fx", 2, true, true);
        const double d = measure("unison 4, two osc + sub + noise, sync, all fx", 4, true, true);
        check(b < 5.0, "16 voices, unison 1, everything on: %.2f %% of a desktop core", b);
        info("worst case (16 x unison 4 x 2 osc) %.2f %%; MPC estimate ~10-20x desktop (see README)", d);
        (void)a; (void)c;
    }

    // ================================================================================================
    std::printf("\n== Demo ==\n");
    // argv[1]: the 1.0 programs and the Colour sweep; argv[2]: the programs added in 1.1.
    for (int dm = 1; dm < argc && dm <= 2; ++dm)
    {
        const int first11 = 16;
        const int from = dm == 1 ? 0 : first11, to = dm == 1 ? std::min(first11, numPrograms) : numPrograms;
        Session demo(0);
        demo.bpm = 100.0; demo.play(true, 0.0);
        const double beat = 60.0 / demo.bpm;
        double t = 0.2;
        std::vector<std::pair<double, std::string>> marks;
        for (int pr = from; pr < to; ++pr)
        {
            demo.p->setProgram(pr);
            const mpc::Program& prg = demo.p->programs_[(size_t)pr];
            marks.push_back({t, prg.name});
            const bool mono = demo.p->getInt(P::pPlayMode) != kPoly;
            const float rel = demo.p->get(P::pRelease) * 0.001f;
            const bool longNotes = demo.p->get(P::pAttack) > 100.0f;
            std::vector<Ev> ev;
            if (mono)
            {
                const bool bass = demo.p->get(P::pCutoff) < 1000.0f || demo.p->get(P::pSub) > 0.0f;
                const int base = bass ? 36 : 60;
                const int seq[16] = {0, 0, 12, 0, 3, 0, 7, 10, 0, 0, 12, 15, 10, 7, 5, 3};
                for (int i = 0; i < 16; ++i)
                {
                    const double st = t + i * 0.5 * beat;
                    ev.push_back(on(st, base + seq[i], 100));
                    ev.push_back(off(st + 0.5 * beat * (i % 4 == 3 ? 0.5 : 1.08), base + seq[i]));   // overlapping = legato slides
                }
                demo.run(t + 8.5 * beat, ev);
                t += 8.5 * beat + 0.6;
            }
            else if (longNotes)
            {
                const int chords[4][4] = {{50, 57, 62, 65}, {46, 53, 58, 62}, {43, 50, 55, 62}, {45, 52, 57, 61}};
                for (int c = 0; c < 4; ++c)
                    for (int k = 0; k < 4; ++k) { ev.push_back(on(t + c * 4 * beat, chords[c][k], 90)); ev.push_back(off(t + (c + 1) * 4 * beat - 0.05, chords[c][k])); }
                demo.run(t + 16 * beat + std::min(rel, 3.0f), ev);
                t += 16 * beat + std::min(rel, 3.0f) + 0.3;
            }
            else
            {
                const int arp[16] = {62, 65, 69, 72, 69, 65, 62, 65, 58, 62, 65, 70, 65, 62, 57, 61};
                for (int i = 0; i < 16; ++i)
                {
                    const double st = t + i * 0.5 * beat;
                    ev.push_back(on(st, arp[i], 70 + (i % 4 == 0 ? 40 : (i * 13) % 30)));
                    ev.push_back(off(st + 0.4 * beat, arp[i]));
                }
                ev.push_back(on(t + 8.5 * beat, 50, 100)); ev.push_back(on(t + 8.5 * beat, 57, 100)); ev.push_back(on(t + 8.5 * beat, 62, 100));
                ev.push_back(off(t + 11 * beat, 50)); ev.push_back(off(t + 11 * beat, 57)); ev.push_back(off(t + 11 * beat, 62));
                demo.run(t + 11 * beat + std::min(rel, 2.5f) + 0.5, ev);
                t += 11 * beat + std::min(rel, 2.5f) + 0.8;
            }
        }
        // the slow Colour sweep on Colour Shift
        if (dm == 1) {
        int cs = 0; for (int pr = 0; pr < numPrograms; ++pr) if (demo.p->programs_[(size_t)pr].name == "Colour Shift") cs = pr;
        demo.p->setProgram(cs);
        marks.push_back({t, "Colour sweep (Colour Shift, Colour 0 -> 100 -> 0 over 24 s)"});
        std::vector<Ev> ev;
        const int chord[5] = {38, 50, 57, 62, 64};
        for (int k = 0; k < 5; ++k) { ev.push_back(on(t, chord[k], 95)); ev.push_back(off(t + 24.0, chord[k])); }
        std::vector<Auto> au;
        for (int i = 0; i <= 240; ++i) { const double x = i / 240.0; au.push_back({t + 24.0 * x, P::pColour, (float)(100.0 * (1.0 - std::fabs(2.0 * x - 1.0)))}); }
        demo.run(t + 28.0, ev, au);
        }
        writeWav(argv[dm], demo.L, demo.R);
        std::printf("INFO  wrote %s (%.1f s)\n", argv[dm], demo.L.size() / SR);
        for (auto& m : marks) std::printf("      %6.1f s  %s\n", m.first, m.second.c_str());
        check(!demo.bad && peakOf(demo.L) < 1.0f, "demo finite, peak %.1f dBFS", db(std::max(peakOf(demo.L), peakOf(demo.R))));
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
