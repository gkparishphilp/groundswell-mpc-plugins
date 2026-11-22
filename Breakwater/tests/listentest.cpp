// Does Breakwater behave like an amp? A synthetic plucked guitar (Karplus-Strong) and test tones,
// checked for: clean when soft / breakup when hard, interacting tone stack, distinct cabs, a tremolo
// locked to the song, sag and compression doing something, sane levels, and CPU. Also renders the
// demo, with Fathom's real binary in front of Breakwater for the funk part when it is built.
//
//   mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
//   tests/build/listentest [demo.wav]          (run from plugins/Breakwater)
#include <vector>
#include <cstdio>
#include <cmath>
#include <string>
#include <chrono>
#include <algorithm>
#include <dlfcn.h>

#define private public
#define protected public
#include "../src/Breakwater.cpp"
#undef private
#undef protected

static const double SR = 44100.0;
static int failures = 0;
static void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }
static double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }
static char msg[512];

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

// ---- a plucked string (Karplus-Strong with a fractional-delay allpass and pick position) ----------
static unsigned g_seed = 12345;
static float rnd() { g_seed = g_seed * 1664525u + 1013904223u; return (float)(g_seed >> 8) / 16777216.0f * 2.0f - 1.0f; }

// Adds one plucked note into buf at t0 (samples). vel 0..1: level and brightness. mute shortens it.
static void pluck(std::vector<float>& buf, long long t0, float hz, float vel, float durS, bool mute = false)
{
    const double period = SR / hz;
    const int N = (int)std::floor(period - 0.5);
    const float frac = (float)(period - 0.5 - N);
    const float apc = (1.0f - frac) / (1.0f + frac);   // allpass tuning
    std::vector<float> line((size_t)N + 2, 0.0f);
    // excitation: noise, low-passed more for soft picks, with a pick-position comb
    std::vector<float> ex((size_t)N, 0.0f);
    float lp = 0.0f; const float bright = 0.25f + 0.7f * vel;
    for (int i = 0; i < N; ++i) { lp += bright * (rnd() - lp); ex[(size_t)i] = lp; }
    const int pp = std::max(1, (int)(N * 0.13));
    for (int i = N - 1; i >= pp; --i) ex[(size_t)i] -= ex[(size_t)(i - pp)];
    for (int i = 0; i < N; ++i) line[(size_t)i] = ex[(size_t)i] * vel;
    const float loss = mute ? 0.93f : (0.9965f - 0.0000045f * hz);
    const int len = (int)(durS * SR);
    int w = 0; float apz = 0.0f, prev = 0.0f, hp = 0.0f, hpx = 0.0f;
    for (int i = 0; i < len && t0 + i < (long long)buf.size(); ++i)
    {
        const float a = line[(size_t)w];
        const float avg = 0.5f * (a + prev); prev = a;
        // fractional delay allpass on the loop
        const float y = apc * avg + apz; apz = avg - apc * y;
        line[(size_t)w] = y * loss;
        w = (w + 1) % N;
        // release envelope at the end, and a gentle body high-pass
        float env = 1.0f; const int rel = (int)(0.02 * SR);
        if (i > len - rel) env = (float)(len - i) / (float)rel;
        const float o = a * env;
        hp = 0.995f * (hp + o - hpx); hpx = o;
        buf[(size_t)(t0 + i)] += hp;
    }
}

static float midiHz(int n) { return 440.0f * std::pow(2.0f, (n - 69) / 12.0f); }

// A strum across a chord (MIDI notes), low to high (down) or high to low (up).
static void strum(std::vector<float>& buf, long long t0, const std::vector<int>& chord, float vel, float durS,
                  bool up = false, bool mute = false)
{
    for (size_t k = 0; k < chord.size(); ++k)
    {
        const size_t idx = up ? chord.size() - 1 - k : k;
        const float v = vel * (0.85f + 0.15f * rnd());
        pluck(buf, t0 + (long long)(k * 0.012 * SR), midiHz(chord[idx]), v, durS, mute);
    }
}

static const float kDI = 0.16f;   // scales the plucks to a guitar DI: a hard strum peaks around -8 dBFS

// ---- rendering ---------------------------------------------------------------------------------
struct Render { std::vector<float> l, r; };

static Render render(Breakwater& p, const std::vector<float>& srcL, const std::vector<float>* srcR = nullptr,
                     double bpm = 96.0, bool playing = true, double ppq0 = 0.0, int block = 128)
{
    Render out; out.l.assign(srcL.size(), 0.0f); out.r.assign(srcL.size(), 0.0f);
    std::vector<float> l(4096), r(4096);
    const double spb = SR * 60.0 / bpm;
    const int total = (int)srcL.size();
    int pos = 0, bi = 0;
    while (pos < total)
    {
        int bs = block > 0 ? block : (32 + (bi * 97) % 481);   // block <= 0: varying sizes 32..512
        ++bi;
        const int n = std::min(bs, total - pos);
        for (int i = 0; i < n; ++i) { l[i] = srcL[(size_t)(pos + i)]; r[i] = srcR ? (*srcR)[(size_t)(pos + i)] : l[i]; }
        const float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {l.data(), r.data()};
        mpc::ProcessContext ctx;
        ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = n;
        ctx.sampleRate = SR;
        ctx.transport.valid = ctx.transport.tempoValid = true;
        ctx.transport.ppqValid = playing; ctx.transport.playing = playing; ctx.transport.bpm = bpm;
        ctx.transport.ppqPos = playing ? ppq0 + pos / spb : 0.0;
        p.process(ctx);
        for (int i = 0; i < n; ++i) { out.l[(size_t)(pos + i)] = l[i]; out.r[(size_t)(pos + i)] = r[i]; }
        pos += n;
    }
    return out;
}

static void loadProgram(Breakwater& p, int prog)
{
    const auto& v = p.programs_[(size_t)prog].plainValues;
    for (int i = 0; i < p.numParams(); ++i) p.set(i, v[(size_t)i]);
    p.reset();
}
static void fresh(Breakwater& p, int prog) { p.prepare(SR, 512); loadProgram(p, prog); }

