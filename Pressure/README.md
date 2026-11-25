# Pressure

**A compressor and saturator with a lot of character.** An MPC insert effect by Groundswell.

There are plenty of clean, transparent compressors. Pressure is not one of them. One big **Depth** knob
squeezes harder the deeper you go, and the output stays at the same loudness, so turning it up makes the
sound thicker, denser and dirtier, not just louder. Four **Hulls** give it four personalities, **Implode**
blows a drum loop apart, and the saturation **growls** harder the more it compresses.

Insert effect, stereo in / stereo out. MIT licence.

## How it works

```
in ─┬──────────────────────────────────────────────────── dry (delayed 23 samples) ──┐
    │                                                                                │
    ├─► [Mid/Side] ─► gain element ─► [back to L/R] ─► makeup ─► saturator (2x) ──── wet
    │        ▲                                                Tape / Tube / Iron     │
    │   detector: SC HPF ─► peak or RMS ─► Link ─► curve ─► attack / release ─► Bloom   │
    │   (Bite, Bus listen to the input; Glass, Valve listen to their own output)        │
    │                                                                                ▼
    └─► level tracker (sets the reference)                Blend ─► loudness match ─► + Breath ─► Output
```

- **The reference.** Pressure has no threshold or input knob. It follows the level of what you feed it (a
  slow peak envelope and a slow RMS, a few seconds long, pulled a little towards -14 dBFS) and sets the
  threshold relative to that. So Depth does about the same amount of squeezing on a quiet single sound,
  a hot drum bus or a full mix. Loud and quiet sections of a song keep most of their difference; loud
  passages are squeezed slightly more. The saturator is scaled to the same reference, so how dirty it gets
  does not depend on how hot the track arrives either.
