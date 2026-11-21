# Low Tide

**Tape with a memory and a lifespan.** A lo-fi insert effect for the MPC by Groundswell.

Most lo-fi boxes are a fixed colour: set the wobble, the crunch and the vinyl, and that is the sound
for the whole song. Low Tide moves, in time with the song:

- **Generation loss.** The audio runs through a virtual tape loop that is dubbed again on every pass,
  like a cassette of a cassette of a cassette. Each pass loses top end, adds hiss, softens the
  transients and smears the pitch. A loop is hi-fi on bar 1 and worn out a few bars later. Tap
  **Fresh** and it is a clean copy again.
- **The Tide.** One **Wear** macro sweeps every stage from clean to wrecked, and a slow cycle locked
  to the bars (1 to 32) swings Wear in and out, so the track crumbles and then washes back.
- **Tape accidents.** Now and then, on the beat: a dropout, the motor dragging, a speed slip, a
  burst of crinkled-tape flutter, one side cutting out for half a beat. A gentle cousin of
  Wipeout, not a glitch box.
- **Sampler crunch.** The old-sampler trick: record a source sped up and play it back slowed down.
  The sample rate drops with the pitch, so the grit does too: 12-bit, about 26 kHz at no
  pitch-up, with the converter's input and output filters.
- **Tape flaws.** Print-through ghosts a beat before and after, azimuth drift (phasey, dull and
  closing in towards mono), hiss that breathes with the music, and gentle wow and flutter
  underneath.

At **Wear 0 %** the track passes through untouched (tested bit for bit). Every program sits within
about 1.5 dB of the input level, and **Mix** blends it in, so it is usable on a whole mix or a bus.

| You want | Try |
|---|---|
| A loop that wears out as it plays, and a button to renew it | **Worn Shores**: Generations 6, Bars 2; tap Fresh |
| Lo-fi that comes and goes with the arrangement | Tide Depth 100 %, Tide Rate 8 Bars, Shape Erode (program **Eight Bar Erosion**) |
| The track slowly falling apart for good (an outro) | Memory 90–100 %, Generations 16 (program **Disintegration**) |
| Old-sampler grit on a drum bus | **Sampler Crunch**; push Rec Pitch for more |
| A wonky, unreliable cassette deck | **Warped Cassette** |
| A little age on a whole mix | **Mix Patina**, or any program with Wear 20–35 % |
| Endless decay and renewal | **Lifespan**: generations bounce up and down, the tide steps bar by bar |

## How it works

Signal flow: input → **crunch** → **generation copy** → **tape loop** → **print-through** →
**azimuth** → **transport** (wow, flutter, smear, drag, slips, crinkle) → **dropouts and cuts** →
**tape compression and breathing hiss** → Mix → Output.

- **Wear scales everything.** Each stage's amount is multiplied by twice the effective wear and
  then limited: at **Wear 50 %** every stage sits at its own knob setting, at **0 %** everything is
  off, at **100 %** everything runs at twice its setting. That includes accident chance and
  severity, so a clean stretch of the tide is also a calm one.
- **The Tide** moves Wear. Effective wear = Wear × (1 − Depth × (1 − tide)), where the tide shape
  runs from 0 to 1. **Wear** is how wrecked it gets at the peak; **Tide Depth** is how far it
  washes back (100 % = all the way to clean). Shapes:
  - **Swell**: a sine, clean on bar 1.
  - **Erode**: wears out steadily and washes back over the last eighth of the cycle.
  - **Ebb**: up, then down.
  - **Steps**: one step per bar (per beat on a 1-bar cycle).
  - **Drift**: a random level every quarter cycle, eased between. It is the same for the same bar,
    so a song renders the same way every time.

  The Wear Now meter shows the result.
- **Generations.** The loop is **Bars** long (1, 2, 4 or 8) and lined up with the bar lines. Each
  time it comes round, the generation counter moves on, and the copy you hear glides to that
  generation over half a beat, so each dub lands on the downbeat. **Generations** is the cap.
  **At Cap** chooses what happens there:
  - **Hold**: stays at the cap.
  - **Restart**: a fresh tape, then it wears out again.
  - **Bounce**: counts back down to clean, then up again.

  What one generation costs:
  - Top end: a 2-pole low pass that falls from 20 kHz towards 2.2 kHz.
  - Lows: a high pass that rises to about 140 Hz.
  - Transients: tape saturation, harder with each dub.
  - Hiss: Hiss × √generations, darkened by the same low pass.
  - Pitch: a random wander that grows with √generations.

  Generation 0 is exactly clean.
