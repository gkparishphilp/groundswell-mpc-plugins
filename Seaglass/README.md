# Seaglass

**A lush reverb you can freeze.** A stereo reverb effect for Akai MPC standalone hardware, by Groundswell.

Seaglass is a smooth hall-style reverb with three extra tricks. **Freeze** holds whatever is in the tail as an
endless pad and lets it go without a click; **Add** layers the next note on top, so you can build a frozen chord
one note at a time, and **Live** keeps the normal reverb playing beside the pad. **Shimmer** pitch-shifts the tail inside
the feedback loop, so every pass rises and the tail sparkles: by any interval up to two octaves up or down
(**Shift**), or by a few cents for a slow, drifting wash. **Swell** keeps the tail down while you play and lets it bloom back in the gaps, the way
Tidepools waits for you to stop.

It picks up where Abyss left off: Abyss's README lists a reverb freeze and a true shimmer tail as still to come,
and Seaglass is built on Abyss's reverb.

## New in 1.2

- **Shift** replaces Interval: one bipolar dial for the shimmer's pitch. The centre is no shift; the middle fifth
  of its travel is cents (up to ±100, finest at the centre) for a slowly rising or sinking, chorus-like wash; beyond
  that it steps through whole semitones up to ±24, so every interval lands exactly (+7 st is a fifth, +12 an
  octave, -5 a fourth down, +19 an octave and a fifth). It reads "+15 ct" or "+7 st".
- For big shifts the shifter's grains get longer (at least ~45 ms each), so ±2 octaves sounds smoother than 1.1's
  2 Oct did.
- The shimmer's limiter now shuts the feed completely as the tail passes its ceiling, so even Shift at 0 (where the
  shimmer is plain extra feedback) can't run away.
- Two new programs: **Drift Glass** (+14 ct: a slow detuned wash) and **Bell Buoy** (+19 st: an octave and a
  fifth). Moon Pool and Kelp Shadow now use Shift (+7 and -12) and sound as before.
- **Projects saved before 1.2** keep their Interval: while Shift sits at its default (+12) and the old Interval is
  Fifth, 2 Oct or Oct Down, that Interval still applies. Move Shift and Shift takes over.

## New in 1.1

- **Add**: while Freeze is on, every tap of Add puts what the reverb is sounding now on top of the frozen pad.
  Freeze a note, play the next, tap Add, play the next, tap Add: a frozen chord, built one note at a time.
- **Live**: the frozen pad now lives beside the normal reverb instead of replacing it, so the reverb keeps playing
  on whatever you play over the pad. Live sets how loud it is while Freeze is on (100 % = as normal, 0 % = 1.0's
  behaviour, the pad alone).
- **A loudness guard** eases the pad down if stacked layers get loud (about -10 dBFS RMS).
- **Two tabs** on the panel (SEA and DETAIL), and two new programs, **Glass Chord** and **Shore Layers**.
- **Projects saved with 1.0** load with Live at 100 %, so while frozen they now hear their reverb as well as the
  pad. Set Live to 0 % for 1.0's sound. Fade now also sets how long a capture keeps soaking up what you play.

## Install

1. Copy the folder `Groundswell - VST - Seaglass` (in `build/` after building) into a folder named
   `VST2 Plugins` on a drive the MPC can see. The internal drive is the safest choice.
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC. Seaglass appears under **External Plugins** in the effect browser. Put it on a track or
   program insert, or on a return with Mix at 100 %.

## How it works

```
in -+------------------------------------------------------------------- dry ---+
    +-> Low Cut -> Pre-Delay -> (+ shimmer) -> 4 diffusers -> 8-line FDN -> wet  +-> Mix -> Output
                                    ^                                    |
                                    +-- pitch shifter <- filters <-------+  (Width, Swell duck on the wet)
```

- **The reverb** is an 8-line feedback delay network (delays of 35–93 ms, a Hadamard mix, a low-pass on each
  line for **Tone**, a gain on each line for **Decay**). Four of the lines drift slowly in length (**Motion**),
  which gives the tail a gentle chorus and stops it ringing metallically. Four allpass diffusers per side in
  front of it smear drum hits into a wash instead of a flutter of echoes. The input is scaled with the decay so a
  held sound comes out at about the same level whatever the Decay.
