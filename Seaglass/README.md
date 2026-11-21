# Seaglass

**A lush reverb you can freeze.** A stereo reverb effect for Akai MPC standalone hardware, by Groundswell.

Seaglass is a smooth hall-style reverb with three extra tricks. **Freeze** holds whatever is in the tail as an
endless pad, so you can play over it, then lets it go without a click. **Shimmer** pitch-shifts the tail up an
octave inside the feedback loop, so every pass rises and the tail sparkles (or climbs by a fifth or two octaves,
or sinks an octave). **Swell** keeps the tail down while you play and lets it bloom back in the gaps, the way
Tidepools waits for you to stop.

It picks up where Abyss left off: Abyss's README lists a reverb freeze and a true shimmer tail as still to come,
and Seaglass is built on Abyss's reverb.

## Install

1. Copy the folder `Groundswell - VST - Seaglass` (in `build/` after building) into a folder named
   `VST2 Plugins` on a drive the MPC can see. The internal drive is the safest choice.
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC. Seaglass appears under **External Plugins** in the effect browser. Put it on a track or
   program insert, or on a return with Mix at 100 %.

## How it works

```
in -+------------------------------------------------------------------- dry ---+
    +-> Low Cut -> Pre-Delay -> (+ shimmer) -> 4 diffusers -> 8-line FDN -> wet  +-> Mix -> Output
                                    ^                                    |
                                    +-- pitch shifter <- filters <-------+  (Width, Swell duck on the wet)
```

- **The reverb** is an 8-line feedback delay network (delays of 35–93 ms, a Hadamard mix, a low-pass on each
  line for **Tone**, a gain on each line for **Decay**). Four of the lines drift slowly in length (**Motion**),
  which gives the tail a gentle chorus and stops it ringing metallically. Four allpass diffusers per side in
  front of it smear drum hits into a wash instead of a flutter of echoes. The input is scaled with the decay so a
  held sound comes out at about the same level whatever the Decay.
- **Freeze** crossfades the loop, over **Fade**, into a lossless state: feedback at exactly 1, damping off, and
  the input and the shimmer muted. Nothing new gets in and nothing leaks out, so the pad holds its level and
  colour indefinitely (tested for 31 s: within 1.3 dB). Turning Freeze off crossfades back, and the pad then
  dies away with the current Decay time. Freeze changes gains, never switches audio, so even a 20 ms Fade
  does not click.
- **Shimmer** takes the tail, filters it (two low-passes so the shifted copy doesn't alias, a high-pass so the
  octave-down mode doesn't pile up mud), shifts it with a cheap two-grain pitch shifter (two read heads 70 ms
  apart, crossfaded) and feeds it back into the reverb input. Each trip round the loop shifts it again, so an
  octave climbs to two octaves, then three, until Tone and the filters roll it off. A limiter backs the shimmer
  feed off when the tail gets loud, and a soft clip guards the feed, so even Decay 30 s with Shimmer 100 % stays
  bounded (it peaks around -16 dBFS RMS and fades away).
- **Swell** follows the level of the input. While it is above **Sense**, the wet signal is ducked (by up to the
  **Swell** amount, in 60 ms). When you stop, it comes back over **Rise** seconds on an S-curve, so the tail
  blooms in. The reverb keeps filling while it is ducked, so the full tail is there to bloom.

## Parameters

| Block | Parameter | Range (default) | What it does |
|---|---|---|---|
| SPACE | Mix | 0–100 % (35) | Dry/wet balance, equal power. 100 % = wet only (for returns). |
| | Decay | 0.5–30 s (5 s) | Reverb time: how long the tail takes to fall by 60 dB. |
| | Pre-Delay | 0–250 ms (20) | Gap before the reverb starts. Glides when you turn it (a short pitch bend in the tail). |
| | Tone | 1–16 kHz (7 kHz) | Damping inside the tail: lower is darker, and the highs die sooner. |
| COLOUR | Low Cut | 20 Hz–1 kHz (120 Hz) | High-pass on the way into the reverb, so kicks and bass don't boom. |
| | Motion | 0–100 % (40) | Slow movement of four delay lines: a gentle chorus in the tail. 0 = still. |
| | Width | 0–100 % (100) | Stereo width of the tail. 0 = mono reverb. |
| | Output | -24 to +6 dB (0) | Level after the mix. |
| FREEZE / SHIMMER | Freeze | Off / On | Hold the tail as an endless pad; the input stops feeding it while it is on. |
| | Fade | 20 ms–4 s (250 ms) | How long Freeze takes to settle in and to let go. Long values swell into the freeze. |
| | Shimmer | 0–100 % (0) | How strongly the pitch-shifted tail is fed back. 30–60 % is a classic shimmer. |
| | Interval | Octave / Fifth / 2 Oct / Oct Down | The shift on each trip round the loop. |
| SWELL | Swell | 0–100 % (0) | How far the tail ducks while you play. 100 % = gone while you play. |
| | Rise | 0.1–8 s (1.5 s) | How long the tail takes to bloom back once you stop. |
| | Sense | -60 to -12 dB (-36) | Input level that counts as playing. Raise it for loud material, lower it for quiet pads. |

