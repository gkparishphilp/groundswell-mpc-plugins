# Tidepools

A granular call-and-response effect for MPC standalone hardware, by Groundswell.

Tidepools listens to what you play, waits until you stop, and answers with it.

That is the whole idea. A granular pedal gets a call-and-response feel somewhat by accident: it granulates a
rolling window of the last few seconds, and if you happen to stop playing you hear that window on its own.
Tidepools does it on purpose. An envelope follower watches the input and marks where a phrase begins. When you
stop, and the silence lasts longer than **Gap**, it decides you have finished, waits for the next beat or bar
if you asked it to, and hands the phrase back through one of four grain engines. Play again and it ducks out of
your way, so you can trade phrases with it.

```
in ─┬──────────────────────────────────────────────────────────────> dry ──┬──> out
    │                                                                      │
    └─> input gain ─> capture buffer (8 s, always rolling)                 │
                           │                                              │
    envelope follower ─────┼──> phrase detector (start, end, Gap)         │
                           │          │                                   │
                           │          └──> answer scheduler (Now/Beat/Bar)│
                           │                     │                        │
                           └─────────────────────┴──> 16 grain voices ──┐ │
                                       Cloud / Blocks / Glide / Bubbles │ │
                                                                        │ │
                             tone ─> swirl ─> reverb ─> limiter ─> trade┴─┘
```

## The seven engines

| Mode | What it does |
|---|---|
| **Cloud** | Grains scattered through the phrase at the pitch you played. The plain texture: a wash that thickens with Activity and smears with Shape. |
| **Blocks** | The phrase cut into tempo-locked slices and handed back in a shuffled pattern — some slices repeated, some reversed, some left as silence. The pattern is seeded per answer, so each reply is a different but *coherent* arrangement rather than mush. |
| **Glide** | The whole answer slides. One pitch offset is shared by every grain and depends only on how far through the pass the answer is, so consecutive grains carry the slide on from each other and the reply moves as one piece. Each pass slides from wherever the last one finished to a new target, so the pitch is continuous however far apart those targets are. **Glide** chooses them: *Down* and *Up* are a tide out to Motion's full depth and back, while *Mixed* picks a real interval each pass — a whole tone, a fourth, a fifth or an octave, up or down, and now and then back to the note you played. |
| **Bubbles** | Short grains, each through its own resonant band-pass, so every one becomes a pitched blip rather than a piece of audio. Their frequencies can be snapped to a scale, which turns a held note into a sparkling arpeggio in key. **Scatter** is a sample-and-hold over those filters: at 0 every bubble picks its own frequency, and turned up they arrive in groups of up to twelve on one pitch, which is what turns a shimmer into a pattern. Each sample draws a resonance with the frequency, so wide **Range** settings vary how sharply each bubble rings as well as where it sits. |
| **Roll** | Takes one short fragment and drills it, at a tempo-locked rate that tightens as the answer runs — a quarter-second apart at the start, down to about an eighth of that by the end. Blocks reorders whole slices; this one picks a fragment and repeats it. |
| **Ebb** | The whole answer in reverse. The playhead walks from the end of the phrase back to its start and every grain reads backwards, so notes swell into their attacks instead of decaying away from them. Motion lets the tide run out, slowing the playback by up to half across the pass. |
| **Chord** | Answers a note with a voicing, and then moves. Each grain is joined by two to four more at the thirds above it — and because Tidepools knows both what you played and what key you are in, those are the thirds the key actually has: major on the first, fourth and fifth degrees, minor on the second, third and sixth, diminished on the seventh. **Change** then walks the chord every bar or two through I – vi – IV – V, counted from the note you played, re-reading a different part of your phrase each time so a new chord is a new sound as well as a new harmony. One finger in, a progression out. Without a scale it stacks a plain major seventh instead, so it still does something on a drum loop. |

Two knobs mean something slightly different in each engine. **Scatter** is about position and order; **Motion**
is about pitch and movement. Both are listed per engine below.

## The screen

Tidepools has its own panel, built from artwork in `art/`: four pages — **LISTEN**, **GRAINS**, **TONE** and **SWIRL** —
each a row of boxes laid out for the Q-Link knobs, with a knob at the left of its box and the reading beside
it. Touch a control to bring its row onto the knobs; double-tap a knob (or press Enter) for a large edit view.

### Page 1: LISTEN

The call-and-response layer. This is the page that makes Tidepools what it is.

