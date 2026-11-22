# Krill

**Tiny, everywhere and chirpy.** An old-school chip instrument for Akai MPC standalone hardware, by Groundswell.

Krill makes the sounds of 8-bit home computers and game consoles: buzzy pulse waves with stepped duty
cycles, a grainy 4-bit triangle, LFSR noise, ring mod and hard sync, crunchy bit and sample-rate
reduction, and a lo-fi talking voice for robot syllables. Its signature is the fast chord arpeggio:
hold one note and it trills through the whole chord at the 50 or 60 Hz "frame" rate. There are tempo-synced
arps too, pitch-sweep drums on the pads (noise hits, laser zaps, blips), and vibrato and glide that move in
steps like a tracker.

## Install

1. Copy the `Groundswell - VST - Krill` folder (in `build/` after building, or from the release zip) into a
   folder named `VST2 Plugins` on a drive the MPC can see. The internal drive is the safest choice.
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC and load Krill from **External Plugins** onto a plugin program.

## How it plays

- **Play Mode.** **Synth**: every note plays the synth. **Drums**: every note plays one of 16 drums.
  **Split**: notes below C2 (MIDI 48) are drums, the rest play the synth, so one program can hold a kit
  and a bassline.
- **Voices.** **Poly** gives 8 voices. **Mono** is one legato voice: a new key while another is held glides
  (if Glide is up) without restarting the envelope, and letting go returns to the key still held.
- **The frame clock.** Glide, vibrato, the chord arp and the talking voice all move once per frame,
  50 or 60 times a second (**Frame Rate**), the way old console music drivers updated once per video frame.
  That stepping is the sound: glides are staircases and vibrato wobbles in little jumps.

### Arp

| Arp | What it does |
|---|---|
| Off | Notes play as they are. |
| Chord | Each held note cycles through **Chord** on its own, one step every **Arp Speed** frames (1 = fastest, 50 or 60 steps a second). Polyphonic: hold two notes and get two trills. **Arp Octaves** stacks the chord over more octaves. |
| Up, Down, Up-Down, Random | A tempo-synced arpeggiator on one voice, stepping every **Arp Div**. Hold **one** note and it arpeggiates the chord built on it; hold **several** and it plays those notes. **Arp Octaves** repeats them in higher octaves, **Arp Gate** sets how long each step sounds. Locked to the song position while the MPC plays; free-running at the project tempo when stopped. |

Chords: Major, Minor, Maj7, Min7, Sus4, Power (root and fifth), Dim, Octave.

### Drums

In Drums mode the 16 drums repeat every 16 notes, starting at C1 (MIDI 36). In Split mode the drums are
the notes below C2: C1–B1 play Kick to Drop, and C0–B0 play Low Tom to Whoosh.

| Note | Drum | Note | Drum |
|---|---|---|---|
| 36 C1 | Kick (triangle, falling pitch) | 44 G#1 | Metal (short "metallic" noise) |
| 37 C#1 | Snare (triangle + noise) | 45 A1 | Blast (long noise, falling) |
| 38 D1 | Closed Hat | 46 A#1 | Rise (pulse sweeping up) |
| 39 D#1 | Open Hat (cut off by Closed Hat) | 47 B1 | Drop (deep triangle dive) |
| 40 E1 | Low Tom | 48 C2 | Ping (ring-modulated bell) |
| 41 F1 | High Tom | 49 C#2 | Click |
| 42 F#1 | Laser (pulse zap down) | 50 D2 | Zap Up |
| 43 G1 | Blip | 51 D#2 | Whoosh (noise rising) |

If your pads send different notes, any note still plays a drum; set the pad notes in the MPC's
program settings to line them up with this map. Drum Tune, Drum Decay, Drum Sweep and Drum Level
change the whole kit.

### The talking voice

**Talk** blends the voice's sound through three formant filters that step through the phonemes of a
**Word**, one per frame, starting at each note: Ah, Uh-Oh, Wah, Yeah, Wow, Oi, Hey, No, Yes, Robot,
Bleep, Zap and Ee-Oo. The oscillators are the vocal buzz (a narrow pulse sounds most like a speech chip)
and a hiss is added for consonants. **Talk Speed** stretches or squeezes the word, **Formant** shifts the
mouth smaller or larger. Words that end on a vowel hold it while the key is down. Try it with a tempo arp
for robot chants, and with Bits and Resample turned down for the full toy sound.

