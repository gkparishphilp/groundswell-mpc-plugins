// Does Wipeout cut on the grid, and cleanly? A drum loop with a bassline, transport playing, checked sample by sample.
//
//   mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest && tests/build/listentest [out.wav]
//   (run from plugins/Wipeout)
#include <vector>
#include <cstdio>
#include <cmath>
#include <string>
#include <chrono>
#include <algorithm>
#include <functional>

struct EventLog { long long sample; int type; double sliceBeats; bool half, roll, hold; };
struct RepLog   { long long sample; int index; bool rev; double len; };
static std::vector<EventLog> g_events;
static std::vector<RepLog>   g_reps;
static long long g_now = 0;          // sample at the start of the current block
#define WIPEOUT_EVENT_HOOK(off, type, sb, half, roll, hold) g_events.push_back({g_now + (off), (type), (sb), (half), (roll), (hold)})
#define WIPEOUT_REP_HOOK(off, idx, rev, len) g_reps.push_back({g_now + (off), (idx), (rev), (len)})

#define private public
#define protected public
#include "../src/Wipeout.cpp"
#undef private
#undef protected

static const double SR = 44100.0;
static const double BPM = 120.0;
static const double SPB = SR * 60.0 / BPM;     // 22050 samples per beat
static int failures = 0;
static void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

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

// Normalised correlation of a[i0..i0+n) with b at j0 (b index = j0 + k, or j0 - k when reversed).
static double corr(const std::vector<float>& a, long long i0, const std::vector<float>& b, long long j0, int n, bool reversed = false)
{
    double ab = 0, aa = 0, bb = 0;
    for (int k = 0; k < n; ++k)
    {
        const double x = a[(size_t)(i0 + k)], y = b[(size_t)(reversed ? j0 - k : j0 + k)];
        ab += x * y; aa += x * x; bb += y * y;
    }
    return (aa > 0 && bb > 0) ? ab / std::sqrt(aa * bb) : 0.0;
}
static double rms(const std::vector<float>& a, long long i0, long long n)
{
    double s = 0; for (long long k = 0; k < n; ++k) s += (double)a[(size_t)(i0 + k)] * a[(size_t)(i0 + k)];
    return n > 0 ? std::sqrt(s / (double)n) : 0.0;
}

static double maxAbsDiffAfter(const std::vector<float>& a, const std::vector<float>& b, long long from, long long to)
{
    double m = 0.0; for (long long i = from; i < to; ++i) m = std::fmax(m, std::fabs(a[(size_t)i] - b[(size_t)i]));
    return m;
}

// Wipeout parameters, plain values, in order.
//                               Out Chnc Slc Len Mix Rev Half Tape Gate Vary Roll Dcy Hold Grid Off Smth Pat
static void setAll(Wipeout& p, std::vector<float> v) { for (int i = 0; i < (int)v.size(); ++i) p.set(i, v[(size_t)i]); }