| Control | Range | Default | What it does |
|---|---|---|---|
| State | Idle / Hear / Wait / Talk | — | Not a control: the lamp. It shows whose turn it is — **Hear** while it is taking your phrase in, **Wait** while it holds for the next beat or bar, **Talk** while it answers. The plugin writes it twenty-five times a second, so tapping it does nothing lasting. Watch it for a minute and the whole plugin explains itself. |
| Listen | Off / Auto / Hold | Auto | **Auto**: watch for phrases and answer them. **Off**: granulate the last few seconds continuously, the way a pedal does — a texture, not a conversation. **Hold**: the Hold switch freezes the buffer and the grains work that frozen moment until you let go. |
| Sens | 0–100 % | 50 % | How readily it decides you have stopped. A note is treated as finished once it has dropped a set distance below its own loudest point — at 0 that is 34 dB (it waits for near silence), at 100 it is 10 dB (it answers into your decay). Sens also caps how long one phrase may run before it is answered regardless: 6 s at 0, about 3.6 s at 50, 1.2 s at 100. **Turn this up for pads, strings and anything with a long release; turn it down for staccato playing in a noisy room.** |
| Gap | 40–1500 ms, or 1/64–1/2 | 220 ms | How long a silence has to last before Tidepools decides you have finished. Short and it finishes your sentences; long and it waits until you really mean it. The phrase end is back-dated to where the silence began, so the gap is not baked into the answer. |
| Gap Mode | Free / Sync | Free | Whether Gap is milliseconds or a note division of the MPC tempo. **Sync** makes the listening window scale with the track, so a patch keeps its feel at any tempo — the same thing Time Mode does for the grains. The divisions stop at 1/2, since nothing longer means anything as "have they stopped playing", and the result is clamped to 1.5 s so a slow tempo cannot leave it waiting forever. |
| Answer | Now / Beat / Bar | Beat | When the reply starts. **Beat** and **Bar** wait for the next line on the MPC's grid, which is the difference between a reply and a spill. Falls back to Now when the transport is stopped. |
| Trade | 0–100 % | 50 % | How far the answer ducks out of the way while you are playing. At 100% it goes silent the moment you come back in and returns when you stop, so you genuinely swap phrases. |
| Hold | Off / On | Off | Freezes the capture buffer. Most useful with Listen on Hold, but it also works during an answer to lock what is being played with. |
| Reply | 0–100 % | 35 % | How far each successive pass drifts from the last — more scattered, more reversed, more wandering. At 0 the answer repeats itself; turned up it evolves. |
| Repeats | 0–100 % | 40 % | How many times the phrase comes back and how much it fades each pass. At 0 you get one answer. Around 50% it dies away over several seconds. Above about 90% it sustains until you play again. |
| Mix | 0–100 % | 50 % | Dry holds full level to half way while the answer fades in; from half way the dry fades out. |
| Input | −∞, −30…+12 dB | 0 dB | Level into the capture buffer. The dry path is untouched. Raise it if Tidepools is not hearing quiet playing. |
| Output | −30…+6 dB | 0 dB | Final level. |

### Page 2: GRAINS

| Control | Range | Default | What it does |
|---|---|---|---|
| Mode | Cloud / Blocks / Glide / Bubbles / Chord / Roll / Ebb | Cloud | Which engine answers (see above). |
| Activity | 0–100 % | 50 % | Grain density, from about 4 a second to about 90 — or to about 42 in Bubbles, which is deliberately thinner. A bubble is a distinct pitched blip rather than a grain of wash, so the ear counts them one by one and the same number reads as far busier. In Blocks it has no effect: slices arrive on the beat. |
| Shape | 0–100 % | 45 % | Grain length, 18 ms to about half a second, capped by Time. Short is percussive and clicky; long is smeared and glassy. |
| Scatter | 0–100 % | 35 % | **Cloud/Glide**: how far grains wander from where the playhead is. **Blocks** (*Shuffle*): how scrambled the slice order is, and how often a slice is dropped for silence. **Chord** (*Strum*): how far apart in time the notes of the voicing arrive — 0 is a block chord, full is a strum across about 120 ms. **Roll** (*Jump*): how readily it moves to a different fragment. **Ebb**: how far grains wander. **Bubbles** (*S&H*): how many bubbles share one filter setting before a new one is drawn — 1 at 0%, up to 12 at full. It counts bubbles rather than milliseconds, so changing Activity changes how fast the pattern runs without changing the pattern. |
| Motion | 0–100 % | 30 % | **Glide** (*Bend*): on Down and Up, how far the slide reaches, up to an octave. On Mixed it is how far down the list of intervals it may go — a whole tone below 25%, then fourths, then fifths, and octaves above 75% — so turning it up lets bigger intervals in rather than stretching one bend wider, which is what you want when the interval is the point. **Blocks** (*Reverse*): how often a slice plays backwards. **Bubbles** (*Range*): how wide the band of filter frequencies is, and how much the resonance varies with it — at 0 every bubble rings exactly as it always did, and at full some ping and others only blip. **Chord** (*Voicing*): two notes, three, then four, and past about two thirds the upper notes open an octave. **Roll** (*Speed*): how far the roll tightens — at 0 it holds its rate, at full it ends eight times faster than it started. **Ebb** (*Slow*): how much the playback slows across the pass, down to half speed. **Cloud**: unused. |