static double rms(const std::vector<float>& x, size_t a = 0, size_t b = 0)
{
    if (b == 0 || b > x.size()) b = x.size();
    double s = 0; for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / std::max<size_t>(1, b - a));
}
static double peak(const std::vector<float>& x) { double m = 0; for (float v : x) m = std::max(m, (double)std::fabs(v)); return m; }

// Goertzel magnitude of frequency hz over x[a..b)
static double tone(const std::vector<float>& x, double hz, size_t a, size_t b)
{
    const double w = 2.0 * M_PI * hz / SR, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = a; i < b; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)(i - a) / (double)(b - a));
        const double s0 = x[i] * win + c * s1 - s2; s2 = s1; s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
}
static double thd(const std::vector<float>& y, double f0)
{
    const size_t a = (size_t)(0.5 * SR), b = (size_t)(1.5 * SR);
    const double fund = tone(y, f0, a, b);
    double h = 0; for (int k = 2; k <= 12; ++k) { const double m = tone(y, f0 * k, a, b); h += m * m; }
    return std::sqrt(h) / std::max(fund, 1e-12);
}
static std::vector<float> sine(double hz, double dbfs, double secs)
{
    std::vector<float> s((size_t)(secs * SR));
    const double a = std::pow(10.0, dbfs / 20.0);
    for (size_t i = 0; i < s.size(); ++i) s[i] = (float)(a * std::sin(2.0 * M_PI * hz * i / SR));
    return s;
}
// Octave-band level of y (dB): averaged Hann-windowed FFT power, summed over [hz/sqrt2, hz*sqrt2).
static void fft(std::vector<double>& re, std::vector<double>& im)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double a = -2.0 * M_PI / (double)len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k)
            {
                const double wr = std::cos(a * k), wi = std::sin(a * k);
                const double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                const double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr; im[i + k] += xi;
            }
    }
}
static double bandDb(const std::vector<float>& y, float hz)
{
    const size_t N = 8192;
    double sum = 0;
    for (size_t start = (size_t)(0.3 * SR); start + N <= y.size(); start += N / 2)
    {
        std::vector<double> re(N), im(N, 0.0);
        for (size_t i = 0; i < N; ++i) re[i] = y[start + i] * (0.5 - 0.5 * std::cos(2.0 * M_PI * i / N));
        fft(re, im);
        const size_t a = (size_t)(hz / std::sqrt(2.0) / SR * N), b = (size_t)(hz * std::sqrt(2.0) / SR * N);
        for (size_t k = a; k < b && k < N / 2; ++k) sum += re[k] * re[k] + im[k] * im[k];
    }
    return 10.0 * std::log10(std::max(sum, 1e-24));
}

// Strip the effects so a check sees only the part it is testing.
static void dryAmp(Breakwater& p)
{
    p.set(kRevMix, 0); p.set(kTremDepth, 0); p.set(kComp, 0); p.reset();
}

// ---- Fathom, loaded from its own native build, for the demo -------------------------------------
static VstTimeInfo g_time;
static VstIntPtr hostCb(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{
    if (op == audioMasterVersion) return 2400;
    if (op == audioMasterGetTime) return (VstIntPtr)&g_time;
    return 0;
}
struct Fathom
{
    void* lib = nullptr; AEffect* fx = nullptr;
    bool load(const char* path, int program)
    {
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        auto mainFn = (AEffect* (*)(audioMasterCallback))dlsym(lib, "VSTPluginMain");
        if (!mainFn) return false;
        fx = mainFn(hostCb);
        if (!fx || fx->magic != kEffectMagic) return false;
        fx->dispatcher(fx, effOpen, 0, 0, nullptr, 0.0f);
        fx->dispatcher(fx, effSetSampleRate, 0, 0, nullptr, (float)SR);
        fx->dispatcher(fx, effSetBlockSize, 0, 128, nullptr, 0.0f);
        fx->dispatcher(fx, effSetProgram, 0, program, nullptr, 0.0f);
        fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0.0f);
        fx->dispatcher(fx, effStartProcess, 0, 0, nullptr, 0.0f);
        return true;
    }
    void process(std::vector<float>& l, std::vector<float>& r, double bpm)
    {
        const double spb = SR * 60.0 / bpm;
        std::vector<float> a(128), b(128), oa(128), ob(128);
        for (size_t pos = 0; pos < l.size(); pos += 128)
        {
            const int n = (int)std::min<size_t>(128, l.size() - pos);
            for (int i = 0; i < n; ++i) { a[i] = l[pos + i]; b[i] = r[pos + i]; }
            g_time = VstTimeInfo(); g_time.sampleRate = SR; g_time.tempo = bpm; g_time.ppqPos = pos / spb;
            g_time.samplePos = (double)pos; g_time.timeSigNumerator = 4; g_time.timeSigDenominator = 4;
            g_time.barStartPos = std::floor(g_time.ppqPos / 4.0) * 4.0;
            g_time.flags = kVstTransportPlaying | kVstPpqPosValid | kVstTempoValid | kVstBarsValid | kVstTimeSigValid;
            float* ins[2] = {a.data(), b.data()}; float* outs[2] = {oa.data(), ob.data()};
            fx->processReplacing(fx, ins, outs, n);
            for (int i = 0; i < n; ++i) { l[pos + i] = oa[i]; r[pos + i] = ob[i]; }
        }
    }
};

// ---- the demo riffs ---------------------------------------------------------------------------
static const double kBpm = 96.0;
static long long beatS(double beats) { return (long long)(beats * SR * 60.0 / kBpm); }

