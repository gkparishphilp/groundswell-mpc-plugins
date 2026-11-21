# Groundswell plugin ideas

A backlog of plugins to build. Move an idea to "Started" when a project folder exists.

## Started

- **Largemouth** (was "Pilot Fish"): listens to your drums and plays a bassline that locks to the
  kick, in the key it hears. Its own sub/808 voice, with controls for how busy it plays, octave
  jumps and glide. Whalesong for the low end.
- **Wipeout** (was "Riptide"): an audio mangler locked to tempo, for fills, glitches, breaks,
  ratchets, rolls, timewarps, reverses and transitions, by chance or by hand: repeats (1/4 to 1/32
  and triplets), reverse, half speed, tape stop and gate rolls, plus Roll, Hold and a pattern lock.
  Built for playing live on the Q-Links. Undertow handles echoes; this cuts the audio up.
- **Krill**: tiny, everywhere, and chirpy. An old-school home-computer / game-console instrument:
  stepped pulse, 4-bit triangle and LFSR noise, ring mod and sync, bit and sample-rate crunch, a
  lo-fi talking voice, the frame-rate chord arpeggio plus tempo arps, 16 pitch-sweep drums on the
  pads, and tracker-style vibrato and glide. Still to come: duty sweeps and drawable tracker tables.
- **Abyss**: hold one note and it slowly evolves into a drone or pad. Additive: 24 drifting sine
  partials per voice open from a sine into a deep pad over an Evolve time, with a wandering
  spectral bloom, detuned twins, sub, fifth and shimmer layers, air, a slow motion cycle (free or
  synced) and a built-in FDN reverb. For ambient beds and intros. Still to come: a reverb freeze
  and a true shimmer tail (shared ground with Sea Glass).

## Effects that listen and respond

- **Echolocation**: a bank of tuned resonators (like sympathetic strings) that rings at the chord
  it detects. Put it on a drum loop and the hits come out as pitched, in-key tones. Cheap on CPU:
  about 8–16 simple filters.
- **Sea Glass**: a reverb you can freeze. Freeze holds the current sound as a pad. Other modes:
  shimmer (adds an octave up) and swell, which brings the tail in after you stop playing, the way
  Tidepools waits for you.

## Mixing and character effects

- **Pressure**: a one-knob processor for a whole mix or bus. Turning it up adds compression and
  tape saturation together.
- **Low Tide**: makes audio sound old: wobbly tape, vinyl crackle, hiss, a narrower frequency
  range and an age control.
- **Swell**: a volume shaper locked to tempo, with pumping, gating and trance-chop shapes. The
  "ducked by the kick" sound without sidechain routing.

## Instruments

- **Kelp**: physical modelling of plucked, struck and bowed sounds (strings, kalimba, marimba,
  glass). Cheap enough on CPU to play chords on the MPC.
- **Driftwood**: a chord instrument. One pad plays a full chord in your key, with voicing, strum
  and humanize controls plus a little tape wobble.
- **Current**: a generative sequencer that plays its own synth. Play a note to set the key and it
  improvises patterns that change step by step. (Plugins can't send MIDI to the MPC, so a
  sequencer has to be an instrument that makes its own sound.)
