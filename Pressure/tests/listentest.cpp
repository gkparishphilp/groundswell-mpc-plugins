// Pressure's render test: does Depth squeeze harder and stay loudness-matched, do the Hulls behave
// differently, does the synced release follow the tempo, do Blend, the sidechain high-pass, Bloom,
// Implode and Growl do what they say, is it clean (no NaN, no denormals) and cheap?
// Also renders a demo WAV.
//
//   mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework -o tests/build/listentest tests/listentest.cpp
//   tests/build/listentest [demo.wav]          (run from plugins/Pressure)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#define private public
#define protected public
#include "../src/Pressure.cpp"
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
// Test material
// ---------------------------------------------------------------------------------------------
static unsigned g_rs = 12345;
static float rnd() { g_rs = g_rs * 1664525u + 1013904223u; return (float)(g_rs >> 8) / 16777216.0f * 2.0f - 1.0f; }

static Buf drums(double bpm, int bars, float kickLevel = 1.0f)
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
                    o[(size_t)(t0 + i)] += kickLevel * (float)(0.85 * std::sin(6.2831853 * ph) * std::exp(-t * 7.0));
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
            // hats on every 16th, accented on 8ths
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
            const double hz = 440.0 * std::pow(2.0, (notes[k] - 69) / 12.0);
            double ph = 0.0;
            for (long long i = 0; i < len && t0 + i < total; ++i)
            {
                ph += hz / SR; ph -= std::floor(ph);
                const float saw = (float)(2.0 * ph - 1.0);
                const float env = (float)(std::exp(-(double)i / SR * 6.0) * std::fmin(1.0, (len - i) / (0.005 * SR)));
                const float cut = 0.05f + 0.25f * env;
                lp += cut * (saw - lp);
                o[(size_t)(t0 + i)] += lp * env * ((k % 4 == 0) ? 0.55f : 0.35f);
            }
        }
    return o;
}

static Buf pad(double bpm, int bars)
{
    const double spb = SR * 60.0 / bpm;
    Buf o((size_t)(bars * 4 * spb), 0.0f);
    const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 55, 59}};
    for (int b = 0; b < bars; ++b)
        for (int v = 0; v < 3; ++v)
        {
            const double hz = 440.0 * std::pow(2.0, (chords[b % 4][v] - 69) / 12.0);
            double ph = 0.0;
            const long long t0 = (long long)(b * 4 * spb), len = (long long)(4 * spb);
            for (long long i = 0; i < len && t0 + i < (long long)o.size(); ++i)
            {
                ph += hz / SR; ph -= std::floor(ph);
                const float env = (float)std::fmin(1.0, std::fmin(i / (0.08 * SR), (len - i) / (0.05 * SR)));
                o[(size_t)(t0 + i)] += (float)(std::sin(6.2831853 * ph) + 0.3 * std::sin(12.566 * ph)) * 0.05f * env;
            }
        }
    return o;
}