**The panel renames those two for you.** Both knobs mean something different in each engine, so their
headings are bound to Mode: Scatter reads SHUFFLE in Blocks, Motion reads REVERSE, BEND or RANGE as you switch,
and the heading goes faint in the engine where the knob does nothing. You should not need this table.
| Time Mode | Free / Sync | Sync | Whether Time is a note division of the MPC tempo or milliseconds. |
| Time | 1/64–2/1, or 30–4000 ms | 1/8 | Slice length in Blocks; elsewhere it caps how long a grain can be. |
| Spread | 0–100 % | 70 % | How widely grains are panned. At 0 the answer is mono. |
| Glide | Down / Up / Mixed | Mixed | **Glide only.** Where the slide goes. *Down* and *Up* are a tide: out to Motion's full depth over one pass, back to the note you played over the next. *Mixed* picks a fresh interval every pass — ±2, ±5, ±7 or ±12 semitones, or home — never the same one twice running, so every pass actually moves. Because each pass starts where the last finished, an octave up followed by an octave down is one continuous two-octave sweep. |
| Change | Off / 1 Bar / 2 Bars / 4 Bars | 1 Bar | **Chord only.** How often the chord moves on. It walks I – vi – IV – V in degrees of the scale, counted from the note you played, so it lands in the key wherever in the key you happen to be — and each move also picks a different part of your phrase to read, which is what keeps it from sitting still. Off holds one chord for the whole answer. It runs on the MPC's bars, not on the phrase, so it stays with the track; with the transport stopped it falls back to 120 bpm. **Much better with Scale set** — without one every chord is a major seventh and the walk is I – VI – IV – V in plain intervals. |

### Page 3: TONE

| Control | Range | Default | What it does |
|---|---|---|---|
| Tone | 200 Hz–16 kHz | 9 kHz | Low-pass on the grains, before the reverb. Also damps the reverb tail. |
| Space | 0–100 % | 30 % | A four-line feedback delay network on the grains only — dense and smooth rather than springy, which suits a cloud. The dry path never goes through it. |
| Decay | 0.3–12 s | 3 s | Reverb tail length. |
| Interval | −12…+12 st | 0 st | Transposes the answer. With Scale off it is plain semitones. With a scale it becomes a **diatonic** interval — see below. |
| Scale | Off / Major / Minor / Penta Maj / Penta Min / Dorian / Whole Tone | Off | The key. It snaps Bubbles' filter frequencies, and turns Interval into a diatonic interval. |
| Root | C … B | C | The note the scale is built on: the key of your music. |
| Key | Fixed / Follow | Fixed | Where the root comes from. **Fixed** uses the Root knob, which is almost always what you want. **Follow** takes it from the phrase instead, which only lands right if you tend to start phrases on the tonic. |
| Heard | – / C … B | – | Not a control: what the pitch detector made of the last phrase, or a dash if it found no pitch. |

### Page 4: SWIRL

Swirl sends the answer round your head: on headphones it circles from in front, to your right, behind you, to
your left and back. It is the same stage as Undertow's Swirl, placed after Tone and before the reverb, so the room
stays put while the answer moves. It touches only the answer; your dry signal never moves.

