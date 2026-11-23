# Cuttlefish

**Change colour.** A morphing wavetable synth for Akai MPC standalone hardware, by Groundswell.

A cuttlefish changes colour; Cuttlefish changes timbre. Each of its two oscillators plays a **wavetable**: 32
single-cycle waveforms in a row. **Morph** slides through them, blending neighbouring frames so the sound
changes smoothly, for example from a sine to a saw to a narrow pulse, or through the vowels A-E-I-O-U. **Warp**
then bends, squashes, syncs or frequency-modulates the cycle. A morph envelope, two LFOs (free or locked to the
song), a small mod matrix and **Drift** keep it moving. One **Colour** knob moves both morphs and the filter
together, so a single Q-Link sweeps the whole sound. It is meant to be the family's everyday synth: leads,
pads, plucks, basses, keys and evolving textures.

## Install

1. Copy the `Groundswell - VST - Cuttlefish` folder (in `build/` after building) into a folder named
   `VST2 Plugins` on a drive the MPC can see. The internal drive is the safest place.
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC. Cuttlefish is listed under External Plugins.

## Architecture

```
  OSC 1 ─┐  Table + Morph (32 frames, read between neighbours) ─ Warp (Bend / Squash / Sync / FM)
  OSC 2 ─┤  Oct / Semi / Fine / Level; Unison 1-4 detuned copies spread across the stereo field
  SUB   ─┤  sine (-1 or -2 oct), triangle or square under oscillator 1
  NOISE ─┘  white noise through a tone control
         ─> FILTER  LP12 / LP24 / BP / HP / Notch state-variable filter with Drive
         ─> AMP     ADSR, velocity, pan
  all voices ─> CHORUS (chorus / ensemble / flange) ─> PING-PONG DELAY ─> REVERB ─> Output ─> soft limiter
```

- **Wavetables are generated in code** the first time Cuttlefish loads (about 65 ms on a desktop, probably
  0.3–1 s on the MPC; once per session, shared by every instance, about 14 MB of memory). Each frame is
  described as a harmonic spectrum, either directly or by drawing the waveform and taking its FFT, and then
  stored at **nine band-limited levels**, one per octave of pitch. The oscillator reads the level whose
  highest harmonic stays below the sample rate minus 19 kHz, so anything that folds back lands above 19 kHz.
  Every frame is normalised to the same loudness, so Morph changes the tone, not the volume.
- **Morph** is smoothed (about 15 ms for knob moves) and interpolated every sample, so sweeping it, even with a
  fast LFO, does not zipper.
- **Warp** comes after the table. The oscillator picks a darker table level when a warp speeds part of the
  cycle up, so warps stay mostly alias-free too.
- **Colour** adds Col>Morph 1, Col>Morph 2 and Col>Filter (5 octaves at 100 %) as you turn it up. Set those
  three amounts per sound, then play Colour. It is also a source in the mod matrix.
- **Drift** gives each voice slow random wander: each oscillator's pitch (up to ±12 cents, independently,
  so the two beat against each other), both morphs (±5 %) and the cutoff (±1.5 semitones).
- **Control rate**: envelopes (except the amp), LFOs, the matrix, pitch, filter coefficients and the table
  level are worked out every 16 samples, using fast polynomial exp and tan approximations. Morph, gain and pan
  ramp smoothly in between, and the amp envelope runs every sample.
- **Voices**: up to 16 (the Voices knob, default 12), plus 4 spare slots. A stolen voice fades out over 5 ms
  in a spare slot while the new note starts, so stealing and repeated notes don't click. Mono and Legato use
  one voice with a note stack (last note wins; releasing returns to the held note).

## The tables (17, 32 frames each)

