// Does Driftwood play the right chords, strummed the right way, in time? Checks the chord on each
// degree in several keys and scales, snapping and out-of-key chords, the pad map, voice leading,
// strum order and timing, Pattern locked to the transport (and free when stopped), Hold, the voice
// limit, tuning, levels and CPU. Then renders a demo WAV: a short progression on every program.
//
//   mkdir -p tests/build
//   clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
//   tests/build/listentest build/driftwood-demo.wav
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <memory>
#include <string>
#include <chrono>
#include <algorithm>
#include <map>

struct Logged { long long t; int pitch; float vel; uint32_t chord; };
static std::vector<Logged> g_log;
#define DRIFTWOOD_NOTE_HOOK(t, p, v, c) g_log.push_back({(long long)(t), (p), (v), (c)})

#define private public
#define protected public
#include "../src/Driftwood.cpp"
#undef private
#undef protected

using namespace driftwood;

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

// ---- a running session: renders on from where it left off --------------------------------------
struct Ev { double t; mpc::MidiEvent e; };
static Ev on(double t, int note, int vel = 100) { return {t, mpc::MidiEvent::noteOn(0, note, vel)}; }
static Ev off(double t, int note) { return {t, mpc::MidiEvent::noteOff(0, note)}; }

// Unless a test is about a program's own settings, start from plain block triads in close voicing.
static bool g_testBase = true;

struct Session
{
    std::unique_ptr<Driftwood> p;
    std::vector<float> L, R;
    long long pos = 0;
    bool playing = false;
    double bpm = 120.0, ppqAtStart = 0.0;
    long long playStart = 0;
    int fixedBlock = 0;
    unsigned rs = 12345;
    int maxLive = 0, maxSlots = 0;
    double cpu = 0.0;
    bool bad = false;

    explicit Session(int prog = 0, std::initializer_list<std::pair<int, float>> overrides = {})
    {
        p = std::make_unique<Driftwood>();
        p->prepare(SR, 512);
        const mpc::Program& pr = p->programs_[(size_t)prog];
        for (int i = 0; i < p->numParams(); ++i) p->set(i, pr.plainValues[(size_t)i]);
        if (g_testBase) { p->set(Driftwood::kQuality, kTriad); p->set(Driftwood::kVoicing, kClose); }
        for (const auto& o : overrides) p->set(o.first, o.second);
        p->reset();
        g_log.clear();
    }
    void set(int param, float v) { p->set(param, v); }
    void play(bool on, double startPpq = 0.0) { playing = on; ppqAtStart = startPpq; playStart = pos; }

    void run(double untilSec, std::vector<Ev> evs = {})
    {
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
        const long long end = (long long)(untilSec * SR);
        size_t ei = 0;
        std::vector<mpc::MidiEvent> be;
        while (pos < end)
        {
            rs = rs * 1664525u + 1013904223u;
            const int block = (int)std::min<long long>(fixedBlock > 0 ? fixedBlock : 32 + (int)((rs >> 8) % 481), end - pos);
            be.clear();
            while (ei < evs.size() && (long long)std::llround(evs[ei].t * SR) < pos + block)
            {
                mpc::MidiEvent e = evs[ei].e;
                e.sampleOffset = (int)std::max<long long>(0, std::llround(evs[ei].t * SR) - pos);
                be.push_back(e); ++ei;
            }
            L.resize((size_t)(pos + block)); R.resize((size_t)(pos + block));
            float* outs[2] = {L.data() + pos, R.data() + pos};
            for (int i = 0; i < block; ++i) outs[0][i] = outs[1][i] = 0.0f;
            mpc::ProcessContext ctx;
            ctx.outputs = outs; ctx.numInputs = 0; ctx.numOutputs = 2; ctx.numFrames = block;
            ctx.midi = be.data(); ctx.numMidiEvents = (int)be.size();
            ctx.sampleRate = SR;
            ctx.transport.valid = ctx.transport.tempoValid = true; ctx.transport.bpm = bpm;
            ctx.transport.playing = playing; ctx.transport.ppqValid = playing;
            ctx.transport.ppqPos = ppqAtStart + (double)(pos - playStart) / SR * bpm / 60.0;
            const auto t0 = std::chrono::steady_clock::now();
            p->process(ctx);
            cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            int live = 0, slots = 0;
            for (auto& v : p->voices_) if (v.stage != kStOff) { ++slots; if (!v.fading) ++live; }
            maxLive = std::max(maxLive, live); maxSlots = std::max(maxSlots, slots);
            for (int i = 0; i < block; ++i) if (!std::isfinite(outs[0][i]) || !std::isfinite(outs[1][i])) bad = true;
            pos += block;
        }
    }
};

// Notes started, grouped by chord id, in start order.
static std::map<uint32_t, std::vector<Logged>> byChord()
{
    std::map<uint32_t, std::vector<Logged>> m;
    for (const auto& l : g_log) m[l.chord].push_back(l);
    return m;
}
static std::vector<int> pitchesOf(const std::vector<Logged>& v)
{
    std::vector<int> p; for (const auto& l : v) p.push_back(l.pitch);
    std::sort(p.begin(), p.end()); return p;
}
static std::string str(const std::vector<int>& v)
{
    std::string s; for (int x : v) { if (!s.empty()) s += " "; s += std::to_string(x); } return s;
}
static std::vector<int> ivs(const Chord& c) { return std::vector<int>(c.iv, c.iv + c.n); }

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
static float db(float x) { return 20.0f * std::log10(std::max(x, 1.0e-9f)); }

// Hann-windowed DFT magnitude scan: the frequency of the strongest component in [lo, hi].
static double peakFreq(const std::vector<float>& x, size_t from, size_t len, double lo, double hi, double step)
{
    double best = 0.0, bestF = lo;
    for (double f = lo; f <= hi; f += step)
    {
        double re = 0.0, im = 0.0;
        const double w = 6.283185307179586 * f / SR;
        for (size_t i = 0; i < len; ++i)
        {
            const double h = 0.5 - 0.5 * std::cos(6.283185307179586 * (double)i / (double)(len - 1));
            re += h * x[from + i] * std::cos(w * (double)i); im += h * x[from + i] * std::sin(w * (double)i);
        }
        const double m = re * re + im * im;
        if (m > best) { best = m; bestF = f; }
    }
    return bestF;
}

