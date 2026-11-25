# Wipeout

**An audio mangler locked to the song: fills, glitches, breaks, ratchets, rolls, timewarps, reverses
and transitions, by the dice or by hand.** An MPC insert effect by Groundswell (it started out as
"Riptide").

Wipeout cuts your track up in time with the sequence. On a grid it rolls the dice, and when they
say so it grabs a slice of what is playing and repeats it (a ratchet), plays it backwards or at half
speed (a timewarp), stops it like a tape machine losing power (a break), chops it with a gate, or
rolls it faster and faster into the next bar (a fill or a transition). Every chance is a knob, so
the panel is a performance surface for the Q-Links: leave it on the dice for automatic fills,
flick **Hold** for a stutter by hand, lock a pattern you like with **Pattern**, and keep four
setups in **Scenes A–D** to switch between by touch or from the sequence's automation.

Undertow does echoes; Wipeout cuts the audio up instead. Nothing is added between events: the dry
track passes through bit-for-bit.

| You want | Try |
|---|---|
| A fill at the end of every bar or two | Grid 1 Bar or 2 Bars, Offset 1.4.1 or 2.4.1, Length 1/4 (program **Bar Fills**) |
| Ratchets | Slice 1/16 or 1/32 with a short Length; Decay for ratchets that fade |
| Rolls | Roll 100%, Length 1/4 to 1 Bar (program **Snare Rolls**) |
| A transition into every 8-bar phrase | Grid 8 Bars, Offset 7.1.1, Length 2 Bars, Roll 100%, Chance 100% (program **Transitions**); Grid 4 Bars + Offset 4.1.1 + Length 1 Bar for 4-bar phrases |
| A long break | Tape Stop 100%, Length 2 or 4 Bars |
| Glitches | Grid 1/4, Vary and Reverse up, short Length (program **Glitch Storm**) |
| Breaks | Tape Stop up, Length 1/2 (program **Tape Breaks**) |
| Timewarps and reverses | Half Speed and Reverse (program **Reverse Wash**) |
| Only by hand | Chance 0, then play **Hold** (program **Hold To Play**) |
| Different mangling per song section | Set up Scenes A–D, then automate **Scene** in the sequence (verse A, build C, drop D) |

## How it works

- **The clock.** With the MPC playing, the grid comes from the song position, so events land
  exactly on the sequence's beats and follow loops and jumps. With the transport stopped, Wipeout
  runs its own clock at the MPC's tempo and restarts it on the first hit after a quarter-second of
  quiet, so a pad played on its own starts the grid.
- **The dice.** Every **Grid** (pushed later by **Offset**), it rolls: **Chance** decides whether
  an event starts. An event lasts **Length**; rolls that fall inside a running event are skipped.
- **What an event does**, in order of precedence:
  - **Tape Stop** (chance): playback slows from full speed to a stop over the event, pitch sinking
    with it, and fades out as it stops. The track comes back on the next grid line.
  - **Gate** (chance): the live track is switched on for the first half of every slice and off for
    the second half.
  - **Repeat** (otherwise): the first slice plays through as it is captured, then repeats until the
    event ends. **Reverse** is a chance for each repeat after the first to play backwards (the
    first can't: it hasn't been recorded yet). **Half Speed** is a chance per event to play half the
    slice an octave down in the same time, so the repeats stay on the grid.
- **Shaping.** **Roll** is a chance per event that the slice halves after a third of the event and
  again after two thirds (1/8 → 1/16 → 1/32, the build-up roll). **Vary** is a chance per event to
  use a random slice size. **Decay** makes each repeat quieter than the one before. A slice is never
  longer than half the event, so there are always at least two.
- **Hold** starts a repeat on the next Slice boundary and keeps going until it is switched off,
  whatever the dice say.
- **Pattern** freezes the dice. On **Free** every bar is new. Choose **1, 2, 4 or 8 Bars** and the
  last 1, 2, 4 or 8 complete bars of dice replay from the next bar on, until you go back to Free. The
  knobs still work while it's locked: they change what the dice mean, not the dice.
- **Scenes A–D.** Each scene is a snapshot of the controls you play: Chance, Slice, Length, Mix,
  Reverse, Half Speed, Tape Stop, Gate, Vary, Roll, Decay, Grid, Offset and Pattern (not Output,
  Hold or Smooth). Turning a knob edits the scene you're on; tapping another scene puts its
  values on the knobs. There's no store button and nothing to copy: the knobs always *are* the
  scene in play. Every program comes with four scenes (below), and all four are saved with the
  project. A scene change takes effect from the next roll of the dice; Mix glides over a few ms,
  so switching mid-event doesn't click.
