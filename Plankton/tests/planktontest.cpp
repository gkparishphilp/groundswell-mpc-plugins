// Plankton render test: are the grains where they should be, in tune, on the beat, click-free and bounded?
// Also renders the demo (build/plankton-demo.wav).
//
//   mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework tests/planktontest.cpp -o tests/build/planktontest
//   tests/build/planktontest [demo.wav]          (run from plugins/Plankton; log goes to stdout)
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <chrono>
#include <algorithm>
#include <functional>

struct Birth { long long sample; float lag; float rate; bool rev; float len; };
static std::vector<Birth> g_births;
static long long g_now = 0;          // absolute sample at the start of the current chunk
#define PLANKTON_GRAIN_HOOK(off, lag, rate, rev, len) g_births.push_back({g_now + (off), (lag), (rate), (rev), (len)})

#define private public
#define protected public
#include "../src/Plankton.cpp"
#undef private
#undef protected

static const double SR = 44100.0;
static int failures = 0, passes = 0;
static void check(bool ok, const char* fmt, ...)
{
    char buf[512];
    va_list a; va_start(a, fmt); std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", buf);
    if (ok) ++passes; else ++failures;
}
static void info(const char* fmt, ...)
{
    char buf[512];
    va_list a; va_start(a, fmt); std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    std::printf("INFO  %s\n", buf);
}

typedef std::vector<float> Buf;
static const double TWO_PI = 6.283185307179586;

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

// ---------------------------------------------------------------------------------------------------
// Running the plugin
// ---------------------------------------------------------------------------------------------------
struct Song { bool playing = false; double bpm = 120.0; double ppqStart = 0.0; };

// Process in[] -> out[] in blocks of `block`, calling `between(sampleIndex)` before each block (to move
// parameters). Transport follows `song` (ppq advances with the audio while playing).
static void run(Plankton& p, const Buf& inL, const Buf& inR, Buf& outL, Buf& outR, int block,
                const Song& song = Song(), std::function<void(long long)> between = nullptr, long long startSample = 0)
{
    const size_t n = inL.size();
    outL.assign(n, 0.0f); outR.assign(n, 0.0f);
    Buf l(block), r(block);
    for (size_t s = 0; s < n; s += (size_t)block)
    {
        const int m = (int)std::min((size_t)block, n - s);
        if (between) between((long long)s);
        std::copy(inL.begin() + (long)s, inL.begin() + (long)s + m, l.begin());
        std::copy(inR.begin() + (long)s, inR.begin() + (long)s + m, r.begin());
        float* io[2] = {l.data(), r.data()};
        mpc::ProcessContext ctx;
        ctx.inputs = io; ctx.outputs = io; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = m;
        ctx.sampleRate = SR;
        ctx.transport.valid = true;
        ctx.transport.tempoValid = true;
        ctx.transport.bpm = song.bpm;
        ctx.transport.playing = song.playing;
        ctx.transport.ppqValid = true;
        ctx.transport.ppqPos = song.ppqStart + (double)(startSample + (long long)s) * song.bpm / (60.0 * SR);
        g_now = startSample + (long long)s;
        {
            mpc::ScopedDenormalsOff ftz;   // the host does this around every process() call
            p.process(ctx);
        }
        std::copy(l.begin(), l.begin() + m, outL.begin() + (long)s);
        std::copy(r.begin(), r.begin() + m, outR.begin() + (long)s);
    }
}

// A "clean" grain setup: one voice at a time, no randomness, unity pan, wet only.
static void clean(Plankton& p)
{
    p.set(Plankton::kOutput, 0); p.set(Plankton::kMix, 100); p.set(Plankton::kFreeze, 0); p.set(Plankton::kCatch, 0);
    p.set(Plankton::kSize, 50); p.set(Plankton::kDensity, 4); p.set(Plankton::kPosition, 500); p.set(Plankton::kSpray, 0);
    p.set(Plankton::kPitch, 0); p.set(Plankton::kScatter, 0); p.set(Plankton::kSnap, 0); p.set(Plankton::kReverse, 0);
    p.set(Plankton::kShape, 0); p.set(Plankton::kSpread, 0); p.set(Plankton::kDrift, 0); p.set(Plankton::kFeedback, 0);
    p.set(Plankton::kSmear, 0); p.set(Plankton::kTone, 0); p.set(Plankton::kSync, 0);
}

// ---------------------------------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------------------------------
static Buf noise(size_t n, uint32_t seed, float amp)
{
    Buf b(n); uint32_t x = seed;
    for (size_t i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = amp * ((float)(x >> 8) / 8388608.0f - 1.0f); }
    return b;
}
static Buf sines(size_t n, std::initializer_list<double> hz, float amp, size_t from = 0, size_t to = (size_t)-1)
{
    Buf b(n, 0.0f);
    for (size_t i = from; i < std::min(n, to); ++i)
    {
        double s = 0; for (double f : hz) s += std::sin(TWO_PI * f * (double)i / SR);
        b[i] = amp * (float)s;
    }
    return b;
}
static double rms(const Buf& a, size_t i0, size_t n)
{
    double s = 0; for (size_t k = 0; k < n && i0 + k < a.size(); ++k) s += (double)a[i0 + k] * a[i0 + k];
    return n ? std::sqrt(s / (double)n) : 0.0;
}
static double goertzel(const Buf& a, size_t i0, size_t n, double hz)
{
    const double w = TWO_PI * hz / SR, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t k = 0; k < n; ++k)
    {
        const double win = 0.5 - 0.5 * std::cos(TWO_PI * (double)k / (double)(n - 1));
        const double s0 = a[i0 + k] * win + c * s1 - s2; s2 = s1; s1 = s0;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (double)n;
}
static double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }
// Peak of the second difference: a click shows up as a spike here even on a loud smooth signal.
static double maxCurve(const Buf& a, size_t from, size_t to)
{
    double m = 0; for (size_t i = std::max<size_t>(from, 2); i < std::min(to, a.size()); ++i) m = std::fmax(m, std::fabs(a[i] - 2 * a[i - 1] + a[i - 2]));
    return m;
}