| Table | Morph 0 % → 100 % |
|---|---|
| Basic | sine → triangle → saw → square → narrow pulse (50 % = saw) |
| PWM | square → 4 % pulse |
| Sync | hard-synced saw, slave ratio 1 → 7 |
| Reso | saw through a resonant low pass sweeping up from the fundamental |
| Vowel | A → E → I → O → U (formants of a 130 Hz voice) |
| Formant | one talking formant from 250 Hz to 4 kHz |
| Organ | drawbar registrations: flute → full organ |
| Tine | electric-piano tine: soft → bark → bell |
| Glass | a sine gaining sparse high partials |
| Metal | clangorous sparse partials, more and brighter |
| FM | 2-operator FM, 1:1, index 0 → 7 |
| Fold | a sine into a wavefolder, more and more folds |
| Digital | a sine with fewer steps per cycle and fewer levels |
| Hollow | odd harmonics → saw → fundamental plus even harmonics |
| Brass | a saw opening up, with a brassy bump |
| Spectra | five random spectra, blended |
| Comb | a saw through a moving comb (phaser and flanger colours) |

Wavetables play their frames at the note's pitch, so formants (Vowel, Formant, Brass) move with the note
rather than staying put like a real voice. Keep them around the middle of the keyboard for the most natural
vowels.

## The panel

A custom panel drawn over Groundswell's cuttlefish art. Every page has four banks of four controls: a column of
four in the dark water on each side, and two rows of four along the seabed, each row headed at the far left. The
cuttlefish, the title and the wavetable frames stay clear. Q-Link column 1 is the left column, column 2 the right,
column 3 the upper row and column 4 the lower row.

The knobs:
- teal arcs for ordinary controls;
- violet arcs that grow from 12 o'clock for signed amounts (tuning, Col> amounts, Env Amt, matrix Amounts);
- rotary selectors with a dot for each choice for menus (Table, Div, Shape, Source and so on);
- a lit RESET pill for Phase.

In Program mode the Q-Links play Colour, Morph 1, Morph 2, Warp 1, Cutoff, Reso, Env Amt, Drift, Attack,
Decay, Sustain, Release, Detune, Glide, Delay and Reverb.

`art/make_skin.py` renders `skins/` from `art/plate-src.webp`. Run it **before** `tools/build.py`, because the
build copies `skins/` as it is and does not re-render. The script refuses to run if the parameter list no
longer matches the plugin. `ui.json` (the stock cyan kit) is only the fallback layout.

## Parameters and pages

Pages, in the order they appear (the panel arranges each as four banks, as above):

**OSC**: Table 1, Morph 1, Warp Mode 1, Warp 1 / Oct 1, Semi 1, Fine 1, Level 1 / the same for oscillator 2.
- **Warp modes**:
  - **Bend** pushes the cycle's phase with a smooth curve, a gentle brightening lean.
  - **Squash** is Casio-style phase distortion: the first half of the cycle is squeezed in, which gives a
    resonant sweep on a sine. Its corners are rounded so it aliases far less.
  - **Sync** is a hard-sync sweep (slave up to 8×, windowed at the reset).
  - **FM**: on oscillator 1, phase modulation by oscillator 2's output, which keeps running for this even at
    Level 2 = 0. On oscillator 2, phase modulation by a sine at oscillator 1's pitch.

**VOICE**: Sub, Sub Type (Sine −1, Sine −2, Tri −1, Sqr −1), Noise, Noise Tone / Unison (1–4), Detune (up to
±60 cents), Width, Drift / Play Mode (Poly, Mono, Legato), Glide (constant time; in Legato only between
overlapping notes; in Poly each new note glides from the last one), Bend Range, Voices / Phase (Free: random
start each note; Reset: fixed start for tight attacks), Output.

**MOTION**: Colour, Col>Morph 1, Col>Morph 2, Col>Filter / M Attack, M Decay, M Sustain, M Release (the
morph envelope) / LFO1 Rate, LFO1 Div, LFO1 Shape, LFO1 Mode / the same for LFO 2.
- **Shapes**: Sine, Triangle, Saw Up, Saw Down, Square, S&H, Smooth Rnd.
- **Modes**:
  - **Free**: one LFO for all voices, at Rate.
  - **Retrig**: each note has its own, starting with the note.
  - **Sync**: one LFO at Div, locked to the song position while the MPC plays, so it lands on the same
    beat every time. The random shapes repeat with the song too. When stopped it runs at the tempo.
  - **Sync Rtg**: each note has its own at Div, starting with the note.