- **Freeze and Add** use up to three copies of the reverb network, sharing the same delay lines' timing and
  modulation: **LIVE** (the normal reverb, always running), **PAD** (lossless: feedback exactly 1, no damping, no
  input) and, while a capture is happening, **STAGE**. Freeze (and each Add) *moves* everything the live reverb is
  holding into STAGE, leaving the live reverb empty. Because the network is linear, the sound at that instant
  doesn't change at all. Over **Fade**, STAGE blends from the live decay and damping to lossless and rises from
  the live level to full, while the input cross-fades from STAGE back to LIVE, so what you play during Fade
  soaks into the capture. Then STAGE is added into PAD (again exactly, so again nothing changes at that instant).
  The pad holds its level and colour indefinitely (tested: within 1.5 dB over 31 s; three notes added one at a
  time each held within 1 dB of freezing them alone). The live reverb meanwhile refills with whatever you play
  next, heard at the **Live** level. Turning Freeze off blends the pad back to the normal decay over Fade, and it
  dies away with the current Decay time. Nothing is ever switched, only blended, so even a 20 ms Fade does not
  click. A tap (or Freeze off) that arrives while a capture is still fading in waits for it to finish (at most
  one Fade); taps that pile up meanwhile count as one.
- **Shimmer** takes the live tail, filters it (two low-passes so the shifted copy doesn't alias, a high-pass so
  downward shifts don't pile up mud), shifts it by **Shift** with a cheap two-grain pitch shifter (two read heads
  half a window apart, crossfaded; the window is 70 ms, longer for big shifts) and feeds it back into the reverb
  input. Each trip round the loop shifts it again, so an octave climbs to two octaves, then three, until Tone and
  the filters roll it off; a few cents drift the whole tail slowly up or down. A limiter shuts the shimmer feed as
  the tail passes about -10 dBFS RMS, and a soft clip guards the feed, so every setting stays bounded (with
  Decay 30 s and Shimmer 100 % a shifted tail peaks around -16 dBFS RMS and fades away; at Shift 0, where the
  shimmer is just extra feedback, it holds steady at about -8 dBFS RMS).
- **Swell** follows the level of the input. While it is above **Sense**, the wet signal is ducked (by up to the
  **Swell** amount, in 60 ms). When you stop, it comes back over **Rise** seconds on an S-curve, so the tail
  blooms in. The reverb keeps filling while it is ducked, so the full tail is there to bloom.

## Parameters

| Block | Parameter | Range (default) | What it does |
|---|---|---|---|
| SPACE | Mix | 0–100 % (35) | Dry/wet balance, equal power. 100 % = wet only (for returns). |
| | Decay | 0.5–30 s (5 s) | Reverb time: how long the tail takes to fall by 60 dB. |
| | Pre-Delay | 0–250 ms (20) | Gap before the reverb starts. Glides when you turn it (a short pitch bend in the tail). |
| | Tone | 1–16 kHz (7 kHz) | Damping inside the tail: lower is darker, and the highs die sooner. |
| COLOUR | Low Cut | 20 Hz–1 kHz (120 Hz) | High-pass on the way into the reverb, so kicks and bass don't boom. |
| | Motion | 0–100 % (40) | Slow movement of four delay lines: a gentle chorus in the tail. 0 = still. |
| | Width | 0–100 % (100) | Stereo width of the tail. 0 = mono reverb. |
| | Output | -24 to +6 dB (0) | Level after the mix. |
| FREEZE / PAD | Freeze | Off / On | Hold what the reverb is sounding as an endless pad. Off lets the whole pad die away. |
| | Add | tap | Every tap (every change, either way) layers what the reverb is sounding now onto the pad. Shows the layer count. Does nothing while Freeze is off. |
| | Fade | 20 ms–4 s (250 ms) | How long a capture keeps soaking up what you play, and how long the pad takes to let go. Long values swell into the freeze. |
| | Live | 0–100 % (100) | How loud the normal reverb is beside the pad while Freeze is on. 0 % = only the pad. |
| SHIMMER | Shimmer | 0–100 % (0) | How strongly the pitch-shifted tail is fed back. 30–60 % is a classic shimmer. |
| | Shift | -24 to +24 st (+12 st) | The shimmer's pitch change on each trip round the loop. Centre = none; the middle fifth of the dial is cents (±100 ct), beyond it whole semitones. |
| (hidden) | Interval | Octave / Fifth / 2 Oct / Oct Down | 1.0/1.1's shift. Not on the panel; applies only while Shift sits at its default (old projects). |
| SWELL | Swell | 0–100 % (0) | How far the tail ducks while you play. 100 % = gone while you play. |
| | Rise | 0.1–8 s (1.5 s) | How long the tail takes to bloom back once you stop. |
| | Sense | -60 to -12 dB (-36) | Input level that counts as playing. Raise it for loud material, lower it for quiet pads. |

