// Does Abyss evolve, stay click-free and stay under 0 dBFS? Holds a note and a chord for 30 s per
// program and measures how the spectrum moves, then renders a demo WAV.
//
//   mkdir -p tests/build
//   clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
//   tests/build/listentest build/abyss-demo.wav
#include <vector>
#include <cstdio>
#include <cmath>
#include <complex>
#include <string>
#include <chrono>
#include <algorithm>

#define private public
#define protected public
#include "../src/Abyss.cpp"
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

// ---- rendering ------------------------------------------------------------------------------------
struct Ev { double t; mpc::MidiEvent e; };
static Ev on(double t, int note, int vel = 100) { return {t, mpc::MidiEvent::noteOn(0, note, vel)}; }
static Ev off(double t, int note) { return {t, mpc::MidiEvent::noteOff(0, note)}; }
static Ev cc(double t, int c, int v) { return {t, mpc::MidiEvent::cc(0, c, v)}; }

// A parameter change at a time, applied between blocks.
struct Pc { double t; int param; float plain; };

static void render(Abyss& p, std::vector<Ev> evs, double seconds, std::vector<float>& L, std::vector<float>& R,
                   std::vector<Pc> pcs = {}, double* cpuSec = nullptr, bool playing = false, double bpm = 100.0)
{
    std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    const int total = (int)(seconds * SR);
    L.assign((size_t)total, 0.0f); R.assign((size_t)total, 0.0f);
    unsigned rs = 12345;
    size_t ei = 0, pi = 0;
    std::vector<mpc::MidiEvent> blockEv;
    double cpu = 0.0;
    for (int pos = 0; pos < total;)
    {
        rs = rs * 1664525u + 1013904223u;
        const int block = std::min(32 + (int)((rs >> 8) % 481), total - pos);   // 32..512, like the MPC
        while (pi < pcs.size() && (int)(pcs[pi].t * SR) <= pos) { p.set(pcs[pi].param, pcs[pi].plain); ++pi; }
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
        pos += block;
    }
    if (cpuSec) *cpuSec = cpu;
}

static void loadProgram(Abyss& p, int prog)
{
    const mpc::Program& pr = p.programs_[(size_t)prog];
    for (int i = 0; i < p.numParams(); ++i) p.set(i, pr.plainValues[(size_t)i]);
}