int main(int argc, char** argv)
{
    using P = Driftwood;

    // ================================================================================================
    std::printf("\n== Chord on each degree (theory) ==\n");
    {
        const char* names[7] = {"I", "ii", "iii", "IV", "V", "vi", "vii"};
        const std::vector<std::vector<int>> cMajor = {{0,4,7},{0,3,7},{0,3,7},{0,4,7},{0,4,7},{0,3,7},{0,3,6}};
        bool ok = true;
        for (int d = 0; d < 7; ++d)
        {
            Chord c; diatonicIntervals(kMajor, d, kTriad, c);
            if (ivs(c) != cMajor[(size_t)d]) { ok = false; std::printf("   C major %s: %s\n", names[d], str(ivs(c)).c_str()); }
        }
        check(ok, "major-key triads: I ii iii IV V vi vii-dim have the right qualities");
        Chord c;
        diatonicIntervals(kMinor, 4, kTriad, c);      check(ivs(c) == std::vector<int>{0,3,7}, "natural minor v is minor (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kHarmMinor, 4, kTriad, c);  check(ivs(c) == std::vector<int>{0,4,7}, "harmonic minor V is major (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kHarmMinor, 6, kTriad, c);  check(ivs(c) == std::vector<int>{0,3,6}, "harmonic minor vii is diminished (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kDorian, 3, kTriad, c);     check(ivs(c) == std::vector<int>{0,4,7}, "dorian IV is major (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kPhrygian, 1, kTriad, c);   check(ivs(c) == std::vector<int>{0,4,7}, "phrygian bII is major (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kLydian, 1, kTriad, c);     check(ivs(c) == std::vector<int>{0,4,7}, "lydian II is major (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMixolydian, 0, kSeventh, c); check(ivs(c) == std::vector<int>{0,4,7,10}, "mixolydian I7 is dominant (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMelMinor, 0, kSeventh, c); check(ivs(c) == std::vector<int>{0,3,7,11}, "melodic minor i is minor-major 7 (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMajor, 0, kSeventh, c);    check(ivs(c) == std::vector<int>{0,4,7,11}, "major Imaj7 (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMajor, 4, kSeventh, c);    check(ivs(c) == std::vector<int>{0,4,7,10}, "major V7 (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMajor, 1, kSeventh, c);    check(ivs(c) == std::vector<int>{0,3,7,10}, "major ii7 (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMajor, 6, kSeventh, c);    check(ivs(c) == std::vector<int>{0,3,6,10}, "major vii half-diminished (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMajor, 0, kNinth, c);      check(ivs(c) == std::vector<int>{0,4,7,11,14}, "major Imaj9 (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMinor, 0, kNinth, c);      check(ivs(c) == std::vector<int>{0,3,7,10,14}, "minor im9 (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMajor, 2, kSus2, c);       check(ivs(c) == std::vector<int>{0,2,7}, "sus2 is a true sus2 on iii (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMajor, 3, kSus4, c);       check(ivs(c) == std::vector<int>{0,5,7}, "sus4 is a true sus4 on IV (%s)", str(ivs(c)).c_str());
        diatonicIntervals(kMajor, 5, kAdd9, c);       check(ivs(c) == std::vector<int>{0,3,7,14}, "vi add9 (%s)", str(ivs(c)).c_str());

        // Keys map: snapping and own chords (C major).
        c = resolveChord(61, 0, kMajor, kTriad, kMapKeys, kSnap, 60);  check(c.root == 60 && c.degree == 0, "Snap: C# in C major plays C (tie snaps down)");
        c = resolveChord(63, 0, kMajor, kTriad, kMapKeys, kSnap, 60);  check(c.root == 62 && c.degree == 1, "Snap: Eb in C major plays Dm");
        c = resolveChord(66, 0, kMajor, kTriad, kMapKeys, kSnap, 60);  check(c.root == 65 && c.degree == 3, "Snap: F# in C major plays F");
        c = resolveChord(70, 0, kMajor, kTriad, kMapKeys, kOwnChord, 60); check(c.root == 70 && ivs(c) == std::vector<int>{0,4,7}, "Own Chord: Bb in C major plays Bb major (bVII)");
        c = resolveChord(68, 0, kMajor, kSeventh, kMapKeys, kOwnChord, 60); check(ivs(c) == std::vector<int>{0,4,7,10}, "Own Chord 7th: Ab7");
        c = resolveChord(70, 9, kHarmMinor, kTriad, kMapKeys, kSnap, 60); check(c.root == 69, "Snap in A harmonic minor: Bb goes to A");
        // Pads map.
        c = resolveChord(36, 9, kMajor, kTriad, kMapPads, kSnap, 60); check(c.root == 57 && c.degree == 0, "Pads, A major, Register C4: pad 1 (36) = I on A3");
        c = resolveChord(42, 9, kMajor, kTriad, kMapPads, kSnap, 60); check(c.root == 68 && c.degree == 6, "Pads: pad 7 (42) = vii (G#)");
        c = resolveChord(43, 9, kMajor, kTriad, kMapPads, kSnap, 60); check(c.root == 69 && c.degree == 0, "Pads: pad 8 (43) = I an octave up");
        c = resolveChord(51, 9, kMajor, kTriad, kMapPads, kSnap, 60); check(c.root == 83 && c.degree == 1, "Pads: pad 16 (51) = ii two octaves up");
        c = resolveChord(35, 0, kMajor, kTriad, kMapPads, kSnap, 60); check(c.root == 59 && c.degree == 6, "Pads: note 35 = vii below pad 1");

        // Voicings.
        int v[6];
        diatonicIntervals(kMajor, 0, kTriad, c); c.root = 60;
        voiceChord(c, 60, 0, kClose, v);  check(std::vector<int>(v, v + 3) == std::vector<int>{60,64,67}, "close C: C4 E4 G4");
        voiceChord(c, 60, 1, kClose, v);  check(std::vector<int>(v, v + 3) == std::vector<int>{64,67,72}, "1st inversion: E4 G4 C5");
        voiceChord(c, 60, 2, kClose, v);  check(std::vector<int>(v, v + 3) == std::vector<int>{67,72,76}, "2nd inversion: G4 C5 E5");
        voiceChord(c, 60, 0, kOpen, v);   check(std::vector<int>(v, v + 3) == std::vector<int>{52,60,67}, "open (drop-2): E3 C4 G4");
        voiceChord(c, 60, 0, kSpread, v); check(std::vector<int>(v, v + 3) == std::vector<int>{60,67,76}, "spread: C4 G4 E5");
        diatonicIntervals(kMajor, 0, kTriad, c); c.root = 60; voiceChord(c, 60, 0, kClose, v);
        check(bassNote(c, v, 1) == 48 && bassNote(c, v, 2) == 36, "bass: C3 (-1 oct) and C2 (-2 oct) under C4 E4 G4");
        voiceChord(c, 60, 1, kClose, v);
        check(bassNote(c, v, 1) == 60 && bassNote(c, v, 2) == 48, "bass under 1st inversion: C4 / C3 (the root just under E4)");
    }

    {
        // Choice parameters show their own names (a custom value text must never hijack them).
        Driftwood d; bool ok = true; std::string t;
        for (int i = 0; i < d.numParams(); ++i)
            if (d.param(i).kind == mpc::Param::Kind::Choice && d.formatValue(i, 1.0f, t)) { ok = false; std::printf("   %s -> %s\n", d.param(i).name.c_str(), t.c_str()); }
        check(ok, "every choice parameter shows its choice names");
    }

    // ================================================================================================
    std::printf("\n== Chords through the plugin (Keys map, Voice Lead off) ==\n");
    {
        struct Case { int key, scale, quality, note; std::vector<int> want; const char* what; };
        const std::vector<Case> cases = {
            {0, kMajor, kTriad, 62, {62,65,69}, "C major, D -> Dm (ii)"},
            {10, kMajor, kTriad, 63, {63,67,70}, "Bb major, Eb -> Eb (IV)"},
            {4, kMinor, kTriad, 71, {71,74,78}, "E minor, B -> Bm (v)"},
            {4, kHarmMinor, kTriad, 71, {71,75,78}, "E harmonic minor, B -> B (V)"},
            {7, kMajor, kSeventh, 62, {62,66,69,72}, "G major 7th, D -> D7 (V7)"},
            {2, kDorian, kNinth, 62, {62,65,69,72,76}, "D dorian 9th, D -> Dm9"},
            {9, kMinor, kSus4, 64, {64,69,71}, "A minor sus4, E -> Esus4"},
            {5, kLydian, kAdd9, 67, {67,71,74,81}, "F lydian add9, G -> G add9 (II)"},
        };
        for (const auto& k : cases)
        {
            Session s(0, {{P::kKey, (float)k.key}, {P::kScale, (float)k.scale}, {P::kQuality, (float)k.quality},
                          {P::kLead, 0}, {P::kVoicing, kClose}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLooseVel, 0}});
            s.run(0.4, {on(0.05, k.note), off(0.3, k.note)});
            const auto got = pitchesOf(g_log);
            check(got == k.want, "%s: %s", k.what, str(got).c_str());
        }
        Session s(0, {{P::kLead, 0}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kBass, 2}});
        s.run(0.4, {on(0.05, 67), off(0.3, 67)});
        check(pitchesOf(g_log) == std::vector<int>{43,67,71,74}, "Bass -2 Oct under G major: G2 + G4 B4 D5 (%s)", str(pitchesOf(g_log)).c_str());
        Session s2(0, {{P::kMap, kMapPads}, {P::kKey, 9}, {P::kLead, 0}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kVoicing, kClose}});
        s2.run(0.6, {on(0.05, 36), off(0.2, 36), on(0.3, 40), off(0.5, 40)});
        auto bc = byChord();
        std::vector<std::vector<int>> chords; for (auto& kv : bc) chords.push_back(pitchesOf(kv.second));
        check(chords.size() == 2 && chords[0] == std::vector<int>{57,61,64} && chords[1] == std::vector<int>{64,68,71},
              "Pads map in A major: pad 1 = A (A3 C#4 E4), pad 5 = E (E4 G#4 B4)");
    }

    // ================================================================================================
    std::printf("\n== Voice leading ==\n");
    {
        const std::vector<int> prog = {60, 69, 65, 67, 60, 64, 65, 62, 67, 60};   // I vi IV V I iii IV ii V I
        auto runProg = [&](bool lead, std::vector<std::vector<int>>& out)
        {
            Session s(0, {{P::kLead, lead ? 1.0f : 0.0f}, {P::kVoicing, kClose}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kQuality, kTriad}});
            std::vector<Ev> ev;
            for (size_t i = 0; i < prog.size(); ++i) { ev.push_back(on(0.1 + 0.5 * (double)i, prog[i])); ev.push_back(off(0.5 + 0.5 * (double)i, prog[i])); }
            s.run(0.5 * (double)prog.size() + 0.5, ev);
            out.clear();
            for (auto& kv : byChord()) out.push_back(pitchesOf(kv.second));
        };
        std::vector<std::vector<int>> led, raw;
        runProg(true, led); runProg(false, raw);
        int maxMove = 0, totLed = 0, totRaw = 0;
        for (size_t i = 1; i < led.size(); ++i)
        {
            for (size_t j = 0; j < led[i].size() && j < led[i - 1].size(); ++j) maxMove = std::max(maxMove, std::abs(led[i][j] - led[i - 1][j]));
            totLed += movement(led[i - 1].data(), (int)led[i - 1].size(), led[i].data(), (int)led[i].size());
            totRaw += movement(raw[i - 1].data(), (int)raw[i - 1].size(), raw[i].data(), (int)raw[i].size());
        }
        std::printf("   led:"); for (auto& c : led) std::printf(" [%s]", str(c).c_str()); std::printf("\n");
        check(led.size() == prog.size(), "%zu chords played", led.size());
        check(maxMove <= 4, "with Voice Lead no voice moves more than 4 semitones between chords (max %d)", maxMove);
        check(totLed * 2 < totRaw, "Voice Lead moves the hands less than half as far as root positions (%d vs %d)", totLed, totRaw);
        // A long random walk stays near Register.
        Session s(0, {{P::kLead, 1}, {P::kQuality, kSeventh}, {P::kVoicing, kOpen}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kRegister, 60}});
        std::vector<Ev> ev; unsigned r = 7;
        for (int i = 0; i < 60; ++i) { r = r * 1103515245u + 12345u; const int n = 48 + (int)((r >> 16) % 36); ev.push_back(on(0.05 + 0.1 * i, n)); ev.push_back(off(0.1 + 0.1 * i, n)); }
        s.run(6.5, ev);
        float worst = 0.0f;
        for (auto& kv : byChord()) { auto c = pitchesOf(kv.second); worst = std::max(worst, std::fabs(0.5f * (float)(c.front() + c.back()) - 60.0f)); }
        check(worst <= 7.0f, "60 random chords: every chord's centre stays within 7 semitones of Register (worst %.1f)", worst);
    }

    // ================================================================================================
    std::printf("\n== Strum order and timing ==\n");
    {
        auto strum = [&](int dir, float ms, int vel, std::initializer_list<std::pair<int, float>> extra, int chords)
        {
            Session s(0, {{P::kDirection, (float)dir}, {P::kStrum, ms}, {P::kLooseTime, 0}, {P::kLooseVel, 0}, {P::kVelStrum, 0},
                          {P::kLead, 0}, {P::kVoicing, kClose}});
            for (const auto& e : extra) s.set(e.first, e.second);
            std::vector<Ev> ev;
            for (int i = 0; i < chords; ++i) { ev.push_back(on(0.1 + 0.6 * i, 60, vel)); ev.push_back(off(0.5 + 0.6 * i, 60)); }
            s.run(0.6 * chords + 0.2, ev);
            return byChord();
        };
        auto bc = strum(kDown, 90, 100, {}, 1);
        auto c = bc.begin()->second;
        bool asc = c.size() == 3 && c[0].pitch < c[1].pitch && c[1].pitch < c[2].pitch;
        const long long t0 = (long long)std::llround(0.1 * SR);
        const long long gap = (long long)std::llround(0.045 * SR);
        check(asc, "Down strum plays low to high (%d %d %d)", c[0].pitch, c[1].pitch, c[2].pitch);
        check(std::llabs(c[0].t - t0) <= 0 && std::llabs(c[1].t - (t0 + gap)) <= 1 && std::llabs(c[2].t - (t0 + 2 * gap)) <= 1,
              "Strum 90 ms over 3 notes: starts at 0 / 45 / 90 ms (got %.1f / %.1f / %.1f ms)",
              (c[0].t - t0) * 1000.0 / SR, (c[1].t - t0) * 1000.0 / SR, (c[2].t - t0) * 1000.0 / SR);
        bc = strum(kUp, 90, 100, {}, 1); c = bc.begin()->second;
        check(c[0].pitch > c[1].pitch && c[1].pitch > c[2].pitch, "Up strum plays high to low (%d %d %d)", c[0].pitch, c[1].pitch, c[2].pitch);
        bc = strum(kAlternate, 60, 100, {}, 3);
        std::vector<bool> downs; for (auto& kv : bc) downs.push_back(kv.second[0].pitch < kv.second[1].pitch);
        check(downs.size() == 3 && downs[0] && !downs[1] && downs[2], "Alternate: down, up, down");
        bc = strum(kByVelocity, 60, 40, {}, 1); c = bc.begin()->second;
        bool softUp = c[0].pitch > c[1].pitch;
        bc = strum(kByVelocity, 60, 110, {}, 1); c = bc.begin()->second;
        check(softUp && c[0].pitch < c[1].pitch, "By velocity: soft hits strum up, hard hits down");
        bc = strum(kDown, 100, 127, {{P::kVelStrum, 100}}, 1); c = bc.begin()->second;
        const double fast = (c.back().t - c.front().t) * 1000.0 / SR;
        bc = strum(kDown, 100, 30, {{P::kVelStrum, 100}}, 1); c = bc.begin()->second;
        const double slow = (c.back().t - c.front().t) * 1000.0 / SR;
        check(fast < 50.0 && slow > 130.0, "Vel>Strum 100: a hard hit strums in %.0f ms, a soft one in %.0f ms (Strum 100 ms)", fast, slow);
        // Humanise: the spread is bounded and not all identical.
        {
            Session s(0, {{P::kStrum, 0}, {P::kLooseTime, 20}, {P::kLooseVel, 50}, {P::kLead, 0}});
            std::vector<Ev> ev; for (int i = 0; i < 20; ++i) { ev.push_back(on(0.1 + 0.3 * i, 60, 100)); ev.push_back(off(0.3 + 0.3 * i, 60)); }
            s.run(6.3, ev);
            double maxLate = 0.0; float vmin = 1.0f, vmax = 0.0f;
            int i = 0;
            for (auto& kv : byChord())
            {
                const long long t0c = (long long)std::llround((0.1 + 0.3 * i) * SR); ++i;
                for (auto& l : kv.second) { maxLate = std::max(maxLate, (l.t - t0c) * 1000.0 / SR); vmin = std::min(vmin, l.vel); vmax = std::max(vmax, l.vel); }
            }
            check(maxLate <= 20.5 && maxLate > 10.0, "Loose Time 20 ms: notes up to %.1f ms late, never more than 20", maxLate);
            check(vmax - vmin > 0.15f && vmax <= 1.0f, "Loose Vel 50 %%: velocities spread %.2f..%.2f", vmin, vmax);
        }
    }

    // ================================================================================================
    std::printf("\n== Pattern ==\n");
    {
        // Locked to the transport: 100 BPM, Eighths, the pad held for two bars from the downbeat.
        Session s(0, {{P::kPattern, 2}, {P::kStrum, 10}, {P::kLooseTime, 0}, {P::kLooseVel, 0}, {P::kLead, 0}});
        s.bpm = 100.0; s.play(true, 0.0);
        const double spb = SR * 60.0 / 100.0;
        const double bar = 4.0 * 60.0 / 100.0;
        s.run(2.0 * bar + 0.3, {on(0.0, 60, 100), off(2.0 * bar - 0.01, 60)});
        auto bc = byChord();
        int onGrid = 0, n = 0; double worst = 0.0;
        for (auto& kv : bc)
        {
            const long long t = kv.second.front().t;
            const double eighth = t / (spb * 0.5);
            const double err = std::fabs(eighth - std::round(eighth)) * spb * 0.5;
            worst = std::max(worst, err);
            if (err <= 1.0) ++onGrid;
            ++n;
        }
        check(n == 16, "Eighths over two bars: 16 strokes (%d)", n);
        check(onGrid == n, "every stroke starts on an eighth note of the song, within 1 sample (worst %.2f samples)", worst);
        int ups = 0; for (auto& kv : bc) if (kv.second.size() > 1 && kv.second[0].pitch > kv.second[1].pitch) ++ups;
        check(ups == 8, "half of them are up strokes (%d)", ups);
        check(peakOf(s.L, (size_t)(2.0 * bar * SR + 0.25 * SR)) < 0.01f, "releasing the pad stops the pattern");

        // Swing: Gallop, 66 %: the off-beat sixteenth (step 3) lands a third of a sixteenth late.
        Session sw(0, {{P::kPattern, 6}, {P::kSwing, 66.6667f}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLead, 0}});
        sw.bpm = 120.0; sw.play(true, 0.0);
        sw.run(2.2, {on(0.0, 60), off(2.0, 60)});
        const double six = SR * 60.0 / 120.0 / 4.0;
        std::vector<long long> starts; for (auto& kv : byChord()) starts.push_back(kv.second.front().t);
        // Gallop "D.DUD.DU": steps 0, 2, 3, 4, ...
        const double want3 = 3.0 * six + (0.666667 - 0.5) * 2.0 * six;
        check(starts.size() > 4 && std::fabs((double)starts[2] - want3) <= 1.5, "Swing 66 %%: step 3 at %.1f samples (want %.1f)", starts.size() > 2 ? (double)starts[2] : -1.0, want3);
        check(starts.size() > 4 && std::fabs((double)starts[1] - 2.0 * six) <= 1.0, "Swing leaves the even steps on the grid");

        // Song starting at bar 3 (ppq 8) with the pad already down: the first stroke is on the downbeat.
        Session st(0, {{P::kPattern, 1}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLead, 0}});
        st.bpm = 90.0;
        st.run(0.5, {on(0.2, 62)});          // stopped: free-running from the note
        auto b1 = byChord();
        check(b1.size() == 1 && b1.begin()->second.front().t == (long long)std::llround(0.2 * SR), "transport stopped: the first stroke is on the note itself");
        g_log.clear();
        st.play(true, 8.0);
        st.run(0.5 + 2.0 * 60.0 / 90.0 + 0.05);
        auto b2 = byChord();
        std::vector<long long> ts; for (auto& kv : b2) ts.push_back(kv.second.front().t);
        const long long playAt = (long long)std::llround(0.5 * SR), q = (long long)std::llround(SR * 60.0 / 90.0);
        check(ts.size() == 3 && std::llabs(ts[0] - playAt) <= 1 && std::llabs(ts[1] - (playAt + q)) <= 1 && std::llabs(ts[2] - (playAt + 2 * q)) <= 1,
              "transport starts at ppq 8: strokes on each beat from the downbeat (%zu strokes)", ts.size());

        // Free mode: 120 BPM, Quarters, note at 0.3 s -> strokes at 0.3, 0.8, 1.3 s.
        Session fr(0, {{P::kPattern, 1}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLead, 0}});
        fr.bpm = 120.0;
        fr.run(1.5, {on(0.3, 60), off(1.45, 60)});
        std::vector<long long> ft; for (auto& kv : byChord()) ft.push_back(kv.second.front().t);
        check(ft.size() == 3 && std::llabs(ft[0] - 13230) <= 1 && std::llabs(ft[1] - 35280) <= 1 && std::llabs(ft[2] - 57330) <= 1,
              "free-running at the song tempo from the first note (0.3 / 0.8 / 1.3 s)");

        // A pad hit 20 ms late on a playing transport plays that stroke at once, then locks to the grid.
        Session lt(0, {{P::kPattern, 1}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLead, 0}});
        lt.bpm = 120.0; lt.play(true, 0.0);
        lt.run(1.3, {on(0.52, 60), off(1.25, 60)});
        std::vector<long long> lts; for (auto& kv : byChord()) lts.push_back(kv.second.front().t);
        check(lts.size() == 2 && std::llabs(lts[0] - (long long)std::llround(0.52 * SR)) <= 1 && std::llabs(lts[1] - 44100) <= 1,
              "late by 20 ms: the beat is caught at once, the next stroke is on the grid");

        // Changing chord while the pattern runs: the next stroke plays the new chord.
        Session ch(0, {{P::kPattern, 2}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLead, 0}, {P::kHold, 1}});
        ch.bpm = 120.0; ch.play(true, 0.0);
        ch.run(2.0, {on(0.0, 60), off(0.1, 60), on(1.1, 65), off(1.2, 65)});
        auto cb = byChord();
        int beforeF = 0, afterF = 0;
        for (auto& kv : cb)
        {
            const bool isF = std::find_if(kv.second.begin(), kv.second.end(), [](const Logged& l) { return l.pitch % 12 == 5; }) != kv.second.end();
            if (kv.second.front().t < (long long)(1.1 * SR)) beforeF += isF; else afterF += !isF;
        }
        check(beforeF == 0 && afterF == 0 && cb.size() >= 7, "Hold + Pattern: strokes play C until the F pad, then F (%zu strokes)", cb.size());
        check(peakOf(ch.L, (size_t)(1.8 * SR)) > 0.01f, "Hold keeps the pattern going after the pads are released");
    }

    // ================================================================================================
    std::printf("\n== Arpeggiator ==\n");
    {
        auto arpRun = [&](int mode, int rate, int oct, std::initializer_list<std::pair<int, float>> extra, double bars,
                          double bpm, bool play, std::vector<long long>* times = nullptr)
        {
            Session s(0, {{P::kArp, (float)mode}, {P::kArpRate, (float)rate}, {P::kArpOct, (float)oct}, {P::kStrum, 0},
                          {P::kLooseTime, 0}, {P::kLooseVel, 0}, {P::kLead, 0}, {P::kRelease, 80}});
            for (const auto& e : extra) s.set(e.first, e.second);
            s.bpm = bpm; if (play) s.play(true, 0.0);
            const double bar = 4.0 * 60.0 / bpm;
            s.run(bars * bar + 0.2, {on(play ? 0.0 : 0.1, 60, 100), off((play ? 0.0 : 0.1) + bars * bar - 0.005, 60)});
            std::vector<int> seq;
            if (times) times->clear();
            for (const auto& l : g_log) { seq.push_back(l.pitch); if (times) times->push_back(l.t); }
            return seq;
        };
        auto first = [](const std::vector<int>& v, size_t n) { return std::vector<int>(v.begin(), v.begin() + (long)std::min(n, v.size())); };
        std::vector<long long> ts;
        auto seq = arpRun(kArpUp, 3, 1, {}, 1.0, 120.0, true, &ts);
        check(first(seq, 7) == std::vector<int>{60,64,67,60,64,67,60}, "Up: %s", str(first(seq, 7)).c_str());
        const double six = SR * 60.0 / 120.0 / 4.0;
        double worst = 0.0;
        for (size_t i = 0; i < ts.size(); ++i) worst = std::max(worst, std::fabs((double)ts[i] - six * (double)i));
        check(seq.size() == 16 && worst <= 1.0, "1/16 at 120 BPM: 16 notes in a bar, each on its sixteenth within 1 sample (worst %.2f)", worst);
        seq = arpRun(kArpDown, 3, 1, {}, 0.5, 120.0, true);
        check(first(seq, 4) == std::vector<int>{67,64,60,67}, "Down: %s", str(first(seq, 4)).c_str());
        seq = arpRun(kArpUpDown, 3, 1, {}, 0.5, 120.0, true);
        check(first(seq, 8) == std::vector<int>{60,64,67,64,60,64,67,64}, "Up-Down: %s", str(first(seq, 8)).c_str());
        seq = arpRun(kArpDownUp, 3, 1, {}, 0.5, 120.0, true);
        check(first(seq, 6) == std::vector<int>{67,64,60,64,67,64}, "Down-Up: %s", str(first(seq, 6)).c_str());
        seq = arpRun(kArpUp, 3, 2, {}, 0.5, 120.0, true);
        check(first(seq, 7) == std::vector<int>{60,64,67,72,76,79,60}, "Up over 2 octaves: %s", str(first(seq, 7)).c_str());
        seq = arpRun(kArpThumb, 3, 1, {{P::kBass, 1}}, 0.5, 120.0, true);
        check(first(seq, 8) == std::vector<int>{48,60,48,64,48,67,48,60}, "Thumb with Bass: the bass alternates with the chord going up: %s", str(first(seq, 8)).c_str());
        seq = arpRun(kArpRandom, 3, 2, {}, 2.0, 120.0, true);
        bool inChord = true, noRepeat = true; std::vector<int> used;
        for (size_t i = 0; i < seq.size(); ++i)
        {
            const int pc = seq[i] % 12; if (pc != 0 && pc != 4 && pc != 7) inChord = false;
            if (i && seq[i] == seq[i - 1]) noRepeat = false;
            if (std::find(used.begin(), used.end(), seq[i]) == used.end()) used.push_back(seq[i]);
        }
        check(inChord && noRepeat && used.size() == 6, "Random: only chord notes, never the same note twice running, all 6 used in 2 bars");
        seq = arpRun(kArpUp, 2, 1, {{P::kSwing, 66}}, 1.0, 100.0, true, &ts);
        const double trip = SR * 60.0 / 100.0 / 3.0;
        worst = 0.0;
        for (size_t i = 0; i < ts.size(); ++i) worst = std::max(worst, std::fabs((double)ts[i] - trip * (double)i));
        check(ts.size() == 12 && worst <= 1.0, "1/8T at 100 BPM: 12 notes a bar on the triplet grid, Swing ignored (worst %.2f samples)", worst);
        seq = arpRun(kArpUp, 3, 1, {{P::kSwing, 66.6667f}}, 0.25, 120.0, true, &ts);
        check(ts.size() == 4 && std::fabs((double)ts[1] - (six + six / 3.0)) <= 1.5 && std::fabs((double)ts[2] - 2.0 * six) <= 1.0,
              "Swing 66 %% on 1/16: the off-beat sixteenth a third late, the next one on the grid");
        seq = arpRun(kArpUp, 3, 1, {{P::kPattern, 6}}, 0.5, 120.0, true);
        bool single = true; { std::map<uint32_t, int> cnt; for (auto& l : g_log) if (++cnt[l.chord] > 1) single = false; }
        check(single && seq.size() == 8, "Arp overrides Pattern: one note per step (%zu notes)", seq.size());
        seq = arpRun(kArpUp, 3, 1, {}, 0.5, 120.0, false, &ts);
        check(!ts.empty() && ts[0] == (long long)std::llround(0.1 * SR) && seq.size() == 8, "transport stopped: free-running from the note itself");
        // Gate: 1/4 at 60 BPM (a step is 1 s), Arp Gate 50 %: the note is let go half-way through the step.
        {
            Session s(0, {{P::kArp, kArpUp}, {P::kArpRate, 0}, {P::kArpGate, 50}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLead, 0}});
            s.bpm = 60.0; s.play(true, 0.0);
            s.run(0.45, {on(0.0, 60)});
            int heldAt45 = 0; for (auto& v : s.p->voices_) if (v.stage != kStOff && v.stage != kStRelease) ++heldAt45;
            s.run(0.55);
            int heldAt55 = 0; for (auto& v : s.p->voices_) if (v.stage != kStOff && v.stage != kStRelease) ++heldAt55;
            check(heldAt45 == 1 && heldAt55 == 0, "Arp Gate 50 %%: the note is held to half a step, then released");
        }
        {
            // With Hold, the arpeggio keeps going on the latched chord and follows a new pad.
            Session s(0, {{P::kArp, kArpUp}, {P::kHold, 1}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLead, 0}});
            s.bpm = 120.0; s.play(true, 0.0);
            s.run(2.0, {on(0.0, 60), off(0.1, 60), on(1.0, 65), off(1.1, 65)});
            bool cFirst = true, fAfter = true;
            for (auto& l : g_log) { const int pc = l.pitch % 12; if (l.t < (long long)SR) cFirst &= (pc == 0 || pc == 4 || pc == 7); else fAfter &= (pc == 5 || pc == 9 || pc == 0); }
            check(cFirst && fAfter && g_log.size() == 16, "Hold: the arp runs on after the pad is released and changes chord on the next pad");
        }
    }

    // ================================================================================================
    std::printf("\n== Velocity and Warmth ==\n");
    {
        auto level = [&](float sens, int vel, float warmth, float outDb)
        {
            Session s(0, {{P::kVelocity, sens}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLooseVel, 0}, {P::kLead, 0},
                          {P::kWarmth, warmth}, {P::kOutput, outDb}, {P::kDust, 0}, {P::kWobble, 0}});
            s.run(1.0, {on(0.05, 60, vel), off(0.9, 60)});
            return std::make_pair(rmsOf(s.L, (size_t)(0.06 * SR), (size_t)(0.8 * SR)), peakOf(s.L));
        };
        const float a = db(level(100, 30, 0, -3).first), b = db(level(100, 120, 0, -3).first);
        const float c = db(level(0, 30, 0, -3).first), d = db(level(0, 120, 0, -3).first);
        check(b - a > 8.0f && std::fabs(d - c) < 3.0f, "Velocity 100 %%: soft vs hard %.1f dB apart; Velocity 0: %.1f dB apart (brightness only)", b - a, d - c);
        const auto clean = level(100, 127, 0, 6), warm = level(100, 127, 100, 6);
        check(warm.second < clean.second && std::isfinite(warm.first), "Warmth rounds off a loud chord (peak %.1f -> %.1f dBFS)", db(clean.second), db(warm.second));
        const float q0 = db(level(100, 40, 0, -10).first), q1 = db(level(100, 40, 30, -10).first);
        check(std::fabs(q1 - q0) < 0.6f, "Warmth 30 %% leaves a quiet chord's level alone (%.2f dB)", q1 - q0);
    }

    // ================================================================================================
    std::printf("\n== Hold (latch) and pedal ==\n");
    {
        Session s(0, {{P::kHold, 1}, {P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLead, 0}, {P::kRelease, 100}});
        s.run(1.0, {on(0.05, 60), off(0.3, 60)});
        int held = 0; for (auto& v : s.p->voices_) if (v.stage != kStOff && v.held) ++held;
        check(held == 3, "Hold: the chord keeps sounding after the pad is released (%d voices held)", held);
        check(rmsOf(s.L, (size_t)(0.8 * SR), (size_t)(1.0 * SR)) > 0.003f, "and it is still audible at 1 s");
        s.run(1.6, {on(1.2, 65), off(1.25, 65)});
        int heldC = 0, heldF = 0;
        for (auto& v : s.p->voices_) if (v.stage != kStOff && v.held) { if (v.pitch == 60 || v.pitch == 64 || v.pitch == 67) ++heldC; else ++heldF; }
        check(heldC == 0 && heldF == 3, "the next pad replaces the latched chord (C held %d, F held %d)", heldC, heldF);
        s.set(P::kHold, 0);
        s.run(2.6);
        int any = 0; for (auto& v : s.p->voices_) if (v.stage != kStOff && v.held) ++any;
        check(any == 0 && peakOf(s.L, (size_t)(2.4 * SR)) < 0.002f, "switching Hold off releases it");

        Session pd(0, {{P::kStrum, 0}, {P::kLooseTime, 0}, {P::kLead, 0}, {P::kRelease, 100}});
        pd.run(1.2, {{0.02, mpc::MidiEvent::cc(0, 64, 127)}, on(0.05, 60), off(0.2, 60), {1.0, mpc::MidiEvent::cc(0, 64, 0)}});
        check(rmsOf(pd.L, (size_t)(0.8 * SR), (size_t)(0.95 * SR)) > 0.003f && peakOf(pd.L, (size_t)(1.15 * SR)) < 0.01f,
              "sustain pedal holds the chord until it is lifted");
    }

    // ================================================================================================
    std::printf("\n== Voice limit, stealing and CPU ==\n");
    g_testBase = false;
    {
        Session s(2, {{P::kQuality, kNinth}, {P::kBass, 1}, {P::kRelease, 4000}, {P::kAttack, 5}, {P::kStrum, 30}});
        std::vector<Ev> ev;
        for (int i = 0; i < 24; ++i) { const int nn = 50 + (i * 5) % 19; ev.push_back(on(0.05 + 0.05 * i, nn)); ev.push_back(off(0.08 + 0.05 * i, nn)); }
        s.run(3.0, ev);
        check(s.maxLive <= kMaxLive && s.maxSlots <= kNumSlots, "24 six-note chords in 1.2 s: at most %d voices sounding (limit 16), %d slots", s.maxLive, s.maxSlots);
        check(!s.bad && peakOf(s.L) < 1.0f && peakOf(s.R) < 1.0f, "no NaN, peak %.1f dBFS", db(std::max(peakOf(s.L), peakOf(s.R))));
        // Steals fade instead of clicking: the largest sample-to-sample step stays small.
        float jump = 0.0f; for (size_t i = 1; i < s.L.size(); ++i) jump = std::max(jump, std::fabs(s.L[i] - s.L[i - 1]));
        check(jump < 0.12f, "voice stealing does not click (largest step %.3f)", jump);

        const char* names[4] = {"Keys", "Pad", "Pluck", "Organ"};
        double worstPct = 0.0;
        for (int e = 0; e < 4; ++e)
        {
            Session c(0, {{P::kEngine, (float)e}, {P::kQuality, kNinth}, {P::kBass, 1}, {P::kStrum, 0}, {P::kChorus, 50},
                          {P::kRoom, 50}, {P::kWobble, 50}, {P::kDust, 50}, {P::kFilter, 8000}, {P::kRelease, 5000}, {P::kLead, 0}});
            c.fixedBlock = 128;
            // 16 voices: three six-note chords held (the first one stolen into), then measure.
            c.run(0.6, {on(0.01, 48), on(0.02, 55), on(0.03, 62)});
            c.cpu = 0.0;
            c.run(4.6);
            int live = 0; for (auto& v : c.p->voices_) if (v.stage != kStOff && !v.fading) ++live;
            const double pct = 100.0 * c.cpu / 4.0;
            worstPct = std::max(worstPct, pct);
            std::printf("   %-6s %2d voices + everything on: %.2f %% of one desktop core\n", names[e], live, pct);
        }
        check(worstPct < 2.0, "CPU: worst engine %.2f %% of a desktop core with 16 voices and every effect on", worstPct);
        Session idle(0, {});
        idle.run(1.0);
        check(peakOf(idle.L) == 0.0f, "idle: exact silence (Dust only rides on the sound)");
    }

    // ================================================================================================
    std::printf("\n== Tuning (A3 = 220 Hz, each voice alone) ==\n");
    {
        const char* names[4] = {"Keys", "Pad", "Pluck", "Organ"};
        for (int e = 0; e < 4; ++e)
        {
            for (int note : {45, 57, 72})
            {
                Session s(0, {{P::kEngine, (float)e}, {P::kWobble, 0}, {P::kDust, 0}, {P::kRoom, 0}, {P::kChorus, 0}, {P::kAttack, 1}});
                s.p->readSettings();
                Voice* v = s.p->startVoice(note, 0.8f, 1, -1, false);
                if (e == kPad) v->detune = 1.0f;    // the pad's two saws sit +-7 cents either side of the note
                s.run(1.3);
                const double f = 440.0 * std::pow(2.0, (note - 69) / 12.0);
                const size_t len = (size_t)(0.8 * SR);
                const double got = peakFreq(s.L, (size_t)(0.25 * SR), len, f * 0.985, f * 1.015, f * 0.0002);
                const double cents = 1200.0 * std::log2(got / f);
                check(std::fabs(cents) < 3.0, "%-5s note %d: %.2f Hz (%+.1f cents)", names[e], note, got, cents);
            }
        }
    }

    // ================================================================================================
    std::printf("\n== Levels per program (C major chord, velocity 100) ==\n");
    {
        float lo = 0.0f, hi = -100.0f;
        for (size_t pi = 0; pi < (size_t)Driftwood().numPrograms(); ++pi)
        {
            Session s((int)pi, {{P::kPattern, 0}, {P::kArp, 0}, {P::kHold, 0}});
            s.run(2.5, {on(0.05, 60, 100), off(1.6, 60)});
            const float pk = std::max(peakOf(s.L), peakOf(s.R));
            const float rm = db(std::max(rmsOf(s.L, (size_t)(0.1 * SR), (size_t)(0.9 * SR)), rmsOf(s.R, (size_t)(0.1 * SR), (size_t)(0.9 * SR))));
            lo = std::min(lo, rm); hi = std::max(hi, rm);
            check(pk < 0.92f && rm > -26.0f && rm < -10.0f && !s.bad, "%-18s peak %5.1f dBFS, RMS %5.1f dBFS", s.p->programs_[pi].name.c_str(), db(pk), rm);
        }
        check(hi - lo < 9.0f, "programs within %.1f dB of each other", hi - lo);
    }

    // ================================================================================================
    // Demo: I - V - vi - IV in D major, one bar each at 88 BPM, on every program.
    const char* wav = argc > 1 ? argv[1] : nullptr;
    if (wav)
    {
        std::printf("\n== Demo ==\n");
        std::vector<float> DL, DR;
        const double bpm = 88.0, beat = 60.0 / bpm, bar = 4.0 * beat;
        const int prog[4] = {62, 69, 71, 67};   // D A Bm G (Keys map, key of D)
        for (size_t pi = 0; pi < (size_t)Driftwood().numPrograms(); ++pi)
        {
            Session s((int)pi, {{P::kKey, 2}});
            s.bpm = bpm; s.play(true, 0.0);
            const bool pattern = s.p->getInt(P::kPattern) > 0 || s.p->getInt(P::kArp) > 0;
            const int eng = s.p->getInt(P::kEngine);
            std::vector<Ev> ev;
            for (int b = 0; b < 4; ++b)
            {
                const double t = b * bar;
                if (pattern || eng == kPad) { ev.push_back(on(t, prog[b], 100)); ev.push_back(off(t + bar - 0.02, prog[b])); }
                else
                {
                    // Played: a strum on 1, a lighter one on the "and" of 2, again on 3 (keys, plucks, organ).
                    ev.push_back(on(t, prog[b], 104)); ev.push_back(off(t + 1.4 * beat, prog[b]));
                    ev.push_back(on(t + 1.5 * beat, prog[b], 72)); ev.push_back(off(t + 2.4 * beat, prog[b]));
                    ev.push_back(on(t + 2.5 * beat, prog[b], 92)); ev.push_back(off(t + 3.9 * beat, prog[b]));
                }
            }
            s.run(4.0 * bar + 2.0, ev);
            std::printf("   %5.1f s  %s\n", DL.size() / SR, s.p->programs_[pi].name.c_str());
            DL.insert(DL.end(), s.L.begin(), s.L.end()); DR.insert(DR.end(), s.R.begin(), s.R.end());
        }
        writeWav(wav, DL, DR);
        std::printf("   wrote %s (%.1f s)\n", wav, DL.size() / SR);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