Parameter order and ids are permanent (new parameters will only ever be added at the end), so projects keep
their settings in later versions.

## Freeze, Add and the Q-Links

Effects on the MPC receive no MIDI, so Freeze and Add are parameters, not notes. On the SEA tab they are
**Q-Link 9** (Freeze) and **Q-Link 10** (Add), with Fade and Live beside them on 11 and 12.

**Building a frozen chord:**
1. Play the first note (or chord). While it rings, turn Freeze on.
2. Play the next note. While it rings, tap Add.
3. Repeat. Add's value shows how many layers the pad holds. Turn Live down if you want to hear only the pad.
4. Turn Freeze off to let the whole chord fade away at the Decay time.

- **On the screen**, tap the Freeze buttons (Off / On) and the Add button. Every tap of Add adds a layer.
- **On a Q-Link**, a switch is on in the top half of the knob's travel and off in the bottom half. For Freeze,
  turn up to freeze and down to let go. For Add, every crossing of the half-way point adds a layer, in either
  direction, so a small turn back and forth adds one note at a time.
- **Fade**: short (20–100 ms) catches a moment exactly; long (1–2 s) soaks up what you play for that long and
  lets the pad melt away slowly.
- **In a sequence**, record the Q-Link moves (or draw them as automation) to freeze and add at points in the song,
  for example the last chord of a phrase under a breakdown, released on the drop.

The input always plays dry (at the Mix balance) over the pad. Swell still works on a frozen pad: with Swell up,
the pad ducks under what you play and comes back in the gaps. Shimmer is fed only by the live reverb; whatever
shimmer is sounding when you capture is held in the pad.

## The panel

Two tabs drawn over your own artwork: the Seaglass title over a sunset beach, with glowing sea glass on the
pebbles. The controls sit in dark glass boxes down each side, so the title, the wave and the glass stay in view
in the middle. Knob arcs are seafoam and headings lavender, taken from the glass. Each row is one row of Q-Links:

| Tab | Row | Left box | Right box | Q-Links |
|---|---|---|---|---|
| SEA | 1 | SPACE: Mix, Decay | TAIL: Pre-Delay, Tone | 13–16 |
| | 2 | FREEZE: Freeze (Off / On buttons), Add (tap button + layer count) | PAD: Fade, Live | 9–12 |
| | 3 | SHIMMER: Shimmer, Shift (bipolar knob) | SWELL: Swell, Rise | 5–8 |
| | 4 | COLOUR: Low Cut, Motion | OUTPUT: Width, Output | 1–4 |
| DETAIL | 1 | SWELL: Swell, Rise | SENSE: Sense | 13–15 |
| | 2 | COLOUR: Low Cut, Motion | OUTPUT: Width, Output | 9–12 |

The skin is rendered by `art/make_skin.py` into `skins/`; run it before `tools/build.py` whenever the art or the
layout changes (the build copies `skins/` as it is). `ui.json` is a stock-kit fallback layout and is not used while
`skins/` exists.

## Programs

| Program | Character |
|---|---|
| Seaglass | The default: a 5 s hall at 35 %. |
| Frosted Shore | Short, bright and airy (2.8 s), for drums and vocals. |
| Wave-Worn | Long and dark (12 s, Tone 2.6 kHz), lots of motion. |
| Tumbled Light | Classic octave shimmer (55 %), 10 s. |
| Moon Pool | Shimmer up a fifth (Shift +7) over a long, wide 14 s tail with 60 ms pre-delay. |
| Kelp Shadow | Shimmer an octave down (Shift -12): a dark tail that sinks. |
| Cobalt Bloom | Swell 85 %: the tail waits for gaps, with a touch of shimmer. |
| Ghost Tide | Swell 100 % with a slow 4 s rise over an 18 s shimmering tail. |
| Endless Shoal | Ready to freeze: 20 s decay, 1.2 s Fade, Mix 60 %, Live 30 %. |
| Glass Chord | Built for Freeze + Add: 6 s decay, 400 ms Fade, Mix 70 %, Live 60 %. |
| Shore Layers | Freeze + Add with the reverb fully live and a touch of shimmer: 10 s, 900 ms Fade, Mix 55 %. |
| Drift Glass | Shift +14 ct, Shimmer 70 %, 16 s: the tail slowly rises and smears like a detuned chorus. |
| Bell Buoy | Shift +19 st (an octave and a fifth), Shimmer 45 %, bright, Low Cut 250 Hz: bell-like overtones. |

## Tips

