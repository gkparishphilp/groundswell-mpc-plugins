// Low Tide - tape with a memory and a lifespan. A lo-fi effect by Groundswell.
//
// Most lo-fi boxes are a fixed colour. Low Tide moves: the audio runs through a virtual tape loop that is
// dubbed again on every pass, so a loop starts hi-fi and wears out bar by bar, a slow Tide cycle swings the
// whole thing between clean and wrecked in time with the song, and now and then the tape has an accident
// on the beat.
//
// Signal flow (per channel unless noted)
// --------------------------------------
//   in ─┬───────────────────────────────────────────────────────────────────────────── dry ──┐
//       └─> 1 Crunch ─> 2 Generation copy ─> 3 Tape loop ─> 4 Print-through ─> 5 Azimuth     │
//             ─> 6 Transport (wow, flutter, smear, drag, slips, crinkle) ─> 7 Dropouts, cuts  ├─> Mix ─> Output
//             ─> 8 Tape compression + breathing hiss ──────────────────────────────── wet ───┘
//
//   Wear. Every stage's amount is scaled by the effective Wear w (0..1): stage amount x 2w, clamped. So at
//   Wear 50 % each stage sits at its own knob setting, at 0 % everything is clean, at 100 % everything is at
//   twice its setting. The Tide moves w: w = Wear x (1 - Depth x (1 - c)), where c (0..1) is the Tide
//   shape at the current song position. Depth 100 % washes all the way back to clean at the low point.
//
//   1 Crunch (old-sampler resample). Input filter, sample and hold at 26.04 kHz / 2^(Rec Pitch/12), Bits,
//     output filter. Filters: Clean (4-pole filters that track the rate), Dirty (a loose 2-pole input filter
//     that lets some aliasing in, and a fixed output filter that lets the images through: the crunch grows
//     with Rec Pitch), Raw (no filters). Crunch blends it in.
//   2 Generation copy. The live signal, as it would sound after g dubs. g glides to the generation counter
//     (x 2w). Each generation: the top end falls (a 2-pole low pass from 20 kHz towards 2.2 kHz), the lows
//     thin (a 1-pole high pass up to ~140 Hz), transients soften (tape saturation, drive 1 + 0.06 g), hiss
//     builds (Hiss x sqrt(g), darkened by the same low pass) and the pitch smears (a random wander, sqrt(g)).
//   3 Tape loop. A real loop of Bars bars (int16, up to 32 s), aligned to the bar lines. What is recorded on
//     it is the copy blended with what was already there: write = copy x (1 - Memory) + ghost x Memory,
//     where the ghost is the previous pass after one more generation of loss. Memory 0 = what you hear is
//     the live copy (safe on a mix); Memory 100 % = the loop keeps replaying itself and crumbles.
//     Generations: on each pass the counter moves (Hold at the cap / Restart from fresh / Bounce back down).
//     Fresh (any change of the switch, or starting the transport) snaps back to generation 0 and erases the
//     loop over one pass (the erase head: no buffer clearing in the audio thread).
//   4 Print-through. Faint, mid-range ghosts from the loop: one Print Time after (post-echo) and one Print
//     Time before (pre-echo). The pre-echo is read from the previous pass of the loop, a Print Time ahead,
//     so it needs no look-ahead and adds no latency; it is a true pre-echo for material that repeats with
//     the loop, as most MPC sequences do.
//   5 Azimuth. The head out of line: one channel lags the other by up to 0.35 ms, the side signal shrinks
//     (towards mono, combing as it does) and the top end dulls; Az Drift makes the error wander and swap
//     sides slowly.
//   6 Transport. One modulated delay line (a few ms on average, up to ~70 ms during a drag) carries the wow
//     (0.55 Hz, wandering), flutter (6.3 + 10.7 Hz), the generation smear and the speed accidents.
//   7 Accidents. On a grid (Density) the dice roll (Chance x 2w). Allowed types, one at a time:
//       Dropouts: level dips with top-end loss, 1/16-1/4 beat.      Drag: the motor slows (pitch sags up to
//       ~1.4 semitones) and recovers, 1-2 beats.                    Slips: a short pitch hiccup, 1/8 beat.
//       Crinkle: a fast flutter burst with level wobble, 1/4-1/2 beat.   Chan Cut: one side drops out for
//       half a beat. Severity sets how deep. Pattern repeats the dice every 1/2/4 bars.
//   8 Tape compression and hiss. An envelope (1 ms / 250 ms) gently compresses the wet signal; Breathe makes
//     the hiss ride up under loud hits and sink in the gaps.
//
// Clock. Playing: the song position, but counted as bars played (a looping sequence keeps the Tide and the
// generations moving instead of jumping back); starting the transport counts from the song position and is
// a Fresh. Stopped: a free-running clock at the host tempo (120 BPM if none), so everything keeps moving.
//
// Realtime: all buffers are allocated in prepare(); process() does no allocation, locking or I/O and no
// transcendental function per sample (sines are a parabolic approximation; coefficients are per block).

#include "mpc_plugin.h"
#include "dsp/smoother.h"
#include "dsp/svf.h"
#include "dsp/noise.h"
#include "TapeParts.h"

#include <cmath>
#include <cstdint>
#include <vector>

// Test hooks (tests/listentest.cpp defines these before including this file).
#ifndef LOWTIDE_ACCIDENT_HOOK
#define LOWTIDE_ACCIDENT_HOOK(offset, type)
#endif
#ifndef LOWTIDE_PASS_HOOK
#define LOWTIDE_PASS_HOOK(offset, gen)
#endif