- **Fresh** snaps back to generation 0 (over 30 ms) and erases the tape loop. **Any** change of the
  switch does this, so every tap is a fresh start, whichever way the switch shows. Starting the
  transport is also a Fresh, so a song plays and renders the same way every time.
- **Memory** is the loop re-recording over itself:
  recorded = live copy × (1 − Memory) + last pass × Memory. The last pass goes through one more
  generation of loss first.
  - **0 %**: you hear the live track through its generation copy. It follows the song exactly
    (best on a mix).
  - **50 %**: earlier passes linger under the new one, each one duller.
  - **100 %**: the loop just keeps replaying itself, and crumbles a little more on every pass (at
    Wear 0 it replays unchanged, a perfect looper).
- **Print-through**: faint mid-range ghosts (about −20 dB pre-echo and −23 dB post-echo at full)
  one Spacing after and one Spacing before the audio.
  - The post-echo is read Spacing ago from the tape loop.
  - The pre-echo is read from the previous pass of the loop, Spacing ahead. It needs no look-ahead
    and adds **no latency**, and it is a true pre-echo for anything that repeats with the loop, as
    an MPC sequence does. On material that does not repeat, the pre-echo is a ghost of what played
    a loop earlier.
- **Azimuth**: one channel lags the other by up to 0.35 ms, the side signal shrinks towards mono
  (combing as it goes) and the top end dulls. **Drift** makes the error wander slowly and swap
  sides.
- **Accidents.** On the **Density** grid (each bar, half bar, beat or 8th) the dice roll; **Chance**
  is the chance per grid point. Only one accident happens at a time, and it starts exactly on the
  grid point. The kinds you allow are picked at random:

  | Kind | What happens | Length |
  |---|---|---|
  | Dropouts | The level dips with top-end loss | 1/16–1/4 beat |
  | Drag | The motor slows (the pitch sags up to about 1.4 semitones), then catches up (briefly a little sharp) | 1–2 beats |
  | Slips | A quick pitch hiccup down, then back | 1/8 beat |
  | Crinkle | A fast 18–28 Hz flutter burst with a level wobble and lost highs | 1/4–1/2 beat |
  | Chan Cut | One side drops out | Exactly half a beat |

  **Severity** sets how deep they go. **Pattern** repeats the dice every 1, 2 or 4 bars, so the
  accidents become part of the groove.
- **Sampler crunch**: an input filter, sample and hold at 26.04 kHz ÷ 2^(Rec Pitch/12), **Bits**
  (8–16), then an output filter. **Crunch** blends it in. The **Filters** switch:
  - **Clean**: 4-pole filters that track the rate. Dark and smooth.
  - **Dirty**: a loose input filter that lets some aliasing in, and a fixed output filter that
    lets the images through, so the grit grows with Rec Pitch.
  - **Raw**: no filters.
- **Breathe**: a gentle tape compressor (1 ms attack, 250 ms release) on the wet signal. The hiss
  rides up under loud hits and sinks back in the gaps.
- **Wow and flutter**: wow at about 0.55 Hz (its rate wanders), flutter at 6.3 and 10.7 Hz. Wow
  100 % at Wear 50 % is a 0.5 % speed swing, about 9 cents. All the pitch movement is one
  modulated delay, a few milliseconds on average.
- **The clock.** With the MPC playing, everything follows the song position, but **counts bars as
  played**: when a 2-bar sequence loops, an 8-bar tide and the generation counter keep going
  instead of jumping back. Position inside the bar always follows the song. With the transport
  stopped, a free-running clock at the MPC's tempo keeps everything moving (120 BPM if the host
  gives none).

## Pages and controls

Three pages. On each page a row is one row of four Q-Links: two boxes of two controls, one either
side of the cassette.