| Control | Range | Default | What it does |
|---|---|---|---|
| Swirl | 0–100 % | 0 % (Off) | How much of the circling to hear, blended with the normal stereo answer. 0 switches it off and costs nothing. |
| Mode | Free / Sync / Slice | Slice | **Slice**: the answer moves a fixed step round every Time (1/8 by default), so in Blocks each slice lands a step further round and the pattern spirals. It follows Time, and so the tempo when Time Mode is Sync. **Sync**: one turn per note length, locked to the song while the MPC is playing. **Free**: a speed in Hz. |
| Rate | see Mode | 1/8 turn | One knob for all three modes. Slice: 1/24 to 3/8 of a turn per Time (1/8 turn on eighths = once round a bar). Sync: 16 bars to 1/8 per turn. Free: 0.02–4 Hz. Slice is capped at 8 turns a second, so a very short Time cannot turn it into a buzz. |
| Direction | Clockwise / Counter | Clockwise | Seen from above: front, right, behind, left, or the other way round. |
| Depth | 0–100 % | 70 % | The headphone cues. The far ear hears the answer up to 0.66 ms later and duller (your head is in the way), and behind you it is darker and a little quieter. At 0 it is a plain auto-pan with no delays, which is the setting for speakers or anything that ends up in mono. |
| Width | 0–180° | 0° | How far apart the left and right sides of the answer sit on the circle. At 0 they travel together as one source: the clearest swirl, though it folds Spread's width into that one point. Turn it up to keep the cloud wide while it turns; at 180° the two sides sit opposite each other and the stereo image rotates. |
| Spread, Mix | | | The same controls as on GRAINS and LISTEN, gathered here because they decide what goes round. |

The **orbit** in the middle of the page is a dot that follows the swirl round the listener, with a short tail
behind it showing the direction. It hides while Swirl is at 0.

Turning Swirl up keeps the level within about 1.5 dB. Front and back over headphones is a suggestion, as it is
for any binaural trick: left and right carry the motion, and the "behind you" darkening helps it along. On
speakers you get an auto-pan with a gentle tone change; set Depth to 0 there.

### Pitch tracking, and what it buys

When a phrase ends, Tidepools works out what note it was in — once, not continuously. The analysis is a YIN-style
autocorrelation on a copy of the input decimated to a quarter of the sample rate, which is what makes it cheap:
about 19 µs, or under 1% of one audio block, once per phrase. Measured against plucked notes it is exact from
E2 to A5 (errors under 0.02 of a semitone), and it reports *no* pitch for noise and for silence rather than
inventing one. The **Heard** readout shows what it decided.

That pitch is what lets **Interval become diatonic**. With a scale chosen, the knob is read as a number of scale
steps rather than semitones, and the answer moves that many degrees from wherever your note sits in the key. Ask
for +4 in C major and you get a third — a major third from C, F and G, a minor third from D, E and A, because
that is what a third is on those degrees.

It is done by counting degrees rather than by snapping the shifted note, and that matters: snapping leaves the
tritone ambiguous, so a note detected a hundredth of a semitone sharp would answer a fourth up where a flat one
answered a third. Counting has no such edge.

With Scale off, Interval is an honest semitone shift and no pitch is needed.

## Programs

| Name | What it is |
|---|---|
| Call and Answer | The plain version: Cloud, answering on the beat. Start here. |
| Chopped Reply | Blocks on sixteenths, shuffled, with a high Reply so each answer rearranges itself |
| Bending Back | Glide on Mixed with Motion at 85%, so the reply sets off a fifth or an octave in either direction and keeps moving |
| Bubble Bath | Bubbles with a wide filter band, unscaled |
| In Key | Bubbles snapped to A penta minor, answering on the bar — arpeggios out of single notes |
| Octave Answer | Cloud an octave up, long Gap, answering on the bar |
| Texture (no reply) | Listen off: a continuous granular wash, no conversation |
| Freeze and Play | Listen on Hold with Hold engaged: a frozen moment to play over |
| One Finger Chords | Chord in C major, answering on the bar — play single notes and get triads back |
| Tightening Roll | A fragment drilled on eighths, accelerating through each answer |
| Ebb Tide | The phrase backwards and slowing, long Gap, answering on the bar |

**For sustained sources.** Anything with a long release — strings, pads, a piano with the pedal down —
needs Sens up, or the detector waits for a silence that never comes. These two arrive that way.

| Name | What it is |
|---|---|
| Long Strings | Cloud with Sens at 85% and a 400 ms Gap: answers into the decay instead of waiting it out, and Trade at 70% keeps it out of your way while you play |
| Pad Partner | Bubbles in Dorian with Sens at 90% and Trade at 100%, so it fills every gap you leave and vanishes the moment you come back |

