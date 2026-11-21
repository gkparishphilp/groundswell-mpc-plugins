// Krill offline demo and checks: renders every program through a short musical passage (chord arps,
// tempo arps, the 16 pad drums, the talking voice, bass, bells, lead) to one WAV, checks that each part
// does what it should, and times a worst case for CPU.
//
//   mkdir -p tests/build
//   clang++ -std=c++17 -O2 -I../../framework tests/demotest.cpp -o tests/build/demotest && tests/build/demotest [out.wav]
//   (run from plugins/Krill)
#include <vector>
#include <cstdio>
#include <cmath>
#include <string>
#include <chrono>
#include <algorithm>
#include <functional>

#define private public
#define protected public
#include "../src/Krill.cpp"
#undef private
#undef protected

static const double SR = 44100.0;
static const double BPM = 120.0;
static const double SPB = SR * 60.0 / BPM;   // samples per beat
static int failures = 0;
static void check(bool ok, const char* fmt, ...)
{
    char buf[256]; va_list a; va_start(a, fmt); std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
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

// A timed event inside a section: a MIDI message or a parameter change.
struct Ev { long long t; int kind; int a; float b; };   // kind 0 note on (a note, b vel), 1 note off, 2 set param a = b
static Ev on(double beat, int note, int vel = 110) { return {(long long)(beat * SPB), 0, note, (float)vel}; }
static Ev off(double beat, int note) { return {(long long)(beat * SPB), 1, note, 0.0f}; }
static Ev par(double beat, int idx, float v) { return {(long long)(beat * SPB), 2, idx, v}; }

static std::vector<float> outL, outR;
static long long songPos = 0;   // samples since the song started (transport keeps running across sections)

// Hook called after every block with the block start (relative to the section) so checks can watch state.
using Hook = std::function<void(Krill&, long long)>;

static void renderSection(Krill& p, int program, double beats, std::vector<Ev> evs, Hook hook = nullptr, int block = 128)
{
    p.setProgram(program);
    p.reset();
    std::sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.t < y.t; });
    const long long total = (long long)(beats * SPB);
    std::vector<float> l((size_t)512), r((size_t)512);
    std::vector<mpc::MidiEvent> midi;
    size_t ei = 0;
    int blockVar = 0;
    for (long long pos = 0; pos < total;)
    {
        // Vary the block size like the MPC does (32..512).
        const int sizes[4] = {block, 32, 200, 512};
        const int n = (int)std::min<long long>(sizes[(blockVar++) % 4], total - pos);
        midi.clear();
        while (ei < evs.size() && evs[ei].t < pos + n)
        {
            const Ev& e = evs[ei++];
            const int off = (int)std::max<long long>(0, e.t - pos);
            if (e.kind == 0) midi.push_back(mpc::MidiEvent::noteOn(0, e.a, (int)e.b, off));
            else if (e.kind == 1) midi.push_back(mpc::MidiEvent::noteOff(0, e.a, off));
            else p.set(e.a, e.b);
        }
        std::fill(l.begin(), l.begin() + n, 0.0f); std::fill(r.begin(), r.begin() + n, 0.0f);
        float* outs[2] = {l.data(), r.data()};
        mpc::ProcessContext ctx;
        ctx.outputs = outs; ctx.numInputs = 0; ctx.numOutputs = 2; ctx.numFrames = n; ctx.sampleRate = SR;
        ctx.midi = midi.data(); ctx.numMidiEvents = (int)midi.size();
        ctx.transport.valid = ctx.transport.tempoValid = ctx.transport.ppqValid = ctx.transport.playing = true;
        ctx.transport.bpm = BPM; ctx.transport.ppqPos = (double)songPos / SPB;
        p.process(ctx);
        for (int i = 0; i < n; ++i) { outL.push_back(l[i]); outR.push_back(r[i]); }
        if (hook) hook(p, pos);
        pos += n; songPos += n;
    }
}

