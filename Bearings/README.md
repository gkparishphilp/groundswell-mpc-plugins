# Bearings

Places a sound in front of you and moves it around. Put one on a track (or on a pad, if your MPC offers External
Plugins in a pad's insert slots) and it sets where that sound sits: how far left or right, and how near or far.
It can also send the sound round a path, locked to the song: a circle, a figure 8, a pendulum, a ping-pong, a slow
drift, a tide that comes towards you and goes back, or a step or scatter that jumps on every hit.

An effect, by Groundswell. MIT licence.

## How it works

Bearings sums its input to mono, so there is one clear point to move, and places it on a plane: left to right,
and near to far. Four things make a position sound like a position rather than a pan knob:

- **Ears.** Constant-power panning, but the far ear still hears the sound round your head (12 dB down at the
  side), hears it up to 0.66 ms later, and hears it duller (head shadow). The timing difference is the strongest
  left/right cue there is. These are the same head cues as Tidepools' Swirl.
- **Distance.** Farther away is quieter (up to 18 dB) and has less top end (up to 3 octaves of high cut). The
  **Depth** control sets how strong both effects are.
- **Room.** A small built-in room. The farther away the sound, the more of it reaches the room and the less
  pre-delay the room has: 25 ms when close, 3 ms when far. The ratio of direct sound to room sound is how your
  ear judges distance, so this does a lot of the work. Use it as a hint of space, and keep your main reverb on
  a send.
- **Doppler.** A sound that is farther away arrives later (up to 20 ms at Doppler 100), so moving towards or
  away from you bends the pitch slightly. The bend is capped at about 4% so a sudden jump cannot chirp.

**Mono Safe** turns off the ear timing difference, so a mono speaker cannot hear any comb filtering. Placement
then comes from level and tone only.

### Paths

Every path starts in front of you, in the centre.

| Path | Movement |
|---|---|
| **Fixed** | Stays at Pan / Distance |
| **Circle** | Front, right, behind, left (Counter: left first) |
| **Figure 8** | A loop on each side |
| **Pendulum** | A swing that comes nearest in the middle (Size Y sets how curved the arc is) |
| **Ping-Pong** | Jumps side to side; Glide rounds off the jumps |
| **Drift** | A smooth random wander |
| **Tide** | Straight towards you and away again |
| **Step** | Moves Step Angle further round a circle on every hit |
| **Scatter** | Jumps to a new random spot on every hit |

With **Sync** on, the position comes from the song position. Every Bearings instance with the same Rate and
Phase is in the same place on the same beat, however late it started. Give three tracks Phase 0, 120 and 240
and they circle you in formation (programs *Formation A/B/C*).

Step and Scatter ignore the clock and react to hits instead. The hit detector listens to the input, and
**Hit Sens** sets how readily it fires.

### React

A loudness follower (**Fol Attack**, **Fol Release**, **Fol Gain**) can change three things. **Fol>Size**: louder
playing widens the path (or narrows it, if negative). **Fol>Rate**: louder playing moves it faster (or slower, or
backwards). **Fol>Push**: louder playing pushes it farther away (or pulls it closer). Note that in Sync, Fol>Rate
moves the sound ahead of the beat, so it stops landing on the same spot every bar.

**Throw** is meant for a Q-Link. Turning it up flings the sound outward and back over about 30 ms. When you turn
it down, the sound drifts home over about 1.5 s.

### Split

**Split** cuts the sound into three bands at **Low Xover** and **High Xover**. The crossovers are Linkwitz-Riley,
phase-aligned so the bands add back up flat (the test suite checks this within 0.02 dB). Each band follows the
same path, offset by **Mid Phase** / **High Phase** and scaled by its own **Size**. **Low Size** starts at 0, so
the bass stays still at Pan / Distance; turn it up to let the bass move too. A pad or a drum loop comes apart into
three layers that move separately (program *Prism Pad*).

## Parameters

| # | Name | Range | Default | |
|---|---|---|---|---|
| 0 | Output | -24..+12 dB | 0 | |
| 1 | Mix | 0..100 % | 100 | Dry is your original stereo signal |
| 2 | Pan | L 100..C..R 100 | C | Centre of the path, left/right |
| 3 | Distance | 0..100 % | 25 | Centre of the path, near/far |
| 4 | Depth | 0..100 % | 60 | How much distance changes level and tone |
| 5 | Doppler | 0..100 % | 25 | Pitch bend from moving towards or away |
| 6 | Room | 0..100 % | 20 | Room send; more of it the farther away the sound is |
| 7 | Room Size | 0..100 % | 40 | Booth to hall (about 0.35 to 2.75 s) |
| 8 | Mono Safe | Off/On | Off | No ear timing difference |
| 9 | Path | see above | Circle | |
| 10 | Sync | Free/Sync | Sync | |
| 11 | Rate | 16 bars..1/16, or 0.01..10 Hz | 2 bars | |
| 12 | Direction | Clockwise/Counter | Clockwise | |
| 13 | Size X | 0..100 % | 70 | Path width |
| 14 | Size Y | 0..100 % | 30 | Path depth |
| 15 | Phase | 0..360° | 0 | Where on the path this instance is |
| 16 | Glide | 5 ms..2 s | 40 ms | Smoothing on every move |
| 17 | Step Angle | 15..180° | 90 | Step: degrees moved per hit |
| 18 | Hit Sens | 0..100 % | 50 | Step / Scatter |
| 19 | Fol Attack | 1..1000 ms | 20 | |
| 20 | Fol Release | 10..5000 ms | 400 | |
| 21 | Fol Gain | -12..+36 dB | 12 | Raise for quiet sources |
| 22 | Fol>Size | ±100 % | 0 | |
| 23 | Fol>Rate | ±100 % | 0 | |
| 24 | Fol>Push | ±100 % | 0 | |
| 25 | Throw | 0..100 % | 0 | For a Q-Link |
| 26 | Split | Off/On | Off | Three-band mode |
| 27 | Low Xover | 60..1000 Hz | 200 | |
| 28 | High Xover | 1..10 kHz | 2.5 kHz | |
| 29–31 | Low / Mid / High Size | 0..100 % | 0 / 100 / 100 | Each band's share of the path size (0 = still) |
| 32–33 | Mid / High Phase | 0..360° | 120 / 240 | Each band's offset along the path |
| 34–35 | X Now / Y Now | | | Display only: where the sound is right now (the mid band in Split) |
| 36–43 | Dot 1..8 | | | Display only: drive the dot on the globe (see below) |

## The panel

A custom skin built from `art/` by `art/make_skin.py`: the Bearings plate, with a wireframe globe round a
listener's head in the middle and four control boxes down each side. Five pages, each with eight controls; the
Q-Links take the outer columns, top to bottom.

| Page | Left | Right |
|---|---|---|
| **PLACE** | Pan, Distance, Depth, Mix | Path, Rate, Size X, Size Y |
| **MOTION** | Path, Sync, Rate, Direction | Phase, Glide, Step Angle, Hit Sens |
| **SPACE** | Room, Room Size, Doppler, Mono Safe | Distance, Depth, Mix, Output |
| **REACT** | Fol Attack, Fol Release, Fol Gain, Hit Sens | Fol>Size, Fol>Rate, Fol>Push, Throw |
| **BANDS** | Split, Low Size, Mid Size, High Size | Low Xover, High Xover, Mid Phase, High Phase |

**Path** is nine buttons, each with a drawing of its path. Controls that do nothing in the current setting are
greyed out, not hidden: Step Angle unless Path is Step, Hit Sens unless it is Step or Scatter, and everything on
BANDS except Split while Split is off.

**The globe shows where the sound is, live.** You are behind the listener, so left and right are the screen's,
and "in front of them" is up into the globe: just above the head is close, near the top is far. The dot gets
smaller and turns from cyan to violet as it moves away. In Split it follows the mid band.

How it is drawn: an MPC film strip cannot hold a two-dimensional position at this size (strips stay under about
11,400 px tall, which allows about 30 globe-sized frames), so the plane is cut into 8 rows from near to far.
Each row is its own strip of 32 positions, driven by its own display parameter (Dot 1..8). The row the sound is
in holds its position and the others hold a blank frame.

Re-render the skin before building whenever the art, the layout or the parameter list changes. The build copies
`skins/` as it finds it:

```
python3 plugins/Bearings/art/make_skin.py
python3 tools/build.py plugins/Bearings
```

## Presets

| Program | |
|---|---|
| **Slow Orbit** | The default: a 2-bar circle, a little wider than it is deep |
| **Centre Stage** | Fixed, slightly in front: just the room and distance cues |
| **Halo** | A big, slow 4-bar circle with more room |
| **Hat Orbit** | One circle a bar, shallow; for hats and shakers |
| **Pendulum Keys** | A 2-bar swing with a gentle arc |
| **Ping-Pong** | Side to side every beat |
| **Drift Away** | Free, slow random wandering, set back in the room |
| **Tide** | Breathes towards you and away every 2 bars |
| **Kick Push** | Stays still, but each loud hit pushes it back into the room |
| **Step Around** | A quarter turn per hit |
| **Scatter Percs** | A new spot per hit, with a short glide |
| **Prism Pad** | Split: mids and highs circle 120° apart, bass stays put |
| **Formation A / B / C** | The same circle at 0°, 120° and 240°: one on each of three tracks |
| **Far Away** | Fixed, far back, roomy |
| **Up Close** | Fixed, right in front, almost dry |
| **Swell Orbit** | A small circle that widens when the part gets louder |

## Tips

- On drums, put it on individual pads or on separate tracks: Hat Orbit on the hats, Scatter Percs on the
  shakers and congas, kick and snare left dry in the middle.
- Stereo inputs are summed to mono, which is the point: Bearings gives the sound a single position. A source
  panned hard to one side before it reaches Bearings comes in 6 dB quieter. Mix below 100 blends the original
  stereo back in.
- Short Glide with Ping-Pong, Step or Scatter jumps. Long Glide (200 ms or more) makes the same paths float.
- If the mix will be heard in mono (a phone speaker, a club's mono sub), turn on Mono Safe.
- Sustained, tonal sounds heard on their own (a solo piano, a held pad) can sound wobbly or chorused while they
  move. Moving changes the arrival time at each ear and, with Doppler, the pitch, and a long steady tone shows
  this up. In a mix it reads as space. If a solo part bothers you, lower Doppler first, then slow the Rate or
  turn on Mono Safe. A wide stereo piano sample can also lose some body when summed to mono.

## On the computer

```
python tools/build.py plugins/Bearings --target desktop
```

builds `build/desktop/Bearings.vst` for VST2 hosts (FL Studio, Reaper, MPC Software). It draws the same skin
as the MPC, and the globe animates there too. For where to put it, and FL Studio's search-path catch, see the
same section in Undertow's README. After replacing a build, quit the host completely: a host keeps the old
binary loaded until then.

## Tests

`tests/placetest.cpp` holds the plugin to what this README claims, by reading the emitters directly and
measuring the audio. A centred source keeps its level. Hard left is louder on the left and 29 samples later on
the right, and Mono Safe removes the delay. Far away is quieter, duller and wetter. Split adds back up flat.
Synced paths are where the song position says, at any block size and whatever bar they started on. Step and
Scatter move on hits. Doppler stays under its cap even on instant jumps. The globe's dot meters light exactly
one row, at the right column. Every program is finite, audible and under full scale.

```
cd plugins/Bearings/tests
clang++ -std=c++17 -O2 -I../../../framework -o build/placetest placetest.cpp && build/placetest
clang++ -std=c++17 -O2 -I../../../framework -o build/render tools/render.cpp && build/render ../build
```

`tools/render.cpp` writes demo WAVs (hats, percussion, a pad, through the programs) to `build/`, and prints which
way each one leans, beat by beat.

## What's not here (yet)

- In Split, the globe shows only the mid band. Three dots, one per band, would need 16 more display parameters.
- Height cues (above/below) and true behind-you placement: stereo speakers cannot do these convincingly, so
  "far" means farther back and more diffuse, not behind.
- Not yet confirmed: whether the MPC lists External Plugins in a drum pad's insert slots, and the CPU cost per
  instance on the hardware.

## CPU

Per band: one short delay line read at three points, two one-pole filters and a few multiplies. The room is
four delay lines and four allpasses; with Room at 0 it switches off once its tail has died away. Split adds
nine biquads. Position maths (a few sin/cos/exp) runs every 16 samples. On the desktop simulator a block costs
about 0.1% of real time. On the MPC, expect a few percent of a core per instance, and roughly double with Split
on. Measure before putting one on all 16 pads.

## Credits and licence

Original design, MIT. The head model (far-ear floor, interaural delay, head-shadow filter) and the four-line
room come from Groundswell's own Tidepools.
