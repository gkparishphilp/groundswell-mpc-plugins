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
  synced) and a built-in FDN reverb. For ambient beds and intros. 1.1 added Orbit (each note's
  halves circling each other), Stretch, Hollow, interval choices, motion shapes and Sink. Still to
  come: a reverb freeze and a true shimmer tail (shared ground with Sea Glass).
- **Seaglass**: a lush reverb you can freeze. Freeze holds the tail as an endless pad and lets it
  go without a click; Shimmer pitch-shifts the tail in the feedback loop (octave, fifth, two
  octaves or an octave down) so it rises and sparkles; Swell ducks the tail while you play and
  lets it bloom back in the gaps, the way Tidepools waits for you. Built on Abyss's FDN reverb.
  1.1 added Add (layer note after note onto the frozen pad: a frozen chord) and Live (the normal
  reverb keeps playing beside the pad). 1.2 added Shift (the shimmer by any interval up to two
  octaves either way, or by cents for a drifting wash). Still to come: a Size control, per-layer
  release, and an on-screen hold-to-freeze button.
- **Hammerhead** (was "Kelp"): struck and plucked physical modelling, built to be percussive:
  marimba, vibes, xylophone and glock bars, kalimba, bell, glass, steel pan (8-mode resonator bank
  plus a tuned tube) and plucked or hammered strings (waveguide). Mallet hardness, strike position,
  click, decay/damping, mute and pedal, a pitch drop for log drums and toms, motor, body and room.
  16 voices, cheap enough for chords. Custom shark panel. 1.1 added Bow and Blow
  exciters on every model (pressure, aftertouch, swell, vibrato); 1.2 added Buzz, an LFO on
  position, hardness, stretch and pressure, and per-hit randomness.
- **Low Tide**: tape with a memory and a lifespan: lo-fi that moves in time with the song, unlike a
  static colour box. A tape loop of 1-8 bars is dubbed again on every pass (each generation duller,
  hissier, softer and wobblier, up to a cap; Fresh snaps it back; Memory re-records old passes), a
  Wear macro swept by a Tide cycle of 1-32 bars, gentle tape accidents on the beat (dropouts, drag,
  slips, crinkle, channel cuts), an old-sampler resample crunch that follows the record pitch,
  print-through, azimuth drift, breathing hiss, wow and flutter. Custom cassette panel.
- **Breakwater**: a guitar amp for clean, chimey tones and the edge of breakup, voiced to follow
  Fathom. American, British and Boutique voices; an oversampled preamp that stays clean when you play
  softly; the classic passive tone stack (the knobs interact); Bright, Chime, Sag and Master; filter
  cabs (1x12, 2x12, 4x12, Direct) with Mic and Distance; a drippy two-spring reverb; a smooth, opto
  or harmonic tremolo locked to the song. Mono amp, stereo spring. Custom amp-head panel.
- **Pressure**: a compressor and saturator with a lot of character. One Depth macro squeezes
  threshold, ratio, saturation and density together, loudness-matched; four Hulls (Glass optical,
  Bite transistor, Valve variable-mu, Bus console glue); Implode for crushed, exploding drum rooms;
  Tape/Tube/Iron saturation that growls harder as it compresses; Tide release in ms or note values
  with Bloom; Blend for parallel smash, a sidechain high-pass, Link, Mid/Side and a Breath noise
  floor. Custom porthole panel; the caged lamp burns brighter with the gain reduction.
- **Driftwood**: one pad, one chord. Set a key and scale and each note plays the chord on that
  degree (triad to 9th, sus, add9; close, open or spread; voice leading near a register; bass), on
  a Keys map or a 16-pad degree map. Strum with direction, velocity speed and humanize, or let a
  tempo-locked rhythm-guitar Pattern re-strum it (down/up strokes, mutes, chops, swing), or
  arpeggiate it (up, down, up-down, random, fingerpicked thumb; 1-3 octaves, gate). Its own voices:
  FM keys, pad, plucked string, organ; tape warmth, wobble, dust, chorus and a small room; Hold.
  Custom driftwood-guitar panel. Still to come: per-degree chord overrides, a pattern editor, 3/4.
- **Conch** (was "Echolocation"): a bank of tuned resonators, like sympathetic strings, that rings
  at the chord or the key: drum hits come out as pitched, in-key tones, a mix gets a singing halo.
  String, Bell and Pipe (odd-harmonic, hollow) materials, up to 16 notes; Hits rings them from
  transients only so held notes don't wash them out; Duck, Drive, In HPF. Notes from Listen (hears
  the key and chord, Largemouth's analysis; Key/Scale until sure), Key, a hand-set Chord or a
  progression locked to the song. Click-free chord changes (three banks, old chords ring out),
  Freeze, Shimmer, Tune. Custom glowing-conch panel. Still to come: capturing new input into a frozen
  chord.
- **Plankton**: a granular texture engine for live input. Catches the track in tiny grains (up to 48)
  and drifts them into clouds: Size, Density, Position, Spray, Pitch, Scatter snapped to octaves,
  fifths or a scale, Reverse, window Shape and Spread; Freeze plus a hold-to-Catch button with a
  Linger release; an automatic Tail that keeps the cloud playing after you stop; Drift, guarded
  Feedback, an allpass Smear and Tone; grains and Drift locked to the song with Sync. Custom
  plankton panel. Still to come: anti-aliasing for big upward shifts, a scan/speed control.
- **Urchin**: the nasty one, a spiky fuzz and distortion box (Breakwater is clean, Low Tide crunchy).
  Six Spines, each its own circuit: a starved, sputtering Gated Fuzz, an Octave Fuzz, a Torn
  Speaker that bottoms out and buzzes, a half/full-wave folding Rectifier, a West Coast Wavefolder
  and a sane Overdrive; Bias is each one's character control. Spike takes everything from mild to
  ruined on one Q-Link; Bloom sag, a Gate that chops, Focus pre-emphasis, a Tone tilt, Blend and Low
  Keep for bass and kicks. 2x oversampled with anti-aliased (ADAA) shapers and an automatic level
  match; Mono core option. Custom panel from the user's urchin art. 1.1 added Split (a different
  Spine on each side, like two amps panned apart) with Width, and made Starve properly audible.
- **Cuttlefish**: changes colour, so it changes timbre. The family's everyday wavetable synth: two
  oscillators morphing through 17 generated, band-limited tables (analog shapes, PWM, sync, resonant
  sweep, vowels, formant, organ drawbars, EP tine, glass, metal, FM, fold, digital, hollow, brass,
  random spectra, comb), each warped by Bend, Squash (phase distortion), Sync or FM; unison, sub and
  noise; a morph envelope, two LFOs (free or locked to the song, smooth random), a 4-slot matrix,
  Drift and one Colour macro for the Q-Links; multimode filter with drive; poly, mono and legato
  glide; light chorus, ping-pong delay and reverb. 16 voices. Custom panel from the user's cuttlefish
  art. 1.1 added 25 programs (41: leads, pads, keys, basses, textures). Still to come: drawn or user
  tables, per-oscillator unison.

## Mixing and character effects

- **Swell**: a volume shaper locked to tempo, with pumping, gating and trance-chop shapes. The
  "ducked by the kick" sound without sidechain routing.

## Instruments

- **Current**: a generative sequencer that plays its own synth. Play a note to set the key and it
  improvises patterns that change step by step. (Plugins can't send MIDI to the MPC, so a
  sequencer has to be an instrument that makes its own sound.)
