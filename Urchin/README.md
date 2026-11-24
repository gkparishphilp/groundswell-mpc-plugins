# Urchin

**A spiky fuzz and distortion box with real character.** An MPC insert effect by Groundswell.

Breakwater is the clean amp and Low Tide does lo-fi crunch. Urchin is the nasty one. Six **Spines**, each
its own little circuit rather than another curve: a starved, sputtering fuzz, an octave fuzz, a speaker
with a torn cone, a rectifier, a West Coast wavefolder and a sane overdrive. Then there is **Spike**, one
Q-Link that takes everything from mild to ruined. **Low Keep** and **Blend** keep the low end of bass and
kicks clean, so it works on drums, bass, synths and guitar. **Split** puts a different Spine on each side,
like two amps panned apart, for wide guitars and basses from a mono source.

Insert effect, stereo in / stereo out (or a Mono core to save CPU). MIT licence. Version 1.1 added Split and
Width, and made Starve properly audible.

## How it works

```
in ─┬─────────────────────────────────────────────── dry (delayed 23 samples) ───────────────┐
    │                                                                                       │
    ├─► Low Keep split (4th-order crossover) ── low band, clean ──────────────────────┐     │
    │                                                                                 │     │
    └─► high band ─► [Mono: L+R] ─► coupling high-pass ─► Focus pre-emphasis           │     │
          ─► Drive × Bloom sag ─► 2x up ─► the Spine (every shaper anti-aliased) ─► 2x down     │
          ─► DC block ─► Focus de-emphasis ─► Spine voicing (low-pass, resonance) ─► Tone     │
          ─► auto level ─► Level ─► Gate ─────────────────── + low band = wet ──┐     │
                                                                                ▼     ▼
                                                      Blend: dry × min(1, 2(1−b)) + wet × min(1, 2b) ─► Output
```

- **Anti-aliasing.** Every shaper is a piecewise polynomial with an exact antiderivative, run as a first-order
  ADAA stage (it outputs the average of the curve over the line between two samples, which smooths the hard
  corners that would otherwise fold harmonics back as fizz). It runs at 2x oversampling, with the same
  Kaiser-windowed halfband filter as Breakwater and Pressure. There are no per-sample transcendental
  functions: `exp`, `pow` and `log` only run once per block, for coefficients.
- **Auto level.** Each block, Urchin runs a -12 dBFS sine through the Spine's static curve and sets the gain so
  it comes out at the same level, which takes effect immediately. A slow loudness match then measures what the
  circuit is fed against what it gives back (±9 dB, about 1.5 s) and trims the rest. Drive and Spike add dirt and
  sustain, not volume. The match sits around the shaper only, so the voicing filters, Tone and Level still
  change the level as you would expect.
- **Bloom.** A detector follows how hard you are playing (relative to peaks around -10 dBFS). Playing hard pulls
  the drive down (sag: it compresses). A slower copy widens a crossover dead zone and leans the bias after loud
  notes, so decays sputter and spit.

## The Spines

| Spine | The circuit | Bias does | Its signature (220 Hz at -12 dBFS, Drive 50) |
|---|---|---|---|
| **Gated Fuzz** | A starved transistor fuzz: hard, asymmetric clipping after a crossover dead zone. The bias point leans further after loud notes (the coupling cap charging), which chokes the decay into sputter. | **Starve**: from a healthy battery to a dying one. The dead zone is measured at the input, so it bites at any Drive: it grows from -80 dBFS (healthy) to -18 dBFS (dead), off-centre so the two halves of the wave open at different levels. One side clips early, and up to 9 dB of gain goes. Notes splatter as they decay, then gate. | Healthy: odd harmonics, THD about 38 %. Starve 70: splatty even harmonics (H2 at -12 dB). On the guitar riff, Starve 0 / 35 / 70 / 100 leaves the output silent 6 / 9 / 19 / 28 % of the time. |
| **Octave Fuzz** | A soft first stage into a rectifier, through a coupling cap that drains the rectified DC, into a hard second stage. Drive sits after the rectifier, so the octave stays clear at any setting. | **Octave**: how full-wave the rectifier is (35–100 %). | H2 about 17 dB above H1: high notes ring an octave up. |
| **Torn Speaker** | An amp into a speaker. The cone's excursion is a low-passed copy of the signal. Past its limits (nearer on one side) the cone bottoms out and the torn flap slaps, adding a buzz and a paper rattle gated by how far past it is. Speaker voicing: 85 Hz to 4.5 kHz, with a 1.7 kHz tear resonance. | **Tear**: lower limits, more buzz and rattle, more asymmetry. | Level dependent: 0.7 % THD at -30 dBFS, 18 % at -6 dBFS (80 Hz). H2 at -20 dB when loud. |
| **Rectifier** | Rectify, then fold the rectified wave back on itself at hard corners. Drive sets how many folds (below one fold it is a plain rectifier). | **Half** (0) → **Full** (50): the fundamental fades away and leaves octave grit. **Fold** (50–100) shifts the fold pattern for more spit. | Half-wave: H1 with H2 at -7 dB and no odd harmonics. Full-wave: the fundamental all but vanishes. |
| **Wavefolder** | A West Coast style folder: past full scale the wave folds back smoothly, again and again. | **Symm**: a symmetry offset, which adds even harmonics. | Clippers can't get an overtone above -9.5 dB re H1. The fold already pushes H3 above H1 at Drive 50. |
| **Overdrive** | A smooth, mid-focused soft clipper with a clean path beside it: the sane fallback. | **Asym**: asymmetry (warmth, even harmonics). | THD about 8 %, mostly H3. High harmonics 25 dB under the fuzz. |

