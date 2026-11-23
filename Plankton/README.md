# Plankton

**A granular texture engine for live input.** A stereo effect for Akai MPC standalone hardware, by Groundswell.

Plankton keeps the last few seconds of the track in a buffer and plays them back as a cloud of tiny grains:
short, softly windowed slices from somewhere back in time, each at its own pitch and direction and in its own
place in the stereo field. Few, short grains stutter and sparkle; many long ones blur into a pad. **Freeze**
stops the buffer recording, so the cloud plays from the moment you caught for as long as you like, and lets go
without a click. **Catch** does the same while you hold it. **Linger** is their release: let go and the caught
cloud stays a while, then gives way to the live one. **Tail** needs no button: when you stop playing, the cloud
keeps playing what you just played and fades out over the time you set. **Drift** keeps a frozen cloud wandering, so it never
sits still. **Feedback** writes the cloud back into the buffer for textures that grow and smear, and **Smear**
melts the grains into a wash. With **Sync** on, grains land on the song's grid for rhythmic stutters instead of
free clouds.

## Install

1. Copy the folder `Groundswell - VST - Plankton` (in `build/` after building) into a folder named
   `VST2 Plugins` on a drive the MPC can see. The internal drive is the safest choice.
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC. Plankton appears under **External Plugins** in the effect browser. Put it on a track or
   program insert, or on a return with Mix at 100 %.

## How it works

```
in ─┬──────────────────────────────────────────────────────────────────────────── dry ──┐
    │                                                                                    │
    └─► (+ Feedback × the cloud, DC-blocked, level-guarded) ─► soft ceiling ─►            │
        RING BUFFER (stereo, ~22–24 s; Freeze / Catch stop it recording)                 │
          │                                                                              │
          ├─► SCHEDULER: Density grains per second (±20 % timing jitter), or one grain   │
          │   per Division on the song's grid when Sync is on                            │
          │                                                                              │
          └─► up to 48 GRAINS. Each one, at birth:                                       │
                where:  Position + Spray × random + Drift back from now                  │
                pitch:  Pitch + Scatter × random (Free / Oct / 5th / Scale) + Drift      │
                        reversed with probability Reverse                                │
                window: Shape (smooth Hann → flat top → struck)                          │
                stereo: Spread (narrowed and panned at random)                           │
              read with 4-point Hermite interpolation                                    │
                │                                                                        │
                └─► level normalised to the overlap ─► SMEAR (6 allpasses per side) ─►    │
                    TONE (low-pass left of centre, high-pass right) ──────── wet ────────┴─► Mix ─► Output ─► ceiling
```

- **Position** is the delay at a grain's centre. At 500 ms a forward grain at normal pitch plays exactly what
  happened 500 ms ago (tested to the sample). A pitched grain reads faster or slower, but its centre is still
  Position back, so changing Pitch doesn't move the cloud in time. Spray and Drift only ever push grains
  further back, so Position is always the freshest the cloud gets (the minimum is about 8 ms).
- **Freeze without clicks.** The buffer's write head is a seam: the newest sample sits next to the oldest. When
  you freeze, the 8 ms either side of the seam fade to silence, so a grain that reaches it dips quietly instead
  of jumping. Grains already sounding that would run through the seam stop inside that silent stretch. On
  release, recording comes back with an 8 ms fade-in, and frozen grains never read the oldest 0.6 s (the part
  the recorder overwrites first), so nothing is overwritten under a grain that is playing.
- **Tail** follows the input level. The input counts as playing while it is within 20 dB of its recent peak
  and above -55 dBFS. When it goes quiet, the live grains keep reading the last thing you played until they run
  past it (Position back). Then Tail grains take over: each one picks the loudest of four random spots inside
  that last event (from where it got loud to where it got quiet, at most its last second), using a loudness
  map of the buffer, so the tail is a cloud of the body of the note, not its fading end. Each new grain starts
  a little quieter, reaching -60 dB after Tail. Below that no grain starts, so the cloud ends in true silence.
  Play again and the grains move to the new sound at full level. On drums the gaps between hits fill with the
  last hit. Tail doesn't apply while frozen (Freeze already holds the cloud).
