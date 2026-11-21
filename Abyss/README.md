# Abyss

**Hold one note and let it sink.** A slowly evolving drone and pad instrument for Akai MPC standalone
hardware, by Groundswell.

Abyss is for ambient beds, intros and long held chords. A note starts as an almost pure sine. Over the next
seconds to a minute (**Evolve**) it opens into a deep pad: more harmonics come in, a moving formant-like peak
(**Bloom**) drifts through them, an interval layer (a fifth, or any of eight intervals) and a sub fill it out,
high partials glint like shimmer, a breath of air rises behind it, and a long reverb holds it all. Once it has
opened it keeps moving. Every partial wanders slowly in level and pitch (**Drift**), a slow cycle (**Motion**),
free-running or locked to the song, sweeps the bloom, the brightness and the stereo image, and with **Orbit** the
two halves of every note circle each other in the stereo field. No two notes sound quite the same.

## New in 1.1

- **Orbit**: each note's two stereo halves (the odd and the even partials) circle each other, swapping sides and
  front and back. The half behind is quieter and darker, with a few cents of Doppler as they pass. Every note of a
  chord circles at its own offset. **Orbit Speed** is set in orbits per motion cycle, so it follows Sync too.
- **Stretch** pulls the partials out of tune with each other: up for bells, glass and metal, down for a blurred,
  clustered tone. **Hollow** takes out the even harmonics, for a woodwind or clarinet colour.
- **Interval**: the old Fifth layer can play a fifth, fourth, octave, major or minor third, minor or major
  seventh, or a ninth, so one note implies a chord. Its level knob is now called **Int Level**.
- **Motion Shape**: Sine, Drift (wandering at random), Swell (a long rise and a quicker fall) or Tide (two
  cycles, one inside the other).
- **Evolve Dir**: Rise (as before) or **Sink**, where notes start bright and slowly darken into the abyss.
- **Eight new programs** (16 in all) and a new **TONE** page on the panel.
- **Bug fix: the Sub and Fifth knobs did nothing in 1.0.** The sub and the fifth were always fully on, mixed into
  the body of the sound. They now follow their knobs. The 8 original programs were re-voiced (Sub and Int Level)
  to stay as close as possible to how they sounded in 1.0. **Projects saved with 1.0 keep their own Sub and Fifth
  settings, so they will sound a little different: lighter, without the fifth unless you turn Int Level up.**
  Everything else in a 1.0 project loads unchanged, and the new controls start at settings that add nothing.

## Install

1. Copy the `Groundswell - VST - Abyss` folder (in `build/` after building, or from the release zip) into a
   folder named `VST2 Plugins` on a drive the MPC can see. The internal drive is the safest choice.
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC and load Abyss from **External Plugins** onto a plugin program.

## How it works

Each of the 6 voices is 24 sine partials:

| Layer | Partials | What it does |
|---|---|---|
| Main | harmonics 1–10 | The body. **Bright** sets how many are open once the note has evolved; **Bloom** lifts a band of them that wanders up and down. |
| Twins | harmonics 1–3, detuned | A second copy of the lowest harmonics, **Detune** cents apart, for slow chorus-like beating. |
| Sub | one octave down | **Sub** level. Fades out below about 15–35 Hz, where it would only use up headroom. |
| Interval | the interval, ×2, ×3 | **Int Level**; **Interval** picks which (a fifth by default). Comes in as the note evolves (from about 15 % open). |
| Shimmer | harmonics 12–28 | Random glints that fade in and out over 0.3–1.5 s. Comes in late (from about 35 % open). |

Each voice's partials are split into two halves for stereo: the centre (fundamental and sub), A (odd partials)
and B (even partials). **Width** pans A and B apart; **Orbit** sends them round each other.

The main, twin and interval partials are normalised to a steady loudness, so opening the brightness or the bloom
changes the colour of the sound, not its level. On top of the voices:

- **Air**: two band-passed noises (one per side) whose level follows how many voices are sounding and how far
  they have evolved.
- **Space**: an 8-line feedback-delay-network reverb, high-passed at 150 Hz at its input so the sub stays
  clean. **Size** is the decay time; **Space Tone** darkens or brightens the tail. The input is scaled so a held
  pad sounds about as loud at any size.
- **Output**: Volume, then a soft clip with a −0.3 dBFS ceiling. A six-note chord at full velocity peaks about
  3 dB below that, so the clip only touches the loudest moments.

### Evolve and motion

- **Evolve** is the time a held note takes to open fully. The curve starts slowly, so even at short settings the
  first moment is soft. Each voice evolves on its own, so a note added later starts dark under ones that are
  already open. With **Evolve Dir** on Sink it runs the other way: notes start open and darken over the same
  time (the shimmer fades out as they sink; the interval layer still comes in).