The Spines are generic, circuit-inspired characters voiced by ear and by measurement, not models of particular
pedals.

## Spike

One Q-Link that ramps everything from as-set (0) to ruined (100). Spike adds up to 8–18 dB of drive (depending on
the Spine), moves Bias 65 % of the way to its nastiest, moves Bloom 80 % of the way to full, widens the sputter
dead zone and leans the Tone brighter. Because the auto level follows it, the level stays about the same while it
gets nastier (within about 2 dB on the mix in the tests; up to 2.5 dB louder on a sparse guitar part, which gains sustain).

## Parameters

| # | Name | Range | Default | What it does |
|---|---|---|---|---|
| 0 | **Output** | -24 to +12 dB | 0 dB | After everything. |
| 1 | **Spine** | 6 circuits | Gated Fuzz | Which circuit. Changing it dips the sound for about 11 ms, so it never clicks. |
| 2 | **Drive** | 0–100 % | 50 % | How hard the Spine is driven. Each Spine has its own range (for example Gated Fuzz +6 to +48 dB, Rectifier 0 to +22 dB). |
| 3 | **Spike** | 0–100 % | 0 % | The performance macro (above). |
| 4 | **Tone** | Dark 100 to Bright 100 | Flat | A tilt EQ around 800 Hz, ±9 dB at the ends, after the circuit. |
| 5 | **Focus** | Flat / Mids / Lows | Flat | Pre-emphasis: +10 dB at 1 kHz (Mids) or below 250 Hz (Lows) before the circuit, so that band clips first, with 7 dB taken back afterwards. |
| 6 | **Bias** | 0–100 | 25 | The Spine's own character control (the table above). The display names it: Starve, Octave, Tear, Half/Full/Fold, Symm, Asym. |
| 7 | **Bloom** | 0–100 % | 25 % | Sag: playing hard compresses and spits. |
| 8 | **Gate** | Off, -80 to -40 dB, Chop 0–100 | Off | A noise gate on the distortion, listening to the input. Up to 50 % it is a normal gate (threshold -80 to -40 dBFS, release 120 to 40 ms, 25 ms hold). Past 50 % it becomes a chopper: the threshold also follows the recent peak (up to 6 dB under it), the release drops to 3 ms and the hold to zero, so every decay is cut short. |
| 9 | **Blend** | 0–100 % | 100 % | 0 = dry, 50 = full dry plus full fuzz (parallel), 100 = fuzz only. |
| 10 | **Low Keep** | Off, 21–800 Hz | Off | A crossover: below it the sound stays clean and only the band above is distorted. |
| 11 | **Level** | -24 to +12 dB | 0 dB | The distortion's own level, after the auto level. |
| 12 | **Stereo** | Stereo / Mono | Stereo | Mono runs one circuit on L+R, which roughly halves the CPU. The dry path and the clean low band stay stereo. Ignored in Split. |
| 13 | **Spine R** | Same, or one of the 6 | Same | **Split.** Set a Spine here and the input (summed to mono) runs through Spine on the left and Spine R on the right: two circuits panned apart, like a guitar double-tracked through two different amps. Drive, Bias, Bloom and the rest are shared; each side keeps its own auto level. Changing it is click-free. |
| 14 | **Width** | 0–100 % | 100 % | Width of the distortion: 100 = as is (hard left and right in Split), 0 = both sides in the middle. The dry path and the clean low band are not touched. |

Parameter IDs and order are permanent; new parameters will be added at the end.

## The panel

