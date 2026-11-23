# Conch

**Hold it to your ear and the track sings back.** An MPC insert effect by Groundswell.

Conch is a bank of tuned resonators, like the sympathetic strings of a sitar, that ring at the chord
or the key. Put it on a drum loop and every hit comes out as pitched, in-key tones; put it on a mix
and it adds a singing, harmonic halo. It processes the track's own audio (nothing is played on top),
so it works the same on drums, a sample, a submix or the master.

It is cheap on CPU: up to 16 simple resonators, no FFTs, and coefficients worked out only when
something changes.

## How it works

```
input --+------------------------------------------------------------------------ dry --+
        |                                                                               |
        +--> mono --> In HPF --> Drive --> Hits (transient gate) --> tone --> excitation |
        |                                       |                              |        |
        |                                       +--> Duck envelope             v        |
        |                                                    resonator banks (3, one ringing  |
        |                                                    out while the next listens)      |
        |                                                    String / Bell / Pipe, up to 16   |
        |                                                    notes, panned by Width           |
        |                                                                      |        |
        |                                           level guard --> duck --> Mix -------+--> Output
        |
        +--> mono --> key and chord analysis (Listen sources only)
```

- **Excitation.** The input is summed to mono and high-passed (**In HPF**), so a kick's boom does
  not drown the low resonators. **Drive** saturates it for a denser ring. **Hits** gates it with a
  transient detector, so drum hits ring the resonators while held notes, pads and bass don't. At 0%
  all of the input rings them; at 100% only the first ~20–30 ms of each hit does. **Bright** also
  shades the excitation with a gentle low-pass.
- **Resonators.** Every chord tone between **Low Note** and **Spread** octaves above it gets a
  resonator: up to 16, ascending, panned alternately left and right (wider as they go up) by
  **Width**. Below C3 only the root and the fifth get one, so the low end stays clear. Two chords that
  share a note share the very same resonator pitch.
- **Materials.**
  - **String**: a sympathetic string per note (a comb filter one period long with a low-pass in the
    loop, the Karplus-Strong loop). It rings every harmonic of its note. Bright sets the loop's
    damping: dark strings lose their overtones quickly, more so on low notes, as on a real string.
  - **Bell** (bell/glass): three modes per note at 1 : 2.76 : 5.40, the free-bar ratios of gamelan
    and glockenspiel bars, each a two-pole resonator. Bright lifts the upper modes and lets them
    ring longer. It sounds metallic and slightly out of the key on its upper partials, by nature.
  - **Pipe** (hollow): a stopped pipe per note: the loop is half a period long with the feedback
    inverted, so only the odd harmonics ring. That is the hollow, clarinet-like colour of a shell
    held to the ear.
- **Decay** is the time for each note's fundamental to fall by 60 dB (0.1–20 s), whatever the note
  and the Bright setting. Longer decays ring somewhat louder as well as longer.
- **Duck** dips the ringing under each new hit (up to −20 dB, back over ~150 ms), so the drums stay
  punchy and the ring blooms in the gaps.
- **Level guard.** A held note right on a resonator's pitch with a long decay and Hits at 0 would
  build up enormously. The ringing may not get more than ~3 dB above the input's recent peak, and
  anything above 0.8 is bent softly toward full scale. On drums with sensible settings it stays out
  of the way (the tests check it does not act at the default settings).

### Chord changes

Conch never retunes a ringing resonator to a new note. When the chord changes, the new chord starts in
a fresh bank: its input fades in over 5 ms and it starts silent, so nothing clicks. The old bank
stops listening and **rings out** over at most 1.2 s (or the Decay, if shorter), so the last hit's
chord tails off under the new one. There are three banks, which covers fast changes. If all three are
busy, the quietest ring-out fades away in 15 ms first.

**Tune** (±50 cents, for tracks that aren't at A440) and **Shimmer** (each note drifting slowly by up
to 8 cents at its own rate, for a chorused, living ring) are small glides of the notes a bank already
has. They are click-free too.

### Freeze