- **Motion** is a slow cycle, **Cycle** seconds long (2–120 s), in one of four shapes (**Motion Shape**). With **Sync** on, it lasts **Cycle Div**
  (1–32 bars) at the project tempo, and is locked to the song position while the MPC plays. Each voice reads the
  cycle at its own offset, so a chord moves in counterpoint, not in lockstep.
- **Drift** sets how far each partial, and each voice's brightness and pitch, wander at random (new targets every
  2–10 s). At 100 % partials wander about ±7 cents.
- The **mod wheel** (CC 1) deepens the bloom. The **sustain pedal** (CC 64) holds notes. **Pitch bend** is
  ±2 semitones.

## Parameters

Parameter IDs and their order are permanent: new parameters are only ever added at the end (22–28 arrived in
1.1).

| # | Name | Range | Default | What it does |
|---|---|---|---|---|
| 0 | Volume | −40…+6 dB | −3 dB | Output level. |
| 1 | Evolve | 1–60 s | 15 s | Time for a held note to open from a sine into the full pad. |
| 2 | Bright | 0–100 % | 50 % | How many harmonics are open once evolved (about 1.5 to 23). |
| 3 | Bloom | 0–100 % | 50 % | Depth of the wandering peak in the spectrum: vowel-like, choir-like movement. |
| 4 | Drift | 0–100 % | 40 % | Random wandering of partial levels and pitches, and of each voice's brightness. |
| 5 | Detune | 0–30 ct | 7 ct | Detune of the twin partials (0 turns them off). |
| 6 | Sub | 0–100 % | 70 % | Sine one octave down. |
| 7 | Int Level | 0–100 % | 50 % | The interval layer, fading in as the note evolves ("Fifth" in 1.0). |
| 8 | Shimmer | 0–100 % | 25 % | High glinting partials, fading in late. |
| 9 | Air | 0–100 % | 15 % | Breathy band-passed noise behind the voices. |
| 10 | Motion | 0–100 % | 50 % | How far the slow cycle moves the bloom, brightness and stereo sway. |
| 11 | Sync | Free / Sync | Free | Cycle in seconds, or in bars at the project tempo. |
| 12 | Cycle | 2–120 s | 20 s | Length of the motion cycle when Sync is Free. |
| 13 | Cycle Div | 1–32 bars | 4 bars | Length of the motion cycle when Sync is on. |
| 14 | Attack | 10 ms–20 s | 2.5 s | Fade-in. |
| 15 | Release | 50 ms–30 s | 6 s | Fade-out to −60 dB after the key (or pedal) is let go. |
| 16 | Width | 0–100 % | 70 % | Stereo spread of the partials. 0 is mono. |
| 17 | Space | 0–100 % | 40 % | Reverb amount (dry fades a little as it goes up). |
| 18 | Size | 1–20 s | 8 s | Reverb decay time. |
| 19 | Space Tone | 1–16 kHz | 5 kHz | Damping of the reverb tail. |
| 20 | Velocity | 0–100 % | 30 % | How much velocity changes level and brightness. |
| 21 | Octave | −2…+2 | 0 | Transposes everything by octaves. |
| 22 | Orbit | 0–100 % | 0 % | How far each note's two halves circle each other (0 = side by side, as in 1.0). |
| 23 | Orbit Speed | 1/2 … 8x | 2x | Orbits per motion cycle (2x with a 20 s cycle = one orbit every 10 s). |
| 24 | Stretch | −100…+100 % | 0 % | Partials pulled sharp (bells, glass) or squeezed together (blur). At +100 % the 10th partial sits near 22× the note. |
| 25 | Hollow | 0–100 % | 0 % | Removes the even harmonics. |
| 26 | Interval | Fifth … 9th | Fifth | Which interval the interval layer plays: Fifth, Fourth, Octave, Maj 3rd, Min 3rd, Min 7th, Maj 7th, 9th (just intonation). |
| 27 | Motion Shape | Sine / Drift / Swell / Tide | Sine | The shape of the motion cycle. Drift ignores Sync (it wanders). |
| 28 | Evolve Dir | Rise / Sink | Rise | Notes open over Evolve, or start bright and sink into the dark. |

## The panel

A custom skin over the canyon artwork (`art/plate-src.webp`): controls sit in dark glass boxes down both sides
so the light rays and the glowing plume stay in view in the middle. Each row is one bank of four Q-Links.