- **Depth** moves everything together along a tuned curve: the threshold drops (from a few dB under the
  programme's peaks down to 25–30 dB under them), the ratio rises (each Hull has its own range), the
  saturation builds, and in Auto timing the attack and release get quicker, so it gets denser.
- **Loudness match.** Two parts. Most of the gain the compressor takes away comes straight back
  (predicted from the curve), so a Depth move is roughly level at once; then a slow measurement of input
  against output (a 100 Hz high-passed RMS, settling over about 1.5 s, at most ±9 dB) trims the rest. In
  the tests, every Hull at every Depth stays within 0.3 dB of the input on drums, bass and a mix, and every
  program within 0.1 dB. The measurement makes the match honest for A/B comparisons, but it is a slow
  automatic gain: after a big jump you may hear the level settle for a second or so. **Output** comes after
  it, for when you actually want it louder.

## The Hulls

Measured at Depth 60 with a -6 dBFS tone burst (attack: time to 63 % of the gain reduction; release: time
to recover 90 %).

| Hull | Character | Detector | Curve | Attack | Release |
|---|---|---|---|---|---|
| **Glass** | Slow, smooth optical. Gentle on peaks, glues vocals and pads | RMS (12 ms), feedback | Very soft knee (14 dB), 2:1 → 6:1 | ~10 ms | Two-stage: ~350 ms after a short hit, ~1.5 s after a long squeeze |
| **Bite** | Fast, aggressive transistor grab, slightly dirty: the gain element adds even harmonics the harder it grabs | Instantaneous peak, feed-forward | Hard knee (3 dB), 4:1 → 14:1 | ~0.5 ms | ~150 ms, no memory |
| **Valve** | Variable-mu tube style: soft at the threshold, firmer the harder you push, warm glue | RMS (20 ms), feedback | The ratio rises with level, up to 3:1 → 10:1 | ~15 ms | ~0.8 s; about 1.6 s after a long squeeze |
| **Bus** | Tight, punchy console/VCA glue for drum buses and the master | RMS (3 ms), feed-forward | Medium knee (6 dB), 2:1 → 6:1 | ~6 ms | ~450 ms auto release: slower after a long squeeze, so it does not pump by accident |

- **Feedback Hulls** (Glass, Valve) listen to their own output, the way the old optical and tube designs
  do. That makes them rounder and more programme-dependent, and it makes them react two to three times
  faster than their attack setting, because the loop speeds them up (Glass's Auto attack at Depth 60 is
  set to 28 ms and reaches 63 % in 10 ms). Bear that in mind with a manual Attack.
- **Slow attacks let transients through.** On drums, Glass and Valve at medium Depth leave the hits alone
  and squeeze what is between them, so at the same loudness the peaks come out a few dB hotter (+3.5 dB in
  the demo). That is the punch of a slow compressor; mind your headroom when using them on the master.
- **Implode** is the all-buttons-in trick taken further. It works on every Hull (always with Bite's peak
  detector) and is most at home on Bite and drums. The curve is close to a limiter, and past 6 dB over it
  folds back: the louder the hit, the quieter it comes out, so the room and the tails explode around it.
  The attack is fast, the Auto release shortens and overshoots (extra Bloom), and the saturator is driven
  about 10 dB harder with extra asymmetry, reacting to each hit the moment it arrives. On the test drums
  it more than doubles the gain reduction and takes a 110 Hz tone from 3 % to 20 % distortion.

## Saturation: Colour and Growl

The saturator runs after the gain element and makeup, oversampled 2x (a 47-tap halfband, 23 samples of
latency, reported to the host; the dry path is delayed to match). The shaper is a rational tanh with
separate limits for the positive and negative swings, so quiet signals pass almost untouched and
asymmetry (even harmonics) appears as it is pushed.

- **Growl** sets how much it saturates, and how strongly the drive follows the gain reduction: about
  0.75 dB more drive per dB of gain reduction at 100 %. The harder it compresses, the more it growls. At
  Growl 60 on a Valve, a 110 Hz tone goes from 0.6 % distortion at Depth 10 to 4 % at Depth 90 (Tape).
  Growl 0 still leaves a little colour that builds with Depth.

| Colour | What it does |
|---|---|
| **Tape** | Symmetric. Pre-emphasis before the shaper and matching de-emphasis after, so the highs saturate first and the top softens as you push |
| **Tube** | Asymmetric: second harmonic, warmth |
| **Iron** | A transformer core: the band under ~150 Hz is driven harder than the rest, for weight and low-end grit |

Bite (and Implode) add asymmetry in proportion to the gain reduction: a dirty gain element.

## Timing: Attack, Tide and Bloom

- **Attack**: fully left is **Auto** (from the Hull and Depth); turn it up for 0.05–120 ms by hand.
- **Tide** picks where the release comes from: **Auto** (the Hull's own release, getting quicker with
  Depth), **Free** (the **Release** knob, 20 ms to 3 s) or **Sync** (the **Release** knob picks a note value,
  1/32 to 2 bars, locked to the song's tempo). Release times are recovery times: a synced 1/4 has let go
  (90 %) by about three quarters of a beat, ready for the next one. In Sync, the Hull's two-stage memory is
  bypassed so the pumping stays exactly in time. The tests check that the release scales exactly with tempo.
- **Bloom** lets the release overshoot: as the gain recovers it swells above unity (up to 6 dB) and then
  settles back, a breathing, blooming pump. At 100 % after a 10 dB squeeze, the overshoot is about 2 dB.

## Mix, sidechain and stereo

- **Blend**: 0 % is dry, 50 % is the full dry signal with the full compressed signal under it (New York
  style parallel smash), 100 % is compressed only. The loudness match covers the blend too.
- **SC HPF**: a 12 dB/octave high-pass on the detector only, Off or 60–250 Hz, so the kick does not yank
  the whole mix. On a kick-heavy mix at Depth 60 (Bus), 250 Hz cuts the mean gain reduction from 4.4 dB to
  0.7 dB.
- **Link**: 100 % gives both sides one gain (the louder side decides); 0 % lets each compress on its own.
- **Stereo**: **L/R** or **Mid/Side**. In Mid/Side the gain element works on mid and side (with Link
  between them), then converts back to left and right before the saturator. With Link down, a wide mix's
  sides can breathe separately from the centre.
- **Breath**: an old-gear noise floor, pink hiss through a 7 kHz low-pass, about 60 dB under the programme,
  rising 1 dB per dB of gain reduction, so the harder it works the more you hear the machine.

## Parameters

| # | Name | Range | What it does |
|---|---|---|---|
| 0 | Output | -18..+12 dB | Level after everything (the host nudges parameter 0 on load; it does nothing else) |
| 1 | Depth | 0..100 % | The macro: threshold, ratio, saturation, density and Auto timing |
| 2 | Hull | Glass / Bite / Valve / Bus | The compressor's personality |
| 3 | Implode | Off / On | The all-buttons-in drum crush |
| 4 | Colour | Tape / Tube / Iron | The saturator's flavour |
| 5 | Growl | 0..100 % | How much saturation, and how hard it follows the gain reduction |
| 6 | Attack | Auto, 0.05..120 ms | Auto follows Hull and Depth |
| 7 | Tide | Auto / Free / Sync | Where the release comes from |
| 8 | Release | 20 ms..3 s, or 1/32..2 bars | Used in Free and Sync; shows "Auto" in Auto |
| 9 | Bloom | 0..100 % | Release overshoot and swell |
| 10 | Blend | 0..100 % | Dry / parallel / compressed |
| 11 | SC HPF | Off, 60..250 Hz | Sidechain high-pass |
| 12 | Link | 0..100 % | Stereo link |
| 13 | Stereo | L/R / Mid/Side | Which pair is compressed |
| 14 | Breath | 0..100 % | Noise floor that rises as it compresses |
| 15 | GR | 0..20 dB | Meter: gain reduction (not saved, not automatable) |

The parameter IDs and order are permanent; new parameters will be appended.

### The GR meter

The plugin writes the gain reduction (the louder side, or mid/side) about 25 times a second, only when it
changes, with `setNormalised` and a host notification. It has VU-like ballistics (90 ms) and is quantised
to 1/128 of its 0–20 dB scale, so any film strip up to 129 frames can follow it (frame = normalised value ×
(frames − 1); 0 = no gain reduction). The panel's lamp uses 21 frames; a needle meter could use 128.

## The panel

The controls sit inside a rusted brass porthole under water. Each row of four is one Q-Link bank, on a soft dark band
so the light rays in the glass do not wash out the lettering. Depth has the big knob. Hull, Colour and Tide are
rotary selectors (detent dots, the value shows the choice); Implode and Mid/Side are lit pills.

| Page | Q-Links 1–4 | Q-Links 5–8 |
|---|---|---|
| **PRESSURE** | Depth, Growl, Blend, Output | Hull, Colour, Implode, Breath |
| **TIDE** | Attack, Release, Tide, Bloom | SC HPF, Link, Mid/Side, (empty) |

**The caged lamp is the gain-reduction meter**, on both pages. The background has it turned down to an ember. A
21-frame film strip on the GR parameter (1 dB per frame) brings the real lit lamp back through a soft mask, with a
halo on the wall, so it burns brighter the harder Pressure squeezes. The value is printed under it as SQUEEZE.

`art/make_skin.py` renders `skins/` from `art/plate-src.webp` (the porthole art, 1774×887) and the shared knob and
pill images. Run it before `tools/build.py`, which copies `skins/` as it finds it. It also writes
`build/skin-preview-1.png` and `-2.png`. `ui.json` is only the stock-kit fallback.

## Programs

| Program | For | Settings |
|---|---|---|
| **Gentle Glue** | The master | Bus, Depth 38, Iron, SC HPF 90 Hz: about 1.5 dB of glue |
| **Drum Smash** | Drum bus | Bite + Implode, Depth 75, Tube, Blend 50 (parallel), Bloom 30 |
| **Pumping Bus** | Pumping to the beat | Bus, Depth 65, release synced to 1/4, Bloom 45, sidechain open so the kick drives it |
| **Vocal Glass** | Vocals, leads | Glass, Depth 50, Tube, SC HPF 120 Hz |
| **Bass Grip** | Bass | Bite, Depth 55, 3 ms attack, Iron |
| **Warm Valve Tape** | Pads, keys, a soft master | Valve, Depth 45, Tape, Growl 50, a little Breath |
| **Deep Trench** | Extreme | Glass at Depth 100, Growl 85, 1/2 synced release, Bloom 60, Mid/Side with Link 70, Breath 35 |
| **Loop Crush** | Sampled loops | Bite + Implode, Depth 85, Tape, 1/8 synced release, Blend 80, Breath 25 |

## On the MPC Sample (Knob FX)

From 1.1, Pressure runs on the MPC Sample (firmware 1.4 or later) as a Knob FX, listed as
**Pressure** at the bottom of FX Select. The MPC Sample has no plugin screen, so six parameters
sit on its three knobs:

| | Knob 1 | Knob 2 | Knob 3 |
|---|---|---|---|
| **Knobs** | Depth | Growl | Blend |
| **SHIFT held** | Hull | Colour | Implode |

The amounts are on the knobs you play; the character switches (Hull, Colour, Implode) are on
SHIFT. Everything else stays at Gentle Glue's settings: Output 0 dB, auto attack and release,
no Bloom, SC HPF 90 Hz, full Link, L/R, no Breath. Depth is loudness-matched, so Output is not
needed on a knob. This makes it a good effect to resample through: set the squeeze, bake it in
with the Resample Pad, then layer another effect on top.

To install, copy the plugin folder into `VST2 Plugins` on the MPC Sample's microSD card in SD
Card Access mode, and turn on MIDI Config → Dev Enhancements (Beta) → Custom Plug-In Support.

## CPU

About 0.21 % of one desktop core with the defaults and 0.25 % with everything on (a feedback Hull, Iron,
Mid/Side, the sidechain filter, Bloom, Breath and Blend). By the usual 15–30x factor, that is roughly
3–7 % of one core on an older 32-bit MPC and less on newer units. Not yet measured on hardware. Most of
the cost is the 2x oversampled saturator. There are no per-sample transcendental functions: levels and
gains use fast log2/exp2 polynomials, and coefficients are computed once per block.

## Tests

`tests/listentest.cpp` renders synthetic drums, bass and a mix and checks 38 things, including:
- gain reduction grows with Depth on every Hull and source, from under 3 dB at Depth 0 to over 6 dB at
  Depth 100;
- the loudness match (worst 0.3 dB across all Hulls, Depths and sources);
- each Hull's attack and release, with every pair differing by 1.4x or more, and Glass's programme-dependent
  release;
- the synced release scaling exactly with tempo;
- Blend 0 nulling against the dry signal;
- the SC HPF, Link, Mid/Side, Implode, Growl, Bloom, Breath and the meter;
- no NaN or denormals for every program at 44.1, 48 and 96 kHz and under random settings;
- the CPU.

```
mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework -o tests/build/listentest tests/listentest.cpp
tests/build/listentest build/pressure-demo.wav
```

The second line also renders `build/pressure-demo.wav` (2 min 35 s at 100 BPM). It plays drums, then bass,
then a mix, each dry and then through Glass, Bite, Valve and Bus at Depth 65 and Bite + Implode. Then a
Depth sweep from 0 to 100 % over 8 bars, and all eight programs on the mix.

## Not implemented / honest notes

- No threshold, ratio or input knobs: Depth and the reference tracker decide them. If you want a fixed
  threshold that responds to how hot you drive it, this is not that compressor.
- The level tracker needs a moment: right after silence, the first second may compress a little
  differently while it finds the level.
- The loudness match is an automatic gain with a slow measurement. It is not an LUFS meter, and it will
  gently counter deliberate level changes inside the plugin (Blend, Bloom) over a second or two.
- No lookahead and no true-peak limiting: Pressure is not a brickwall limiter.
- The sidechain is internal only (MPC effects get no external sidechain input).
- The Hulls are generic personalities voiced by ear and by measurement, not models of particular units.

## Credits

Original design and code, MIT licence. The 2x halfband oversampler is the one from Groundswell's
Breakwater (Kaiser-windowed, written for these plugins). No third-party code.
