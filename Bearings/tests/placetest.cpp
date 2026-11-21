// placetest - does Bearings put the sound where it says, and move it the way it says?
//
// Includes the plugin source with `private` defined away, so it can read where each emitter is
// instead of guessing from the audio, then checks the audio as well: levels, interaural delay,
// distance cues, a flat band split, song-locked paths, hits, Doppler limits and every program.
//
//   clang++ -std=c++17 -O2 -I../../../framework -o build/placetest placetest.cpp && build/placetest

#define private public
#include "../src/Bearings.cpp"
#undef private

#include "dsp/noise.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
static void check(bool ok, const std::string& what)
{
    ++checks;
    std::printf("    %s  %s\n", ok ? "OK  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
static std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0)
{
    char buf[256]; std::snprintf(buf, sizeof(buf), f, a, b, c); return buf;
}

constexpr double kSr = 44100.0;

struct Rig
{
    std::unique_ptr<Bearings> p{ new Bearings() };
    double ppq = 0.0, bpm = 120.0;
    bool playing = false;
    int block = 128;

    Rig() { p->prepare(kSr, 512); }
    void set(int i, float v) { p->set(i, v); }
    void neutral()   // centred, still, dry room, no Doppler
    {
        set(kPath, kFixed); set(kPan, 0); set(kDistance, 0); set(kRoom, 0); set(kDoppler, 0);
        set(kGlide, 5); set(kMix, 100); set(kOutput, 0);
        p->reset();
    }
    // Run `in` (mono, sent to both inputs) through the plugin; returns L and R.
    void run(const std::vector<float>& in, std::vector<float>& L, std::vector<float>& R)
    {
        const size_t n = in.size();
        L.assign(n, 0.0f); R.assign(n, 0.0f);
        std::vector<float> a(512), b(512);
        for (size_t pos = 0; pos < n; pos += (size_t)block)
        {
            const int frames = (int)std::min((size_t)block, n - pos);
            for (int i = 0; i < frames; ++i) a[(size_t)i] = b[(size_t)i] = in[pos + (size_t)i];
            const float* ins[2] = { a.data(), b.data() };
            float* outs[2] = { a.data(), b.data() };   // in place, like the MPC
            mpc::ProcessContext ctx;
            ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2;
            ctx.numFrames = frames; ctx.sampleRate = kSr;
            ctx.transport.valid = true; ctx.transport.tempoValid = true; ctx.transport.bpm = bpm;
            ctx.transport.playing = playing; ctx.transport.ppqValid = playing; ctx.transport.ppqPos = ppq;
            p->process(ctx);
            for (int i = 0; i < frames; ++i) { L[pos + (size_t)i] = a[(size_t)i]; R[pos + (size_t)i] = b[(size_t)i]; }
            if (playing) ppq += frames / kSr * bpm / 60.0;
        }
    }
};

static std::vector<float> sine(double hz, double secs, float amp = 0.5f)
{
    std::vector<float> v((size_t)(secs * kSr));
    for (size_t i = 0; i < v.size(); ++i) v[i] = amp * (float)std::sin(2.0 * M_PI * hz * (double)i / kSr);
    return v;
}
static std::vector<float> noise(double secs, float amp = 0.3f, uint32_t seed = 7)
{
    Xorshift32 r(seed);
    std::vector<float> v((size_t)(secs * kSr));
    for (float& x : v) x = amp * r.nextBipolar();
    return v;
}
static double rmsDb(const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0.0; to = std::min(to, v.size());
    for (size_t i = from; i < to; ++i) s += (double)v[i] * v[i];
    return 10.0 * std::log10(s / (double)(to - from) + 1e-30);
}
// Lag (in samples) at which R best matches L: positive = R is later.
static int bestLag(const std::vector<float>& L, const std::vector<float>& R, size_t from, size_t to, int maxLag)
{
    int best = 0; double bestV = -1e30;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double s = 0.0;
        for (size_t i = from; i < to; ++i) s += (double)L[i] * R[(size_t)((long)i + lag)];
        if (s > bestV) { bestV = s; best = lag; }
    }
    return best;
}
// Energy in a band (2nd-order band-pass, Q 4), dB.
static double bandDb(const std::vector<float>& v, double hz, size_t from)
{
    Biquad bp; bp.setCoeffs(BiquadCoeffs::bandpass(kSr, (float)hz, 4.0f));
    double s = 0.0;
    for (size_t i = 0; i < v.size(); ++i) { const float y = bp.process(v[i]); if (i >= from) s += (double)y * y; }
    return 10.0 * std::log10(s + 1e-30);
}

