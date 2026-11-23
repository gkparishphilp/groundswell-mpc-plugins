// Does Conch ring the right notes, hear the key, retune without clicks, stay stable and cheap?
//
//   mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework -o tests/build/conchtest tests/conchtest.cpp
//   tests/build/conchtest build/conch-demo.wav
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#define private public
#define protected public
#include "../src/Conch.cpp"
#undef private
#undef protected

static const double SR = 44100.0;
static const double kPiD = 3.14159265358979;
static int failures = 0, passes = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) ++passes; else ++failures;
}

using Buf = std::vector<float>;

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

// ---- test material ----------------------------------------------------------------------------
static unsigned g_rs = 99;
static float rnd() { g_rs = g_rs * 1664525u + 1013904223u; return (float)(g_rs >> 8) / 16777216.0f * 2.0f - 1.0f; }
static double midiHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

// A boom-bap loop: kick on 16ths 0, 7, 10; snare on 4 and 12; hats on 8ths. Peaks around -6 dBFS.
static Buf drums(double bpm, int bars)
{
    const double spb = SR * 60.0 / bpm, step = spb / 4.0;
    const int total = (int)(bars * 4 * spb);
    Buf in((size_t)total, 0.0f);
    const int kickSteps[3] = {0, 7, 10};
    for (int b = 0; b < bars; ++b)
    {
        for (int k : kickSteps)
        {
            const long long t0 = (long long)((b * 16 + k) * step);
            double ph = 0.0;
            for (int i = 0; i < (int)(0.35 * SR) && t0 + i < total; ++i)
            {
                const double t = i / SR;
                ph += (48.0 + 110.0 * std::exp(-t * 35.0)) / SR;
                in[(size_t)(t0 + i)] += (float)(0.4 * std::sin(2 * kPiD * ph) * std::exp(-t * 9.0));
            }
        }
        for (int k = 0; k < 16; k += 2)
        {
            const long long t0 = (long long)((b * 16 + k) * step);
            const bool snare = (k == 4 || k == 12);
            for (int i = 0; i < (int)((snare ? 0.18 : 0.04) * SR) && t0 + i < total; ++i)
            {
                const double t = i / SR;
                float v = rnd() * (float)std::exp(-t * (snare ? 22.0 : 90.0)) * (snare ? 0.22f : 0.06f);
                if (snare) v += (float)(0.12 * std::sin(2 * kPiD * 190.0 * t) * std::exp(-t * 30.0));
                in[(size_t)(t0 + i)] += v;
            }
        }
    }
    return in;
}

// Saw-triad pad, one chord per bar, cycling through `chords`.
static Buf pad(double bpm, int bars, const std::vector<std::vector<int>>& chords, float level)
{
    const double spb = SR * 60.0 / bpm;
    const int total = (int)(bars * 4 * spb);
    Buf out((size_t)total, 0.0f);
    for (int b = 0; b < bars; ++b)
        for (int note : chords[(size_t)b % chords.size()])
        {
            const double hz = midiHz(note);
            double ph = 0.0;
            const long long t0 = (long long)(b * 4 * spb), len = (long long)(4 * spb);
            for (long long i = 0; i < len && t0 + i < total; ++i)
            {
                ph += hz / SR; ph -= std::floor(ph);
                const float env = (float)std::fmin(1.0, std::fmin(i / (0.02 * SR), (len - i) / (0.02 * SR)));
                out[(size_t)(t0 + i)] += (float)(2.0 * ph - 1.0) * level * env;
            }
        }
    return out;
}

static Buf add(const Buf& a, const Buf& b) { Buf o(std::max(a.size(), b.size()), 0.0f); for (size_t i = 0; i < o.size(); ++i) o[i] = (i < a.size() ? a[i] : 0.0f) + (i < b.size() ? b[i] : 0.0f); return o; }

// ---- rendering ----------------------------------------------------------------------------------
struct Render { Buf l, r; };
using BlockHook = std::function<void(Conch&, long long pos)>;

static Render render(Conch& p, const Buf& src, int block, double bpm, bool playing, BlockHook hook = nullptr,
                     size_t extra = 0)
{
    Render out;
    const size_t total = src.size() + extra;
    out.l.assign(total, 0.0f); out.r.assign(total, 0.0f);
    std::vector<float> l((size_t)block), r((size_t)block);
    const double spb = SR * 60.0 / bpm;
    mpc::ScopedDenormalsOff ftz;   // the host does this around every process() call
    for (size_t pos = 0; pos < total; pos += (size_t)block)
    {
        const int n = (int)std::min((size_t)block, total - pos);
        for (int i = 0; i < n; ++i) l[(size_t)i] = r[(size_t)i] = (pos + (size_t)i < src.size()) ? src[pos + (size_t)i] : 0.0f;
        if (hook) hook(p, (long long)pos);
        const float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {l.data(), r.data()};
        mpc::ProcessContext ctx;
        ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = n;
        ctx.sampleRate = SR;
        ctx.transport.valid = ctx.transport.tempoValid = true;
        ctx.transport.ppqValid = playing; ctx.transport.playing = playing; ctx.transport.bpm = bpm;
        ctx.transport.ppqPos = playing ? (double)pos / spb : 0.0;
        p.process(ctx);
        for (int i = 0; i < n; ++i) { out.l[pos + (size_t)i] = l[(size_t)i]; out.r[pos + (size_t)i] = r[(size_t)i]; }
    }
    return out;
}

