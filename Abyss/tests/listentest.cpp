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

    // ---- 5. CPU, roughly: the worst case (6 voices, every layer on) ----------------------------
    {
        auto p = make(0);
        p->set(Abyss::kBright, 100); p->set(Abyss::kSub, 100); p->set(Abyss::kFifth, 100);
        p->set(Abyss::kShimmer, 100); p->set(Abyss::kAir, 100); p->set(Abyss::kDetune, 20);
        p->set(Abyss::kEvolve, 1); p->set(Abyss::kAttack, 0.1f); p->set(Abyss::kOctave, -1);
        std::vector<Ev> evs;
        for (int n : chord) evs.push_back(on(0.0, n, 127));
        double cpu = 0.0;
        render(*p, evs, 20.0, L, R, {}, &cpu);
        std::printf("\nINFO  CPU (desktop, worst case: 6 voices x 24 partials, all layers): %.2f%% of real time\n", 100.0 * cpu / 20.0);
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

    std::printf("\n%s  (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