static double peakDb(size_t from, size_t to)
{
    float pk = 0.0f;
    for (size_t i = from; i < to && i < outL.size(); ++i) pk = std::max(pk, std::fabs(outL[i]));
    return 20.0 * std::log10(std::max(1e-9f, pk));
}
static double rmsDb(size_t from, size_t to)
{
    double s = 0.0; size_t n = 0;
    for (size_t i = from; i < to && i < outL.size(); ++i) { s += (double)outL[i] * outL[i]; ++n; }
    return 10.0 * std::log10(std::max(1e-12, n ? s / (double)n : 0.0));
}

int main(int argc, char** argv)
{
    Krill p;
    p.prepare(SR, 512);
    const auto t0 = std::chrono::steady_clock::now();

    // 1. Chord Chirp: frame-rate chord arpeggio. One finger plays a whole chord, 50 steps a second.
    {
        const size_t start = outL.size();
        int changes = 0, lastStep = -1;
        renderSection(p, 1, 8, {on(0, 60), off(3, 60), on(3, 65), off(6, 65), on(6, 67), on(6, 72), off(7.5, 67), off(7.5, 72)},
            [&](Krill& k, long long pos) {
                if (pos < (long long)(3 * SPB) && k.voices_[0].active) { if (k.voices_[0].arpStep != lastStep) ++changes; lastStep = k.voices_[0].arpStep; }
            });
        // 1.5 s at 50 frames/s with one frame per step: ~75 steps, watched at block granularity.
        check(changes >= 55 && changes <= 80, "chord arp: held C4 stepped through its chord %d times in 1.5 s (expect ~75)", changes);
        check(rmsDb(start, outL.size()) > -35.0, "chord arp: audible (rms %.1f dBFS)", rmsDb(start, outL.size()));
    }
    // 2. Minor Arp Run: tempo-synced 1/16 up-down arp, mono. One note -> its minor chord over 2 octaves.
    {
        const size_t start = outL.size();
        uint32_t firstOrder = 0, lastOrder = 0; bool seen = false;
        renderSection(p, 2, 8, {on(0, 57), off(4, 57), on(4, 60), on(4, 64), on(4, 67), off(7.75, 60), off(7.75, 64), off(7.75, 67)},
            [&](Krill& k, long long pos) {
                if (pos < (long long)(4 * SPB)) { if (!seen) { firstOrder = k.orderCounter_; seen = true; } lastOrder = k.orderCounter_; }
            });
        const int steps = (int)(lastOrder - firstOrder) + 1;
        check(steps >= 15 && steps <= 17, "tempo arp: %d steps in one bar of 1/16 at 120 BPM (expect 16)", steps);
        check(rmsDb(start, outL.size()) > -35.0, "tempo arp: audible (rms %.1f dBFS)", rmsDb(start, outL.size()));
    }
    // 3. Pad Drum Kit: all 16 drums in turn (notes 36..51, one per 8th), then a two-bar beat.
    {
        const size_t start = outL.size();
        std::vector<Ev> evs;
        for (int d = 0; d < 16; ++d) { evs.push_back(on(d * 0.5, 36 + d)); evs.push_back(off(d * 0.5 + 0.25, 36 + d)); }
        for (int s = 0; s < 32; ++s)
        {
            const double b = 8.0 + s * 0.25;
            if (s % 8 == 0 || s % 16 == 10) evs.push_back(on(b, 36));
            if (s % 8 == 4) evs.push_back(on(b, 37));
            if (s % 2 == 0) evs.push_back(on(b, s % 8 == 6 ? 39 : 38, s % 4 == 0 ? 100 : 70));
            if (s == 15) evs.push_back(on(b, 42));     // laser
            if (s == 31) evs.push_back(on(b, 45));     // blast
            if (s % 8 == 7) evs.push_back(on(b, 43, 80));  // blip
        }
        renderSection(p, 6, 16, evs);
        int loud = 0; double quietest = 0.0; int quietestDrum = 0;
        for (int d = 0; d < 16; ++d)
        {
            const size_t a = start + (size_t)(d * 0.5 * SPB), b = a + (size_t)(0.5 * SPB);
            const double pk = peakDb(a, b);
            if (pk > -30.0) ++loud;
            if (pk < quietest) { quietest = pk; quietestDrum = d; }
        }
        check(loud == 16, "drums: %d of 16 pads sound (quietest: %s at %.1f dBFS peak)", loud, kDrums[quietestDrum].name, quietest);
        const size_t lastHit = start + (size_t)(7.5 * SPB) + (size_t)(0.45 * SPB);
        check(peakDb(lastHit, lastHit + 1000) < peakDb(start + (size_t)(7.5 * SPB), lastHit) - 3.0, "drums: one-shots decay on their own");
    }
    // 4. Talking Toy: eight words, one per note.
    {
        const size_t start = outL.size();
        const int words[8] = {3, 9, 4, 6, 7, 1, 10, 11};   // Yeah, Robot, Wow, Hey, No, Uh-Oh, Bleep, Zap
        const int notes[8] = {55, 55, 58, 55, 53, 55, 60, 62};
        std::vector<Ev> evs;
        for (int w = 0; w < 8; ++w)
        {
            evs.push_back(par(w * 1.5 - 0.01 < 0 ? 0 : w * 1.5 - 0.01, Krill::kWord, (float)words[w]));
            evs.push_back(on(w * 1.5, notes[w])); evs.push_back(off(w * 1.5 + 1.1, notes[w]));
        }
        renderSection(p, 5, 12, evs);
        int voiced = 0;
        for (int w = 0; w < 8; ++w)
        {
            const size_t a = start + (size_t)(w * 1.5 * SPB), b = a + (size_t)(0.5 * SPB);
            if (rmsDb(a, b) > -40.0) ++voiced;
        }
        check(voiced == 8, "talk: %d of 8 words audible", voiced);
    }
    {
        const size_t start = outL.size();
        // Crunch off so the filters themselves are measured. Note 57 = 220 Hz: harmonics 4 (880 Hz) and 10-11 (2.2-2.4 kHz).
        renderSection(p, 5, 3, {par(0, Krill::kWord, 12), par(0, Krill::kBits, 16), par(0, Krill::kResample, 44100),
                                par(0, Krill::kTone, 20000), on(0, 57), off(2.5, 57)});
        auto goertzel = [&](size_t a, size_t b, double hz) {
            const double w = 2.0 * 3.14159265358979 * hz / SR, c = 2.0 * std::cos(w); double s1 = 0, s2 = 0;
            for (size_t i = a; i < b; ++i) { const double s0 = outL[i] + c * s1 - s2; s2 = s1; s1 = s0; }
            return s1 * s1 + s2 * s2 - c * s1 * s2; };
        auto brightness = [&](size_t a, size_t b) { return (goertzel(a, b, 2200) + goertzel(a, b, 2420)) / std::max(1e-12, goertzel(a, b, 880)); };
        const double ee = brightness(start + (size_t)(0.06 * SR), start + (size_t)(0.18 * SR));
        const double oo = brightness(start + (size_t)(0.32 * SR), start + (size_t)(0.44 * SR));
        check(ee > oo * 4.0, "talk: formants move ('Ee' 2.3k/880 energy ratio %.2f vs 'Oo' %.3f)", ee, oo);
    }
    // 5. Sync Sweep Bass: hard-sync sweeps on a 16th bassline.
    {
        const size_t start = outL.size();
        const int line[16] = {36, 0, 36, 48, 0, 36, 39, 0, 41, 0, 41, 43, 0, 39, 36, 34};
        std::vector<Ev> evs;
        for (int rep = 0; rep < 2; ++rep)
            for (int s = 0; s < 16; ++s) if (line[s]) { const double b = rep * 4 + s * 0.25; evs.push_back(on(b, line[s] + 12)); evs.push_back(off(b + 0.2, line[s] + 12)); }
        renderSection(p, 3, 8, evs);
        check(rmsDb(start, outL.size()) > -35.0, "sync bass: audible (rms %.1f dBFS)", rmsDb(start, outL.size()));
    }
    // 6. Split Kit + Bass: drums below C2, bass above, together.
    {
        const size_t start = outL.size();
        std::vector<Ev> evs;
        for (int s = 0; s < 32; ++s)
        {
            const double b = s * 0.25;
            if (s % 8 == 0 || s % 16 == 11) evs.push_back(on(b, 36));
            if (s % 8 == 4) evs.push_back(on(b, 37));
            if (s % 2 == 1) evs.push_back(on(b, 38, 70));
        }
        const int bass[8] = {48, 48, 55, 53, 48, 51, 53, 55};
        for (int i = 0; i < 8; ++i) { evs.push_back(on(i, bass[i])); evs.push_back(off(i + 0.7, bass[i])); }
        renderSection(p, 7, 8, evs);
        check(rmsDb(start, outL.size()) > -35.0, "split: drums and bass together (rms %.1f dBFS)", rmsDb(start, outL.size()));
    }
    // 7. Ring Bells, 8. Pulse Lead (stepped vibrato and glide).
    renderSection(p, 4, 6, {on(0, 72), on(1, 76), on(2, 79), on(3, 84), off(4, 72), off(4, 76), off(4, 79), off(4, 84)});
    renderSection(p, 0, 8, {par(0, Krill::kVoiceMode, 1), par(0, Krill::kGlide, 90), on(0, 64), on(1.5, 67), off(1.6, 64),
                            on(2, 69), off(2.1, 67), on(3, 71), off(3.1, 69), on(3.5, 72), off(3.6, 71), off(6.5, 72)});
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    float pk = 0.0f; bool finite = true;
    for (size_t i = 0; i < outL.size(); ++i) { pk = std::max(pk, std::fabs(outL[i])); if (!std::isfinite(outL[i])) finite = false; }
    check(finite, "whole demo: no NaN/Inf");
    check(pk < 1.0f, "whole demo: no clipping (peak %.1f dBFS)", 20.0 * std::log10(std::max(1e-9f, pk)));
    std::printf("CPU (desktop, whole demo): %.2f%% of real time\n", 100.0 * sec / ((double)outL.size() / SR));

    // Worst case: all 8 voices, talking, noise on B, ring + sync, crunch on. 10 seconds.
    {
        Krill w; w.prepare(SR, 512); w.reset();
        w.set(Krill::kTalk, 100); w.set(Krill::kWaveB, kNoise); w.set(Krill::kLevelB, 60); w.set(Krill::kOscMod, kModBoth);
        w.set(Krill::kArpMode, Krill::kArpChord); w.set(Krill::kBits, 8); w.set(Krill::kResample, 11025); w.set(Krill::kTone, 6000);
        std::vector<float> l(128), r(128);
        const long long total = (long long)(10 * SR);
        const auto w0 = std::chrono::steady_clock::now();
        for (long long pos = 0; pos < total; pos += 128)
        {
            mpc::MidiEvent ev[8]; int ne = 0;
            if (pos == 0) for (int v = 0; v < 8; ++v) ev[ne++] = mpc::MidiEvent::noteOn(0, 48 + v * 3, 100, v);
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            float* outs[2] = {l.data(), r.data()};
            mpc::ProcessContext ctx; ctx.outputs = outs; ctx.numOutputs = 2; ctx.numFrames = 128; ctx.sampleRate = SR;
            ctx.midi = ev; ctx.numMidiEvents = ne;
            w.process(ctx);
        }
        const double ws = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        std::printf("CPU (desktop, worst case: 8 talking voices, noise, ring+sync, crunch): %.2f%% of real time\n", 100.0 * ws / 10.0);
    }

    const char* path = argc > 1 ? argv[1] : "tests/build/krill-demo.wav";
    writeWav(path, outL, outR);
    std::printf("wrote %s (%.1f s)\n", path, (double)outL.size() / SR);
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