- Bar divisions assume 4/4.

**MOD**: Src 1–4, Dst 1–4, Amt 1–4 (bipolar), then Env>Morph (the morph envelope's depth on both morphs) and
LFO1 Fade (LFO 1 fades in over this time after each note: delayed vibrato).
- **Sources**: LFO 1, LFO 2, Mod Env, Filt Env, Amp Env, Velocity, Mod Wheel, Aftertouch (channel or poly),
  Key, LFO1 x MW (vibrato on the wheel), Random (per note), Colour.
- **Destinations**: Morph 1, Morph 2, Morph 1+2, Warp 1, Warp 2, Cutoff (±5 octaves), Reso, Pitch, Pitch 2
  (oscillator 2 only), Osc Mix, Pan, Level, Detune, Noise.
- Pitch amounts are curved for fine vibrato: 10 % = 0.12 semitone, 50 % = 3 semitones, 100 % = an octave.
- The default slot 1 is LFO1 x MW → Pitch at 15 %, so the mod wheel gives vibrato.

**FILT/AMP**: Cutoff, Reso, Drive, Filter (mode) / Env Amt (±5 octaves), Key Track, Vel>Filter, Vel>Level /
F Attack, F Decay, F Sustain, F Release / Attack, Decay, Sustain, Release.

**FX**: Chorus, Ch Rate, Ch Depth, Ch Mode (Chorus, Ensemble, Flange) / Delay, Dly Time (1/32 to 1/2, dotted
and triplet, at the song tempo), Dly Fdbk, Dly Tone / Reverb, Rev Size, Rev Tone, Rev Pre. These are
deliberately light. Seaglass, Undertow and the coming Whirlpool are the real ones.

Mod wheel (CC1), sustain pedal (CC64), pitch bend, channel and poly aftertouch, and all-notes-off are
handled.

## Programs (16)

Init (a plain saw), Ink Cloud Pad, Chromatophore Lead, Reef Pluck, Deep Ink Bass, Tidal Keys, Colour Shift
(the evolving one: song-locked LFOs walk both morphs), Siren Vowels, Glass Bells, Sunken Organ, Kelp Brass,
PWM Strings, Pixel Pluck, Wobble Deep (song-locked 1/8 wobble), Phaser Haze, Inkjet Funk (phase-distortion
clav). Turn **Colour** on any of them.

## Demo

`build/cuttlefish-demo.wav` (168 s at 100 BPM) is rendered by the test (below). It plays every program in turn,
then holds a chord on Colour Shift while Colour goes 0 → 100 → 0 over 24 s (from 139.8 s). Each program
starts at: Init 0.2 s, Ink Cloud Pad 7.8, Chromatophore Lead 20.2, Reef Pluck 25.9, Deep Ink Bass 33.7, Tidal
Keys 39.4, Colour Shift 47.4, Siren Vowels 60.3, Glass Bells 71.1, Sunken Organ 81.0, Kelp Brass 88.5, PWM
Strings 96.2, Pixel Pluck 107.3, Wobble Deep 114.9, Phaser Haze 120.6, Inkjet Funk 132.3.

## CPU

Measured on a desktop (Apple Silicon), 16 voices held, 128-sample blocks:

| Patch | Desktop core | Per voice |
|---|---|---|
| one oscillator, unison off, no effects (like Init) | 0.75 % | 0.05 % |
| two oscillators + sub + noise + sync warp + drive, all effects | 0.93 % | 0.06 % |
| same, unison 2 | 1.3 % | 0.08 % |
| same, unison 4 (the worst case) | 1.75 % | 0.11 % |

The MPC is roughly 10–20 times slower than this desktop, **so expect about 0.5–1 % of an MPC core per voice at
unison 1, and up to 2 % at unison 4**. That means a few percent for typical playing, 8–18 % with all 16
voices sounding, and up to about 30 % for 16 voices of 4-way unison. These are estimates: Cuttlefish has not
been timed on the hardware yet. If the MPC struggles, lower **Voices** (pads with unison 3 ship at 10) or
Unison. The effects cost about as much as one voice.

## Tests

`tests/rendertest.cpp` drives the plugin directly and checks (72 checks, all passing):

- **Tables**: finite, loudness-matched, no DC, each level holds only its harmonics.
- **Morph**:
  - a 4 s knob sweep adds no between-harmonic (zipper) energy beyond what still frames have;
  - a 7 Hz LFO on Morph would put a −34 to −42 dB zipper at the control rate if the morph were stepped, and
    the per-sample ramp removes it;
  - a 0 → 100 % knob jump does not click.
- **Aliasing**:
  - every table, C6–C8, morph 0/50/100: at or below −68 dB (a naive saw: −12 dB);
  - warps on C5/C6 at 30–100 %: Bend ≤ −52 dB, Squash ≤ −45 dB, Sync ≤ −55 dB, FM (sine on sine) ≤ −60 dB.
- **Tempo sync**: phase within 0.001 beat of the song position, follows relocation, correct period when
  stopped, Sync Rtg restarts per note.
- **Voices**: the voice limit holds; stealing, repeated keys and mono retriggers don't click (with a
  self-test that an injected step is caught).