static std::unique_ptr<Abyss> make(int prog)
{
    auto p = std::make_unique<Abyss>();
    p->prepare(SR, 512);
    loadProgram(*p, prog);
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

// Spectral shape of the mono mix around time t: third-octave band levels in dB relative to the
// window's total energy (so a level change alone does not count as a change of sound), plus the
// spectral centroid. Averaged over 4 frames of 8192 samples.
struct Shape { std::vector<float> bandDb; float centroid; float rmsDb; };
static Shape shapeAt(const std::vector<float>& L, const std::vector<float>& R, double t)
{
    const int N = 8192, frames = 4;
    std::vector<double> power(N / 2, 0.0);
    double sumSq = 0.0; int cnt = 0;
    for (int f = 0; f < frames; ++f)
    {
        const long start = (long)(t * SR) + (long)f * N / 2;
        std::vector<std::complex<float>> a(N);
        for (int i = 0; i < N; ++i)
        {
            const long s = start + i;
            const float x = (s >= 0 && s < (long)L.size()) ? 0.5f * (L[(size_t)s] + R[(size_t)s]) : 0.0f;
            sumSq += (double)x * x; ++cnt;
            const float w = 0.5f - 0.5f * std::cos(6.2831853f * (float)i / (float)N);
            a[i] = std::complex<float>(x * w, 0.0f);
        }
        fft(a);
        for (int k = 0; k < N / 2; ++k) power[k] += std::norm(a[k]);
    }
    Shape s;
    double tot = 1e-30, cen = 0.0;
    for (int k = 1; k < N / 2; ++k) { tot += power[k]; cen += power[k] * k * SR / N; }
    s.centroid = (float)(cen / tot);
    s.rmsDb = (float)(10.0 * std::log10(sumSq / cnt + 1e-30));
    for (double fc = 31.5; fc < 16000.0; fc *= 1.259921)
    {
        const int k0 = std::max(1, (int)(fc / 1.122462 * N / SR)), k1 = std::max(k0 + 1, (int)(fc * 1.122462 * N / SR));
        double e = 0.0;
        for (int k = k0; k < k1 && k < N / 2; ++k) e += power[k];
        s.bandDb.push_back((float)(10.0 * std::log10(e / tot + 1e-12)));
    }
    return s;
}

// Mean absolute difference (dB) between two spectral shapes, over the bands within 50 dB of the
// loudest band of either.
static float highBandDb(const Shape& s)
{
    double e = 1e-12; double fc = 31.5;
    for (float b : s.bandDb) { if (fc >= 1000.0) e += std::pow(10.0, b / 10.0); fc *= 1.259921; }
    return (float)(10.0 * std::log10(e));
}

static float shapeDistance(const Shape& a, const Shape& b)
{
    float top = -1e9f;
    for (size_t i = 0; i < a.bandDb.size(); ++i) top = std::max(top, std::max(a.bandDb[i], b.bandDb[i]));
    float sum = 0.0f; int n = 0;
    for (size_t i = 0; i < a.bandDb.size(); ++i)
    {
        if (std::max(a.bandDb[i], b.bandDb[i]) < top - 50.0f) continue;
        const float x = std::max(a.bandDb[i], top - 50.0f), y = std::max(b.bandDb[i], top - 50.0f);
        sum += std::fabs(x - y); ++n;
    }
    return n ? sum / (float)n : 0.0f;
}

// Magnitude spectrum (dB) of the mono mix around time t, 16384 points (2.7 Hz bins), Hann window.
static std::vector<float> spectrumAt(const std::vector<float>& L, const std::vector<float>& R, double t, int channel = -1)
{
    const int N = 16384;
    std::vector<std::complex<float>> a(N);
    const long start = (long)(t * SR);
    for (int i = 0; i < N; ++i)
    {
        const long s = start + i;
        float x = 0.0f;
        if (s >= 0 && s < (long)L.size()) x = channel == 0 ? L[(size_t)s] : channel == 1 ? R[(size_t)s] : 0.5f * (L[(size_t)s] + R[(size_t)s]);
        a[i] = std::complex<float>(x * (0.5f - 0.5f * std::cos(6.2831853f * (float)i / (float)N)), 0.0f);
    }
    fft(a);
    std::vector<float> m(N / 2);
    for (int k = 0; k < N / 2; ++k) m[k] = 10.0f * std::log10(std::norm(a[k]) + 1e-20f);
    return m;
}
static float binHz(int k) { return (float)(k * SR / 16384.0); }
// Frequency of the strongest bin between lo and hi Hz.
static float peakHz(const std::vector<float>& m, float lo, float hi)
{
    int best = -1;
    for (int k = (int)(lo / binHz(1)); k <= (int)(hi / binHz(1)) && k < (int)m.size(); ++k) if (best < 0 || m[k] > m[best]) best = k;
    return binHz(best);
}
// Level (dB) of the strongest bin within +-6 Hz of f.
static float levelAt(const std::vector<float>& m, float f) { const float pk = peakHz(m, f - 6.0f, f + 6.0f); return m[(size_t)std::lround(pk / binHz(1))]; }

static float peakOf(const std::vector<float>& L, const std::vector<float>& R)
{
    float p = 0.0f;
    for (size_t i = 0; i < L.size(); ++i) p = std::max(p, std::max(std::fabs(L[i]), std::fabs(R[i])));
    return p;
}
static bool finite(const std::vector<float>& L, const std::vector<float>& R)
{
    for (size_t i = 0; i < L.size(); ++i) if (!std::isfinite(L[i]) || !std::isfinite(R[i])) return false;
    return true;
}
static float db(float x) { return 20.0f * std::log10(std::max(x, 1e-10f)); }

// Clicks: a click is a jump in value or slope, which shows as a spike in the second difference.
// Compare each 2 ms window's largest second difference against the median of its neighbours over
// +-50 ms; in this pad nothing natural goes above about 2.3x, so 3x counts as a click. Returns the number of clicks and
// the time of the worst one.
static int countClicks(const std::vector<float>& x, double& worstT, float& worstRatio)
{
    const int W = (int)(0.002 * SR);
    const int nw = (int)x.size() / W;
    std::vector<float> mx((size_t)nw, 0.0f);
    for (int w = 0; w < nw; ++w)
        for (int i = std::max(2, w * W); i < (w + 1) * W; ++i)
            mx[(size_t)w] = std::max(mx[(size_t)w], std::fabs(x[(size_t)i] - 2.0f * x[(size_t)i - 1] + x[(size_t)i - 2]));
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
        if (mx[(size_t)w] > 2e-4f && ratio > worstRatio) { worstRatio = ratio; worstT = (double)w * W / SR; }
        if (mx[(size_t)w] > 2e-4f && ratio > 3.0f) ++clicks;
    }
    return clicks;
}
static void checkClicks(const std::vector<float>& L, const std::vector<float>& R, const char* what)
{
    double tl, tr; float rl, rr;
    const int cl = countClicks(L, tl, rl), cr = countClicks(R, tr, rr);
    check(cl + cr == 0, "%s: no clicks (worst jump %.1fx its surroundings at %.2f s)", what,
          rl > rr ? rl : rr, rl > rr ? tl : tr);
}

