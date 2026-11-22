# Driftwood

**One pad, one chord.** A strummed chord instrument for Akai MPC standalone hardware, by Groundswell.

Set a **Key** and a **Scale**, and every note you play becomes the chord built on that scale degree, strummed
like a hand across strings and played by Driftwood's own warm voices: an electric-piano tine, a soft pad, a
plucked string and an organ, worn in a little by tape wobble and dust. Pick the chord type (triad, 7th, 9th, sus2,
sus4, add9) and voicing, and let **Voice Lead** keep each new chord close to the last one, the way a keyboard player
moves their hands as little as they can. Turn on a **Pattern** and Driftwood re-strums the held chord on the beat
like a rhythm guitarist, or turn on the **Arp** and it plays the chord one note at a time. Both lock to the MPC's
transport.

Plugins can't send MIDI to the MPC, so Driftwood can't drive your other instruments. It makes its own sound.

## Install

1. Copy `build/Groundswell - VST - Driftwood/` into a folder named `VST2 Plugins` on a drive the MPC can see (the
   internal drive is safest).
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC. Driftwood is listed under **External Plugins** (Synth).

## Which note plays which chord

### Note Map: Keys (default, for keyboards)

The note's pitch picks the scale degree, and the chord's root is that note. In C major, D plays D minor (ii), G
plays G major (V), B plays B diminished (vii°).