namespace {

using namespace lowtide;

// Parameter order and ids are permanent: append new parameters at the end.
enum ParamIndex
{
    kOutput = 0,   // dB: output level (parameter 0 is nudged by the host on every load: harmless here)
    kMix,          // %: dry/wet
    kWear,         // %: the macro. Scales every stage (50 % = each stage at its own setting)
    kGens,         // 0..16: the cap - how many dubs before the copy stops getting worse
    kBars,         // choice: loop length, 1/2/4/8 bars (one generation per pass)
    kAtCap,        // choice: at the cap, Hold / Restart from fresh / Bounce back down
    kFresh,        // switch: any change snaps back to a clean copy (generation 0) and erases the loop
    kMemory,       // %: how much of the previous pass is re-recorded (0 = live copy, 100 = the loop replays itself)
    kTideDepth,    // %: how far the Tide washes Wear back towards clean
    kTideRate,     // choice: Tide cycle length, 1..32 bars
    kTideShape,    // choice: Swell (sine), Erode (wear out, then wash back), Ebb (triangle), Steps (per bar), Drift (random)
    kChance,       // %: chance of an accident at each grid point (x 2w)
    kDensity,      // choice: grid for the dice - Bars, Halves, Beats, 8ths
    kSeverity,     // %: how deep an accident goes (x 2w)
    kDropout,      // switch: allow dropouts
    kDrag,         // switch: allow motor drag
    kSlip,         // switch: allow speed slips
    kCrinkle,      // switch: allow crinkled-tape flutter bursts
    kChanCut,      // switch: allow one channel cutting out for half a beat
    kPattern,      // choice: Free, or repeat the dice every 1/2/4 bars
    kCrunch,       // %: blend of the old-sampler resample (x 2w)
    kPitch,        // semitones: record pitch-up; the crunch rate is 26.04 kHz / 2^(st/12)
    kBits,         // 8..16: converter bits
    kFilters,      // choice: Clean / Dirty / Raw converter filters
    kPrint,        // %: print-through ghosts (x 2w)
    kPrintTime,    // choice: 1/2, 1 or 2 beats between the audio and its ghosts
    kAzimuth,      // %: head misalignment (x 2w)
    kAzDrift,      // %: how much the misalignment wanders
    kHiss,         // %: hiss of one generation (x 2w); grows with sqrt(generation)
    kBreathe,      // %: hiss rides with the music + gentle tape compression (x 2w)
    kWow,          // %: slow pitch drift (x 2w)
    kFlutter,      // %: fast pitch flutter (x 2w)
    // Meters, written by the plugin. Not saved, not automatable.
    kWearNow,      // %: effective wear right now (Wear moved by the Tide)
    kGenNow,       // the generation counter
    kAccidentNow,  // which accident is happening
    kNumParams
};

enum AccidentType { kAccNone = 0, kAccDropout, kAccDrag, kAccSlip, kAccCrinkle, kAccCutL, kAccCutR };

constexpr double kMinBpm = 40.0, kMaxBpm = 300.0;
constexpr double kLoopSeconds = 32.0;        // tape loop capacity (at up to 48 kHz; shorter above)
constexpr double kLoopMaxRate = 48000.0;
constexpr float  kLoopScale = 16384.0f;      // int16 loop: +-2.0 full scale (soft-clipped before writing)
constexpr double kModSeconds = 0.25;         // transport delay line
constexpr int    kAzRing = 256;              // azimuth delay ring (0.35 ms at 192 kHz = 67 samples)
constexpr float  kCrunchBaseRate = 26040.0f;
constexpr int    kBarsChoices[4] = {1, 2, 4, 8};
constexpr int    kTideBars[6] = {1, 2, 4, 8, 16, 32};
constexpr double kPrintBeats[3] = {0.5, 1.0, 2.0};

} // namespace

class LowTide : public mpc::Plugin
{
public:
    LowTide() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        auto meter = [this](Param p) { p.automatable = false; p.saved = false; addParam(p); };