- **Glide**: legato and mono timing (200 ms), no glide on detached legato notes, return to the held note,
  poly glide.
- **Motion**: Colour, Drift, velocity → morph, the morph envelope, mod wheel → pitch, pitch bend.
- **Safety**: no NaN/Inf or denormals on any program, under random knob moves on every block, or in 40 s
  feedback tails.
- **Levels**: every program's single-note peak is between −16 and −7 dBFS; a velocity-127 four-note chord
  peaks at −0.7 dBFS or less.
- **CPU**, as in the table above.

```
mkdir -p tests/build
clang++ -std=c++17 -O2 -I../../framework tests/rendertest.cpp -o tests/build/rendertest
tests/build/rendertest build/cuttlefish-demo.wav
```

Also run the SDK's host simulator: `.sdk-tools/host_sim plugins/Cuttlefish/build/native/cuttlefish.dylib test`
(19 passed, 0 warnings, 0 failures).

## Not implemented (yet)

- You can't draw or load your own wavetables. The 17 tables are fixed (generated in code).
- Unison applies to both oscillators together, and the sub and noise are never unisoned.
- The LFOs can't modulate each other's rate. There is no LFO delay for LFO 2 (only LFO1 Fade).
- **Aliasing limits:**
  - FM with a bright modulator (oscillator 2 on a saw) at high depth aliases audibly on high notes, like any
    digital FM. Use a sine-like table on oscillator 2 for clean FM.
  - Squash and Sync at full depth on notes above C6 can alias a little (around −45 dB).
- Formant tables move with the pitch (see above).
- Mono note priority is last-note only.
- Not yet tried on MPC hardware.

## Project layout

- `plugin.json`: manifest (uid GqJ3; must match the code).
- `src/Cuttlefish.cpp`: parameters, voices, modulation, filter, rendering.
- `src/Wavetables.h`: table generation (FFT, band-limited mip levels).
- `src/Fx.h`: chorus, delay, reverb.
- `src/Programs.h`: the built-in programs.
- `art/`: the plate art, the shared knob and pill sources, and `make_skin.py`; `skins/` is its output.
- `ui.json`: the fallback stock-kit layout (unused while `skins/` exists).
- `tests/rendertest.cpp`: the render test and demo.

## Licence

MIT. All DSP is original. The state-variable filter is the well-known trapezoidal (Cytomic/Simper) topology,
written fresh here. The formant frequencies are textbook averages for adult voices.