// ---------------------------------------------------------------------------------------------------
// Demo sources: a drum loop, a chord pad and a vocal-ish line, all synthesised here.
// ---------------------------------------------------------------------------------------------------
static void drumLoop(Buf& L, Buf& R, size_t n, double bpm)
{
    L.assign(n, 0.0f); R.assign(n, 0.0f);
    const double spb = SR * 60.0 / bpm, s16 = spb / 4.0;
    uint32_t x = 12345;
    auto rnd = [&]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (float)(x >> 8) / 8388608.0f - 1.0f; };
    const int kick[16]  = {1,0,0,0, 0,0,1,0, 1,0,0,0, 0,0,0,1};
    const int snare[16] = {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0};
    for (size_t step = 0; (double)step * s16 < (double)n; ++step)
    {
        const size_t t0 = (size_t)((double)step * s16);
        const int st = (int)(step % 16);
        if (kick[st]) for (size_t k = 0; k < (size_t)(0.35 * SR) && t0 + k < n; ++k)
        {
            const double t = k / SR, f = 48.0 + 110.0 * std::exp(-t * 28.0);
            static double ph; if (k == 0) ph = 0; ph += TWO_PI * f / SR;
            const float v = 0.75f * (float)(std::sin(ph) * std::exp(-t * 7.0));
            L[t0 + k] += v; R[t0 + k] += v;
        }
        if (snare[st]) for (size_t k = 0; k < (size_t)(0.22 * SR) && t0 + k < n; ++k)
        {
            const double t = k / SR;
            const float v = (float)(0.35 * rnd() * std::exp(-t * 18.0) + 0.3 * std::sin(TWO_PI * 190.0 * t) * std::exp(-t * 25.0));
            L[t0 + k] += v * 0.9f; R[t0 + k] += v;
        }
        // closed hats on every 16th, accented on the offbeat
        const float acc = (st % 4 == 2) ? 0.22f : 0.1f;
        float hp = 0, prev = 0;
        for (size_t k = 0; k < (size_t)(0.05 * SR) && t0 + k < n; ++k)
        {
            const float w = rnd(); hp = 0.6f * (hp + w - prev); prev = w;
            const float v = acc * hp * (float)std::exp(-(double)k / SR * 70.0);
            L[t0 + k] += v * 0.7f; R[t0 + k] += v;
        }
    }
}
static void chordPad(Buf& L, Buf& R, size_t n, std::initializer_list<std::initializer_list<double>> chords, double chordSec)
{
    L.assign(n, 0.0f); R.assign(n, 0.0f);
    std::vector<std::vector<double>> cs; for (auto c : chords) cs.push_back(std::vector<double>(c));
    double lpL = 0, lpR = 0;
    std::vector<double> ph(16, 0.0);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = i / SR;
        const size_t ci = (size_t)(t / chordSec) % cs.size();
        const double inChord = std::fmod(t, chordSec);
        const double env = std::min(1.0, inChord / 0.4) * std::min(1.0, (chordSec - inChord) / 0.3);
        double sl = 0, sr = 0;
        for (size_t v = 0; v < cs[ci].size(); ++v)
            for (int d = 0; d < 2; ++d)
            {
                double& p = ph[v * 2 + d];
                p += cs[ci][v] * (d ? 1.004 : 0.996) / SR; p -= std::floor(p);
                const double saw = 2.0 * p - 1.0;
                (d ? sr : sl) += saw;
            }
        const double a = 0.06;   // gentle low-pass
        lpL += a * (sl - lpL); lpR += a * (sr - lpR);
        L[i] = (float)(0.22 * env * lpL); R[i] = (float)(0.22 * env * lpR);
    }
}
// Staccato chord stabs: 0.3 s every 2 s, so the tail has room.
static void stabs(Buf& L, Buf& R, size_t n)
{
    L.assign(n, 0.0f); R.assign(n, 0.0f);
    const double chords[3][4] = {{220.0, 261.63, 329.63, 440.0}, {174.61, 220.0, 261.63, 349.23}, {196.0, 246.94, 293.66, 392.0}};
    for (int s = 0; (double)s * 2.0 + 0.3 < (double)n / SR; ++s)
    {
        const size_t t0 = (size_t)((0.2 + 2.0 * s) * SR);
        for (size_t k = 0; k < (size_t)(0.3 * SR) && t0 + k < n; ++k)
        {
            const double t = k / SR, e = std::min(1.0, t / 0.005) * std::exp(-t * 6.0) * std::min(1.0, (0.3 - t) / 0.02);
            double v = 0; for (double f : chords[s % 3]) v += std::sin(TWO_PI * f * t) + 0.3 * std::sin(TWO_PI * 2 * f * t);
            L[t0 + k] += (float)(0.12 * e * v); R[t0 + k] += (float)(0.12 * e * v);
        }
    }
}
// Vowel-ish voice: a glottal pulse train through three formant resonators, a small melody with vibrato.
static void voice(Buf& L, Buf& R, size_t n)
{
    L.assign(n, 0.0f); R.assign(n, 0.0f);
    const double notes[] = {57, 60, 64, 62, 60, 59, 57, 55};
    const double vowels[][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {570, 840, 2410}, {530, 1840, 2480}};
    double ph = 0, y[3][2] = {{0}};
    for (size_t i = 0; i < n; ++i)
    {
        const double t = i / SR;
        const double noteLen = 0.9;
        const size_t ni = (size_t)(t / noteLen) % 8;
        const double inNote = std::fmod(t, noteLen);
        const double env = std::min(1.0, inNote / 0.08) * std::min(1.0, (noteLen - 0.05 - inNote) / 0.12);
        const double f0 = 440.0 * std::pow(2.0, (notes[ni] - 69.0) / 12.0) * (1.0 + 0.012 * std::sin(TWO_PI * 5.2 * t) * std::min(1.0, inNote / 0.3));
        ph += f0 / SR; double pulse = 0; if (ph >= 1.0) { ph -= 1.0; pulse = 1.0; }
        const double* fm = vowels[(ni / 2) % 4];
        double out = 0;
        for (int k = 0; k < 3; ++k)
        {
            const double bw = 80.0 + 40.0 * k, r = std::exp(-M_PI * bw / SR);
            const double c1 = 2 * r * std::cos(TWO_PI * fm[k] / SR), c2 = -r * r;
            const double v = pulse * (1.0 - r) + c1 * y[k][0] + c2 * y[k][1];
            y[k][1] = y[k][0]; y[k][0] = v;
            out += v * (k == 0 ? 1.0 : k == 1 ? 0.6 : 0.3);
        }
        const float s = (float)(2.2 * std::max(0.0, env) * out);
        L[i] = s; R[i] = s;
    }
}