| Page | Row (Q-Links) | Left box | Right box |
|---|---|---|---|
| TAPE | 1 | TAPE: **Wear**, **Generations** | LEVEL: **Mix**, **Output** |
| | 2 | LOOP: **Bars** (1/2/4/8), **At Cap** (Hold/Restart/Bounce) | MEMORY: **Fresh**, **Memory** |
| | 3 | TIDE: **Depth**, **Rate** | TIDE: **Shape**, **Wear Now** (meter) |
| | 4 | COUNTER: **Generation**, **Accident** (meters, no Q-Links) | - |
| ACCIDENT | 1 | ACCIDENTS: **Chance**, **Severity** | GRID: **Density** (Bar/½/Beat/8th), **Pattern** (Free/1/2/4) |
| | 2 | KINDS: **Dropouts**, **Drag** | KINDS: **Slips**, **Crinkle** |
| | 3 | KINDS: **Chan Cut**, **Now** (accident meter) | - |
| FLAWS | 1 | SAMPLER: **Crunch**, **Rec Pitch** | SAMPLER: **Bits**, **Filters** (Clean/Dirty/Raw) |
| | 2 | PRINT-THROUGH: **Print**, **Spacing** (½/1/2 beats) | AZIMUTH: **Azimuth**, **Drift** |
| | 3 | NOISE: **Hiss**, **Breathe** | MOTION: **Wow**, **Flutter** |

The custom skin is drawn over the user's rock-pool cassette plate (`art/plate-src.webp`) by
`art/make_skin.py`. The mock-up (`art/mock-reference.webp`) was a guide to the look only: its
controls are not Low Tide's. **Run `python3 "plugins/Low Tide/art/make_skin.py"` before
`tools/build.py`**, because the build copies `skins/` as it finds it. `ui.json` (the cyan UI kit, the
same pages in blocks of four) is the fallback if `skins/` is removed.

## Parameters

Index order is permanent (projects store it). Meters are written by the plugin and are not saved.

| # | Name | Range | Default | Notes |
|---|---|---|---|---|
| 0 | Output | −24…+6 dB | 0 dB | |
| 1 | Mix | 0–100 % | 100 % | Dry/wet |
| 2 | Wear | 0–100 % | 50 % | The macro; 50 % = every stage as set |
| 3 | Generations | 0–16 | 6 | The cap |
| 4 | Bars | 1, 2, 4, 8 bars | 2 bars | Loop length = one generation |
| 5 | At Cap | Hold / Restart / Bounce | Hold | |
| 6 | Fresh | switch | Off | Any change = clean copy, loop erased |
| 7 | Memory | 0–100 % | 0 % | Re-record the previous pass |
| 8 | Tide Depth | 0–100 % | 50 % | |
| 9 | Tide Rate | 1, 2, 4, 8, 16, 32 bars | 8 bars | |
| 10 | Tide Shape | Swell / Erode / Ebb / Steps / Drift | Swell | |
| 11 | Chance | 0–100 % | 15 % | Per grid point (× Wear scaling) |
| 12 | Density | Bars / Halves / Beats / 8ths | Beats | Grid for the dice |
| 13 | Severity | 0–100 % | 40 % | |
| 14–18 | Dropouts, Drag, Slips, Crinkle, Chan Cut | switches | on, on, on, on, off | Kinds allowed |
| 19 | Pattern | Free / 1 / 2 / 4 bars | Free | Repeat the dice |
| 20 | Crunch | 0–100 % | 0 % | |
| 21 | Rec Pitch | 0–24 st | 5 st | Lower rate, more grit |
| 22 | Bits | 8–16 | 12 | |
| 23 | Filters | Clean / Dirty / Raw | Dirty | |
| 24 | Print | 0–100 % | 25 % | |
| 25 | Print Time | ½, 1, 2 beats | 1 beat | "Spacing" on the skin |
| 26 | Azimuth | 0–100 % | 25 % | |
| 27 | Az Drift | 0–100 % | 40 % | |
| 28 | Hiss | 0–100 % | 30 % | Hiss of one generation; 100 % ≈ −40 dB rms |
| 29 | Breathe | 0–100 % | 40 % | |
| 30 | Wow | 0–100 % | 30 % | |
| 31 | Flutter | 0–100 % | 25 % | |
| 32 | Wear Now | meter | | Effective wear |
| 33 | Gen | meter | | Generation counter |
| 34 | Accident | meter | | The accident in progress |

## Programs