Parameter order and ids are permanent (new parameters will only ever be added at the end), so projects keep
their settings in later versions.

## Freeze on the Q-Links

Effects on the MPC receive no MIDI, so Freeze is a parameter, not a note. It is on **Q-Link 5** (the first knob of
the third row; the panel's rows map to Q-Links 13–16, 9–12, 5–8 and 1–3).

- **On the screen**, tap the Freeze switch: it toggles.
- **On a Q-Link**, a switch is on in the top half of the knob's travel and off in the bottom half. Turn it up to
  freeze and back down to let go: this is as close to a momentary freeze as the MPC allows. Set **Fade** to taste:
  short (20–100 ms) to catch a moment exactly, long (1–2 s) to swell into it and let it melt away.
- **In a sequence**, record the Q-Link move (or draw it as automation) to freeze at a point in the song: for example,
  freeze the last chord of a phrase under a breakdown and release it on the drop.

While frozen, the input still plays dry (at the Mix balance) over the pad, but it doesn't add to the pad. Swell
still works on a frozen pad: with Swell up, the pad ducks under what you play and comes back in the gaps. Shimmer is
paused inside the frozen loop and picks up again when you release.

## The panel

One page, **SEA**, drawn over your own artwork: the Seaglass title over a sunset beach, with glowing sea glass on
the pebbles. The controls sit in eight dark glass boxes, four down each side, so the title, the wave and the glass
stay in view in the middle. Knob arcs are seafoam and headings lavender, taken from the glass. Each row is one row
of Q-Links:

| Row | Left box | Right box | Q-Links |
|---|---|---|---|
| 1 | SPACE: Mix, Decay | TAIL: Pre-Delay, Tone | 13–16 |
| 2 | COLOUR: Low Cut, Motion | OUTPUT: Width, Output | 9–12 |
| 3 | FREEZE: Freeze (Off / On buttons), Fade | SHIMMER: Shimmer, Interval (tap for a menu) | 5–8 |
| 4 | SWELL: Swell, Rise | SWELL: Sense | 1–3 |

The skin is rendered by `art/make_skin.py` into `skins/`; run it before `tools/build.py` whenever the art or the
layout changes (the build copies `skins/` as it is). `ui.json` is a stock-kit fallback layout and is not used while
`skins/` exists.

## Programs

| Program | Character |
|---|---|
| Seaglass | The default: a 5 s hall at 35 %. |
| Frosted Shore | Short, bright and airy (2.8 s), for drums and vocals. |
| Wave-Worn | Long and dark (12 s, Tone 2.6 kHz), lots of motion. |
| Tumbled Light | Classic octave shimmer (55 %), 10 s. |
| Moon Pool | Shimmer up a fifth over a long, wide 14 s tail with 60 ms pre-delay. |
| Kelp Shadow | Shimmer an octave down: a dark tail that sinks. |
| Cobalt Bloom | Swell 85 %: the tail waits for gaps, with a touch of shimmer. |
| Ghost Tide | Swell 100 % with a slow 4 s rise over an 18 s shimmering tail. |
| Endless Shoal | Ready to freeze: 20 s decay, 1.2 s Fade, Mix 60 %. |

## Tips

- **Freeze a chord, then play over it**: play a chord into Endless Shoal, turn Q-Link 5 up, keep playing. Drop Mix
  for less pad, or put Seaglass on a return so the frozen pad sits under several tracks.
- **Freeze a drum hit** for a drone: freeze just after a snare with Tone low and Motion up.
- **Shimmer on a return**, fed a little of pads or keys, gives the "choir in the rafters" sound without cluttering
  the dry track. Keep Low Cut up (200 Hz+) so only the top climbs.
- **Swell on drums**: Swell 100 % and Rise 0.3–0.6 s keeps the groove dry and lets the room bloom only in the
  breaks and at the end of fills. Use Sense to choose what counts as a hit.
- For a frozen pad that slowly fades rather than holds, leave Freeze off and set Decay to 30 s instead.

## CPU

On a desktop, Seaglass takes about **0.14 % of real time** for one instance with shimmer, freeze and swell all
in use (measured over the 60 s demo). By the ratio seen with the other Groundswell plugins that should be roughly
**1–2 % of one MPC core**. This is an estimate from the desktop, not yet measured on the hardware.

How it stays cheap: everything that needs `exp`, `pow` or `log` (loop gains, damping, filter coefficients, the
modulation, the swell detector) is worked out once per block and ramped through it. Per sample it does 8 delay
reads, 4 allpass interpolations for the moving lines, a 24-addition Hadamard mix, 8 one-pole filters, 8 diffuser
allpasses, and, only while Shimmer is above 0, the shimmer path (6 one-pole filters, 4 interpolated reads).

## What is not in this version

- **No Size control.** The delay lengths are fixed (35–93 ms, a medium-large hall); Decay sets the reverb time and
  Pre-Delay the distance. Changing the room size live without artefacts would need interpolated reads on all eight
  lines.
- **Freeze replaces, it doesn't layer.** While frozen the input is shut out of the pad; there is no "freeze and keep
  adding" mode yet.
- **No true hold-to-freeze**: Freeze is a toggle on the screen and a half-way threshold on the knob (see above).
  A button that freezes only while held would need a momentary control for plugin switches, which we haven't found
  on the MPC. Not yet tried on the hardware.
- **Shimmer is a simple two-grain shifter**, not a formant-preserving one: it has a light 14 Hz grain flutter on
  its own, which the reverb smears into the tail. The 2 Oct setting is the roughest (its grains are shortest).
- **Shimmer at 100 % and the longest Decay doesn't sustain forever**: energy climbs out of range and the tail fades.
  For an endless shimmer pad, let it bloom, then Freeze.
- **Swell watches the input level only**, not the tempo or the notes. It is not locked to the song.
- **Changing Interval while the shimmer is sounding** jumps the shift ratio at once (a pitch change in the shimmer,
  not a click).

## Testing

All checks run on the computer:

- `python3 tools/build.py plugins/Seaglass`: native, arm32 and arm64 builds and the folder validation, 0 errors,
  0 warnings.
- `.sdk-tools/host_sim plugins/Seaglass/build/native/seaglass.dylib test --seconds 6` (and `--block 32`):
  20 pass, 0 warn, 0 fail. host_sim has no option to feed a file, so it uses its own test signal (a 220 Hz sine
  and a noise burst, mono); the stereo material is in the listen test below.
- `tests/listentest.cpp` (66 checks) feeds synthesised stereo material: a pad chord detuned between left and
  right, a sine, steady noise and a drum loop with hats panned left and right. It checks:
  - every program is finite, under 0 dBFS and leaves a tail;
  - every one of the 15 controls changes the sound on its own;
  - Freeze holds a chord for 31 s (1 s windows within 1.3 dB, no drift, spectrum steady). With noise playing
    straight through, engaging and releasing cause no level jump (Fade 250 ms and 20 ms). The input is shut out
    while frozen (drums change the frozen pad by -146 dB). It lets go at the Decay rate. No clicks anywhere,
    including frozen with Shimmer and under a playing chord at Mix 35 %. The click detector self-tests on the
    same frozen audio and catches a 0.03 step in the pad and a 0.002 step in the release tail;
  - Shimmer puts energy at 440 Hz (Octave), 330 Hz (Fifth), 880 Hz (2 Oct) and 110 Hz (Oct Down) from a 220 Hz
    tone, and the octave keeps climbing to 880 Hz. At Decay 30 s and Shimmer 100 % with a loud chord and drums,
    every interval stays bounded and finite;
  - Swell 100 % keeps the tail down while a chord plays (gone), 50 % ducks part way, the tail blooms back over
    Rise after the chord stops, and on drums with gaps it is held 58 dB down under the hits and fully back in
    the gaps;
  - random parameter changes every 50 ms stay finite and under 0 dBFS, at 44.1, 48 and 96 kHz;
  - a 60 s demo: drums and chords → freeze → frozen pad alone → release → shimmer → swell on chord stabs.

```
cd plugins/Seaglass
mkdir -p tests/build
clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
tests/build/listentest build/seaglass-demo.wav
```

Not yet tried on the MPC itself.

## Project layout

| Path | What |
|---|---|
| `plugin.json` | Name, manufacturer, uid `GvXf`, version |
| `src/Seaglass.cpp` | All the DSP, parameters and programs |
| `art/` | Your plate (`plate-src.webp`), the shared knob and pill art, and `make_skin.py` |
| `skins/` | The rendered panel (TUI.json, Q-Links.json, images); copied into the build as is |
| `ui.json` | A stock-kit fallback layout (unused while `skins/` exists) |
| `tests/listentest.cpp` | The offline listen test and demo render |

## Licence

MIT, like the rest of the Groundswell plugins. Original code; the reverb is Abyss's (also Groundswell, MIT).
The FDN, Schroeder allpass diffusers, allpass interpolation and the two-grain delay-line pitch shifter are
textbook techniques; no code was copied from elsewhere.
