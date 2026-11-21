// Does Low Tide wear out and wash back in time with the song? A drum loop + pad, checked bar by bar.
//
//   mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
//   tests/build/listentest [demo.wav]          (run from plugins/Low Tide)
#include <vector>
#include <cstdio>
#include <cmath>
#include <string>
#include <chrono>
#include <algorithm>

struct AccLog { long long sample; int type; };
struct PassLog { long long sample; int gen; };
static std::vector<AccLog> g_acc;
static std::vector<PassLog> g_pass;
static long long g_now = 0;
#define LOWTIDE_ACCIDENT_HOOK(offset, type) g_acc.push_back({g_now + (offset), (int)(type)})
#define LOWTIDE_PASS_HOOK(offset, gen) g_pass.push_back({g_now + (offset), (int)(gen)})

#define private public
#define protected public
#include "../src/LowTide.cpp"
#undef private
#undef protected

static const double SR = 44100.0;
static int failures = 0;
static void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }
static double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

static void writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r)
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

// ---- a one-bar drum loop with a pad, repeated -------------------------------------------------------
static std::vector<float> makeLoop(double bpm, int bars, bool pad = true)
{
    const double spb = SR * 60.0 / bpm, step = spb / 4.0;
    const int total = (int)(bars * 4 * spb);
    std::vector<float> in((size_t)total, 0.0f);
    unsigned rs = 99; auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (float)(rs >> 8) / 16777216.0f * 2.0f - 1.0f; };
    for (int b = 0; b < bars; ++b)
    {
        for (int k : {0, 7, 10})
        {
            const long long t0 = (long long)((b * 16 + k) * step);
            double ph = 0.0;
            for (int i = 0; i < (int)(0.3 * SR) && t0 + i < total; ++i)
            {
                const double t = i / SR, hz = 48.0 + 110.0 * std::exp(-t * 35.0);
                ph += hz / SR;
                in[(size_t)(t0 + i)] += (float)(0.7 * std::sin(6.2831853 * ph) * std::exp(-t * 9.0));
            }
        }
        for (int k = 0; k < 16; k += 2)
        {
            const long long t0 = (long long)((b * 16 + k) * step);
            const bool snare = (k == 4 || k == 12);
            float hp = 0.0f;
            for (int i = 0; i < (int)((snare ? 0.18 : 0.05) * SR) && t0 + i < total; ++i)
            {
                const double t = i / SR;
                const float w = rnd(); const float hpv = w - hp; hp = w;          // brighter hats
                float v = (snare ? w : hpv) * (float)std::exp(-t * (snare ? 22.0 : 70.0)) * (snare ? 0.4f : 0.18f);
                if (snare) v += (float)(0.22 * std::sin(6.2831853 * 190.0 * t) * std::exp(-t * 30.0));
                in[(size_t)(t0 + i)] += v;
            }
        }
        if (pad)
        {
            const int chord[3] = {57, 60, 64};
            for (int v = 0; v < 3; ++v)
            {
                const double hz = 440.0 * std::pow(2.0, (chord[v] - 69) / 12.0);
                double ph = 0.0;
                const long long t0 = (long long)(b * 4 * spb), len = (long long)(4 * spb);
                for (long long i = 0; i < len && t0 + i < total; ++i)
                {
                    ph += hz / SR; ph -= std::floor(ph);
                    const float env = (float)std::fmin(1.0, std::fmin(i / (0.02 * SR), (len - i) / (0.02 * SR)));
                    in[(size_t)(t0 + i)] += (float)(2.0 * ph - 1.0) * 0.05f * env;
                }
            }
        }
    }
    for (float& v : in) v *= 0.6f;
    return in;
}

struct Render
{
    std::vector<float> l, r;
};