**Freeze** stops the current bank listening and sets its decay to about five minutes: the chord that
is ringing holds, and the drums carry on dry on top. New chords (from Listen, Prog or the knobs) wait
until Freeze is off, and then the change happens. Freeze holds what is ringing at the moment you press
it: if nothing is ringing, it holds silence. On the String, Freeze also lets the overtones ring longer
than Bright normally allows.

## Which notes ring: Source

| Source | Rings | Tuned By shows |
|---|---|---|
| **Listen** | The key it hears and the chord it hears now: the triad (or 7th / 9th / 5ths, see Tones) on the scale degree it detects. | **Heard**, or **Key (unsure)** until it is sure |
| **Listen Key** | The key it hears, tonic chord only. It doesn't follow chord changes, so it is steadier on a busy mix. | as above |
| **Key** | The tonic chord of **Key** and **Scale**. | Key/Scale |
| **Chord** | **Chord Root** + **Quality**, set by hand: Major, Minor, Sus2, Sus4, Power, Dim, Aug, Maj7, Min7, Dom7, m7b5, Maj9, Min9, Dom9. Tones has no effect here. | Chord |
| **Prog** | A progression in Key and Scale (1-6-7-1, 1-4-5-1, 1-5-6-4, 1-7-6-7, 1-4-6-5, 1-6-4-5), one chord per **Chord Len** (½, 1, 2 or 4 bars), locked to the song position. With the transport stopped it stays on the chord it was on. | Prog |

**Tones** sets how much of a key's chord to ring: **Triad**, **7th**, **9th** (a ninth that would
be a minor ninth is left out: it clashes), or **5ths** (root and fifth only, the safest on drums).

### Listen, and drums on their own

Listen uses the key and chord analysis from Largemouth. It builds a four-octave note spectrum (C2–B5),
discards the drum noise between the notes, and compares what is left against the standard major and
minor key profiles (Krumhansl & Kessler) for all 24 keys. Chords are scored among the seven chords of
that key, with hysteresis. It takes one or two bars to be sure of a key. A chord change is heard about
a third of a second after it happens, so the first hit of a new chord can still ring the old one.

**A drum loop on its own has no harmony to hear.** The kick's pitch and a snare's ring can still make
a confident-looking key, so Conch also asks how tonal the input is (drums alone score about half of
what drums plus a pad do). Until both say "sure", Listen uses **Key** and **Scale** and the **Tuned By**
meter reads **Key (unsure)**. On a drums-only track, set Key and Scale and use **Key**, **Chord** or
**Prog**: those modes never listen and are fully usable on their own. Once Listen has found a key it
keeps it through a drum break, and it moves to a new key only after hearing it for a second.

The Scale knob flavours a heard key: Dorian or Phrygian for a minor key, Mixolydian for a major one.
A Scale of the other kind falls back to plain minor or major.

## Parameters

| Page | Control | What it does |
|---|---|---|
| RING | Material | **String**, **Bell**, **Pipe** (see above). |
| | Decay | Ring time to −60 dB, 0.1–20 s. |
| | Bright | Dark to bright: loop damping (String, Pipe), upper modes (Bell), and the excitation's tone. |
| | Mix | 0% dry, 50% both at full level, 100% resonators only. |
| | Spread | How many octaves of chord tones get a resonator, from Low Note up (1–4). |
| | Low Note | The bottom of that range: C1, C2, C3 or C4. |
| | Tones | Triad, 7th, 9th or 5ths (for Listen, Key and Prog). |
| | Width | Resonators panned across the stereo field. 0% is mono. |
| | Hits | 0%: all of the input rings them. 100%: only the transients do. |
| | In HPF | High-pass on what rings them, 20 Hz–2 kHz. Your dry signal is untouched. |
| | Drive | Saturation on what rings them. |
| | Duck | How far the ringing dips under each new hit. |
| | Freeze | Hold the current chord indefinitely (also on NOTES). |
| | Shimmer | Slow detune drift, up to 8 cents per note. |
| | Tune | Fine tune, ±50 cents. |
| | Output | Master volume. |
| NOTES | Source | Listen, Listen Key, Key, Chord, Prog. |
| | Key, Scale | Key root and scale (Minor, Major, Dorian, Phrygian, Mixolydian) for Key and Prog, and for Listen until it is sure. |
| | Chord Root, Quality | The chord for Source: Chord. |
| | Prog, Chord Len | The progression for Source: Prog, and how long each chord lasts. |
| | Tuned By | Read-only: where the chord comes from right now: Key/Scale, **Key (unsure)**, Heard, Chord, Prog or Frozen. |
| | Ringing | Read-only: the chord the resonators are tuned to (Am, Fmaj7, G5...). |
| | Found, Sure | Read-only, Listen sources: the key it hears ("--" until sure) and how sure it is. |