- **Build a chord**: in Glass Chord, play a bass note, Freeze (Q-Link 9); play a fifth above, nudge Add (Q-Link 10);
  then a third, a seventh... Then play a melody over it: Live puts the reverb on the melody too.
- **Freeze a chord, then play over it**: play a chord into Endless Shoal, turn Q-Link 9 up, keep playing. Drop Mix
  for less pad, or put Seaglass on a return so the frozen pad sits under several tracks.
- **Freeze a drum hit** for a drone: freeze just after a snare with Tone low and Motion up.
- **Shimmer on a return**, fed a little of pads or keys, gives the "choir in the rafters" sound without cluttering
  the dry track. Keep Low Cut up (200 Hz+) so only the top climbs.
- **Swell on drums**: Swell 100 % and Rise 0.3–0.6 s keeps the groove dry and lets the room bloom only in the
  breaks and at the end of fills. Use Sense to choose what counts as a hit.
- For a frozen pad that slowly fades rather than holds, leave Freeze off and set Decay to 30 s instead.

## CPU

On a desktop, Seaglass takes about **0.14 % of real time** as a plain reverb, **0.20 %** averaged over the 80 s
demo (pad, shimmer and swell in use), and **about 0.5 %** in the worst case: a capture fading in over a held pad with
Shimmer on, which runs all three networks at once (for at most one Fade). By the ratio seen with the other
Groundswell plugins that should be roughly **1–2 % of one MPC core** normally and **up to about 5 %** during a
capture. These are estimates from the desktop, not yet measured on the hardware. The pad network runs only while
there is a pad (it stops once a released pad has died away), the capture network only during a capture, and a
capture or merge copies about 22,000 samples once.

How it stays cheap: everything that needs `exp`, `pow` or `log` (loop gains, damping, filter coefficients, the
modulation, the swell detector) is worked out once per block and ramped through it. Per sample it does 8 delay
reads, 4 allpass interpolations for the moving lines, a 24-addition Hadamard mix, 8 one-pole filters, 8 diffuser
allpasses, and, only while Shimmer is above 0, the shimmer path (6 one-pole filters, 4 interpolated reads).

## What is not in this version

- **No Size control.** The delay lengths are fixed (35–93 ms, a medium-large hall); Decay sets the reverb time and
  Pre-Delay the distance. Changing the room size live without artefacts would need interpolated reads on all eight
  lines.
- **One pad, not separate slots.** All the layers live in one pad, so they can only be let go together (Freeze
  off). Releasing notes one by one would need a network per layer, at a CPU cost each.
- **Add captures what the reverb is sounding, not the dry note.** Tap it while the note is still ringing in the
  reverb; longer Decay makes the timing easier. There is no "keep layering everything I play" mode (the pad would
  grow without end).
- **Pure tones come out of the pad at different strengths**: an 8-line reverb colours single sine-like notes by
  several dB (the plain reverb does the same). Rich sounds even this out; Motion smooths it over time.
- **With Live below 100 %, Freeze off brings the live reverb back** with whatever you played while frozen (it kept
  listening). A short Fade makes that a quick swell.
- **No hold-to-freeze button yet**: Freeze is a toggle on the screen and a half-way threshold on the knob. The MPC's
  skin format documents a momentary handle (`ResetOnGestureEnd`) that could make an on-screen button freeze only
  while held; not tried yet.
- **Shimmer is a simple two-grain shifter**, not a formant-preserving one. Its grains flutter lightly (at 14 Hz
  for an octave), its two heads act as a gentle comb (a few dB of ripple every ~28 Hz), and its output comes in
  lines spaced at the grain rate, the strongest within about half that rate of the exact interval (for an octave
  from 440 Hz, up to ~7 Hz off; for cents, almost exact). The reverb smears all of this into the tail, but the
  shimmer is a sparkle, not a precise harmoniser.
- **Shimmer at 100 % and the longest Decay doesn't sustain forever**: energy climbs out of range and the tail fades.
  For an endless shimmer pad, let it bloom, then Freeze.
- **Swell watches the input level only**, not the tempo or the notes. It is not locked to the song.
- **Turning Shift through the semitones** steps the shimmer's pitch at once at each semitone (a pitch change in
  the shimmer, not a click; tested). Big changes also glide the grain window over 100 ms.
- **At Shift 0 (or within a cent or two) with Shimmer high and a long Decay**, the shimmer is extra feedback and
  the tail can hold indefinitely, kept at about -8 dBFS by the limiter. Use it as a soft hold, or move Shift off
  the centre.

## Testing