- **No clicks.** Every slice edge and every event edge is faded over **Smooth** milliseconds (a
  smoothstep curve, 3 ms by default). Two event slots let a new event fade in while the old one fades
  out.

## Parameters

| Page | Control | What it does |
|---|---|---|
| PLAY | Chance | Chance that an event starts at each grid point. |
| | Slice | Size of the repeated piece: 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T. |
| | Length | How long an event lasts: 1/8, 1/4, 1/2, 1 bar, 2 bars or 4 bars. |
| | Mix | Level of the effect during an event. 100% replaces the track; lower blends the repeats over it. |
| | Reverse | Chance that each repeat after the first plays backwards. |
| | Half Speed | Chance that a repeat event plays an octave down at half speed. |
| | Tape Stop | Chance that an event is a tape stop. |
| | Gate | Chance that an event is a gate (chops the live track per slice). |
| | Vary | Chance that an event uses a random slice size. |
| | Roll | Chance that the slice halves as the event goes on. |
| | Decay | Each repeat quieter than the last (0% = all the same level). |
| | Hold | Stutter now (from the next slice) until switched off. |
| | Grid | How often the dice are rolled: every 1/4, 1/2, 1, 2, 4 or 8 bars. |
| | Offset | Moves the roll points later, in 16ths up to 8 bars, shown as bar.beat.16th: 1.4.1 = beat 4, 7.1.1 = the start of bar 7. It wraps at the Grid (2.4.1 with a one-bar grid is the same as 1.4.1). |
| | Pattern | Free, or replay the last 1, 2, 4 or 8 bars of dice. |
| | Output | Output level. |
| | Smooth | Fade time at slice and event edges, 0.5–20 ms. Short is snappier, long is softer. |
| | Scene | A, B, C or D: the scene in play (the pills under the wave). New in 1.1. |

## Programs

Each program has four scenes. A is the program as listed; B, C and D are variations that go from
busier to a break or transition. A program always starts on scene A.

| Program | A | B | C | D |
|---|---|---|---|---|
| Bar Fills | 1/16 repeat on beat 4 of half the bars | half-bar rolling fill from beat 3 | 1/8 roll-up over the last half of every 2 bars | a half-bar tape stop at the end of every 4 bars |
| Glitch Storm | busy 1/32 glitches on every beat | sparser | nearly constant, more reverse, decaying | triplet slices, lower, rolling, 80% mix |
| Tape Breaks | half-bar tape stops from beat 3 | tape stops and rolls | a whole-bar stop on bar 2 of every 2 | a 2-bar stop over the last two bars of every 8 |
| Snare Rolls | 1/8 roll on beat 4 | half-bar roll | a bar of roll every 2 bars | bar 4 of every 4 rolls, fading |
| Reverse Wash | backwards 1/8 washes every half bar | more often | all backwards, mostly an octave down | half-bar events, fading tails, 60% mix |
| Trance Gate | 1/16T gates, sometimes rolling | gating all the time | faster (1/32) chops | gate off: a breakdown tape stop every 4 bars |
| Hold To Play | Chance 0: Hold with 1/16 repeats | 1/8 | 1/32, half reversed | 1/8T, fading |
| Transitions | a 2-bar 1/8 → 1/16 → 1/32 roll over the last two bars of every 8 | a 1-bar roll on bar 8 | a 1-bar tape stop on bar 8 | a 4-bar build from bar 5 |

## Tips

- Put it on a drum bus or the master. On a single drum track a repeat of silence is silence.
- For live play, start from **Hold To Play** and use Hold as your stutter button, then bring
  Chance up for the variations.
- **Grid 1 Bar + Offset 1.4.1** puts all the action on the last beat of the bar, where fills belong.
  **Offset 1.3.1, Length 1/2** gives two-beat turnarounds.
- Scenes are for switching while the track plays: tap B for the build, D for the drop. To
  automate them, record the Scene pill changes (or draw them) in the sequence's automation.
- When the dice give you a bar you love, turn **Pattern** to 1 Bar while that bar is still playing:
  from the next bar on, it replays it.

## What is not implemented

- The first repeat of an event can't be reversed (it is being recorded as it plays).
- Reverse and Half Speed only apply to repeat events, not gates or tape stops.
- Hold always repeats: it doesn't roll for a tape stop or gate, and Roll doesn't apply to it.
- The tape stop slows linearly over the event; there is no separate stop-time or spin-back control.
- The dice state and a locked pattern are not saved with the project.
- Scenes have no copy/paste: to start scene B from scene A, set it up by hand (or start from a
  program, whose scenes are already related).
- Choosing the program you're already on resets only the scene in play to the program's values
  (the host gives no way to tell a re-pick from a normal knob change). Pick another program and
  back to reset all four.