**The two Glide directions.** Mixed wanders through intervals and never settles; these commit.

| Name | What it is |
|---|---|
| Octave Diver | Glide down a full octave over each pass and back, on eighths |
| Rising Tide | Glide up a full octave over each pass, answering on the bar, with a long reverb behind it |

**What the sample and hold is for.** Bubbles stops being a texture when the filters hold still.

| Name | What it is |
|---|---|
| Riff Machine | Bubbles in C penta minor, S&H at 55% and Activity high: groups of seven bubbles to a note, so it writes a part rather than a shimmer |
| Glass Harmonica | S&H at 75% with Range at 90%, long grains and a lot of Space — slow, pitched and ringing, each note a different sharpness |

**Chords that move.**

| Name | What it is |
|---|---|
| Backing Part | Chord in C major changing every 2 bars, Repeats at 95% and Trade at 90%: hold one note and it keeps a progression going underneath you |
| Minor Changes | Chord in A minor changing every 4 bars, wide voicings, long reverb — slower and more deliberate |

**The rest.**

| Name | What it is |
|---|---|
| Sixteenth Gate | Blocks with Gap on Sync at a sixteenth, so the listening window follows the tempo: tight, rhythmic and quick to answer |
| Trading Fours | Cloud with Trade at 100% and Repeats at 95%, Gap synced to an eighth, answering on the bar — it plays whenever you do not |

**Swirl.** Best on headphones. Every other program leaves Swirl off.

| Name | What it is |
|---|---|
| Circling Answer | Call and Answer going round you: an eighth of a turn every eighth note, so once round a bar |
| Spiral Blocks | Chopped Reply with every sixteenth slice a sixth of a turn further round, counter-clockwise, scattering the shuffled pattern round the circle |
| Halo | A continuous Cloud texture (Listen off) turning slowly overhead, about 8 s a turn, with the two sides 90° apart so it stays wide |
| Orbiting Chords | The chord walk changing every bar, one turn every two bars, locked to the song |

## Tips

- **Start with one note.** Play a single sustained note, stop, and listen. Everything Tidepools does is easier to
  hear on one note than on a phrase.
- **If you hear nothing, it is Sens.** On a patch with a long release — strings, pads, anything with a tail —
  the input never really goes quiet, so a detector set to wait for silence waits for ever. Turn Sens up to
  70–100% and it will answer into the decay instead. Watch the State lamp: if it sits on **Hear** and never
  reaches **Talk**, that is exactly what is happening.
- **Gap is the feel knob.** Short (80–150 ms) and it jumps in almost before you have finished, which is exciting
  and a bit pushy. Long (500 ms+) and it waits politely. It changes the character more than anything else here.
- **Answer on the Bar for solos, on the Beat for stabs.**
- **Trade at 100% with Repeats at 95%** gives you a partner: it plays continuously while you are quiet and
  vanishes the instant you come in.
- **In Key with one finger.** Bubbles, Scale to the key of your track, Activity high, Interval 0. Hold down a
  single low note and you get an arpeggio that stays in key.
- **Bubbles with S&H around 40–70%** stops being a texture and starts being a riff: groups of four to nine
  bubbles on one note, then a jump. With a Scale set those jumps land in key, so it writes a part rather than
  a shimmer. Leave S&H at 0 for the original wash.
- **Chord needs a key.** Set Scale and Root to your track before using it, or it falls back to stacking a major
  seventh on everything, which is right about a third of the time. Strum at 40–60% makes it sound played rather
  than triggered.
- **Chord with Change on 2 Bars and Repeats near 95%** turns one held note into a backing part that keeps
  going: four chords round, a different slice of your phrase under each. Answer on the Bar so it starts in the
  right place, and it will stay with the track from there.
- **Glide needs Motion up to be interesting.** At the default 30% Mixed only has whole tones and
  fourths to choose from. Past 75% the octaves come in, and an octave up followed by an octave down
  is a continuous two-octave sweep, because each pass sets off from wherever the last one landed.
- **Blocks wants a rhythmic source.** On a pad it sounds like a stutter; on a drum loop or a staccato line it
  rearranges the part.
- **Swirl on Blocks with Mode on Slice** is the one to try first: every slice lands somewhere new on the circle,
  so the rearranged pattern also moves round you. Steps of 1/3 or 3/8 turn throw consecutive slices across the
  circle; 1/24 or 1/16 make a slow spiral.