The panel is the user's artwork: the URCHIN title over open green water, kelp and barnacled rocks on the left,
a purple sea urchin on the right. The controls sit in the open water between the kelp and the urchin's spines,
on a soft dark band per row. The water narrows towards the bottom where the spines reach in, so the lower row
is narrower. Knob arcs and the lit pill are in the urchin's magenta. Each row of four is one Q-Link bank. Drive
has the big knob; Spine, Spine R and Focus are rotary selectors (detent dots, the value shows the choice); Mono is a lit
pill.

| Page | Q-Links 1–4 | Q-Links 5–8 |
|---|---|---|
| **URCHIN** | Drive, Spike, Tone, Blend | Spine, Bias, Bloom, Gate |
| **SHELL** | Low Keep, Focus, Level, Output | Spine R, Width, Stereo/Mono, (empty) |

`art/make_skin.py` renders `skins/` from `art/plate-src.webp` (the artwork, 2560×1256, exactly twice the MPC's
1280×628 tab area) and the shared knob and pill images. Run it before `tools/build.py`, which copies `skins/` as it
finds it. It also writes `build/skin-preview-1.png` and `-2.png`. `ui.json` is only the stock-kit fallback.

## Programs

| Program | For | Settings |
|---|---|---|
| **Drum Smasher** | Drum bus | Gated Fuzz, Drive 70, Focus Mids, Bloom 50, Blend 55 (parallel), a little brighter |
| **Bass Grind** | Bass | Rectifier (mostly half-wave), Drive 60, Focus Mids, Low Keep 130 Hz, Mono |
| **Dying Battery** | Guitar, synth leads | Gated Fuzz, Starve 75, Drive 75, Bloom 40, Gate 25, darker, Mono |
| **Octave Spit** | Guitar leads | Octave Fuzz, Octave 80, Drive 65, Focus Mids, Gate 15, Mono |
| **Torn Speaker** | Anything that should sound broken | Torn Speaker, Tear 60, Drive 55, Bloom 30 |
| **Folded Synth** | Synths | Wavefolder, Drive 55, Symm 20 |
| **Gentle Hair** | A bit of grit on anything | Overdrive, Drive 25, Focus Mids, Blend 70 |
| **Total Urchin** | Ruin | Rectifier at Fold 90, Drive 85, Spike 80, Bloom 60, Gate Chop 20, brighter |
| **Twin Amps** | A mono guitar or bass, made wide | Split: Overdrive left, Gated Fuzz right, Drive 55, Focus Mids |
| **Split Spit** | Wide and wrong | Split: Octave Fuzz left, Torn Speaker right, Drive 55, Width 80 |

## CPU

Measured with everything on (Low Keep, Focus, Gate, Blend, Tone, Bloom), on a desktop core: 0.19–0.39 % in Stereo
depending on the Spine, 0.13–0.22 % in Mono. Split costs the same as Stereo (worst 0.36 % with the Octave Fuzz on
one side). The Octave Fuzz is the heaviest (two anti-aliased stages) and the
Wavefolder the lightest. By the usual 15–30x factor that is roughly 3–6 % of one older 32-bit MPC core in Mono
and up to 5–11 % in Stereo for the Octave Fuzz. Newer units are faster. Not yet measured on hardware. If an
older MPC struggles, switch to Mono: that is what it is for.

## Tests

`tests/listentest.cpp` renders synthetic drums, bass, a synth line and a plucked guitar and checks 38 things:
- **Each Spine has its own harmonic signature** (the table above): the octave fuzz's strong second harmonic, the
  full-wave rectifier losing its fundamental, the starved fuzz's even harmonics, the fold pushing an overtone
  past the fundamental, the overdrive being smoother than the fuzz, the torn speaker being level dependent. Every
  pair of profiles is at least 6 dB RMS apart.
- **Starve is audible**: on the guitar, the time spent gated rises from 6 % (Starve 0) to 19 % (70) and 28 % (100), and
  at 70 the output differs from Starve 0 by a residual within about 2 dB of the signal itself.
- **Aliasing at full Drive** on a -6 dBFS sine (power that is not a harmonic, relative to the total, 20 Hz–20 kHz):

  | Spine | 6123 Hz | 1987 Hz | same curve run naively, 6123 Hz |
  |---|---|---|---|
  | Gated Fuzz | -43 dB | -57 dB | -11 dB |
  | Octave Fuzz | -31 dB | -56 dB | -7 dB |
  | Torn Speaker | -45 dB | -58 dB | -11 dB |
  | Rectifier (half / full / fold) | -66 / -50 / -31 dB | -55 / -47 / -53 dB | about 0 dB |
  | Wavefolder | -41 dB | -52 dB | -1 dB |
  | Overdrive | -46 dB | -60 dB | -12 dB |

  The octave fuzz and the full-wave rectifier double the frequency, so a 6 kHz note is a 12 kHz wave inside them.
