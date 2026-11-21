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
| [Krill](Krill/) | Instrument: an old-school chip synth — stepped pulse, 4-bit triangle and LFSR noise, ring mod and sync, bit and sample-rate crunch, a lo-fi talking voice, the 50/60 Hz one-finger chord arpeggio and tempo arps, 16 pitch-sweep drums on the pads, tracker-style vibrato and glide; 8 voices, 8 programs, custom skin | Original design | MIT |
| [Wipeout](Wipeout/) | Effect: an audio mangler locked to the song — fills, glitches, breaks, ratchets, rolls, timewarps, reverses and transitions: on a grid it rolls the dice and repeats a slice (1/4 to 1/32 and triplets), reversed or at half speed, or tape-stops or gates it, with Roll, Vary, Hold and a pattern lock for the Q-Links; events up to 4 bars on grids up to 8 bars for transitions; 8 programs, custom skin | Original design | MIT |
| [Abyss](Abyss/) | Instrument: an evolving drone and pad — hold a note and it opens from a sine (or sinks from bright) into a deep, moving pad: 24 drifting sine partials per voice with a wandering spectral bloom, stretch and hollow, detuned twins, sub, an interval layer and shimmer, air, a slow motion cycle in four shapes (free or locked to the song), Orbit (each note's halves circling each other) and a built-in FDN reverb; 6 voices, 16 programs, custom skin | Original design | MIT |
| [Seaglass](Seaglass/) | Effect: a lush stereo reverb you can freeze — Freeze holds the tail as an endless pad and releases it without a click, Add layers note after note onto it (a frozen chord) while the live reverb keeps playing beside it; Shimmer pitch-shifts the tail inside the feedback loop by any interval up to two octaves either way, or by a few cents for a drifting wash, so it climbs and sparkles; Swell ducks the tail while you play and lets it bloom back in the gaps; 13 programs, custom skin | Original design (reverb from Abyss) | MIT |
| [Hammerhead](Hammerhead/) | Instrument: struck, plucked, bowed and blown physical modelling built around the attack — marimba, vibraphone, xylophone and glockenspiel bars, kalimba, bell, glass and steel pan (an 8-mode resonator bank with a tuned tube) and plucked or hammered strings (a waveguide); mallet hardness, strike position and click, decay, damping, mute and pedal, a pitch drop for log drums and toms, vibraphone motor, wooden body and room; 1.1 added Bow (stick-slip friction) and Blow (a breath jet) on every model, with pressure, pad aftertouch, swell and vibrato; 1.2 added Buzz (bottle caps, balafon gourds), an LFO (free or synced) on strike position, hardness, stretch and pressure, and per-hit randomness; 16 voices, 20 programs, custom skin | Original design | MIT |
| [Low Tide](Low%20Tide/) | Effect: tape with a memory and a lifespan — lo-fi that moves with the song: a tape loop of 1–8 bars dubbed again on every pass (each generation duller, hissier, softer and wobblier, up to a cap; Fresh snaps it back; Memory lets old passes linger and crumble), one Wear macro swept by a Tide cycle of 1–32 bars, gentle tape accidents on the beat (dropouts, motor drag, slips, crinkle, channel cuts), an old-sampler resample crunch that follows the record pitch, print-through, azimuth drift, breathing hiss, wow and flutter; 8 programs, custom skin | Original design | MIT |

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
