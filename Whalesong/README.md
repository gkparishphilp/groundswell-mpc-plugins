# Whalesong

**An accompanist.** Put it on a track, play something, and it works out what key you are in and
plays along on its own synth.

Nothing of your audio comes out of it except the dry signal passing through. Every note you hear
from Whalesong is synthesised inside the plugin. It is an insert effect only because that is the
only way an MPC plugin can hear the track — instruments get MIDI, effects get audio, and this one
needs to listen.

## What it does

1. **Listens.** A pitch detector runs about twenty times a second on whatever is coming in.
2. **Learns the key.** Every confident pitch drops a vote into one of twelve bins. After a bar or
   two there is enough in the histogram to say which key the music is in, and how sure it is.
3. **Plays along.** A small polyphonic synth plays a line in that key — a bassline, a lead, a
   counter-melody, a pad or an arpeggio.

It needs **melody** to work with. A single line is ideal: a lead, a bass, a vocal, a mono synth.
Dense chords and full mixes confuse the pitch detector, and drums tell it nothing at all.

## The panel

The artwork leaves a lit panel in the middle and a gutter either side. The gutters hold the
controls, four to a side, eight to a tab. The middle panel is **the readout**, and it is the same
on all three tabs, because it is the point of the plugin:

- **DETECTED KEY** — the root, large, with the mode under it. A dash until it has decided.
- **HEARING** — the pitch class the detector is hearing right now.
- **CONFIDENCE** — how sure it is, as a bar.

Watch those while you play. If Confidence never climbs, the input is not giving the detector
enough to work with: try a more melodic part, or raise Sens.

### LISTEN

| Control | What it does |
|---|---|
| **Listen** | `Off` uses the Key and Scale buttons and ignores the input. `Follow` keeps re-reading the key as you play. `Lock` follows until it is sure, then stops changing its mind. |
| **Sens** | How quiet the input can be and still count as playing. Turn it up for quiet or soft-attack material. |
| **Wait** | How long it listens before it is allowed to play. Two bars is the default, and there is a reason: after one bar it is often confidently wrong. |
| **Mix** | How loud the accompaniment sits against your dry signal. |
| **Key** / **Scale** | The key to use when Listen is Off. Ignored while it is following. |
| **Voices** | How many notes may sound at once, 1–6. Pad wants 4 or more; Bass wants 1 or 2. |
| **Output** | Master volume. |

### PLAY

| Control | What it does |
|---|---|
| **Role** | `Bass` and `Pad` sustain: when nothing of theirs is sounding they always play, so Activity sets how often the chord *changes*, not whether there is one. `Bass` roots and fifths, low and slow. `Lead` a melodic line that steps more often than it leaps. `Counter` moves opposite to you — when you rise, it falls. `Pad` sustained triads. `Arp` cycles chord tones. |
| **Activity** | How often it takes an opportunity to play. At 0 it is silent; at 100 it plays every one. |
| **Motion** | How far the line is allowed to range from where it is. |
| **Octave** | Transposes the whole line, ±2 octaves. |
| **Sync** | `Free` runs on its own clock, `Beat` and `Bar` lock to the host transport. |
| **Rate** | The note division it plays on when synced. |
| **Human** | Timing and velocity jitter. A little stops it sounding like a sequencer. |
| **Glide** | Portamento between notes. |

### PHRASE

Normally Whalesong improvises continuously and never plays the same thing twice. A **Window**
changes that: it plays a phrase of that length, then plays it again.

| Control | What it does |
|---|---|
| **Window** | `Off` improvises forever. `1`–`8 Bars` sets how long a phrase is. The window is both what it remembers and what it loops, and it lines up with the song's bars rather than with whenever the plugin loaded. |
| **Vary** | How much each repeat differs. At 0 the phrase is exact. Variation moves a note to a neighbouring degree, leaves a rest, jumps an octave, or changes a note's weight — the shape stays, the detail moves. |
| **Renew** | How many repeats before it writes a fresh phrase, so a take does not ossify. |

Notes are remembered as **scale degrees, not pitches**, so if the detector changes its mind about
the key halfway through a take, the phrase transposes to the new key rather than fighting it.

### FX