int main(int argc, char** argv)
{
    const int bars = 16;
    const int total = (int)(bars * 4 * SPB);
    const double step = SPB / 4.0;
    std::vector<float> in((size_t)total, 0.0f);
    unsigned rs = 7; auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (float)(rs >> 8) / 16777216.0f * 2.0f - 1.0f; };

    // Drum loop at 120 BPM: kick on 1, the "and" of 2, and 3; snare on 2 and 4; hats on 16ths with accents.
    for (int b = 0; b < bars; ++b)
        for (int k = 0; k < 16; ++k)
        {
            const long long t0 = (long long)((b * 16 + k) * step);
            const bool kick = (k == 0 || k == 6 || k == 8), snare = (k == 4 || k == 12);
            if (kick)
            {
                double ph = 0.0;
                for (int i = 0; i < (int)(0.3 * SR) && t0 + i < total; ++i)
                {
                    const double t = i / SR, hz = 50.0 + 120.0 * std::exp(-t * 30.0);
                    ph += hz / SR;
                    in[(size_t)(t0 + i)] += (float)(0.7 * std::sin(6.2831853 * ph) * std::exp(-t * 10.0));
                }
            }
            const double hatLen = snare ? 0.18 : 0.03;
            for (int i = 0; i < (int)(hatLen * SR) && t0 + i < total; ++i)
            {
                const double t = i / SR;
                float v = rnd() * (float)std::exp(-t * (snare ? 20.0 : 120.0)) * (snare ? 0.4f : (k % 4 == 2 ? 0.14f : 0.07f));
                if (snare) v += (float)(0.25 * std::sin(6.2831853 * 185.0 * t) * std::exp(-t * 28.0));
                in[(size_t)(t0 + i)] += v;
            }
        }
    // A saw bassline (A, A, C, G per bar) so repeats, reverses and slow-downs have pitch to hear.
    {
        const int notes[4] = {45, 45, 48, 43};
        double ph = 0.0, lp = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const int beatIdx = (int)(i / SPB);
            const double hz = 440.0 * std::pow(2.0, (notes[beatIdx % 4] - 69) / 12.0);
            ph += hz / SR; ph -= std::floor(ph);
            const double inBeat = std::fmod(i / SPB, 1.0);
            const double env = std::fmin(1.0, std::fmin(inBeat * SPB / 200.0, (1.0 - inBeat) * SPB / 200.0)) * std::exp(-inBeat * 2.0);
            lp += 0.15 * ((2.0 * ph - 1.0) - lp);
            in[(size_t)i] += (float)(0.18 * lp * env);
        }
    }
    float inPk = 0.0f; for (float v : in) inPk = std::fmax(inPk, std::fabs(v));
    for (float& v : in) v *= 0.5f / inPk;                 // peak -6 dBFS

    // A plain sine for the click and pitch checks.
    std::vector<float> sine((size_t)total);
    for (int i = 0; i < total; ++i) sine[(size_t)i] = (float)(0.5 * std::sin(6.2831853 * 330.0 * i / SR));

    using BlockFn = std::function<void(Wipeout&, long long)>;
    using PpqFn = std::function<double(long long)>;
    auto render = [&](Wipeout& p, const std::vector<float>& src, int block, bool playing,
                      std::vector<float>& oL, std::vector<float>& oR, BlockFn perBlock = nullptr, PpqFn ppqAt = nullptr)
    {
        g_events.clear(); g_reps.clear();
        const int len = (int)src.size();
        oL.assign(src.size(), 0.0f); oR.assign(src.size(), 0.0f);
        std::vector<float> l(block), r(block);
        for (int pos = 0; pos < len; pos += block)
        {
            const int n = std::min(block, len - pos);
            if (perBlock) perBlock(p, pos);
            for (int i = 0; i < n; ++i) l[i] = r[i] = src[(size_t)(pos + i)];
            const float* ins[2] = {l.data(), r.data()};
            float* outs[2] = {l.data(), r.data()};
            mpc::ProcessContext ctx;
            ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = n;
            ctx.sampleRate = SR;
            ctx.transport.valid = ctx.transport.tempoValid = true;
            ctx.transport.ppqValid = playing; ctx.transport.playing = playing; ctx.transport.bpm = BPM;
            ctx.transport.ppqPos = playing ? (ppqAt ? ppqAt(pos) : pos / SPB) : 0.0;
            ctx.transport.timeSigNumerator = 4; ctx.transport.timeSigDenominator = 4;
            g_now = pos;
            p.process(ctx);
            for (int i = 0; i < n; ++i) { oL[(size_t)(pos + i)] = l[i]; oR[(size_t)(pos + i)] = r[i]; }
        }
    };
    auto fresh = [&](Wipeout& p, std::vector<float> v) { p.prepare(SR, 512); setAll(p, v); p.reset(); };
    std::vector<float> oL, oR;
    const int F = (int)(0.003 * SR);           // default Smooth: 3 ms

    // ---- 1. Repeats land on the grid and repeat the slice ----
    {
        Wipeout p;
        //        Out Chnc Slc Len Mix Rev Half Tape Gate Vary Roll Dcy Hold Grid Off Smth Pat
        fresh(p, {0,  100, 4,  1,  100, 0,  0,   0,   0,   0,   0,   0,  0,   2,   12, 3,   0});
        render(p, in, 128, true, oL, oR);
        int onGrid = 0; double worstCorr = 1.0; int repsOnGrid = 0, reps = 0;
        const double S = 0.25 * SPB;            // 1/16 = 5512.5 samples
        for (const EventLog& e : g_events)
        {
            const double want = std::round(e.sample / (4 * SPB) - 0.75) * 4 * SPB + 3 * SPB;   // beat 4 of a bar
            if (std::fabs(e.sample - want) <= 1.0 && e.type == kTypeRepeat) ++onGrid;
            for (int k = 1; k < 4; ++k)
            {
                const long long rs0 = e.sample + (long long)std::ceil(k * S);
                for (const RepLog& r : g_reps) if (r.index == k && std::llabs(r.sample - rs0) <= 1) { ++repsOnGrid; break; }
                ++reps;
                // the repeat's middle should be the captured slice's middle
                worstCorr = std::fmin(worstCorr, corr(oL, rs0 + F + 4, in, e.sample + F + 4, (int)(S - 2 * F - 8)));
            }
        }
        std::printf("grid 1 bar, offset beat 4, 1/16 x 4: %zu events, %d on beat 4 (+/-1 sample), %d/%d repeats on the 1/16 grid, worst repeat-vs-slice correlation %.4f\n",
                    g_events.size(), onGrid, repsOnGrid, reps, worstCorr);
        check((int)g_events.size() == bars && onGrid == bars, "one event per bar, exactly on beat 4");
        check(repsOnGrid == reps, "every repeat starts on the 1/16 grid");
        check(worstCorr > 0.98, "repeats replay the captured slice");
        // the dry signal is untouched outside events (beats 1-3 of each bar)
        double maxDiff = 0.0;
        for (int b = 0; b < bars; ++b)
            for (long long i = (long long)(b * 4 * SPB) + F; i < (long long)((b * 4 + 3) * SPB) - 1; ++i)
                maxDiff = std::fmax(maxDiff, std::fabs(oL[(size_t)i] - in[(size_t)i]));
        std::printf("outside events, max |out - in| = %.2e\n", maxDiff);
        check(maxDiff < 1e-6, "dry signal passes untouched between events");
    }

    // ---- 2. Reverse ----
    {
        Wipeout p;
        fresh(p, {0, 100, 2, 2, 100, 100, 0, 0, 0, 0, 0, 0, 0, 2, 8, 3, 0});   // 1/8 slices over a half note from beat 3
        render(p, in, 128, true, oL, oR);
        const double S = 0.5 * SPB;
        int revCount = 0, revTotal = 0; double worst = 1.0;
        for (const EventLog& e : g_events)
            for (int k = 1; k < 4; ++k)
            {
                const long long rs0 = e.sample + (long long)std::ceil(k * S);
                for (const RepLog& r : g_reps) if (r.index == k && std::llabs(r.sample - rs0) <= 1) { ++revTotal; if (r.rev) ++revCount; break; }
                // reversed: out[rs0 + j] ~ in[start + S - 1 - j]
                worst = std::fmin(worst, corr(oL, rs0 + F + 4, in, e.sample + (long long)S - 1 - F - 4, (int)(S - 2 * F - 8), true));
            }
        std::printf("reverse 100%%: %d/%d repeats reversed, worst reversed correlation %.4f\n", revCount, revTotal, worst);
        check(revTotal > 0 && revCount == revTotal, "Reverse 100%: every repeat after the first is reversed");
        check(worst > 0.98, "reversed repeats are the slice played backwards");
    }

    // ---- 3. Half speed ----
    {
        Wipeout p;
        fresh(p, {0, 100, 2, 2, 100, 0, 100, 0, 0, 0, 0, 0, 0, 2, 8, 3, 0});
        render(p, in, 128, true, oL, oR);
        const double S = 0.5 * SPB;
        int halves = 0; double worst = 1.0;
        for (const EventLog& e : g_events)
        {
            if (e.half) ++halves;
            for (int k = 1; k < 4; ++k)
            {
                const long long rs0 = e.sample + (long long)std::ceil(k * S);
                // out[rs0 + j] ~ in[start + j/2]: compare against a 2x-stretched copy of the slice
                std::vector<float> st((size_t)S);
                for (int j = 0; j < (int)S; ++j) { const double x = j * 0.5; const int i0 = (int)x; const float fr = (float)(x - i0);
                    st[(size_t)j] = in[(size_t)(e.sample + i0)] + (in[(size_t)(e.sample + i0 + 1)] - in[(size_t)(e.sample + i0)]) * fr; }
                worst = std::fmin(worst, corr(oL, rs0 + F + 4, st, F + 4, (int)(S - 2 * F - 8)));
            }
        }
        std::printf("half speed 100%%: %d/%zu events at half speed, worst correlation with the slice stretched 2x %.4f\n", halves, g_events.size(), worst);
        check(halves == (int)g_events.size() && worst > 0.98, "Half Speed plays half the slice an octave down, on the same grid");
    }

    // ---- 4. Tape stop: pitch sinks, then silence, then the dry track is back on the next beat ----
    {
        Wipeout p;
        fresh(p, {0, 100, 4, 2, 100, 0, 0, 100, 0, 0, 0, 0, 0, 2, 8, 3, 0});   // half-note tape stop from beat 3
        render(p, sine, 128, true, oL, oR);
        const long long L = (long long)(2 * SPB);
        auto zcrHz = [&](long long a, long long n) { int z = 0; for (long long i = a + 1; i < a + n; ++i) if ((oL[(size_t)i - 1] < 0) != (oL[(size_t)i] < 0)) ++z; return z * 0.5 * SR / (double)n; };
        bool allTape = true; double worstMid = 0, worstEnd = 0, worstBack = 0;
        for (const EventLog& e : g_events)
        {
            if (e.type != kTypeTape) allTape = false;
            const double startHz = zcrHz(e.sample + 200, 2205);
            const double midHz = zcrHz(e.sample + L / 2 - 1102, 2205);    // speed 0.5 here
            worstMid = std::fmax(worstMid, std::fabs(midHz / 165.0 - 1.0));
            (void)startHz;
            worstEnd = std::fmax(worstEnd, rms(oL, e.sample + L - L / 25, L / 25) / rms(sine, e.sample, L));
            const long long after = e.sample + L + 2 * F;
            if (after + 2000 < total) worstBack = std::fmax(worstBack, (double)std::fabs(corr(oL, after, sine, after, 2000) - 1.0));
        }
        std::printf("tape stop: %zu events, pitch at mid-stop off from 165 Hz by %.1f%% at worst, last 4%% at %.1f dB, dry back after (corr err %.1e)\n",
                    g_events.size(), worstMid * 100.0, 20.0 * std::log10(worstEnd + 1e-9), worstBack);
        check(allTape && !g_events.empty(), "Tape Stop 100%: every event is a tape stop");
        check(worstMid < 0.06, "tape stop: half speed (an octave down) halfway through");
        check(worstEnd < 0.1, "tape stop: stopped (below -20 dB) at the end");
        check(worstBack < 1e-4, "tape stop: the dry track is back after the event");
    }

    // ---- 5. Gate rolls ----
    {
        Wipeout p;
        fresh(p, {0, 100, 4, 1, 100, 0, 0, 0, 100, 0, 0, 0, 0, 2, 12, 3, 0});
        render(p, in, 128, true, oL, oR);
        const double S = 0.25 * SPB;
        double worstOff = 0, worstOn = 1;
        for (const EventLog& e : g_events)
            for (int k = 0; k < 4; ++k)
            {
                const long long a = e.sample + (long long)std::ceil(k * S);
                worstOn = std::fmin(worstOn, corr(oL, a + F + 4, in, a + F + 4, (int)(S / 2 - 2 * F - 8)));
                worstOff = std::fmax(worstOff, rms(oL, a + (long long)(S / 2) + F + 2, (long long)(S / 2) - 2 * F - 4));
            }
        std::printf("gate 100%%: on-halves correlate with the dry %.4f at worst, off-halves at %.1f dBFS at worst\n", worstOn, 20 * std::log10(worstOff + 1e-12));
        check(worstOn > 0.999 && worstOff < 1e-5, "Gate: dry on the first half of each 1/16, silent on the second");
    }

    // ---- 6. Roll: 1/8 then 1/16 then 1/32 across a one-bar event ----
    {
        Wipeout p;
        fresh(p, {0, 100, 2, 3, 100, 0, 0, 0, 0, 0, 100, 0, 0, 3, 0, 3, 0});   // grid 2 bars, 1-bar events, 1/8 slices
        render(p, in, 128, true, oL, oR);
        int ok = 0, n = 0;
        for (const RepLog& r : g_reps)
        {
            const EventLog* e = nullptr;
            for (const EventLog& ev : g_events) if (r.sample >= ev.sample && r.sample < ev.sample + (long long)(4 * SPB)) e = &ev;
            if (!e) continue;
            const double beat = (r.sample - e->sample) / SPB;
            const double want = (beat < 4.0 / 3.0 - 0.01 ? 0.5 : beat < 8.0 / 3.0 - 0.01 ? 0.25 : 0.125) * SPB;
            ++n; if (std::fabs(r.len - want) < 1.0) ++ok;
            // and still on the 1/32 grid
            const double g = (r.sample - e->sample) / (0.125 * SPB);
            if (std::fabs(g - std::round(g)) * 0.125 * SPB > 1.01) --ok;
        }
        std::printf("roll: %d/%d repeats have the right length (1/8 -> 1/16 -> 1/32) and sit on the grid\n", ok, n);
        check(n > 0 && ok == n, "Roll halves the slice each third of the event");
    }

    // ---- 7. Clicks: a sine through everything at once ----
    double maxDin = 0.0;
    for (int i = 1; i < total; ++i) maxDin = std::fmax(maxDin, std::fabs(sine[(size_t)i] - sine[(size_t)i - 1]));
    {
        Wipeout p;
        fresh(p, {0, 80, 4, 1, 100, 50, 30, 20, 25, 60, 40, 20, 0, 0, 0, 3, 0});
        render(p, sine, 128, true, oL, oR);
        double maxD = 0.0; int over = 0;
        for (int i = 1; i < total; ++i) { const double d = std::fabs(oL[(size_t)i] - oL[(size_t)i - 1]); maxD = std::fmax(maxD, d); if (d > 2.0 * maxDin) ++over; }
        int types[3] = {0, 0, 0}; for (const EventLog& e : g_events) ++types[e.type];
        // control: the same slices cut hard (no fades) would jump by this much
        double hard = 0.0;
        for (const EventLog& e : g_events)
            for (int k = 1; k < 4 && e.type == kTypeRepeat; ++k)
            {
                const long long a = e.sample + (long long)std::ceil(k * e.sliceBeats * SPB);
                if (a < total) hard = std::fmax(hard, std::fabs(sine[(size_t)e.sample] - sine[(size_t)a - 1]));
            }
        std::printf("clicks: %zu events (%d repeat, %d tape, %d gate), max step %.4f vs the sine's own %.4f (%.2fx); a hard cut would step up to %.3f\n",
                    g_events.size(), types[0], types[1], types[2], maxD, maxDin, maxD / maxDin, hard);
        check(types[0] > 10 && types[1] > 0 && types[2] > 0, "click test exercised repeats, tape stops and gates");
        check(over == 0, "no sample step larger than 2x the sine's own: slice and event edges are faded");
    }
    {
        Wipeout p;   // the same with Hold toggled every 3/4 beat on top (overlapping events, slot hand-over)
        fresh(p, {0, 80, 5, 0, 100, 50, 30, 20, 25, 60, 40, 0, 0, 0, 0, 3, 0});
        render(p, sine, 64, true, oL, oR, [&](Wipeout& q, long long pos) { q.set(kHold, std::fmod(pos / SPB, 1.5) < 0.75 ? 1.0f : 0.0f); });
        double maxD = 0.0;
        for (int i = 1; i < total; ++i) maxD = std::fmax(maxD, std::fabs(oL[(size_t)i] - oL[(size_t)i - 1]));
        std::printf("clicks with Hold toggling: max step %.4f (%.2fx the sine's own)\n", maxD, maxD / maxDin);
        check(maxD <= 2.0 * maxDin, "no clicks when Hold interrupts events");
    }

    // ---- 8. Hold: starts on the next slice boundary, stops when released ----
    {
        Wipeout p;
        fresh(p, {0, 0, 4, 1, 100, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 3, 0});
        const long long onAt = 128 * 700, offAt = 128 * 1100;    // beat 4.063 and 6.385
        render(p, in, 128, true, oL, oR, [&](Wipeout& q, long long pos) { q.set(kHold, (pos >= onAt && pos < offAt) ? 1.0f : 0.0f); });
        const long long want = (long long)std::ceil(4.25 * SPB);
        const bool started = g_events.size() == 1 && g_events[0].hold && std::llabs(g_events[0].sample - want) <= 1;
        const double back = maxAbsDiffAfter(oL, in, offAt + F + 2, total);
        std::printf("hold: switched on at beat %.3f, started at beat %.4f; after release max |out - in| = %.2e\n",
                    onAt / SPB, g_events.empty() ? -1.0 : g_events[0].sample / SPB, back);
        check(started, "Hold starts a repeat on the next 1/16");
        check(back < 1e-6, "Hold off returns to the dry track");
    }

    // ---- 9. Pattern lock replays the last bar ----
    {
        Wipeout p;
        fresh(p, {0, 50, 4, 0, 100, 30, 20, 15, 15, 40, 20, 0, 0, 0, 0, 3, 0});
        const long long lockAt = (long long)(4.5 * 4 * SPB) / 128 * 128;   // in the middle of bar 5 (index 4)
        render(p, in, 128, true, oL, oR, [&](Wipeout& q, long long pos) { q.set(kPattern, pos >= lockAt ? 1.0f : 0.0f); });
        auto barSig = [&](int bar) {
            std::string s;
            for (const EventLog& e : g_events)
            {
                const long long a = (long long)(bar * 4 * SPB), b = (long long)((bar + 1) * 4 * SPB);
                if (e.sample >= a && e.sample < b) { char buf[64]; std::snprintf(buf, sizeof buf, "%lld:%d:%.3f:%d%d|", e.sample - a, e.type, e.sliceBeats, e.half, e.roll); s += buf; }
            }
            return s;
        };
        const std::string ref = barSig(4);
        int same = 0; for (int b = 5; b < bars; ++b) if (barSig(b) == ref) ++same;
        int differ = 0; for (int b = 1; b < 4; ++b) if (barSig(b) != barSig(b - 1)) ++differ;
        std::printf("pattern: free bars differ from each other %d/3 times; after locking, %d/%d bars repeat bar 5 (%s)\n", differ, same, bars - 5, ref.c_str());
        check(!ref.empty() && same == bars - 5, "Pattern 1 Bar replays the last bar's dice exactly");
        check(differ >= 2, "Pattern Free gives a new pattern each bar");
    }

    // ---- 9b. Pattern 8 Bars replays all eight ----
    {
        Wipeout p;
        fresh(p, {0, 50, 4, 0, 100, 30, 20, 15, 15, 40, 20, 0, 0, 0, 0, 3, 0});
        const long long lockAt = (long long)(7.5 * 4 * SPB) / 128 * 128;   // during bar 8
        render(p, in, 128, true, oL, oR, [&](Wipeout& q, long long pos) { q.set(kPattern, pos >= lockAt ? 4.0f : 0.0f); });
        auto barSig = [&](int bar) {
            std::string s;
            for (const EventLog& e : g_events)
            {
                const long long a = (long long)(bar * 4 * SPB), b = (long long)((bar + 1) * 4 * SPB);
                if (e.sample >= a && e.sample < b) { char buf[64]; std::snprintf(buf, sizeof buf, "%lld:%d:%.3f:%d%d|", e.sample - a, e.type, e.sliceBeats, e.half, e.roll); s += buf; }
            }
            return s;
        };
        int same = 0, nonEmpty = 0;
        for (int b = 0; b < 8; ++b) { if (barSig(b + 8) == barSig(b)) ++same; if (!barSig(b).empty()) ++nonEmpty; }
        std::printf("pattern 8 bars: bars 9-16 repeat bars 1-8 in %d/8 bars (%d of them have events)\n", same, nonEmpty);
        check(same == 8 && nonEmpty >= 6, "Pattern 8 Bars replays the last eight bars' dice exactly");
    }

    // ---- 10. Transport stopped: free clock that restarts on the first hit ----
    {
        Wipeout p;
        fresh(p, {0, 100, 4, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0});   // every beat
        const long long hitAt = 51234;                                           // not on any grid
        std::vector<float> late((size_t)total, 0.0f);
        for (long long i = hitAt; i < total; ++i) late[(size_t)i] = in[(size_t)(i - hitAt)];
        render(p, late, 128, false, oL, oR);
        int onClock = 0, after = 0;
        for (const EventLog& e : g_events)
        {
            if (e.sample < hitAt - 2) continue;
            ++after;
            const double b = (e.sample - hitAt) / SPB;
            if (std::fabs(b - std::round(b)) * SPB < 0.003 * SR) ++onClock;
        }
        std::printf("stopped: %d events after the first hit, %d on beats counted from that hit (within 3 ms)\n", after, onClock);
        check(after > 20 && onClock == after, "transport stopped: the grid starts on the first hit, at the host tempo");
    }

    // ---- 11. A loop jump in the transport resyncs to the new position ----
    {
        Wipeout p;
        fresh(p, {0, 100, 4, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 3, 0});   // every half note
        const long long loopLen = (long long)(3.25 * SPB) / 128 * 128;          // the sequence loops at an odd place
        render(p, in, 128, true, oL, oR, nullptr, [&](long long pos) { return (pos % loopLen) / SPB; });
        int ok = 0;
        for (const EventLog& e : g_events)
        {
            const double b = (e.sample % loopLen) / SPB;
            if (std::fabs(b / 2.0 - std::round(b / 2.0)) * 2.0 * SPB <= 1.0) ++ok;
        }
        std::printf("loop jump every %.3f beats: %d/%zu events on the half-note grid of the song position\n", loopLen / SPB, ok, g_events.size());
        check(!g_events.empty() && ok == (int)g_events.size(), "events follow the song position through loop jumps");
    }

    // ---- 12. Transitions: an 8-bar grid, a 2-bar roll into the next phrase (the "Transitions" program) ----
    {
        Wipeout p; p.prepare(SR, 512); setAll(p, p.programs_[7].plainValues); p.reset();
        render(p, in, 128, true, oL, oR);
        const bool placed = g_events.size() == 2 && std::llabs(g_events[0].sample - (long long)(24 * SPB)) <= 1
                            && std::llabs(g_events[1].sample - (long long)(56 * SPB)) <= 1;
        int ok = 0, n = 0;
        for (const RepLog& r : g_reps)
        {
            if (r.sample < (long long)(24 * SPB) || r.sample >= (long long)(32 * SPB)) continue;
            const double beat = (r.sample - 24 * SPB) / SPB;
            const double want = (beat < 8.0 / 3.0 - 0.01 ? 0.5 : beat < 16.0 / 3.0 - 0.01 ? 0.25 : 0.125) * SPB;
            ++n; if (std::fabs(r.len - want) < 1.0) ++ok;
        }
        const double back = maxAbsDiffAfter(oL, in, (long long)(32 * SPB) + F + 2, (long long)(56 * SPB) - 1);
        std::printf("transitions (program \"%s\"): events at beats %.3f and %.3f (want 24 and 56); %d/%d repeats 1/8 -> 1/16 -> 1/32; dry between: max |out - in| %.1e\n",
                    p.programs_[7].name.c_str(), g_events.empty() ? -1.0 : g_events[0].sample / SPB,
                    g_events.size() < 2 ? -1.0 : g_events[1].sample / SPB, ok, n, back);
        check(placed, "Grid 8 Bars + Offset 7.1.1: a 2-bar event on the last two bars of each 8");
        check(n > 20 && ok == n, "a 2-bar roll: 1/8 for its first third, then 1/16, then 1/32");
        check(back < 1e-6, "the dry track is untouched for the rest of the phrase");
    }

    // ---- 13. A 4-bar tape stop (Length 4 Bars) still slows evenly and stops ----
    {
        Wipeout p;
        fresh(p, {0, 100, 4, 5, 100, 0, 0, 100, 0, 0, 0, 0, 0, 5, 64, 3, 0});   // from bar 5, every 8 bars
        render(p, sine, 128, true, oL, oR);
        const long long L = (long long)(16 * SPB), s0 = g_events.empty() ? 0 : g_events[0].sample;
        auto zcrHz = [&](long long a, long long n) { int z = 0; for (long long i = a + 1; i < a + n; ++i) if ((oL[(size_t)i - 1] < 0) != (oL[(size_t)i] < 0)) ++z; return z * 0.5 * SR / (double)n; };
        const double q1 = zcrHz(s0 + L / 4 - 2205, 4410), mid = zcrHz(s0 + L / 2 - 2205, 4410);
        const double endLevel = rms(oL, s0 + L - L / 25, L / 25) / rms(sine, s0, L);
        const double back = maxAbsDiffAfter(oL, sine, s0 + L + F + 2, std::min<long long>(total, s0 + L + (long long)SPB));
        std::printf("4-bar tape stop at beat %.3f: %.0f Hz a quarter in (want 248), %.0f Hz halfway (want 165), last 4%% at %.1f dB, dry after %.1e\n",
                    s0 / SPB, q1, mid, 20.0 * std::log10(endLevel + 1e-9), back);
        check(g_events.size() == 2 && std::llabs(s0 - (long long)(16 * SPB)) <= 1, "Length 4 Bars: the tape stop starts on bar 5");
        check(std::fabs(q1 / 247.5 - 1.0) < 0.06 && std::fabs(mid / 165.0 - 1.0) < 0.06, "the 4-bar stop slows evenly (3/4 speed, then 1/2)");
        check(endLevel < 0.1 && back < 1e-6, "it ends stopped, and the track is back afterwards");
    }

    // ---- CPU, roughly ----
    {
        Wipeout p;
        fresh(p, {0, 80, 4, 1, 100, 50, 30, 20, 25, 60, 40, 20, 0, 0, 0, 3, 0});
        const auto t0 = std::chrono::steady_clock::now();
        render(p, in, 128, true, oL, oR);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("CPU (desktop, busy settings): %.3f%% of real time\n", 100.0 * sec / (total / SR));
    }

    // ---- the demo render: the programs, four bars each ----
    if (argc > 1)
    {
        Wipeout p; p.prepare(SR, 512); p.reset();
        const int progs[4] = {0, 3, 2, 1};      // Bar Fills, Snare Rolls, Tape Breaks, Glitch Storm
        std::vector<float> dL, dR;
        render(p, in, 128, true, dL, dR, [&](Wipeout& q, long long pos) {
            const int want = progs[(int)(pos / (16 * SPB)) % 4];
            static int last = -1;
            if (want != last) { setAll(q, q.programs_[(size_t)want].plainValues); last = want; }
        });
        writeWav(argv[1], dL, dR);
        std::printf("wrote %s (bars 1-4 Bar Fills, 5-8 Snare Rolls, 9-12 Tape Breaks, 13-16 Glitch Storm)\n", argv[1]);
    }
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