int main()
{
    std::vector<float> L, R;

    std::printf("Centre: a centred mono source comes out at the level it went in\n");
    {
        Rig r; r.neutral();
        const auto in = sine(1000.0, 1.0);
        r.run(in, L, R);
        const double inDb = rmsDb(in, 22050, 44100), lDb = rmsDb(L, 22050, 44100), rDb = rmsDb(R, 22050, 44100);
        check(std::fabs(lDb - inDb) < 0.3 && std::fabs(rDb - inDb) < 0.3,
              fmt("L %+.2f dB, R %+.2f dB against the input", lDb - inDb, rDb - inDb));
    }

    std::printf("Pan: hard left is louder on the left, and the right ear hears it later\n");
    {
        Rig r; r.neutral(); r.set(kPan, -100); r.p->reset();
        const auto in = noise(1.0);
        r.run(in, L, R);
        const double diff = rmsDb(L, 22050, 44100) - rmsDb(R, 22050, 44100);
        check(diff > 10.0 && diff < 20.0, fmt("left is %.1f dB louder than right", diff));
        const int lag = bestLag(L, R, 22050, 40000, 60);
        const int want = (int)std::lround(kItdSeconds * kSr);
        check(std::abs(lag - want) <= 1, fmt("right ear %.0f samples later (head model: %.0f)", lag, want));

        Rig m; m.neutral(); m.set(kPan, -100); m.set(kMonoSafe, 1); m.p->reset();
        m.run(in, L, R);
        check(bestLag(L, R, 22050, 40000, 60) == 0, "Mono Safe: no interaural delay");

        Rig q; q.neutral(); q.set(kPan, 100); q.p->reset();
        q.run(in, L, R);
        const double d2 = rmsDb(R, 22050, 44100) - rmsDb(L, 22050, 44100);
        check(std::fabs(d2 - diff) < 0.2, fmt("hard right mirrors it (%.1f dB)", d2));
    }

    std::printf("Distance: far away is quieter and duller, and wetter when there is a room\n");
    {
        const auto in = noise(1.0);
        Rig nearR; nearR.neutral(); nearR.set(kDepth, 100); nearR.p->reset();
        nearR.run(in, L, R);
        const double nearDb = rmsDb(L, 22050, 44100), nearHi = bandDb(L, 8000.0, 22050), nearLo = bandDb(L, 300.0, 22050);
        Rig farR; farR.neutral(); farR.set(kDepth, 100); farR.set(kDistance, 100); farR.p->reset();
        farR.run(in, L, R);
        const double farDb = rmsDb(L, 22050, 44100), farHi = bandDb(L, 8000.0, 22050), farLo = bandDb(L, 300.0, 22050);
        check(nearDb - farDb > 15.0, fmt("%.1f dB quieter far away at Depth 100", nearDb - farDb));
        check((nearHi - farHi) - (nearLo - farLo) > 8.0,
              fmt("the highs fall %.1f dB more than the lows", (nearHi - farHi) - (nearLo - farLo)));

        Rig d0; d0.neutral(); d0.set(kDepth, 0); d0.set(kDistance, 100); d0.p->reset();
        d0.run(in, L, R);
        check(std::fabs(rmsDb(L, 22050, 44100) - nearDb) < 0.5, "Depth 0: distance leaves the level alone");

        // Direct-to-room ratio: the room's share of the output, near and far.
        auto roomShare = [&](float dist)
        {
            Rig dry; dry.neutral(); dry.set(kDistance, dist); dry.p->reset(); dry.run(in, L, R);
            const double dryDb = rmsDb(L, 22050, 44100);
            Rig wet; wet.neutral(); wet.set(kDistance, dist); wet.set(kRoom, 60); wet.p->reset(); wet.run(in, L, R);
            return rmsDb(L, 22050, 44100) - dryDb;
        };
        const double nearRoom = roomShare(0), farRoom = roomShare(100);
        check(farRoom > nearRoom + 4.0 && nearRoom > 0.5, fmt("Room adds %.1f dB near, %.1f dB far", nearRoom, farRoom));
        check(nearRoom < 3.0, "near, the room stays a background");
    }

    std::printf("Split: with nothing moving, three bands add back up flat\n");
    {
        const auto in = noise(1.5);
        Rig a; a.neutral(); a.run(in, L, R);
        std::vector<float> whole = L;
        Rig b; b.neutral(); b.set(kSplit, 1); b.p->reset(); b.run(in, L, R);
        double worst = 0.0, worstHz = 0.0;
        for (double hz : { 60.0, 120.0, 200.0, 350.0, 700.0, 1400.0, 2500.0, 4000.0, 8000.0, 14000.0 })
        {
            const double d = bandDb(L, hz, 22050) - bandDb(whole, hz, 22050);
            if (std::fabs(d) > std::fabs(worst)) { worst = d; worstHz = hz; }
        }
        check(std::fabs(worst) < 0.5, fmt("largest difference %+.2f dB at %.0f Hz", worst, worstHz));
    }

    std::printf("Paths: a synced circle is where the song position says\n");
    {
        Rig r; r.neutral(); r.set(kPath, kCircle); r.set(kRate, syncPos(4)); r.set(kSizeX, 100); r.set(kSizeY, 0);
        r.p->reset(); r.playing = true; r.ppq = 0.0;
        // 1 bar = 4 beats: a quarter of the way round (ppq 1) is hard right, ppq 3 hard left.
        auto xAt = [&](Rig& g, double ppq)
        {
            const double secs = (ppq - g.ppq) * 60.0 / g.bpm;
            g.run(std::vector<float>((size_t)(secs * kSr), 0.0f), L, R);
            return g.p->em_[0].x;
        };
        const float x1 = xAt(r, 1.0), x2 = xAt(r, 2.0), x3 = xAt(r, 3.0);
        check(x1 > 0.97f, fmt("beat 2: x = %+.2f (hard right)", x1));
        check(std::fabs(x2) < 0.05f, fmt("beat 3: x = %+.2f (centre, behind)", x2));
        check(x3 < -0.97f, fmt("beat 4: x = %+.2f (hard left)", x3));

        Rig c; c.neutral(); c.set(kPath, kCircle); c.set(kRate, syncPos(4)); c.set(kSizeX, 100); c.set(kDirection, 1);
        c.p->reset(); c.playing = true;
        check(xAt(c, 1.0) < -0.97f, "Counter goes left first");

        // Formation: started two bars late, an instance is in the same place on the same beat; Phase 120 is not.
        Rig late; late.neutral(); late.set(kPath, kCircle); late.set(kRate, syncPos(4)); late.set(kSizeX, 100);
        late.p->reset(); late.playing = true; late.ppq = 8.0;
        const float xl = xAt(late, 9.0);
        check(std::fabs(xl - x1) < 0.02f, fmt("started late, beat 2 is still x = %+.2f", xl));
        Rig ph; ph.neutral(); ph.set(kPath, kCircle); ph.set(kRate, syncPos(4)); ph.set(kSizeX, 100); ph.set(kPhase, 120);
        ph.p->reset(); ph.playing = true;
        const float xp = xAt(ph, 1.0), wantP = (float)std::sin(2.0 * M_PI * (0.25 + 120.0 / 360.0));
        check(std::fabs(xp - wantP) < 0.03f, fmt("Phase 120: x = %+.2f (expected %+.2f)", xp, wantP));

        // Block size does not move it.
        Rig s32; s32.neutral(); s32.set(kPath, kCircle); s32.set(kRate, syncPos(4)); s32.set(kSizeX, 100);
        s32.p->reset(); s32.playing = true; s32.block = 32;
        const float xb = xAt(s32, 1.3);
        Rig s512; s512.neutral(); s512.set(kPath, kCircle); s512.set(kRate, syncPos(4)); s512.set(kSizeX, 100);
        s512.p->reset(); s512.playing = true; s512.block = 512;
        const float xc = xAt(s512, 1.3);
        check(std::fabs(xb - xc) < 0.02f, fmt("blocks of 32 and 512 agree (%+.3f, %+.3f)", xb, xc));

        // Tide only moves in depth.
        Rig t; t.neutral(); t.set(kPath, kTide); t.set(kRate, syncPos(4)); t.set(kSizeY, 100); t.set(kDistance, 50);
        t.p->reset(); t.playing = true;
        xAt(t, 2.0);
        check(std::fabs(t.p->em_[0].x) < 1e-4f && t.p->em_[0].y > 0.97f,
              fmt("Tide at half a bar: x = %+.2f, y = %.2f (far)", t.p->em_[0].x, t.p->em_[0].y));
    }

    std::printf("Display: the globe's dot meters light one row, at the right column\n");
    {
        Rig r; r.neutral(); r.set(kPath, kCircle); r.set(kRate, syncPos(4)); r.set(kSizeX, 100); r.set(kSizeY, 0);
        r.set(kDistance, 30); r.p->reset(); r.playing = true;
        r.run(std::vector<float>((size_t)(0.5 * kSr), 0.0f), L, R);   // beat 2 of a 1-bar circle: hard right
        int lit = 0, litRow = -1;
        for (int k = 0; k < kDotRows; ++k) if (r.p->get(kDot0 + k) > 0.0f) { ++lit; litRow = k; }
        check(lit == 1 && litRow == 2, fmt("one row lit (%.0f), row %.0f for Distance 30 %%", lit, litRow));
        check(std::fabs(r.p->get(kDot0 + 2) - 1.0f) < 1e-4f, fmt("hard right is the last column (%.3f)", r.p->get(kDot0 + 2)));
        r.run(std::vector<float>((size_t)(1.0 * kSr), 0.0f), L, R);   // beat 4: hard left
        check(std::fabs(r.p->get(kDot0 + 2) - 1.0f / kDotCols) < 1e-4f, "hard left is the first column");
    }

    std::printf("Hits: Step moves a set angle per hit, Scatter jumps somewhere new\n");
    {
        // Clicks every 250 ms.
        std::vector<float> clicks((size_t)(2.0 * kSr), 0.0f);
        for (int k = 0; k < 8; ++k)
            for (int i = 0; i < 200; ++i)
                clicks[(size_t)(k * 11025 + 2000 + i)] = 0.8f * (float)std::exp(-i / 30.0) * ((i & 1) ? 1.0f : -1.0f);
        Rig s; s.neutral(); s.set(kPath, kStep); s.set(kStepAngle, 90); s.set(kSizeX, 100); s.set(kSizeY, 0);
        s.p->reset();
        s.run(std::vector<float>(clicks.begin(), clicks.begin() + 11025 + 6000), L, R);   // two clicks, settled
        check(s.p->hits_ == 2, fmt("2 clicks counted as %.0f hits", (double)s.p->hits_));
        check(std::fabs(s.p->em_[0].x) < 0.05f, fmt("two 90-degree steps: behind, x = %+.2f", s.p->em_[0].x));
        Rig t; t.neutral(); t.set(kPath, kStep); t.set(kStepAngle, 90); t.set(kSizeX, 100); t.p->reset();
        t.run(std::vector<float>(clicks.begin(), clicks.begin() + 6000), L, R);
        check(t.p->em_[0].x > 0.97f, fmt("one step: hard right, x = %+.2f", t.p->em_[0].x));

        Rig sc; sc.neutral(); sc.set(kPath, kScatter); sc.set(kSizeX, 100); sc.set(kSizeY, 100); sc.set(kDistance, 50);
        sc.p->reset();
        float lastX = 99.0f; int moved = 0; bool inside = true;
        for (int k = 0; k < 8; ++k)
        {
            sc.run(std::vector<float>(clicks.begin() + k * 11025, clicks.begin() + (k + 1) * 11025), L, R);
            const Emitter& e = sc.p->em_[0];
            if (std::fabs(e.x - lastX) > 0.05f) ++moved;
            lastX = e.x;
            inside = inside && e.x >= -1.0f && e.x <= 1.0f && e.y >= 0.0f && e.y <= 1.0f;
        }
        check(moved >= 7, fmt("Scatter landed somewhere new %.0f times in 8 hits", moved));
        check(inside, "and never left the plane");
    }

    std::printf("Doppler: movement bends the pitch, but only a little, and jumps cannot chirp\n");
    {
        // Instantaneous frequency of a 1 kHz tone from zero crossings, as a ratio to 1 kHz.
        auto maxDeviation = [&](const std::vector<float>& v, size_t from)
        {
            double worst = 0.0, last = -1.0;
            for (size_t i = from + 1; i < v.size(); ++i)
                if (v[i - 1] < 0.0f && v[i] >= 0.0f)
                {
                    const double t = (double)(i - 1) + v[i - 1] / (v[i - 1] - v[i]);
                    if (last >= 0.0) worst = std::max(worst, std::fabs(kSr / (t - last) / 1000.0 - 1.0));
                    last = t;
                }
            return worst;
        };
        const auto tone = sine(1000.0, 3.0);
        Rig still; still.neutral(); still.set(kPath, kTide); still.set(kRate, syncPos(8)); still.set(kSizeY, 100);
        still.set(kDistance, 50); still.set(kDepth, 0); still.p->reset(); still.playing = true;
        still.run(tone, L, R);
        const double dev0 = maxDeviation(L, 4410);
        Rig mv; mv.neutral(); mv.set(kPath, kTide); mv.set(kRate, syncPos(8)); mv.set(kSizeY, 100);
        mv.set(kDistance, 50); mv.set(kDepth, 0); mv.set(kDoppler, 100); mv.p->reset(); mv.playing = true;
        mv.run(tone, L, R);
        const double dev1 = maxDeviation(L, 4410);
        check(dev0 < 0.002, fmt("Doppler 0, Tide at 1/4: pitch within %.2f %%", dev0 * 100.0));
        check(dev1 > 0.01 && dev1 < 0.05, fmt("Doppler 100, Tide at 1/4: pitch moves up to %.2f %%", dev1 * 100.0));

        // Scatter at the shortest Glide and Doppler 100: each jump is slew-limited.
        std::vector<float> mix = tone;
        for (int k = 0; k < 10; ++k) for (int i = 0; i < 100; ++i) mix[(size_t)(k * 13000 + 3000 + i)] += 0.9f;
        Rig sc; sc.neutral(); sc.set(kPath, kScatter); sc.set(kSizeY, 100); sc.set(kDistance, 50); sc.set(kDepth, 0);
        sc.set(kDoppler, 100); sc.set(kGlide, 5); sc.set(kHitSens, 90); sc.p->reset();
        sc.run(mix, L, R);
        // Crossings disturbed by the clicks themselves are skipped: check only well after each click.
        double worst = 0.0;
        for (int k = 0; k < 10; ++k)
        {
            std::vector<float> seg(L.begin() + k * 13000 + 4200, L.begin() + k * 13000 + 12900);   // after the click, which arrives up to 20 ms late
            worst = std::max(worst, maxDeviation(seg, 0));
        }
        check(sc.p->hits_ >= 8, fmt("Scatter took %.0f hits", (double)sc.p->hits_));
        check(worst < 0.05, fmt("and no jump bent the pitch more than %.2f %%", worst * 100.0));
    }

    std::printf("Programs: every one is finite, audible and under full scale\n");
    {
        const auto in = noise(2.0, 0.35f);
        Bearings probe;
        for (int pr = 0; pr < probe.numPrograms(); ++pr)
        {
            Rig r; r.p->setProgram(pr); r.p->reset(); r.playing = true;
            r.run(in, L, R);
            double peak = 0.0; bool finite = true;
            for (size_t i = 0; i < L.size(); ++i)
            {
                finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
                peak = std::max(peak, (double)std::max(std::fabs(L[i]), std::fabs(R[i])));
            }
            const double lvl = std::max(rmsDb(L, 22050, L.size()), rmsDb(R, 22050, R.size()));
            check(finite && peak < 1.0 && lvl > -30.0,
                  std::string(r.p->programName(pr)) + fmt(": peak %.2f, level %.1f dB", peak, lvl));
        }
    }

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