- **Linger** is a release for Freeze and Catch. When you let go, each new grain comes from the caught moment
  with a chance that starts at 100 % and falls to 0 over the Linger time, along a raised-cosine curve: it stays
  near 100 % for a while, then drops smoothly. Every other new grain comes from the live buffer. So the caught
  cloud gives way to the live one grain by grain, with no fade dip in between (the level holds within 1 dB
  in the test). Meanwhile the recorder is running again and overwrites the caught moment from its oldest end.
  Each lingering grain stays clear of that, and when nothing of the moment is left at the depth a grain wants,
  that grain comes from the live buffer instead. Freeze or Catch again during a linger and the new catch
  replaces it.
- **Level.** The cloud is scaled by the expected overlap (Density × Size, weighted by the window's power), so
  sparse grains play at about input level and dense clouds don't pile up.
- **Feedback** sums the cloud (after Smear and Tone) back into what is recorded. A DC blocker and a soft ceiling
  on the recorded signal keep it in range, and a guard eases the feedback off when the cloud's peaks pass about
  -7 dBFS. At 100 % with an octave shimmer it builds into a dense, rising wash that holds its level, and
  dies away once the input stops. Feedback does nothing while frozen, because nothing is being recorded.
- **Sync** fires a grain on every Division of the song position, on the nearest sample, whatever the block
  size. While the transport is stopped, it runs free at the song tempo. Drift's wander follows the song
  position too, so a loop drifts the same way every time round.

## Parameters

Ids and order are permanent: projects remember parameters by them.

| # | Name | Range | What it does |
|---|---|---|---|
| 0 | Output | -24 … +6 dB | Level after the mix (the MPC nudges parameter 0 on load: harmless here) |
| 1 | Mix | 0 … 100 % | Dry / cloud, equal power |
| 2 | Freeze | Off / On | Stop recording: the cloud plays from the captured moment. A toggle |
| 3 | Catch | Off / On | The same, held: frozen while Catch is on. On the panel it springs back when you let go |
| 4 | Size | 5 … 500 ms | Grain length |
| 5 | Density | 1 … 200 /s | Grains per second (free mode) |
| 6 | Position | 0 … 3 s | How far back the grains play from |
| 7 | Spray | 0 … 1 s | Random extra distance back, per grain |
| 8 | Pitch | ±24 st | Transposition of every grain |
| 9 | Scatter | 0 … 24 st | Random transposition per grain, up to this far either way |
| 10 | Snap | Free / Oct / 5th / Scale | Scatter free, or snapped to octaves, to fifths and octaves, or to a minor pentatonic scale |
| 11 | Reverse | 0 … 100 % | Chance that a grain plays backwards |
| 12 | Shape | 0 … 100 % | Window: 0 smooth (Hann), 50 flat-topped, 100 struck (sharp attack, decaying) |
| 13 | Spread | 0 … 100 % | Random stereo placement per grain |
| 14 | Drift | 0 … 100 % | Slow wander of position (up to 750 ms further back) and pitch (up to ±1 st) |
| 15 | Drift Rate | 0.05 … 4 Hz | How fast Drift wanders. With Sync on it shows a period from 8 bars to 1/16 |
| 16 | Feedback | 0 … 100 % | The cloud written back into the buffer |
| 17 | Smear | 0 … 100 % | Allpass blur after the grains: crisp to a wash |
| 18 | Tone | Dark 100 … Flat … Thin 100 | Left: low-pass from 18 kHz down to 300 Hz. Right: high-pass from 20 Hz up to 2 kHz |
| 19 | Sync | Off / On | Grains and Drift locked to the song |
| 20 | Division | 1/64 … 2/1 (with triplets and dotted) | One grain per Division when Sync is on |
| 21 | Linger | Off … 10 s | Release for Freeze and Catch: how long the caught cloud stays after you let go, giving way to the live cloud grain by grain |
| 22 | Tail | Off … 10 s | When the input goes quiet, the cloud keeps playing what you just played and fades out, -60 dB after this long |

Pitch, Scatter, Reverse, Size, Spread and position are fixed when a grain is born, so moving a knob changes the
grains that follow, not the ones already sounding. That keeps knob moves smooth.

## The panel

Two tabs over the plankton plate. Each row is one Q-Link row: a box of two controls either side, with the
title and the glowing swirl kept in view in the middle.

| Row | CLOUD (left box, right box) | DETAIL |
|---|---|---|
| 1 | **CATCH**: Freeze (OFF/ON), Catch (HOLD) · **TAIL**: Tail, Linger | **MOTION**: Drift Rate, Spread · **COLOUR**: Shape, Tone |
| 2 | **GRAIN**: Size, Density or Division · **PLACE**: Position, Spray | **TEMPO**: Sync (OFF/ON), Division · **OUT**: Output |
| 3 | **PITCH**: Pitch, Scatter · **TWIST**: Snap, Reverse | |
| 4 | **DRIFT**: Drift, Feedback · **WASH**: Smear, Mix | |

- **HOLD** is momentary on the touch screen: hold it and the cloud is frozen, lift your finger and it lets go.
  It uses the skin format's `ResetOnGestureEnd` binding, which has **not been tried on hardware yet**. If it
  turns out to latch on your MPC, Freeze still works as a toggle. If the panel shows no tabs at all, delete
  `skins/` and rebuild to get the plain UI-kit layout from `ui.json`, and please report it.
- **On a Q-Link**, Freeze and Catch switch at the halfway point: turn up to catch, turn back down to let go.
- **GRAIN's second slot** shows Density while Sync is off and Division while it is on. Its Q-Link drives Density.
  Sync and Division have their own Q-Links on DETAIL.
- **Catch and Linger together** work like an envelope: HOLD is the gate and Linger the release.
- `art/make_skin.py` draws the panel into `skins/`; build.py copies `skins/` as it is. After changing the
  parameters or the layout, run `python3 plugins/Plankton/art/make_skin.py` **before**
  `python3 tools/build.py plugins/Plankton`.

## Programs

All programs load with Freeze off (a program with Freeze on would catch silence). Hit Freeze or hold Catch
while something is playing.

| Program | Tail | Linger | Sound |
|---|---|---|---|
| Soft Cloud | 3 s | 1.5 s | 120 ms grains, a little spray and drift, gently smeared: the everyday cloud |
| Frozen Pad | 6 s | 4 s | Long smooth grains spread wide with lots of spray: freeze a chord and it becomes a pad |
| Shimmer Dust | 5 s | 2.5 s | +12 st with octave scatter (unison, +12, +24), feedback and smear: sparkle |
| Reverse Swarm | 3 s | 1.5 s | Every grain backwards, sprayed over half a second |
| Rhythmic Grains | Off | 0.5 s | Synced 1/16 grains, struck window, octave scatter, a quarter reversed: stutters on the grid |
| Smear Wash | 8 s | 3 s | Long dense grains, full smear and 40 % feedback: a dark wash |
| Glitch Plankton | Off | Off | 14 ms struck grains sprayed over a second, pentatonic scatter, half reversed: lets go at once |
| Deep Sea | 6 s | 3 s | An octave down with fifth scatter, dark tone, smear and feedback |

## Tips

- **Live cloud vs frozen cloud.** Unfrozen, the cloud follows what you play, Position behind. Frozen, Position and
  Spray move you around inside the caught moment. Turn Position while frozen to scrub through it.
- **Scrub on a Q-Link.** Freeze, Spray 0, Size about 80 ms, Density 40, then play Position.
- **Pads and drums.** For a frozen pad from drums, use long grains (300 ms+), Spray of a beat or more and
  Smear above 50 %. For rhythm, Sync on, a struck Shape, Spray 0 and a Position of one beat.
- **Tail on single notes and stabs.** Play short chords or notes with gaps: each one blooms into a cloud that
  hangs and fades. Shimmer Dust and Deep Sea are set up for it. Turn Tail off for tight, rhythmic material.
- **Catch with a long Linger** is a swell you play by hand. Hold, let go, and the moment fades back into the
  live cloud over the Linger time.
- **Feedback with Pitch +12 or Scatter on Oct** climbs: every pass goes up another octave.

## CPU

Measured on a desktop (Apple silicon) by `tests/planktontest.cpp`, 44.1 kHz, block 128, with Feedback, Smear,
Tone and full Spread on:

| Load | Desktop | Grains at once |
|---|---|---|
| Default (16/s, 90 ms) | 0.09 % of a core | 2 |
| Busy (50/s, 200 ms, scatter) | 0.19 % | 12 |
| Maximum (200/s, 500 ms, +24 st, scatter 24) | 0.63 % | 48 (capped; extra triggers skipped) |

The MPC's ARM cores are roughly ten times slower than the desktop by the siblings' measurements, so estimate
about 1 % of one MPC core at the default, 2 % busy and 6–8 % at the absolute maximum. These estimates have
**not been measured on hardware**. The cost grows with the number of grains sounding at once (Density × Size),
up to the cap of 48. Above that, triggers are skipped, so nothing is cut off and the cloud just stops getting
denser. Every pow, exp and cos runs once per grain at its birth, and filter coefficients once per block. The
per-sample work is a window-table lookup, two Hermite reads and a 2×2 pan per grain. Tail adds a level
follower and a loudness map (a few operations per sample). The buffer takes 8 MB at 44.1/48 kHz (16 MB at 96 kHz).

## What is not in this version

- **HOLD (momentary Catch) is untested on the MPC**: see The panel.
- **No anti-aliasing for big upward shifts.** A grain at +24 st reads four samples per sample with plain
  Hermite interpolation, so bright material aliases a little at high pitches. In a cloud it mostly reads as
  sparkle.
- **No time-stretch or scan control.** Grains play the buffer at their own pitch. There is no separate playback
  speed or automatic scan through a frozen moment (move Position or use Drift).
- **Free timing jitter is fixed at ±20 %**, so dense clouds don't buzz at the grain rate. There is no control
  for perfectly regular free grains (use Sync for those).
- **Density is ignored while Sync is on** (Division sets the rate). It is hidden on the panel then.
- **Feedback is inactive while frozen** (nothing is recorded).
- **Linger is a handover, not a volume envelope.** The cloud's level stays the same and only where the grains
  come from changes. For the cloud to fade to silence after you let go, lower Mix (or let the input stop).
- **Tail's idea of "quiet" is fixed**: 20 dB below the recent peak, or below -55 dBFS. There is no threshold
  control. A note that sustains without dropping 20 dB counts as still playing, so its tail starts when it
  finally does. A noisy input that never drops below -55 dBFS never tails off.
- **Tail draws from at most the last second of what you played**, so the tail of a long phrase is a cloud of
  its end.
- **The Drift Rate readout changes between Hz and bars when you touch it** after switching Sync. The MPC caches
  value text until a control is touched.
- **The frozen moment is up to about 21–23 s** (the buffer, less the 0.6 s guard). Position reaches 3 s, and Spray
  and Drift add up to 1.75 s more.
- **No per-grain level randomisation, no envelope follower, no MIDI** (effects receive no MIDI on the MPC).

## Testing

`tests/planktontest.cpp` renders through the plugin offline and checks it sample by sample (68 checks; log in
`build/planktontest.log`):

```
cd plugins/Plankton
mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework tests/planktontest.cpp -o tests/build/planktontest
tests/build/planktontest            # also writes build/plankton-demo.wav
```

- **Positions**: every grain starts at Position, and its output equals the window × the input from exactly
  500 ms earlier (correlation > 0.999, and no match 10 ms off). Spray keeps grains inside
  [Position, Position + Spray]. Reversed grains are the input backwards, centred on Position.
- **Pitch**: each grain's frequency, from zero crossings, is within 1 cent of the target at 0, +3.5, +7, ±12,
  ±24 and -0.3 st. Every scattered grain lands on the Oct, 5th or Scale grid. Shimmer Dust's grains are only
  ever unison, +12 or +24.
- **Freeze and Catch**: the frozen cloud keeps a captured A minor while the input moves to G major (level steady
  within 1 dB), follows the input again after release, and shows no click on either edge (peak second
  difference below steady playing). Freeze toggled every 37 ms with Linger 2 s and pitched, reversed, sprayed
  grains is no rougher than never toggling.
- **Linger**: with Linger off, 0.9 s after letting go the cloud is the new chord. With 3 s, the caught chord
  still leads at 0.9 s, both sound halfway through, and only the new chord remains after it, with no level dip
  and no click. A 10 s linger from a full buffer at the deepest reach is click-free.
- **Tail**: with Tail off, the cloud is gone 0.8 s after a note. With 3 s, it still plays that note (not
  anything else) 0.6–1 s later, fades about 20 dB a second (-60 dB over 3 s), and is digital silence once the tail
  has run out. A new note takes over at full level, with no clicks anywhere. On hits every 0.5 s, the gaps stay
  filled with the last hit.
- **Tempo**: synced grains at 1/8 at 120, 1/16T at 93, 1/16 at 174 and 1/4 at 60 BPM, with blocks of 37 to
  512 frames, come out in the expected number and within half a sample of the grid. Stopped, they run free at
  the song tempo. Free grains average Density per second, within ±20 %. Synced Drift follows the bar.
- **Feedback** at 100 % with an octave shimmer, +6 dB output and loud noise: output ≤ 0 dBFS, the buffer never
  above full scale, the level steady over 15 s, and the tail gone 12 s after the input stops.
- **Safety**: no NaN or denormals over all 8 programs with random parameter jumps and 1e-30-level input; every
  parameter at its minimum and at its maximum; 44.1, 48 and 96 kHz.
- **CPU and the cap** (above).

The SDK's host simulator passes 20/0/0 at 44.1, 48 and 96 kHz, with blocks of 32 and 512, and with the
transport playing.

### Demo

`build/plankton-demo.wav` (180 s): a synthesised drum loop at 96 BPM, a chord pad, a vowel-ish voice and chord stabs, each
first dry and then through the programs:

| Time | Section |
|---|---|
| 0:00 | Drums, dry |
| 0:12 | Rhythmic Grains on the drums (synced) |
| 0:24 | Glitch Plankton on the drums |
| 0:36 | Smear Wash on the drums |
| 0:48 | Pad, dry |
| 1:00 | Soft Cloud on the pad |
| 1:12 | Frozen Pad: Freeze at 1:15, the pad stops at 1:16, release at 1:19 into a 4 s Linger |
| 1:24 | Shimmer Dust on the pad |
| 1:36 | Voice, dry |
| 1:48 | Reverse Swarm on the voice |
| 2:00 | Deep Sea on the voice |
| 2:12 | Soft Cloud on the voice, Catch held 2:16–2:20 while the voice carries on, then a 1.5 s Linger |
| 2:24 | Chord stabs, dry (0.3 s every 2 s) |
| 2:36 | Shimmer Dust on the stabs: each one blooms and fades (Tail 5 s) |
| 2:48 | Deep Sea on the stabs (Tail 6 s) |

## Project layout

| Path | What |
|---|---|
| `plugin.json` | Name, manufacturer, uid `G3Dd`, version |
| `src/Plankton.cpp` | All the DSP, parameters and programs |
| `art/` | Your plate (`plate-src.webp`), the shared knob and pill art, and `make_skin.py` |
| `skins/` | The rendered panel (TUI.json, Q-Links.json, images), copied into the build as it is |
| `ui.json` | A stock-kit fallback layout (unused while `skins/` exists) |
| `tests/planktontest.cpp` | The render test and the demo |

## Licence

MIT, like the rest of the Groundswell plugins. Original code. Granular synthesis with windowed grains,
Hermite interpolation, Schroeder allpasses and the state-variable filter (from the SDK) are textbook
techniques; no code was copied from elsewhere. The skin machinery is Seaglass's generator.