// 4 bars per program. Each style is a little riff in the program's spirit.
static std::vector<float> riff(int style, int bars)
{
    std::vector<float> b((size_t)beatS(bars * 4.0 + 2.0), 0.0f);
    const std::vector<int> Cmaj7 = {48, 52, 55, 59, 64}, Am9 = {45, 52, 55, 59, 60, 64}, Fmaj7 = {41, 48, 52, 57, 60, 64},
                           G6 = {43, 47, 50, 55, 59, 64}, E9 = {40, 47, 50, 56, 59, 66}, Dm7 = {50, 57, 60, 65};
    const std::vector<std::vector<int>> prog = {Cmaj7, Am9, Fmaj7, G6};
    for (int bar = 0; bar < bars; ++bar)
    {
        const double t = bar * 4.0;
        const auto& ch = prog[(size_t)(bar % 4)];
        switch (style)
        {
            case 0:   // edge of breakup: soft verse strums, then two hard bars that dig in
            {
                const float v = bar < 2 ? 0.35f : 1.0f;
                strum(b, beatS(t), ch, v, 1.4f); strum(b, beatS(t + 1.5), ch, v * 0.7f, 0.5f, true);
                strum(b, beatS(t + 2.0), ch, v, 1.0f); strum(b, beatS(t + 3.0), ch, v * 0.8f, 0.9f, true);
                break;
            }
            case 1:   // tremolo: held chords
                strum(b, beatS(t), ch, 0.6f, 3.9f);
                break;
            case 2:   // funk: 16th chops on a 9th chord, ghosted
            {
                for (int s = 0; s < 16; ++s)
                {
                    const bool accent = (s == 0 || s == 3 || s == 6 || s == 10 || s == 12 || s == 14);
                    strum(b, beatS(t + s * 0.25), E9, accent ? 0.75f : 0.25f, accent ? 0.16f : 0.06f, s % 2 == 1, !accent);
                }
                break;
            }
            case 3:   // surf: tremolo-picked low melody, then a stab
            {
                const int mel[8] = {40, 43, 45, 47, 48, 47, 45, 43};
                for (int s = 0; s < 12; ++s) pluck(b, beatS(t + s * 0.25), midiHz(mel[(bar * 3 + s / 2) % 8] + 12), 0.8f, 0.24f);
                strum(b, beatS(t + 3.0), {40, 47, 52, 55, 59}, 0.9f, 0.9f);
                break;
            }
            case 4:   // jazz: soft chord melody
                strum(b, beatS(t), ch, 0.4f, 1.9f); strum(b, beatS(t + 2.0), Dm7, 0.35f, 1.9f);
                break;
            case 5:   // sparkly arpeggio: 8ths up and down the chord
            {
                for (int s = 0; s < 8; ++s)
                {
                    const int k = s < 4 ? s : 8 - s;
                    pluck(b, beatS(t + s * 0.5), midiHz(ch[(size_t)std::min<int>(k + 1, (int)ch.size() - 1)] + 12), 0.55f, 1.2f);
                }
                break;
            }
            case 6:   // harmonic swirl: held chord and a fill
                strum(b, beatS(t), ch, 0.6f, 3.0f);
                pluck(b, beatS(t + 3.0), midiHz(ch.back() + 12), 0.5f, 0.9f);
                break;
            default:  // push: driving 8ths on power-ish voicings
                for (int s = 0; s < 8; ++s)
                    strum(b, beatS(t + s * 0.5), {ch[0], ch[0] + 7, ch[0] + 12}, s % 2 ? 0.7f : 1.0f, 0.42f, false, s % 4 == 3);
                break;
        }
    }
    for (float& v : b) v *= kDI;
    return b;
}