- **Low end**: on a 55 Hz bass note, Low Keep 150 Hz keeps the fundamental within 0.2 dB on every Spine (the fuzz
  alone loses 5–24 dB of it), and Blend 50 never lets it drop more than 1.1 dB.
- **Gate**: without it the gaps between phrases carry hiss at -62 dBFS; at 35 % they are silent (below -110 dBFS);
  Chop leaves 24 % of the playing itself silent, with about two cuts per second on an 8th-note riff.
- **No runaway**: across Drive 0–100 and Spike 100, on bass, guitar and the mix, every Spine's level moves by 3.4 dB
  or less and stays within 5 dB of the input. Peaks stay under +4 dBFS.
- **Split**: each side is exactly its Spine run on its own (residual under -140 dB); the two sides really differ and sit
  within 2.3 dB of each other (Overdrive and Gated Fuzz on the guitar); Width 0 makes them identical, Width 50 halves
  the side signal; changing Spine R mid-note is click-free; Split costs no more than Stereo.
- All ten programs are clean and within 4 dB of the input.
- No NaN, Inf or denormals at extreme settings with every control jumping; silence in gives silence out; switching
  Spines mid-note is click-free; Mono gives identical sides.
- The CPU.

`host_sim test` passes with no warnings at 44.1, 48 and 96 kHz and with 32-sample blocks while playing.

```
mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework -o tests/build/listentest tests/listentest.cpp
tests/build/listentest build/urchin-demo.wav
```

The second line also renders `build/urchin-demo.wav` (3 min 23 s at 100 BPM, normalised to peak at -1 dBFS). It
plays:

| Time | What |
|---|---|
| 0:00 | Dry: drums, bass, synth and guitar together |
| 0:10 | Gated Fuzz (Dying Battery): drums, bass, synth, guitar (2 bars each) |
| 0:31 | Octave Fuzz (Octave Spit): the same four |
| 0:52 | Torn Speaker: the same four |
| 1:14 | Rectifier (Bass Grind): the same four |
| 1:35 | Wavefolder (Folded Synth): the same four |
| 1:56 | Overdrive (Gentle Hair): the same four |
| 2:17 | Drum Smasher on the drums |
| 2:23 | Starve 0 → 100 on the guitar (Gated Fuzz, Drive 50): the battery dies as you listen |
| 2:33 | Spike 0 → 100 on the mix (Gated Fuzz, Drive 45) |
| 2:43 | Spike 0 → 100 on the guitar (Octave Fuzz, Drive 40) |
| 2:53 | Split, Twin Amps on the guitar: Overdrive left, Gated Fuzz right (use headphones) |
| 3:03 | Split, Split Spit on the guitar: Octave Fuzz left, Torn Speaker right |
| 3:13 | Total Urchin on the mix |

## Not implemented / honest notes

- The Spines are circuit-inspired, not circuit simulations: there is no component-level model of transistors,
  transformers or a real speaker. They are generic characters, not copies of any pedal.
- Anti-aliasing is 2x oversampling with first-order ADAA. It is very good on the clippers but not perfect on the
  folding Spines at full Drive on very high notes (see the table). To help, the Wavefolder and Octave Fuzz hear
  their input through a gentle one-pole low-pass at 2.5 kHz, and the Rectifier through two poles, so a high note
  folds fewer times than a low one. The fold puts the top end back.
- The torn speaker's rattle is shaped noise and a gated buzz, not a mechanical model. Its output is soft-bounded.
  It can still peak a few dB above the input on bass (+3 dBFS from a -3 dBFS bass in the tests).
- The auto level is a static estimate plus a slow match. After a Spine change, or when you start playing after
  silence, it takes a second or two to settle. It deliberately cancels the level change from Drive and Spike;
  use Level or Output to make it louder.
- Bloom, Starve and the sputter respond to absolute input level, like a pedal: a quiet track drives it less, and at high
  Starve a quiet track (peaks under about -18 dBFS) is mostly gated away. Turn the track up, or Starve down.
- Split sums the input to mono first, so a stereo source loses its own stereo image on the distorted path (the dry
  path and the clean low band keep it). On drums or a mix, two different Spines usually just tilt the image; Split
  is meant for mono sources.
- The Gate acts on the distortion only; Blend's dry path and the Low Keep band are never gated. Chop follows the
  playing's own decays; it is not tempo-synced.
- Effects on the MPC get no MIDI, so nothing here follows notes or tempo.
- Reports 23 samples of latency (the oversampler); the dry path is delayed to match.

## Credits

Original design and code, MIT licence. The 2x halfband oversampler is the one from Groundswell's Breakwater
(Kaiser-windowed, written for these plugins); the skin generator is adapted from Pressure's. The artwork is the
user's. No third-party code.
