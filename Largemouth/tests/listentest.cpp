// Does Largemouth lock to the kick and follow the chords? Drums + a pad in A minor, checked note by note.
//
//   clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest && tests/build/listentest [out.wav]
#include <vector>
#include <cstdio>
#include <cmath>
#include <string>
#include <chrono>
#include <algorithm>

struct NoteLog { long long sample; int type; float midi; float vel; };
static std::vector<NoteLog> g_notes;
static long long g_now = 0;          // sample at the start of the current block
#define LARGEMOUTH_NOTE_HOOK(type, midi, vel) g_notes.push_back({g_now, (int)(type), (midi), (vel)})

#define private public
#define protected public
#include "../src/Largemouth.cpp"
#undef private
#undef protected

static const double SR = 44100.0;
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

int main(int argc, char** argv)
{
    const double bpm = 92.0;
    const int bars = 16;
    const double spb = SR * 60.0 / bpm, step = spb / 4.0;
    const int total = (int)(bars * 4 * spb);
    std::vector<float> in((size_t)total, 0.0f);
    unsigned rs = 99; auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (float)(rs >> 8) / 16777216.0f * 2.0f - 1.0f; };

    // Kicks on 16th steps 0, 7, 10 of each bar (a boom-bap pattern), slightly late on step 7 (swing-ish).
    const int kickSteps[3] = {0, 7, 10};
    std::vector<long long> kickTimes;
    for (int b = 0; b < bars; ++b)
        for (int k : kickSteps)
        {
            const long long t0 = (long long)((b * 16 + k) * step + (k == 7 ? 0.12 * step : 0.0));
            kickTimes.push_back(t0);
            double ph = 0.0;
            for (int i = 0; i < (int)(0.35 * SR) && t0 + i < total; ++i)
            {
                const double t = i / SR;
                const double hz = 48.0 + 110.0 * std::exp(-t * 35.0);
                ph += hz / SR;
                in[(size_t)(t0 + i)] += (float)(0.8 * std::sin(6.2831853 * ph) * std::exp(-t * 9.0));
            }
        }
    // Snares on 4 and 12, hats on every 8th.
    for (int b = 0; b < bars; ++b)
        for (int k = 0; k < 16; k += 2)
        {
            const long long t0 = (long long)((b * 16 + k) * step);
            const bool snare = (k == 4 || k == 12);
            for (int i = 0; i < (int)((snare ? 0.18 : 0.04) * SR) && t0 + i < total; ++i)
            {
                const double t = i / SR;
                float v = rnd() * (float)std::exp(-t * (snare ? 22.0 : 90.0)) * (snare ? 0.45f : 0.12f);
                if (snare) v += (float)(0.25 * std::sin(6.2831853 * 190.0 * t) * std::exp(-t * 30.0));
                in[(size_t)(t0 + i)] += v;
            }
        }
    std::vector<float> padOnly((size_t)total, 0.0f);
    // Pad: Am | F | G | Am (i-VI-VII-i), one bar each, saw triads around C4.
    const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {57, 60, 64}};
    const int chordRootPc[4] = {9, 5, 7, 9};
    for (int b = 0; b < bars; ++b)
        for (int v = 0; v < 3; ++v)
        {
            const double hz = 440.0 * std::pow(2.0, (chords[b % 4][v] - 69) / 12.0);
            double ph = 0.0;
            const long long t0 = (long long)(b * 4 * spb), len = (long long)(4 * spb);
            for (long long i = 0; i < len && t0 + i < total; ++i)
            {
                ph += hz / SR; ph -= std::floor(ph);
                const float env = (float)std::fmin(1.0, std::fmin(i / (0.02 * SR), (len - i) / (0.02 * SR)));
                { const float v = (float)(2.0 * ph - 1.0) * 0.06f * env; in[(size_t)(t0 + i)] += v; padOnly[(size_t)(t0 + i)] += v; }
            }
        }

    auto render = [&](Largemouth& p, const std::vector<float>& src, int block, bool playing,
                      std::vector<float>& oL, std::vector<float>& oR)
    {
        g_notes.clear();
        oL.assign(src.size(), 0.0f); oR.assign(src.size(), 0.0f);
        std::vector<float> l(block), r(block);
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
            ctx.transport.ppqPos = playing ? pos / spb : 0.0;
            ctx.transport.timeSigNumerator = 4; ctx.transport.timeSigDenominator = 4;
            g_now = pos;
            p.process(ctx);
            for (int i = 0; i < n; ++i) { oL[(size_t)(pos + i)] = l[i]; oR[(size_t)(pos + i)] = r[i]; }
        }
    };
    auto countAnchorsOnKicks = [&](long long from, int tol, int& anchors, int& onKick)
    {
        anchors = onKick = 0;
        for (const NoteLog& nl : g_notes)
        {
            if (nl.type != kAnchor || nl.sample < from) continue;
            ++anchors;
            for (long long kt : kickTimes) if (nl.sample >= kt - tol && nl.sample <= kt + tol) { ++onKick; break; }
        }
    };
    const int block = 128;
    std::vector<float> outL, outR;

    for (float& v : in) v *= 0.5f;          // a drum track peaking around -6 dBFS
    for (float& v : padOnly) v *= 0.5f;

    Largemouth p;
    p.prepare(SR, 512);
    p.set(kListen, 0);            // Follow
    p.set(kKey, 2);               // deliberately wrong (D) until it hears the key
    p.set(kChanges, 0);           // Follow the chords
    p.reset();
    render(p, in, block, true, outL, outR);
    const std::vector<NoteLog> mainNotes = g_notes;
    const std::vector<float> mainL = outL, mainR = outR;
    std::printf("learned offsets (16ths): step0 %+.3f  step7 %+.3f (played +0.120)  step10 %+.3f\n",
                p.offset_[0], p.offset_[7], p.offset_[10]);
    check(std::fabs(p.offset_[0]) < 0.03f && std::fabs(p.offset_[7] - 0.12f) < 0.03f && std::fabs(p.offset_[10]) < 0.03f,
          "groove memory learned the late kick on step 7");

    // ---- what it heard ----
    std::printf("found key: %s%s, confidence %.2f\n", kNoteNames[p.harm_.foundRoot()], p.harm_.foundMinor() ? "m" : "", p.harm_.confidence());
    check(p.harm_.foundRoot() == 9 && p.harm_.foundMinor(), "key: hears A minor");
    check(p.keyRoot_ == 9, "key in use is A (not the D on the Key knob)");

    // ---- anchors after it has learned (bars 4..15) land on the kicks ----
    int anchors = 0, onTime = 0, rightRoot = 0, rootChecked = 0;
    for (const NoteLog& nl : g_notes)
    {
        if (nl.type != kAnchor || nl.sample < (long long)(4 * 4 * spb)) continue;
        ++anchors;
        long long best = 1LL << 40;
        for (long long kt : kickTimes) if (std::llabs(kt - nl.sample) < std::llabs(best)) best = kt - nl.sample;
        if (std::llabs(best) <= block + 64) ++onTime;
        else std::printf("  off-kick anchor at beat %.3f (nearest kick %+.3f beats)\n", nl.sample / spb, best / spb);      // logged per block: within one block
        const int bar = (int)(nl.sample / (4 * spb));
        const double inBar = nl.sample / (4 * spb) - bar;
        if (inBar > 0.3)                                    // allow the detector a beat to hear a change
        {
            ++rootChecked;
            if (((int)std::lround(nl.midi) % 12) == chordRootPc[bar % 4] || ((int)std::lround(nl.midi) % 12) == chordRootPc[bar % 4]) ++rightRoot;
        }
    }
    std::printf("anchors after bar 4: %d (kicks there: %d), on a kick: %d, right chord root: %d/%d\n",
                anchors, 12 * 3, onTime, rightRoot, rootChecked);
    for (long long kt : kickTimes)
    {
        if (kt < (long long)(4 * 4 * spb)) continue;
        bool hit = false;
        for (const NoteLog& nl : mainNotes) if (nl.type == kAnchor && std::llabs(nl.sample - kt) <= block + 64) hit = true;
        if (!hit) std::printf("  kick without a note at beat %.3f\n", kt / spb);
    }
    check(anchors >= 33 && anchors <= 38, "about one anchor per kick");
    check(onTime >= anchors - 1, "anchors land on the kicks");
    check(rootChecked > 0 && rightRoot >= (int)(0.8 * rootChecked), "anchors play the chord root (80%+)");

    int extras = 0; for (const NoteLog& nl : g_notes) if (nl.type == kExtra) ++extras;
    std::printf("extra (Busy) notes: %d\n", extras);
    check(extras > 0, "Busy adds notes between kicks");

    // Print the first bars' notes for a human to read.
    for (const NoteLog& nl : g_notes)
    {
        const double beat = nl.sample / spb;
        if (beat > 24) break;
        std::printf("  bar %2d beat %.2f  %s %s%d\n", (int)(beat / 4) + 1, std::fmod(beat, 4.0) + 1, nl.type == kAnchor ? "KICK " : "extra",
                    kNoteNames[(int)std::lround(nl.midi) % 12], (int)std::lround(nl.midi) / 12 - 1);
    }


    // ---- Hits mode, transport stopped: a note on every kick, ~9 ms late ----
    {
        Largemouth h; h.prepare(SR, 512); h.set(kFollow, 1); h.set(kBusy, 0); h.reset();
        std::vector<float> a, b;
        render(h, in, 32, false, a, b);
        int an, on; countAnchorsOnKicks(0, (int)(0.02 * SR), an, on);
        std::printf("Hits, stopped, 32-frame blocks: %d notes for %d kicks, %d within 20 ms of a kick\n", an, (int)kickTimes.size(), on);
        check(an >= (int)kickTimes.size() - 2 && an <= (int)kickTimes.size() && on == an, "Hits mode: one note per kick");
    }
    // ---- Grid mode with the transport stopped falls back to hits ----
    {
        Largemouth h; h.prepare(SR, 512); h.reset();
        std::vector<float> a, b;
        render(h, in, 512, false, a, b);
        int an, on; countAnchorsOnKicks(0, (int)(0.02 * SR), an, on);
        check(an >= (int)kickTimes.size() - 2 && on == an, "Grid mode, transport stopped: still follows the kicks");
    }
    // ---- drums only, Listen Off, a progression: roots follow 1-6-7-1 in G minor ----
    {
        std::vector<float> drums((size_t)total, 0.0f);
        for (size_t i = 0; i < drums.size(); ++i) drums[i] = in[i] - padOnly[i];
        Largemouth h; h.prepare(SR, 512);
        h.set(kListen, 2); h.set(kKey, 7); h.set(kScale, 0); h.set(kChanges, 2); h.set(kChordLen, 1); h.set(kBusy, 0);
        h.reset();
        std::vector<float> a, b;
        render(h, drums, block, true, a, b);
        const int want[4] = {7, 3, 5, 7};     // G, Eb, F, G
        int ok = 0, n = 0;
        for (const NoteLog& nl : g_notes)
        {
            if (nl.type != kAnchor) continue;
            const int bar = (int)(nl.sample / (4 * spb) + 0.01);
            ++n; if (((int)std::lround(nl.midi)) % 12 == want[bar % 4]) ++ok;
        }
        std::printf("drums only, G minor 1-6-7-1: %d/%d roots right\n", ok, n);
        check(n > 0 && ok == n, "progression roots without any harmony to hear");
    }
    // ---- levels: the bass alone, per voice, against the input ----
    {
        float inPk = 0.0f; for (float v : in) inPk = std::fmax(inPk, std::fabs(v));
        std::printf("input peak %.1f dBFS\n", 20.0 * std::log10(inPk));
        const char* names[5] = {"Sub", "808", "Pluck", "Acid", "Reese"};
        for (int v = 0; v < 5; ++v)
        {
            Largemouth h; h.prepare(SR, 512); h.set(kVoice, (float)v); h.set(kSolo, 1); h.reset();
            std::vector<float> a, b;
            render(h, in, block, true, a, b);
            float pk = 0.0f; double sq = 0.0;
            for (float x : a) { pk = std::fmax(pk, std::fabs(x)); sq += (double)x * x; }
            std::printf("  bass only, %-5s peak %5.1f dBFS  rms %5.1f dBFS\n", names[v], 20.0 * std::log10(pk), 10.0 * std::log10(sq / a.size()));
        }
    }
    // ---- CPU, roughly: time the main render ----
    {
        Largemouth h; h.prepare(SR, 512); h.set(kVoice, 4); h.reset();
        std::vector<float> a, b;
        const auto t0 = std::chrono::steady_clock::now();
        render(h, in, block, true, a, b);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("CPU (desktop, Reese voice): %.2f%% of real time\n", 100.0 * sec / (total / SR));
    }

    if (argc > 1) { writeWav(argv[1], mainL, mainR); std::printf("wrote %s\n", argv[1]); }
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