int main(int argc, char** argv)
{
    const char* demoPath = argc > 1 ? argv[1] : "build/breakwater-demo.wav";
    Breakwater p;
    const int nProg = (int)p.programs_.size();
    std::printf("Breakwater listen test: %d params, %d programs\n\n", p.numParams(), nProg);

    // Programs must match the parameter count, and program 0 must equal the defaults.
    {
        bool ok = true;
        for (auto& pr : p.programs_) ok = ok && (int)pr.plainValues.size() == p.numParams();
        check(ok, "every program sets every parameter");
        bool same = true;
        for (int i = 0; i < p.numParams(); ++i)
            same = same && std::fabs(p.programs_[0].plainValues[(size_t)i] - p.param(i).def) < 1e-4f;
        check(same, "program 0 (Edge of Breakup) is the default sound");
    }

    // ---- 1. soft stays clean, hard breaks up -------------------------------------------------
    std::printf("\n== 1. Clean when soft, breakup when hard (220 Hz sine, Direct cab, no comp/reverb/trem) ==\n");
    double thdSoftEdge = 0, thdHardEdge = 0, softWorst = 0;
    for (int prog = 0; prog < nProg; ++prog)
    {
        double t[3]; const double lv[3] = {-32.0, -20.0, -8.0};
        for (int k = 0; k < 3; ++k)
        {
            fresh(p, prog); dryAmp(p); p.set(kCab, kDirect); p.reset();
            Render r = render(p, sine(220.0, lv[k], 1.6));
            t[k] = thd(r.l, 220.0);
        }
        std::printf("  %-16s THD  soft(-32) %6.2f%%   medium(-20) %6.2f%%   hard(-8) %6.2f%%\n",
                    p.programs_[(size_t)prog].name.c_str(), t[0] * 100, t[1] * 100, t[2] * 100);
        if (prog == 0) { thdSoftEdge = t[0]; thdHardEdge = t[2]; }
        softWorst = std::max(softWorst, t[0]);
    }
    std::snprintf(msg, sizeof(msg), "every program is clean played softly (worst soft THD %.2f%% < 1.5%%)", softWorst * 100);
    check(softWorst < 0.015, msg);
    std::snprintf(msg, sizeof(msg), "Edge of Breakup: soft THD %.2f%% < 1%%, hard THD %.1f%% > 5%%", thdSoftEdge * 100, thdHardEdge * 100);
    check(thdSoftEdge < 0.01 && thdHardEdge > 0.05, msg);
    {
        // Gain sweep at a medium level: distortion must rise steadily with Gain.
        double last = -1; bool mono = true;
        std::printf("  Gain sweep (British, -20 dBFS): ");
        for (int g = 0; g <= 10; g += 2)
        {
            fresh(p, 0); dryAmp(p); p.set(kCab, kDirect); p.set(kGain, (float)g); p.reset();
            const double t = thd(render(p, sine(220.0, -20.0, 1.6)).l, 220.0);
            std::printf("%d:%.1f%% ", g, t * 100);
            if (t < last * 0.9) mono = false;
            last = t;
        }
        std::printf("\n");
        check(mono, "distortion rises with Gain");
    }
    {
        // Even harmonics: the biased stages make a 2nd harmonic that grows with level.
        fresh(p, 0); dryAmp(p); p.set(kCab, kDirect); p.reset();
        Render r = render(p, sine(220.0, -14.0, 1.6));
        const size_t a = (size_t)(0.5 * SR), b = (size_t)(1.5 * SR);
        const double h2 = dB(tone(r.l, 440, a, b) / tone(r.l, 220, a, b)), h3 = dB(tone(r.l, 660, a, b) / tone(r.l, 220, a, b));
        std::snprintf(msg, sizeof(msg), "asymmetric stages make even harmonics (H2 %.1f dB, H3 %.1f dB re fundamental)", h2, h3);
        check(h2 > -40.0, msg);
    }
    {
        // Plucked guitar: a soft strum and a hard one through Edge of Breakup. The hard one must be
        // compressed by the amp (output rises much less than the 9 dB input step).
        std::vector<float> soft((size_t)(2.5 * SR)), hard((size_t)(2.5 * SR));
        g_seed = 7; strum(soft, 0, {40, 47, 52, 56, 59, 64}, 0.35f, 2.4f);
        g_seed = 7; strum(hard, 0, {40, 47, 52, 56, 59, 64}, 1.0f, 2.4f);
        for (float& v : soft) v *= kDI; for (float& v : hard) v *= kDI;
        const double inStep = dB(peak(hard) / peak(soft));
        fresh(p, 0); dryAmp(p); const double os = rms(render(p, soft).l, 0, (size_t)(0.3 * SR));
        fresh(p, 0); dryAmp(p); const double oh = rms(render(p, hard).l, 0, (size_t)(0.3 * SR));
        std::snprintf(msg, sizeof(msg), "plucked: hard strum (in +%.1f dB, peak %.1f dBFS) comes out only +%.1f dB louder: the amp is working",
                      inStep, dB(peak(hard)), dB(oh / os));
        check(dB(oh / os) < inStep - 2.0, msg);
    }

    // ---- 2. tone stack interaction ------------------------------------------------------------
    std::printf("\n== 2. The tone stack interacts ==\n");
    {
        const double fs2 = 2.0 * SR;
        auto resp = [&](int voice, float b, float m, float t, double hz) {
            p.prepare(SR, 512); p.set(kVoice, (float)voice); p.set(kBass, b); p.set(kMiddle, m); p.set(kTreble, t); p.reset();
            return dB(p.stack_.magnitude(hz, fs2) * p.makeup_);
        };
        for (int v = 0; v < 3; ++v)
        {
            const char* vn[3] = {"American", "British", "Boutique"};
            const double bassRange = resp(v, 10, 5, 5, 100) - resp(v, 0, 5, 5, 100);
            const double trebRange = resp(v, 5, 5, 10, 4000) - resp(v, 5, 5, 0, 4000);
            const double midRange = resp(v, 5, 10, 5, 500) - resp(v, 5, 0, 5, 500);
            const double bassAt400 = std::fabs(resp(v, 10, 5, 5, 400) - resp(v, 0, 5, 5, 400));
            const double bassRangeMid0 = resp(v, 10, 0, 5, 100) - resp(v, 0, 0, 5, 100);
            const double bassRangeMid10 = resp(v, 10, 10, 5, 100) - resp(v, 0, 10, 5, 100);
            const double trebAt1k = std::fabs(resp(v, 5, 5, 10, 700) - resp(v, 5, 5, 0, 700));
            const double allDown = resp(v, 0, 0, 0, 1000) - resp(v, 5, 5, 5, 1000);
            std::printf("  %-9s bass %+.1f dB @100, treble %+.1f dB @4k, middle %+.1f dB @500 | bass moves 400 Hz by %.1f dB,"
                        " treble moves 700 Hz by %.1f dB, bass range %.1f dB (mid 0) vs %.1f dB (mid 10), all at 0: %.1f dB\n",
                        vn[v], bassRange, trebRange, midRange, bassAt400, trebAt1k, bassRangeMid0, bassRangeMid10, allDown);
            std::snprintf(msg, sizeof(msg), "%s: each knob has a real range (bass %.1f, treble %.1f, middle %.1f dB)", vn[v], bassRange, trebRange, midRange);
            check(bassRange > 6 && trebRange > 6 && midRange > 3, msg);
            const double trebRangeMid0 = resp(v, 5, 0, 10, 4000) - resp(v, 5, 0, 0, 4000);
            const double trebRangeMid10 = resp(v, 5, 10, 10, 4000) - resp(v, 5, 10, 0, 4000);
            std::snprintf(msg, sizeof(msg), "%s: the knobs interact (Middle changes the Bass range by %.1f dB and the Treble range by %.1f dB)",
                          vn[v], std::fabs(bassRangeMid0 - bassRangeMid10), std::fabs(trebRangeMid0 - trebRangeMid10));
            check(std::fabs(bassRangeMid0 - bassRangeMid10) > 1.5 && std::fabs(trebRangeMid0 - trebRangeMid10) > 1.0, msg);
            std::snprintf(msg, sizeof(msg), "%s: all three at 0 is much quieter, like a passive stack (%.1f dB)", vn[v], allDown);
            check(allDown < -6.0, msg);
        }
    }

    // ---- 2b. Bright and Chime ------------------------------------------------------------------------
    std::printf("\n== 2b. Bright and Chime ==\n");
    {
        std::vector<float> noise((size_t)(2.0 * SR));
        g_seed = 41; for (float& v : noise) v = 0.004f * rnd();   // quiet, so the amp stays linear
        auto tilt = [&](int prog, float gain, bool bright, float chime) {
            fresh(p, prog); dryAmp(p); p.set(kCab, kDirect); p.set(kGain, gain); p.set(kBright, bright ? 1.0f : 0.0f);
            p.set(kChime, chime); p.reset();
            Render r = render(p, noise);
            return bandDb(r.l, 4000) - bandDb(r.l, 400);
        };
        const double lowOn = tilt(1, 1, true, 3) - tilt(1, 1, false, 3), highOn = tilt(1, 10, true, 3) - tilt(1, 10, false, 3);
        std::snprintf(msg, sizeof(msg), "Bright: +%.1f dB of treble at Gain 1, +%.1f dB at Gain 10 (a bright cap fades out)", lowOn, highOn);
        check(lowOn > 4.0 && highOn < 1.0, msg);
        auto band = [&](float chime, float hz) {
            fresh(p, 0); dryAmp(p); p.set(kCab, kDirect); p.set(kGain, 2.0f); p.set(kChime, chime); p.reset();
            Render r = render(p, noise);
            return bandDb(r.l, hz) - bandDb(r.l, 400);
        };
        const double ch = band(10, 2800) - band(0, 2800);
        std::snprintf(msg, sizeof(msg), "Chime 10 lifts the upper mids by %.1f dB over Chime 0", ch);
        check(ch > 4.0, msg);
    }

    // ---- 3. cabinets differ, no fizz ------------------------------------------------------------
    std::printf("\n== 3. The cabinets differ, and none of them fizz ==\n");
    {
        // Pink noise (Paul Kellet's filter), clean amp.
        std::vector<float> noise((size_t)(3.0 * SR));
        g_seed = 99;
        { float b0 = 0, b1 = 0, b2 = 0; for (float& v : noise) { const float w = rnd(); b0 = 0.99765f * b0 + w * 0.0990460f;
              b1 = 0.96300f * b1 + w * 0.2965164f; b2 = 0.57000f * b2 + w * 1.0526913f; v = 0.02f * (b0 + b1 + b2 + w * 0.1848f); } }
        const float bands[8] = {100, 200, 400, 800, 1600, 3200, 6400, 10000};
        double lv[4][8];
        const char* cn[4] = {"1x12 open", "2x12 open", "4x12 closed", "Direct"};
        for (int c = 0; c < 4; ++c)
        {
            fresh(p, 1); dryAmp(p); p.set(kGain, 1.0f); p.set(kCab, (float)c); p.reset();
            Render r = render(p, noise);
            for (int k = 0; k < 8; ++k) lv[c][k] = bandDb(r.l, bands[k]);
            std::printf("  %-11s", cn[c]);
            for (int k = 0; k < 8; ++k) std::printf(" %5.0f:%6.1f", bands[k], lv[c][k] - lv[c][4]);
            std::printf("  (dB re 1.6 kHz)\n");
        }
        for (int a = 0; a < 3; ++a)
            for (int b = a + 1; b < 4; ++b)
            {
                double maxd = 0; for (int k = 0; k < 8; ++k) maxd = std::max(maxd, std::fabs((lv[a][k] - lv[a][4]) - (lv[b][k] - lv[b][4])));
                std::snprintf(msg, sizeof(msg), "%s and %s differ (largest band difference %.1f dB)", cn[a], cn[b], maxd);
                check(maxd > 3.0, msg);
            }
        for (int c = 0; c < 3; ++c)
        {
            // How much the cab takes off at 10 kHz relative to 1.6 kHz, compared with no cab.
            const double fizz = (lv[c][7] - lv[c][4]) - (lv[3][7] - lv[3][4]);
            std::snprintf(msg, sizeof(msg), "%s: 10 kHz down %.1f dB against 1.6 kHz, relative to Direct (no fizz: < -24 dB)", cn[c], fizz);
            check(fizz < -24.0, msg);
        }
        // Mic and distance.
        auto cabRender = [&](float mic, float dist) {
            fresh(p, 1); dryAmp(p); p.set(kGain, 1.0f); p.set(kCab, k2x12); p.set(kMic, mic); p.set(kDistance, dist); p.reset();
            return render(p, noise);
        };
        Render cen = cabRender(0, 10), edge = cabRender(100, 10), close = cabRender(30, 2), far = cabRender(30, 100);
        const double micDiff = (bandDb(cen.l, 3200) - bandDb(cen.l, 400)) - (bandDb(edge.l, 3200) - bandDb(edge.l, 400));
        std::snprintf(msg, sizeof(msg), "Mic: centre is %.1f dB brighter than edge (3.2 kHz vs 400 Hz)", micDiff);
        check(micDiff > 4.0, msg);
        const double prox = (bandDb(close.l, 100) - bandDb(close.l, 1600)) - (bandDb(far.l, 100) - bandDb(far.l, 1600));
        std::snprintf(msg, sizeof(msg), "Distance: 2 cm has %.1f dB more low end than 100 cm (proximity effect)", prox);
        check(prox > 2.0, msg);
    }

    // ---- 4. tremolo locks to the song -----------------------------------------------------------
    std::printf("\n== 4. Tremolo follows the tempo and the beat ==\n");
    {
        auto tremEnv = [&](int mode, int div, double bpm, double ppq0, int block, bool sync, float freeRate,
                           std::vector<double>& env) {
            fresh(p, 1); dryAmp(p); p.set(kCab, kDirect); p.set(kGain, 1.0f);
            p.set(kTremMode, (float)mode); p.set(kTremDepth, 100); p.set(kTremSync, sync ? 1.0f : 0.0f);
            p.set(kTremRate, sync ? syncPos(div) : freeRate); p.reset();
            Render r = render(p, sine(440.0, -20.0, 6.0), nullptr, bpm, true, ppq0, block);
            const size_t w = 501, h = 44;   // 5 periods of 440 Hz: no ripple
            env.clear();
            for (size_t i = 0; i + w < r.l.size(); i += h) env.push_back(rms(r.l, i, i + w));
        };
        // Synced 1/4 at 100 BPM, started off the grid (ppq 3.3), with varying block sizes: in every beat
        // the quietest moment must land half way between the beats.
        const double bpm = 100.0, spb = 60.0 / bpm, hop = 44.0 / SR;   // the envelope's hop
        const double ppq0 = 3.3;
        std::vector<double> env;
        tremEnv(kSmooth, 4, bpm, ppq0, 0, true, 0, env);
        double worst = 0; int count = 0;
        for (int beat = (int)std::ceil(ppq0) + 1; ; ++beat)
        {
            const double tA = (beat - ppq0) * spb, tB = tA + spb;
            const size_t ia = (size_t)(tA / hop), ib = (size_t)(tB / hop);
            if (ib >= env.size()) break;
            size_t im = ia; for (size_t i = ia; i < ib; ++i) if (env[i] < env[im]) im = i;
            const double err = im * hop + 250.0 / SR - (tA + 0.5 * spb);   // + half the window
            worst = std::max(worst, std::fabs(err)); ++count;
        }
        std::snprintf(msg, sizeof(msg), "synced 1/4 at 100 BPM, odd start, varying blocks: %d troughs, all within %.1f ms of the off-beat (< 3 ms)", count, worst * 1000);
        check(count >= 6 && worst < 0.003, msg);
        {
            const double t0 = (std::ceil(ppq0 + 1.0) - ppq0) * spb;
            const size_t iOn = (size_t)(t0 / hop), iOff = (size_t)((t0 + 0.5 * spb) / hop);
            std::snprintf(msg, sizeof(msg), "synced: full volume on the beat (%.1f dB above the off-beat)", dB(env[iOn] / std::max(env[iOff], 1e-9)));
            check(env[iOn] > 10 * env[iOff], msg);
        }
        auto period = [&](const std::vector<double>& e) {
            // autocorrelation of the envelope; the first peak within 90% of the best is the period
            double mean = 0; for (double v : e) mean += v; mean /= (double)e.size();
            const size_t lo = (size_t)(0.05 / hop), hi = std::min((size_t)(1.5 / hop), e.size() / 2);
            std::vector<double> ac(hi + 1, 0.0); double best = 0; bool neg = false;
            for (size_t L = lo; L <= hi; ++L)
            {
                double s2 = 0; size_t cnt = 0;
                for (size_t i = (size_t)(0.5 / hop); i + L < e.size(); ++i) { s2 += (e[i] - mean) * (e[i + L] - mean); ++cnt; }
                s2 /= (double)std::max<size_t>(cnt, 1);
                ac[L] = s2;
                if (s2 < 0.0) neg = true;
                if (neg) best = std::max(best, s2);   // ignore the lobe round lag 0
            }
            bool crossed = false;
            for (size_t L = lo + 1; L < hi; ++L)
            {
                if (ac[L] < 0.0) crossed = true;
                if (crossed && ac[L] > 0.9 * best && ac[L] >= ac[L - 1] && ac[L] >= ac[L + 1])
                {
                    // parabolic refinement
                    const double d = 0.5 * (ac[L - 1] - ac[L + 1]) / (ac[L - 1] - 2 * ac[L] + ac[L + 1]);
                    return (L + d) * hop;
                }
            }
            return 0.0;
        };
        tremEnv(kSmooth, 5, 120.0, 0.0, 128, true, 0, env);
        double per = period(env);
        std::snprintf(msg, sizeof(msg), "synced 1/8D at 120 BPM: period %.1f ms (expect 375)", per * 1000);
        check(std::fabs(per - 0.375) < 0.003, msg);
        tremEnv(kSmooth, 2, 100.0, 0.0, 128, true, 0, env);
        per = period(env);
        std::snprintf(msg, sizeof(msg), "synced 1/2 at 100 BPM: period %.1f ms (expect 1200)", per * 1000);
        check(std::fabs(per - 1.2) < 0.012, msg);   // only ~4 periods in the window: 1%
        tremEnv(kOpto, 0, 120.0, 0.0, 128, false, freePos(5.0f), env);
        per = period(env);
        std::snprintf(msg, sizeof(msg), "free at 5 Hz (Opto): period %.1f ms (expect 200)", per * 1000);
        check(std::fabs(per - 0.2) < 0.003, msg);
        // Harmonic: the level barely moves, but the tone does.
        {
            fresh(p, 6); dryAmp(p); p.set(kTremDepth, 100); p.set(kCab, kDirect); p.reset();
            std::vector<float> both((size_t)(4.0 * SR));
            for (size_t i = 0; i < both.size(); ++i) both[i] = (float)(0.03 * std::sin(2 * M_PI * 150 * i / SR) + 0.03 * std::sin(2 * M_PI * 3000 * i / SR));
            Render r = render(p, both, nullptr, 96.0, true, 0.0, 128);
            const size_t spbS = (size_t)(SR * 60.0 / 96.0);
            // level on the beat vs half a beat later, for the low and high tone separately
            const size_t on = 4 * spbS, off = on + spbS / 2, w = (size_t)(0.03 * SR);
            const double loOn = tone(r.l, 150, on - w, on + w), loOff = tone(r.l, 150, off - w, off + w);
            const double hiOn = tone(r.l, 3000, on - w, on + w), hiOff = tone(r.l, 3000, off - w, off + w);
            std::snprintf(msg, sizeof(msg), "harmonic: lows %+.1f dB and highs %+.1f dB from beat to off-beat (opposite phase)",
                          dB(loOff / loOn), dB(hiOff / hiOn));
            check(dB(loOff / loOn) < -6.0 && dB(hiOff / hiOn) > 6.0, msg);
        }
    }

    // ---- 5. dynamics: sag and the compressor -------------------------------------------------------
    std::printf("\n== 5. Sag and compression ==\n");
    {
        auto stepGain = [&](float sag, float comp, float master) {
            // a soft and a hard strum; returns the output level difference in dB
            std::vector<float> soft((size_t)(2.0 * SR)), hard((size_t)(2.0 * SR));
            g_seed = 3; strum(soft, 0, {45, 52, 57, 60, 64}, 0.3f, 1.9f);
            g_seed = 3; strum(hard, 0, {45, 52, 57, 60, 64}, 1.0f, 1.9f);
            for (float& v : soft) v *= kDI; for (float& v : hard) v *= kDI;
            auto lvl = [&](const std::vector<float>& in) {
                fresh(p, 7); dryAmp(p); p.set(kSag, sag); p.set(kComp, comp); p.set(kMaster, master); p.reset();
                return rms(render(p, in).l, (size_t)(0.05 * SR), (size_t)(0.6 * SR));
            };
            return dB(lvl(hard) / lvl(soft));
        };
        const double s0 = stepGain(0, 0, 8), s10 = stepGain(10, 0, 8);
        std::snprintf(msg, sizeof(msg), "Sag squeezes hard playing: soft-to-hard step %.1f dB at Sag 0, %.1f dB at Sag 10", s0, s10);
        check(s10 < s0 - 1.0, msg);
        const double c0 = stepGain(0, 0, 3), c80 = stepGain(0, 80, 3);
        std::snprintf(msg, sizeof(msg), "Comp evens pick strokes: soft-to-hard step %.1f dB at 0%%, %.1f dB at 80%%", c0, c80);
        check(c80 < c0 - 1.5, msg);
        // Bloom: after a hard hit, sag lets the level swell back as the rail recovers.
        fresh(p, 7); dryAmp(p); p.set(kSag, 10); p.set(kMaster, 9); p.reset();
        std::vector<float> s = sine(196.0, -6.0, 1.5);
        Render r = render(p, s);
        const double early = rms(r.l, (size_t)(0.02 * SR), (size_t)(0.04 * SR)), later = rms(r.l, (size_t)(0.6 * SR), (size_t)(0.7 * SR));
        std::printf("  INFO  held hard note with Sag 10: level settles %.1f dB from its first 20 ms (compression)\n", dB(later / early));
    }

    // ---- 6. spring ----------------------------------------------------------------------------------
    std::printf("\n== 6. Spring reverb ==\n");
    {
        fresh(p, 3); p.set(kTremDepth, 0); p.set(kRevMix, 100); p.set(kDwell, 10); p.set(kCab, kDirect); p.reset();
        std::vector<float> click((size_t)(8.0 * SR), 0.0f);
        g_seed = 5; pluck(click, 0, 330.0f, 0.8f * kDI, 0.08f);
        Render r = render(p, click);
        double pk = 0; for (size_t i = 0; i < (size_t)(0.5 * SR); ++i) pk = std::max(pk, (double)std::fabs(r.l[i]));
        const double at1 = rms(r.l, (size_t)(1.0 * SR), (size_t)(1.2 * SR)), at6 = rms(r.l, (size_t)(6.5 * SR), (size_t)(7.5 * SR));
        std::snprintf(msg, sizeof(msg), "Dwell 10: tail still there at 1 s (%.1f dB re peak), gone by 7 s (%.1f dB)", dB(at1 / pk), dB(at6 / pk));
        check(dB(at1 / pk) > -40 && dB(at6 / pk) < -60, msg);
        double corr = 0, el = 0, er = 0;
        for (size_t i = (size_t)(0.1 * SR); i < (size_t)(2.0 * SR); ++i) { corr += (double)r.l[i] * r.r[i]; el += (double)r.l[i] * r.l[i]; er += (double)r.r[i] * r.r[i]; }
        corr /= std::sqrt(el * er + 1e-30);
        std::snprintf(msg, sizeof(msg), "two springs: left/right correlation of the tail %.2f (< 0.5, so it is stereo)", corr);
        check(corr < 0.5, msg);
        // Dispersion: in the first echo, the treble arrives later than the mids (the drip).
        {
            fresh(p, 3); p.set(kTremDepth, 0); p.set(kRevMix, 100); p.set(kDwell, 3); p.set(kCab, kDirect); p.set(kRevTone, 10); p.reset();
            std::vector<float> imp((size_t)(1.0 * SR), 0.0f);
            imp[100] = 0.3f;
            Render ir = render(p, imp);
            auto centroid = [&](float hz) {
                mpc::dsp::Biquad bp; bp.setCoeffs(mpc::dsp::BiquadCoeffs::bandpass(SR, hz, 2.0f));
                double e = 0, te = 0;
                for (size_t i = 0; i < (size_t)(0.060 * SR); ++i)   // the first echo only (the second is at ~70 ms)
                {
                    const float v = bp.process(ir.l[i]);
                    if (i < (size_t)(0.028 * SR)) continue;   // skip the dry click
                    e += (double)v * v; te += (double)v * v * (double)i;
                }
                return te / std::max(e, 1e-30) / SR;
            };
            const double t500 = centroid(500), t2k = centroid(2000), t4k = centroid(4000);
            std::snprintf(msg, sizeof(msg), "spring chirp: in the first echo 2 kHz arrives %.1f ms and 4 kHz %.1f ms after 500 Hz",
                          (t2k - t500) * 1000, (t4k - t500) * 1000);
            check(t2k > t500 && t4k - t500 > 0.005, msg);
        }
        // Dwell 2: shorter.
        fresh(p, 3); p.set(kTremDepth, 0); p.set(kRevMix, 100); p.set(kDwell, 2); p.set(kCab, kDirect); p.reset();
        Render r2 = render(p, click);
        const double at1b = rms(r2.l, (size_t)(1.0 * SR), (size_t)(1.2 * SR));
        std::snprintf(msg, sizeof(msg), "Dwell 2 decays faster than Dwell 10 (at 1 s: %.1f vs %.1f dB)", dB(at1b / pk), dB(at1 / pk));
        check(at1b < at1 * 0.5, msg);
    }

    // ---- 7. program levels --------------------------------------------------------------------------
    std::printf("\n== 7. Program levels ==\n");
    {
        // The same riff (soft verse, hard chorus) through every program: they should sit together.
        g_seed = 31;
        std::vector<float> common = riff(0, 4);
        std::vector<double> loud;
        for (int prog = 0; prog < nProg; ++prog)
        {
            fresh(p, prog);
            Render r = render(p, common, nullptr, kBpm);
            loud.push_back(dB(rms(r.l)));
        }
        std::vector<double> s2 = loud; std::sort(s2.begin(), s2.end());
        const double med = s2[s2.size() / 2];
        double worst = 0;
        // Each program's own riff: it must not clip.
        bool peaksOk = true;
        for (int prog = 0; prog < nProg; ++prog)
        {
            g_seed = 11 + (unsigned)prog;
            std::vector<float> in = riff(prog, 4);
            fresh(p, prog);
            Render r = render(p, in, nullptr, kBpm);
            const double pk = std::max(peak(r.l), peak(r.r));
            std::printf("  %-16s same riff: rms %6.1f dBFS (%+.1f)   own riff: in peak %6.1f, out peak %6.1f, out rms %6.1f dBFS\n",
                        p.programs_[(size_t)prog].name.c_str(), loud[(size_t)prog], loud[(size_t)prog] - med, dB(peak(in)), dB(pk), dB(rms(r.l)));
            if (pk > 0.89) peaksOk = false;
            worst = std::max(worst, std::fabs(loud[(size_t)prog] - med));
        }
        check(peaksOk, "every program's own riff peaks under -1 dBFS");
        std::snprintf(msg, sizeof(msg), "on the same riff, every program is within %.1f dB of the median loudness (< 3 dB)", worst);
        check(worst < 3.0, msg);
    }

    // ---- 8. robustness ------------------------------------------------------------------------------
    std::printf("\n== 8. Robustness ==\n");
    {
        bool finite = true;
        for (int prog = 0; prog < nProg; ++prog)
        {
            fresh(p, prog); p.set(kGain, 10); p.set(kMaster, 10); p.set(kSag, 10); p.set(kInput, 18); p.set(kDwell, 10); p.set(kRevMix, 100); p.reset();
            Render r = render(p, sine(110.0, 0.0, 1.0), nullptr, 120, true, 0, 0);
            for (float v : r.l) if (!std::isfinite(v) || std::fabs(v) > 8.0f) finite = false;
        }
        check(finite, "everything maxed on a full-scale input: finite and bounded");
        // Parameter 0 (Output) nudged 0 -> 1 -> 0 normalised: nothing but the level may change.
        fresh(p, 0);
        const int v0 = p.voice_; const float g0 = p.gainT_;
        p.setNormalised(0, 1.0f); p.setNormalised(0, p.param(0).toNormalised(0.0f));
        check(p.voice_ == v0 && p.gainT_ == g0 && std::fabs(p.get(0)) < 1e-4f, "parameter 0 (Output) nudge is harmless");
        for (double rate : {48000.0, 96000.0})
        {
            Breakwater q; q.prepare(rate, 512);
            const auto& v = q.programs_[3].plainValues; for (int i = 0; i < q.numParams(); ++i) q.set(i, v[(size_t)i]); q.reset();
            std::vector<float> l(256), rr(256); bool ok = true;
            for (int b = 0; b < (int)(rate * 2 / 256); ++b)
            {
                for (int i = 0; i < 256; ++i) l[i] = rr[i] = 0.2f * (float)std::sin(2 * M_PI * 220 * (b * 256 + i) / rate);
                const float* ins[2] = {l.data(), rr.data()}; float* outs[2] = {l.data(), rr.data()};
                mpc::ProcessContext ctx; ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = 256; ctx.sampleRate = rate;
                q.process(ctx);
                for (float x : l) if (!std::isfinite(x) || std::fabs(x) > 4) ok = false;
            }
            std::snprintf(msg, sizeof(msg), "runs at %.0f kHz (spring K = %d)", rate / 1000, q.spring_.s[0].K);
            check(ok, msg);
        }
    }

    // ---- 9. CPU -------------------------------------------------------------------------------------
    std::printf("\n== 9. CPU ==\n");
    {
        g_seed = 21;
        std::vector<float> in = riff(2, 8);
        in.resize((size_t)(20.0 * SR), 0.0f);
        for (size_t i = 0; i < in.size(); ++i) in[i] += 1e-4f * rnd();
        auto timeIt = [&](const char* label, int prog, bool rev, bool trem, bool cab) {
            fresh(p, prog);
            if (!rev) p.set(kRevMix, 0); else p.set(kRevMix, 40);
            if (!trem) p.set(kTremDepth, 0); else { p.set(kTremDepth, 60); p.set(kTremMode, kHarmonic); }
            if (!cab) p.set(kCab, kDirect);
            p.reset();
            const auto t0 = std::chrono::high_resolution_clock::now();
            Render r = render(p, in, nullptr, kBpm, true, 0, 128);
            const auto t1 = std::chrono::high_resolution_clock::now();
            const double secs = std::chrono::duration<double>(t1 - t0).count();
            const double pct = 100.0 * secs / (in.size() / SR);
            std::printf("  %-38s %.3f%% of one desktop core\n", label, pct);
            (void)r;
            return pct;
        };
        const double core = timeIt("amp only (no cab, spring, trem)", 0, false, false, false);
        const double withCab = timeIt("amp + cab", 0, false, false, true);
        const double withRev = timeIt("amp + cab + spring", 0, true, false, true);
        const double all = timeIt("everything (harmonic trem, spring, cab)", 6, true, true, true);
        (void)core; (void)withCab; (void)withRev;
        std::snprintf(msg, sizeof(msg), "everything on: %.3f%% of a desktop core (< 0.6%%; MPC roughly 10-20x that)", all);
        check(all < 0.6, msg);
    }

    // ---- demo ---------------------------------------------------------------------------------------
    std::printf("\n== Demo ==\n");
    {
        std::vector<float> L, R;
        auto append = [&](Render r) {
            // a short fade at the end, so one program's spring tail doesn't cut off into the next
            const size_t f = (size_t)(0.25 * SR);
            for (size_t i = 0; i < f && i < r.l.size(); ++i) { const float g = (float)i / (float)f; r.l[r.l.size() - 1 - i] *= g; r.r[r.r.size() - 1 - i] *= g; }
            L.insert(L.end(), r.l.begin(), r.l.end()); R.insert(R.end(), r.r.begin(), r.r.end());
        };
        for (int prog = 0; prog < nProg; ++prog)
        {
            g_seed = 11 + (unsigned)prog;
            std::vector<float> in = riff(prog, 4);
            fresh(p, prog);
            append(render(p, in, nullptr, kBpm));
            std::printf("  %5.1f s  %s\n", (L.size() - in.size()) / SR, p.programs_[(size_t)prog].name.c_str());
        }
        // Fathom (Classic Quack) -> Breakwater (Funk Glass), if Fathom's native build is there.
        Fathom f;
        const char* fp = "../Fathom/build/native/fathom.dylib";
        if (f.load(fp, 1))
        {
            char name[64] = {0};
            f.fx->dispatcher(f.fx, effGetProgramName, 0, 0, name, 0.0f);
            g_seed = 77;
            std::vector<float> in = riff(2, 8), inR = in;
            f.process(in, inR, kBpm);
            fresh(p, 2);
            const double at = L.size() / SR;
            append(render(p, in, &inR, kBpm));
            std::printf("  %5.1f s  Fathom (%s) -> Breakwater (Funk Glass)\n", at, name);
        }
        else std::printf("  (Fathom's native build not found at %s: skipping the Fathom pass)\n", fp);
        writeWav(demoPath, L, R);
        std::printf("  wrote %s (%.1f s)\n", demoPath, L.size() / SR);
    }

    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