- **Use Hold as a performance control.** Listen on Hold, then tap Hold while a chord rings: the chord freezes
  and you can play over it.

## How a phrase is found

Two envelope followers watch the input. The slow one (2 ms attack, 60 ms release) drives Trade and the grain
gate, where a smooth envelope is what you want. A faster one (22 ms release) decides where a phrase *ends*,
because the slow follower takes about a third of a second to fall quiet after an abrupt stop — which put that
much dead air on the end of every phrase. It never showed until Ebb started reading from that end and found
nothing there.

The threshold that follower is compared against is **relative**, not a fixed floor, and that matters more than
anything else here. A fixed floor cannot tell a held note from a room that has gone quiet. Ask it to call a
phrase finished at −56 dBFS and a long-decay patch — a string, a pad, a piano with the pedal down — sits above
that line for seconds, or for ever: the phrase never ends, so nothing is ever answered and the plugin appears
to be broken. Instead a phrase is over once it has fallen a given distance below *its own* loudest point,
which is what "they have stopped playing" actually sounds like. Sens sets that distance.

Two things back it up. A phrase that never drops at all — a held drone — is cut off and answered anyway once
it passes the Sens time limit, so there is no input that produces silence. And after a phrase ends on its own
decay the input is still well above the threshold, so the level it ended at has to be beaten before anything
counts as playing again; otherwise one note's tail would open phrase after phrase and each new answer would
cut off the last. That re-arming relaxes over about 400 ms.

**If you want none of this**, set Listen to **Off**: there is then no detector, no waiting and no decay, and
Tidepools granulates the last few seconds continuously the way an ordinary pedal does.

## CPU

About 0.06% of one core per instance on a desktop machine at default settings, measured by the host simulator —
well inside the MPC's budget, with room for several instances. Sixteen grain voices, each a linear-interpolated
buffer read, a windowing multiply and (in Bubbles only) one state-variable filter whose coefficients are set
once when the grain starts, never per sample. The reverb is four delay lines with Hadamard mixing and one
damping pole each. Swirl, while it is above 0, adds about 0.06% of one core (0.11% → 0.17% with Listen off
and the grains running continuously): four short interpolated delays and four one-pole filters a sample, with
its sines, cosines and exponentials worked out every 16 samples. At 0 it is skipped. Memory is about 3 MB per instance, nearly all of it the 8-second stereo capture buffer.

Nothing allocates, locks or does I/O in the audio callback. Grain voices are a fixed pool; when they are all
busy a new grain steals the one furthest through its life.

## Not implemented yet

- A looper. The MPC has one, and a proper looper needs a footswitch idiom that does not translate to a
  touchscreen. Hold covers most of the same ground.
- Chords. The detector finds one pitch, so a chord is read as whichever note dominates.
- A level meter on screen. The plugin already reports one; only the state lamp is drawn so far.
- Feeding the answer back into the capture buffer, so repeats degrade as well as fade.

## Build

```
python tools/build.py plugins/Tidepools
python tools/package.py plugins/Tidepools
```

For the computer (a VST2 that draws this same panel in FL Studio, Reaper or MPC Software):

```
python tools/build.py plugins/Tidepools --target desktop
```

## Skin

`art/make_skin.py` builds `skins/` from the panel artwork, the knob and pill sources in the same folder, and
the parameter list. Run it after changing any control:

```
python3 plugins/Tidepools/art/make_skin.py      # writes skins/ and build/skin-preview-*.png
```

It binds to parameter *indices*, so it must be regenerated whenever the parameter list changes. The boxes,
headings, value arcs, pill captions and the RESPONSE curve are drawn by the script; only the background plate,
the knobs and the pill shape are source art.

The drawing machinery started as Undertow's generator and is deliberately a copy rather than a shared import:
Undertow's skin is shipping and verified, and one plugin's artwork should not break because a sibling's did.

## Licence

MIT. No third-party DSP: the grain engine, the phrase detector and the reverb are written for this plugin.

## Installing on the MPC

Copy `build/Groundswell - VST - Tidepools/` into a folder named `VST2 Plugins` on an MPC drive. The MPC needs
MPC OS 3.10.1 or later, and External plugins must be enabled: Preferences → Enhancements (Beta) → Enable
Enhancements, then External Plugin Support. The MPC only scans for plugins when its app starts, so restart it
afterwards; Tidepools then appears under External Plugins.