All checks run on the computer:

- `python3 tools/build.py plugins/Seaglass`: native, arm32 and arm64 builds and the folder validation, 0 errors,
  0 warnings.
- `.sdk-tools/host_sim plugins/Seaglass/build/native/seaglass.dylib test --seconds 6` (and `--block 32`, `--block 512`):
  20 pass, 0 warn, 0 fail. host_sim has no option to feed a file, so it uses its own test signal (a 220 Hz sine
  and a noise burst, mono); the stereo material is in the listen test below.
- `tests/listentest.cpp` (115 checks) feeds synthesised stereo material: a pad chord detuned between left and
  right, a sine, steady noise and a drum loop with hats panned left and right. It checks:
  - every program is finite, under 0 dBFS and leaves a tail;
  - every one of the 18 controls changes the sound on its own (Freeze and Add switched during the render);
  - Freeze (Live 0 %) holds a chord for 31 s (1 s windows within 1.5 dB, no drift, spectrum steady). With noise
    playing straight through, engaging holds the level and releasing is smooth (Fade 250 ms and 20 ms); with
    Live 100 % the pad and the refilling live reverb settle about 2.6 dB above the reverb alone, reached smoothly.
    New input never reaches the pad (drums change it by -146 dB). It lets go at the Decay rate. No clicks anywhere,
    including frozen with Shimmer and under a playing chord at Mix 35 %. The click detector self-tests on the
    same frozen audio and catches a 0.03 step in the pad and a 0.002 step in the release tail;
  - Freeze + Add: C, E and G frozen one at a time are each held within 1 dB of freezing that note alone, without
    Add only the first is held, the chord holds for 22 s, no clicks (Live 0 % and 100 %), Add shows "3 layers";
    a note played over the pad is heard through the reverb in proportion to Live (+26 dB at 50 %, +32 dB at 100 %
    against 0 %); Add does nothing while Freeze is off; quick taps during a capture are smooth; eleven layers of a
    loud chord are held down to about -12 dBFS; a released pad stops running; a project loaded with Freeze and
    Add on adds nothing;
  - Shimmer puts energy at 440 Hz (Octave), 330 Hz (Fifth), 880 Hz (2 Oct) and 110 Hz (Oct Down) from a 220 Hz
    tone (the old Interval, still honoured), and the octave keeps climbing to 880 Hz. Shift: the pitch shifter on
    its own lands within its grain tolerance of the exact interval for +3, +5, +7, +12, +19, -5, -7 and -12 st and
    ±50 ct (cents almost exactly); in the reverb, energy builds at +7, +3, -5 and +19 st; ±50 ct drift a chord's
    tail down and up; the dial reads "0 ct", "+25 ct", "+7 st", "-12 st", "±24 st"; a moved Shift overrides an old
    Interval. At Decay 30 s and Shimmer 100 % with a loud chord and drums, Shift +12, +7, ±24, -12, -1 st, +10 ct
    and 0 all stay bounded and finite, and turning Shift through its whole range while a tail rings is click-free;
  - Swell 100 % keeps the tail down while a chord plays (gone), 50 % ducks part way, the tail blooms back over
    Rise after the chord stops, and on drums with gaps it is held 58 dB down under the hits and fully back in
    the gaps;
  - random parameter changes every 50 ms stay finite and under 0 dBFS, at 44.1, 48 and 96 kHz;
  - an 80 s demo: drums and chords → a chord built one note at a time with Freeze and Add → a melody over it
    with the live reverb → release → shimmer → swell on chord stabs → a +14 ct drifting wash.

```
cd plugins/Seaglass
mkdir -p tests/build
clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
tests/build/listentest build/seaglass-demo.wav
```

Not yet tried on the MPC itself.

## Project layout

| Path | What |
|---|---|
| `plugin.json` | Name, manufacturer, uid `GvXf`, version |
| `src/Seaglass.cpp` | All the DSP, parameters and programs |
| `art/` | Your plate (`plate-src.webp`), the shared knob and pill art, and `make_skin.py` |
| `skins/` | The rendered panel (TUI.json, Q-Links.json, images); copied into the build as is |
| `ui.json` | A stock-kit fallback layout (unused while `skins/` exists) |
| `tests/listentest.cpp` | The offline listen test and demo render |

## Licence

MIT, like the rest of the Groundswell plugins. Original code; the reverb is Abyss's (also Groundswell, MIT).
The FDN, Schroeder allpass diffusers, allpass interpolation and the two-grain delay-line pitch shifter are
textbook techniques; no code was copied from elsewhere.