        addParam(floatParam ("output",    "Output",      -24.0f, 6.0f,   0.0f,  "dB", 1));
        addParam(floatParam ("mix",       "Mix",           0.0f, 100.0f, 100.0f, "%", 0));
        addParam(floatParam ("wear",      "Wear",          0.0f, 100.0f, 50.0f, "%",  0));
        addParam(intParam   ("gens",      "Generations",   0, 16, 6));
        addParam(choiceParam("bars",      "Bars",        {"1 Bar", "2 Bars", "4 Bars", "8 Bars"}, 1));
        addParam(choiceParam("atcap",     "At Cap",      {"Hold", "Restart", "Bounce"}, 0));
        addParam(boolParam  ("fresh",     "Fresh",       false));
        addParam(floatParam ("memory",    "Memory",        0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(floatParam ("tidedepth", "Tide Depth",    0.0f, 100.0f, 50.0f, "%",  0));
        addParam(choiceParam("tiderate",  "Tide Rate",   {"1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars", "32 Bars"}, 3));
        addParam(choiceParam("tideshape", "Tide Shape",  {"Swell", "Erode", "Ebb", "Steps", "Drift"}, 0));
        addParam(floatParam ("chance",    "Chance",        0.0f, 100.0f, 15.0f, "%",  0));
        addParam(choiceParam("density",   "Density",     {"Bars", "Halves", "Beats", "8ths"}, 2));
        addParam(floatParam ("severity",  "Severity",      0.0f, 100.0f, 40.0f, "%",  0));
        addParam(boolParam  ("dropout",   "Dropouts",    true));
        addParam(boolParam  ("drag",      "Drag",        true));
        addParam(boolParam  ("slip",      "Slips",       true));
        addParam(boolParam  ("crinkle",   "Crinkle",     true));
        addParam(boolParam  ("chancut",   "Chan Cut",    false));
        addParam(choiceParam("pattern",   "Pattern",     {"Free", "1 Bar", "2 Bars", "4 Bars"}, 0));
        addParam(floatParam ("crunch",    "Crunch",        0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(intParam   ("pitch",     "Rec Pitch",     0, 24, 5, "st"));
        addParam(intParam   ("bits",      "Bits",          8, 16, 12));
        addParam(choiceParam("filters",   "Filters",     {"Clean", "Dirty", "Raw"}, 1));
        addParam(floatParam ("print",     "Print",         0.0f, 100.0f, 25.0f, "%",  0));
        addParam(choiceParam("printtime", "Print Time",  {"1/2", "1 Beat", "2 Beats"}, 1));
        addParam(floatParam ("azimuth",   "Azimuth",       0.0f, 100.0f, 25.0f, "%",  0));
        addParam(floatParam ("azdrift",   "Az Drift",      0.0f, 100.0f, 40.0f, "%",  0));
        addParam(floatParam ("hiss",      "Hiss",          0.0f, 100.0f, 30.0f, "%",  0));
        addParam(floatParam ("breathe",   "Breathe",       0.0f, 100.0f, 40.0f, "%",  0));
        addParam(floatParam ("wow",       "Wow",           0.0f, 100.0f, 30.0f, "%",  0));
        addParam(floatParam ("flutter",   "Flutter",       0.0f, 100.0f, 25.0f, "%",  0));
        meter(floatParam ("wearnow",  "Wear Now", 0.0f, 100.0f, 0.0f, "%", 0));
        meter(intParam   ("gennow",   "Gen",      0, 16, 0));
        meter(choiceParam("accident", "Accident", {"-", "Dropout", "Drag", "Slip", "Crinkle", "Cut L", "Cut R"}, 0));

        //  Out Mix Wear Gen Bar Cap Frs Mem | TDp TRt TSh | Chn Den Sev Drp Drg Slp Crk Cut Pat | Crn Pch Bit Flt | Prn PTm Az AzD Hss Brt Wow Flt
        addProgram("Worn Shores",       {0, 100, 50,  6, 1, 0, 0,  0,   50, 3, 0,   15, 2, 40, 1, 1, 1, 1, 0, 0,   0,  5, 12, 1,   25, 1, 25, 40, 30, 40, 30, 25});
        addProgram("Dub Of A Dub",      {0, 100, 60, 12, 0, 0, 0, 40,    0, 3, 0,   10, 1, 40, 1, 0, 0, 1, 0, 0,   0,  5, 12, 1,   45, 1, 30, 30, 35, 50, 25, 20});
        addProgram("Eight Bar Erosion", {0, 100, 85,  7, 0, 1, 0,  0,  100, 3, 1,   20, 2, 50, 1, 1, 1, 1, 0, 0,  30,  7, 12, 1,   30, 1, 35, 40, 35, 40, 35, 30});
        addProgram("Sampler Crunch",    {0, 100, 50,  2, 1, 0, 0,  0,    0, 3, 0,    0, 2, 30, 1, 1, 1, 1, 0, 0,  90, 12, 12, 1,   10, 1, 10, 20, 15, 30, 15, 15});
        addProgram("Disintegration",    {0, 100, 70, 16, 2, 0, 0, 90,   30, 4, 2,   25, 1, 60, 1, 1, 0, 1, 0, 0,   0,  5, 12, 1,   40, 1, 40, 50, 45, 50, 40, 30});
        addProgram("Warped Cassette",   {0, 100, 60,  4, 1, 0, 0,  0,   40, 2, 4,   45, 2, 70, 1, 1, 1, 1, 1, 2,   0,  5, 12, 1,   20, 1, 45, 70, 40, 50, 80, 60});
        addProgram("Mix Patina",        {0, 100, 35,  4, 1, 0, 0,  0,   30, 4, 0,    0, 2, 30, 1, 1, 0, 0, 0, 0,   0,  5, 12, 1,   15, 1, 10, 30, 15, 25, 15, 15});
        addProgram("Lifespan",          {0, 100, 75,  6, 0, 2, 0, 25,   60, 2, 3,   20, 2, 50, 1, 1, 1, 1, 0, 1,  20,  5, 12, 1,   30, 1, 25, 40, 30, 40, 30, 25});
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = sampleRate > 1000.0 ? sampleRate : 44100.0;
        loopCap_ = (int)(kLoopSeconds * (sr_ < kLoopMaxRate ? sr_ : kLoopMaxRate));
        for (int c = 0; c < 2; ++c) loop_[c].assign((size_t)loopCap_, 0);
        modLen_ = 1;
        while (modLen_ < (int)(kModSeconds * sr_) + 8) modLen_ <<= 1;
        modMask_ = modLen_ - 1;
        for (int c = 0; c < 2; ++c) mod_[c].assign((size_t)modLen_, 0.0f);
        for (int c = 0; c < 2; ++c) genSvf_[c].setSampleRate(sr_);
        envAtt_ = 1.0f - (float)std::exp(-1.0 / (0.001 * sr_));
        envRel_ = (float)std::exp(-1.0 / (0.25 * sr_));
        gateA_ = 1.0f - (float)std::exp(-1.0 / (0.005 * sr_));
        accSmoothA_ = 1.0f - (float)std::exp(-1.0 / (0.002 * sr_));
        meterInterval_ = (int)(0.1 * sr_);
        crunchKey_ = -1;
        L_ = 0;
        reset();
    }

    void reset() override
    {
        // The tape loop is NOT cleared here (megabytes, and reset() runs on the audio thread after a state
        // or program load): the erase head below makes sure stale tape is never heard.
        for (int c = 0; c < 2; ++c) std::fill(mod_[c].begin(), mod_[c].end(), 0.0f);
        for (int c = 0; c < 2; ++c) for (int k = 0; k < kAzRing; ++k) az_[c][k] = 0.0f;
        for (int c = 0; c < 2; ++c)
        {
            genSvf_[c].reset(); genHp_[c].z = 0.0f; passLp_[c].z = 0.0f; printLp_[c].z = 0.0f; printHp_[c].z = 0.0f;
            azLp_[c].z = 0.0f; contactLp_[c].z = 0.0f; crunch_[c].reset();
            hissPre_[c].reset(); hissPost_[c].reset();
        }
        hissPre_[0].reseed(0x1111u); hissPre_[1].reseed(0x2222u);
        hissPost_[0].reseed(0x3333u); hissPost_[1].reseed(0x4444u);
        passNoise_.reseed(0x5555u);
        crunchPhase_ = 0.0f;
        modW_ = 0; azW_ = 0; idx_ = 0;
        env_ = 0.0f;
        wowPh_ = 0.0f; flutPh_ = 0.0f; flut2Ph_ = 0.37f; wanderZ_ = 0.0f; wanderT_ = 0.0f; wanderCount_ = 0;
        wowRateZ_ = 0.0f; azR_ = 0.0f; azRT_ = 0.0f; azCount_ = 0;
        beat_ = 0.0; expPpq_ = 0.0; wasPlaying_ = false; needResync_ = true;
        genTarget_ = 0; genPos_ = 0.0f; dir_ = 1; freshGlide_ = false;
        memGate_ = 0.0f; preGate_ = 0.0f; postGate_ = 0.0f;
        eraseLeft_ = L_ > 0 ? L_ : 1 << 30; sinceErase_ = 0;
        acc_ = Acc(); accDelayZ_ = 0.0f; accMeter_ = 0;
        rng_.s = 0x2545F491u; lockActive_ = false; lockN_ = 0; lockBar_ = 0; lockSeed_ = rng_.s;
        lastFresh_ = getBool(kFresh);
        mix_.snap(get(kMix) * 0.01f);
        out_.snap(dbToGain(get(kOutput)));
        crunchAmt_.snap(0.0f);
        tauRamp_.snap(0.0f);
        meterCountdown_ = 0;
        for (float& m : lastMeter_) m = -1.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (n <= 0 || ctx.numOutputs < 1 || loop_[0].empty()) return;

        // ---- clock ------------------------------------------------------------------------------
        const mpc::Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        double bpm = (tr.valid && tr.tempoValid) ? tr.bpm : 120.0;
        bpm = bpm < kMinBpm ? kMinBpm : (bpm > kMaxBpm ? kMaxBpm : bpm);
        const double spb = sr_ * 60.0 / bpm;
        const double bps = 1.0 / spb;
        double barBeats = 4.0;
        if (tr.valid && tr.timeSigNumerator > 0 && tr.timeSigDenominator > 0)
            barBeats = mpc::clamp((double)tr.timeSigNumerator * 4.0 / (double)tr.timeSigDenominator, 1.0, 16.0);

        bool relocated = false;
        if (playing)
        {
            if (!wasPlaying_)
            {
                beat_ = tr.ppqPos;                 // count from the song position
                needResync_ = true;
                startFresh();                      // every play starts from a clean copy: renders repeat
            }
            else if (std::fabs(tr.ppqPos - expPpq_) > 0.05)
            {
                // A jump (a looping sequence wrapping, or a relocate): keep counting bars played, but line the
                // position inside the bar up with the song.
                double d = std::fmod(tr.ppqPos, barBeats) - std::fmod(beat_, barBeats);
                if (d > 0.5 * barBeats) d -= barBeats;
                if (d < -0.5 * barBeats) d += barBeats;
                beat_ += d;
                if (std::fabs(d) > 1e-3) { needResync_ = true; relocated = true; }
            }
            expPpq_ = tr.ppqPos + n * bps;
        }
        else if (wasPlaying_) needResync_ = true;
        wasPlaying_ = playing;

        // ---- Tide and Wear --------------------------------------------------------------------------
        const float wear  = get(kWear) * 0.01f;
        const float depth = get(kTideDepth) * 0.01f;
        const double cycleBeats = kTideBars[mpc::clamp(getInt(kTideRate), 0, 5)] * barBeats;
        const float c = tideShape(getInt(kTideShape), beat_, cycleBeats, barBeats);
        const float w = clampf(wear * (1.0f - depth * (1.0f - c)), 0.0f, 1.0f);
        const float wc = 2.0f * w;
        wearNow_ = w;

        // ---- loop length, Fresh, generations --------------------------------------------------------
        int barsEff = kBarsChoices[mpc::clamp(getInt(kBars), 0, 3)];
        double Ld = barsEff * barBeats * spb;
        while (Ld > (double)loopCap_ && barsEff > 1) { barsEff >>= 1; Ld *= 0.5; }
        int Li = (int)(Ld > (double)loopCap_ ? (double)loopCap_ : Ld);
        if (Li < 64) Li = 64;
        if (L_ == 0 || std::abs(Li - L_) > 2) { L_ = Li; beginErase(); needResync_ = true; }

        const bool fr = getBool(kFresh);
        if (fr != lastFresh_) { lastFresh_ = fr; startFresh(); }
        if (relocated) beginErase();

        const int cap = mpc::clamp(getInt(kGens), 0, 16);
        if (genTarget_ > cap) genTarget_ = cap;
        atCap_ = mpc::clamp(getInt(kAtCap), 0, 2);
        cap_ = cap;
        {
            const float rate = freshGlide_ ? freshRate_ : (float)(1.0 / (0.5 * spb));   // a dub lands over half a beat
            const float tgt = (float)genTarget_;
            const float step = rate * (float)n;
            if (genPos_ < tgt) genPos_ = genPos_ + step > tgt ? tgt : genPos_ + step;
            else if (genPos_ > tgt) genPos_ = genPos_ - step < tgt ? tgt : genPos_ - step;
            if (genPos_ == tgt) freshGlide_ = false;
        }
        const float g = genPos_ * wc;                                   // effective generations, 0..32

        if (needResync_) { resync(barBeats, spb); needResync_ = false; }

        // ---- per-block coefficients ------------------------------------------------------------------
        // 2 generation copy
        const float genFc = 2200.0f + 17800.0f * std::pow(0.85f, g);
        for (int ch = 0; ch < 2; ++ch) { genSvf_[ch].set(genFc, 0.12f); genHp_[ch].setHz(10.0f + 4.0f * g, sr_); }
        const float lpMix = g < 1.0f ? g : 1.0f;
        const float drive = 1.0f + 0.06f * g;
        const float invDrive = 1.0f / drive;
        const float hissAmt = clampf(get(kHiss) * 0.01f * wc, 0.0f, 1.0f);
        const float hissAmp = 0.01f * hissAmt / 0.24f;                // 100 % = about -40 dB rms of pink hiss
        const float genHissAmp = hissAmp * std::sqrt(g);

        // 3 tape loop: one more generation of loss per pass
        const float memory = get(kMemory) * 0.01f;
        passLp_[0].setHz(1500.0f + 18500.0f * std::pow(0.6f, wc), sr_); passLp_[1].a = passLp_[0].a;
        const float passGain = 1.0f - 0.015f * wc;
        const float passMix = wc < 1.0f ? wc : 1.0f;                   // Wear 0: the loop replays itself exactly

        // 1 crunch
        const int filters = mpc::clamp(getInt(kFilters), 0, 2);
        const int pitch = mpc::clamp(getInt(kPitch), 0, 24);
        const int bits = mpc::clamp(getInt(kBits), 8, 16);
        const float q = (float)(1 << (bits - 1));
        const float invQ = 1.0f / q;
        const float srEff = kCrunchBaseRate / std::pow(2.0f, (float)pitch / 12.0f);
        const float crunchStep = clampf(srEff / (float)sr_, 0.0f, 1.0f);
        updateCrunchFilters(filters, pitch, srEff);
        crunchAmt_.setTarget(clampf(get(kCrunch) * 0.01f * wc, 0.0f, 1.0f), n);
        const bool crunchOn = crunchAmt_.value() > 0.0f || crunchAmt_.target() > 0.0f;

        // 4 print-through
        const float printAmt = clampf(get(kPrint) * 0.01f * wc, 0.0f, 1.0f);
        int S = (int)(kPrintBeats[mpc::clamp(getInt(kPrintTime), 0, 2)] * spb);
        if (S > L_ / 2) S = L_ / 2;
        if (S < 1) S = 1;
        const float preG = 0.10f * printAmt, postG = 0.07f * printAmt;   // about -20 / -23 dB at full
        printLp_[0].setHz(3000.0f, sr_); printLp_[1].a = printLp_[0].a;
        printHp_[0].setHz(250.0f, sr_);  printHp_[1].a = printHp_[0].a;

        // 5 azimuth (the error wanders as a slow random walk)
        const float azAmt = clampf(get(kAzimuth) * 0.01f * wc, 0.0f, 1.0f);
        const float azDrift = get(kAzDrift) * 0.01f;
        azCount_ -= n;
        if (azCount_ <= 0) { azCount_ = (int)(2.0 * sr_); azRT_ = 2.0f * azRng_.next() - 1.0f; }
        azR_ += (azRT_ - azR_) * clampf((float)n / (float)(1.5 * sr_), 0.0f, 1.0f);
        const float azEff = azAmt * (1.0f - 0.5f * azDrift + 0.5f * azDrift * std::fabs(azR_));
        tauRamp_.setTarget((float)(0.00035 * sr_) * azAmt * ((1.0f - azDrift) + azDrift * azR_), n);
        const float sideG = 1.0f - 0.7f * azEff;
        const float azDull = azEff * 4.0f < 1.0f ? azEff * 4.0f : 1.0f;   // exactly clean at 0
        azLp_[0].setHz(18000.0f - 12000.0f * azEff, sr_); azLp_[1].a = azLp_[0].a;

        // 6 transport: wow, flutter, smear (delays in samples; speed deviation a at f Hz = a / (2 pi f) s)
        const float wowAmt = clampf(get(kWow) * 0.01f * wc, 0.0f, 2.0f);
        const float flutAmt = clampf(get(kFlutter) * 0.01f * wc, 0.0f, 2.0f);
        wowRateZ_ += (wowRng_.next() * 2.0f - 1.0f - wowRateZ_) * 0.002f * (float)n / 128.0f;
        const float wowHz = 0.55f * (1.0f + 0.25f * clampf(wowRateZ_ * 4.0f, -1.0f, 1.0f));
        const float wowA  = (float)sr_ * 0.005f * wowAmt / (kTwoPi * wowHz);
        const float flutA = (float)sr_ * 0.0012f * flutAmt / (kTwoPi * 6.3f);
        const float flut2A = (float)sr_ * 0.0005f * flutAmt / (kTwoPi * 10.7f);
        const float smearA = (float)sr_ * 0.001f * std::sqrt(g) / (kTwoPi * 1.2f);
        const float wowInc = wowHz / (float)sr_, flutInc = 6.3f / (float)sr_, flut2Inc = 10.7f / (float)sr_;
        const float wanderA = 1.0f - std::exp(-kTwoPi * 1.2f / (float)sr_);
        const int wanderEvery = (int)(sr_ / 8.0);

        // 7 accidents
        chanceEff_ = clampf(get(kChance) * 0.01f * wc, 0.0f, 1.0f);
        sevEff_ = clampf(get(kSeverity) * 0.01f * wc, 0.0f, 1.0f);
        const int dens = mpc::clamp(getInt(kDensity), 0, 3);
        const double gridBeats = dens == 0 ? barBeats : dens == 1 ? barBeats * 0.5 : dens == 2 ? 1.0 : 0.5;
        if (gridBeats != lastGridBeats_) { lastGridBeats_ = gridBeats; nextGridBeat_ = std::ceil((beat_ - 1e-6) / gridBeats) * gridBeats; }
        const int patMode = mpc::clamp(getInt(kPattern), 0, 3);
        const int patN = patMode == 0 ? 0 : 1 << (patMode - 1);
        numAllowed_ = 0;
        if (getBool(kDropout)) allowed_[numAllowed_++] = kAccDropout;
        if (getBool(kDrag))    allowed_[numAllowed_++] = kAccDrag;
        if (getBool(kSlip))    allowed_[numAllowed_++] = kAccSlip;
        if (getBool(kCrinkle)) allowed_[numAllowed_++] = kAccCrinkle;
        if (getBool(kChanCut)) allowed_[numAllowed_++] = kAccCutL;
        contactLp_[0].setHz(1500.0f, sr_); contactLp_[1].a = contactLp_[0].a;

        // 8 tape compression and breathing hiss
        const float breathe = clampf(get(kBreathe) * 0.01f * wc, 0.0f, 1.0f);
        const float makeup = 1.0f + 0.25f * breathe;

        mix_.setTarget(get(kMix) * 0.01f, n);
        out_.setTarget(dbToGain(get(kOutput)), n);

        const int nIn = ctx.inputs != nullptr ? ctx.numInputs : 0;
        const bool stereoOut = ctx.numOutputs >= 2;
        const float invScale = 1.0f / kLoopScale;
        const double spbD = spb;

        for (int i = 0; i < n; ++i)
        {
            const float inL = (nIn > 0 && ctx.inputs[0] != nullptr) ? ctx.inputs[0][i] : 0.0f;
            const float inR = (nIn > 1 && ctx.inputs[1] != nullptr) ? ctx.inputs[1][i] : inL;

            // ---- clock events: bar lines (Pattern) and the accident grid ----
            if (beat_ >= nextBarBeat_ - 1e-9)
            {
                const long long bar = (long long)std::floor(nextBarBeat_ / barBeats + 0.5);
                nextBarBeat_ += barBeats;
                onBar(bar, patN);
            }
            if (beat_ >= nextGridBeat_ - 1e-9)
            {
                nextGridBeat_ += gridBeats;
                rollAccident(i, spbD);
            }

            float x[2] = {inL, inR};

            // ---- 1 crunch ----
            if (crunchOn)
            {
                const float amt = crunchAmt_.next();
                crunchPhase_ += crunchStep;
                const bool tick = crunchPhase_ >= 1.0f;
                if (tick) crunchPhase_ -= 1.0f;
                for (int ch = 0; ch < 2; ++ch)
                {
                    CrunchChannel& cc = crunch_[ch];
                    float a = x[ch];
                    if (filters != 2) { a = cc.in1.process(a); if (filters == 0) a = cc.in2.process(a); }
                    if (tick) cc.held = std::floor(clampf(a, -1.0f, 1.0f) * q + 0.5f) * invQ;
                    float b = cc.held;
                    if (filters != 2) { b = cc.out1.process(b); if (filters == 0) b = cc.out2.process(b); }
                    x[ch] += amt * (b - x[ch]);
                }
            }

            // ---- 2 generation copy ----
            for (int ch = 0; ch < 2; ++ch)
            {
                float v = x[ch];
                v += lpMix * (genHp_[ch].hp(v) - v);
                v += genHissAmp * hissPre_[ch].process();
                float lp, bp, hp;
                genSvf_[ch].processAll(v, lp, bp, hp);
                v += lpMix * (lp - v);
                x[ch] = v + lpMix * (softClip(v * drive) * invDrive - v);   // saturation, faded in over the first dub
            }

            // ---- 3 tape loop ----
            memGate_ += ((eraseLeft_ > 0 ? 0.0f : 1.0f) - memGate_) * gateA_;
            const float mEff = memory * memGate_;
            const float passHiss = hissAmp * 0.5f * mEff;
            for (int ch = 0; ch < 2; ++ch)
            {
                int16_t* lp = loop_[ch].data();
                const float old = (float)lp[idx_] * invScale;
                const float ghost = (old + passMix * (passLp_[ch].lp(old) - old)) * passGain
                                  + passHiss * passNoise_.nextBipolar();
                float wr = x[ch] * (1.0f - mEff) + ghost * mEff;
                wr = clampf(wr, -1.999f, 1.999f);                    // the int16 loop holds +-2.0
                lp[idx_] = (int16_t)std::lrintf(wr * kLoopScale * 0.999f);
                x[ch] = wr;
            }

            // ---- 4 print-through (from the loop: S after, and S before via the previous pass) ----
            preGate_ += ((eraseLeft_ > 0 ? 0.0f : 1.0f) - preGate_) * gateA_;
            postGate_ += ((sinceErase_ >= S ? 1.0f : 0.0f) - postGate_) * gateA_;
            if (printAmt > 0.0f)
            {
                int pre = idx_ + S; if (pre >= L_) pre -= L_;
                int post = idx_ - S; if (post < 0) post += L_;
                const float gPre = preG * preGate_, gPost = postG * postGate_;
                for (int ch = 0; ch < 2; ++ch)
                {
                    const int16_t* lp = loop_[ch].data();
                    float gh = ((float)lp[pre] * gPre + (float)lp[post] * gPost) * invScale;
                    gh = printHp_[ch].hp(printLp_[ch].lp(gh));
                    x[ch] += gh;
                }
            }

            if (++idx_ >= L_) { idx_ = 0; onPass(i + 1); }   // the next sample starts a new pass
            if (eraseLeft_ > 0) --eraseLeft_;
            if (sinceErase_ < (1 << 30)) ++sinceErase_;

            // ---- 5 azimuth ----
            {
                const float tau = tauRamp_.next();
                az_[0][azW_] = x[0]; az_[1][azW_] = x[1];
                const int lagCh = tau >= 0.0f ? 1 : 0;
                const float d = std::fabs(tau);
                const int di = (int)d; const float fr2 = d - (float)di;
                const int i0 = (azW_ - di) & (kAzRing - 1), i1 = (i0 - 1) & (kAzRing - 1);
                x[lagCh] = az_[lagCh][i0] + (az_[lagCh][i1] - az_[lagCh][i0]) * fr2;
                azW_ = (azW_ + 1) & (kAzRing - 1);
                const float mid = 0.5f * (x[0] + x[1]);
                const float side = 0.5f * (x[0] - x[1]) * sideG;
                const float l = mid + side, r = mid - side;
                x[0] = l + azDull * (azLp_[0].lp(l) - l);
                x[1] = r + azDull * (azLp_[1].lp(r) - r);
            }

            // ---- 7a accidents: envelopes ----
            float gain[2] = {1.0f, 1.0f};
            float contact = 0.0f, accDelay = 0.0f;
            if (acc_.type != kAccNone) runAccident(gain, contact, accDelay);
            accDelayZ_ += (accDelay - accDelayZ_) * accSmoothA_;

            // ---- 6 transport: one modulated delay ----
            wowPh_ += wowInc; if (wowPh_ >= 1.0f) wowPh_ -= 1.0f;
            flutPh_ += flutInc; if (flutPh_ >= 1.0f) flutPh_ -= 1.0f;
            flut2Ph_ += flut2Inc; if (flut2Ph_ >= 1.0f) flut2Ph_ -= 1.0f;
            if (--wanderCount_ <= 0) { wanderCount_ = wanderEvery; wanderT_ = 2.0f * wanderRng_.next() - 1.0f; }
            wanderZ_ += (wanderT_ - wanderZ_) * wanderA;
            float dly = wowA * (1.0f + fastSin01(wowPh_)) + flutA * (1.0f + fastSin01(flutPh_))
                      + flut2A * (1.0f + fastSin01(flut2Ph_)) + smearA * (1.0f + wanderZ_) + accDelayZ_;
            const float maxD = (float)(modLen_ - 4);
            if (dly > maxD) dly = maxD;
            if (dly < 0.0f) dly = 0.0f;
            {
                mod_[0][(size_t)modW_] = x[0]; mod_[1][(size_t)modW_] = x[1];
                const int di = (int)dly; const float fr2 = dly - (float)di;
                const int i0 = (modW_ - di) & modMask_, i1 = (i0 - 1) & modMask_;
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* mb = mod_[ch].data();
                    x[ch] = mb[i0] + (mb[i1] - mb[i0]) * fr2;
                }
                modW_ = (modW_ + 1) & modMask_;
            }

            // ---- 7b dropouts / cuts: level and lost contact (top end) ----
            for (int ch = 0; ch < 2; ++ch)
            {
                const float lpv = contactLp_[ch].lp(x[ch]);
                x[ch] = (x[ch] + contact * (lpv - x[ch])) * gain[ch];
            }

            // ---- 8 tape compression + breathing hiss ----
            {
                const float a = std::fmax(std::fabs(x[0]), std::fabs(x[1]));
                env_ = a > env_ ? env_ + envAtt_ * (a - env_) : env_ * envRel_;
                const float gr = makeup / (1.0f + 0.8f * breathe * env_);
                const float envN = env_ / (env_ + 0.15f);
                const float hg = hissAmp * ((1.0f - breathe) + breathe * (0.2f + 2.0f * envN));
                x[0] = x[0] * gr + hg * hissPost_[0].process();
                x[1] = x[1] * gr + hg * hissPost_[1].process();
            }

            const float m = mix_.next();
            const float og = out_.next();
            ctx.outputs[0][i] = (inL + m * (x[0] - inL)) * og;
            if (stereoOut) ctx.outputs[1][i] = (inR + m * (x[1] - inR)) * og;

            beat_ += bps;
        }

        updateMeters(n);
    }

private:
    struct Acc
    {
        int   type = kAccNone;
        int   t = 0, len = 1, edge = 1;
        float depth = 0.0f;          // dropout / cut depth, or the peak delay (samples) for drag and slip
        float rate = 0.0f, ph = 0.0f, wobble = 0.0f;   // crinkle
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Low Tide";
        info.vendor       = "Groundswell";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1100;           // 1.1.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 0;              // Memory can replay the loop indefinitely: unknown tail
        return info;
    }

    static constexpr char kUid[5] = "Gy5X";

    static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }

    // c in 0..1 (1 = most worn) at song position `beat`.
    static float tideShape(int shape, double beat, double cycleBeats, double barBeats)
    {
        if (cycleBeats <= 0.0) return 1.0f;
        double ph = std::fmod(beat, cycleBeats) / cycleBeats;
        if (ph < 0.0) ph += 1.0;
        const float p = (float)ph;
        switch (shape)
        {
            case 1:  // Erode: wears out steadily, then washes back over the last eighth
                return p < 0.875f ? p / 0.875f : 1.0f - smooth01((p - 0.875f) / 0.125f);
            case 2:  // Ebb: up and down
                return p < 0.5f ? 2.0f * p : 2.0f - 2.0f * p;
            case 3:  // Steps: one step per bar (per beat for a 1-bar cycle)
            {
                int steps = (int)std::lround(cycleBeats / barBeats);
                if (steps < 2) steps = (int)std::lround(barBeats);
                if (steps < 2) return 1.0f;
                int k = (int)(p * (float)steps);
                if (k >= steps) k = steps - 1;
                return (float)k / (float)(steps - 1);
            }
            case 4:  // Drift: a random level per quarter cycle, eased between (the same for the same bar)
            {
                const double seg = cycleBeats * 0.25;
                const double s = beat / seg;
                const double k = std::floor(s);
                const float f = smooth01((float)(s - k));
                const float a = hash01((uint32_t)(int64_t)k * 2654435761u + 77u);
                const float b = hash01((uint32_t)((int64_t)k + 1) * 2654435761u + 77u);
                return a + (b - a) * f;
            }
            default: // Swell: a sine, clean at the start of the cycle
                return 0.5f - 0.5f * fastSin01(p + 0.25f);
        }
    }

    void resync(double barBeats, double spb)
    {
        nextBarBeat_ = std::ceil((beat_ - 1e-6) / barBeats) * barBeats;
        if (lastGridBeats_ > 0.0) nextGridBeat_ = std::ceil((beat_ - 1e-6) / lastGridBeats_) * lastGridBeats_;
        // the tape position follows the song position (passes land on bar lines)
        double pos = std::fmod(beat_ * spb, (double)L_);
        if (pos < 0.0) pos += (double)L_;
        int want = (int)pos;
        if (want >= L_) want = 0;
        int diff = std::abs(want - idx_);
        if (diff > L_ / 2) diff = L_ - diff;
        if (diff > 64) { idx_ = want; beginErase(); }
        else idx_ = want;
    }

    void beginErase()
    {
        eraseLeft_ = L_ > 0 ? L_ : 1;
        sinceErase_ = 0;
    }

    void startFresh()
    {
        genTarget_ = 0;
        dir_ = 1;
        freshGlide_ = true;
        freshRate_ = (genPos_ > 1.0f ? genPos_ : 1.0f) / (float)(0.03 * sr_);
        beginErase();
    }

    // One full pass of the loop: the tape has been dubbed once more.
    void onPass(int offset)
    {
        if (cap_ <= 0) genTarget_ = 0;
        else if (atCap_ == 0) { if (genTarget_ < cap_) ++genTarget_; }
        else if (atCap_ == 1) { if (genTarget_ >= cap_) startFresh(); else ++genTarget_; }
        else
        {
            genTarget_ += dir_;
            if (genTarget_ >= cap_) { genTarget_ = cap_; dir_ = -1; }
            if (genTarget_ <= 0) { genTarget_ = 0; dir_ = 1; }
        }
        LOWTIDE_PASS_HOOK(offset, genTarget_);
        (void)offset;
    }

    void onBar(long long bar, int patN)
    {
        if (patN == 0) { lockActive_ = false; return; }
        if (!lockActive_ || patN != lockN_)
        {
            lockActive_ = true; lockN_ = patN; lockBar_ = bar; lockSeed_ = rng_.s;
            return;
        }
        long long k = (bar - lockBar_) % patN;
        if (k < 0) k += patN;
        if (k == 0) rng_.s = lockSeed_;
    }

    // Grid point: always five draws, so a locked pattern replays exactly.
    void rollAccident(int offset, double spb)
    {
        const float rC = rng_.next(), rT = rng_.next(), rL = rng_.next(), rS = rng_.next(), rD = rng_.next();
        if (acc_.type != kAccNone || numAllowed_ == 0 || rC >= chanceEff_) return;
        int type = allowed_[mpc::clamp((int)(rT * (float)numAllowed_), 0, numAllowed_ - 1)];
        if (type == kAccCutL && rS >= 0.5f) type = kAccCutR;
        const float sev = sevEff_;
        const double sr = sr_;
        auto beats = [&](double b, double lo, double hi) { return (int)mpc::clamp(b * spb, lo * sr, hi * sr); };
        Acc a;
        a.type = type;
        a.t = 0;
        switch (type)
        {
            case kAccDropout:
            {
                const double lens[3] = {0.0625, 0.125, 0.25};
                a.len = beats(lens[mpc::clamp((int)(rL * 3.0f), 0, 2)], 0.02, 0.4);
                a.edge = (int)(0.004 * sr);
                a.depth = 0.5f + 0.47f * sev * (0.7f + 0.3f * rD);
                break;
            }
            case kAccDrag:
            {
                a.len = beats(1.0 + rL, 0.15, 1.5);
                const float drop = 0.02f + 0.06f * sev * (0.6f + 0.4f * rD);   // speed lost at the bottom
                a.depth = drop * 0.55f * (float)a.len / 1.5f;                     // smoothstep peak slope is 1.5x
                break;
            }
            case kAccSlip:
            {
                a.len = beats(0.125, 0.04, 0.15);
                const float s = 0.01f + 0.03f * sev * (0.6f + 0.4f * rD);
                a.depth = s * 0.4f * (float)a.len;
                break;
            }
            case kAccCrinkle:
            {
                a.len = beats(0.25 + 0.25 * rL, 0.06, 0.5);
                a.edge = (int)(0.006 * sr);
                a.rate = (18.0f + 10.0f * rD) / (float)sr;
                const float s = 0.005f + 0.015f * sev;
                a.depth = (float)sr * s / (kTwoPi * 22.0f);
                a.wobble = 0.2f + 0.3f * sev;
                break;
            }
            default: // channel cut: half a beat, exactly
            {
                a.len = beats(0.5, 0.05, 1.0);
                a.edge = (int)(0.005 * sr);
                a.depth = 0.7f + 0.3f * sev;
                break;
            }
        }
        if (a.edge < 1) a.edge = 1;
        if (a.edge * 2 > a.len) a.edge = a.len / 2 > 0 ? a.len / 2 : 1;
        acc_ = a;
        accMeter_ = type;
        accMeterHold_ = (int)(0.25 * sr);
        LOWTIDE_ACCIDENT_HOOK(offset, type);
        (void)offset;
    }

    void runAccident(float* gain, float& contact, float& accDelay)
    {
        Acc& a = acc_;
        if (a.t >= a.len) { a.type = kAccNone; return; }
        const float u = (float)a.t / (float)a.len;
        const float env = smooth01((float)a.t / (float)a.edge) * smooth01((float)(a.len - a.t) / (float)a.edge);
        switch (a.type)
        {
            case kAccDropout:
                gain[0] = gain[1] = 1.0f - a.depth * env;
                contact = env;
                break;
            case kAccDrag:   // delay rises (pitch sags), then returns (pitch catches up)
                accDelay = a.depth * (u < 0.55f ? smooth01(u / 0.55f) : 1.0f - smooth01((u - 0.55f) / 0.45f));
                break;
            case kAccSlip:   // a constant-pitch dip, a beat of normal, a gentler climb back
                accDelay = a.depth * (u < 0.4f ? u / 0.4f : (u < 0.5f ? 1.0f : 1.0f - (u - 0.5f) / 0.5f));
                break;
            case kAccCrinkle:
            {
                a.ph += a.rate; if (a.ph >= 1.0f) a.ph -= 1.0f;
                const float s = fastSin01(a.ph);
                accDelay = a.depth * env * (1.0f + s);
                gain[0] = gain[1] = 1.0f - a.wobble * env * (0.5f + 0.5f * fastSin01(a.ph * 2.0f + 0.13f));
                contact = 0.6f * env;
                break;
            }
            case kAccCutL: gain[0] = 1.0f - a.depth * env; break;
            case kAccCutR: gain[1] = 1.0f - a.depth * env; break;
            default: break;
        }
        ++a.t;
    }

    void updateCrunchFilters(int filters, int pitch, float srEff)
    {
        const int key = filters * 100 + pitch;
        if (key == crunchKey_) return;
        crunchKey_ = key;
        using mpc::dsp::BiquadCoeffs;
        const float nyq = 0.45f * (float)sr_;
        if (filters == 0)        // Clean: 4-pole Butterworth in and out, tracking the rate
        {
            const float fc = clampf(0.45f * srEff, 200.0f, nyq);
            const BiquadCoeffs a = BiquadCoeffs::lowpass(sr_, fc, 0.5412f), b = BiquadCoeffs::lowpass(sr_, fc, 1.3066f);
            for (CrunchChannel& c : crunch_) { c.in1.setCoeffs(a); c.in2.setCoeffs(b); c.out1.setCoeffs(a); c.out2.setCoeffs(b); }
        }
        else if (filters == 1)   // Dirty: loose input filter (some aliasing), fixed output filter (images pass)
        {
            const BiquadCoeffs a = BiquadCoeffs::lowpass(sr_, clampf(0.7f * srEff, 200.0f, nyq), 0.7071f);
            const BiquadCoeffs b = BiquadCoeffs::lowpass(sr_, clampf(0.45f * kCrunchBaseRate, 200.0f, nyq), 0.7071f);
            for (CrunchChannel& c : crunch_) { c.in1.setCoeffs(a); c.out1.setCoeffs(b); }
        }
    }

    // Meters: once every 100 ms, and only when they moved.
    void updateMeters(int n)
    {
        if (accMeterHold_ > 0) accMeterHold_ -= n;
        meterCountdown_ -= n;
        if (meterCountdown_ > 0) return;
        meterCountdown_ = meterInterval_;
        sendMeter(0, kWearNow, std::round(wearNow_ * 100.0f) * 0.01f);
        sendMeter(1, kGenNow, (float)genTarget_ / 16.0f);
        const int shown = (acc_.type != kAccNone || accMeterHold_ > 0) ? accMeter_ : kAccNone;
        sendMeter(2, kAccidentNow, (float)shown / 6.0f);
    }

    void sendMeter(int slot, int param, float norm)
    {
        if (norm == lastMeter_[slot]) return;
        lastMeter_[slot] = norm;
        setNormalised(param, norm);
        notifyHostParameterChanged(param);
    }

    // buffers (allocated in prepare)
    std::vector<int16_t> loop_[2];
    std::vector<float>   mod_[2];
    float  az_[2][kAzRing] = {};
    int    loopCap_ = 0, L_ = 0, idx_ = 0;
    int    modLen_ = 1, modMask_ = 0, modW_ = 0, azW_ = 0;
    double sr_ = 44100.0;

    // clock
    double beat_ = 0.0, expPpq_ = 0.0, nextBarBeat_ = 0.0, nextGridBeat_ = 0.0, lastGridBeats_ = -1.0;
    bool   wasPlaying_ = false, needResync_ = true;

    // generations and the loop
    int    genTarget_ = 0, dir_ = 1, cap_ = 6, atCap_ = 0;
    float  genPos_ = 0.0f, freshRate_ = 0.0f;
    bool   freshGlide_ = false, lastFresh_ = false;
    int    eraseLeft_ = 0, sinceErase_ = 0;
    float  memGate_ = 0.0f, preGate_ = 0.0f, postGate_ = 0.0f, gateA_ = 0.01f;
    float  wearNow_ = 0.0f;

    // stages
    lowtide::CrunchChannel crunch_[2];
    float  crunchPhase_ = 0.0f;
    int    crunchKey_ = -1;
    mpc::dsp::SVF genSvf_[2];
    lowtide::OnePole genHp_[2], passLp_[2], printLp_[2], printHp_[2], azLp_[2], contactLp_[2];
    mpc::dsp::PinkNoise hissPre_[2], hissPost_[2];
    mpc::dsp::Xorshift32 passNoise_;
    float  env_ = 0.0f, envAtt_ = 0.5f, envRel_ = 0.999f;
    float  wowPh_ = 0.0f, flutPh_ = 0.0f, flut2Ph_ = 0.0f, wanderZ_ = 0.0f, wanderT_ = 0.0f, wowRateZ_ = 0.0f;
    int    wanderCount_ = 0, azCount_ = 0;
    float  azR_ = 0.0f, azRT_ = 0.0f;
    lowtide::Rng wanderRng_, wowRng_, azRng_;

    // accidents
    Acc    acc_;
    lowtide::Rng rng_;
    float  chanceEff_ = 0.0f, sevEff_ = 0.0f, accDelayZ_ = 0.0f, accSmoothA_ = 0.01f;
    int    allowed_[5] = {}, numAllowed_ = 0, accMeter_ = 0, accMeterHold_ = 0;
    bool   lockActive_ = false;
    int    lockN_ = 0;
    long long lockBar_ = 0;
    uint32_t lockSeed_ = 0;

    mpc::dsp::LinearRamp mix_, out_, crunchAmt_, tauRamp_;

    int   meterCountdown_ = 0, meterInterval_ = 4410;
    float lastMeter_[3] = {-1.0f, -1.0f, -1.0f};
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(LowTide)