// Render `src` through `p`. playFrom/ppqWrap simulate the transport: ppqWrap > 0 makes the song position
// jump back every ppqWrap beats (an MPC sequence looping).
static Render render(LowTide& p, const std::vector<float>& src, double bpm, bool playing, int block = 128,
                     double ppqWrap = 0.0, bool record = true)
{
    if (record) { g_acc.clear(); g_pass.clear(); }
    const double spb = SR * 60.0 / bpm;
    Render out; out.l.assign(src.size(), 0.0f); out.r.assign(src.size(), 0.0f);
    std::vector<float> l(block), r(block);
    const int total = (int)src.size();
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min(block, total - pos);
        for (int i = 0; i < n; ++i) l[i] = r[i] = src[(size_t)(pos + i)];
        const float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {l.data(), r.data()};
        mpc::ProcessContext ctx;
        ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = n;
        ctx.sampleRate = SR;
        ctx.transport.valid = ctx.transport.tempoValid = true;
        ctx.transport.ppqValid = playing; ctx.transport.playing = playing; ctx.transport.bpm = bpm;
        double ppq = pos / spb;
        if (ppqWrap > 0.0) ppq = std::fmod(ppq, ppqWrap);
        ctx.transport.ppqPos = playing ? ppq : 0.0;
        ctx.transport.timeSigNumerator = 4; ctx.transport.timeSigDenominator = 4;
        g_now = pos;
        p.process(ctx);
        for (int i = 0; i < n; ++i) { out.l[(size_t)(pos + i)] = l[i]; out.r[(size_t)(pos + i)] = r[i]; }
    }
    return out;
}

// Brightness: energy of the first difference over energy (a crude "how much top end" number).
static double brightness(const std::vector<float>& x, long long a, long long b)
{
    double e = 0.0, d = 0.0;
    for (long long i = std::max(1LL, a); i < b && i < (long long)x.size(); ++i)
    {
        e += (double)x[(size_t)i] * x[(size_t)i];
        const double df = (double)x[(size_t)i] - x[(size_t)i - 1];
        d += df * df;
    }
    return e > 0.0 ? d / e : 0.0;
}
static double rms(const std::vector<float>& x, long long a, long long b)
{
    double e = 0.0; long long n = 0;
    for (long long i = std::max(0LL, a); i < b && i < (long long)x.size(); ++i, ++n) e += (double)x[(size_t)i] * x[(size_t)i];
    return n ? std::sqrt(e / (double)n) : 0.0;
}

// A plugin with every stage off except the ones a test turns on.
static void isolate(LowTide& p)
{
    p.set(kMix, 100); p.set(kWear, 50); p.set(kGens, 0); p.set(kMemory, 0); p.set(kTideDepth, 0);
    p.set(kChance, 0); p.set(kCrunch, 0); p.set(kPrint, 0); p.set(kAzimuth, 0); p.set(kHiss, 0);
    p.set(kBreathe, 0); p.set(kWow, 0); p.set(kFlutter, 0);
}