int main(int argc, char** argv)
{
    const char* demoPath = argc > 1 ? argv[1] : "build/plankton-demo.wav";
    const int BLOCK = 128;

    // =============================================================================================
    std::printf("== 1. Grains come from the right place in the buffer ==\n");
    {
        Plankton p; p.prepare(SR, 512); clean(p); p.reset();
        const size_t n = (size_t)(4.0 * SR);
        Buf inL = noise(n, 1, 0.5f), inR = noise(n, 2, 0.5f), oL, oR;
        g_births.clear();
        run(p, inL, inR, oL, oR, BLOCK);
        const float pos = 500.0f * 0.001f * (float)SR;
        int good = 0, tested = 0; double worst = 1.0; bool lagsOk = true;
        for (const Birth& b : g_births)
        {
            if (std::fabs(b.lag - pos) > 0.5f) lagsOk = false;
            const long long s0 = b.sample, len = (long long)b.len;
            if (s0 < (long long)(0.6 * SR) || s0 + len >= (long long)n) continue;
            // compare the output with window x input delayed by Position, over the middle half of the grain
            double ab = 0, aa = 0, bb = 0, ab2 = 0, bb2 = 0;
            for (long long k = len / 4; k < 3 * len / 4; ++k)
            {
                const double w = 0.5 - 0.5 * std::cos(TWO_PI * (double)k / (double)len);
                const long long t = s0 + k;
                const double x = oL[(size_t)t], y = w * inL[(size_t)(t - (long long)pos)];
                const double y2 = w * inL[(size_t)(t - (long long)pos - 441)];
                ab += x * y; aa += x * x; bb += y * y; ab2 += x * y2; bb2 += y2 * y2;
            }
            const double c = ab / std::sqrt(aa * bb), c2 = ab2 / std::sqrt(aa * bb2);
            ++tested; if (c > 0.999 && std::fabs(c2) < 0.2) ++good;
            worst = std::min(worst, c);
        }
        check(lagsOk, "every grain started at Position (500 ms = %.0f samples back)", pos);
        check(tested >= 10 && good == tested, "output = window x input from 500 ms ago (%d/%d grains, worst corr %.5f; 10 ms off: no match)", good, tested, worst);

        // Spray: start lags spread over [Position, Position + Spray]
        Plankton q; q.prepare(SR, 512); clean(q); q.set(Plankton::kSpray, 400); q.set(Plankton::kDensity, 40); q.reset();
        g_births.clear();
        run(q, inL, inR, oL, oR, BLOCK);
        float lo = 1e9f, hi = -1e9f;
        for (const Birth& b : g_births) { lo = std::min(lo, b.lag); hi = std::max(hi, b.lag); }
        check(lo >= pos - 1 && hi <= pos + 0.4f * SR + 1 && hi - lo > 0.3f * SR,
              "Spray 400 ms: grains from %.0f to %.0f ms back (inside 500..900)", lo * 1000 / SR, hi * 1000 / SR);

        // Reverse: output = window x input read backwards around the same centre
        Plankton r; r.prepare(SR, 512); clean(r); r.set(Plankton::kReverse, 100); r.reset();
        g_births.clear();
        run(r, inL, inR, oL, oR, BLOCK);
        good = 0; tested = 0;
        for (const Birth& b : g_births)
        {
            if (!b.rev) continue;
            const long long s0 = b.sample, len = (long long)b.len;
            if (s0 < (long long)(0.6 * SR) || s0 + len >= (long long)n) continue;
            // reversed: read position at k is (s0 - lag) - k
            double ab = 0, aa = 0, bb = 0;
            for (long long k = len / 4; k < 3 * len / 4; ++k)
            {
                const double w = 0.5 - 0.5 * std::cos(TWO_PI * (double)k / (double)len);
                const long long src = s0 - (long long)std::lround(b.lag) - k;
                const double x = oL[(size_t)(s0 + k)], y = w * inL[(size_t)src];
                ab += x * y; aa += x * x; bb += y * y;
            }
            ++tested; if (ab / std::sqrt(aa * bb) > 0.999) ++good;
        }
        // centre of a reversed grain: (s0 + len/2) - (s0 - lag - len/2) = lag + len  ->  lag = Position - len
        check(tested >= 10 && good == tested, "Reverse 100 %%: every grain is the input played backwards (%d/%d)", good, tested);
        bool centred = true;
        for (const Birth& b : g_births) if (std::fabs(b.lag + b.len - pos) > 1.5f) centred = false;
        check(centred, "reversed grains are centred on Position too");
    }

    // =============================================================================================
    std::printf("\n== 2. Pitch is accurate ==\n");
    {
        const size_t n = (size_t)(3.0 * SR);
        Buf in = sines(n, {440.0}, 0.4f), oL, oR;
        for (float st : {0.0f, 7.0f, -12.0f, 12.0f, 24.0f, -24.0f, 3.5f, -0.3f})
        {
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kPitch, st); p.set(Plankton::kSize, 200); p.set(Plankton::kDensity, 3); p.set(Plankton::kPosition, 600);
            p.reset();
            g_births.clear();
            run(p, in, in, oL, oR, BLOCK);
            // Each grain alone (3/s, 200 ms: no overlap): frequency from the interpolated zero crossings
            // over the middle 70 % of the grain.
            const double want = 440.0 * std::pow(2.0, st / 12.0);
            double worst = 0; int measured = 0;
            for (const Birth& b : g_births)
            {
                // skip grains that could reach back before the sine started (up to 600 ms + 4 x 100 ms)
                if (b.sample < (long long)(1.2 * SR) || b.sample + (long long)b.len >= (long long)n) continue;
                const long long a0 = b.sample + (long long)(0.15 * b.len), a1 = b.sample + (long long)(0.85 * b.len);
                double first = -1, last = -1; int count = 0;
                for (long long i = a0 + 1; i < a1; ++i)
                    if (oL[(size_t)i - 1] < 0.0f && oL[(size_t)i] >= 0.0f)
                    {
                        const double t = (double)(i - 1) + oL[(size_t)i - 1] / (oL[(size_t)i - 1] - oL[(size_t)i]);
                        if (first < 0) first = t; last = t; ++count;
                    }
                if (count < 3) continue;
                const double got = SR * (double)(count - 1) / (last - first);
                worst = std::max(worst, std::fabs(cents(got, want))); ++measured;
            }
            check(measured >= 5 && worst < 1.0, "Pitch %+.1f st: %d grains at %.2f Hz, worst %.2f ct off", st, measured, want, worst);
        }
        // Scatter snapping: every grain's rate is an allowed interval
        struct { int snap; const float* set; int count; const char* name; } snaps[] = {
            {kSnOctave, kOctaveSet, 5, "Oct"}, {kSnFifth, kFifthSet, 9, "5th"}, {kSnScale, kScaleSet, 21, "Scale"}};
        for (auto& s : snaps)
        {
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kPitch, 5); p.set(Plankton::kScatter, 24); p.set(Plankton::kSnap, (float)s.snap);
            p.set(Plankton::kDensity, 80); p.set(Plankton::kSize, 20); p.reset();
            g_births.clear();
            run(p, in, in, oL, oR, BLOCK);
            int bad = 0, distinct = 0; std::vector<int> seen;
            for (const Birth& b : g_births)
            {
                const float semis = 12.0f * std::log2(b.rate) - 5.0f;
                bool ok = false;
                for (int i = 0; i < s.count; ++i) if (std::fabs(semis - s.set[i]) < 0.01f) ok = true;
                if (!ok) ++bad;
                const int key = (int)std::lround(semis);
                if (std::find(seen.begin(), seen.end(), key) == seen.end()) { seen.push_back(key); ++distinct; }
            }
            check(bad == 0 && distinct >= std::min(s.count, 5), "Scatter 24 st, Snap %s: all %zu grains on the grid (%d different intervals)", s.name, g_births.size(), distinct);
        }
        // Shimmer Dust's octave scatter around +12: only 0, +12, +24 above the input
        Plankton p; p.prepare(SR, 512); p.setProgram(2); p.set(Plankton::kDrift, 0); p.reset();
        g_births.clear();
        run(p, in, in, oL, oR, BLOCK);
        int bad = 0; for (const Birth& b : g_births) { const float st = 12.0f * std::log2(b.rate); if (std::fabs(st) > 0.01f && std::fabs(st - 12) > 0.01f && std::fabs(st - 24) > 0.01f) ++bad; }
        check(bad == 0, "Shimmer Dust (Drift 0): every grain at unison, +12 or +24 st");
    }

    // =============================================================================================
    std::printf("\n== 3. Freeze holds the captured moment and lets go without clicks ==\n");
    {
        // A low chord (A minor) for 2 s, then a different low chord (G major). Freeze at 1.5 s, release at 5 s.
        const size_t n = (size_t)(8.0 * SR), tF = (size_t)(1.5 * SR), tR = (size_t)(5.0 * SR), tSwitch = (size_t)(2.0 * SR);
        Buf a = sines(n, {110.0, 130.81, 164.81}, 0.2f, 0, tSwitch);
        Buf g = sines(n, {98.0, 123.47, 146.83}, 0.2f, tSwitch);
        Buf in(n); for (size_t i = 0; i < n; ++i) in[i] = a[i] + g[i];
        for (int useCatch = 0; useCatch < 2; ++useCatch)
        {
            const char* what = useCatch ? "Catch (hold)" : "Freeze";
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kSize, 150); p.set(Plankton::kDensity, 30); p.set(Plankton::kPosition, 250); p.set(Plankton::kSpray, 300);
            p.set(Plankton::kReverse, 50); p.set(Plankton::kSpread, 50); p.set(Plankton::kDrift, 60); p.set(Plankton::kDriftRate, 1.0f);
            p.reset();
            Buf oL, oR;
            const int param = useCatch ? Plankton::kCatch : Plankton::kFreeze;
            run(p, in, in, oL, oR, BLOCK, Song(), [&](long long s) {
                if (s == (long long)(tF / BLOCK * BLOCK)) p.set(param, 1);
                if (s == (long long)(tR / BLOCK * BLOCK)) p.set(param, 0);
            });
            // while frozen (from 2.5 s, after the old chord would have left the buffer): A minor, not G major
            const size_t w0 = (size_t)(3.0 * SR), wn = (size_t)(1.5 * SR);
            const double aE = goertzel(oL, w0, wn, 110.0) + goertzel(oL, w0, wn, 164.81);
            const double gE = goertzel(oL, w0, wn, 98.0) + goertzel(oL, w0, wn, 146.83);
            check(aE > 10.0 * gE && rms(oL, w0, wn) > 0.03, "%s: the frozen cloud keeps the captured chord (A %.4f vs G %.4f, rms %.3f)", what, aE, gE, rms(oL, w0, wn));
            const double held1 = rms(oL, (size_t)(2.5 * SR), (size_t)SR), held2 = rms(oL, (size_t)(4.0 * SR), (size_t)SR);
            check(std::fabs(20 * std::log10(held2 / held1)) < 3.0, "%s: frozen level holds (%.1f dB change over 1.5 s)", what, 20 * std::log10(held2 / held1));
            // after release (+ Position + Spray + a grain): the new chord
            const size_t r0 = (size_t)(6.2 * SR);
            const double aE2 = goertzel(oL, r0, wn, 110.0) + goertzel(oL, r0, wn, 164.81);
            const double gE2 = goertzel(oL, r0, wn, 98.0) + goertzel(oL, r0, wn, 146.83);
            check(gE2 > 10.0 * aE2, "%s off: the cloud follows the input again (G %.4f vs A %.4f)", what, gE2, aE2);
            // clicks: the curvature around each transition must stay within what steady frozen / live playing shows
            const double steady = std::max(maxCurve(oL, (size_t)(2.5 * SR), (size_t)(4.5 * SR)), maxCurve(oL, (size_t)(0.5 * SR), (size_t)(1.4 * SR)));
            const double atF = maxCurve(oL, tF - 2000, tF + (size_t)(0.6 * SR));
            const double atR = maxCurve(oL, tR - 2000, tR + (size_t)(1.2 * SR));
            const double steady2 = maxCurve(oL, (size_t)(6.2 * SR), (size_t)(7.9 * SR));
            check(atF <= 1.3 * steady, "%s on: no click (curvature %.5f, steady %.5f)", what, atF, steady);
            check(atR <= 1.3 * std::max(steady, steady2), "%s off: no click (curvature %.5f, steady %.5f / %.5f)", what, atR, steady, steady2);
            if (!useCatch) writeWav("build/test-freeze.wav", oL, oR);
        }
        // Hammering Freeze on and off every 37 ms with pitched, reversed, sprayed grains: still smooth
        {
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kSize, 120); p.set(Plankton::kDensity, 60); p.set(Plankton::kPosition, 30); p.set(Plankton::kSpray, 100);
            p.set(Plankton::kReverse, 50); p.set(Plankton::kPitch, 7); p.set(Plankton::kScatter, 12); p.set(Plankton::kSnap, 1);
            p.reset();
            const size_t n2 = (size_t)(6 * SR);
            Buf in2 = sines(n2, {110.0, 165.0}, 0.25f), oL, oR, refL, refR;
            p.set(Plankton::kLinger, 2000);   // and every release lingers, so caught and live grains interleave
            run(p, in2, in2, oL, oR, 64, Song(), [&](long long s) { p.set(Plankton::kFreeze, ((s / 1632) % 2) ? 1.0f : 0.0f); });
            Plankton q; q.prepare(SR, 512); clean(q);
            q.set(Plankton::kSize, 120); q.set(Plankton::kDensity, 60); q.set(Plankton::kPosition, 30); q.set(Plankton::kSpray, 100);
            q.set(Plankton::kReverse, 50); q.set(Plankton::kPitch, 7); q.set(Plankton::kScatter, 12); q.set(Plankton::kSnap, 1);
            q.reset();
            run(q, in2, in2, refL, refR, 64);
            const double c1 = maxCurve(oL, (size_t)SR, n2), c0 = maxCurve(refL, (size_t)SR, n2);
            check(c1 <= 1.3 * c0, "Freeze toggled every 37 ms with Linger 2 s (pitched, reversed): curvature %.4f vs %.4f never toggled", c1, c0);
        }
    }

    // =============================================================================================
    std::printf("\n== 3b. Linger: the caught cloud stays after letting go, then gives way ==\n");
    {
        const size_t n = (size_t)(10.0 * SR), tF = (size_t)(1.5 * SR), tR = (size_t)(5.0 * SR), tSwitch = (size_t)(2.0 * SR);
        Buf a = sines(n, {110.0, 130.81, 164.81}, 0.2f, 0, tSwitch);
        Buf g = sines(n, {98.0, 123.47, 146.83}, 0.2f, tSwitch);
        Buf in(n); for (size_t i = 0; i < n; ++i) in[i] = a[i] + g[i];
        auto render = [&](float lingerMs, Buf& oL, int& spawns) {
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kSize, 150); p.set(Plankton::kDensity, 30); p.set(Plankton::kPosition, 250); p.set(Plankton::kSpray, 300);
            p.set(Plankton::kSpread, 50); p.set(Plankton::kLinger, lingerMs);
            p.reset();
            Buf oR;
            run(p, in, in, oL, oR, BLOCK, Song(), [&](long long s) {
                if (s == (long long)(tF / BLOCK * BLOCK)) p.set(Plankton::kCatch, 1);
                if (s == (long long)(tR / BLOCK * BLOCK)) p.set(Plankton::kCatch, 0);
            });
            spawns = p.lingerSpawns_;
        };
        Buf o0, o3; int sp0 = 0, sp3 = 0;
        render(0.0f, o0, sp0);
        render(3000.0f, o3, sp3);
        auto aE = [&](const Buf& o, double t0, double len) { return goertzel(o, (size_t)(t0 * SR), (size_t)(len * SR), 110.0) + goertzel(o, (size_t)(t0 * SR), (size_t)(len * SR), 164.81); };
        auto gE = [&](const Buf& o, double t0, double len) { return goertzel(o, (size_t)(t0 * SR), (size_t)(len * SR), 98.0) + goertzel(o, (size_t)(t0 * SR), (size_t)(len * SR), 146.83); };
        check(sp0 == 0 && sp3 > 20, "Linger off draws no caught grains after release; Linger 3 s drew %d", sp3);
        check(gE(o0, 5.9, 0.5) > 5.0 * aE(o0, 5.9, 0.5), "Linger off: 0.9 s after letting go the cloud is the new chord (G %.4f vs A %.4f)", gE(o0, 5.9, 0.5), aE(o0, 5.9, 0.5));
        check(aE(o3, 5.9, 0.5) > 2.0 * gE(o3, 5.9, 0.5), "Linger 3 s: 0.9 s after letting go the caught chord still leads (A %.4f vs G %.4f)", aE(o3, 5.9, 0.5), gE(o3, 5.9, 0.5));
        const double mixA = aE(o3, 6.3, 0.5), mixG = gE(o3, 6.3, 0.5);
        check(mixA > 0.15 * mixG && mixG > 0.15 * mixA, "Linger 3 s: halfway through, both chords sound (A %.4f, G %.4f)", mixA, mixG);
        check(gE(o3, 8.4, 1.0) > 10.0 * aE(o3, 8.4, 1.0), "Linger 3 s: after it the cloud is the new chord (G %.4f vs A %.4f)", gE(o3, 8.4, 1.0), aE(o3, 8.4, 1.0));
        const double held = rms(o3, (size_t)(3.0 * SR), (size_t)(1.5 * SR)), during = rms(o3, (size_t)(5.5 * SR), (size_t)(2.5 * SR));
        check(std::fabs(20 * std::log10(during / held)) < 3.0, "Linger: no level dip while the clouds hand over (%.1f dB vs frozen)", 20 * std::log10(during / held));
        const double steady = maxCurve(o3, (size_t)(3.0 * SR), (size_t)(4.9 * SR));
        const double steady2 = maxCurve(o3, (size_t)(8.5 * SR), (size_t)(9.9 * SR));
        const double atR = maxCurve(o3, tR - 2000, (size_t)(8.5 * SR));
        check(atR <= 1.3 * std::max(steady, steady2), "Linger: no click through the release (curvature %.5f, steady %.5f / %.5f)", atR, steady, steady2);
        writeWav("build/test-linger.wav", o3, o3);

        // The hard case: a full buffer, the deepest reach (Position 3 s + Spray 1 s + Drift), pitched and
        // reversed grains, Linger 10 s, so the recorder overwrites the caught moment while it lingers.
        auto stress = [&](bool freeze) {
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kSize, 300); p.set(Plankton::kDensity, 40); p.set(Plankton::kPosition, 3000); p.set(Plankton::kSpray, 1000);
            p.set(Plankton::kDrift, 100); p.set(Plankton::kDriftRate, 2.0f); p.set(Plankton::kReverse, 50);
            p.set(Plankton::kScatter, 12); p.set(Plankton::kSnap, 1); p.set(Plankton::kLinger, 10000);
            p.reset();
            const size_t m = (size_t)(40.0 * SR);   // the buffer (~24 s at 44.1 kHz) is full before the freeze
            Buf inS = sines(m, {110.0, 165.0}, 0.25f), oL, oR;
            run(p, inS, inS, oL, oR, BLOCK, Song(), [&](long long s) {
                if (!freeze) return;
                if (s == (long long)(26.0 * SR) / BLOCK * BLOCK) p.set(Plankton::kFreeze, 1);
                if (s == (long long)(28.0 * SR) / BLOCK * BLOCK) p.set(Plankton::kFreeze, 0);
            });
            return std::make_pair(maxCurve(oL, (size_t)(25.0 * SR), m), p.lingerSpawns_);
        };
        const auto withFreeze = stress(true), without = stress(false);
        check(withFreeze.first <= 1.3 * without.first && withFreeze.second > 50,
              "Linger 10 s from a full buffer at the deepest reach: no clicks (curvature %.4f vs %.4f never frozen; %d caught grains)",
              withFreeze.first, without.first, withFreeze.second);
    }

    // =============================================================================================
    std::printf("\n== 3c. Tail: the cloud keeps playing after the input stops, and fades ==\n");
    {
        // A minor for 1 s (0.2-1.2 s), silence, then G major for 1 s (5.0-6.0 s), silence.
        const size_t n = (size_t)(11.0 * SR);
        Buf in(n, 0.0f);
        auto note = [&](double t0, double t1, std::initializer_list<double> hz) {
            for (size_t i = (size_t)(t0 * SR); i < (size_t)(t1 * SR); ++i)
            {
                const double t = i / SR, x = std::min(1.0, std::min((t - t0) / 0.02, (t1 - t) / 0.02));
                const double e = 0.5 - 0.5 * std::cos(M_PI * x);   // raised-cosine edges: no corners of its own
                double v = 0; for (double f : hz) v += std::sin(TWO_PI * f * t);
                in[i] = (float)(0.2 * e * v);
            }
        };
        note(0.2, 1.2, {110.0, 130.81, 164.81});
        note(5.0, 6.0, {98.0, 123.47, 146.83});
        auto render = [&](float tailMs) {
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kSize, 150); p.set(Plankton::kDensity, 30); p.set(Plankton::kPosition, 250); p.set(Plankton::kSpray, 200);
            p.set(Plankton::kSpread, 50); p.set(Plankton::kTail, tailMs);
            p.reset();
            Buf oL, oR;
            run(p, in, in, oL, oR, BLOCK);
            return oL;
        };
        const Buf o0 = render(0.0f), o4 = render(3000.0f);
        auto db = [](double v) { return 20.0 * std::log10(std::max(v, 1e-12)); };
        const double during = rms(o4, (size_t)(0.7 * SR), (size_t)(0.4 * SR));
        check(rms(o0, (size_t)(2.0 * SR), (size_t)(0.5 * SR)) < 1e-4, "Tail off: the cloud is gone 0.8 s after the note (%.1f dBFS)", db(rms(o0, (size_t)(2.0 * SR), (size_t)(0.5 * SR))));
        const double r1 = rms(o4, (size_t)(1.8 * SR), (size_t)(0.4 * SR)), r2 = rms(o4, (size_t)(2.8 * SR), (size_t)(0.4 * SR));
        check(r1 > 0.25 * during, "Tail 3 s: 0.6-1.0 s after the note the cloud still plays it (%.1f dB vs %.1f dB during)", db(r1), db(during));
        const double slope = db(r2) - db(r1);
        check(slope < -14.0 && slope > -26.0, "Tail 3 s: it fades about 20 dB per second (%.1f dB over 1 s)", slope);
        const double aE = goertzel(o4, (size_t)(2.0 * SR), (size_t)(0.8 * SR), 110.0) + goertzel(o4, (size_t)(2.0 * SR), (size_t)(0.8 * SR), 164.81);
        const double oE = goertzel(o4, (size_t)(2.0 * SR), (size_t)(0.8 * SR), 98.0) + goertzel(o4, (size_t)(2.0 * SR), (size_t)(0.8 * SR), 146.83);
        check(aE > 5.0 * oE, "Tail 3 s: the tail is the note that was played (A %.4f vs other %.4f)", aE, oE);
        // the tail starts once the live grains run past the note (Position, 250 ms) and lasts Tail (3 s), plus a grain
        check(rms(o4, (size_t)(4.7 * SR), (size_t)(0.25 * SR)) < 1e-5, "Tail 3 s: silent once the tail has run out (%.1f dBFS 3.5 s after the note)", db(rms(o4, (size_t)(4.7 * SR), (size_t)(0.25 * SR))));
        const double gG = goertzel(o4, (size_t)(5.55 * SR), (size_t)(0.4 * SR), 98.0) + goertzel(o4, (size_t)(5.55 * SR), (size_t)(0.4 * SR), 146.83);
        const double gA = goertzel(o4, (size_t)(5.55 * SR), (size_t)(0.4 * SR), 110.0) + goertzel(o4, (size_t)(5.55 * SR), (size_t)(0.4 * SR), 164.81);
        check(gG > 10.0 * gA && rms(o4, (size_t)(5.55 * SR), (size_t)(0.4 * SR)) > 0.6 * during, "Tail 3 s: a new note takes over at full level (G %.4f vs A %.4f)", gG, gA);
        const double steady = maxCurve(o4, (size_t)(0.6 * SR), (size_t)(1.15 * SR));
        check(maxCurve(o4, (size_t)(1.15 * SR), n) <= 1.3 * steady, "Tail: no clicks as the note ends, fades and a new one starts (curvature %.5f vs %.5f)", maxCurve(o4, (size_t)(1.15 * SR), n), steady);
        writeWav("build/test-tail.wav", o4, o4);
        // A drum-like input: short hits with gaps. The gaps fill with the last hit, then the next hit takes over.
        Buf hits(n, 0.0f);
        for (int h = 0; h < 8; ++h)
            for (size_t k = 0; k < (size_t)(0.12 * SR); ++k)
                hits[(size_t)((0.3 + 0.5 * h) * SR) + k] = (float)(0.6 * std::sin(TWO_PI * 180.0 * k / SR) * std::exp(-(double)k / SR * 30.0));
        Plankton p; p.prepare(SR, 512); clean(p); p.set(Plankton::kSize, 120); p.set(Plankton::kDensity, 30); p.set(Plankton::kPosition, 60);
        p.set(Plankton::kTail, 1500); p.reset();
        Buf hL, hR; run(p, hits, hits, hL, hR, BLOCK);
        double gapMin = 1e9;
        for (int h = 1; h < 7; ++h) gapMin = std::min(gapMin, rms(hL, (size_t)((0.3 + 0.5 * h - 0.12) * SR), (size_t)(0.08 * SR)));
        check(gapMin > 0.01, "Tail 1.5 s on hits every 0.5 s: the gaps stay filled (quietest gap %.1f dBFS)", db(gapMin));
    }

    // =============================================================================================
    std::printf("\n== 4. Synced grains follow the tempo ==\n");
    {
        struct Case { double bpm; int div; const char* name; double ppq0; int block; };
        const Case cases[] = {{120.0, kDivisionEighth, "1/8 at 120", 0.0, 128}, {93.0, 3, "1/16T at 93", 0.37, 160},
                              {174.0, kDivisionSixteenth, "1/16 at 174", 7.9, 37}, {60.0, kDivisionQuarter, "1/4 at 60", 2.0, 512}};
        for (const Case& c : cases)
        {
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kSync, 1); p.set(Plankton::kDivision, (float)c.div); p.set(Plankton::kSize, 20);
            p.reset();
            const size_t n = (size_t)(6.0 * SR);
            Buf in = noise(n, 3, 0.3f), oL, oR;
            g_births.clear();
            Song song; song.playing = true; song.bpm = c.bpm; song.ppqStart = c.ppq0;
            run(p, in, in, oL, oR, c.block, song);
            const double spb = SR * 60.0 / c.bpm, div = divisionBeats(c.div);
            int off = 0; double worst = 0;
            for (const Birth& b : g_births)
            {
                const double ppq = c.ppq0 + (double)b.sample / spb;
                const double frac = ppq / div - std::round(ppq / div);
                const double errSamp = std::fabs(frac * div * spb);
                worst = std::max(worst, errSamp); if (errSamp > 0.51) ++off;
            }
            // boundaries inside [ppq0, ppq0 + 6 s)
            const double ppqEnd = c.ppq0 + (6.0 * SR - 1.0) / spb;
            const double expect = std::floor(ppqEnd / div) - std::ceil(c.ppq0 / div - 1e-9) + 1.0;
            check(off == 0 && (double)g_births.size() == expect,
                  "Sync %s (block %d): %zu grains (expected %.0f), all within %.2f samples of the grid", c.name, c.block, g_births.size(), expect, worst);
        }
        // Stopped transport: free-running at the song tempo
        {
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kSync, 1); p.set(Plankton::kDivision, (float)kDivisionEighth); p.reset();
            const size_t n = (size_t)(3.0 * SR);
            Buf in = noise(n, 4, 0.3f), oL, oR;
            g_births.clear();
            Song song; song.playing = false; song.bpm = 100.0;
            run(p, in, in, oL, oR, 128, song);
            const double want = SR * 60.0 / 100.0 * 0.5;
            bool even = g_births.size() > 4;
            for (size_t i = 1; i < g_births.size(); ++i) if (std::fabs((double)(g_births[i].sample - g_births[i - 1].sample) - want) > 1.01) even = false;
            check(even, "Sync with the transport stopped: a grain every 1/8 at the song tempo (%zu grains, %.0f samples apart)", g_births.size(), want);
        }
        // Free mode: Density per second on average, with jitter
        {
            Plankton p; p.prepare(SR, 512); clean(p); p.set(Plankton::kDensity, 25); p.reset();
            const size_t n = (size_t)(8.0 * SR);
            Buf in = noise(n, 5, 0.3f), oL, oR;
            g_births.clear();
            run(p, in, in, oL, oR, 128);
            const double perSec = (double)g_births.size() / 8.0;
            double minGap = 1e9, maxGap = 0;
            for (size_t i = 1; i < g_births.size(); ++i) { const double gp = (double)(g_births[i].sample - g_births[i - 1].sample); minGap = std::min(minGap, gp); maxGap = std::max(maxGap, gp); }
            check(std::fabs(perSec - 25.0) < 1.5 && minGap >= 0.79 * SR / 25 && maxGap <= 1.21 * SR / 25,
                  "Density 25/s free: %.2f grains/s, gaps %.0f..%.0f ms (+-20 %% jitter)", perSec, minGap * 1000 / SR, maxGap * 1000 / SR);
        }
        // Synced Drift follows the song position (same place in the bar -> same drift)
        {
            Plankton p; p.prepare(SR, 512); clean(p);
            p.set(Plankton::kSync, 1); p.set(Plankton::kDrift, 100); p.set(Plankton::kDriftRate, p.param(Plankton::kDriftRate).toPlain(3.0f / 7.0f));   // 1 bar
            p.reset();
            const size_t n = (size_t)(4.0 * SR);
            Buf in = noise(n, 6, 0.3f), oL, oR;
            Song song; song.playing = true; song.bpm = 120.0;
            std::vector<int64_t> segs; std::vector<double> phases;
            run(p, in, in, oL, oR, 128, song, [&](long long) { segs.push_back(p.driftSeg_); phases.push_back(p.driftPhase_); });
            // after the run, check the last chunk's segment against the song position
            const double ppqEnd = (double)((n - 1) / 128 * 128) * 120.0 / (60.0 * SR);
            check(p.driftSeg_ == (int64_t)std::floor(ppqEnd / 4.0) && std::fabs(p.driftPhase_ - (ppqEnd / 4.0 - std::floor(ppqEnd / 4.0))) < 1e-6,
                  "Sync Drift (1 bar): wander segment %lld, phase %.3f = song position %.3f bars", (long long)p.driftSeg_, p.driftPhase_, ppqEnd / 4.0);
            std::string label; p.formatValue(Plankton::kDriftRate, p.get(Plankton::kDriftRate), label);
            check(label == "1 bar", "Drift Rate shows a musical period when synced (\"%s\")", label.c_str());
        }
    }

    // =============================================================================================
    std::printf("\n== 5. Feedback stays bounded ==\n");
    {
        Plankton p; p.prepare(SR, 512); p.setProgram(5);   // Smear Wash
        p.set(Plankton::kFeedback, 100); p.set(Plankton::kPitch, 12); p.set(Plankton::kScatter, 12); p.set(Plankton::kSnap, 1);
        p.set(Plankton::kDensity, 120); p.set(Plankton::kSize, 300); p.set(Plankton::kMix, 100); p.set(Plankton::kOutput, 6);
        p.reset();
        const size_t n = (size_t)(30.0 * SR);
        Buf in = noise(n, 7, 0.9f), oL, oR;
        for (size_t i = (size_t)(15 * SR); i < n; ++i) in[i] = 0.0f;        // loud noise for 15 s, then silence
        Buf inR = in;
        run(p, in, inR, oL, oR, BLOCK);
        double peak = 0; bool finite = true;
        for (size_t i = 0; i < n; ++i) { peak = std::max(peak, (double)std::fabs(oL[i])); peak = std::max(peak, (double)std::fabs(oR[i])); if (!std::isfinite(oL[i]) || !std::isfinite(oR[i])) finite = false; }
        float bufPeak = 0; for (float v : p.buf_) bufPeak = std::max(bufPeak, std::fabs(v));
        const double r1 = rms(oL, (size_t)(4 * SR), (size_t)(2 * SR)), r2 = rms(oL, (size_t)(12 * SR), (size_t)(2 * SR));
        const double tail = rms(oL, (size_t)(27 * SR), (size_t)(2 * SR));
        check(finite && peak <= 1.0, "Feedback 100 %% + octave shimmer, +6 dB out, 0.9 noise: output peak %.3f, finite", peak);
        check(bufPeak <= 1.0f, "buffer never exceeds full scale (peak %.3f)", bufPeak);
        check(r2 < r1 * 1.6, "cloud level does not keep growing (rms %.3f at 4 s, %.3f at 12 s)", r1, r2);
        info("12 s after the input stops the feedback tail sits at %.1f dBFS rms (guarded; it does not run away)", 20 * std::log10(std::max(1e-9, tail)));
        check(tail < 0.5, "feedback tail stays below -6 dBFS rms after the input stops (%.3f)", tail);
    }

    // =============================================================================================
    std::printf("\n== 6. No NaNs or denormals (random extremes, every program) ==\n");
    {
        bool finite = true; long long denorm = 0; int runs = 0;
        uint32_t x = 99;
        auto rnd = [&]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (float)(x >> 8) / 16777216.0f; };
        for (int pr = 0; pr < 8; ++pr)
        {
            Plankton p; p.prepare(SR, 512); p.setProgram(pr); p.reset();
            const size_t n = (size_t)(6.0 * SR);
            Buf in = noise(n, 10 + pr, 0.5f);
            for (size_t i = (size_t)(2 * SR); i < n; ++i) in[i] = (i < (size_t)(3 * SR)) ? in[i] * 1e-30f : 0.0f;   // tiny, then silence
            Buf oL, oR;
            run(p, in, in, oL, oR, 96, Song(), [&](long long s) {
                if (s % 4800 == 0 && s > 0) { const int k = (int)(rnd() * Plankton::kNumParams); p.setNormalised(k, rnd() < 0.3f ? (rnd() < 0.5f ? 0.0f : 1.0f) : rnd()); }
            });
            for (size_t i = 0; i < n; ++i)
            {
                if (!std::isfinite(oL[i]) || !std::isfinite(oR[i])) finite = false;
                if (oL[i] != 0.0f && std::fabs(oL[i]) < 1.17549435e-38f) ++denorm;
                if (oR[i] != 0.0f && std::fabs(oR[i]) < 1.17549435e-38f) ++denorm;
            }
            for (float v : p.buf_) if (v != 0.0f && std::fabs(v) < 1.17549435e-38f) ++denorm;
            ++runs;
        }
        check(finite, "all output finite over %d programs with random parameter jumps", runs);
        check(denorm == 0, "no denormal samples in the output or the buffer (%lld)", denorm);
        // Every parameter at both extremes at once
        for (int corner = 0; corner < 2; ++corner)
        {
            Plankton p; p.prepare(SR, 512);
            for (int k = 0; k < Plankton::kNumParams; ++k) if (k != Plankton::kFreeze && k != Plankton::kCatch) p.setNormalised(k, (float)corner);
            p.reset();
            const size_t n = (size_t)(8 * SR);     // Position + Spray + Drift reach 4.75 s back at the maximum
            Buf in = noise(n, 20, 0.7f), oL, oR;
            run(p, in, in, oL, oR, 512);
            double peak = 0; bool fin = true;
            for (size_t i = 0; i < n; ++i) { if (!std::isfinite(oL[i])) fin = false; peak = std::max(peak, (double)std::fabs(oL[i])); }
            check(fin && peak <= 1.0 && peak > 0.01, "every parameter at its %s: finite, peak %.3f", corner ? "maximum" : "minimum", peak);
        }
        // Sample rates
        for (double sr : {44100.0, 48000.0, 96000.0})
        {
            Plankton p; p.prepare(sr, 256); p.setProgram(2); p.reset();
            const size_t n = (size_t)(2 * sr);
            Buf in = noise(n, 30, 0.5f), oL(n), oR(n);
            bool fin = true;
            Buf l(256), r(256);
            for (size_t s = 0; s < n; s += 256)
            {
                for (int k = 0; k < 256; ++k) l[k] = r[k] = in[s + k < n ? s + k : 0];
                float* io[2] = {l.data(), r.data()};
                mpc::ProcessContext ctx; ctx.inputs = io; ctx.outputs = io; ctx.numInputs = 2; ctx.numOutputs = 2;
                ctx.numFrames = 256; ctx.sampleRate = sr;
                mpc::ScopedDenormalsOff ftz; p.process(ctx);
                for (int k = 0; k < 256; ++k) if (!std::isfinite(l[k])) fin = false;
            }
            check(fin && p.bufFrames_ >= (int)(6.0 * sr), "%.0f kHz: finite, %.1f s of buffer", sr / 1000, p.bufFrames_ / sr);
        }
    }

    // =============================================================================================
    std::printf("\n== 7. CPU (desktop, for orientation) and the voice cap ==\n");
    {
        struct Load { const char* name; float density, size; float pitch, scatter; float smear; };
        const Load loads[] = {{"default (16/s, 90 ms)", 16, 90, 0, 0, 25}, {"busy (50/s, 200 ms)", 50, 200, 0, 7, 50},
                              {"maximum (200/s, 500 ms, +24 st, scatter 24)", 200, 500, 24, 24, 100}};
        for (const Load& L : loads)
        {
            Plankton p; p.prepare(SR, 128); p.setProgram(0);
            p.set(Plankton::kDensity, L.density); p.set(Plankton::kSize, L.size); p.set(Plankton::kPitch, L.pitch);
            p.set(Plankton::kScatter, L.scatter); p.set(Plankton::kSmear, L.smear); p.set(Plankton::kFeedback, 50); p.set(Plankton::kTone, -40);
            p.set(Plankton::kSpread, 100);
            p.reset();
            const size_t n = (size_t)(20.0 * SR);
            Buf in = noise(n, 40, 0.4f), oL, oR;
            int maxActive = 0;
            const auto t0 = std::chrono::steady_clock::now();
            run(p, in, in, oL, oR, 128, Song(), [&](long long) {
                int a = 0; for (const Grain& g : p.grains_) a += g.active ? 1 : 0; maxActive = std::max(maxActive, a); });
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double pct = 100.0 * secs / 20.0;
            info("%s: %.2f %% of one desktop core, up to %d grains at once, %d triggers skipped", L.name, pct, maxActive, p.skipped_);
            if (L.density == 200) check(maxActive <= kPool && p.skipped_ > 0, "maximum density is capped at %d grains (extra triggers skipped, none cut off)", kPool);
            check(pct < 3.0, "%s under 3 %% of a desktop core (%.2f %%)", L.name, pct);
        }
    }

    // =============================================================================================
    std::printf("\n== 8. Demo ==\n");
    {
        const double bpm = 96.0;
        Buf drL, drR, padL, padR, voL, voR;
        const size_t secN = (size_t)(12.0 * SR);
        drumLoop(drL, drR, secN, bpm);
        chordPad(padL, padR, secN, {{146.83, 220.0, 277.18, 349.23}, {116.54, 174.61, 233.08, 293.66}, {130.81, 196.0, 261.63, 329.63}}, 4.0);
        voice(voL, voR, secN);
        Buf stL, stR; stabs(stL, stR, secN);
        struct Section { const char* name; int program; int source; double freezeAt, releaseAt; double sourceStop; };
        // source: 0 drums, 1 pad, 2 voice, 3 chord stabs. freeze/release in seconds within the section (<0: none).
        const Section sections[] = {
            {"dry drums", -1, 0, -1, -1, 99}, {"Rhythmic Grains on drums (synced, 96 BPM)", 4, 0, -1, -1, 99},
            {"Glitch Plankton on drums", 6, 0, -1, -1, 99}, {"Smear Wash on drums", 5, 0, -1, -1, 99},
            {"dry pad", -1, 1, -1, -1, 99}, {"Soft Cloud on the pad", 0, 1, -1, -1, 99},
            {"Frozen Pad: Freeze at 3 s, pad stops at 4 s, release at 7 s (Linger 4 s)", 1, 1, 3.0, 7.0, 4.0},
            {"Shimmer Dust on the pad", 2, 1, -1, -1, 99},
            {"dry voice", -1, 2, -1, -1, 99}, {"Reverse Swarm on the voice", 3, 2, -1, -1, 99},
            {"Deep Sea on the voice", 7, 2, -1, -1, 99},
            {"Soft Cloud on the voice: Catch held 4-8 s while the voice goes on (Linger 1.5 s)", 0, 2, 4.0, 8.0, 99},
            {"dry chord stabs", -1, 3, -1, -1, 99}, {"Shimmer Dust on the stabs (Tail 5 s)", 2, 3, -1, -1, 99},
            {"Deep Sea on the stabs (Tail 6 s)", 7, 3, -1, -1, 99},
        };
        Buf outL, outR;
        double t = 0;
        std::printf("INFO  demo sections (start time):\n");
        for (const Section& s : sections)
        {
            const Buf& sl = s.source == 0 ? drL : s.source == 1 ? padL : s.source == 2 ? voL : stL;
            const Buf& sr = s.source == 0 ? drR : s.source == 1 ? padR : s.source == 2 ? voR : stR;
            Buf inL = sl, inR = sr;
            for (size_t i = (size_t)(s.sourceStop * SR); i < secN; ++i) { inL[i] = 0; inR[i] = 0; }
            Buf oL, oR;
            if (s.program < 0) { oL = inL; oR = inR; }
            else
            {
                Plankton p; p.prepare(SR, 512); p.setProgram(s.program); p.reset();
                Song song; song.playing = (s.source == 0); song.bpm = bpm;
                const int param = (s.program == 0 && s.freezeAt > 0) ? Plankton::kCatch : Plankton::kFreeze;
                run(p, inL, inR, oL, oR, 256, song, [&](long long smp) {
                    if (s.freezeAt > 0 && smp == (long long)(s.freezeAt * SR) / 256 * 256) p.set(param, 1);
                    if (s.releaseAt > 0 && smp == (long long)(s.releaseAt * SR) / 256 * 256) p.set(param, 0);
                });
            }
            // 30 ms fades at the joins
            const size_t f = (size_t)(0.03 * SR);
            for (size_t i = 0; i < f; ++i) { const float g = (float)i / f; oL[i] *= g; oR[i] *= g; oL[secN - 1 - i] *= g; oR[secN - 1 - i] *= g; }
            std::printf("INFO    %5.1f s  %s\n", t, s.name);
            outL.insert(outL.end(), oL.begin(), oL.end());
            outR.insert(outR.end(), oR.begin(), oR.end());
            t += 12.0;
        }
        double peak = 0; for (size_t i = 0; i < outL.size(); ++i) peak = std::max(peak, (double)std::max(std::fabs(outL[i]), std::fabs(outR[i])));
        writeWav(demoPath, outL, outR);
        info("wrote %s (%.0f s, peak %.1f dBFS)", demoPath, outL.size() / SR, 20 * std::log10(peak));
        check(peak <= 1.0 && peak > 0.1, "demo peak %.2f", peak);
    }

    std::printf("\nResult: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