Spread × Tones decides how many resonators there are. The defaults (C2, 3 octaves, triad) give 8; 4
octaves of 9ths from C2 gives the maximum, 16.

## The panel

A custom skin on the user's conch artwork: the glowing shell in its driftwood window stays clear, with the controls
on the teal panels either side and on the plank along the bottom, gold knob arcs to match the shell's glow. Two
pages, three Q-Link banks each (bank direction Column: knob column 1 = the left panel top to bottom, column 2 = the
right panel, column 3 = the plank left to right):

| Page | Left panel | Right panel | Plank |
|---|---|---|---|
| RING | RESONATORS: Material, Decay, Bright, Shimmer | EXCITE: Hits, In HPF, Drive, Duck | Mix, Width, Freeze, Output |
| NOTES | SOURCE: Source, Key, Scale, Tones | CHORD: Chord Root, Quality, Prog, Chord Len | Spread, Low Note, Tune, Freeze |

At the top of the side panels, on both pages, are read-only readouts: **RINGING** (the chord the resonators are
tuned to) on the left, and **TUNED BY** with **FOUND** and **SURE** on the right. So "Key (unsure)" is always in
view while Listen hasn't found a key yet. Choice controls are rotary selectors with a detent dot per choice; Freeze
is a gold pill.

The skin is rendered by `art/make_skin.py` from `art/plate-src.webp` into `skins/`; **run it before
`tools/build.py`**, which copies `skins/` as it is. `ui.json` (stock cyan kit) is only the fallback if `skins/` is
removed. Previews: `build/skin-preview-1.png` and `-2.png`.

## Programs

| Program | Idea |
|---|---|
| Drum Strings | String, Key A minor (set your key), Hits 60%: the basic "drums become a sitar" sound. |
| Glass Kit | Bell, 9ths from C3 over 3 octaves, bright and wide: hits turn into glassy chords. |
| Hollow Shell | Pipe, two octaves from C2, a little Shimmer: the shell-to-your-ear sound. |
| Mix Halo | String, Listen, 7ths, Hits 20%, low Mix, wide: a soft harmonic halo on a mix or a submix. |
| Dub Pipes | Pipe, Prog 1-6-7-1 at 2 bars a chord, 5ths, Drive, heavy Duck. Press play. |
| Frozen Chord | String, Chord Am9 by hand, 12 s Decay, Shimmer: play a few bars, then press **Freeze**. |
| Gamelan Hits | Bell, root and fifth over 3 octaves from C3, bright, full width. |
| Listening Strings | The defaults: String, Source Listen. |

Programs that use Key or Prog are set to A minor; change Key and Scale to your track's.

## CPU

Measured on a desktop (Apple Silicon) by `tests/conchtest.cpp`, as a share of one core in real time:

| Case | Desktop |
|---|---|
| String, 8 notes, Source Key | 0.09% |
| String, 8 notes, Source Listen (the default) | 0.32% |
| Bell, 16 notes | 0.23% |
| String, 16 notes, a chord change every beat (two or three banks running) | 0.20% |

The key and chord analysis is the biggest part (~0.23%), and it only runs in the Listen sources. The
resonators cost about 1 multiply-add per note per sample for String and Pipe, and a little more for
Bell. **MPC estimate: about 1–2% of a core in Key, Chord or Prog, and 3–5% in Listen** on the older
32-bit units, less on the newer ones. Not yet measured on hardware. If CPU is tight, use Key, Chord or
Prog rather than Listen.

## Tests

`tests/conchtest.cpp` renders synthetic drums (kick, snare, hats at 92 BPM) and a saw pad, and checks
51 things:

