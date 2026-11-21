// render - demo WAVs of Bearings on synthesised hats, percussion and a pad, at 120 BPM with the transport running.
// A probe, not a test: it prints how far left/right the output leans over time, so you can see the motion too.
//
//   clang++ -std=c++17 -O2 -I../../../framework -o build/render tools/render.cpp && build/render ../build

#define private public
#include "../../src/Bearings.cpp"
#undef private

#include "dsp/noise.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

constexpr double kSr = 44100.0, kBpm = 120.0;
constexpr double kBeat = 60.0 / kBpm;

static void writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("cannot write %s\n", path.c_str()); return; }
    const uint32_t n = (uint32_t)L.size(), data = n * 4, sr = (uint32_t)kSr, br = sr * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(sr); u32(br); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
    for (uint32_t i = 0; i < n; ++i)
    {
        const int16_t l = (int16_t)std::lround(clamp(L[i], -1.0f, 1.0f) * 32767.0f);
        const int16_t r = (int16_t)std::lround(clamp(R[i], -1.0f, 1.0f) * 32767.0f);
        std::fwrite(&l, 2, 1, f); std::fwrite(&r, 2, 1, f);
    }
    std::fclose(f);
}

static std::vector<float> hats(double secs)
{
    std::vector<float> v((size_t)(secs * kSr), 0.0f);
    Xorshift32 rng(99);
    OnePoleHP hp; hp.setCutoff(7000.0f, kSr);
    const double step = kBeat / 4.0;
    for (int k = 0; k * step < secs; ++k)
    {
        const size_t at = (size_t)(k * step * kSr);
        const float acc = (k % 4 == 2) ? 0.6f : 0.3f;
        for (size_t i = 0; i < 3000 && at + i < v.size(); ++i)
            v[at + i] += acc * rng.nextBipolar() * (float)std::exp(-(double)i / 500.0);
    }
    for (float& x : v) x = hp.process(x) * 1.6f;
    return v;
}
static std::vector<float> percs(double secs)
{
    std::vector<float> v((size_t)(secs * kSr), 0.0f);
    const double pattern[] = { 0.5, 1.25, 1.75, 2.5, 3.0, 3.5 };   // beats within each bar
    const double freqs[] = { 220.0, 330.0, 260.0, 220.0, 390.0, 290.0 };
    for (int bar = 0; bar * 4 * kBeat < secs; ++bar)
        for (int j = 0; j < 6; ++j)
        {
            const size_t at = (size_t)((bar * 4 + pattern[j]) * kBeat * kSr);
            double ph = 0.0;
            for (size_t i = 0; i < 9000 && at + i < v.size(); ++i)
            {
                const double t = (double)i / kSr;
                ph += 2.0 * M_PI * freqs[j] * (1.0 + 0.6 * std::exp(-t * 40.0)) / kSr;
                v[at + i] += 0.5f * (float)(std::sin(ph) * std::exp(-t * 14.0));
            }
        }
    return v;
}
static std::vector<float> pad(double secs)
{
    std::vector<float> v((size_t)(secs * kSr), 0.0f);
    const double notes[] = { 130.81, 155.56, 196.0, 261.63, 311.13 };   // C minor
    double ph[10] = {};
    OnePoleLP lp; lp.setCutoff(2500.0f, kSr);
    for (size_t i = 0; i < v.size(); ++i)
    {
        float s = 0.0f;
        for (int k = 0; k < 10; ++k)
        {
            ph[k] += notes[k / 2] * (k & 1 ? 1.004 : 0.996) / kSr;
            ph[k] -= std::floor(ph[k]);
            s += (float)(2.0 * ph[k] - 1.0);
        }
        const double t = (double)i / kSr;
        const float env = (float)std::fmin(1.0, t / 0.8) * (float)std::fmin(1.0, (secs - t) / 0.8);
        v[i] = lp.process(s) * 0.07f * env;
    }
    return v;
}

static void render(const std::string& dir, const char* file, const char* program, const std::vector<float>& in)
{
    Bearings p;
    p.prepare(kSr, 512);
    int prog = -1;
    for (int i = 0; i < p.numPrograms(); ++i) if (std::string(p.programName(i)) == program) prog = i;
    if (prog < 0) { std::printf("no program %s\n", program); return; }
    p.setProgram(prog);
    p.reset();

    const size_t n = in.size();
    std::vector<float> L(n), R(n), a(128), b(128);
    double ppq = 0.0;
    for (size_t pos = 0; pos < n; pos += 128)
    {
        const int frames = (int)std::min((size_t)128, n - pos);
        for (int i = 0; i < frames; ++i) a[(size_t)i] = b[(size_t)i] = in[pos + (size_t)i];
        const float* ins[2] = { a.data(), b.data() };
        float* outs[2] = { a.data(), b.data() };
        mpc::ProcessContext ctx;
        ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = frames;
        ctx.sampleRate = kSr;
        ctx.transport.valid = ctx.transport.tempoValid = ctx.transport.playing = ctx.transport.ppqValid = true;
        ctx.transport.bpm = kBpm; ctx.transport.ppqPos = ppq;
        p.process(ctx);
        for (int i = 0; i < frames; ++i) { L[pos + (size_t)i] = a[(size_t)i]; R[pos + (size_t)i] = b[(size_t)i]; }
        ppq += frames / kSr * kBpm / 60.0;
    }
    writeWav(dir + "/" + file, L, R);

    // Lean per half beat: L-R balance in dB, drawn as a bar.
    std::printf("%-22s %-14s  ", file, program);
    const size_t hop = (size_t)(kBeat * 0.5 * kSr);
    for (size_t at = 0; at + hop <= n && at < 16 * hop; at += hop)
    {
        double l = 1e-12, r = 1e-12;
        for (size_t i = at; i < at + hop; ++i) { l += (double)L[i] * L[i]; r += (double)R[i] * R[i]; }
        const double db = 10.0 * std::log10(r / l);
        std::printf("%c", db < -6 ? 'L' : db < -2 ? 'l' : db > 6 ? 'R' : db > 2 ? 'r' : '|');
    }
    std::printf("\n");
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    const double secs = 16.0 * kBeat;   // four bars
    const auto h = hats(secs), pc = percs(secs), pd = pad(secs);
    std::vector<float> kit(h.size());
    for (size_t i = 0; i < kit.size(); ++i) kit[i] = h[i] + pc[i];

    std::printf("lean per 1/8 note over 2 bars: L/l = left, | = centre, r/R = right\n");
    render(dir, "demo-dry-hats.wav",     "Centre Stage",  h);
    render(dir, "demo-hat-orbit.wav",    "Hat Orbit",     h);
    render(dir, "demo-pingpong.wav",     "Ping-Pong",     h);
    render(dir, "demo-step-percs.wav",   "Step Around",   pc);
    render(dir, "demo-scatter-percs.wav","Scatter Percs", pc);
    render(dir, "demo-formation-a.wav",  "Formation A",   pc);
    render(dir, "demo-dry-pad.wav",      "Up Close",      pd);
    render(dir, "demo-prism-pad.wav",    "Prism Pad",     pd);
    render(dir, "demo-halo-pad.wav",     "Halo",          pd);
    render(dir, "demo-tide-pad.wav",     "Tide",          pd);
    render(dir, "demo-far-away.wav",     "Far Away",      kit);
    render(dir, "demo-kick-push.wav",    "Kick Push",     kit);
    return 0;
}
