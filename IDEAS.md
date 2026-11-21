# Groundswell plugin ideas

A backlog of plugins to build. Move an idea to "Started" when a project folder exists.

## Started

- **Largemouth** (was "Pilot Fish"): listens to your drums and plays a bassline that locks to the
  kick, in the key it hears. Its own sub/808 voice, with controls for how busy it plays, octave
  jumps and glide. Whalesong for the low end.
- **Krill**: tiny, everywhere, and chirpy. An old-school home-computer / game-console instrument:
  stepped pulse, 4-bit triangle and LFSR noise, ring mod and sync, bit and sample-rate crunch, a
  lo-fi talking voice, the frame-rate chord arpeggio plus tempo arps, 16 pitch-sweep drums on the
  pads, and tracker-style vibrato and glide. Still to come: duty sweeps and drawable tracker tables.

## Effects that listen and respond

- **Echolocation**: a bank of tuned resonators (like sympathetic strings) that rings at the chord
  it detects. Put it on a drum loop and the hits come out as pitched, in-key tones. Cheap on CPU:
  about 8–16 simple filters.
- **Sea Glass**: a reverb you can freeze. Freeze holds the current sound as a pad. Other modes:
  shimmer (adds an octave up) and swell, which brings the tail in after you stop playing, the way
  Tidepools waits for you.
- **Riptide**: a stutter/repeat effect locked to tempo, with chance controls: repeats, reverse,
  half speed, tape stop and gate rolls. Built for playing live on the Q-Links. Undertow handles
  echoes; this cuts the audio up.

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
- **Abyss**: hold one note and it slowly evolves into a drone or pad (additive, with slow drift).
  Good for ambient beds and intros.
- **Current**: a generative sequencer that plays its own synth. Play a note to set the key and it
  improvises patterns that change step by step. (Plugins can't send MIDI to the MPC, so a
  sequencer has to be an instrument that makes its own sound.)