int main(int argc, char** argv)
{
    Abyss probe; probe.prepare(SR, 512);
    const int numPrograms = (int)probe.programs_.size();
    std::vector<float> L, R;

    // ---- 1. One held note, default program: does it open up and keep moving? -------------------
    std::printf("\n== One note (C2), Abyssal Plain, held 30 s ==\n");
    {
        auto p = make(0);
        render(*p, {on(0.0, 36), off(30.0, 36)}, 42.0, L, R);
        std::vector<Shape> sh;
        for (int s = 0; s <= 40; ++s) sh.push_back(shapeAt(L, R, s));
        std::printf("      t(s):      ");
        for (int s = 2; s <= 30; s += 4) std::printf("%7d", s);
        std::printf("\n      centroid:  ");
        for (int s = 2; s <= 30; s += 4) std::printf("%7.0f", sh[(size_t)s].centroid);
        std::printf("\n      rms dB:    ");
        for (int s = 2; s <= 30; s += 4) std::printf("%7.1f", sh[(size_t)s].rmsDb);
        std::printf("\n      change dB: ");
        for (int s = 2; s <= 30; s += 4) std::printf("%7.2f", shapeDistance(sh[(size_t)s], sh[(size_t)(s + 4 <= 40 ? s + 4 : s)]));
        std::printf("   (shape change to 4 s later)\n");
        check(sh[24].centroid > 1.6f * sh[2].centroid, "it opens up: centroid %.0f Hz at 2 s -> %.0f Hz at 24 s",
              sh[2].centroid, sh[24].centroid);
        check(shapeDistance(sh[2], sh[24]) > 4.0f, "the spectrum at 24 s differs from 2 s by %.1f dB per band",
              shapeDistance(sh[2], sh[24]));
        float minLate = 1e9f, maxLate = 0.0f;
        for (int s = 14; s <= 26; ++s)
        {
            const float d = shapeDistance(sh[(size_t)s], sh[(size_t)s + 4]);
            minLate = std::min(minLate, d); maxLate = std::max(maxLate, d);
        }
        check(minLate > 0.7f, "after it has opened it keeps moving: every 4 s the shape changes by %.1f..%.1f dB per band",
              minLate, maxLate);
        const float pk = peakOf(L, R);
        check(pk < 0.97f && finite(L, R), "peak %.2f dBFS, finite", db(pk));
        checkClicks(L, R, "single note, onset to tail");
        check(sh[40].rmsDb < sh[28].rmsDb - 25.0f, "it dies away after release (%.1f dB held -> %.1f dB 10 s after)",
              sh[28].rmsDb, sh[40].rmsDb);
    }

    // ---- 2. A six-voice chord in every program --------------------------------------------------
    std::printf("\n== Six-note chord at velocity 127, held 30 s, every program ==\n");
    const int chord[6] = {36, 48, 52, 55, 59, 62};
    float loudest = 0.0f;
    for (int prog = 0; prog < numPrograms; ++prog)
    {
        auto p = make(prog);
        std::vector<Ev> evs;
        for (int n : chord) { evs.push_back(on(0.0, n, 127)); evs.push_back(off(30.0, n)); }
        render(*p, evs, 34.0, L, R, {}, nullptr, true, 96.0);
        const Shape a = shapeAt(L, R, 3.0), b = shapeAt(L, R, 22.0), c = shapeAt(L, R, 26.0);
        const float pk = peakOf(L, R);
        loudest = std::max(loudest, pk);
        const char* name = p->programs_[(size_t)prog].name.c_str();
        std::printf("      %-16s peak %6.2f dBFS  rms@22s %6.1f dB  centroid %5.0f -> %5.0f Hz  change 3->22 s %.1f dB, 22->26 s %.1f dB\n",
                    name, db(pk), b.rmsDb, a.centroid, b.centroid, shapeDistance(a, b), shapeDistance(b, c));
        char what[96];
        std::snprintf(what, sizeof(what), "%s: under 0 dBFS and finite", name);
        check(pk < 0.97f && finite(L, R), "%s", what);
        std::snprintf(what, sizeof(what), "%s: the spectrum evolves", name);
        check(shapeDistance(a, b) > 2.0f && shapeDistance(b, c) > 0.4f, "%s", what);
        std::snprintf(what, sizeof(what), "%s chord", name);
        checkClicks(L, R, what);
    }

    // ---- 3. Voice stealing, short notes, retriggers, pedal, bend, knob moves ---------------------
    std::printf("\n== Abuse: stealing, staccato, retrigger, sustain pedal, bend, fast knob moves ==\n");
    {
        auto p = make(6);   // Pressure Wave: the fastest attack in the programs
        p->set(Abyss::kAttack, 0.05f);
        p->set(Abyss::kRelease, 0.4f);
        std::vector<Ev> evs;
        for (int i = 0; i < 6; ++i) evs.push_back(on(0.1 * i, 48 + 3 * i, 110));          // fill all voices
        for (int i = 0; i < 12; ++i) { evs.push_back(on(2.0 + 0.15 * i, 60 + i, 90)); evs.push_back(off(2.06 + 0.15 * i, 60 + i)); }  // steal, staccato
        for (int i = 0; i < 6; ++i) evs.push_back(off(5.0, 48 + 3 * i));
        for (int i = 0; i < 5; ++i) evs.push_back(on(6.0 + 0.3 * i, 50, 100));            // retrigger one note
        evs.push_back(off(8.0, 50));
        evs.push_back(cc(9.0, 64, 127));
        for (int i = 0; i < 4; ++i) { evs.push_back(on(9.1 + 0.1 * i, 55 + 4 * i)); evs.push_back(off(9.4 + 0.1 * i, 55 + 4 * i)); }
        evs.push_back(cc(12.0, 64, 0));
        for (int i = 0; i <= 40; ++i)
        {
            const int b = 8192 + (int)(8191.0 * std::sin(i * 0.3));
            mpc::MidiEvent e; e.size = 3; e.data[0] = 0xE0; e.data[1] = (uint8_t)(b & 127); e.data[2] = (uint8_t)(b >> 7);
            evs.push_back({13.0 + 0.05 * i, e});
        }
        evs.push_back(on(13.0, 43)); evs.push_back(off(16.0, 43));
        std::vector<Pc> pcs;
        for (int i = 0; i < 30; ++i)
        {
            const double t = 13.0 + 0.1 * i;
            pcs.push_back({t, Abyss::kVolume, (i & 1) ? 6.0f : -20.0f});
            pcs.push_back({t, Abyss::kBright, (i & 1) ? 100.0f : 0.0f});
            pcs.push_back({t, Abyss::kSpace, (i & 1) ? 100.0f : 0.0f});
            pcs.push_back({t, Abyss::kWidth, (i & 1) ? 100.0f : 0.0f});
            pcs.push_back({t, Abyss::kSub, (i & 1) ? 100.0f : 0.0f});
        }
        render(*p, evs, 20.0, L, R, pcs);
        const float pk = peakOf(L, R);
        check(pk < 0.97f && finite(L, R), "abuse pass: peak %.2f dBFS, finite", db(pk));
        checkClicks(L, R, "abuse pass");
        int sounding = 0;
        for (auto& v : p->voices_) sounding += v.stage != 0;
        check(sounding == 0, "every voice finished after the abuse pass (%d still sounding)", sounding);
    }

    // ---- 4. Program change while a chord is held ------------------------------------------------
    {
        auto p = make(0);
        std::vector<Ev> evs;
        for (int n : chord) { evs.push_back(on(0.0, n, 100)); evs.push_back(off(14.0, n)); }
        std::vector<Pc> pcs;
        for (int prog = 1; prog < numPrograms; ++prog)
        {
            const auto& pv = p->programs_[(size_t)prog].plainValues;
            for (int i = 0; i < p->numParams(); ++i)
                if (i != Abyss::kOctave) pcs.push_back({4.0 + 1.2 * prog, i, pv[(size_t)i]});   // octave jumps are pitch jumps, by design
        }
        render(*p, evs, 18.0, L, R, pcs);
        checkClicks(L, R, "program changes under a held chord");
        check(peakOf(L, R) < 0.97f && finite(L, R), "program changes: peak %.2f dBFS, finite", db(peakOf(L, R)));
    }

    // ---- 4b. 1.1 features ---------------------------------------------------------------------
    std::printf("\n== 1.1: orbit, stretch, hollow, interval, sink, motion shapes ==\n");
    // A plain, steady A2 so single partials can be found: everything that moves or blurs is off.
    auto plain = [](float bright = 100.0f)
    {
        auto p = make(0);
        for (auto pv : std::vector<std::pair<int, float>>{{Abyss::kBright, bright}, {Abyss::kBloom, 0}, {Abyss::kDrift, 0},
                                                         {Abyss::kSub, 0}, {Abyss::kFifth, 0}, {Abyss::kShimmer, 0},
                                                         {Abyss::kAir, 0}, {Abyss::kSpace, 0}, {Abyss::kDetune, 0}, {Abyss::kMotion, 0},
                                                         {Abyss::kEvolve, 1}, {Abyss::kAttack, 0.1f}})
            p->set(pv.first, pv.second);
        return p;
    };
    const float f0 = 110.0f;
    {
        float got[3];
        const float stretch[3] = {0.0f, 60.0f, -60.0f};
        for (int i = 0; i < 3; ++i)
        {
            auto p = plain();
            p->set(Abyss::kStretch, stretch[i]);
            render(*p, {on(0.0, 45, 100)}, 6.0, L, R);
            got[i] = peakHz(spectrumAt(L, R, 4.0), 1.6f * f0, 2.9f * f0);
        }
        const float want[3] = {2.0f * f0, f0 * std::pow(2.0f, 1.21f), f0 * std::pow(2.0f, 0.79f)};
        check(std::fabs(got[0] - want[0]) < 6 && std::fabs(got[1] - want[1]) < 6 && std::fabs(got[2] - want[2]) < 6,
              "Stretch moves the 2nd partial: %.0f Hz at 0 %%, %.0f Hz at +60 %%, %.0f Hz at -60 %% (expected %.0f, %.0f, %.0f)",
              got[0], got[1], got[2], want[0], want[1], want[2]);
    }
    {
        float even[2];
        for (int i = 0; i < 2; ++i)
        {
            auto p = plain();
            p->set(Abyss::kHollow, i ? 100.0f : 0.0f);
            render(*p, {on(0.0, 45, 100)}, 6.0, L, R);
            const auto m = spectrumAt(L, R, 4.0);
            even[i] = levelAt(m, 2.0f * f0) - levelAt(m, f0);
        }
        check(even[1] < even[0] - 15.0f, "Hollow takes out the even harmonics: 2nd harmonic %.1f dB -> %.1f dB re the fundamental",
              even[0], even[1]);
    }
    {
        struct { int interval; float ratio; const char* name; } cases[3] = {{3, 1.25f, "Maj 3rd"}, {5, 1.8f, "Min 7th"}, {7, 2.25f, "9th"}};
        for (auto& c : cases)
        {
            float lvl[2];
            for (int i = 0; i < 2; ++i)
            {
                auto p = plain();
                p->set(Abyss::kInterval, (float)c.interval);
                p->set(Abyss::kFifth, i ? 100.0f : 0.0f);
                render(*p, {on(0.0, 45, 100)}, 6.0, L, R);
                const auto m = spectrumAt(L, R, 4.0);
                lvl[i] = levelAt(m, c.ratio * f0) - levelAt(m, f0);
            }
            check(lvl[1] > -20.0f && lvl[1] > lvl[0] + 25.0f, "Interval %s plays %.1f Hz: %.1f dB re the fundamental at Int Level 100 %%, %.1f dB at 0 %%",
                  c.name, c.ratio * f0, lvl[1], lvl[0]);
        }
    }
    {
        // Orbit: the 3rd harmonic (on the A half) travels from side to side once per orbit (8 s); without Orbit
        // it stays on its side.
        float range[2]; int crossings = 0;
        const float h3 = 3.0f * 130.8128f;
        for (int i = 0; i < 2; ++i)
        {
            auto p = plain(40.0f);
            p->set(Abyss::kCycle, 8); p->set(Abyss::kOrbitSpeed, 1);
            p->set(Abyss::kOrbit, i ? 100.0f : 0.0f);
            render(*p, {on(0.0, 48, 100)}, 32.0, L, R);
            float lo = 1e9f, hi = -1e9f, prev = 0.0f;
            for (double t = 6.0; t < 31.0; t += 0.5)
            {
                const float bal = levelAt(spectrumAt(L, R, t, 0), h3) - levelAt(spectrumAt(L, R, t, 1), h3);
                lo = std::min(lo, bal); hi = std::max(hi, bal);
                if (i == 1 && t > 6.0 && (bal > 0.0f) != (prev > 0.0f)) ++crossings;
                prev = bal;
            }
            range[i] = hi - lo;
        }
        check(range[1] > 20.0f && range[0] < 4.0f && crossings >= 4 && crossings <= 9,
              "Orbit carries a partial round the stereo field: 3rd harmonic swings %.1f dB L/R (%d crossings in 25 s, 3 orbits), %.1f dB without",
              range[1], crossings, range[0]);
    }
    {
        // The Sub knob sets the sub (1.0 had a bug where it did nothing).
        float sub[2];
        for (int i = 0; i < 2; ++i)
        {
            auto p = plain();
            p->set(Abyss::kSub, i ? 100.0f : 0.0f);
            render(*p, {on(0.0, 45, 100)}, 6.0, L, R);
            const auto m = spectrumAt(L, R, 4.0);
            sub[i] = levelAt(m, 0.5f * f0) - levelAt(m, f0);
        }
        check(sub[1] > -12.0f && sub[1] > sub[0] + 30.0f, "Sub sets the sub: %.1f dB re the fundamental at 100 %%, %.1f dB at 0 %%", sub[1], sub[0]);
    }
    {
        // Sink: starts bright, ends dark.
        auto p = make(11);
        render(*p, {on(0.0, 48, 100)}, 42.0, L, R);
        const float early = highBandDb(shapeAt(L, R, 3.0)), late = highBandDb(shapeAt(L, R, 38.0));
        check(early > late + 8.0f, "Sinking gets darker: energy above 1 kHz %.1f dB at 3 s -> %.1f dB at 38 s", early, late);
    }
    {
        // Every motion shape keeps the chord moving, without clicks.
        const char* names[4] = {"Sine", "Drift", "Swell", "Tide"};
        for (int sh = 0; sh < 4; ++sh)
        {
            auto p = make(0);
            p->set(Abyss::kMotion, 100); p->set(Abyss::kMotionShape, (float)sh); p->set(Abyss::kCycle, 6); p->set(Abyss::kEvolve, 3);
            std::vector<Ev> evs;
            for (int n : {48, 55, 60}) evs.push_back(on(0.0, n, 100));
            render(*p, evs, 20.0, L, R);
            float minD = 1e9f;
            for (int t = 8; t <= 14; t += 2) minD = std::min(minD, shapeDistance(shapeAt(L, R, t), shapeAt(L, R, t + 2)));
            check(minD > 0.5f, "Motion Shape %s moves the sound: at least %.1f dB per band every 2 s", names[sh], minD);
            char what[64]; std::snprintf(what, sizeof(what), "Motion Shape %s", names[sh]);
            checkClicks(L, R, what);
        }
    }
    {
        // A 1.0 project: its state has only the first 22 parameters. Loaded into a fresh instance, the 1.1
        // parameters must stay at their defaults (which sound like 1.0).
        Abyss src; src.prepare(SR, 512); loadProgram(src, 2);
        const std::vector<uint8_t> full = src.internalSaveState();
        std::vector<uint8_t> v10(full.begin(), full.begin() + 12);
        size_t pos = 12;
        for (int i = 0; i < 22; ++i) { const size_t len = 1 + full[pos] + 8; v10.insert(v10.end(), full.begin() + (long)pos, full.begin() + (long)(pos + len)); pos += len; }
        const uint32_t n = 22; std::memcpy(&v10[8], &n, 4);
        for (int i = 0; i < 4; ++i) v10.push_back(0);   // no extra state
        Abyss dst; dst.prepare(SR, 512);
        const bool ok = dst.internalLoadState(v10.data(), v10.size());
        bool same = ok, defaults = true;
        for (int i = 0; i < 22; ++i) same = same && std::fabs(dst.get(i) - src.get(i)) < 1e-4f;
        for (int i = 22; i < dst.numParams(); ++i) defaults = defaults && dst.get(i) == dst.param(i).def;
        check(same && defaults, "a 1.0 project loads: its 22 values restored, the %d new parameters at their defaults", dst.numParams() - 22);
    }

    // ---- 5. CPU, roughly: the worst case (6 voices, every layer on) ----------------------------
    {
        auto p = make(0);
        p->set(Abyss::kBright, 100); p->set(Abyss::kSub, 100); p->set(Abyss::kFifth, 100);
        p->set(Abyss::kShimmer, 100); p->set(Abyss::kAir, 100); p->set(Abyss::kDetune, 20);
        p->set(Abyss::kOrbit, 100); p->set(Abyss::kStretch, 30); p->set(Abyss::kMotionShape, 3);
        p->set(Abyss::kEvolve, 1); p->set(Abyss::kAttack, 0.1f); p->set(Abyss::kOctave, -1);
        std::vector<Ev> evs;
        for (int n : chord) evs.push_back(on(0.0, n, 127));
        double cpu = 0.0;
        render(*p, evs, 20.0, L, R, {}, &cpu);
        std::printf("\nINFO  CPU (desktop, worst case: 6 voices x 24 partials, all layers, orbit): %.2f%% of real time\n", 100.0 * cpu / 20.0);
        auto q = make(0);
        evs.clear();
        for (int n : {48, 55, 60}) evs.push_back(on(0.0, n, 100));
        render(*q, evs, 20.0, L, R, {}, &cpu);
        std::printf("INFO  CPU (desktop, default program, 3-note chord): %.2f%% of real time\n", 100.0 * cpu / 20.0);
        check(peakOf(L, R) < 0.97f, "worst-case layers: peak %.2f dBFS", db(peakOf(L, R)));
    }

    // ---- 5b. Reverb balance: a held chord fully dry vs fully wet --------------------------------
    {
        float rms[2];
        for (int w = 0; w < 2; ++w)
        {
            auto p = make(0);
            p->set(Abyss::kSpace, w ? 100.0f : 0.0f);
            std::vector<Ev> evs;
            for (int n : {48, 55, 60}) evs.push_back(on(0.0, n, 100));
            render(*p, evs, 24.0, L, R);
            rms[w] = shapeAt(L, R, 20.0).rmsDb;
        }
        std::printf("INFO  held chord: Space 0 %% %.1f dB rms, Space 100 %% %.1f dB rms\n", rms[0], rms[1]);
        check(std::fabs(rms[1] - rms[0]) < 4.0f, "fully wet is within 4 dB of fully dry (%.1f dB)", rms[1] - rms[0]);
    }

    // ---- 6. Demo ---------------------------------------------------------------------------------
    {
        // A C2 drone opens up on its own; a Cm9 chord joins at 14 s; everything lets go at 40 s.
        auto p = make(0);
        std::vector<Ev> evs = {on(0.0, 36, 100)};
        for (int n : {48, 51, 55, 58, 62}) { evs.push_back(on(14.0, n, 90)); evs.push_back(off(40.0, n)); }
        evs.push_back(off(40.0, 36));
        render(*p, evs, 52.0, L, R);
        const char* path = argc > 1 ? argv[1] : "build/abyss-demo.wav";
        writeWav(path, L, R);
        std::printf("INFO  demo: %s (52 s, peak %.2f dBFS)\n", path, db(peakOf(L, R)));
        check(peakOf(L, R) < 0.97f && finite(L, R), "demo under 0 dBFS");
        checkClicks(L, R, "demo");
        // The click detector must notice small steps in this same audio: 0.01 (-40 dBFS) in the lone
        // drone at 6 s, 0.03 (-30 dBFS) in the full chord at 24 s, where air noise and high partials
        // hide anything smaller (from the ear too), and 0.002 (-54 dBFS) in the release tail at 44 s.
        const double at[3] = {6.0, 24.0, 44.0};
        const float size[3] = {0.01f, 0.03f, 0.002f};
        for (int k = 0; k < 3; ++k)
        {
            std::vector<float> bad = L;
            for (size_t i = (size_t)(at[k] * SR); i < bad.size(); ++i) bad[i] += size[k];
            double t; float ratio;
            const int n = countClicks(bad, t, ratio);
            check(n > 0 && std::fabs(t - at[k]) < 0.01, "click detector self-test: a %.3f step at %.0f s is caught (%.1fx at %.2f s)",
                  size[k], at[k], ratio, t);
        }
    }

    // ---- 7. Orbit demo -----------------------------------------------------------------------------
    {
        // Siphonophore: a held Ebmaj9 with Orbit at 100 %, the halves of every note circling each other.
        auto p = make(9);
        std::vector<Ev> evs;
        for (int n : {39, 51, 55, 58, 62, 65}) { evs.push_back(on(0.0, n, 95)); evs.push_back(off(36.0, n)); }
        render(*p, evs, 46.0, L, R);
        std::string path = argc > 1 ? std::string(argv[1]) : std::string("build/abyss-demo.wav");
        const size_t dot = path.rfind(".wav");
        path = (dot == std::string::npos ? path : path.substr(0, dot)) + "-orbit.wav";
        writeWav(path.c_str(), L, R);
        std::printf("INFO  orbit demo: %s (46 s, Siphonophore, peak %.2f dBFS)\n", path.c_str(), db(peakOf(L, R)));
        check(peakOf(L, R) < 0.97f && finite(L, R), "orbit demo under 0 dBFS");
        checkClicks(L, R, "orbit demo");
    }

    std::printf("\n%s  (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