- Scene has no Q-Link: all 16 are taken by the performance controls. Switch by touch or automation.
- A project saved with Wipeout 1.0 opens on scene A with its saved settings. Scenes B–D start as
  Bar Fills' scenes.
- With the transport stopped, the grid is only as good as the MPC's tempo and the first hit.
- Odd meters: the bar lengths in Length, Grid and Pattern follow the time signature; the slice sizes
  are always note values, and Offset always counts 16 sixteenths to a bar (its bar.beat display
  assumes 4/4).
- A tape stop longer than about 20 seconds (4 bars below about 48 BPM) reaches a stop early and
  stays silent for the rest of the event: the history it reads from is 10 seconds long.

## On the MPC Sample (Knob FX)

From 1.1, Wipeout runs on the MPC Sample (firmware 1.4 or later) as a Knob FX, listed as
**Wipeout** at the bottom of FX Select. The MPC Sample has no plugin screen, so six parameters
sit on its three knobs:

| | Knob 1 | Knob 2 | Knob 3 |
|---|---|---|---|
| **Knobs** | Chance | Slice | Hold |
| **SHIFT held** | Scene | Length | Reverse |

Chance and Slice set how often and how small; Hold is the stutter button (turn it past halfway
to stutter from the next slice, back to let go). Scene on SHIFT is the big one: it switches
between the program's four scenes, so the knob reaches the busier variations, the rolls and the
tape-stop transition as well as scene A. The MPC Sample starts on the first program, Bar Fills;
everything not on a knob stays at its settings (Grid 1 Bar, Offset 1.4.1, Mix 100%, Smooth 3 ms)
unless a scene changes it. Knob values show without their % sign on the MPC Sample.

To install, copy the plugin folder into `VST2 Plugins` on the MPC Sample's microSD card in SD
Card Access mode, and turn on MIDI Config → Dev Enhancements (Beta) → Custom Plug-In Support.

## CPU

Very light: a few multiplies and one linear interpolation per sample, no filters. On a desktop it
takes about 0.03% of real time with busy settings; by comparison with Largemouth (0.35% on the
desktop, 2–4% on the MPC) that is well under 1% of one MPC core. Not yet measured on the device.
Memory: 10 s of stereo history for the tape stop plus two one-beat capture buffers (about
5 MB at 48 kHz), all allocated when the plugin is prepared.

## The panel

A custom skin made from the artwork in `art/` (the wave plate; `mock-reference.webp` is the
designer's mock-up, for the look only). One tab, PLAY: four rows, each one row of Q-Links, with two
controls either side of the wave. Under the wave, level with row 4, a SCENE box holds the A–D
pills and Smooth (neither has a Q-Link).

| Row | Left | Right | Q-Links |
|---|---|---|---|
| 1 | TRIGGER: Chance, Hold | TIMING: Slice, Length | 13–16 |
| 2 | MANGLE: Reverse, Half Speed | BREAK: Tape Stop, Gate | 9–12 |
| 3 | SHAPE: Roll, Decay | DICE: Vary, Pattern | 5–8 |
| 4 | GRID: Grid, Offset | OUTPUT: Mix, Output | 1–4 |

`ui.json` (the stock cyan kit, the same rows) is not used while `skins/` exists. The skin binds to
parameter numbers, so after changing parameters, render it before building:

```
python3 plugins/Wipeout/art/make_skin.py      # writes skins/ and build/skin-preview-1.png
python3 tools/build.py plugins/Wipeout
```

## Testing

`tests/listentest.cpp` plays a 120 BPM drum loop with a bassline through Wipeout, transport
playing, and checks each piece sample by sample: events land on the grid (to the sample) and
repeats on the slice grid, repeats are exact copies of the captured slice, the dry track is
untouched between events, Reverse plays the slice backwards, Half Speed plays half of it at half
speed, the tape stop is an octave down halfway and silent at the end, the gate's off-halves are
silent, Roll halves the slice on schedule, a sine through every effect at once has no step larger
than its own (no clicks, including Hold interrupting events), Hold starts on the next 1/16, Pattern
replays one bar and eight bars exactly, an 8-bar grid puts a 2-bar roll on the last two bars of the
phrase, a 4-bar tape stop slows evenly, the stopped-transport clock starts on the first hit, and loop jumps resync.
For scenes it checks that switching recalls and edits stay with their scene, that Output, Hold and
Smooth stay put, that scenes survive a save and reload (and a 1.0 project still loads), that a
program brings its four scenes, that switching A → D changes what plays from the next roll, and that
switching mid-event doesn't click.
It also renders a 16-bar demo (Bar Fills, Snare Rolls, Tape Breaks, Glitch Storm, four bars each):

```
mkdir -p tests/build
clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
tests/build/listentest build/listen.wav
```

## Licence

MIT. Original design and code.
