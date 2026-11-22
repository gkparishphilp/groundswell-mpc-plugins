# Breakwater

**A guitar amp for clean, chimey tones and the edge of breakup.** An MPC insert effect by Groundswell.

Plug a guitar into the MPC, put Breakwater on the track, and play. Breakwater stays clean when you play
softly and only starts to hair up when you dig in, the way a good valve amp does. It is built for glassy
cleans, chime, funk, surf, jazz and arpeggios, rather than for high gain. It is also voiced to sit after
[Fathom](../Fathom/), the auto-wah: put Fathom first in the track's inserts and Breakwater second.

Insert effect, stereo in / stereo out. The amp itself is mono; only the spring reverb is stereo. MIT licence.

## How it works

```
in L+R ─► Input ─► Comp ─► Bright ─► ┌─────────────── 2x oversampled ────────────────┐
                                     │ stage 1 ─► Gain ─► stage 2 ─► tone stack      │
                                     │ ─► Chime ─► Master ─► power stage (Sag)       │
                                     └───────────────────────────────────────────────┘
        ─► Tremolo ─► Cabinet (Mic, Distance) ─┬───────────────────────────► dry ─┐
                                               └─► spring reverb (2 springs) ─ wet ┴─► Output ─► L, R
```

- **Mono amp.** The left and right inputs are summed, (L + R) / 2. That costs half the CPU of a stereo
  amp. A guitar recorded on one side only arrives 6 dB quieter: turn **Input** up by 6 dB. Fathom's
  Parallel mode pans two filters apart; the amp folds them back to the centre, so use Fathom in Series
  (or keep its stereo for after the amp by putting a second Fathom later in the chain).
- **Input and Comp.** **Input** sets where your playing sits against the breakup point. **Comp** is a
  gentle peak compressor before the preamp, from a light touch to about 4:1. It evens out pick strokes
  so a wah's peaks and a strummed chord drive the amp more consistently. 0% bypasses it.
- **Preamp.** Two triode-like stages, oversampled 2x. Each is a smooth tanh curve that clips at +1 on
  the positive swing but later on the negative one. Around zero the curve is almost straight and
  symmetrical, so soft playing is clean (well under 1% distortion). The asymmetry, which makes the
  even harmonics of a valve stage, only appears as you play harder. **Gain** sets how hard stage 2 is
  pushed. The output is partly compensated, so the level rises with Gain, but gently.
- **Bright.** A bright cap across the volume pot, modelled as a treble shelf. Its boost is biggest with
  Gain low (+7 dB at Gain 1) and gone at full Gain, as on the real thing.
- **Tone stack.** The classic passive bass/middle/treble circuit, computed from its component values
  (see Credits). Because it is the circuit and not three EQ bands, the knobs interact. Middle changes
  how much the Bass and Treble can do: the Bass range changes by 3–5 dB between Middle 0 and 10.
  Everything at 0 is nearly silent, as on a real amp. Bass uses an audio-taper pot. Coefficients are
  recomputed only when a knob moves (it glides over ~30 ms).
- **Chime.** A presence peak in the upper mids, plus a little even-harmonic "shimmer": the upper mids are
  squared and added back. That is the sparkle of a class-A power amp, and it grows with level.
- **Power stage and Sag.** **Master** drives a soft push-pull clip; the level is roughly compensated, so
  Master changes the feel more than the volume. **Sag** makes the supply rail drop as the output gets
  loud (8 ms) and recover over about 220 ms. Hard playing is squeezed and clips earlier, then blooms
  back as the note rings.
- **Voice** sets the character of the whole amp: the tone-stack component values, how hard stage 1 is
  pushed, each stage's asymmetry, the interstage brightness, where Chime sits and how much the supply
  sags.

