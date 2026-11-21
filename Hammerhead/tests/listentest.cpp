// Does Hammerhead sound like hitting something? Checks the attack, the tuning, what hardness and
// strike position do, decay times, muting and the pedal, re-strikes, voice stealing, the pitch
// drop, chord headroom and CPU; (1.1) that a new Model or Exciter takes effect on a ringing note,
// and that Bow and Blow sustain in tune on every model; (1.2) Buzz, the LFO and per-hit randomness.
// Then renders a demo WAV.
//
//   mkdir -p tests/build
//   clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
//   tests/build/listentest build/hammerhead-demo.wav
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <chrono>
#include <algorithm>

#define private public
#define protected public
#include "../src/Hammerhead.cpp"
#undef private
#undef protected

static const double SR = 44100.0;
static int failures = 0, passes = 0;
static void check(bool ok, const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", buf);
    if (ok) ++passes; else ++failures;
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

// ---- rendering ------------------------------------------------------------------------------------
struct Ev { double t; mpc::MidiEvent e; };
static Ev on(double t, int note, int vel = 100) { return {t, mpc::MidiEvent::noteOn(0, note, vel)}; }
static Ev off(double t, int note) { return {t, mpc::MidiEvent::noteOff(0, note)}; }
static Ev cc(double t, int c, int v) { return {t, mpc::MidiEvent::cc(0, c, v)}; }

static int maxVoices = 0, maxSlots = 0;
static int activeVoices(Hammerhead& p) { int c = 0; for (auto& v : p.voices_) if (v.stage != hammerhead::kOff) ++c; return c; }
static int ringingVoices(Hammerhead& p) { int c = 0; for (auto& v : p.voices_) if (v.stage == hammerhead::kOn) ++c; return c; }

static void render(Hammerhead& p, std::vector<Ev> evs, double seconds, std::vector<float>& L, std::vector<float>& R,
                   double* cpuSec = nullptr, int fixedBlock = 0, bool playing = false, double bpm = 120.0)
{
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    const int total = (int)(seconds * SR);
    L.assign((size_t)total, 0.0f); R.assign((size_t)total, 0.0f);
    unsigned rs = 12345;
    size_t ei = 0;
    std::vector<mpc::MidiEvent> blockEv;
    double cpu = 0.0;
    maxVoices = 0; maxSlots = 0;
    for (int pos = 0; pos < total;)
    {
        rs = rs * 1664525u + 1013904223u;
        const int block = std::min(fixedBlock > 0 ? fixedBlock : 32 + (int)((rs >> 8) % 481), total - pos);   // 32..512, like the MPC
        blockEv.clear();
        while (ei < evs.size() && (int)(evs[ei].t * SR) < pos + block)
        {
            mpc::MidiEvent e = evs[ei].e; e.sampleOffset = std::max(0, (int)(evs[ei].t * SR) - pos);
            blockEv.push_back(e); ++ei;
        }
        float* outs[2] = {L.data() + pos, R.data() + pos};
        mpc::ProcessContext ctx;
        ctx.outputs = outs; ctx.numInputs = 0; ctx.numOutputs = 2; ctx.numFrames = block;
        ctx.midi = blockEv.data(); ctx.numMidiEvents = (int)blockEv.size();
        ctx.sampleRate = SR;
        ctx.transport.valid = ctx.transport.tempoValid = true; ctx.transport.bpm = bpm;
        ctx.transport.playing = playing; ctx.transport.ppqValid = playing;
        ctx.transport.ppqPos = pos / SR * bpm / 60.0;
        const auto t0 = std::chrono::steady_clock::now();
        p.process(ctx);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        maxVoices = std::max(maxVoices, ringingVoices(p));
        maxSlots = std::max(maxSlots, activeVoices(p));
        pos += block;
    }
    if (cpuSec) *cpuSec = cpu;
}

static void loadProgram(Hammerhead& p, int prog)
{
    const mpc::Program& pr = p.programs_[(size_t)prog];
    for (int i = 0; i < p.numParams(); ++i) p.set(i, pr.plainValues[(size_t)i]);
}

static std::unique_ptr<Hammerhead> make(int prog, std::initializer_list<std::pair<int, float>> overrides = {})
{
    auto p = std::make_unique<Hammerhead>();
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

constexpr int kFft = 16384;
// Magnitude spectrum (dB) of the mono mix of `len` samples from time t, Hann-windowed and
// zero-padded to 16384 points.
static std::vector<float> spectrum(const std::vector<float>& L, const std::vector<float>& R, double t, int len = kFft)
{
    std::vector<std::complex<float>> a(kFft);
    const long start = (long)(t * SR);
    for (int i = 0; i < len; ++i)
    {
        const long s = start + i;
        const float x = (s >= 0 && s < (long)L.size()) ? 0.5f * (L[(size_t)s] + R[(size_t)s]) : 0.0f;
        a[i] = std::complex<float>(x * (0.5f - 0.5f * std::cos(6.2831853f * (float)i / (float)len)), 0.0f);
    }
    fft(a);
    std::vector<float> m(kFft / 2);
    for (int k = 0; k < kFft / 2; ++k) m[k] = 10.0f * std::log10(std::norm(a[k]) + 1e-20f);
    return m;
}
static float binHz() { return (float)(SR / kFft); }
// Frequency of the strongest peak between lo and hi Hz, refined by parabolic interpolation.
static float peakHz(const std::vector<float>& m, float lo, float hi)
{
    int best = -1;
    for (int k = std::max(1, (int)(lo / binHz())); k <= (int)(hi / binHz()) && k < (int)m.size() - 1; ++k)
        if (best < 0 || m[k] > m[best]) best = k;
    const float a = m[best - 1], b = m[best], c = m[best + 1];
    const float den = a - 2.0f * b + c;
    const float off = std::fabs(den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f;
    return ((float)best + clamp(off, -0.5f, 0.5f)) * binHz();
}
// Level (dB) of the strongest bin within +-3 % of f.
static float levelAt(const std::vector<float>& m, float f)
{
    float best = -400.0f;
    for (int k = (int)(f * 0.97f / binHz()); k <= (int)(f * 1.03f / binHz()) + 1 && k < (int)m.size(); ++k) best = std::max(best, m[k]);
    return best;
}
static float centroid(const std::vector<float>& m)
{
    double tot = 1e-30, cen = 0.0;
    for (int k = 1; k < (int)m.size(); ++k) { const double p = std::pow(10.0, m[k] / 10.0); tot += p; cen += p * k * binHz(); }
    return (float)(cen / tot);
}
static float peakOf(const std::vector<float>& L, const std::vector<float>& R, double t0 = 0.0, double t1 = 1e9)
{
    float p = 0.0f;
    const size_t a = (size_t)std::max(0.0, t0 * SR), b = std::min(L.size(), (size_t)(t1 * SR));
    for (size_t i = a; i < b; ++i) p = std::max(p, std::max(std::fabs(L[i]), std::fabs(R[i])));
    return p;
}
static double peakTime(const std::vector<float>& L, const std::vector<float>& R)
{
    float p = 0.0f; size_t at = 0;
    for (size_t i = 0; i < L.size(); ++i) { const float a = std::max(std::fabs(L[i]), std::fabs(R[i])); if (a > p) { p = a; at = i; } }
    return (double)at / SR;
}
static float rmsDb(const std::vector<float>& L, const std::vector<float>& R, double t0, double t1)
{
    double s = 0.0; size_t n = 0;
    for (size_t i = (size_t)(t0 * SR); i < (size_t)(t1 * SR) && i < L.size(); ++i) { s += 0.5 * ((double)L[i] * L[i] + (double)R[i] * R[i]); ++n; }
    return (float)(10.0 * std::log10(s / std::max<size_t>(n, 1) + 1e-30));
}
static bool finite(const std::vector<float>& L, const std::vector<float>& R)
{
    for (size_t i = 0; i < L.size(); ++i) if (!std::isfinite(L[i]) || !std::isfinite(R[i])) return false;
    return true;
}
static float db(float x) { return 20.0f * std::log10(std::max(x, 1e-10f)); }
static float cents(float f, float ref) { return 1200.0f * std::log2(f / ref); }

// Clicks: a jump in value or slope shows as a spike in the second difference. Each 2 ms window's
// largest second difference is compared with the median of its neighbours over +-50 ms. Only
// meaningful with soft mallets (a hard mallet's onset is supposed to be a spike).
static int countClicks(const std::vector<float>& x, double from, double& worstT, float& worstRatio)
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
static void checkClicks(const std::vector<float>& L, const std::vector<float>& R, double from, const char* what)
{
    double tl, tr; float rl, rr;
    const int cl = countClicks(L, from, tl, rl), cr = countClicks(R, from, tr, rr);
    check(cl + cr == 0, "%s: no clicks (worst jump %.1fx its surroundings at %.2f s)", what,
          rl > rr ? rl : rr, rl > rr ? tl : tr);
}

// The largest click ratio (as in countClicks) in the 2 ms windows from t - 4 ms to t + 14 ms of each time:
// for events (voice steals) inside a dense mix, where natural beating elsewhere can reach 3x.
static float worstNear(const std::vector<float>& x, const std::vector<double>& times, double& at)
{
    const int W = (int)(0.002 * SR);
    const int nw = (int)x.size() / W;
    std::vector<float> mx((size_t)nw, 0.0f);
    for (int w = 0; w < nw; ++w)
        for (int i = std::max(2, w * W); i < (w + 1) * W; ++i)
            mx[(size_t)w] = std::max(mx[(size_t)w], std::fabs(x[(size_t)i] - 2.0f * x[(size_t)i - 1] + x[(size_t)i - 2]));
    float worst = 0.0f; at = 0.0;
    std::vector<float> nb;
    for (double t : times)
        for (int w = (int)((t - 0.004) * SR / W); w <= (int)((t + 0.014) * SR / W) && w < nw; ++w)
        {
            nb.clear();
            for (int j = std::max(0, w - 25); j <= std::min(nw - 1, w + 25); ++j) if (std::abs(j - w) > 1) nb.push_back(mx[(size_t)j]);
            std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end());
            const float r = mx[(size_t)w] / std::max(nb[nb.size() / 2], 1e-9f);
            if (r > worst) { worst = r; at = (double)w * W / SR; }
        }
    return worst;
}

// Pitch by autocorrelation of the mono mix over 4096 samples from t: the first lag (between 1/1200
// and 1/60 s) whose normalised autocorrelation is within 10 % of the best, refined parabolically.
static float acPitch(const std::vector<float>& L, const std::vector<float>& R, double t)
{
    const int N = 4096, lo = (int)(SR / 1200.0), hi = (int)(SR / 60.0);
    const size_t s0 = (size_t)(t * SR);
    std::vector<float> x((size_t)(N + hi + 2));
    for (size_t i = 0; i < x.size(); ++i) x[i] = s0 + i < L.size() ? 0.5f * (L[s0 + i] + R[s0 + i]) : 0.0f;
    std::vector<double> ac((size_t)hi + 2, 0.0);
    double e0 = 1e-20;
    for (int i = 0; i < N; ++i) e0 += (double)x[(size_t)i] * x[(size_t)i];
    double best = 0.0;
    for (int lag = lo; lag <= hi + 1; ++lag)
    {
        double a = 0.0;
        for (int i = 0; i < N; ++i) a += (double)x[(size_t)i] * x[(size_t)(i + lag)];
        ac[(size_t)lag] = a / e0;
        if (lag <= hi) best = std::max(best, ac[(size_t)lag]);
    }
    for (int lag = lo + 1; lag <= hi; ++lag)
        if (ac[(size_t)lag] >= 0.9 * best && ac[(size_t)lag] >= ac[(size_t)lag - 1] && ac[(size_t)lag] >= ac[(size_t)lag + 1])
        {
            const double a = ac[(size_t)lag - 1], b = ac[(size_t)lag], c = ac[(size_t)lag + 1];
            const double den = a - 2.0 * b + c;
            const double off = std::fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
            return (float)(SR / ((double)lag + std::max(-0.5, std::min(0.5, off))));
        }
    return 0.0f;
}
// Air (1.1.1): power away from the harmonics of f0 (more than 3 % off every k*f0) relative to the power near them,
// 100 Hz..16 kHz, at t = 1 s; and that noise's centroid and the share of it above 3 kHz.
static void noiseToTone(const std::vector<float>& L, const std::vector<float>& R, float f0, float& ratioDb, float& cen, float& above3k)
{
    const auto s = spectrum(L, R, 1.0, 16384);
    double tone = 1e-30, noise = 1e-30, c = 0.0, hf = 1e-30;
    for (int k = (int)(100 / binHz()); k < (int)(16000 / binHz()); ++k)
    {
        const float f = k * binHz(), h = f / f0;
        const double pw = std::pow(10.0, s[k] / 10.0);
        if (std::round(h) >= 1 && std::fabs(h - std::round(h)) * f0 / f < 0.03f) tone += pw;
        else { noise += pw; c += pw * f; if (f > 3000.0f) hf += pw; }
    }
    ratioDb = (float)(10.0 * std::log10(noise / tone)); cen = (float)(c / noise); above3k = (float)(10.0 * std::log10(hf / noise));
}
static bool sustained(Hammerhead& p) { return p.getInt(Hammerhead::kExciter) != hammerhead::kMallet; }

using H = Hammerhead;

int main(int argc, char** argv)
{
    Hammerhead probe; probe.prepare(SR, 512);
    const int numPrograms = (int)probe.programs_.size();
    std::vector<float> L, R;

    // ---- 1. Every program: one note. Is it percussive, and does it go away? ----------------------
    std::printf("\n== One note per program, velocity 100 ==\n");
    std::printf("      %-18s %8s %8s %9s %9s %9s\n", "program", "peak dB", "peak ms", "5 ms dB", "+1 s dB", "free at");
    for (int prog = 0; prog < numPrograms; ++prog)
    {
        auto p = make(prog);
        if (sustained(*p))
        {
            // Bowed and blown programs: they should speak, hold steady while the note is held, and
            // ring away after it.
            render(*p, {on(0.0, 60, 100), off(2.5, 60)}, 12.0, L, R);
            const float a = rmsDb(L, R, 1.0, 1.4), b = rmsDb(L, R, 2.0, 2.4), tail = rmsDb(L, R, 3.6, 3.8);
            std::printf("      %-18s held %6.1f dB -> %6.1f dB, 1.1 s after release %6.1f dB, peak %6.1f dB\n",
                        p->programName(prog), a, b, tail, db(peakOf(L, R)));
            check(finite(L, R) && a > -34.0f && db(peakOf(L, R)) < -6.0f && std::fabs(a - b) < 3.0f,
                  "%s: sustains while held (%.1f -> %.1f dB RMS), peak %.1f dBFS", p->programName(prog), a, b, db(peakOf(L, R)));
            check(tail < b - 8.0f && activeVoices(*p) == 0, "%s: and dies away after release (%.1f dB down 1.1 s later; voice freed by 12 s)",
                  p->programName(prog), b - tail);
            continue;
        }
        render(*p, {on(0.0, 60, 100), off(0.25, 60)}, 14.0, L, R);
        const float pk = peakOf(L, R);
        const double pt = peakTime(L, R);
        const float early = peakOf(L, R, 0.0, 0.005);
        const float late = rmsDb(L, R, 1.0, 1.1) - rmsDb(L, R, 0.0, 0.1);
        // When did the last voice go?
        auto q = make(prog);
        std::vector<float> l2, r2;
        double freeAt = -1.0;
        {
            std::vector<Ev> evs = {on(0.0, 60, 100), off(0.25, 60)};
            for (double t = 0.0; t < 40.0; t += 0.5)
            {
                render(*q, t == 0.0 ? evs : std::vector<Ev>{}, 0.5, l2, r2);
                if (activeVoices(*q) == 0) { freeAt = t + 0.5; break; }
            }
        }
        std::printf("      %-18s %8.1f %8.1f %9.1f %9.1f %8.1fs\n", p->programName(prog), db(pk), pt * 1000.0, db(early), late, freeAt);
        const bool ok = finite(L, R) && db(pk) > -24.0f && db(pk) < -3.0f;
        check(ok, "%s: finite, peak %.1f dBFS (wanted -24..-3)", p->programName(prog), db(pk));
        check(db(early) > db(pk) - 6.0f, "%s: leans into the attack - within 5 ms it is %.1f dB below its peak",
              p->programName(prog), db(pk) - db(early));
        check(freeAt > 0.0, "%s: the voice frees itself after the ring dies (%.1f s)", p->programName(prog), freeAt);
    }

    // ---- 2. Tuning: every model plays the note it is given --------------------------------------
    std::printf("\n== Tuning: A4 (440 Hz), every model, octave 0, no drop ==\n");
    for (int m = 0; m < hammerhead::kNumModels; ++m)
    {
        auto p = make(0, {{H::kModel, (float)m}, {H::kOctave, 0}, {H::kDrop, 0}, {H::kRoom, 0}, {H::kBody, 0},
                          {H::kDecay, 4}, {H::kHard, 50}, {H::kPos, 30}});
        render(*p, {on(0.0, 69, 100)}, 1.0, L, R);
        const auto spec = spectrum(L, R, 0.05);
        const float f = peakHz(spec, 400.0f, 480.0f);
        check(std::fabs(cents(f, 440.0f)) < 8.0f, "%s: A4 rings at %.2f Hz (%+.1f cents)", hammerhead::kModelNames[m], f, cents(f, 440.0f));
    }

    // ---- 3. Hardness: a soft mallet only rings the fundamental, a hard one rings everything ------
    std::printf("\n== Hardness (Marimba, C4, velocity 100, first 50 ms) ==\n");
    {
        float over[2], cen[2];
        for (int i = 0; i < 2; ++i)
        {
            auto p = make(0, {{H::kHard, i ? 100.0f : 0.0f}, {H::kVelHard, 0}, {H::kClick, 0}, {H::kRoom, 0}, {H::kBody, 0}});
            render(*p, {on(0.0, 60, 100)}, 0.5, L, R);
            const auto s = spectrum(L, R, 0.0, 2205);
            over[i] = std::max(levelAt(s, 261.6f * 3.98f), levelAt(s, 261.6f * 9.38f)) - levelAt(s, 261.6f);
            cen[i] = centroid(s);
            std::printf("      hardness %3d%%: strongest overtone %6.1f dB re fundamental, centroid %5.0f Hz\n", i * 100, over[i], cen[i]);
        }
        check(over[1] - over[0] > 15.0f, "a hard mallet rings the overtones %.1f dB louder than a soft one", over[1] - over[0]);
        check(cen[1] > 1.25f * cen[0], "and the attack is brighter: centroid %.0f Hz -> %.0f Hz", cen[0], cen[1]);
        // Velocity does the same through Vel>Hard.
        float ov[2];
        for (int i = 0; i < 2; ++i)
        {
            auto p = make(0, {{H::kVelHard, 100}, {H::kClick, 0}, {H::kRoom, 0}, {H::kBody, 0}});
            render(*p, {on(0.0, 60, i ? 127 : 30)}, 0.5, L, R);
            const auto s = spectrum(L, R, 0.0, 2205);
            ov[i] = levelAt(s, 261.6f * 3.98f) - levelAt(s, 261.6f);
        }
        check(ov[1] - ov[0] > 8.0f, "harder hits are brighter, not just louder: 4:1 overtone %.1f dB -> %.1f dB re fundamental (vel 30 -> 127)", ov[0], ov[1]);
    }

    // ---- 4. Strike position ------------------------------------------------------------------
    std::printf("\n== Strike position ==\n");
    {
        // Marimba C4: the 4:1 overtone is antisymmetric (a node at the centre); the fundamental has
        // a node about a third of the way out.
        float f1[3], f2[3];
        const float pos[3] = {0.0f, 25.0f, 100.0f};
        for (int i = 0; i < 3; ++i)
        {
            auto p = make(0, {{H::kPos, pos[i]}, {H::kHard, 100}, {H::kVelHard, 0}, {H::kClick, 0}, {H::kTube, 0}, {H::kRoom, 0}, {H::kBody, 0}});
            render(*p, {on(0.0, 60, 100)}, 0.5, L, R);
            const auto s = spectrum(L, R, 0.0, 4096);
            f1[i] = levelAt(s, 261.6f); f2[i] = levelAt(s, 261.6f * 3.98f);
            std::printf("      pos %3.0f%%: fundamental %6.1f dB, 4:1 overtone %6.1f dB\n", pos[i], f1[i], f2[i]);
        }
        check((f2[1] - f1[1]) - (f2[0] - f1[0]) > 10.0f, "marimba: hitting the centre kills the 4:1 overtone (%.1f dB less, relative to the fundamental)",
              (f2[1] - f1[1]) - (f2[0] - f1[0]));
        check(f1[1] - f1[2] > 6.0f, "marimba: hitting near the node thins the fundamental by %.1f dB", f1[1] - f1[2]);

        // Pluck A3: plucked in the middle, the even harmonics vanish.
        float h1[2], h2[2];
        for (int i = 0; i < 2; ++i)
        {
            auto p = make(7, {{H::kPos, i ? 70.0f : 0.0f}, {H::kRoom, 0}, {H::kBody, 0}, {H::kOctave, 0}, {H::kClick, 0}});
            render(*p, {on(0.0, 57, 100)}, 0.6, L, R);
            const auto s = spectrum(L, R, 0.02, 8192);
            h1[i] = levelAt(s, 220.0f); h2[i] = levelAt(s, 440.0f);
        }
        check((h2[1] - h1[1]) - (h2[0] - h1[0]) > 12.0f, "pluck: in the middle of the string the 2nd harmonic drops by %.1f dB (relative)",
              (h2[1] - h1[1]) - (h2[0] - h1[0]));
    }

    // ---- 5. Decay times ------------------------------------------------------------------------
    std::printf("\n== Decay (T60 of the lowest mode at C4) ==\n");
    {
        struct Case { int prog; float decay; const char* name; };
        const Case cases[] = {{0, 2.0f, "Marimba"}, {1, 6.0f, "Vibes"}, {7, 3.0f, "Pluck"}, {8, 3.0f, "Dulcimer"}};
        for (const Case& c : cases)
        {
            auto p = make(c.prog, {{H::kDecay, c.decay}, {H::kTube, 0}, {H::kRoom, 0}, {H::kBody, 0}, {H::kMute, 0},
                                   {H::kOctave, 0}, {H::kMotor, 0}, {H::kDrop, 0}});
            render(*p, {on(0.0, 60, 100)}, std::max(3.0, c.decay * 0.9), L, R);
            const double t0 = c.decay * 0.25, t1 = c.decay * 0.75;
            const float slope = (rmsDb(L, R, t0, t0 + 0.1) - rmsDb(L, R, t1, t1 + 0.1)) / (float)(t1 - t0);
            const float t60 = 60.0f / std::max(slope, 1e-3f);
            check(std::fabs(t60 / c.decay - 1.0f) < 0.3f, "%s: Decay %.1f s measures T60 %.2f s", c.name, c.decay, t60);
        }
    }

    // ---- 6. Mute and the sustain pedal ---------------------------------------------------------
    std::printf("\n== Mute and pedal (Harbour Vibes, Mute 65) ==\n");
    {
        auto ring = make(1, {{H::kMute, 0}, {H::kRoom, 0}, {H::kMotor, 0}});
        render(*ring, {on(0.0, 60, 100), off(0.5, 60)}, 1.2, L, R);
        const float ringDb = std::max(-150.0f, rmsDb(L, R, 0.9, 1.0));
        auto mute = make(1, {{H::kRoom, 0}, {H::kMotor, 0}});
        render(*mute, {on(0.0, 60, 100), off(0.5, 60)}, 1.2, L, R);
        const float muteDb = std::max(-150.0f, rmsDb(L, R, 0.9, 1.0));
        check(ringDb - muteDb > 20.0f, "note-off chokes the bar: %.1f dB quieter 0.4 s after release than with Mute 0", ringDb - muteDb);
        auto ped = make(1, {{H::kRoom, 0}, {H::kMotor, 0}});
        render(*ped, {cc(0.0, 64, 127), on(0.01, 60, 100), off(0.5, 60), cc(1.1, 64, 0)}, 1.6, L, R);
        const float pedDb = rmsDb(L, R, 0.9, 1.0), afterDb = std::max(-150.0f, rmsDb(L, R, 1.5, 1.6));
        check(std::fabs(pedDb - ringDb) < 1.5f, "with the pedal down it rings on after note-off (%.1f dB vs %.1f dB unmuted)", pedDb, ringDb);
        check(pedDb - afterDb > 20.0f, "pedal up chokes it (%.1f dB down 0.4 s later)", pedDb - afterDb);
        checkClicks(L, R, 0.2, "mute on pedal up");
    }

    // ---- 7. Re-strikes and voice stealing (soft mallets, so any click is a fault) ------------------
    std::printf("\n== Re-strikes and stealing ==\n");
    {
        auto p = make(0, {{H::kHard, 0}, {H::kVelHard, 0}, {H::kClick, 0}, {H::kDecay, 20}, {H::kRoom, 0}});
        std::vector<Ev> evs;
        for (int i = 0; i < 8; ++i) evs.push_back(on(0.3 * i, 60, 60 + 8 * i));   // a roll on one bar
        render(*p, evs, 3.0, L, R);
        check(maxVoices == 1, "a roll on one note stays on one voice (%d used): the bar keeps ringing and each hit adds to it", maxVoices);
        checkClicks(L, R, 0.0, "roll on one note");

        auto q = make(0, {{H::kHard, 0}, {H::kVelHard, 0}, {H::kClick, 0}, {H::kDecay, 20}, {H::kRoom, 0}, {H::kTube, 0}});
        evs.clear();
        for (int i = 0; i < 24; ++i) evs.push_back(on(0.15 * i, 40 + i, 90));
        render(*q, evs, 5.0, L, R);
        check(maxVoices <= hammerhead::kNumVoices && finite(L, R), "24 ringing notes: %d ringing voices at most (%d slots incl. fades), finite", maxVoices, maxSlots);
        std::vector<double> steals;
        for (int i = 16; i < 24; ++i) steals.push_back(0.15 * i);
        double tl, tr;
        const float wl = worstNear(L, steals, tl), wr = worstNear(R, steals, tr);
        check(std::max(wl, wr) < 3.0f, "voice stealing (notes 17-24): no clicks at the steals (worst jump %.1fx its surroundings at %.2f s)",
              std::max(wl, wr), wl > wr ? tl : tr);
    }

    // ---- 8. Pitch drop -------------------------------------------------------------------------
    std::printf("\n== Pitch drop (Hull Drum, C3, velocity 120) ==\n");
    {
        auto p = make(9, {{H::kRoom, 0}});
        render(*p, {on(0.0, 48, 120)}, 1.0, L, R);
        const float fEarly = peakHz(spectrum(L, R, 0.0, 1024), 100.0f, 250.0f);
        const float fLate = peakHz(spectrum(L, R, 0.35, 8192), 100.0f, 250.0f);
        check(fEarly > fLate * 1.08f, "the strike starts sharp and falls: %.1f Hz in the first 23 ms -> %.1f Hz", fEarly, fLate);
        check(std::fabs(cents(fLate, 130.81f)) < 10.0f, "and lands on the note (%+.1f cents)", cents(fLate, 130.81f));
    }
    {
        // The same on a string (the delay line glides), and pitch bend on both engines.
        auto p = make(7, {{H::kRoom, 0}, {H::kBody, 0}, {H::kDrop, 7}, {H::kDropTime, 60}, {H::kOctave, 0}});
        render(*p, {on(0.0, 57, 120)}, 1.0, L, R);
        const float fEarly = peakHz(spectrum(L, R, 0.0, 1024), 180.0f, 400.0f);
        const float fLate = peakHz(spectrum(L, R, 0.4, 8192), 200.0f, 240.0f);
        check(fEarly > fLate * 1.15f && std::fabs(cents(fLate, 220.0f)) < 10.0f,
              "pluck with Drop 7: %.1f Hz at the strike -> %.1f Hz (%+.1f cents from A3)", fEarly, fLate, cents(fLate, 220.0f));
        for (int prog : {0, 7})
        {
            auto q = make(prog, {{H::kRoom, 0}, {H::kBody, 0}, {H::kOctave, 0}, {H::kDecay, 4}});
            mpc::MidiEvent pb; pb.size = 3; pb.data[0] = 0xE0; pb.data[1] = 0x7F; pb.data[2] = 0x7F;   // full up = +2 st
            render(*q, {on(0.0, 57, 100), {0.3, pb}}, 1.2, L, R);
            const float f = peakHz(spectrum(L, R, 0.5, 16384), 230.0f, 260.0f);
            check(std::fabs(cents(f, 246.94f)) < 10.0f, "%s: pitch bend up 2 st while ringing -> %.1f Hz (%+.1f cents from B3)",
                  q->programName(prog), f, cents(f, 246.94f));
        }
    }

    // ---- 8b. A new Model or Exciter takes effect on a note that is still ringing --------------------
    std::printf("\n== Changing Model or Exciter while a note rings (1.1) ==\n");
    {
        auto p = make(3, {{H::kDecay, 20}, {H::kRoom, 0}});   // Shell Kalimba (C5), ringing for a long time
        render(*p, {on(0.0, 60, 100)}, 0.5, L, R);
        p->set(H::kModel, 6);                                   // Glass
        render(*p, {on(0.0, 60, 100)}, 0.05, L, R);
        int glass = 0, kalimbaOn = 0, fading = 0;
        for (auto& v : p->voices_)
        {
            if (v.stage == hammerhead::kOn && v.model == 6) ++glass;
            if (v.stage == hammerhead::kOn && v.model == 4) ++kalimbaOn;
            if (v.stage == hammerhead::kSteal) ++fading;
        }
        check(glass == 1 && kalimbaOn == 0, "re-hitting a ringing kalimba note after switching to Glass plays glass (%d glass voice, %d kalimba still ringing)",
              glass, kalimbaOn);
        render(*p, {}, 1.0, L, R);
        const auto s = spectrum(L, R, 0.1, 16384);
        // Glass's 2.32 mode is there; the kalimba's 6.27 tine overtone is not (it faded with the old voice).
        check(levelAt(s, 523.25f * 2.32f) - levelAt(s, 523.25f * 6.27f) > 15.0f,
              "the new hit sounds like glass: 2.32x mode %.1f dB above the kalimba's 6.27x overtone",
              levelAt(s, 523.25f * 2.32f) - levelAt(s, 523.25f * 6.27f));
        p->set(H::kExciter, hammerhead::kBow);
        render(*p, {on(0.0, 60, 100)}, 0.05, L, R);
        int bowed = 0;
        for (auto& v : p->voices_) if (v.stage == hammerhead::kOn && v.note == 60 && v.exc == hammerhead::kBow) ++bowed;
        check(bowed == 1, "and switching the Exciter does the same (%d bowed voice on the note)", bowed);
    }

    // ---- 8c. Bow and Blow on every model -----------------------------------------------------------
    std::printf("\n== Bow and Blow on every model: A3 held 2 s, Pressure 50 %% (1.1) ==\n");
    std::printf("      %-6s %-10s %8s %8s %8s %9s %9s %8s\n", "", "model", "0.1 s", "1 s", "2 s", "pitch", "1 s after", "peak");
    for (int exc = hammerhead::kBow; exc <= hammerhead::kBlow; ++exc)
        for (int m = 0; m < hammerhead::kNumModels; ++m)
        {
            auto p = make(0, {{H::kModel, (float)m}, {H::kExciter, (float)exc}, {H::kRoom, 0}, {H::kBody, 0}, {H::kOctave, 0},
                              {H::kDecay, 3}, {H::kSwell, 0.1f}});
            render(*p, {on(0.0, 57, 100), off(2.0, 57)}, 3.2, L, R);
            const float e = rmsDb(L, R, 0.05, 0.15), a = rmsDb(L, R, 0.9, 1.1), b = rmsDb(L, R, 1.8, 2.0), t = rmsDb(L, R, 2.9, 3.1);
            const float f = acPitch(L, R, 1.2);
            const char* en = exc == hammerhead::kBow ? "Bow" : "Blow";
            std::printf("      %-6s %-10s %8.1f %8.1f %8.1f %8.1fHz %9.1f %8.1f\n", en, hammerhead::kModelNames[m], e, a, b, f, t, db(peakOf(L, R)));
            const bool ok = finite(L, R) && a > -36.0f && std::fabs(a - b) < 3.0f && e > a - 10.0f &&
                            f > 0.0f && std::fabs(cents(f, 220.0f)) < 15.0f && t < b - 12.0f && peakOf(L, R) < 0.5f;
            check(ok, "%s %s: speaks in 0.1 s (%.1f dB), holds (%.1f -> %.1f dB), plays A3 (%+.1f ct), rings away after release",
                  en, hammerhead::kModelNames[m], e, a, b, f > 0.0f ? cents(f, 220.0f) : 9999.0f);
        }

    std::printf("\n== Swell, pressure, aftertouch, vibrato (1.1) ==\n");
    {
        float early[2], steady[2];
        for (int i = 0; i < 2; ++i)
        {
            auto p = make(0, {{H::kModel, 1}, {H::kExciter, hammerhead::kBow}, {H::kRoom, 0}, {H::kSwell, i ? 1.5f : 0.05f}});
            render(*p, {on(0.0, 57, 100)}, 3.5, L, R);
            early[i] = rmsDb(L, R, 0.1, 0.2); steady[i] = rmsDb(L, R, 3.2, 3.4);
        }
        check(steady[0] - early[0] < 6.0f && steady[1] - early[1] > 12.0f,
              "Swell: at 50 ms a bowed bar is %.1f dB under full after 0.1 s; at 1.5 s it is %.1f dB under", steady[0] - early[0], steady[1] - early[1]);

        float lvl[2];
        for (int i = 0; i < 2; ++i)
        {
            auto p = make(0, {{H::kModel, 6}, {H::kExciter, hammerhead::kBlow}, {H::kRoom, 0}, {H::kPressure, 10}, {H::kAir, 0}});
            mpc::MidiEvent at; at.size = 2; at.data[0] = 0xD0; at.data[1] = (uint8_t)(i ? 127 : 0);
            render(*p, {on(0.0, 57, 100), {0.5, at}}, 2.0, L, R);
            lvl[i] = rmsDb(L, R, 1.5, 1.9);
        }
        check(lvl[1] - lvl[0] > 3.0f, "pad pressure (aftertouch) blows harder: %.1f dB louder at full pressure", lvl[1] - lvl[0]);

        // Vibrato on a blown glass (nearly a sine): the zero-crossing rate wobbles.
        float spread[2];
        for (int i = 0; i < 2; ++i)
        {
            auto p = make(0, {{H::kModel, 6}, {H::kExciter, hammerhead::kBlow}, {H::kRoom, 0}, {H::kAir, 0}, {H::kBody, 0},
                              {H::kVibrato, i ? 100.0f : 0.0f}, {H::kMotorRate, 5}, {H::kOctave, 0}});
            render(*p, {on(0.0, 57, 100)}, 3.0, L, R);
            float lo = 1e9f, hi = 0.0f;
            for (double t = 1.0; t < 2.5; t += 0.02)
            {
                int zc = 0;
                const size_t a = (size_t)(t * SR), b = (size_t)((t + 0.05) * SR);
                for (size_t k = a + 1; k < b; ++k) if ((L[k - 1] + R[k - 1] < 0.0f) != (L[k] + R[k] < 0.0f)) ++zc;
                const float f = (float)zc / 2.0f / 0.05f;
                lo = std::min(lo, f); hi = std::max(hi, f);
            }
            spread[i] = cents(hi, lo);
        }
        check(spread[0] < 40.0f && spread[1] > 2.0f * spread[0] + 20.0f, "Vibrato 100 %%: pitch swings over %.0f cents (%.0f without)", spread[1], spread[0]);

        // Air (1.1.1): the scratch or breath sits round the note, not as a hiss above it, and the knob's
        // lower half is a hint.
        for (int exc = hammerhead::kBow; exc <= hammerhead::kBlow; ++exc)
        {
            float r[3], c[3], hf[3];
            const float airs[3] = {0.0f, 25.0f, 100.0f};
            for (int i = 0; i < 3; ++i)
            {
                auto p = make(0, {{H::kModel, 1}, {H::kExciter, (float)exc}, {H::kRoom, 0}, {H::kBody, 0}, {H::kAir, airs[i]}});
                render(*p, {on(0.0, 57, 100)}, 2.0, L, R);
                noiseToTone(L, R, 220.0f, r[i], c[i], hf[i]);
            }
            const char* en = exc == hammerhead::kBow ? "Bow" : "Blow";
            check(r[2] > r[1] + 8.0f && r[1] < -32.0f && c[2] < 2000.0f && hf[2] < -10.0f,
                  "%s Air: %.1f dB noise-to-tone at 25 %% (%.1f at 0), %.1f dB at 100 %%, centred at %.0f Hz with %.1f dB of it above 3 kHz",
                  en, r[1], r[0], r[2], c[2], hf[2]);
        }
    }

    // ---- 8d. Buzz, LFO, randomness (1.2) ----------------------------------------------------------------
    std::printf("\n== Buzz (1.2) ==\n");
    {
        // High-frequency energy (3..12 kHz) of a window, dB.
        auto hfDb = [&](double t, int len) {
            const auto sp = spectrum(L, R, t, len);
            double e = 1e-30;
            for (int k = (int)(3000 / binHz()); k < (int)(12000 / binHz()); ++k) e += std::pow(10.0, sp[k] / 10.0);
            return (float)(10.0 * std::log10(e));
        };
        float early[2], late[2];
        for (int i = 0; i < 2; ++i)
        {
            auto p = make(3, {{H::kBuzz, i ? 60.0f : 0.0f}, {H::kRoom, 0}, {H::kBody, 0}, {H::kClick, 0}});   // Shell Kalimba, C5
            render(*p, {on(0.0, 60, 110)}, 2.0, L, R);
            early[i] = hfDb(0.01, 4096); late[i] = hfDb(1.2, 8192);
        }
        check(early[1] - early[0] > 8.0f, "Buzz 60 %% on a kalimba: %.1f dB more energy above 3 kHz while the note is loud", early[1] - early[0]);
        check(late[1] - late[0] < 3.0f, "and it lets go as the ring decays: %.1f dB more 1.2 s later", late[1] - late[0]);
        auto p = make(15, {{H::kBuzz, 100}});   // Barnacle Kalimba, buzz all the way up
        std::vector<Ev> evs;
        for (int n : {48, 52, 55, 59, 62, 67}) evs.push_back(on(0.0, n, 127));
        render(*p, evs, 2.0, L, R);
        size_t hot = 0;
        for (size_t i = 0; i < L.size(); ++i) if (std::fabs(L[i]) > 0.7f || std::fabs(R[i]) > 0.7f) ++hot;
        check(finite(L, R) && peakOf(L, R) < 0.98f && hot < L.size() / 200, "Buzz 100 %%, six-note chord at velocity 127: peak %.1f dBFS, %.2f%% in the limiter",
              db(peakOf(L, R)), 100.0 * (double)hot / (double)L.size());
    }

    std::printf("\n== LFO and per-hit randomness (1.2) ==\n");
    {
        // Sync: the LFO follows the song position (1/16 at 120 bpm while playing).
        auto p = make(17, {{H::kRoom, 0}});   // Tide Machine: S&H on 1/16
        render(*p, {}, 1.37, L, R, nullptr, 0, true, 120.0);
        const double ppq = (double)L.size() / SR * 2.0;
        float expect = ppqPhase(ppq, 0.25f), got = p->lfoPhase_;
        float err = std::fabs(got - expect); err = std::min(err, 1.0f - err);
        check(err < 0.12f, "LFO Sync: locked to the song (phase %.2f, song says %.2f)", got, expect);

        // LFO>Pos / LFO>Hard: every hit of a 1/16 roll lands somewhere else on the bar.
        float lo[2] = {1, 1}, hi[2] = {0, 0};
        for (int i = 0; i < 2; ++i)
        {
            auto q = make(17, {{H::kRoom, 0}, {H::kLfoPos, i ? 80.0f : 0.0f}});
            for (int h = 0; h < 16; ++h)
            {
                render(*q, {on(0.0, 60, 100)}, 0.125, L, R, nullptr, 0, true, 120.0);
                for (auto& v : q->voices_) if (v.stage == hammerhead::kOn && v.note == 60) { lo[i] = std::min(lo[i], v.pos); hi[i] = std::max(hi[i], v.pos); }
            }
        }
        check(hi[1] - lo[1] > 0.3f && hi[0] - lo[0] < 1e-4f, "LFO>Pos 80 %% (S&H, 1/16): 16 hits land between %.0f %% and %.0f %% of the way out (all at %.0f %% without)",
              100 * lo[1], 100 * hi[1], 100 * lo[0]);

        // LFO>Stretch: a ringing glass's 2.32x mode moves with the LFO; the fundamental doesn't.
        auto g = make(18, {{H::kRoom, 0}, {H::kLfoShape, hammerhead::kLfoSquare}, {H::kLfoRate, 0.5f}});   // Rip Current
        render(*g, {on(0.0, 57, 100)}, 2.0, L, R);
        const float fa = peakHz(spectrum(L, R, 0.3, 8192), 440 * 2.32f * 0.85f, 440 * 2.32f * 1.15f);
        const float fb = peakHz(spectrum(L, R, 1.3, 8192), 440 * 2.32f * 0.85f, 440 * 2.32f * 1.15f);
        const float f1a = peakHz(spectrum(L, R, 0.3, 8192), 400, 480), f1b = peakHz(spectrum(L, R, 1.3, 8192), 400, 480);
        check(fa / fb > 1.1f && std::fabs(cents(f1a, f1b)) < 5.0f,
              "LFO>Stretch: the glass's 2.32x mode swings %.0f -> %.0f Hz while the note rings; the fundamental stays (%+.1f ct)", fa, fb, cents(f1a, f1b));

        // LFO>Press: a blown note swells and falls with the LFO (slowly: a 3 s glass smooths a fast breath out).
        float swing[2];
        for (int i = 0; i < 2; ++i)
        {
            auto b = make(14, {{H::kRoom, 0}, {H::kLfoRate, 0.4f}, {H::kLfoPress, i ? 100.0f : 0.0f}, {H::kAir, 0}});   // Bottle Breath
            render(*b, {on(0.0, 57, 100)}, 5.0, L, R);
            float mn = 1e9f, mx = -1e9f;
            for (double t = 1.5; t < 4.9; t += 0.05) { const float r = rmsDb(L, R, t, t + 0.05); mn = std::min(mn, r); mx = std::max(mx, r); }
            swing[i] = mx - mn;
        }
        check(swing[1] > 4.0f && swing[0] < 1.5f, "LFO>Press 100 %% at 0.4 Hz on a blown note: the level swings %.1f dB (%.1f without)", swing[1], swing[0]);

        // Rnd Pitch: twelve fresh notes, each detuned on its own, within +-25 cents.
        float dmin[2] = {1e9f, 1e9f}, dmax[2] = {-1e9f, -1e9f};
        for (int i = 0; i < 2; ++i)
        {
            auto q = make(0, {{H::kRoom, 0}, {H::kBody, 0}, {H::kDecay, 0.3f}, {H::kTube, 0}, {H::kRndPitch, i ? 100.0f : 0.0f}});
            for (int h = 0; h < 12; ++h)
            {
                const int note = 57 + 2 * (h % 6);
                render(*q, {on(0.0, note, 100)}, 0.4, L, R);
                const float f0 = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
                const float f = peakHz(spectrum(L, R, 0.0, 8192), f0 * 0.97f, f0 * 1.03f);
                dmin[i] = std::min(dmin[i], cents(f, f0)); dmax[i] = std::max(dmax[i], cents(f, f0));
            }
        }
        check(dmax[1] - dmin[1] > 15.0f && dmax[1] < 27.0f && dmin[1] > -27.0f && dmax[0] - dmin[0] < 2.0f,
              "Rnd Pitch 100 %%: twelve notes land %+.1f .. %+.1f cents off (%.1f cents spread without)", dmin[1], dmax[1], dmax[0] - dmin[0]);

        // Rnd Level: a roll of 12 hits varies in loudness.
        float lspread[2];
        for (int i = 0; i < 2; ++i)
        {
            auto q = make(0, {{H::kRoom, 0}, {H::kDecay, 0.2f}, {H::kTube, 0}, {H::kRndLevel, i ? 100.0f : 0.0f}});
            float mn = 1e9f, mx = -1e9f;
            for (int h = 0; h < 12; ++h)
            {
                render(*q, {on(0.0, 60, 100)}, 0.5, L, R);
                const float pk = db(peakOf(L, R, 0.0, 0.05));
                mn = std::min(mn, pk); mx = std::max(mx, pk);
            }
            lspread[i] = mx - mn;
        }
        check(lspread[1] > 4.0f && lspread[0] < 1.5f, "Rnd Level 100 %%: hits vary over %.1f dB (%.1f dB without)", lspread[1], lspread[0]);
    }

    // ---- 9. Chords: headroom in every program ----------------------------------------------------
    std::printf("\n== Six-note chord at velocity 127, every program ==\n");
    {
        const int chord[6] = {48, 52, 55, 59, 62, 67};
        float loudest = 0.0f;
        for (int prog = 0; prog < numPrograms; ++prog)
        {
            auto p = make(prog);
            std::vector<Ev> evs;
            for (int n : chord) evs.push_back(on(0.0, n, 127));
            render(*p, evs, 3.0, L, R);
            const float pk = peakOf(L, R);
            loudest = std::max(loudest, pk);
            size_t hot = 0;
            for (size_t i = 0; i < L.size(); ++i) if (std::fabs(L[i]) > 0.7f || std::fabs(R[i]) > 0.7f) ++hot;
            check(finite(L, R) && pk < 0.98f && hot < L.size() / 1000, "%s: chord peak %.1f dBFS, %.2f%% of samples in the limiter",
                  p->programName(prog), db(pk), 100.0 * (double)hot / (double)L.size());
        }
        std::printf("      loudest chord: %.1f dBFS\n", db(loudest));
    }

    // ---- 10. CPU ------------------------------------------------------------------------------
    std::printf("\n== CPU (desktop, for orientation; the MPC is roughly 10-20x slower per core) ==\n");
    {
        const int progs[6] = {1, 8, 10, 12, 18, 15};   // vibes, dulcimer; bowed vibes, bowed string (1.1); LFO>Stretch, Buzz (1.2)
        for (int prog : progs)
        {
            auto p = make(prog, {{H::kDecay, 20}, {H::kMute, 0}});
            std::vector<Ev> evs;
            for (int i = 0; i < 16; ++i) evs.push_back(on(0.02 * i, 36 + 3 * i, 100));
            double cpu = 0.0;
            render(*p, evs, 10.0, L, R, &cpu, 128);
            const double pct = 100.0 * cpu / 10.0;
            check(pct < 2.0 && maxVoices == 16, "%s: 16 voices ringing for 10 s = %.2f%% of one core", p->programName(prog), pct);
        }
        auto p = make(0);
        double cpu = 0.0;
        render(*p, {on(0.0, 60, 100)}, 0.2, L, R);
        render(*p, {}, 30.0, L, R, &cpu, 128);
        check(activeVoices(*p) == 0, "idle after the ring dies: %d voices, %.3f%% of one core for the reverb and body", activeVoices(*p), 100.0 * cpu / 30.0);
    }

    // ---- Demo ----------------------------------------------------------------------------------
    if (argc > 1)
    {
        // Each program plays a short phrase and a chord; segments overlap so tails ring into the next.
        const double seg = 4.6;
        const int total = (int)((seg * numPrograms + 4.0) * SR);
        std::vector<float> dl((size_t)total, 0.0f), dr((size_t)total, 0.0f);
        const int scale[] = {0, 2, 4, 7, 9, 12, 14, 16};    // major pentatonic
        for (int prog = 0; prog < numPrograms; ++prog)
        {
            auto p = make(prog);
            const int root = prog == 9 ? 36 : (p->getInt(H::kOctave) > 0 ? 48 : 55);
            std::vector<Ev> evs;
            const double step = 60.0 / 112.0 / 2.0;   // eighths at 112 BPM
            const int pattern[12] = {0, 2, 4, 3, 5, 4, 2, 1, 3, 2, 0, 4};
            const bool sus = sustained(*p);
            for (int i = 0; i < 12; i += sus ? 2 : 1)
            {
                // Bowed and blown programs play half as many notes, legato.
                const int note = root + scale[pattern[i]];
                const int vel = (i % 4 == 0) ? 118 : (i % 2 ? 72 : 92);
                evs.push_back(on(step * i, note, vel));
                evs.push_back(off(step * i + step * (sus ? 2.05 : 0.8), note));
            }
            if (prog == 9)   // Hull Drum: a little groove instead of a chord
            {
                const double s16 = step / 2.0;
                const int hits[8] = {0, 3, 6, 8, 10, 11, 13, 14};
                for (int h : hits) { evs.push_back(on(12 * step + h * s16, h % 3 ? 43 : 36, h % 4 ? 85 : 120)); }
            }
            else
            {
                const int ch[4] = {0, 4, 7, 11};
                for (int k = 0; k < 4; ++k)
                {
                    evs.push_back(on(12 * step + 0.012 * k, root + ch[k], 105));
                    evs.push_back(off(12 * step + 1.4, root + ch[k]));
                }
            }
            render(*p, evs, seg + 3.0, L, R);
            const size_t at = (size_t)(prog * seg * SR);
            for (size_t i = 0; i < L.size() && at + i < dl.size(); ++i) { dl[at + i] += L[i]; dr[at + i] += R[i]; }
        }
        for (size_t i = 0; i < dl.size(); ++i) { dl[i] = hammerhead::limitOut(dl[i]); dr[i] = hammerhead::limitOut(dr[i]); }
        writeWav(argv[1], dl, dr);
        std::printf("\nwrote %s (%.1f s, programs in order: ", argv[1], dl.size() / SR);   // bowed/blown programs last
        for (int prog = 0; prog < numPrograms; ++prog) std::printf("%s%s", probe.programName(prog), prog + 1 < numPrograms ? ", " : ")\n");
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