| Program | What it does |
|---|---|
| Worn Shores | The default: 6 generations over 2-bar passes, an 8-bar swell, occasional accidents |
| Dub Of A Dub | 12 generations a bar at a time, Memory 40 % so old passes linger, strong print-through |
| Eight Bar Erosion | Hi-fi on bar 1, wrecked by bar 8, washed clean on bar 9 (Erode tide plus Restart every 8 passes), light crunch |
| Sampler Crunch | Old-sampler grit (Rec Pitch +12, Dirty filters), light tape |
| Disintegration | Memory 90 %, 16 generations over 4-bar passes: the loop keeps replaying itself and falls apart |
| Warped Cassette | Heavy wow and flutter, frequent drags, slips, crinkles and channel cuts on a 2-bar pattern, drifting azimuth |
| Mix Patina | Gentle: a little hiss, wear and wobble, no accidents. For a bus or a whole mix |
| Lifespan | Generations bounce up and down every bar, the tide climbs in steps, a 1-bar accident pattern |

## Tips

- On a whole mix, keep Mix at 100 % and use Wear for the amount. Wear 20–35 % is a light patina.
  Mix below 100 % blends a track with a few milliseconds of wow delay against the dry signal. That
  is chorus-like, so use it on purpose.
- Map **Fresh** to a pad or Q-Link and play it: let a loop rot for eight bars, then snap it back on
  the drop.
- For a song that degrades once and stays degraded, use At Cap **Hold** and Tide Depth 0.
- With the transport stopped everything still moves (free clock), so you can audition a pad loop
  or the Sample Edit screen through it.

## Tests

- `tests/listentest.cpp`: 29 checks. Build and run it from `plugins/Low Tide`:

  ```
  mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
  tests/build/listentest build/low-tide-demo.wav
  ```

  It checks that:
  - One pass happens per bar, on the bar line.
  - Brightness falls generation by generation (to 0.08 of the input by generation 8), holds at
    the cap, and Fresh restores it.
  - The level stays within 3 dB on every bar.
  - Restart and Bounce count the generations correctly.
  - Wear 0 is bit-exact to the input and has no accidents.
  - The Tide matches its formula on every bar line, keeps counting through a looping 2-bar
    sequence, and free-runs with the transport stopped.
  - Every accident starts exactly on a beat, all five kinds occur, peaks never rise, a dropout
    dips, a channel cut is one-sided, and a 2-bar Pattern repeats exactly.
  - Memory 100 % replays unchanged at Wear 0 and disintegrates at Wear 70.
  - The print-through ghosts sit at −24 dB a beat before and −27 dB a beat after, with nothing in
    between and no latency.
  - The crunch residual rises with Rec Pitch (−38 → −21 dB from 0 to 19 semitones).

  It also renders all 8 programs, 8 bars each at 92 BPM, to `build/low-tide-demo.wav`.
- `host_sim test` passes with no failures at 44.1, 48 and 96 kHz. It gives two warnings, both
  understood:
  - The meters are not saved (deliberate).
  - With a silent input the output is about −40 dBFS. That is the tape hiss; set Hiss to 0 for
    silence.

## CPU and memory

- Desktop: 0.13 % of one core with every stage on (Warped Cassette with crunch and Memory).
- MPC: expect roughly 2–4 % of one core. **Not yet measured on hardware.**
- No allocation, locks or I/O in the audio thread, and no per-sample trigonometry (sines are a
  cheap polynomial).
- The tape loop is 16-bit and holds 32 s (8 bars down to 60 BPM in 4/4). That is about 5.6 MB at
  44.1 kHz. Slower tempos halve the loop's bars until it fits; above 48 kHz the 32 s shrinks
  proportionally.

## What is not implemented

- No vinyl (crackle, rumble) or radio colours: Low Tide is tape and sampler, on purpose.
- Accidents are one at a time (never two overlapping). With Pattern on, their kind, timing and
  depth all repeat; Severity and Chance still scale them.
- The drag recovery plays briefly sharp. This is inherent to doing it with no latency: the lost
  time has to be caught up.
- Fresh erases the loop over one pass (an erase head), not instantly. Memory fades out at once,
  so nothing old is heard, but the pre-echo ghost is muted until the pass completes.
- With Memory above 0 the output carries material from up to a loop earlier. That is the point,
  but it does not follow song changes until the old passes fade.

Licence: MIT. All DSP is original; no code is borrowed.