| Voice | Character |
|---|---|
| **American** | American clean. A scooped stack, big headroom and a stiff supply: glassy, deep, hard to break up |
| **British** | British chime. A mid-forward stack, brighter between the stages, breaks up earlier, with class-A asymmetry in the power stage. The default |
| **Boutique** | Boutique edge. A fat-mid stack, warmer between the stages, the most touch-sensitive breakup and the most sag |

- **Cabinet.** A handful of filters instead of a long impulse response, to save CPU: a resonant
  low-end roll-off, low-mid body, the cone's mid dip, the speaker's presence peak and three low-pass
  sections. No cab "fizz": 10 kHz sits 25–29 dB further down than with **Direct**. Two short reflections
  are added, one from the floor and, on open-back cabs, one from the wall behind.

| Cab | Character |
|---|---|
| **1x12** (open back) | Lighter lows, a boxy upper-mid honk, the brightest top |
| **2x12** (open back) | Fuller lows, a scooped lower mid, a broad presence hump |
| **4x12** (closed back) | The tight resonant thump around 100 Hz, the deepest mid scoop, a sharp presence peak |
| **Direct** | No cabinet: for an external speaker or your own impulse response |

  **Mic** moves the microphone from the centre of the cone (strong presence, extended top) to the edge
  (darker, rounder, more low mids): about 10 dB of tilt. **Distance** runs from 2 cm to 1 m. Up close
  you get the proximity effect, up to +7 dB of low end. Further back the floor bounce combs the sound,
  the top softens, and on open backs the room comes in.
- **Spring reverb.** Two springs, left and right, for a stereo tail. Each is a feedback loop (34 / 41
  ms, with a slow wobble) around a chain of 40 *stretched* allpass filters. These delay the upper
  frequencies more than the lower ones, so each echo comes back as a chirp. In the first echo, 4 kHz
  arrives about 7 ms after 500 Hz, and every pass round the loop smears it further: the drip. **Dwell**
  pushes the tank's driver harder (big hits splash and compress) and lengthens the decay from 0.7 s to
  4 s. **Rev Tone** is the tail's brightness. **Rev Mix** adds the tail on top of the amp, with the dry
  level easing back a little at high settings. At Rev Mix 0 the spring is switched off.
- **Tremolo.** **Smooth** is a sine. **Opto** is the squarer, lamp-and-light-cell throb, quick to switch
  on and slow to let go. **Harmonic** splits the signal at about 650 Hz and swells the lows and highs in
  opposite phase: the level hardly moves, but the tone swirls. **Trem Sync: Sync** locks the tremolo to
  the song while the MPC plays: full volume on the beat, at 2 bars down to 1/32, including dotted and
  triplet values. **Free** runs from 0.5 to 12 Hz. Trem Depth 0 switches it off.

## Parameters

| # | Name | Range | Default | What it does |
|---|---|---|---|---|
| 0 | Output | −24..+12 dB | −2 | Level after everything (the host nudges parameter 0 on load; it does nothing else) |
| 1 | Input | −12..+18 dB | 0 | Level into the amp |
| 2 | Comp | 0..100 % | 15 | Gentle compression before the preamp |
| 3 | Voice | American / British / Boutique | British | The amp's character |
| 4 | Gain | 0..10 | 6 | Preamp drive |
| 5 | Bright | Off / On | On | Treble boost, strongest at low Gain |
| 6–8 | Bass, Middle, Treble | 0..10 | 4.5, 6, 6 | The tone stack |
| 9 | Chime | 0..10 | 6 | Upper-mid presence and class-A shimmer |
| 10 | Sag | 0..10 | 5 | Supply sag: compression and bloom |
| 11 | Master | 0..10 | 6 | Drive into the power stage |
| 12 | Cab | 1x12 / 2x12 / 4x12 / Direct | 2x12 | |
| 13 | Mic | Centre..Edge | 30 % | Where the mic points on the cone |
| 14 | Distance | 2..100 cm | 15 cm | How far the mic is from the grille |
| 15 | Dwell | 0..10 | 3 | Spring drive and decay |
| 16 | Rev Tone | 0..10 | 5 | Spring brightness |
| 17 | Rev Mix | 0..100 % | 12 | Spring level |
| 18 | Trem Mode | Smooth / Opto / Harmonic | Smooth | |
| 19 | Trem Depth | 0..100 % | 0 | 0 = off |
| 20 | Trem Sync | Free / Sync | Sync | |
| 21 | Trem Rate | 0.5..12 Hz, or 2 bars..1/32 | 1/8 | One control; the display follows Trem Sync |