## Parameters

The panel has four tabs (PLAY, OSC, MOTION, VOX+DRUM) around the krill; each row of controls is one row of Q-Links.
Arp, Chord, Glide and Play Mode appear on two tabs each so they are at hand where you need them.

| Page | Control | What it does |
|---|---|---|
| MAIN | Volume | Master volume. |
| | Bits | Bit depth of the output, 16 (Off) down to 2. |
| | Resample | Sample-rate reduction (sample and hold), Off down to 1 kHz. |
| | Tone | A gentle low-pass on the output, like the RC filter on an old console's audio out. |
| | Play Mode | Synth, Drums or Split. |
| | Voices | Poly (8 voices) or Mono (legato). |
| | Arp, Chord | See **Arp** above. |
| | Attack, Decay, Sustain, Release | The amp envelope. |
| | Step Env | On: the envelope moves in 16 volume steps, like a 4-bit volume register. |
| | Velocity | How much key velocity changes the level. |
| | Glide | Portamento time. The glide moves in equal steps, one per frame. |
| | Bend Range | Pitch-bend range in semitones. |
| OSC | Wave A, Wave B | Pulse, Tri (4-bit, 16 steps), Saw (5-bit) or Noise. Noise is pitched by the note. |
| | Duty A, Duty B | Pulse width: 12.5%, 25%, 37.5% or 50%. |
| | Level A, Level B | Oscillator levels. B is off by default. |
| | Noise Mode | **Long**: a 32767-step sequence, hiss. **Short**: a 93-step loop that rings at the note's pitch, metallic. |
| | Osc Mod | **Ring**: B is flipped by A's square, for bells and clangs. **Sync**: B restarts every cycle of A. **Both**. |
| | B Semi, B Detune | Osc B tuning against A. With Sync, B's pitch sets the timbre while A sets the note. |
| | Sweep, Sweep Time | A pitch sweep at the start of each note. Positive starts above and falls (zaps), negative rises from below. |
| | Sweep To | **Pitch** (both oscillators) or **Osc B** only: with Sync on, that is the classic sync sweep. |
| MOTION | Arp Speed | Frames per chord step (Chord arp). |
| | Frame Rate | 50 Hz or 60 Hz. |
| | Arp Div, Arp Octaves, Arp Gate | Tempo arp step length, octave range and step length. Arp Octaves also widens the Chord arp. |
| | Vib Depth, Vib Speed, Vib Delay | Stepped vibrato that starts after the delay. The mod wheel adds up to half a semitone. |
| VOX+DRUM | Talk, Word, Talk Speed, Formant | See **The talking voice**. |
| | Drum Tune, Drum Decay, Drum Sweep, Drum Level | Pitch, length, sweep depth and level of the whole drum kit. |

## Programs

| Program | Sound |
|---|---|
| Pulse Lead | A 25% pulse lead with delayed stepped vibrato. |
| Chord Chirp | The one-finger major-chord trill at 50 Hz on a thin 12.5% pulse. |
| Minor Arp Run | A 1/16 up-down arp over two octaves; one key plays its minor chord. |
| Sync Sweep Bass | Mono hard-sync bass with a falling sync sweep, glide and 8-bit crunch. |
| Ring Bells | Ring-modulated triangle and pulse, a decaying chime. |
| Talking Toy | Mono talking voice saying "Yeah", 6-bit, 8 kHz, through the Tone filter. |
| Pad Drum Kit | All 16 pitch-sweep drums across the pads, lightly crunched. |
| Split Kit + Bass | Drums below C2, a mono triangle bass above. |
| **Leads** | |
| Duo Lead | A square and a 25% pulse detuned against each other, mono with glide and late vibrato. |
| Laser Lead | Every note zaps in from 12 semitones above; faster vibrato. |
| Sync Scream | Hard-synced saw whose timbre opens over almost a second, with glide. |
| Pulse Whistle | A thin 12.5% pulse through the Tone filter with a wide, quick vibrato. Play it high. |
| **Chord trills and arps** | |
| Maj7 Shimmer | A slower major-seventh trill over two octaves, with a triangle an octave up. |
| Power Trill | Root-fifth-octave on a crunchy 8-bit saw at 60 Hz. |
| Dim Alarm | A diminished trill with fast, deep vibrato: tension. |
| Random Bleeps | A random 1/16 arp over a minor-seventh chord across three octaves, short blips. |
| Triplet Climb | A rising 1/8-triplet sus4 arp over three octaves on a triangle. |
| **Basses** | |
| Octave Pump Bass | A triangle bass bouncing between octaves every three frames. |
| Saw Grind Bass | Saw plus a pulse an octave down, 6-bit and 11 kHz. |
| **Pads and textures** | |
| Chip Pad | A slow, stepped swell of two detuned pulses. |
| Metal Drone | Short "metallic" noise ringing at the note's pitch, over a triangle. |
| **Sound effects** | |
| Pickup Chime | A quick root-fourth-fifth chime that dies away. |
| Power-Up | A rising sweep with a major trill on top. |
| Noise Blast | A falling, crunched noise burst you can pitch from the keys. |
| **Talking** | |
| Robot Chant | "Robot" on every step of a 1/8 minor arp, 8-bit. |
| Wah Bass | A saw bass saying "Wah" with a lowered mouth. |
| Ee-Oo Choir | Slow "Ee-Oo" on detuned pulses: a lo-fi vocal pad. |
| **Kits** | |
| Crunch Kit | The 16 drums at 5 bits and 8 kHz, shorter and with deeper sweeps. |
| Boom Kit | The kit tuned down, longer and with big sweeps. |
| Split Kit + Arp | Drums below C2, a 1/16 minor up-down arp above. |