- **Pitch**: in Key (A minor) and Chord (Dmaj7) modes, on every material, every resonator rings within
  5 cents of its note, measured on the free ringing (worst error 0.5 cents), including dark settings
  where the loop filter's delay has to be compensated. Chord tones carry ≥ 90% of the pitched energy.
  Tune +30 shifts every note by 30 cents.
- **Listen**: on drums + pad in A minor (with the knobs set to D major), it starts out "Key (unsure)",
  hears A minor within two bars, and follows Am–F–G–Am. On drums alone it stays on Key/Scale and says
  so. Prog changes chord exactly on the bar lines.
- **Clicks**: a second-difference click detector (self-tested on a 0.02 step) finds no click at chord
  changes while ringing, on all three materials, or with changes every 30 ms that force banks to be
  recycled.
- **Hits**: at 100%, a held chord on the resonators' own notes rings them 19 dB less than at 0%, drums
  ring them only ~1 dB less, and adding the held chord to the drums does not raise the ring.
- **Duck** dips the ringing just after a kick by about 12 dB at 100%.
- **Freeze** loses 1–2 dB in 10 s with a 2 s Decay (String 0.9, Pipe 1.2, Bell 2.2), where unfrozen
  it would lose 300 dB. It doesn't retune while frozen, and the waiting chord change happens when it is
  turned off.
- **Stability**: Decay 20 s, Bright, Drive and Shimmer at 100%, 16 notes, and a 0 dBFS note held right
  on a resonator for 15 s: no NaN, wet peak about −1 dBFS. No denormals in a decaying tail; ringing-out banks
  go idle; parameter 0 being nudged causes no retune.
- **Levels**: the three materials within 3 dB of each other on the drum loop; no program clips.
- **CPU** (the table above).

```
mkdir -p tests/build && clang++ -std=c++17 -O2 -I../../framework -o tests/build/conchtest tests/conchtest.cpp
tests/build/conchtest build/conch-demo.wav
```

`host_sim test` passes (20 pass, 0 fail; the one warning is the four read-only meters not being
saved, which is intended) at 44.1, 48 and 96 kHz, with 32- and 512-frame blocks, stopped and playing.

### Demo

`build/conch-demo.wav` (263 s), made by the test:

| Time | What |
|---|---|
| 0:00 | The drum loop, dry |
| 0:21 | Through Drum Strings (String) |
| 0:44 | Through Glass Kit (Bell) |
| 1:07 | Through Hollow Shell (Pipe) |
| 1:30 | Through Gamelan Hits |
| 1:53 | Through Dub Pipes (Prog 1-6-7-1, 2 bars a chord) |
| 2:38 | Drums + pad (Am F G Am), dry |
| 3:20 | The same through Mix Halo (Listen: unsure at first, then hears A minor and follows) |
| 4:04 | Frozen Chord: Freeze at bar 3, the drums stop, the chord holds |

## Not implemented / honest notes

- The pitched notes come from your audio: a track with no transients and Hits at 100% makes almost no
  ring. That is the point of Hits; turn it down for pads and mixes.
- Listen hears a chord about a third of a second late, and on a drums-only track it never hears one
  (see above). A tuned 808 alone may be enough for it to settle on a key, rightly or not.
- Bell's upper partials are inharmonic by design (1 : 2.76 : 5.40), so Bell is less "in key" than
  String or Pipe on its overtones. The fundamentals are exactly on the chord.
- Freeze holds what is ringing when you press it; it doesn't capture new input.
- Very high notes can't ring as long as a 20 s Decay asks: each trip round a loop loses a minimum
  amount (that keeps it stable), and a high note makes many trips a second. At C6 a String tops out
  around 12–20 s and a Pipe (two trips per period) around 6–11 s, less when dark.
- Not measured on MPC hardware yet.

## Credits

- Key and chord analysis copied from Groundswell's **Largemouth** (`src/Harmony.h`, MIT), with a faster
  Goertzel loop. The key profiles are the published probe-tone data of **Krumhansl & Kessler (1982)**.
- Comb-filter strings after the Karplus-Strong plucked-string algorithm (Karplus & Strong, 1983),
  written from the published description. Lagrange fractional-delay interpolation and the coupled-form
  resonator are textbook DSP.
- Everything else: original, MIT.