## The panel

The controls sit on the faceplate of the amp head, under its glowing valves, screen-printed like a real
amp's. Two pages; each half-row of four is one Q-Link bank, framed on the plate so the Q-Link focus lands
on it:

- **AMP**: Gain, Bass, Middle, Treble | Chime, Sag, Master, Output on the top row; Input, Comp, Voice,
  Bright | Cabinet, Mic, Distance and the pilot lamp on the second.
- **FX**: Spring Reverb (Dwell, Tone, Mix) | Tremolo (Depth, Rate, Mode, Sync).

Voice, Cabinet and the tremolo's Mode are rotary selectors: a knob with a dot for each position. Tap one
for a menu. Bright and Sync are single buttons that light amber when on.

The skin is generated by `art/make_skin.py` from `art/plate-src.webp`. The knob and pill art is shared
with the other Groundswell plugins, and the pill's glow is re-tinted to valve amber. Render it **before**
building, because the build copies `skins/` as it finds it:

```
python3 plugins/Breakwater/art/make_skin.py
python3 tools/build.py plugins/Breakwater
```

`ui.json` (the SDK's amber kit) is kept as a fallback layout; it is not used while `skins/` exists.

## Programs

| Program | What it is |
|---|---|
| **Edge of Breakup** | British voice, Bright on, Chime and Sag at mid-way, a 2x12: glassy when you pick lightly, crunchy when you dig in. The default |
| **Clean Tremolo** | American clean with a deep smooth tremolo on 1/8 notes, locked to the song, and some spring |
| **Funk Glass** | Voiced to follow Fathom. Low gain, so the wah's peak never breaks the amp up. Comp at 45% to even out the strokes, a lower-mid scoop, a bright 1x12 close-miked on the centre, a stiff supply and almost no reverb |
| **Surf Spring** | American clean, bright, with Dwell at 8 and a lot of spring: drip on every pick |
| **Jazz Warm** | American, Bright off, Treble low and Bass up, the mic towards the edge of a 1x12 and further back: round and warm |
| **Sparkle Arps** | British, Chime at 8, Treble up, a little compression and a bright spring for ringing arpeggios |
| **Harmonic Swirl** | Boutique voice with the harmonic tremolo on 1/4 notes |
| **Boutique Push** | Boutique voice into a 4x12, with Gain and Sag high: the most broken-up sound here, and it still cleans up when you play softly |

On the same riff every program sits within about 1 dB of the others.

## With Fathom

Fathom goes first, then Breakwater. Start from **Funk Glass**, or any program, and:

- Put Fathom's **Drive** low: Breakwater's preamp adds the grit, and it reacts to the wah's peaks.
- If the wah's peaks break the amp up too much, lower **Gain** or **Input**, or raise **Comp**.
- Fathom's stereo (Parallel, Spread) folds to mono in the amp; see "Mono amp" above.

## Tips

- **Set Input first.** On Edge of Breakup, raise Input until your hardest strums just crunch and your
  light ones stay clean. Then shape the sound with Gain.
- **Bright** is mostly for low Gain settings: with Gain above 7 it does almost nothing.
- **Middle** is the most interactive knob. Turning it down scoops the mids and also changes what Bass and
  Treble do. If Bass seems to do little, raise Middle.
- **Mic and Distance** are the quickest way to brighten or darken a sound without touching the amp.
- **Sag** is felt more than heard: it changes how notes swell after you pick. Try it with Master at 7 or
  more.
- A synced tremolo locks to the bar only while the MPC plays; stopped, it runs at the tempo's rate.

## What's not modelled

- The preamp and power stage are shaped curves, not circuit simulations: no grid current, no blocking
  distortion, no coupling-cap bias shift beyond what the DC blockers do, no output transformer and no
  speaker impedance pulling on the power stage. Sag follows the output level, not a model of a supply.
- The cabinets are filter sets, not impulse responses: no cone break-up detail, no room. Choose Direct to
  use your own impulse response after Breakwater.
- The spring models the low "chirp" region only (up to about 4.3 kHz). There is no separate high-
  frequency splash path, and the springs don't rattle when the amp is bumped.
- The tremolo changes volume only; there is no pitch vibrato.
- No noise gate, hum or hiss.
- Latency: 23 samples (0.5 ms at 44.1 kHz), from the oversampling filters. It is reported to the host.

## CPU

On a desktop, everything on (spring, harmonic tremolo, cab) takes about 0.38% of one core. The amp
alone takes 0.18%, the cab 0.03% and the spring 0.17%. The spring's two allpass chains are worked out
two samples at a time, interleaved, which halves their cost with identical output. An MPC is roughly 10–20x
slower, so expect somewhere around 4–8% of one core on an older 32-bit MPC, and less on a Live III or XL.
This has not yet been measured on hardware. Rev Mix 0 switches the spring off, Trem Depth 0 the tremolo,
and Direct the cab. Per-sample work uses only rational shapers (no exp, log or tanh). Coefficients are
recomputed per block, and only when a knob has moved; the compressor's dB maths runs every 16 samples.

## Tests

`tests/listentest.cpp` renders a synthetic plucked guitar (Karplus-Strong) and test tones through the
plugin, and checks:
- soft playing stays clean in every program (worst: 1.2% distortion at −32 dBFS), while hard playing
  breaks up (Edge of Breakup: 23% at −8 dBFS);
- distortion rises with Gain, and even harmonics appear;
- the tone stack's knobs have real ranges and interact;
- Bright fades out with Gain, and Chime lifts the upper mids;
- the cabs differ from one another, and none of them fizz;
- Mic and Distance do what they say;
- the synced tremolo lands within 1 ms of the beat (started off the grid, with varying block sizes),
  dotted and long divisions have the right period, and the harmonic mode swaps lows and highs;
- Sag and Comp squeeze hard playing;
- the spring's decay, stereo and chirp;
- the programs' levels, and that nothing clips;
- 48 and 96 kHz;
- CPU.

It also writes the demo, `build/breakwater-demo.wav`: every program on its own riff at 96 BPM, then the
funk riff through Fathom's own build (Classic Quack) into Funk Glass.

```
cd plugins/Breakwater
mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
tests/build/listentest
```

## On the computer

```
python3 tools/build.py plugins/Breakwater --target desktop
cp -R plugins/Breakwater/build/desktop/Breakwater.vst ~/Library/Audio/Plug-Ins/VST/
```

After a rebuild, quit FL Studio completely before trying the new version: a running host keeps the old
binary loaded.

## Credits and licence

MIT. Everything is written from scratch for this plugin:

- **Tone stack**: the transfer function of the classic passive bass/middle/treble circuit, from D. T. Yeh
  and J. O. Smith's paper on discretising the guitar amp tone stack, Proc. DAFx-06 (Montreal, 2006),
  discretised with the bilinear transform as the paper describes. The three voices use three well-known
  sets of component values for that circuit.
- **Spring reverb**: the stretched-allpass dispersion structure follows V. Välimäki, J. Parker and
  J. S. Abel, "Parametric spring reverberation effect", JAES 58(7/8), 2010. Only the structure is used; the
  code and tuning are original.
- **Oversampling**: a Kaiser-windowed halfband FIR, the textbook design.
- **Biquads**: the SDK's RBJ "Audio EQ Cookbook" filters (MIT).