Notes that aren't in the scale follow **Off-Key**:
- **Snap**: the nearest scale note's chord. A note halfway between two scale notes (C# in C major) snaps **down**
  (C# → C, F# → F, Eb → D minor).
- **Own Chord**: the major-family chord on that note (major triad, dominant 7th, dominant 9th, sus2, sus4, add9 for
  the six qualities). That gives you the borrowed chords (Bb, Ab and Eb in C major: bVII, bVI, bIII) and the
  secondary dominants (D, E and A major in C: II, III, VI).

### Note Map: Pads (for the 16 MPC pads)

Every note is the next scale degree, so the pads walk up the key, with no out-of-key chords at all. Note 36
(pad 1 of bank A on a plugin track) is degree I:

```
            pad 13 = I''  (48)    pad 14 = ii'' (49)    pad 15 = iii''(50)    pad 16 = IV'' (51)
            pad 9  = iii' (44)    pad 10 = IV'  (45)    pad 11 = V'   (46)    pad 12 = vi'  (47)
            pad 5  = V    (40)    pad 6  = vi   (41)    pad 7  = vii  (42)    pad 8  = I'   (43)
            pad 1  = I    (36)    pad 2  = ii   (37)    pad 3  = iii  (38)    pad 4  = IV   (39)
```

`'` is an octave up, `''` two octaves up. In A major that's A, Bm, C#m, D / E, F#m, G#°, A' / C#m', D', E', F#m' /
A'', Bm'', C#m'', D''. Notes below 36 carry on down the scale (35 = vii below pad 1). Degree I of pad 1 sits on the
tonic at or just below **Register** (Register C4 and Key A put pad 1 on A3).

If your pads send different notes (another bank, or a custom pad layout), the rule stays the same: each semitone
up is one scale step up, counting from note 36.

### Building the chord

| Quality | Tones | Notes |
|---|---|---|
| Triad | root, 3rd, 5th from the scale | major, minor or diminished depending on the degree |
| 7th | + the scale's 7th | maj7, m7, dominant 7, half-diminished, m(maj7)... |
| 9th | + the scale's 9th | five notes |
| Sus2 | root, major 2nd, perfect 5th | always a true sus chord; on vii in major the 5th is outside the key |
| Sus4 | root, perfect 4th, perfect 5th | the same |
| Add9 | triad + the scale's 9th | on iii in Phrygian or vii in major the 9th is a flat 9: crunchy |

- **Voicing**: **Close** (stacked within an octave), **Open** (drop-2: the second-highest note of the close chord goes
  down an octave), **Spread** (the 2nd and 4th notes from the bottom go up an octave: wide and airy).
- **Inversion**: raises the lowest 1, 2 or 3 chord tones an octave (1st, 2nd, 3rd). On a triad, "3rd" is root
  position an octave up.
- **Voice Lead** (on by default): ignores Inversion (except for the very first chord) and picks the inversion and
  octave that move least from the previous chord, keeping the chord's centre within 7 semitones of **Register**. With
  Voice Lead on, the octave you play in doesn't matter, only the degree; with it off, a Keys-map chord is built up
  from the note you play.
- **Bass**: adds the chord's root below the voicing, **-1 Oct** (1 to 12 semitones under the lowest note) or
  **-2 Oct** (an octave further down). It isn't part of the voice leading, so it follows the roots.

## Strum and feel

- **Strum**: time from the first note of the chord to the last (0 = a block chord, up to 500 ms for a slow harp
  roll). It is spread evenly over the chord's notes.
- **Direction**: **Down** plays low to high (a guitar downstroke), **Up** high to low, **Alt** alternates down and
  up on each chord, **Velocity** strums up on soft hits (below 64) and down on hard ones.
- **Vel>Strum**: how much harder hits strum faster. At 100 % a full-velocity hit strums in a quarter of the Strum
  time and a very soft one takes 1.75 times as long.
- **Loose Time**: every note starts up to this much late, at random (0–40 ms).
- **Loose Vel**: random velocity spread of each note (±20 % at 50 %).

## Pattern (rhythm guitar)

With **Pattern** on, a pad doesn't strum the chord directly: it sets the chord, and the pattern strums it on its
own rhythm for as long as you hold the pad (or until the next pad, with **Hold** on). Patterns are 16 steps of
sixteenth notes (one bar of 4/4 at **Pat Speed** Normal; Half stretches it over two bars, Double squeezes it into
half a bar):

| Pattern | Steps | Feel |
|---|---|---|
| Quarters | `D...D...D...D...` | four on the floor |
| Eighths | `D.U.D.U.D.U.D.U.` | steady down-up |
| Folk | `D...D.U...U.D.U.` | the classic "down, down-up, up-down-up" |
| Tresillo | `D..D..D...D.U.U.` | 3-3-2 |
| Ballad | `D.......D...U.U.` | open and slow |
| Gallop | `D.DUD.DUD.DUD.DU` | driving sixteenths |
| Skank | `..K...K...K...K.` | offbeat chops |
| Funk 16 | `DXUXDUXUDXUXDUXU` | sixteenths with muted chucks |

`D` down stroke (every note, low to high), `U` up stroke (the top four notes, high to low, never the bass), `X`
muted chuck (every note, choked after 30 ms), `K` chop (the top notes, short). Each stroke chokes the one before,
like a hand going back to the strings. Beats are accented, then the off-beat eighths, then the sixteenths. Strokes
use the **Strum** time (capped at 60 % of a step so fast patterns stay tight); Direction is ignored while a Pattern
is on.

- **While the MPC plays**, the pattern is locked to the song position (step 1 on every bar line, assuming 4/4 from the
  start of the sequence). Hit a pad up to 40 ms after a step and that step plays at once, so a slightly late hand
  still lands on the beat; otherwise the chord waits for the next stroke.
- **While the MPC is stopped**, the pattern runs free at the sequence's tempo, starting with step 1 on the note you
  play.
- **Swing** delays the off-beat sixteenths, MPC-style: 50 % straight, 66 % a triplet feel.

## Arp (arpeggiator)

With **Arp** on, a pad sets the chord and Driftwood plays its notes one at a time on the same clock as Pattern
(locked to the song while the MPC plays, free from the note when it's stopped, with the same 40 ms late catch).
The Arp overrides Pattern while both are on.

- **Arp**: **Up**, **Down**, **Up-Down** and **Down-Up** (the top and bottom notes aren't repeated at the turns),
  **Random** (never the same note twice in a row), or **Thumb**: the lowest note on every other step with the others
  going up in between, like fingerpicking. Turn on **Bass** and the thumb plays the bass.
- **Arp Rate**: 1/4, 1/8, 1/8T, 1/16, 1/16T or 1/32. **Swing** applies to the straight rates (not the triplets).
- **Arp Oct**: the chord repeated over 1, 2 or 3 octaves.
- **Arp Gate**: how long each note is held, as a share of a step (10–100 %). After that, **Release** lets it ring,
  so a long Release with a pad or keys makes a shimmering wash, and a short one makes it staccato.
- The arp plays every note of the voicing, Bass included, so Quality, Voicing and Voice Lead shape it too. Beat notes
  are slightly accented; Loose Time and Loose Vel humanise it.

## Hold, pedal

- **Hold** latches: chords keep sounding after you let go of the pad, and the next pad replaces them (tapping the same
  pad again re-strums it). Switching Hold off releases whatever isn't still held down. With a Pattern or the Arp, Hold keeps
  it going on the last chord.
- The sustain pedal (CC 64) holds chords until it is lifted. All-notes-off releases everything.

## The voices

Every voice reads **Tone**, **Attack** and **Release**; the voice in use when a note starts keeps playing that note
even if you switch.

- **Keys**: an electric-piano tine. Two-operator FM at 1:1, its brightness (the modulation index) dropping from a bark
  to a mellow sustain over half a second to a second and a half, a fast bell partial at 7x, and a little pickup
  asymmetry. Decays like a real tine (longer for low notes); Release is the damper. Tone = brightness and bark;
  velocity brightens it too.
- **Pad**: two anti-aliased saws ±7 cents apart and a sub sine an octave down, through a gentle per-voice low pass
  that opens as the note swells in. A minimum 120 ms swell is built in; Attack adds to it. Tone = cutoff.
- **Pluck**: a plucked string (Karplus-Strong waveguide, the loop from Hammerhead's Pluck): a noise pick of one
  period, darker for soft hits, comb-filtered by a pick position 18 % along the string, then a delay line, a
  one-pole loss filter tuned out of the pitch and a loop gain set from the decay time (about 6 s low, 1 s high).
  Tone = brightness and sustain. Release is a hand on the strings.
- **Organ**: drawbars at the sub-octave, 1, 2, 3, 4 and 6x the note from one sine table, plus key click. Tone pulls in
  the upper drawbars and the click. Partials above 18 kHz are left out.

**Width** spreads the chord across the stereo field, low notes left. **Filter** is a gentle low pass on the whole
instrument (Open at the top). **Velocity** sets how much pad velocity changes the level (0 % plays every note at full
level; the brightness of Keys and Pluck still follows velocity).

## Lo-fi and space

The output runs Filter → **Chorus** (two modulated delays a quarter-cycle apart) → **Room** (a small four-line
feedback network, about 0.8 s) → **Warmth** (tape saturation: unity gain on quiet material, rounder and a
little compressed when loud, with a touch of even harmonics) → **Wobble** (tape wow at 0.5 Hz with a wandering rate, plus 6.5 Hz flutter; ±10
cents at 100 %) → **Dust** (record crackle, tape hiss and a little top-end roll-off) → **Output** → a soft limiter
that is transparent below -3 dBFS. Dust rides on the sound: it fades in with the notes and out about two seconds
after the last tail, so an idle Driftwood is silent.

## Parameters

| # | Name | Range | What it does |
|---|---|---|---|
| 0 | Output | -40..+6 dB | Output level |
| 1 | Key | C..B | Tonic |
| 2 | Scale | Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Harm Minor, Mel Minor | Scale the chords come from |
| 3 | Quality | Triad, 7th, 9th, Sus2, Sus4, Add9 | Chord type |
| 4 | Voicing | Close, Open, Spread | Spacing |
| 5 | Inversion | Root, 1st, 2nd, 3rd | Lowest tones raised an octave (Voice Lead off, or first chord) |
| 6 | Voice Lead | Off/On | Smallest movement between chords |
| 7 | Register | E2..C6 | Centre of the chords (Voice Lead) and octave of pad 1 (Pads map) |
| 8 | Bass | Off, -1 Oct, -2 Oct | Root doubled below |
| 9 | Note Map | Keys, Pads | How notes pick degrees |
| 10 | Off-Key | Snap, Own Chord | Out-of-key notes (Keys map) |
| 11 | Strum | Block..500 ms | First note to last |
| 12 | Direction | Down, Up, Alt, Velocity | Strum order |
| 13 | Vel>Strum | 0..100 % | Harder = faster strum |
| 14 | Loose Time | 0..40 ms | Random lateness per note |
| 15 | Loose Vel | 0..100 % | Random velocity per note |
| 16 | Voice | Keys, Pad, Pluck, Organ | Sound engine |
| 17 | Tone | 0..100 % | Brightness (per voice, see above) |
| 18 | Attack | 1 ms..3 s | Fade-in |
| 19 | Release | 10 ms..5 s | Fade-out after the pad is released |
| 20 | Filter | 300 Hz..Open | Low pass on the whole instrument |
| 21 | Wobble | 0..100 % | Tape wow and flutter |
| 22 | Dust | 0..100 % | Crackle, hiss, top-end roll-off |
| 23 | Chorus | 0..100 % | Stereo chorus |
| 24 | Room | 0..100 % | Small room |
| 25 | Hold | Off/On | Latch |
| 26 | Pattern | Off, Quarters, Eighths, Folk, Tresillo, Ballad, Gallop, Skank, Funk 16 | Rhythm-guitar re-strum |
| 27 | Pat Speed | Half, Normal, Double | Pattern step length (1/8, 1/16, 1/32) |
| 28 | Swing | 50..75 % | Off-beat sixteenths late |
| 29 | Width | 0..100 % | Stereo spread of the chord |
| 30 | Arp | Off, Up, Down, Up-Down, Down-Up, Random, Thumb | Arpeggiator (overrides Pattern) |
| 31 | Arp Rate | 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 | Arp step length |
| 32 | Arp Oct | 1..3 oct | Octaves the arp climbs through |
| 33 | Arp Gate | 10..100 % | Share of a step each arp note is held |
| 34 | Warmth | 0..100 % | Tape saturation |
| 35 | Velocity | 0..100 % | How much velocity changes the level |

Parameter IDs and their order are permanent; new parameters will be added at the end.

## The panel

A custom skin on Groundswell's Driftwood art: a guitar body built from driftwood on a beach at sunset. Each page has
three Q-Link banks laid out like the art: a column of four on the left board, a column of four on the right board,
and a row of four along the bottom plank. The soundhole and strings stay clear. Every control has its name and live
value beside it; on/off switches are teal pills (lit = the second choice: PADS, OWN chord, LEAD, HOLD), and choice
lists are rotary selectors with a detent dot per choice.

| Page | Left board | Right board | Plank |
|---|---|---|---|
| CHORD | KEY: Key, Scale, Note Map, Off-Key | CHORD: Quality, Voicing, Inversion, Bass | Voice Lead, Register, Hold, Velocity |
| RHYTHM | STRUM: Strum, Direction, Vel>Strum, Loose Time | ARPEGGIO: Arp, Rate, Octaves, Gate | Pattern, Speed, Swing, Loose Vel |
| SOUND | VOICE: Voice, Tone, Attack, Release | SPACE: Filter, Width, Chorus, Room | Warmth, Wobble, Dust, Output |

On a 4x4 MPC, Q-Link column 1 runs down the left board, column 2 down the right board and column 3 along the plank
(column 4 is unused). Program-mode Q-Links: Key, Scale, Quality, Voicing, Register, Bass, Strum, Pattern, Arp, Voice,
Tone, Attack, Release, Filter, Wobble, Room.

The skin is drawn by `art/make_skin.py` from `art/plate-src.webp` (the knob and pills are shared with the other
Groundswell skins). Run it before `tools/build.py`, which copies `skins/` as it is; the script refuses to run if the
parameter list no longer matches it. `ui.json` is only the fallback (stock amber kit) for when `skins/` is absent.

## Programs

| Program | Voice | Character |
|---|---|---|
| Driftwood Keys | Keys | 7ths, open voicing, a quick strum, a little tape and dust (the defaults otherwise) |
| Beach Fire Strum | Pluck | Folk pattern with a light swing, open triads around A3 |
| Sea Mist Pad | Pad | 9ths, spread, slow swell and long release, chorus, room and bass an octave down |
| Boardwalk Organ | Organ | 7ths, block chords, chorus |
| Low Tide Keys | Keys | Dark 9ths, loose timing and velocity, heavy wobble and dust, filtered |
| Harbour Harp | Pluck | Add9 spread, a slow upward roll (260 ms, faster when hit harder), big room |
| Island Skank | Organ | Offbeat chops |
| Rope Swing Funk | Pluck | Close 9ths, Funk 16 pattern with mutes, swung |
| Tidepool Arp | Keys | Spread 7ths arpeggiated Up-Down in sixteenths over two octaves, ringing, with chorus and room |
| Pier Fingerpicking | Pluck | Thumb arp in eighths with the bass on the thumb, lightly swung and loose |

Every program uses the Keys map in C major; set Key, Scale and Note Map for your song after loading one.

## Tips

- **On the MPC pads**, set **Note Map** to **Pads** and the 16 pads become every chord in the key across two octaves.
  With Voice Lead on, the octave of the pad barely matters: pads 1 and 8 both play I, voiced near Register.
- Turn **Voice Lead** off when you want the chord to follow the octave you play (bass-register stabs, high sparkle).
- For a harp or guitar roll, raise **Strum** and **Vel>Strum** together: soft hits roll slowly, hard hits snap.
- **Pattern** with **Hold** is a strumming guitarist you can leave running: tap a new pad on the beat to change chord.
- Record the pads into a sequence and Driftwood re-voices the progression for you each time you change Quality,
  Voicing or Key.

## CPU

Per voice per sample: about 15 simple operations and one or two table reads for Pluck, 20 and three reads for Keys,
30 for Pad (two saws, a sub, a filter), and 25 with up to six reads for Organ. The output stage (filter, chorus,
room, wobble, dust, limiter) is about 60. Everything that needs exp, pow or cos runs once per note or once per
32-sample tick. Voices that have died away cost nothing.

Measured on a desktop (Apple silicon), 16 voices and every effect on: 0.2–0.5 % of one core (Organ is the heaviest).
host_sim's default note: 0.13 % at 44.1 kHz. On the MPC, expect roughly 2–5 % of a core per instance, more on older
32-bit units. This hasn't been measured on hardware yet.

Polyphony: 16 voices sound at once, with 4 extra slots so a stolen voice fades out over 5 ms instead of clicking.
A chord is up to 6 voices (a 9th plus Bass), so two chords and the tail of a third fit. Stealing takes the quietest
released voice first, then the oldest.

## What is not in this version

- No per-chord editing: every degree uses the same Quality. There are no chord-slot overrides (for example a 7th on
  V only) and no slash chords other than the Bass doubling.
- Patterns are fixed: there's no pattern editor, no 3/4 or 6/8 patterns, and the bar position assumes 4/4 from the
  start of the sequence (it doesn't read the time signature).
- Pattern velocity follows the pad that set the chord; aftertouch, pitch bend and the mod wheel are ignored.
- No arp pattern editor or per-step accents, no latch-and-add (each pad replaces the chord), and the arp range
  is whole octaves only.
- The Pad has no filter envelope or LFO beyond its swell, and the Organ has no rotary speaker (use Chorus).
- Not yet tried on MPC hardware, so the CPU figure is an estimate.

## Testing

`tests/listentest.cpp` drives the plugin directly (no host) and checks, 121 checks in all:

- the chord on every degree of C major, and the quality of particular degrees in minor, harmonic and melodic minor,
  Dorian, Phrygian, Lydian and Mixolydian; 7ths, 9ths, sus and add9; snapping (ties go down), Own Chord, the pad map;
  close, open and spread voicings, inversions and the bass note;
- the notes the plugin actually starts for chords in C, Bb, E minor, E harmonic minor, G (7th), D Dorian (9th), A
  minor (sus4) and F Lydian (add9), the bass, and the pad map in A major;
- voice leading over I-vi-IV-V-I-iii-IV-ii-V-I: no voice moves more than 4 semitones, total movement less than half
  that of root positions, and 60 random chords all stay within 7 semitones of Register;
- strum order (down, up, alternate, by velocity) and timing to the sample, Vel>Strum, Loose Time and Loose Vel;
- Pattern: 16 strokes on the eighth-note grid to the sample over two bars at 100 BPM with random block sizes, swing
  at 66 %, the transport starting mid-song, free running when stopped, the 40 ms late catch, chord changes with Hold,
  and the pattern stopping when the pad is released;
- the Arp: every mode's note order (Up, Down, Up-Down, Down-Up, two octaves, Thumb with Bass, Random), sixteenths and
  eighth triplets on the grid to the sample, swing on straight rates only, Arp over Pattern, free running, Arp Gate,
  and Hold with a chord change;
- Velocity (9 dB between soft and hard at 100 %, under 1 dB at 0 %) and Warmth (rounds off a loud chord, leaves a quiet
  one within 0.1 dB);
- every choice parameter shows its own choice names;
- Hold, replace, release, and the sustain pedal;
- 24 six-note chords in 1.2 s never sound more than 16 voices and steals don't click; CPU with 16 voices on every
  engine; idle silence;
- tuning of all four voices at A2, A3 and C5 (within 3 cents; measured 0.0);
- every program's peak and RMS on a chord (within 9 dB of each other).

```
mkdir -p tests/build
clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
tests/build/listentest build/driftwood-demo.wav
```

The demo (129 s) plays D - A - Bm - G at 88 BPM on each program in order: Driftwood Keys 0:00, Beach Fire Strum
0:13, Sea Mist Pad 0:26, Boardwalk Organ 0:39, Low Tide Keys 0:52, Harbour Harp 1:05, Island Skank 1:17, Rope Swing
Funk 1:30, Tidepool Arp 1:43, Pier Fingerpicking 1:56.

`host_sim test` passes 19/0/0 at 44.1, 48 and 96 kHz, with 32-frame blocks and the transport playing, and with a
six-note chord.

## Project layout

- `plugin.json`: manifest (uid `G5eq`; never change it).
- `src/Driftwood.cpp`: parameters, MIDI, strum queue, pattern player, voices and the output stage.
- `src/ChordTheory.h`: scales, chords on each degree, voicings, voice leading.
- `src/LofiParts.h`: chorus, room, tape wobble, dust.
- `art/`: the skin generator and its artwork; `skins/`: the rendered skin (run `art/make_skin.py` first).
- `ui.json`: the fallback panel. `tests/listentest.cpp`: the checks above and the demo.

## Licence

MIT. Original design; the pluck's string loop follows Hammerhead's (also MIT, Groundswell). No third-party code.