static void setup(Conch& p, std::initializer_list<std::pair<int, float>> v)
{
    p.prepare(SR, 512);
    for (const auto& kv : v) p.set(kv.first, kv.second);
    p.reset();
}

// ---- analysis -----------------------------------------------------------------------------------
static double goertzel(const Buf& x, size_t from, size_t len, double hz)
{
    const double w = 2 * kPiD * hz / SR, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < len && from + i < x.size(); ++i)
    {
        const double win = 0.5 - 0.5 * std::cos(2 * kPiD * (double)i / (double)len);
        const double s0 = x[from + i] * win + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return std::sqrt(std::fmax(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (double)len;
}

// Where the spectral peak near `hz` really is, in cents from hz (searches +-60 cents).
static double peakCents(const Buf& x, size_t from, size_t len, double hz)
{
    double best = -1, bestC = 0;
    for (int c = -60; c <= 60; c += 2)
    {
        const double m = goertzel(x, from, len, hz * std::pow(2.0, c / 1200.0));
        if (m > best) { best = m; bestC = c; }
    }
    const double a = goertzel(x, from, len, hz * std::pow(2.0, (bestC - 2) / 1200.0));
    const double b = goertzel(x, from, len, hz * std::pow(2.0, (bestC + 2) / 1200.0));
    const double den = a - 2 * best + b;
    return bestC + (std::fabs(den) > 1e-12 ? 0.5 * (a - b) / den * 2.0 : 0.0);
}

static double rms(const Buf& x, size_t from, size_t len)
{
    double s = 0; size_t n = 0;
    for (size_t i = from; i < from + len && i < x.size(); ++i) { s += (double)x[i] * x[i]; ++n; }
    return n ? std::sqrt(s / (double)n) : 0.0;
}
static double db(double v) { return 20.0 * std::log10(std::fmax(v, 1e-12)); }
static Buf mono(const Render& r) { Buf m(r.l.size()); for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (r.l[i] + r.r[i]); return m; }

// Click detector: the largest second difference in +-3 ms around `at`, against the largest in the
// 150 ms before it. A ringing tone has a steady second difference; a discontinuity spikes it.
static double clickRatio(const Buf& x, size_t at)
{
    auto d2 = [&](size_t i) { return std::fabs((double)x[i] - 2.0 * x[i - 1] + x[i - 2]); };
    const size_t w = (size_t)(0.003 * SR), pre = (size_t)(0.15 * SR);
    double around = 0, before = 1e-9;
    for (size_t i = at - w; i < at + w && i < x.size(); ++i) around = std::fmax(around, d2(i));
    for (size_t i = at - w - pre; i < at - w; ++i) before = std::fmax(before, d2(i));
    return around / before;
}

static void printNotes(const char* label, const int* notes, int n)
{
    std::printf("  %s:", label);
    for (int i = 0; i < n; ++i) std::printf(" %s%d", kNoteNames[notes[i] % 12], notes[i] / 12 - 1);
    std::printf("\n");
}

// Ring the bank with noise bursts, then measure where every expected note really rings.
static void pitchTest(const char* name, std::initializer_list<std::pair<int, float>> settings, int wantRoot, int wantMask)
{
    Conch p;
    setup(p, settings);
    Buf in((size_t)(1.2 * SR), 0.0f);
    for (int b = 0; b < 3; ++b)
        for (int i = 0; i < (int)(0.01 * SR); ++i) in[(size_t)(b * 0.3 * SR) + (size_t)i] = 0.5f * rnd();
    Render r = render(p, in, 128, 120.0, false, nullptr, (size_t)(2.5 * SR));
    const Buf m = mono(r);
    const int n = p.reqN_;
    char label[96];
    std::snprintf(label, sizeof(label), "%s notes", name);
    printNotes(label, p.reqNotes_, n);

    // Pitch: every note within 5 cents, measured on the free ringing after the last burst.
    const size_t from = (size_t)(1.3 * SR), len = (size_t)(2.0 * SR);
    double worst = 0;
    for (int i = 0; i < n; ++i)
    {
        const double c = peakCents(m, from, len, midiHz(p.reqNotes_[i]));
        if (std::fabs(c) >= 5.0) std::printf("    %s%d off by %+.1f cents\n", kNoteNames[p.reqNotes_[i] % 12], p.reqNotes_[i] / 12 - 1, c);
        worst = std::fmax(worst, std::fabs(c));
    }
    std::printf("  %s: %d notes, worst pitch error %.2f cents\n", name, n, worst);
    std::snprintf(label, sizeof(label), "%s: every resonator rings within 5 cents of its note", name);
    check(n >= 4 && worst < 5.0, label);

    // Which pitch classes ring: semitones C2..B6, folded.
    double pcE[12] = {}, total = 0;
    for (int midi = 36; midi < 96; ++midi)
    {
        const double e = std::pow(goertzel(m, from, len, midiHz(midi)), 2.0);
        pcE[midi % 12] += e; total += e;
    }
    double inChord = 0; for (int pc = 0; pc < 12; ++pc) if (wantMask & (1 << pc)) inChord += pcE[pc];
    std::printf("  %s: chord tones carry %.0f%% of the energy at semitone pitches\n", name, 100.0 * inChord / total);
    std::snprintf(label, sizeof(label), "%s: chord tones carry >= 90%% of the pitched energy", name);
    check(inChord / total >= 0.9, label);
    std::snprintf(label, sizeof(label), "%s: chord root is %s", name, kNoteNames[wantRoot]);
    check(p.activeChord_.root == wantRoot, label);
}

int main(int argc, char** argv)
{
    const double bpm = 92.0;
    const double spb = SR * 60.0 / bpm;
    const size_t bar = (size_t)(4 * spb);

    // ================= 1. pitches in Key and Chord modes, every material ==========================
    std::printf("-- pitches --\n");
    const int aMinor = (1 << 9) | (1 << 0) | (1 << 4);
    const char* matNames[3] = {"String", "Bell", "Pipe"};
    for (int mat = 0; mat < 3; ++mat)
    {
        char nm[64];
        std::snprintf(nm, sizeof(nm), "Key A minor, %s", matNames[mat]);
        pitchTest(nm, {{kSource, kSrcKey}, {kKey, 9}, {kScale, 0}, {kMaterial, (float)mat}, {kMix, 100}, {kHits, 0},
                       {kShimmer, 0}, {kDecay, 6.0f}, {kBright, 50}, {kHpf, 20}}, 9, aMinor);
    }
    // D major 7 by hand, over 3 octaves from C3: D F# A C#.
    const int dMaj7 = (1 << 2) | (1 << 6) | (1 << 9) | (1 << 1);
    // Dark (Bright 10): the most loop-filter delay to compensate, and an overtone of one note (the 5th
    // harmonic of D3 is a pure third, 14 cents below F#5) cannot pull the measurement of another.
    pitchTest("Chord Dmaj7, String", {{kSource, kSrcChord}, {kChordRoot, 2}, {kQuality, 7}, {kLowNote, 2}, {kSpread, 3},
                                      {kMix, 100}, {kHits, 0}, {kShimmer, 0}, {kDecay, 6.0f}, {kBright, 10}, {kHpf, 20}}, 2, dMaj7);
    pitchTest("Chord Dmaj7, Pipe", {{kSource, kSrcChord}, {kChordRoot, 2}, {kQuality, 7}, {kLowNote, 2}, {kSpread, 3},
                                    {kMaterial, kPipe}, {kMix, 100}, {kHits, 0}, {kShimmer, 0}, {kDecay, 6.0f}, {kBright, 10}, {kHpf, 20}}, 2, dMaj7);
    // Tune: +30 cents moves every note by +30 cents.
    {
        Conch p;
        setup(p, {{kSource, kSrcKey}, {kMix, 100}, {kHits, 0}, {kShimmer, 0}, {kDecay, 6.0f}, {kTune, 30}, {kHpf, 20}});
        Buf in((size_t)(0.5 * SR), 0.0f);
        for (int i = 0; i < (int)(0.01 * SR); ++i) in[(size_t)i] = 0.5f * rnd();
        Render r = render(p, in, 128, 120.0, false, nullptr, (size_t)(2.5 * SR));
        const Buf m = mono(r);
        double worst = 0;
        for (int i = 0; i < p.reqN_; ++i) worst = std::fmax(worst, std::fabs(peakCents(m, (size_t)(0.6 * SR), (size_t)(2.0 * SR), midiHz(p.reqNotes_[i])) - 30.0));
        std::printf("  Tune +30: worst error %.2f cents\n", worst);
        check(worst < 5.0, "Tune +30 cents shifts every resonator by 30 cents");
    }
    // Voicing sanity: notes below C3 are only root and fifth; Spread 4 + 9th caps at 16.
    {
        int notes[kMaxPitches];
        const int n = voiceChord(diatonicChord(9, 0, 0, kTriad), 36, 3, notes, kMaxPitches);
        printNotes("A minor, C2 + 3 oct", notes, n);
        bool thin = true; for (int i = 0; i < n; ++i) if (notes[i] < 48 && notes[i] % 12 == 0) thin = false;
        check(n == 8 && thin, "voicing: 8 notes, only root and fifth below C3");
        const int n16 = voiceChord(diatonicChord(9, 0, 0, kNinth), 36, 4, notes, kMaxPitches);
        check(n16 == 16, "voicing: Spread 4 with 9ths is capped at 16 resonators");
        const Chord v = diatonicChord(9, 0, 4, kNinth);   // Em7 in A minor: the b9 (F) is left out
        check(qualityOf(v) == 8 && v.n == 4, "diatonic 9th: a minor ninth is left out (v in A minor = Em7)");
    }

    // ================= 2. Listen hears the key on drums + pad ======================================
    std::printf("-- listen --\n");
    const int bars = 16;
    const Buf drumLoop = drums(bpm, bars);
    const std::vector<std::vector<int>> amProg = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {57, 60, 64}};
    const int amRoots[4] = {9, 5, 7, 9};
    const Buf padLoop = pad(bpm, bars, amProg, 0.03f);
    const Buf mixLoop = add(drumLoop, padLoop);
    {
        Conch p;
        setup(p, {{kSource, kSrcListen}, {kKey, 2}, {kScale, 1}});   // knobs say D major: wrong on purpose
        std::vector<int> tunedBy, roots;
        int firstHeardBar = -1;
        Render r = render(p, mixLoop, 128, bpm, true, [&](Conch& c, long long pos) {
            if (pos % (long long)(bar / 4) < 128)                    // sample 4 times a bar
            {
                tunedBy.push_back(c.tunedBy_);
                roots.push_back(c.activeChord_.root);
                if (firstHeardBar < 0 && c.tunedBy_ == kByHeard) firstHeardBar = (int)(pos / (long long)bar);
            }
        });
        std::printf("  found %s%s (confidence %.2f), first heard in bar %d\n", kNoteNames[p.foundRoot_],
                    p.foundMinor_ ? "m" : "", p.harm_.confidence(), firstHeardBar + 1);
        check(tunedBy.size() > 1 && tunedBy[1] == kByUnsure, "Listen: starts out unsure, on Key/Scale");
        check(p.haveFound_ && p.foundRoot_ == 9 && p.foundMinor_, "Listen: hears A minor on drums + pad");
        check(firstHeardBar >= 0 && firstHeardBar <= 4, "Listen: sure of the key within 5 bars");
        // Chord following: in the second half of each bar (after the detector has heard the change),
        // from bar 6 on, the resonators should be on that bar's chord.
        int ok = 0, nChecked = 0;
        for (size_t k = 0; k < roots.size(); ++k)
        {
            const int b = (int)(k / 4), q = (int)(k % 4);
            if (b < 5 || q < 2) continue;
            ++nChecked; if (roots[k] == amRoots[b % 4]) ++ok;
        }
        std::printf("  chord roots right in %d/%d checks (bars 6-16, second half of each bar)\n", ok, nChecked);
        check(nChecked > 0 && ok >= (int)(0.8 * nChecked), "Listen: follows the chords (80%+)");
    }
    {
        Conch p;
        setup(p, {{kSource, kSrcListen}, {kKey, 7}, {kScale, 0}});   // drums only, knobs on G minor
        int unsure = 0, total = 0;
        render(p, drumLoop, 128, bpm, true, [&](Conch& c, long long pos) {
            if (pos > (long long)SR && pos % 4410 < 128) { ++total; if (c.tunedBy_ == kByUnsure) ++unsure; }
        });
        std::printf("  drums only: unsure %d/%d checks, ringing root %s, confidence %.2f\n", unsure, total,
                    kNoteNames[p.activeChord_.root], p.harm_.confidence());
        check(unsure == total && p.activeChord_.root == 7, "Listen on drums only: stays on Key/Scale (G minor) and says so");
    }
    // Prog locked to the transport: 1-6-7-1 in A minor, one bar each: A, F, G, A.
    {
        Conch p;
        setup(p, {{kSource, kSrcProg}, {kKey, 9}, {kScale, 0}, {kProg, 0}, {kChordLen, 1}});
        int ok = 0, n = 0;
        render(p, drumLoop, 128, bpm, true, [&](Conch& c, long long pos) {
            const double inBar = std::fmod((double)pos / (double)bar, 1.0);
            if (pos > 0 && inBar > 0.05 && inBar < 0.95 && pos % 2205 < 128)
            { ++n; if (c.activeChord_.root == amRoots[(pos / (long long)bar) % 4]) ++ok; }
        });
        std::printf("  Prog 1-6-7-1: %d/%d checks on the right chord\n", ok, n);
        check(n > 0 && ok == n, "Prog: chords change on the bar lines, in the key");
    }

    // ================= 3. retuning is click-free ===================================================
    std::printf("-- clicks --\n");
    {
        // Self-test: a 0.02 step in a ringing tone must be caught.
        Buf t((size_t)SR);
        for (size_t i = 0; i < t.size(); ++i) t[i] = (float)(0.3 * std::sin(2 * kPiD * 220.0 * (double)i / SR)) + (i >= 30000 ? 0.02f : 0.0f);
        const double cr = clickRatio(t, 30000);
        std::printf("  detector self-test: a 0.02 step scores %.1f\n", cr);
        check(cr > 2.0, "click detector catches a small step");
    }
    for (int mat = 0; mat < 3; ++mat)
    {
        // (a) Ringing only: one burst, then the chord changes every 0.25 s while it rings.
        Conch p;
        setup(p, {{kSource, kSrcChord}, {kChordRoot, 9}, {kQuality, 1}, {kMaterial, (float)mat}, {kMix, 100},
                  {kHits, 0}, {kDecay, 8.0f}, {kHpf, 20}});
        Buf in((size_t)(3.0 * SR), 0.0f);
        for (int i = 0; i < (int)(0.01 * SR); ++i) in[(size_t)(0.1 * SR) + (size_t)i] = 0.5f * rnd();
        const int roots[6] = {5, 0, 7, 2, 10, 4};
        std::vector<size_t> changes;
        Render r = render(p, in, 64, 120.0, false, [&](Conch& c, long long pos) {
            for (int k = 0; k < 6; ++k)
            {
                const long long at = (long long)((0.5 + 0.25 * k) * SR) / 64 * 64;
                if (pos == at) { c.set(kChordRoot, (float)roots[k]); changes.push_back((size_t)pos); }
            }
        });
        double worst = 0;
        const Buf m = mono(r);
        for (size_t at : changes) worst = std::fmax(worst, std::fmax(clickRatio(r.l, at), clickRatio(r.r, at)));
        std::printf("  %s, ringing through 6 chord changes: worst click score %.2f\n", matNames[mat], worst);
        char lab[96]; std::snprintf(lab, sizeof(lab), "%s: chord changes while ringing are click-free", matNames[mat]);
        check(worst < 1.5, lab);
        // The old chord rings on after the change (not cut), then dies away (banks go idle).
        int busy = 0; for (Bank& b : p.banks_) if (b.state() != Bank::kIdle) ++busy;
        std::printf("    after the last change: %d bank(s) busy\n", busy);
    }
    {
        // (b) Fast changes with a sustained input: every 30 ms, so banks must be killed to make room.
        Conch p;
        setup(p, {{kSource, kSrcChord}, {kChordRoot, 9}, {kQuality, 1}, {kMix, 100}, {kHits, 0}, {kDecay, 5.0f}, {kHpf, 20}});
        Buf in((size_t)(2.0 * SR), 0.0f);
        for (size_t i = 0; i < in.size(); ++i) in[i] = 0.08f * (float)std::sin(2 * kPiD * 110.0 * (double)i / SR) + 0.01f * rnd();
        std::vector<size_t> changes;
        int kills = 0;
        Render r = render(p, in, 64, 120.0, false, [&](Conch& c, long long pos) {
            for (Bank& b : c.banks_) if (b.state() == Bank::kKilling) { ++kills; break; }
            if (pos >= (long long)(0.5 * SR) && pos % 1344 == 0)          // every ~30 ms
            { c.set(kChordRoot, (float)((pos / 1344) % 12)); changes.push_back((size_t)pos); }
        });
        double worst = 0;
        for (size_t at : changes) if (at + 300 < r.l.size()) worst = std::fmax(worst, clickRatio(r.l, at));
        std::printf("  30 ms chord changes on a held tone: %zu changes, %d blocks spent fading a bank, worst score %.2f\n",
                    changes.size(), kills, worst);
        check(kills > 0 && worst < 1.5, "rapid chord changes (banks recycled) are click-free");
        bool finite = true; for (float v : r.l) if (!std::isfinite(v)) finite = false;
        check(finite, "rapid chord changes: output stays finite");
    }

    // ================= 4. Hits keeps sustained input from washing out ==============================
    std::printf("-- hits --\n");
    {
        // A held A minor chord (one attack, then four bars of sustain), right on the resonators.
        const Buf sustained = pad(bpm / 4.0, 1, {{57, 60, 64}}, 0.05f);   // one 4-bar chord at the test tempo
        const Buf dr = drums(bpm, 4);
        float minGuard = 1.0f;
        auto wetRms = [&](const Buf& in, float hits) {
            Conch p;
            setup(p, {{kSource, kSrcKey}, {kMix, 100}, {kHits, hits}, {kDuck, 0}, {kDecay, 1.0f}});
            Render r = render(p, in, 128, bpm, false, [&](Conch& c, long long) { minGuard = std::fmin(minGuard, c.guard_); });
            return rms(mono(r), bar, in.size() - bar);
        };
        const double padHits0 = db(wetRms(sustained, 0)), padHits100 = db(wetRms(sustained, 100));
        const double drHits0 = db(wetRms(dr, 0)), drHits100 = db(wetRms(dr, 100));
        const double bothHits100 = db(wetRms(add(dr, sustained), 100));
        std::printf("  (level guard went down to %.2f during these)\n", minGuard);
        std::printf("  held chord: Hits 0 -> %.1f dBFS, Hits 100 -> %.1f dBFS (%.1f dB less)\n", padHits0, padHits100, padHits0 - padHits100);
        std::printf("  drums:      Hits 0 -> %.1f dBFS, Hits 100 -> %.1f dBFS (%.1f dB less)\n", drHits0, drHits100, drHits0 - drHits100);
        std::printf("  drums + held chord at Hits 100: %.1f dBFS (%.1f dB over drums alone)\n", bothHits100, bothHits100 - drHits100);
        check(padHits0 - padHits100 > 15.0, "Hits 100: a held chord barely rings the resonators (>15 dB less)");
        check(drHits0 - drHits100 < 6.0, "Hits 100: drums still ring them (<6 dB less)");
        check(bothHits100 - drHits100 < 3.0, "Hits 100: adding a held chord to the drums does not wash them out (<3 dB)");
    }
    // Duck: the ringing dips right after each hit.
    {
        const Buf dr = drums(bpm, 4);
        auto ringAfterKick = [&](float duck) {
            Conch p;
            setup(p, {{kSource, kSrcKey}, {kMix, 100}, {kDuck, duck}, {kDecay, 4.0f}});
            Render r = render(p, dr, 128, bpm, false);
            const Buf m = mono(r);
            return rms(m, 3 * bar + (size_t)(0.01 * SR), (size_t)(0.05 * SR));   // 10-60 ms after bar 4's kick
        };
        const double d0 = db(ringAfterKick(0)), d100 = db(ringAfterKick(100));
        std::printf("  just after a kick: Duck 0 %.1f dBFS, Duck 100 %.1f dBFS\n", d0, d100);
        check(d0 - d100 > 8.0, "Duck 100 dips the ringing under a hit by more than 8 dB");
    }

    // ================= 5. Freeze holds =============================================================
    std::printf("-- freeze --\n");
    for (int mat = 0; mat < 3; ++mat)
    {
        Conch p;
        setup(p, {{kSource, kSrcChord}, {kChordRoot, 9}, {kQuality, 1}, {kMaterial, (float)mat}, {kMix, 100},
                  {kDecay, 2.0f}, {kBright, 50}, {kDuck, 0}});
        const Buf dr = drums(bpm, 2);
        const size_t freezeAt = (size_t)(1.5 * bar) / 128 * 128;
        Render r = render(p, dr, 128, bpm, false, [&](Conch& c, long long pos) {
            if ((size_t)pos == freezeAt) c.set(kFreeze, 1);
            if ((size_t)pos == freezeAt + (size_t)(2 * SR) / 128 * 128) c.set(kChordRoot, 0);   // ignored while frozen
        }, (size_t)(12 * SR));
        const Buf m = mono(r);
        const double early = rms(m, freezeAt + (size_t)(1.0 * SR), (size_t)SR);
        const double late = rms(m, freezeAt + (size_t)(11.0 * SR), (size_t)SR);
        std::printf("  %s: 1-2 s after Freeze %.1f dBFS, 11-12 s after %.1f dBFS (%.1f dB in 10 s, Decay 2 s)\n",
                    matNames[mat], db(early), db(late), db(early) - db(late));
        char lab[96]; std::snprintf(lab, sizeof(lab), "%s: Freeze holds the chord (< 4 dB lost in 10 s)", matNames[mat]);
        check(db(early) - db(late) < 4.0 && db(late) > -60.0, lab);
        std::snprintf(lab, sizeof(lab), "%s: no retune while frozen (still A minor)", matNames[mat]);
        check(p.activeChord_.root == 9, lab);
        if (mat == 0)
        {
            p.set(kFreeze, 0);
            render(p, Buf((size_t)(0.1 * SR), 0.0f), 128, bpm, false);
            check(p.activeChord_.root == 0, "after Freeze, the waiting chord change (C minor) happens");
        }
    }

    // ================= 6. stability ================================================================
    std::printf("-- stability --\n");
    for (int mat = 0; mat < 3; ++mat)
    {
        // Worst case: a loud held note right on a resonator, long decay, full drive, no Hits gate,
        // Bright and Shimmer full, then silence.
        Conch p;
        setup(p, {{kSource, kSrcKey}, {kMaterial, (float)mat}, {kMix, 100}, {kHits, 0}, {kDecay, 20.0f}, {kBright, 100},
                  {kDrive, 100}, {kShimmer, 100}, {kSpread, 4}, {kTones, kNinth}, {kLowNote, 0}, {kHpf, 20}, {kWidth, 100}});
        Buf in((size_t)(15 * SR), 0.0f);
        for (size_t i = 0; i < in.size(); ++i) in[i] = 0.9f * (float)std::sin(2 * kPiD * 220.0 * (double)i / SR) + 0.1f * rnd();
        Render r = render(p, in, 128, bpm, false, nullptr, (size_t)(5 * SR));
        bool finite = true; float pk = 0;
        for (size_t i = 0; i < r.l.size(); ++i)
        {
            if (!std::isfinite(r.l[i]) || !std::isfinite(r.r[i])) finite = false;
            pk = std::fmax(pk, std::fmax(std::fabs(r.l[i]), std::fabs(r.r[i])));
        }
        std::printf("  %s, Decay 20 s, held 0 dBFS note on a resonator for 15 s: peak %.2f dBFS\n", matNames[mat], db(pk));
        char lab[96]; std::snprintf(lab, sizeof(lab), "%s: no blow-up or NaN at Decay 20 s (wet peak <= 0 dBFS)", matNames[mat]);
        check(finite && pk <= 1.0f, lab);
    }
    {
        // Denormals: a short decay dies into silence; with the host's flush-to-zero nothing subnormal
        // may come out, and ringing-out banks must go idle.
        Conch p;
        setup(p, {{kSource, kSrcChord}, {kMix, 100}, {kDecay, 0.1f}});
        Buf in((size_t)(0.5 * SR), 0.0f);
        for (int i = 0; i < 2000; ++i) in[(size_t)i] = 0.5f * rnd();
        bool changed = false;
        Render r = render(p, in, 128, bpm, false, [&](Conch& c, long long pos) {
            if (!changed && pos > (long long)(0.05 * SR)) { c.set(kChordRoot, 2); changed = true; }
        }, (size_t)(5 * SR));
        int sub = 0;
        for (float v : r.l) if (v != 0.0f && std::fabs(v) < 1.17549435e-38f) ++sub;
        int busy = 0; for (Bank& b : p.banks_) if (b.state() != Bank::kIdle) ++busy;
        std::printf("  tail: %d subnormal samples; %d bank(s) still busy (the active one)\n", sub, busy);
        check(sub == 0, "no denormals in the decaying tail");
        check(busy == 1, "ringing-out banks go idle once silent");
    }
    {
        // Parameter 0 (Output) toggled like the MPC does on load: no retune, no side effects.
        Conch p;
        setup(p, {{kSource, kSrcKey}});
        render(p, Buf(4410, 0.0f), 128, bpm, false);
        const int before = p.active_;
        p.setNormalised(kOutput, 1.0f); p.setNormalised(kOutput, 0.0f); p.set(kOutput, 0.0f);
        render(p, Buf(4410, 0.0f), 128, bpm, false);
        check(p.active_ == before && !p.pending_, "parameter 0 nudge causes no retune");
    }

    // ================= 7. levels and CPU ===========================================================
    std::printf("-- levels --\n");
    const double dryRms = db(rms(drumLoop, 0, drumLoop.size()));
    double matDb[3];
    for (int mat = 0; mat < 3; ++mat)
    {
        Conch p;
        setup(p, {{kSource, kSrcKey}, {kMaterial, (float)mat}, {kMix, 100}});
        float minGuard = 1.0f; float wetPk = 0.0f;
        Render r = render(p, drumLoop, 128, bpm, false, [&](Conch& c, long long pos) { if (pos > (long long)bar) minGuard = std::fmin(minGuard, c.guard_); });
        for (size_t i = bar; i < r.l.size(); ++i) wetPk = std::fmax(wetPk, std::fabs(r.l[i]));
        matDb[mat] = db(rms(mono(r), bar, drumLoop.size() - bar));
        std::printf("  %s wet only (defaults): %.1f dBFS rms, peak %.1f dBFS (drums dry %.1f dBFS rms); level guard min %.2f\n",
                    matNames[mat], matDb[mat], db(wetPk), dryRms, minGuard);
    }
    const double spreadDb = std::max({matDb[0], matDb[1], matDb[2]}) - std::min({matDb[0], matDb[1], matDb[2]});
    check(spreadDb < 3.0, "materials within 3 dB of each other");
    check(matDb[0] - dryRms > -12.0 && matDb[0] - dryRms < 3.0, "wet level sits near the dry level (-12..+3 dB)");
    {
        // Programs: no program clips the drum loop.
        Conch probe;
        for (int pr = 0; pr < probe.numPrograms(); ++pr)
        {
            Conch p; p.prepare(SR, 512); p.setProgram(pr); p.reset();
            Render r = render(p, mixLoop, 128, bpm, true);
            float pk = 0; for (size_t i = 0; i < r.l.size(); ++i) pk = std::fmax(pk, std::fmax(std::fabs(r.l[i]), std::fabs(r.r[i])));
            std::printf("  program %-18s peak %5.1f dBFS  rms %5.1f dBFS\n", p.programName(pr), db(pk), db(rms(mono(r), 0, r.l.size())));
        }
    }

    std::printf("-- cpu --\n");
    auto cpu = [&](const char* name, std::initializer_list<std::pair<int, float>> s, const Buf& in, BlockHook hook) {
        Conch p;
        setup(p, s);
        const auto t0 = std::chrono::steady_clock::now();
        render(p, in, 128, bpm, true, hook);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double pct = 100.0 * sec / ((double)in.size() / SR);
        std::printf("  %-44s %.3f%% of one desktop core\n", name, pct);
        return pct;
    };
    const double c0 = cpu("String, 8 notes, Key", {{kSource, kSrcKey}}, mixLoop, nullptr);
    const double c1 = cpu("defaults (String, 8 notes, Listen)", {}, mixLoop, nullptr);
    std::printf("  (so the key and chord analysis costs about %.3f%%)\n", c1 - c0);
    const double c2 = cpu("Bell, 16 notes, Key", {{kMaterial, kBell}, {kSpread, 4}, {kTones, kNinth}, {kLowNote, 0}, {kSource, kSrcKey}}, mixLoop, nullptr);
    const double c3 = cpu("String, 16 notes, chord change every beat", {{kSpread, 4}, {kTones, kNinth}, {kLowNote, 0}, {kSource, kSrcChord}},
                          mixLoop, [&](Conch& c, long long pos) { if (pos % (long long)(spb / 128 * 128) < 128) c.set(kChordRoot, (float)((pos / 1000) % 12)); });
    check(std::max({c1, c2, c3}) < 1.0, "CPU under 1% of a desktop core in every case");

    // ================= demo =======================================================================
    if (argc > 1)
    {
        Render demo; demo.l.clear(); demo.r.clear();
        auto append = [&](const Render& r, float gain) {
            for (size_t i = 0; i < r.l.size(); ++i) { demo.l.push_back(r.l[i] * gain); demo.r.push_back(r.r[i] * gain); }
            const size_t gap = (size_t)(0.6 * SR);
            for (size_t i = 0; i < gap; ++i) { demo.l.push_back(0.0f); demo.r.push_back(0.0f); }
        };
        const Buf d8 = drums(bpm, 8);
        auto sec = [&](double s) { return (double)demo.l.size() / SR + s * 0; };
        std::printf("-- demo --\n");
        { Render dry; dry.l = d8; dry.r = d8; std::printf("  %5.1f s  the drum loop, dry\n", sec(0)); append(dry, 1.0f); }
        const int progs[3] = {0, 1, 2};   // Drum Strings, Glass Kit, Hollow Shell
        for (int k = 0; k < 3; ++k)
        {
            Conch p; p.prepare(SR, 512); p.setProgram(progs[k]); p.reset();
            std::printf("  %5.1f s  drums through %s (%s, Key A minor)\n", sec(0), p.programName(progs[k]), matNames[k]);
            append(render(p, d8, 128, bpm, true, nullptr, (size_t)(1.5 * SR)), 1.0f);
        }
        {
            Conch p; p.prepare(SR, 512); p.setProgram(6); p.reset();
            std::printf("  %5.1f s  drums through Gamelan Hits\n", sec(0));
            append(render(p, d8, 128, bpm, true, nullptr, (size_t)(1.5 * SR)), 1.0f);
        }
        {
            Conch p; p.prepare(SR, 512); p.setProgram(4); p.reset();
            std::printf("  %5.1f s  drums through Dub Pipes (Prog 1-6-7-1, 2 bars a chord)\n", sec(0));
            append(render(p, drums(bpm, 16), 128, bpm, true, nullptr, (size_t)(2.0 * SR)), 1.0f);
        }
        {
            Render dry; dry.l = mixLoop; dry.r = mixLoop;
            std::printf("  %5.1f s  drums + pad (Am F G Am), dry\n", sec(0));
            append(dry, 1.0f);
            Conch p; p.prepare(SR, 512); p.setProgram(3); p.reset();
            std::printf("  %5.1f s  the same through Mix Halo (Listen: unsure at first, then hears A minor and follows)\n", sec(0));
            append(render(p, mixLoop, 128, bpm, true, nullptr, (size_t)(2.0 * SR)), 1.0f);
        }
        {
            Conch p; p.prepare(SR, 512); p.setProgram(5); p.reset();
            const Buf d4 = drums(bpm, 4);
            std::printf("  %5.1f s  Frozen Chord: drums, Freeze at bar 3, drums stop, the chord holds for 8 s\n", sec(0));
            append(render(p, d4, 128, bpm, true, [&](Conch& c, long long pos) {
                if (pos == (long long)(2 * bar) / 128 * 128) c.set(kFreeze, 1);
            }, (size_t)(8 * SR)), 1.0f);
        }
        writeWav(argv[1], demo.l, demo.r);
        std::printf("wrote %s (%.1f s)\n", argv[1], (double)demo.l.size() / SR);
    }

    std::printf("%s (%d passed, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", passes, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
