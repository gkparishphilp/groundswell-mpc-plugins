# Largemouth

**A bass player that listens to your drums.** An MPC insert effect by Groundswell.

Put it on a drum track and it plays a bassline on its own synth, locked to your kick drum, in the
key and on the chord it hears. Your audio passes through untouched; every bass note is made inside
the plugin. It is an effect, not an instrument, because only effects can hear a track on the MPC.

It works best on a **submix or the master** with your drums *and* your chords/samples in it: then it
can hear the harmony and follow the changes. On a drums-only track it can't hear any chords, so set
**Key** and **Scale** yourself (Listen: Off) and either stay on the root (Changes: Tonic) or pick a
progression.

## How it follows

- **Kicks.** It listens below 100 Hz for kick onsets. In **Grid** mode, while the MPC is playing, it
  learns where the kicks fall over two bars of 16th notes, including how early or late each one is,
  so swing and feel carry over. It then plays those steps **on time from the song position**,
  instead of late from the detector. A new pattern takes one or two passes to learn; until then,
  kicks it didn't expect are played as it hears them. In **Hits** mode (and whenever the transport
  is stopped) it plays a note on every kick it hears, about 9 ms after the kick starts.
- **Key.** It builds a 4-octave note spectrum (C2–B5), discards the drum noise between the notes,
  and compares what is left against the standard major and minor key profiles (Krumhansl & Kessler)
  for all 24 keys. **Listen: Follow** keeps updating; **Lock** freezes the key after two seconds of
  being sure; **Off** uses the Key and Scale knobs. Until it is sure, it uses Key and Scale.
- **Chords.** With **Changes: Follow** it scores the seven chords of the key against what it heard
  over the last ~third of a second, with an extra vote for the loudest bass note, and plays that
  chord's root. Because it has to hear a chord first, the first note after a chord change can still
  be the old root.

## Parameters

| Page | Control | What it does |
|---|---|---|
| GROOVE | Follow | **Grid**: learn the kick pattern and play it on time. **Hits**: a note on every kick heard. |
| | Kick Sens | How quiet a kick can be and still count. Too high and toms or bass notes count too. |
| | Bass | Level of the bass against your track. |
| | Output | Master volume. |
| | Busy | How many notes it adds between the kicks (pickups into a kick first, then offbeats). |
| | Rate | The grid those extra notes sit on: 1/4, 1/8 or 1/16. |
| | Swing | MPC-style swing for the extra notes. 50% is straight; kick notes keep the swing they were learned with. |
| | Vary | How often the extra notes change, checked every two bars. 0% repeats the same line. |
| NOTES | Changes | **Follow** the heard chords; **Tonic** always the key's root; or a progression of scale degrees (1-6-7-1 is i–VI–VII–i in a minor key). |
| | Chord Len | How long each chord of a progression lasts. |
| | Walk | Fifths, passing notes and approach notes into the next chord, instead of roots. |
| | Oct Jump | Chance of jumping up an octave. |
| | Listen | Follow / Lock / Off (see above). |
| | Key, Scale | The key it uses when Listen is Off, or until it is sure. Scale also flavours a heard key: Dorian or Phrygian for a minor key, Mixolydian for a major one. |
| | Octave | Moves the whole line. Roots sit between E1 and D#2 at 0. |
| SOUND | Voice | **Sub** (sine), **808** (sine with a long pitch drop, decays on its own), **Pluck** (saw + sine, filter envelope), **Acid** (resonant saw, accents on loud kicks, slides when legato), **Reese** (two detuned saws + sine). |
| | Tone, Reso | Low-pass filter cutoff and resonance. |
| | Drive | Saturation before the filter: it helps a sub bass speak on small speakers. |
| | Decay | Envelope decay (the whole note on 808). |
| | Punch | Pitch drop on Sub and 808; filter-envelope depth on Pluck, Acid and Reese. |
| | Glide | Slide time between overlapping notes. |
| | Length | How much of the gap to the next note each note fills. 100% = legato (and Glide slides). |
| MIX | Duck, Duck Time | How far the bass dips under each kick, and how fast it comes back. |
| | Bass Only | Mute your track to hear just what it is playing. |
| | Found, Sure | The key it hears and how sure it is (read-only). |
| | Chord, Kick | The chord the bass is on now, and a flash for every kick it hears (read-only). |

## Programs

Init 808, Deep 808, Sub Anchor, Rolling Pluck, Acid Line, Reese Garage, Dub Steps, Boom Bap.

## Tips

- Turn on **Bass Only** for a moment and play your beat: you hear exactly what it's playing.
- If it misses soft kicks, raise **Kick Sens**. If it plays on snares or toms, lower it. Watch the
  **Kick** meter on the MIX page.
- For a part that holds the root under everything: Changes **Tonic**, Busy **0**, Length **100%**.
- The learned groove is not saved with the project; it relearns in a bar or two.

## What is not implemented

- It can't hear chords on a drums-only track (nothing to hear): use Key/Scale and Tonic or a progression.
- Chords outside the key (borrowed chords, secondary dominants) are heard as the nearest chord in the key.
- Major and minor are the only keys it detects; the modes come from the Scale knob.
- Odd meters work (the grid is two bars of whatever the time signature gives), but have been tested less than 4/4.

## CPU

Roughly 2–4% of one MPC core. On a desktop the whole plugin takes about 0.35% of real time with the
Reese voice. The chord analysis is spread over several audio blocks so no single block pays for it.

## Testing

`tests/listentest.cpp` renders a boom-bap loop with an Am–F–G–Am pad and checks that the plugin
finds A minor, plays one note per kick, on time, on the right chord root, learns a late kick, and
works in Hits mode, with the transport stopped, and with a progression on drums alone:

```
clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
tests/build/listentest build/listen.wav
```

## Licence

MIT. Original design and code. The key profiles are published research data (C. L. Krumhansl and
E. J. Kessler, 1982).