int main(int argc, char** argv)
{
    const double bpm = 120.0;
    const double spb = SR * 60.0 / bpm, bar = 4.0 * spb;

    // ==== 1. Generations wear the loop out pass by pass, and Fresh brings it back ====
    {
        const int bars = 14;
        std::vector<float> in = makeLoop(bpm, bars);
        LowTide p; p.prepare(SR, 512); isolate(p);
        p.set(kGens, 8); p.set(kBars, 0); p.set(kAtCap, 0); p.set(kHiss, 30);
        p.reset();
        // flip Fresh at the start of bar 11 (between blocks)
        Render out; out.l.assign(in.size(), 0.0f); out.r = out.l;
        {
            const int cut = (int)(10 * bar);
            std::vector<float> a(in.begin(), in.begin() + cut), b(in.begin() + cut, in.end());
            Render r1 = render(p, a, bpm, true);
            const std::vector<PassLog> passes = g_pass;
            // continue the song from bar 11: shift ppq by rendering with an offset
            p.set(kFresh, 1);
            g_pass.clear();
            std::vector<float> l(128), r(128);
            Render r2; r2.l.assign(b.size(), 0.0f); r2.r = r2.l;
            for (int pos = 0; pos < (int)b.size(); pos += 128)
            {
                const int n = std::min(128, (int)b.size() - pos);
                for (int i = 0; i < n; ++i) l[i] = r[i] = b[(size_t)(pos + i)];
                const float* ins[2] = {l.data(), r.data()}; float* outs[2] = {l.data(), r.data()};
                mpc::ProcessContext ctx; ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = ctx.numOutputs = 2; ctx.numFrames = n;
                ctx.sampleRate = SR; ctx.transport.valid = ctx.transport.tempoValid = ctx.transport.ppqValid = ctx.transport.playing = true;
                ctx.transport.bpm = bpm; ctx.transport.ppqPos = (cut + pos) / spb;
                g_now = cut + pos; p.process(ctx);
                for (int i = 0; i < n; ++i) { r2.l[(size_t)(pos + i)] = l[i]; r2.r[(size_t)(pos + i)] = r[i]; }
            }
            std::copy(r1.l.begin(), r1.l.end(), out.l.begin());
            std::copy(r2.l.begin(), r2.l.end(), out.l.begin() + cut);
            std::printf("passes in bars 1-10:");
            for (const PassLog& pl : passes) std::printf(" bar%.2f->g%d", pl.sample / bar + 1.0, pl.gen);
            std::printf("\n");
            bool onBars = !passes.empty();
            for (const PassLog& pl : passes) if (std::fabs(pl.sample / bar - std::round(pl.sample / bar)) * bar > 1.0) onBars = false;
            check(onBars && passes.size() == 10, "one pass per bar, on the bar line");
        }
        std::printf("bar  brightness(out/in)  level(out/in dB)\n");
        double ratio[15] = {0};
        for (int b = 0; b < bars; ++b)
        {
            const long long a = (long long)(b * bar + 0.6 * spb), e = (long long)((b + 1) * bar);   // after the dub has landed
            ratio[b] = brightness(out.l, a, e) / brightness(in, a, e);
            std::printf("%3d  %6.3f  %+6.1f\n", b + 1, ratio[b], dB(rms(out.l, a, e) / rms(in, a, e)));
        }
        bool falling = true;
        for (int b = 1; b <= 8; ++b) if (ratio[b] > ratio[b - 1] + 0.01) falling = false;
        check(ratio[0] > 0.9, "bar 1 (generation 0) is close to the input's brightness");
        check(falling && ratio[8] < 0.45 * ratio[0], "brightness falls generation by generation (to under 45 % by generation 8)");
        check(std::fabs(ratio[9] - ratio[8]) < 0.05 * ratio[8], "Hold: stays at the cap (8) in bar 10");
        check(ratio[10] > 0.85 * ratio[0], "Fresh: bar 11 is clean again");
        check(ratio[11] < ratio[10], "and starts wearing again in bar 12");
        double lv = 0.0; for (int b = 0; b < bars; ++b) lv = std::max(lv, std::fabs(dB(rms(out.l, (long long)(b * bar), (long long)((b + 1) * bar)) / rms(in, (long long)(b * bar), (long long)((b + 1) * bar)))));
        check(lv < 3.0, "level stays within 3 dB of the input on every bar (usable on a mix)");
    }

    // ==== 2. At Cap: Restart and Bounce ====
    {
        std::vector<float> in = makeLoop(bpm, 12, false);
        for (int mode = 1; mode <= 2; ++mode)
        {
            LowTide p; p.prepare(SR, 512); isolate(p); p.set(kGens, 3); p.set(kBars, 0); p.set(kAtCap, (float)mode); p.reset();
            render(p, in, bpm, true);
            std::string seq; for (const PassLog& pl : g_pass) seq += std::to_string(pl.gen);
            std::printf("At Cap %s: generations per pass %s\n", mode == 1 ? "Restart" : "Bounce", seq.c_str());
            check(mode == 1 ? seq.rfind("1230123", 0) == 0 : seq.rfind("12321012", 0) == 0,
                  mode == 1 ? "Restart: 1 2 3 then back to 0" : "Bounce: up to 3, back down to 0, up again");
        }
    }

    // ==== 3. Wear 0 is exactly clean, whatever the stages are set to ====
    {
        std::vector<float> in = makeLoop(bpm, 3);
        LowTide p; p.prepare(SR, 512);
        p.setProgram(5);                       // Warped Cassette: everything on
        p.set(kWear, 0); p.set(kMemory, 0); p.reset();
        Render o = render(p, in, bpm, true);
        double md = 0.0; for (size_t i = 0; i < in.size(); ++i) md = std::max(md, (double)std::fabs(o.l[i] - in[i]));
        std::printf("Wear 0, every stage set: max difference from the input %.2e\n", md);
        check(md < 1e-5, "Wear 0 = the input, untouched (no hidden colour)");
        check(g_acc.empty(), "Wear 0 = no accidents");
    }

    // ==== 4. The Tide tracks the bars (and keeps going when a sequence loops or the transport stops) ====
    {
        const int bars = 17;
        std::vector<float> in = makeLoop(bpm, bars, false);
        auto tideAt = [&](int shape, double ppqWrap, bool playing, std::vector<double>& wv)
        {
            LowTide p; p.prepare(SR, 512); isolate(p);
            p.set(kWear, 100); p.set(kTideDepth, 100); p.set(kTideRate, 3); p.set(kTideShape, (float)shape); p.reset();
            wv.clear();
            const int block = 128;
            std::vector<float> l(block), r(block);
            for (int pos = 0; pos < (int)in.size(); pos += block)
            {
                const int n = std::min(block, (int)in.size() - pos);
                for (int i = 0; i < n; ++i) l[i] = r[i] = in[(size_t)(pos + i)];
                const float* ins[2] = {l.data(), r.data()}; float* outs[2] = {l.data(), r.data()};
                mpc::ProcessContext ctx; ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = ctx.numOutputs = 2; ctx.numFrames = n;
                ctx.sampleRate = SR; ctx.transport.valid = ctx.transport.tempoValid = true;
                ctx.transport.ppqValid = ctx.transport.playing = playing; ctx.transport.bpm = bpm;
                double ppq = pos / spb; if (ppqWrap > 0.0) ppq = std::fmod(ppq, ppqWrap);
                ctx.transport.ppqPos = playing ? ppq : 0.0;
                p.process(ctx);
                // sample the wear at each bar line (the block that starts on it)
                if (pos % (int)bar < block) wv.push_back(p.wearNow_);
            }
        };
        std::vector<double> w;
        tideAt(0, 0.0, true, w);
        std::printf("Swell, 8 bars, Depth 100: wear at bar lines:");
        for (double v : w) std::printf(" %.2f", v);
        std::printf("\n");
        bool ok = w.size() >= 17;
        for (size_t b = 0; b < w.size() && b < 17; ++b)
        {
            const double want = 0.5 - 0.5 * std::cos(6.283185307 * (double)b / 8.0);
            if (std::fabs(w[b] - want) > 0.02) ok = false;
        }
        check(ok, "Swell: clean on bar 1, most worn on bar 5, clean again on bar 9 (cycle of 8 bars)");

        tideAt(1, 0.0, true, w);
        std::printf("Erode: ");
        for (double v : w) std::printf(" %.2f", v);
        std::printf("\n");
        check(w.size() >= 10 && w[0] < 0.02 && w[7] > 0.95 && w[8] < 0.02 && w[3] > w[2] && w[2] > w[1],
              "Erode: hi-fi on bar 1, crumbling by bar 8, washed back on bar 9");

        std::vector<double> wl;
        tideAt(0, 8.0, true, wl);               // a 2-bar sequence looping
        bool same = wl.size() == w.size();
        std::vector<double> w0; tideAt(0, 0.0, true, w0);
        for (size_t b = 0; b < wl.size() && b < w0.size(); ++b) if (std::fabs(wl[b] - w0[b]) > 0.02) same = false;
        check(same, "a 2-bar sequence looping: the 8-bar tide keeps counting bars played");

        std::vector<double> ws;
        tideAt(0, 0.0, false, ws);              // transport stopped
        bool moves = false; for (double v : ws) if (v > 0.9) moves = true;
        bool sameFree = ws.size() == w0.size();
        for (size_t b = 0; b < ws.size() && b < w0.size(); ++b) if (std::fabs(ws[b] - w0[b]) > 0.03) sameFree = false;
        check(moves && sameFree, "transport stopped: the tide free-runs at the host tempo");
    }

    // ==== 5. Accidents land on the beat ====
    {
        std::vector<float> in = makeLoop(bpm, 16);
        LowTide p; p.prepare(SR, 512); isolate(p);
        p.set(kWear, 50); p.set(kChance, 60); p.set(kDensity, 2); p.set(kSeverity, 60); p.set(kChanCut, 1); p.reset();
        Render o = render(p, in, bpm, true, 96);
        int onBeat = 0; int types[7] = {0};
        for (const AccLog& a : g_acc)
        {
            const double beatPos = a.sample / spb;
            if (std::fabs(beatPos - std::round(beatPos)) * spb <= 1.0) ++onBeat;
            ++types[a.type];
        }
        std::printf("accidents: %zu on 64 beats (%d on a beat): dropout %d drag %d slip %d crinkle %d cutL %d cutR %d\n",
                    g_acc.size(), onBeat, types[1], types[2], types[3], types[4], types[5], types[6]);
        check(!g_acc.empty() && onBeat == (int)g_acc.size(), "every accident starts exactly on a beat");
        check(types[1] > 0 && types[2] > 0 && types[3] > 0 && types[4] > 0 && types[5] + types[6] > 0, "all five kinds happen");
        check(g_acc.size() >= 12 && g_acc.size() <= 40, "one at a time: a busy setting still leaves room between them");

        // gentle: an accident never makes it louder
        float inPk = 0.0f, outPk = 0.0f;
        for (size_t i = 0; i < in.size(); ++i) { inPk = std::max(inPk, std::fabs(in[i])); outPk = std::max(outPk, std::max(std::fabs(o.l[i]), std::fabs(o.r[i]))); }
        std::printf("peaks: in %.1f dBFS, out %.1f dBFS\n", dB(inPk), dB(outPk));
        check(outPk < inPk * 1.12f, "no accident pushes the level up (peak within 1 dB)");

        // a dropout really dips, a channel cut is one-sided
        bool dipOk = false, cutOk = false;
        for (const AccLog& a : g_acc)
        {
            const long long s = a.sample;
            if (a.type == kAccDropout && !dipOk)
            {
                const double during = rms(o.l, s + 200, s + 600), inRef = rms(in, s + 200, s + 600);
                if (inRef > 1e-3 && during < 0.6 * inRef) dipOk = true;
            }
            if ((a.type == kAccCutL || a.type == kAccCutR) && !cutOk)
            {
                const std::vector<float>& cutCh = a.type == kAccCutL ? o.l : o.r;
                const std::vector<float>& other = a.type == kAccCutL ? o.r : o.l;
                const long long m0 = s + (long long)(0.1 * spb), m1 = s + (long long)(0.4 * spb);
                if (rms(cutCh, m0, m1) < 0.35 * rms(other, m0, m1)) cutOk = true;
            }
        }
        check(dipOk, "a dropout dips the level");
        check(cutOk, "a channel cut drops one side only");

        // Pattern: 2 bars locked -> the same accidents every 2 bars
        LowTide q; q.prepare(SR, 512); isolate(q);
        q.set(kWear, 50); q.set(kChance, 50); q.set(kDensity, 3); q.set(kPattern, 2); q.reset();
        render(q, in, bpm, true);
        std::vector<std::pair<int, int>> cyc[8];
        for (const AccLog& a : g_acc)
        {
            const int barIdx = (int)(a.sample / bar);
            if (barIdx < 2 || barIdx >= 14) continue;
            const int c = (barIdx - 2) / 2;
            cyc[c].push_back({(int)std::lround(std::fmod(a.sample, 2.0 * bar) / (spb * 0.5)), a.type});
        }
        bool rep = !cyc[0].empty();
        for (int c = 1; c < 6; ++c) if (cyc[c] != cyc[0]) rep = false;
        std::printf("Pattern 2 bars: %zu accidents per cycle\n", cyc[0].size());
        check(rep, "Pattern 2 Bars: the same accidents on the same 8ths every 2 bars");
    }

    // ==== 6. Memory: the loop re-records over itself ====
    {
        std::vector<float> in = makeLoop(bpm, 8);
        for (size_t i = (size_t)(2 * bar); i < in.size(); ++i) in[i] = 0.0f;     // 2 bars of music, then silence
        LowTide p; p.prepare(SR, 512); isolate(p); p.set(kMemory, 100); p.set(kWear, 0); p.set(kBars, 0); p.reset();
        Render o = render(p, in, bpm, true);
        const double r1 = rms(in, (long long)bar, (long long)(2 * bar)), r5 = rms(o.l, (long long)(5 * bar), (long long)(6 * bar));
        std::printf("Memory 100 %%, Wear 0: bar 6 replays bar 2 at %+.2f dB\n", dB(r5 / r1));
        check(std::fabs(dB(r5 / r1)) < 0.5, "Memory 100 %, Wear 0: the loop replays itself unchanged");

        LowTide q; q.prepare(SR, 512); isolate(q); q.set(kMemory, 100); q.set(kWear, 70); q.set(kHiss, 30); q.set(kBars, 0); q.reset();
        Render o2 = render(q, in, bpm, true);
        double br[8]; std::printf("Memory 100 %%, Wear 70: brightness per bar:");
        for (int b = 1; b < 8; ++b) { br[b] = brightness(o2.l, (long long)(b * bar + 0.25 * spb), (long long)((b + 1) * bar)); std::printf(" %.4f", br[b]); }
        std::printf("\n");
        check(br[7] < 0.6 * br[2], "Memory 100 %, Wear 70: each pass of the replayed loop is duller (disintegrates)");
    }

    // ==== 7. Print-through: ghosts a beat before and after, with no latency ====
    {
        const int bars = 6;
        std::vector<float> in((size_t)(bars * bar), 0.0f);
        for (int b = 0; b < bars; ++b)                       // one click burst on beat 1 of every bar
            for (int i = 0; i < 400; ++i) in[(size_t)(b * bar + i)] = (float)(0.8 * std::sin(6.2831853 * 1000.0 * i / SR) * std::exp(-i / 120.0));
        LowTide p; p.prepare(SR, 512); isolate(p); p.set(kPrint, 100); p.set(kBars, 0); p.reset();
        Render o = render(p, in, bpm, true);
        const long long b4 = (long long)(4 * bar);
        const double main = rms(o.l, b4, b4 + 400);
        const double post = rms(o.l, b4 + (long long)spb, b4 + (long long)spb + 800);
        const double pre  = rms(o.l, b4 - (long long)spb, b4 - (long long)spb + 800);
        const double gap  = rms(o.l, b4 + (long long)(2.2 * spb), b4 + (long long)(2.2 * spb) + 800);
        std::printf("print-through: main %.1f dBFS, pre-echo %.1f dB, post-echo %.1f dB, between %.1f dB (re main)\n",
                    dB(main), dB(pre / main), dB(post / main), dB(gap / main));
        check(dB(pre / main) > -40.0 && dB(pre / main) < -15.0, "a faint pre-echo a beat before");
        check(dB(post / main) > -40.0 && dB(post / main) < -15.0, "a faint post-echo a beat after");
        check(dB(gap / main) < -60.0, "nothing in between");
        // no latency: the burst comes out where it went in
        long long pk = 0; float pv = 0.0f;
        for (long long i = b4 - 200; i < b4 + 400; ++i) if (std::fabs(o.l[(size_t)i]) > pv) { pv = std::fabs(o.l[(size_t)i]); pk = i; }
        long long pki = 0; float pvi = 0.0f;
        for (long long i = b4 - 200; i < b4 + 400; ++i) if (std::fabs(in[(size_t)i]) > pvi) { pvi = std::fabs(in[(size_t)i]); pki = i; }
        check(std::llabs(pk - pki) <= 2, "no latency (the burst peaks within 2 samples of the input)");
    }

    // ==== 8. Crunch: the grit follows the record pitch ====
    {
        const int len = (int)(1.0 * SR);
        std::vector<float> in((size_t)len);
        for (int i = 0; i < len; ++i) in[(size_t)i] = (float)(0.5 * std::sin(6.2831853 * 1000.0 * i / SR));
        double prev = -1.0; bool rising = true;
        std::printf("crunch (Dirty, 12 bit) residual vs record pitch:");
        for (int st : {0, 7, 12, 19})
        {
            LowTide p; p.prepare(SR, 512); isolate(p); p.set(kCrunch, 50); p.set(kPitch, (float)st); p.set(kFilters, 1); p.reset();
            Render o = render(p, in, bpm, true);
            // project out the 1 kHz fundamental over the second half
            double ss = 0, sc = 0, n = 0;
            for (int i = len / 2; i < len; ++i) { const double a = 6.2831853 * 1000.0 * i / SR; ss += o.l[(size_t)i] * std::sin(a); sc += o.l[(size_t)i] * std::cos(a); ++n; }
            const double A = 2 * ss / n, B = 2 * sc / n;
            double res = 0, tot = 0;
            for (int i = len / 2; i < len; ++i) { const double a = 6.2831853 * 1000.0 * i / SR; const double fit = A * std::sin(a) + B * std::cos(a); res += (o.l[(size_t)i] - fit) * (o.l[(size_t)i] - fit); tot += fit * fit; }
            const double rdb = 10.0 * std::log10(res / tot);
            std::printf("  %d st: %.1f dB", st, rdb);
            if (prev > -1.0 && rdb < prev) rising = false;
            prev = rdb;
        }
        std::printf("\n");
        check(rising, "more record pitch = more crunch (aliasing/imaging residual rises)");
    }

    // ==== 9. CPU, roughly (desktop), the busiest program ====
    {
        std::vector<float> in = makeLoop(bpm, 8);
        LowTide p; p.prepare(SR, 512); p.setProgram(5); p.set(kCrunch, 80); p.set(kMemory, 50); p.reset();
        const auto t0 = std::chrono::steady_clock::now();
        render(p, in, bpm, true);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("CPU (desktop, every stage on): %.2f%% of real time\n", 100.0 * sec / (in.size() / SR));
    }

    // ==== demo: every program, 8 bars each, at 92 BPM ====
    if (argc > 1)
    {
        const double dbpm = 92.0;
        const int barsEach = 8;
        LowTide probe;
        const int np = probe.numPrograms();
        std::vector<float> in = makeLoop(dbpm, barsEach);
        std::vector<float> L, R;
        for (int pr = 0; pr < np; ++pr)
        {
            LowTide p; p.prepare(SR, 512); p.setProgram(pr); p.reset();
            Render o = render(p, in, dbpm, true);
            std::printf("demo %d: %s (%.0f s)\n", pr + 1, p.programName(pr), L.size() / SR);
            L.insert(L.end(), o.l.begin(), o.l.end());
            R.insert(R.end(), o.r.begin(), o.r.end());
        }
        writeWav(argv[1], L, R);
        std::printf("wrote %s (%.0f s)\n", argv[1], L.size() / SR);
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