## CPU

Light. On a desktop the demo below takes about 0.07% of real time, and a deliberate worst case (all 8 voices
held, talking, noise on Osc B, ring and sync, crunch on) about 0.25%. By the ratio seen with the other
Groundswell plugins that should be roughly 0.5–1% of one MPC core for normal playing and around 2–3% for
the worst case. This is an estimate from the desktop, not yet measured on the hardware. The oscillators
are integer phase counters and pitch is recalculated every 32 samples; the formant filters only run while
Talk is above zero.

## What is not in this version

- **Duty-cycle sweeps / PWM**: duty is stepped and fixed per note. A per-frame duty sequence would be next.
- **Per-voice tracker tables** (arpeggio, volume and duty sequences you draw yourself): the chord arp and
  envelopes cover the common cases for now.
- **The talking voice** has a fixed list of 13 words; you can't type your own. It is a cheap three-formant
  sketch of speech, not real LPC resynthesis, so it is more "robot" than intelligible.
- **Sustain pedal** works in Poly only; Mono and the tempo arp ignore it.
- **Output is mono** (the same on both channels), like the machines it imitates. No stereo spread or panning.
- **Aliasing is deliberate**: the waves are naive, so very high notes fold back into gritty tones. That is
  the sound, but it can get harsh above C7.

## Testing

```
python3 plugins/Krill/art/make_skin.py
python tools/build.py plugins/Krill
.sdk-tools/host_sim plugins/Krill/build/native/krill.dylib test --notes 48,60,64,67 --playing
cd plugins/Krill && mkdir -p tests/build
clang++ -std=c++17 -O2 -I../../framework tests/demotest.cpp -o tests/build/demotest && tests/build/demotest
```

`tests/demotest.cpp` plays every program through a short passage and writes `tests/build/krill-demo.wav`
(about two minutes: chord arps, a tempo arp, all 16 drums and a beat, eight spoken words, the sync bass,
the split kit, bells and the lead). It checks that the chord arp steps at the frame rate, the tempo arp
lands 16 steps in a bar of 1/16, every drum sounds and decays, every word is audible, the formants
actually move, and nothing clips. It also times the worst case.

## Project layout

- `plugin.json`: manifest (name, manufacturer, uid). The `uid` must match the C++ `uniqueId`.
- `src/Krill.cpp`: the whole plugin (parameters, MIDI, voices, drums, talking voice, output stage).
- `art/make_skin.py`: draws the panel over `art/plate-src.webp` into `skins/`. Run it **before** `tools/build.py`
  (the build copies `skins/` as it is), and again whenever parameters change.
- `skins/`: the rendered panel (TUI.json, Q-Links.json, backgrounds, knob and button images).
- `ui.json`: a stock-kit fallback layout, unused while `skins/` exists.
- `tests/demotest.cpp`: the offline demo and checks.

## Licence

MIT. Original design and code; the formant frequencies are the standard published averages for English
vowels (Peterson & Barney, 1952), and the noise generator is a generic 15-bit linear-feedback shift register.
