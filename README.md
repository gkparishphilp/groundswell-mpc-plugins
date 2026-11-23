# Groundswell MPC plugins

Open-source instrument and effect plugins for Akai MPC standalone hardware (MPC Live, X, One, Key, Force and
others), built with the MPC Plugin SDK. Each plugin's README says what it does, what is not implemented, and what
it costs in CPU; anything borrowed is credited there with its licence.

| Plugin | Kind | Based on | Licence |
|---|---|---|---|
| [Undertow](Undertow/) | Effect: dub delay with filter, FX, pitch shift, reverse and modulation in the feedback loop; custom skin | Original design | MIT |
| [Tidepools](Tidepools/) | Effect: granular call-and-response — listens to a phrase, waits for the beat, answers it through one of seven grain engines; pitch tracking, 21 programs, custom skin | Original design | MIT |
| [Fathom](Fathom/) | Effect: a filter that breathes with the music — two filters (wah, LP, BP, HP, notch, vowel; in series, or in parallel and panned apart) moved by a slow envelope follower, two tempo-synced LFOs and a swelling transient envelope, with inertia, drift and a Q-Link pedal; 23 programs | Original design | MIT |
| [Bearings](Bearings/) | Effect: places a sound on a plane in front of you (pan, distance, head cues, room, a touch of Doppler) and moves it round song-locked paths — circle, figure 8, pendulum, ping-pong, drift, tide, step and scatter on hits — with a loudness follower, a Throw control and a three-band split; 18 programs | Original design | MIT |
| [Whalesong](Whalesong/) | Effect: an accompanist — infers the key from what you play by pitch-class histogram, then plays along on its own synth as a bass, lead, counter-melody, pad or arpeggio | Original design | MIT |
| [Largemouth](Largemouth/) | Effect: a bass player that listens to your drums — learns the kick pattern and plays a bassline on it (Sub, 808, Pluck, Acid, Reese), on the chord it hears or a progression in the key; 8 programs | Original design | MIT |
| [Krill](Krill/) | Instrument: an old-school chip synth — stepped pulse, 4-bit triangle and LFSR noise, ring mod and sync, bit and sample-rate crunch, a lo-fi talking voice, the 50/60 Hz one-finger chord arpeggio and tempo arps, 16 pitch-sweep drums on the pads, tracker-style vibrato and glide; 8 voices, 30 programs, custom skin | Original design | MIT |
| [Wipeout](Wipeout/) | Effect: an audio mangler locked to the song — fills, glitches, breaks, ratchets, rolls, timewarps, reverses and transitions: on a grid it rolls the dice and repeats a slice (1/4 to 1/32 and triplets), reversed or at half speed, or tape-stops or gates it, with Roll, Vary, Hold and a pattern lock for the Q-Links; events up to 4 bars on grids up to 8 bars for transitions; 8 programs, custom skin | Original design | MIT |
| [Abyss](Abyss/) | Instrument: an evolving drone and pad — hold a note and it opens from a sine (or sinks from bright) into a deep, moving pad: 24 drifting sine partials per voice with a wandering spectral bloom, stretch and hollow, detuned twins, sub, an interval layer and shimmer, air, a slow motion cycle in four shapes (free or locked to the song), Orbit (each note's halves circling each other) and a built-in FDN reverb; 6 voices, 16 programs, custom skin | Original design | MIT |
| [Seaglass](Seaglass/) | Effect: a lush stereo reverb you can freeze — Freeze holds the tail as an endless pad and releases it without a click, Add layers note after note onto it (a frozen chord) while the live reverb keeps playing beside it; Shimmer pitch-shifts the tail inside the feedback loop by any interval up to two octaves either way, or by a few cents for a drifting wash, so it climbs and sparkles; Swell ducks the tail while you play and lets it bloom back in the gaps; 13 programs, custom skin | Original design (reverb from Abyss) | MIT |
| [Hammerhead](Hammerhead/) | Instrument: struck, plucked, bowed and blown physical modelling built around the attack — marimba, vibraphone, xylophone and glockenspiel bars, kalimba, bell, glass and steel pan (an 8-mode resonator bank with a tuned tube) and plucked or hammered strings (a waveguide); mallet hardness, strike position and click, decay, damping, mute and pedal, a pitch drop for log drums and toms, vibraphone motor, wooden body and room; 1.1 added Bow (stick-slip friction) and Blow (a breath jet) on every model, with pressure, pad aftertouch, swell and vibrato; 1.2 added Buzz (bottle caps, balafon gourds), an LFO (free or synced) on strike position, hardness, stretch and pressure, and per-hit randomness; 16 voices, 20 programs, custom skin | Original design | MIT |
| [Low Tide](Low%20Tide/) | Effect: tape with a memory and a lifespan — lo-fi that moves with the song: a tape loop of 1–8 bars dubbed again on every pass (each generation duller, hissier, softer and wobblier, up to a cap; Fresh snaps it back; Memory lets old passes linger and crumble), one Wear macro swept by a Tide cycle of 1–32 bars, gentle tape accidents on the beat (dropouts, motor drag, slips, crinkle, channel cuts), an old-sampler resample crunch that follows the record pitch, print-through, azimuth drift, breathing hiss, wow and flutter; 8 programs, custom skin | Original design | MIT |
| [Breakwater](Breakwater/) | Effect: a guitar amp for clean, chimey tones and the edge of breakup — American, British and Boutique voices; a 2x-oversampled preamp that stays clean when you play softly and hairs up when you dig in; the classic passive bass/middle/treble tone stack, modelled from the circuit so the knobs interact; Bright, Chime (presence and class-A shimmer), Sag and Master; filter-based cabinets (1x12 and 2x12 open, 4x12 closed, Direct) with Mic position and Distance; a drippy two-spring reverb (Dwell, Tone, Mix); a smooth, opto or harmonic tremolo, free or locked to the song; voiced to follow Fathom; 8 programs, custom skin | Original design; tone stack after Yeh & Smith (DAFx-06) | MIT |
| [Driftwood](Driftwood/) | Instrument: one pad, one chord — set a key and scale and every note plays the chord on that degree (triad, 7th, 9th, sus2, sus4, add9; close, open or spread voicing, inversions, or voice leading that moves the fewest notes near a chosen register; a bass note an octave or two down), on a Keys map (with snap or borrowed chords for out-of-key notes) or a Pads map that puts every chord in the key across the 16 pads; strummed down, up, alternating or by velocity, faster when hit harder, with loose timing and velocity, re-strummed by tempo-locked rhythm-guitar patterns (down and up strokes, muted chucks, offbeat chops, swing), or arpeggiated (up, down, up-down, random or a fingerpicked thumb, 1/4 to 1/32 with triplets, over 1–3 octaves, with gate), all running free when the transport is stopped; Hold; its own warm voices (FM electric-piano keys, a soft pad, a waveguide plucked string, a drawbar organ) with tone, filter, chorus, a small room, tape warmth, wobble and dust; 16 voices, 10 programs, custom skin | Original design (string loop from Hammerhead) | MIT |
| [Conch](Conch/) | Effect: tuned resonators that ring at the chord or the key, like sympathetic strings — drum hits come out as pitched, in-key tones and a mix gets a singing halo: String (comb-filter strings), Bell (three inharmonic modes per note) and Pipe (odd harmonics, hollow) over up to 16 notes with Decay, Bright, Spread and Width; Hits rings them from transients only so held notes don't wash them out, with In HPF, Drive and Duck; notes from Listen (hears the key and the current chord, using Key/Scale until it is sure), Key, a hand-set Chord or a progression locked to the song; click-free chord changes that let the old chord ring out, Freeze, Shimmer and Tune; 8 programs, custom skin | Original design (key and chord analysis from Largemouth) | MIT |
| [Plankton](Plankton/) | Effect: a granular texture engine for live input — catches the track in tiny grains and drifts them into clouds: Size, Density, Position and Spray, Pitch with Scatter (free or snapped to octaves, fifths or a pentatonic scale), Reverse, a window Shape from smooth to struck, and Spread; Freeze and a hold-to-Catch button that capture the moment and let it go without a click, with Linger (a release: the caught cloud stays a while, then gives way to the live one grain by grain); Tail, which keeps the cloud playing what you just played after you stop and fades it out over a time you set; Drift (a slow wander of position and pitch so a frozen cloud keeps evolving), guarded Feedback, an allpass Smear and Tone; grains and Drift locked to the song with Sync, free when stopped; up to 48 grains; 8 programs, custom skin | Original design | MIT |

## Installing on an MPC

1. Copy the `Groundswell - VST - <Name>` folder into a folder named `VST2 Plugins` on a drive the MPC can see.
   The internal drive is the safest choice: removable drives are often mounted without permission to run plugins.
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC. The plugin is listed under **External Plugins** in the plugin browser.

No modification of the MPC is needed: this is the official route for third-party plugins.

## Building from source

Each folder is a project for the MPC Plugin SDK. Inside a checkout of the SDK, place this repository at `plugins/`
and run:

```
python tools/build.py plugins/<Name>
```

This compiles the plugin for 32-bit and 64-bit ARM MPCs (and natively, for testing) and writes the
`Groundswell - VST - <Name>` folder to `plugins/<Name>/build/`.

## Licence

MIT unless a plugin's README says otherwise; see [LICENSE](LICENSE). Ported code keeps its original copyright
notice in the plugin's README.