| Page | Rows (left box \| right box) |
|---|---|
| PLAY | Evolve, Drift \| Bright, Bloom · Sub, Int Level \| Shimmer, Air · Attack, Release \| Space, Size · Volume, Width \| Motion, Orbit |
| TONE | Stretch, Hollow \| Interval, Int Level · Bright, Bloom \| Detune, Drift |
| MOTION | Motion, Sync \| Cycle (free), Cycle Div (synced) · Motion Shape, Evolve Dir \| Orbit, Orbit Speed · Drift, Detune \| Evolve, Bloom |
| SPACE | Space, Size \| Space Tone, Width · Octave, Velocity \| Attack, Release |

Volume is on Q-Link 1 on the PLAY page. On MOTION both cycle lengths are shown; the Sync switch decides which one
applies. The skin binds to parameter numbers, so `art/make_skin.py` must be re-run whenever parameters change,
and **before** `tools/build.py` (the build copies `skins/` as it is).

## Programs

| Program | Character |
|---|---|
| Abyssal Plain | The default: a warm drone that opens over 15 s, with sub, a little shimmer and air. |
| Midnight Zone | Dark and slow: 30 s to open, heavy sub, no shimmer, a long dark reverb. |
| Bioluminescence | Bright and glinting: lots of shimmer and air, a 10 s cycle, wide. |
| Whale Fall | An octave down, with sub and fifth, a cycle locked to 8 bars, a big dark space. |
| Thermal Vent | Restless: heavy drift and detune, deep bloom, a smaller room. |
| Hadal Choir | Strong bloom and a fifth for a vowel-like, choral movement; wide. |
| Pressure Wave | Swells locked to every bar (Motion 100 %), opens in 4 s: for intros and builds. |
| Cold Seep | Glassy and high: an octave up, bright, shimmer and the fifth, a long bright reverb. |
| Glass Sponge | Stretched partials (bells and glass) with an octave layer and shimmer, slowly orbiting; very wide. |
| Siphonophore | The dance: Orbit at 100 %, a major third layer and a deep bloom, every note circling on its own. |
| Trench Wind | Hollow (odd harmonics only) with lots of air and a wandering Drift motion: wind in a canyon. |
| Sinking | Starts bright and sinks into the dark over 40 s, the shimmer fading as it goes. |
| Leviathan | An octave down, squeezed partials, a heavy sub, a slow Tide motion and a half-speed orbit. |
| Lanternfish | Swells locked to 2 bars, a minor seventh layer and a fast orbit: rhythmic, for intros. |
| Kelp Cathedral | A fourth layer, a little hollow, a deep bloom on the Tide shape and a 20 s reverb. |
| Black Smoker | Squeezed, restless and gritty: heavy drift and detune, a wandering motion, lots of air. |