static Buf mixOf(const Buf& a, const Buf& b, const Buf& c, float ga, float gb, float gc)
{
    Buf o(std::min(a.size(), std::min(b.size(), c.size())));
    for (size_t i = 0; i < o.size(); ++i) o[i] = ga * a[i] + gb * b[i] + gc * c[i];
    return o;
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------
struct Render
{
    Buf l, r;
    std::vector<float> gr;      // max(gr_[0], gr_[1]) after every block
    int block = 64;
};

static void render(Pressure& p, const Buf& inL, const Buf& inR, double bpm, int block, Render& out,
                   bool playing = true, std::vector<float>* effGr = nullptr)
{
    const size_t total = inL.size();
    out.l.assign(total, 0.0f); out.r.assign(total, 0.0f); out.gr.clear(); out.block = block;
    Buf l((size_t)block), r((size_t)block);
    const double spb = SR * 60.0 / bpm;
    mpc::ScopedDenormalsOff ftz;
    for (size_t pos = 0; pos < total; pos += (size_t)block)
    {
        const int n = (int)std::min((size_t)block, total - pos);
        for (int i = 0; i < n; ++i) { l[(size_t)i] = inL[pos + (size_t)i]; r[(size_t)i] = inR[pos + (size_t)i]; }
        const float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {l.data(), r.data()};
        mpc::ProcessContext ctx;
        ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = n;
        ctx.sampleRate = SR;
        ctx.transport.valid = ctx.transport.tempoValid = true;
        ctx.transport.playing = playing; ctx.transport.ppqValid = playing; ctx.transport.bpm = bpm;
        ctx.transport.ppqPos = (double)pos / spb;
        p.process(ctx);
        for (int i = 0; i < n; ++i) { out.l[pos + (size_t)i] = l[(size_t)i]; out.r[pos + (size_t)i] = r[(size_t)i]; }
        out.gr.push_back(std::max(p.gr_[0], p.gr_[1]));
        if (effGr)
        {
            float m = 1e9f;
            for (int c = 0; c < 2; ++c) m = std::min(m, p.gr_[c] - p.bloomAmt_ * std::max(0.0f, p.bloomF_[c] - p.gr_[c]));
            effGr->push_back(m);
        }
    }
}

struct Settings
{
    float depth = 50, growl = 35, attack = 0, release = 0.5f, bloom = 0, blend = 100, sc = 40, link = 100, breath = 0, output = 0;
    int hull = kBus, colour = kTape, tide = kTideAuto, stereo = 0;
    bool implode = false;
};

static void apply(Pressure& p, const Settings& s)
{
    p.set(kOutput, s.output); p.set(kDepth, s.depth); p.set(kHull, (float)s.hull); p.set(kImplode, s.implode ? 1.0f : 0.0f);
    p.set(kColour, (float)s.colour); p.set(kGrowl, s.growl); p.set(kAttack, s.attack); p.set(kTide, (float)s.tide);
    p.set(kRelease, s.release); p.set(kBloom, s.bloom); p.set(kBlend, s.blend); p.set(kScHpf, s.sc);
    p.set(kLink, s.link); p.set(kStereo, (float)s.stereo); p.set(kBreath, s.breath);
}

static void run(const Settings& s, const Buf& in, double bpm, Render& out, int block = 64, std::vector<float>* effGr = nullptr)
{
    Pressure p;
    p.prepare(SR, 512);
    apply(p, s);
    p.reset();
    render(p, in, in, bpm, block, out, true, effGr);
}

// ---------------------------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------------------------
// RMS in dB with a gentle loudness weighting (a 100 Hz one-pole high-pass), from sample a to b.
static double loudDb(const Buf& x, size_t a, size_t b)
{
    const double r = std::exp(-6.2831853 * 100.0 / SR);
    double x1 = 0, y1 = 0, sum = 0;
    for (size_t i = 0; i < b && i < x.size(); ++i)
    {
        const double y = x[i] - x1 + r * y1; x1 = x[i]; y1 = y;
        if (i >= a) sum += y * y;
    }
    return 10.0 * std::log10(sum / (double)(b - a) + 1e-20);
}
static double peakDb(const Buf& x, size_t a = 0, size_t b = (size_t)-1)
{
    float pk = 0.0f;
    for (size_t i = a; i < std::min(b, x.size()); ++i) pk = std::max(pk, std::fabs(x[i]));
    return 20.0 * std::log10(pk + 1e-20);
}
static double meanOf(const std::vector<float>& v, size_t a, size_t b)
{
    double s = 0; size_t n = 0;
    for (size_t i = a; i < std::min(b, v.size()); ++i) { s += v[i]; ++n; }
    return n ? s / (double)n : 0.0;
}
// Distortion of a sine of known frequency: residual after removing the fundamental (Goertzel fit).
static double thdPct(const Buf& x, size_t a, size_t b, double hz)
{
    double c = 0, s = 0; const size_t n = b - a;
    for (size_t i = a; i < b; ++i) { const double w = 6.283185307 * hz * (double)i / SR; c += x[i] * std::cos(w); s += x[i] * std::sin(w); }
    c *= 2.0 / (double)n; s *= 2.0 / (double)n;
    double res = 0, fund = 0;
    for (size_t i = a; i < b; ++i)
    {
        const double w = 6.283185307 * hz * (double)i / SR;
        const double f = c * std::cos(w) + s * std::sin(w);
        fund += f * f; res += (x[i] - f) * (x[i] - f);
    }
    return 100.0 * std::sqrt(res / (fund + 1e-20));
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

static const char* kHullNames[4] = {"Glass", "Bite", "Valve", "Bus"};

// A sine burst test signal: quiet, loud for `burstS` seconds, quiet again.
static Buf burst(double hz, float quietDb, float loudDb_, double preS, double burstS, double postS)
{
    const size_t a = (size_t)(preS * SR), b = a + (size_t)(burstS * SR), total = b + (size_t)(postS * SR);
    Buf o(total);
    const float gq = (float)std::pow(10.0, quietDb / 20.0), gl = (float)std::pow(10.0, loudDb_ / 20.0);
    for (size_t i = 0; i < total; ++i)
        o[i] = (float)std::sin(6.283185307 * hz * (double)i / SR) * ((i >= a && i < b) ? gl : gq);
    return o;
}

// Attack: ms from the step up until GR reaches 63 % of its peak in the next 300 ms.
// Release: ms from the step down until GR has recovered to 10 % of its value at the step.
static void timing(const std::vector<float>& gr, int block, double preS, double burstS, double& attMs, double& relMs, double& grAtStep)
{
    const size_t up = (size_t)(preS * SR / block), down = (size_t)((preS + burstS) * SR / block);
    float pk = 0.0f;
    for (size_t i = up; i < std::min(gr.size(), up + (size_t)(0.3 * SR / block)); ++i) pk = std::max(pk, gr[i]);
    attMs = -1;
    for (size_t i = up; i < gr.size(); ++i) if (gr[i] >= 0.63f * pk) { attMs = (double)(i - up + 1) * block * 1000.0 / SR; break; }
    grAtStep = gr[down > 0 ? down - 1 : 0];
    relMs = -1;
    for (size_t i = down; i < gr.size(); ++i) if (gr[i] <= 0.1f * grAtStep) { relMs = (double)(i - down) * block * 1000.0 / SR; break; }
}

int main(int argc, char** argv)
{
    const double bpm = 100.0;
    const int bars = 8;
    const Buf dr = drums(bpm, bars), bs = bass(bpm, bars), pd = pad(bpm, bars);
    const Buf mx = mixOf(dr, bs, pd, 0.8f, 0.7f, 1.0f);
    struct Src { const char* name; const Buf* b; };
    const Src srcs[3] = {{"drums", &dr}, {"bass", &bs}, {"mix", &mx}};
    const size_t settle = (size_t)(4.0 * SR);
    std::printf("material: drums %.1f dB, bass %.1f dB, mix %.1f dB (weighted RMS); peaks %.1f / %.1f / %.1f dBFS\n",
                loudDb(dr, 0, dr.size()), loudDb(bs, 0, bs.size()), loudDb(mx, 0, mx.size()), peakDb(dr), peakDb(bs), peakDb(mx));

    // ---- 1 + 2: Depth sweep per Hull and source: GR grows, loudness stays put ----
    std::printf("\n== Depth sweep: mean GR (dB) / loudness change (dB) ==\n");
    const float depths[5] = {0, 25, 50, 75, 100};
    bool allMono = true, allMatched = true;
    double worstMatch = 0.0;
    float grAt[4][3][5];
    for (int h = 0; h < 4; ++h)
        for (int si = 0; si < 3; ++si)
        {
            const Buf& in = *srcs[si].b;
            const double inDb = loudDb(in, settle, in.size());
            std::printf("%-6s %-6s", kHullNames[h], srcs[si].name);
            for (int di = 0; di < 5; ++di)
            {
                Settings s; s.hull = h; s.depth = depths[di];
                Render o; run(s, in, bpm, o);
                const double g = meanOf(o.gr, settle / 64, o.gr.size());
                const double dl = 0.5 * (loudDb(o.l, settle, in.size()) + loudDb(o.r, settle, in.size())) - inDb;
                grAt[h][si][di] = (float)g;
                std::printf("  %5.1f/%+4.1f", g, dl);
                if (std::fabs(dl) > 1.5) allMatched = false;
                worstMatch = std::max(worstMatch, std::fabs(dl));
                if (di > 0 && g <= grAt[h][si][di - 1] + 0.3) allMono = false;
            }
            std::printf("\n");
        }
    check(allMono, "gain reduction grows with Depth on every Hull and source (by > 0.3 dB per step)");
    {
        bool lowOk = true, highOk = true;
        for (int h = 0; h < 4; ++h) for (int si = 0; si < 3; ++si)
        {
            if (grAt[h][si][0] > 3.0f) lowOk = false;
            if (grAt[h][si][4] < 6.0f) highOk = false;
        }
        check(lowOk, "Depth 0 is gentle: mean GR under 3 dB everywhere");
        check(highOk, "Depth 100 squeezes: mean GR over 6 dB everywhere");
    }
    {
        char b[160]; std::snprintf(b, sizeof(b), "loudness matched within 1.5 dB across Depth, Hull and source (worst %.2f dB)", worstMatch);
        check(allMatched, b);
    }

    // ---- 3: each Hull's timing ----
    std::printf("\n== Hull timing (Depth 60, 1 kHz sine, silence -> -6 dBFS burst -> silence) ==\n");
    double att[4], relShort[4], relLong[4];
    for (int h = 0; h < 4; ++h)
    {
        Settings s; s.hull = h; s.depth = 60; s.growl = 0;
        double g1, g2;
        Render o1; run(s, burst(1000.0, -200, -6, 3.0, 0.25, 3.0), bpm, o1, 8);
        timing(o1.gr, 8, 3.0, 0.25, att[h], relShort[h], g1);
        Render o2; run(s, burst(1000.0, -200, -6, 3.0, 3.0, 8.0), bpm, o2, 8);
        double attDummy; timing(o2.gr, 8, 3.0, 3.0, attDummy, relLong[h], g2);
        std::printf("%-6s attack %7.2f ms   release after 0.25 s burst %7.0f ms (GR %.1f)   after 3 s burst %7.0f ms (GR %.1f)\n",
                    kHullNames[h], att[h], relShort[h], g1, relLong[h], g2);
    }
    check(att[kBite] < att[kBus] && att[kBus] < att[kGlass] && att[kGlass] < att[kValve], "attack order: Bite < Bus < Glass < Valve");
    {
        bool distinct = true;
        for (int a = 0; a < 4; ++a) for (int b = a + 1; b < 4; ++b)
        {
            const double ra = std::max(att[a], att[b]) / std::max(1e-3, std::min(att[a], att[b]));
            const double rr = std::max(relShort[a], relShort[b]) / std::max(1e-3, std::min(relShort[a], relShort[b]));
            if (ra < 1.4 && rr < 1.4) distinct = false;
        }
        check(distinct, "every pair of Hulls differs by 1.4x or more in attack or release");
    }
    check(att[kBite] < 1.5, "Bite grabs in under 1.5 ms");
    check(relShort[kValve] > relShort[kBus] && relShort[kValve] > relShort[kGlass] && relShort[kValve] > relShort[kBite], "Valve has the slowest release");
    check(relLong[kGlass] > 2.0 * relShort[kGlass], "Glass: programme-dependent two-stage release (slower after a long squeeze)");
    check(relLong[kBite] < 1.3 * relShort[kBite], "Bite: no release memory (same release after short and long bursts)");

    // ---- 4: synced release follows the tempo ----
    std::printf("\n== Tide Sync (1/4) at 90 and 135 BPM ==\n");
    for (int h : {kBite, kBus})
    {
        double rel[2];
        const double tempos[2] = {90.0, 135.0};
        for (int t = 0; t < 2; ++t)
        {
            Settings s; s.hull = h; s.depth = 60; s.growl = 0; s.tide = kTideSync; s.release = syncPos(8);
            Render o; run(s, burst(1000.0, -200, -6, 3.0, 0.25, 3.0), tempos[t], o, 8);
            double a, g; timing(o.gr, 8, 3.0, 0.25, a, rel[t], g);
            std::printf("%-6s %5.0f BPM: 90%% recovery %6.0f ms (a quarter note is %.0f ms)\n", kHullNames[h], tempos[t], rel[t], 60000.0 / tempos[t]);
        }
        const double ratio = rel[0] / rel[1];
        char b[160]; std::snprintf(b, sizeof(b), "%s: synced release scales with tempo (ratio %.2f, expected 1.50)", kHullNames[h], ratio);
        check(std::fabs(ratio - 1.5) < 0.15, b);
        const double q = 60000.0 / 90.0;
        std::snprintf(b, sizeof(b), "%s: a 1/4 release has recovered within a quarter note (%.0f of %.0f ms)", kHullNames[h], rel[0], q);
        check(rel[0] > 0.4 * q && rel[0] < 1.1 * q, b);
    }

    // ---- 5: Blend ----
    std::printf("\n== Blend ==\n");
    {
        Settings s; s.hull = kBite; s.depth = 80; s.blend = 0;
        Render o; run(s, mx, bpm, o);
        double maxDiff = 0;
        const int lat = pr::kOversampleLatency;
        for (size_t i = (size_t)lat; i < mx.size(); ++i) maxDiff = std::max(maxDiff, (double)std::fabs(o.l[i] - mx[i - (size_t)lat]));
        char b[160]; std::snprintf(b, sizeof(b), "Blend 0 is the dry signal, delayed by the reported latency (max diff %.1e)", maxDiff);
        check(maxDiff < 1e-4, b);

        auto crest = [&](const Buf& x) { return peakDb(x, settle) - loudDb(x, settle, x.size()); };
        Render w, h; s.blend = 100; run(s, mx, bpm, w); s.blend = 50; run(s, mx, bpm, h);
        const double cDry = crest(mx), cHalf = crest(h.l), cWet = crest(w.l);
        std::printf("crest factor: dry %.1f dB, Blend 50 %.1f dB, Blend 100 %.1f dB\n", cDry, cHalf, cWet);
        check(cWet < cHalf && cHalf < cDry, "Blend 50 sits between dry and fully compressed (parallel smash)");
        const double dHalf = loudDb(h.l, settle, mx.size()) - loudDb(mx, settle, mx.size());
        std::snprintf(b, sizeof(b), "Blend 50 is loudness-matched too (%+.2f dB)", dHalf);
        check(std::fabs(dHalf) < 1.5, b);
    }

    // ---- 6: sidechain high-pass ----
    std::printf("\n== Sidechain high-pass ==\n");
    {
        const Buf heavy = mixOf(drums(bpm, bars, 2.0f), bs, pd, 0.8f, 0.5f, 1.0f);
        Settings s; s.hull = kBus; s.depth = 60;
        Render a, b2, c;
        run(s, heavy, bpm, a); s.sc = 120; run(s, heavy, bpm, b2); s.sc = 250; run(s, heavy, bpm, c);
        const double g0 = meanOf(a.gr, settle / 64, a.gr.size()), g1 = meanOf(b2.gr, settle / 64, b2.gr.size()), g2 = meanOf(c.gr, settle / 64, c.gr.size());
        std::printf("kick-heavy mix, mean GR: Off %.2f dB, 120 Hz %.2f dB, 250 Hz %.2f dB\n", g0, g1, g2);
        check(g2 < 0.7 * g0 && g1 < g0, "SC HPF stops the kick yanking the gain (less GR as it rises)");
    }

    // ---- 6b: stereo link and Mid/Side ----
    std::printf("\n== Stereo ==\n");
    {
        // Drums on the left only, the pad on the right only.
        Buf L(dr.size()), R(dr.size());
        for (size_t i = 0; i < L.size(); ++i) { L[i] = dr[i]; R[i] = 2.0f * pd[i]; }
        double grR[2];
        for (int li = 0; li < 2; ++li)
        {
            Settings s; s.hull = kBite; s.depth = 60; s.link = li ? 100.0f : 0.0f;
            Pressure p; p.prepare(SR, 512); apply(p, s); p.reset();
            mpc::ScopedDenormalsOff ftz;
            const int block = 64; Buf l(block), r(block); double sum = 0; int cnt = 0;
            for (size_t pos = 0; pos + block <= L.size(); pos += block)
            {
                for (int i = 0; i < block; ++i) { l[(size_t)i] = L[pos + (size_t)i]; r[(size_t)i] = R[pos + (size_t)i]; }
                const float* ins[2] = {l.data(), r.data()}; float* outs[2] = {l.data(), r.data()};
                mpc::ProcessContext ctx; ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = block; ctx.sampleRate = SR;
                p.process(ctx);
                if (pos > settle) { sum += p.gr_[1] - p.gr_[0]; ++cnt; }
            }
            grR[li] = sum / cnt;
        }
        std::printf("right minus left GR (drums left, pad right): Link 0 %+.1f dB, Link 100 %+.1f dB\n", grR[0], grR[1]);
        check(std::fabs(grR[0]) > 2.0 && std::fabs(grR[1]) < 0.05, "Link: unlinked sides compress on their own, fully linked share one gain");

        Settings s; s.hull = kBus; s.depth = 70; s.stereo = 1; s.link = 0;
        Pressure p; p.prepare(SR, 512); apply(p, s); p.reset();
        Render o; render(p, mx, mx, bpm, 64, o);
        double maxLR = 0; for (size_t i = 0; i < o.l.size(); ++i) maxLR = std::max(maxLR, (double)std::fabs(o.l[i] - o.r[i]));
        std::printf("Mid/Side, Link 0, mono mix: mid GR %.1f dB, side GR %.1f dB, max |L-R| %.1e\n", p.gr_[0], p.gr_[1], maxLR);
        check(maxLR < 1e-5 && p.gr_[1] < 0.01 && meanOf(o.gr, settle / 64, o.gr.size()) > 2.0, "Mid/Side: a mono source compresses the mid only and stays mono");
    }

    // ---- 7: Implode ----
    std::printf("\n== Implode ==\n");
    {
        Settings s; s.hull = kBite; s.depth = 70; s.growl = 40;
        Render a, b2; run(s, dr, bpm, a); s.implode = true; run(s, dr, bpm, b2);
        const double ga = meanOf(a.gr, settle / 64, a.gr.size()), gb = meanOf(b2.gr, settle / 64, b2.gr.size());
        const double ca = peakDb(a.l, settle) - loudDb(a.l, settle, dr.size()), cb = peakDb(b2.l, settle) - loudDb(b2.l, settle, dr.size());
        std::printf("drums, Bite 70%%: mean GR %.1f -> %.1f dB, crest %.1f -> %.1f dB, peak %.1f dBFS\n", ga, gb, ca, cb, peakDb(b2.l));
        check(gb > ga + 2.0, "Implode crushes harder than Bite alone");
        check(cb < ca, "Implode flattens the drums (lower crest factor)");
        check(peakDb(b2.l) < 3.0, "Implode stays bounded (peak under +3 dBFS)");
        Settings t; t.hull = kBite; t.depth = 70; t.growl = 40;
        const Buf sine = burst(110.0, -12, -12, 0.0, 0.0, 3.0);
        Render x, y; run(t, sine, bpm, x); t.implode = true; run(t, sine, bpm, y);
        const size_t a0 = (size_t)(1.5 * SR), a1 = (size_t)(2.5 * SR);
        const double tx = thdPct(x.l, a0, a1, 110.0), ty = thdPct(y.l, a0, a1, 110.0);
        std::printf("110 Hz sine: distortion %.1f %% -> %.1f %% with Implode\n", tx, ty);
        check(ty > 1.5 * tx, "Implode adds distortion");
    }

    // ---- 8: Growl reacts to the gain reduction ----
    std::printf("\n== Reactive saturation (Growl 60) ==\n");
    {
        const Buf sine = burst(110.0, -12, -12, 0.0, 0.0, 3.0);
        const size_t a0 = (size_t)(1.5 * SR), a1 = (size_t)(2.5 * SR);
        for (int col = 0; col < 3; ++col)
        {
            const char* names[3] = {"Tape", "Tube", "Iron"};
            double t[3]; const float ds[3] = {10, 50, 90};
            for (int i = 0; i < 3; ++i)
            {
                Settings s; s.hull = kValve; s.colour = col; s.growl = 60; s.depth = ds[i];
                Render o; run(s, sine, bpm, o);
                t[i] = thdPct(o.l, a0, a1, 110.0);
            }
            std::printf("%s: distortion at Depth 10/50/90: %.2f / %.2f / %.2f %%\n", names[col], t[0], t[1], t[2]);
            char b[120]; std::snprintf(b, sizeof(b), "%s growls harder as it works harder", names[col]);
            check(t[0] < t[1] && t[1] < t[2], b);
        }
        Settings s; s.hull = kValve; s.growl = 0; s.depth = 50;
        Render o0; run(s, sine, bpm, o0);
        s.growl = 100; Render o1; run(s, sine, bpm, o1);
        const double t0 = thdPct(o0.l, a0, a1, 110.0), t1 = thdPct(o1.l, a0, a1, 110.0);
        std::printf("Growl 0 vs 100 at Depth 50: %.2f %% vs %.2f %%\n", t0, t1);
        check(t1 > 3.0 * t0, "Growl sets how much it growls");
    }

    // ---- 9: Bloom ----
    std::printf("\n== Bloom ==\n");
    {
        double minEff[2];
        for (int bi = 0; bi < 2; ++bi)
        {
            Settings s; s.hull = kBus; s.depth = 70; s.growl = 0; s.tide = kTideFree; s.release = freeReleasePos(200.0f); s.bloom = bi ? 100.0f : 0.0f;
            std::vector<float> eff;
            Render o; run(s, burst(1000.0, -200, -6, 3.0, 0.5, 2.0), bpm, o, 8, &eff);
            minEff[bi] = *std::min_element(eff.begin() + (long)(3.0 * SR / 8), eff.end());
        }
        std::printf("lowest effective GR after the burst: Bloom 0 %.2f dB, Bloom 100 %.2f dB (negative = swell above unity)\n", minEff[0], minEff[1]);
        check(minEff[0] > -0.01 && minEff[1] < -1.0, "Bloom makes the release overshoot and swell back");
    }

    // ---- 10: Breath ----
    {
        Settings s; s.breath = 100; s.depth = 20;
        Pressure p; p.prepare(SR, 512); apply(p, s); p.reset();
        Render o; render(p, mx, mx, bpm, 64, o);
        const float lo = p.breathT_;
        Pressure q; q.prepare(SR, 512); s.depth = 95; apply(q, s); q.reset();
        render(q, mx, mx, bpm, 64, o);
        const float hi = q.breathT_;
        std::printf("\nBreath level: Depth 20 %.1f dBFS, Depth 95 %.1f dBFS\n", 20 * std::log10(lo * 0.24f), 20 * std::log10(hi * 0.24f));
        check(hi > 2.0f * lo, "Breath rises as it compresses");
    }

    // ---- 11: meter ----
    {
        Settings s; s.hull = kBite; s.depth = 80;
        Pressure p; p.prepare(SR, 512); apply(p, s); p.reset();
        mpc::ScopedDenormalsOff ftz;
        const int block = 128;
        Buf l(block), r(block);
        double maxErr = 0.0, maxMeter = 0.0;
        for (size_t pos = 0; pos + block <= dr.size(); pos += block)
        {
            for (int i = 0; i < block; ++i) l[(size_t)i] = r[(size_t)i] = dr[pos + (size_t)i];
            const float* ins[2] = {l.data(), r.data()}; float* outs[2] = {l.data(), r.data()};
            mpc::ProcessContext ctx; ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = block; ctx.sampleRate = SR;
            p.process(ctx);
            const double m = p.get(kGrMeter);
            maxMeter = std::max(maxMeter, m);
            // Compare on the blocks where the meter was written.
            if (p.meterCountdown_ == p.meterInterval_) maxErr = std::max(maxErr, std::fabs(m - std::min(p.vu_, kMeterMaxDb)));
        }
        std::printf("\nGR meter on the drums: up to %.1f dB; worst difference from the internal VU when written %.2f dB\n", maxMeter, maxErr);
        check(maxMeter > 2.0 && maxMeter <= kMeterMaxDb && maxErr < 0.2, "GR meter follows the gain reduction");
        check(!p.param(kGrMeter).saved && !p.param(kGrMeter).automatable, "GR meter is not saved and not automatable");
    }

    // ---- 12: programs, robustness ----
    std::printf("\n== Programs ==\n");
    {
        bool allClean = true, allLevel = true;
        const double rates[3] = {44100.0, 48000.0, 96000.0};
        for (int pi = 0; pi < (int)Pressure().programs_.size(); ++pi)
        {
            Pressure p; p.prepare(SR, 512); p.setProgram(pi); p.reset();
            Render o; render(p, mx, mx, bpm, 64, o);
            const double dl = loudDb(o.l, settle, mx.size()) - loudDb(mx, settle, mx.size());
            const double g = meanOf(o.gr, settle / 64, o.gr.size());
            std::printf("%-18s GR %5.1f dB, loudness %+5.2f dB, peak %5.1f dBFS\n", p.programName(pi), g, dl, peakDb(o.l, settle));
            if (std::fabs(dl) > 2.0) allLevel = false;
            for (double rate : rates)
                for (int blk : {32, 512})
                {
                    const double keep = SR; SR = rate;
                    Pressure q; q.prepare(SR, 512); q.setProgram(pi); q.reset();
                    const Buf m2 = mixOf(drums(bpm, 3), bass(bpm, 3), pad(bpm, 3), 0.8f, 0.7f, 1.0f);
                    Render o2; render(q, m2, m2, bpm, blk, o2);
                    int bad, sub;
                    if (!cleanBuf(o2.l, bad, sub) || !cleanBuf(o2.r, bad, sub) || peakDb(o2.l) > 6.0) allClean = false;
                    SR = keep;
                }
        }
        check(allLevel, "every program is within 2 dB of the input's loudness on the mix");
        check(allClean, "every program at 44.1/48/96 kHz, blocks 32 and 512: no NaN, no denormals, peaks under +6 dBFS");
    }
    {
        // Random settings, changed every 50 ms, on loud then silent input.
        Pressure p; p.prepare(SR, 512); p.reset();
        Buf in = mx; in.resize(in.size() + (size_t)(5.0 * SR), 0.0f);
        Buf ol(in.size()), orr(in.size());
        unsigned rs = 777;
        mpc::ScopedDenormalsOff ftz;
        const int block = 128;
        Buf l(block), r(block);
        int changes = 0;
        for (size_t pos = 0; pos < in.size(); pos += block)
        {
            if ((pos / block) % 17 == 0)
            {
                for (int k = 0; k < kGrMeter; ++k) { rs = rs * 1664525u + 1013904223u; p.setNormalised(k, (float)(rs >> 8) / 16777216.0f); ++changes; }
                p.set(kOutput, 0.0f);
            }
            const int n = (int)std::min((size_t)block, in.size() - pos);
            for (int i = 0; i < n; ++i) l[(size_t)i] = r[(size_t)i] = in[pos + (size_t)i] * 2.0f;   // +6 dB hot
            const float* ins[2] = {l.data(), r.data()}; float* outs[2] = {l.data(), r.data()};
            mpc::ProcessContext ctx; ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = n; ctx.sampleRate = SR;
            p.process(ctx);
            for (int i = 0; i < n; ++i) { ol[pos + (size_t)i] = l[(size_t)i]; orr[pos + (size_t)i] = r[(size_t)i]; }
        }
        int bad, sub, bad2, sub2;
        const bool okL = cleanBuf(ol, bad, sub), okR = cleanBuf(orr, bad2, sub2);
        const bool ok = okL && okR;
        char b[160]; std::snprintf(b, sizeof(b), "random settings (%d changes) on +6 dB-hot input: finite, no denormals, peak %.1f dBFS", changes, peakDb(ol));
        check(ok && peakDb(ol) < 12.0, b);
        bool statesOk = true;
        for (int c = 0; c < 2; ++c)
            for (float v : {p.ms_[c], p.gr_[c], p.slow_[c], p.bloomF_[c], p.fbPrev_[c]})
                if (!std::isfinite(v) || (v != 0.0f && std::fabs(v) < 1.17549435e-38f)) statesOk = false;
        check(statesOk, "after 5 s of silence the detector states are finite and not denormal");
        check(peakDb(ol, ol.size() - (size_t)(0.5 * SR)) < -60.0, "silence in, (near) silence out after the release");
    }

    // ---- 13: CPU ----
    std::printf("\n== CPU ==\n");
    {
        Settings s; s.hull = kGlass; s.colour = kIron; s.depth = 80; s.growl = 70; s.bloom = 50; s.sc = 120; s.stereo = 1; s.link = 50; s.breath = 30; s.blend = 60;
        Pressure p; p.prepare(SR, 512); apply(p, s); p.reset();
        Buf big; for (int k = 0; k < 4; ++k) big.insert(big.end(), mx.begin(), mx.end());
        Render o;
        const auto t0 = std::chrono::steady_clock::now();
        render(p, big, big, bpm, 128, o);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double audio = (double)big.size() / SR;
        const double pct = 100.0 * secs / audio;
        std::printf("worst case (Glass feedback, Iron, M/S, SC, Bloom, Breath, Blend): %.3f %% of one desktop core\n", pct);
        std::printf("MPC estimate (x15 to x30 slower per core): %.1f to %.1f %%\n", pct * 15.0, pct * 30.0);
        check(pct < 0.3, "CPU under 0.3 % of a desktop core (a few percent of an MPC core)");
        Pressure q; q.prepare(SR, 512); q.reset();
        const auto t1 = std::chrono::steady_clock::now();
        render(q, big, big, bpm, 128, o);
        const double pctDef = 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() / audio;
        std::printf("default settings (Bus, Tape): %.3f %% of one desktop core\n", pctDef);
    }

    // ---- demo ----
    if (argc > 1)
    {
        Buf L, R;
        auto append = [&](const Buf& a, const Buf& b) { L.insert(L.end(), a.begin(), a.end()); R.insert(R.end(), b.begin(), b.end()); };
        auto gap = [&](double s) { Buf z((size_t)(s * SR), 0.0f); append(z, z); };
        const int db2 = 2;
        const Buf d2 = drums(bpm, db2), b2 = bass(bpm, db2);
        const Buf m2 = mixOf(d2, b2, pad(bpm, db2), 0.8f, 0.7f, 1.0f);
        std::printf("\ndemo sections (2 bars each at %.0f BPM, %.1f s):\n", bpm, d2.size() / SR);
        double tcur = 0;
        auto section = [&](const char* what, const Buf& in, const Settings* s, int program = -1)
        {
            std::printf("  %6.1f s  %s\n", tcur, what);
            if (!s && program < 0) append(in, in);
            else
            {
                Pressure p; p.prepare(SR, 512);
                if (program >= 0) p.setProgram(program); else apply(p, *s);
                p.reset();
                // Pre-roll one pass so the level tracker and loudness match have settled.
                Render pre; render(p, in, in, bpm, 128, pre);
                Render o; render(p, in, in, bpm, 128, o);
                append(o.l, o.r);
            }
            tcur += in.size() / SR;
            gap(0.4); tcur += 0.4;
        };
        const Src dsrc[3] = {{"drums", &d2}, {"bass", &b2}, {"mix", &m2}};
        for (const Src& src : dsrc)
        {
            std::string w = std::string(src.name) + ": dry";
            section(w.c_str(), *src.b, nullptr);
            for (int h = 0; h < 4; ++h)
            {
                Settings s; s.hull = h; s.depth = 65; s.growl = 45;
                w = std::string(src.name) + ": " + kHullNames[h] + ", Depth 65";
                section(w.c_str(), *src.b, &s);
            }
            Settings s; s.hull = kBite; s.depth = 75; s.implode = true; s.growl = 50; s.blend = 100;
            w = std::string(src.name) + ": Bite + Implode";
            section(w.c_str(), *src.b, &s);
        }
        // Depth sweep on the mix: 0 -> 100 over 8 bars, Bus, Tape, Growl 40.
        {
            std::printf("  %6.1f s  mix: Depth sweep 0 -> 100 %% over 8 bars (Bus, Tape, Growl 40)\n", tcur);
            const Buf m8 = mixOf(drums(bpm, 8), bass(bpm, 8), pad(bpm, 8), 0.8f, 0.7f, 1.0f);
            Pressure p; p.prepare(SR, 512);
            Settings s; s.hull = kBus; s.depth = 0; s.growl = 40; apply(p, s); p.reset();
            const int block = 128;
            Buf l(block), r(block), ol(m8.size()), orr(m8.size());
            mpc::ScopedDenormalsOff ftz;
            for (size_t pos = 0; pos < m8.size(); pos += block)
            {
                p.set(kDepth, 100.0f * (float)pos / (float)m8.size());
                const int n = (int)std::min((size_t)block, m8.size() - pos);
                for (int i = 0; i < n; ++i) l[(size_t)i] = r[(size_t)i] = m8[pos + (size_t)i];
                const float* ins[2] = {l.data(), r.data()}; float* outs[2] = {l.data(), r.data()};
                mpc::ProcessContext ctx; ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = n; ctx.sampleRate = SR;
                ctx.transport.valid = ctx.transport.tempoValid = ctx.transport.playing = ctx.transport.ppqValid = true; ctx.transport.bpm = bpm;
                p.process(ctx);
                for (int i = 0; i < n; ++i) { ol[pos + (size_t)i] = l[(size_t)i]; orr[pos + (size_t)i] = r[(size_t)i]; }
            }
            append(ol, orr);
            tcur += m8.size() / SR;
            gap(0.4); tcur += 0.4;
        }
        Pressure names;
        for (int pi = 0; pi < (int)names.programs_.size(); ++pi)
        {
            std::string w = std::string("mix: program \"") + names.programName(pi) + "\"";
            section(w.c_str(), m2, nullptr, pi);
        }
        writeWav(argv[1], L, R);
        std::printf("wrote %s (%.1f s)\n", argv[1], L.size() / SR);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