| Control | What it does |
|---|---|
| **Delay** / **Time** / **Feedback** | A tempo-synced delay whose repeats cross over, so they walk left and right. |
| **Chorus** | Depth only; the rate is fixed slow, the way a chorus wants to be. The two sides run a quarter cycle apart, which is what makes it wide rather than just detuned. |
| **Vibrato** / **Vib Rate** | Pitch wobble, in cents, on every voice. |
| **Tremolo** / **Trem Rate** | Level wobble. |

Every effect defaults to zero depth, so a project saved before they existed sounds exactly as
it did.

### TONE

Shape (waveform), Tone and Reso (filter), Drive, Attack and Release, Spread (stereo width and
oscillator detune), Space (reverb).

## Programs

| Program | What it is |
|---|---|
| **First Light** | A gentle lead that follows you. The one to start with. |
| **Deep Voice** | A bassline under whatever you play. |
| **Answering** | A counter-melody that moves against your line. |
| **Slow Drift** | Pads on the bar, long attack, plenty of reverb. |
| **Sonar** | Sixteenth-note arpeggios, bright and short. |
| **Pod Chorus** | Wide six-voice pads that lock the key and stay there. |
| **Tide Echo** | A lead through a crossing delay, with a little chorus. |
| **Siren** | Deep slow vibrato and tremolo over a free-running lead. The most whale-like of them. |
| **Fixed C Min** | Listening off, C minor by hand — for when you want to choose the key yourself. |

## How well does it actually find the key?

Measured, not guessed. The test harness feeds it generated melodies in known keys:

- **10 of 10 keys identified correctly** across six major and four minor melodies, four bars each.
- **Settles within two bars.** After one bar it is often wrong — hence the Wait default.
- **Refuses to guess.** A single note repeated reports 0% confidence, because one pitch class
  could be the tonic of one key, the third of another and the fifth of a third.
- **Follows a modulation.** C major to A major is picked up within a few bars.
- **All nine programs** identify the key of a test melody, stay finite and stay under the ceiling.
  The audit measures what the *synth* adds, not total output: measuring the total cannot tell a
  working program from a silent one, since the dry input passes through either way, and that is
  exactly how a silent Pad once survived an audit.
- **The phrase window** repeats exactly at Vary 0, keeps roughly two thirds of its notes at Vary
  50%, renews on schedule, and is bit-identical to the old behaviour when Off.

**Lock** needs more than confidence: it waits for an unbroken run of about three seconds in which
every estimate agreed on the same root *and* was over the confidence threshold. Confidence on
ambiguous material spikes and collapses from one estimate to the next, and a single reading over
the line would freeze the wrong key for good.

Confidence is two things multiplied: how far the winning key beats the runner-up, and how many
distinct pitch classes there are to go on. Both have to be good before it claims to be sure.

### Why major and minor get preferential treatment

Dorian and Mixolydian contain exactly the notes of some major scale. No amount of counting can
separate them — the histograms are identical, and only which note the music leans on differs.
The tonic weighting is a weak signal when music leans on its fifth, as music does. Without a
thumb on the scale a C major tune that stresses G reads as G Mixolydian: true about the notes,
and the wrong answer. So major and minor get a small prior and the modes have to earn it.

The pentatonics need no prior, being subsets: they only win when the melody really does avoid
the notes they leave out.

## What is not implemented

- **No chord recognition.** The pitch detector is monophonic (YIN). It hears one note at a time,
  so it infers the key from a melody, not from harmony. Pads and strummed chords will give it a
  wandering pitch stream; the histogram is forgiving, but it is not reading your chords.
- **Nothing below about 79 Hz** registers, so it cannot take its key from a bass part alone.
- **It does not know where the chord changes are.** It plays in a key, not over a progression.
- **It cannot send MIDI.** No MPC plugin can. Everything it plays, it plays itself.

## CPU

About 4–6% of one MPC core with four voices, most of it the oscillators; the pitch detector adds
roughly 1%, and the effects about another 1% with all four running. The detector runs on a hop counter rather than every sample, so its cost lands in one
block out of every forty or so. Six voices of Pad is the heaviest setting.

## The skin

`art/make_skin.py` builds `skins/` from `art/background-src.png` and the knob and pill artwork.
Regenerate it whenever the parameter list changes, because the skin binds to parameter indices:

```
python3 plugins/Whalesong/art/make_skin.py
```

## Licence

MIT. The pitch detector is YIN (de Cheveigné & Kawahara, 2002), implemented from the paper;
the implementation here started as the one in Groundswell's Tidepools.