In 1.1 the Sub and Int Level of the first eight were re-voiced to match how they sounded in 1.0 (see "New in
1.1"). The match was measured: rendered side by side, the 1.1 programs sit within about 1.5–2.7 dB per
third-octave band of the 1.0 sound, which is about how much the sound moves on its own in a few seconds.

## Tips

- Hold a single low note for 20–30 s before deciding what a program sounds like: the first seconds are
  deliberately plain.
- For an intro, use **Pressure Wave** or set **Sync** on with a 4- or 8-bar cycle, so the swells land with the
  arrangement.
- Long releases overlap: with six voices, playing a new chord while the last one rings will steal the quietest
  released voices first (with a 6 ms fade, no click).
- Put Q-Links on **Bright**, **Bloom** and **Space** for slow live sweeps; they are smoothed and never click.
- **Two Abysses dancing**: Orbit moves the halves of each note. To move whole sounds round each other, put Abyss
  on two tracks with Groundswell **Bearings** after each, on the same song-locked path half a cycle apart, and
  give the two tracks different programs (Midnight Zone and Cold Seep, say).

## CPU

On a desktop the default program holding a three-note chord takes about **0.2 % of real time**, and the worst
case (six voices, every layer at 100 %, all partials sounding, Orbit on) about **0.4 %**; 1.1 added no measurable
cost. By the ratio seen with the other
Groundswell plugins that should be roughly **2 %** of one MPC core for normal playing and around **4 %** for the
worst case. This is an estimate from the desktop, not yet measured on the hardware.

How it stays cheap: everything that needs `sin`, `exp` or `pow` (envelopes, drift, the spectrum, pitch, panning,
the orbit) runs once every 64 samples (1.45 ms), and the Stretch, Hollow and Interval tables only when those
knobs move. Per sample, each sounding partial is one table look-up with interpolation, one multiply-add and one
ramp, and each voice adds two one-pole filters for the orbit. Silent layers, and partials above 18 kHz, are skipped (high notes therefore lose
their top partials, by design). The reverb is 8 delay lines with one low-pass each and a fast Hadamard mix.

## What is not in this version

- **No reverb freeze or pitch-shifted shimmer reverb.** The shimmer is additive partials, not an octave-up
  feedback loop. A freeze and a true shimmer tail overlap with the Sea Glass idea and may belong there.
- **Partly fixed partial recipe.** Stretch, Hollow and Interval reshape it, but you can't draw your own spectrum,
  and the layers are fixed (main 1–10, twins, sub, interval, shimmer).
- **Motion has fixed destinations** (bloom, brightness, stereo sway). The Drift shape does not lock to the song.
- **Orbit moves the halves of a note, not whole instances**: two Abyss tracks can't see each other (plugins on
  the MPC can't talk to one another). Use Bearings for that (see Tips).
- **No filter or filter envelope**: brightness is done in the partials themselves.
- **6 voices.** Enough for a held chord plus a drone; long releases with fast playing will steal voices.
- **Size changes decay time only**; the room's delay lengths are fixed.
- **Octave**, **Interval** and **Stretch** changes move held partials at once (a pitch jump, not a click).
- MIDI is applied at control-rate resolution (up to 1.45 ms late), which is inaudible on pads.

## Testing

```
python3 plugins/Abyss/art/make_skin.py
python tools/build.py plugins/Abyss
.sdk-tools/host_sim plugins/Abyss/build/native/abyss.dylib test --notes 36,48,55 --seconds 8 --playing
cd plugins/Abyss && mkdir -p tests/build
clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
tests/build/listentest build/abyss-demo.wav
```

`tests/listentest.cpp` renders with random block sizes (32–512) and checks that:

- a single held C2 opens up (spectral centroid about 75 Hz at 2 s → 135 Hz at 24 s, 11 dB per band of change)
  and keeps changing after it has opened (third-octave spectral shape moves 3–6 dB per band every 4 s, at a
  steady loudness);
- a six-note chord at full velocity evolves in **every one of the 16 programs**, stays under 0 dBFS (peaks −1.4
  to −4.1 dBFS) and has no clicks;
- **Stretch** puts the 2nd partial of A2 where the maths says (256 Hz at +60 %, 191 Hz at −60 %), **Hollow**
  drops the 2nd harmonic by 22 dB, **Interval** puts the layer at the right frequency for a major third, minor
  seventh and ninth, **Sub** and **Int Level** really set their layers (the 1.0 bug), **Orbit** carries a partial
  55 dB from side to side three times in 25 s, **Sink** darkens by 13 dB above 1 kHz, and every **Motion Shape**
  keeps the sound moving without clicks;
- a state saved by 1.0 (22 parameters) loads with its values, and the 7 new parameters at their defaults;
- voice stealing, staccato notes, retriggers, the sustain pedal, pitch bend, fast knob moves and program
  changes under a held chord make no clicks and leave no stuck voices;
- the fully wet reverb is within 4 dB of the dry sound;
- the click detector itself catches a 0.01 step (−40 dBFS) in a sparse passage, 0.03 (−30 dBFS) inside the full
  chord, and 0.002 (−54 dBFS) in a release tail.

It also times the worst case and writes two demos: `build/abyss-demo.wav` (52 s: a C2 drone opening on its own, a
Cm9 chord joining at 14 s, the release and reverb tail from 40 s) and `build/abyss-demo-orbit.wav` (46 s:
Siphonophore holding an Ebmaj9 with Orbit at 100 %).

## Project layout

- `plugin.json`: manifest (name, manufacturer, uid). The `uid` must match the C++ `uniqueId`.
- `src/Abyss.cpp`: the whole plugin (parameters, programs, MIDI, voices, air, reverb, output stage).
- `art/make_skin.py`: draws the panel over `art/plate-src.webp` into `skins/` (with previews in
  `build/skin-preview-*.png`). Run it **before** `tools/build.py`, and again whenever parameters change.
- `art/knob_small.webp`, `art/pill_on.webp`, `art/pill_off.webp`: the shared Groundswell knob and button art.
- `skins/`: the rendered panel (TUI.json, Q-Links.json, page backgrounds, knob and button images).
- `ui.json`: a stock-kit fallback layout (violet), unused while `skins/` exists.
- `tests/listentest.cpp`: the offline evolution, feature, click and level checks, and the demos.

## Licence

MIT. Original design and code. The feedback delay network follows the general structure described by Jot and
Chaigne (1991) and Stautner and Puckette (1982); no code was borrowed.
