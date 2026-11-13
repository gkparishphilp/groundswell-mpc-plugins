# Undertow

A dub delay for Akai MPC standalone hardware, by Groundswell. The feedback loop is an effects chain: every
repeat goes back through a resonant filter, a selectable effect and a tube-style drive stage, so the repeats
change as they decay. Push the feedback past 100% and the resonance towards the top and it self-oscillates; a
limiter on the repeats keeps that loud but under control.

Undertow is an original design, inspired by the idea of desktop dub delays with filters in the feedback path.
It contains no code from any commercial product.

```
in ─┬──────────────────────────────────────────────────────────────┐ dry
    └► delay ─► FILTER ─► FX ─► DRIVE ─┬─► WET LIMITER ─► mix ─────┴─► Output ─► out
         ▲                             │
         └──────── x Feedback ◄────────┘     (Ping-Pong: left and right swap on every repeat)
```

## Page 1: DELAY

| Control | Range | Default | What it does |
|---|---|---|---|
| Time | 10–5000 ms, or 1/64–2/1 | 1/8D | Delay time. In Sync mode the knob snaps to note divisions of the MPC tempo (1/64 up to 2/1, with dotted and triplet values) and shows the division; in Free mode it is in milliseconds. Changing it glides like tape, so turning it warps the repeats' pitch. |
| Feedback | 0–120 % | 55 % | How much of each repeat comes back. Below 100% repeats die away; above 100% they grow until the drive stage saturates them. High Resonance lowers that point (see below). |
| Mix | 0–100 % | 35 % | Dry stays at full level up to 50% while the repeats fade in; from 50% the dry fades out. |
| Time Mode | Free / Sync | Sync | Sync: Time follows the MPC tempo in note divisions. Free: Time in milliseconds. |
| Ping-Pong | Off / On | On | Repeats bounce left and right. The input is summed to mono and starts on the left. |
| Wobble | 0–100 % | 15 % | Tape wow (slow) and flutter (fast) on the delay time. |
| Limit | 0–100 % | 30 % | How hard the repeats are pushed into the limiter (0 to +18 dB). Higher values give denser, more slammed repeats. |
| Output | −30…0 dB | 0 dB | Final level. |

## Page 2: LOOP

| Control | Range | Default | What it does |
|---|---|---|---|
| Filter | Off / LP / BP / HP | LP | Filter inside the loop. Each repeat is filtered again, so they get darker (LP), thinner (HP) or more nasal (BP) as they decay. |
| Cutoff | 40 Hz–16 kHz | 1.8 kHz | Smoothed, so fast sweeps sound like sweeps. |
| Resonance | 0–100 % | 35 % | Makes the repeats ring at the cutoff. At full Resonance the loop self-oscillates from about 67–75% Feedback. |
| Drive | 0–100 % | 25 % | Tube-style saturation (slightly asymmetric). Always in the loop; this is what keeps runaway feedback musical. |
| FX Type | see below | Off | The effect inside the loop. |
| FX Amount | 0–100 % | 50 % | How much of the effect. |
| FX Rate | 0–100 % | 30 % | The effect's second control. |

### FX types

| FX Type | FX Amount | FX Rate |
|---|---|---|
| Chorus | depth | speed (0.05–3 Hz) |
| Flanger | depth and resonance | speed (0.03–2 Hz) |
| Phaser | depth and feedback | speed (0.03–4 Hz) |
| Bitcrush | bit reduction (16 → 2 bits) | sample-rate reduction (1×–8×) |
| Downsample | sample-rate reduction (1×–48×) | smoothing (0 = raw aliasing, 100 = muffled) |
| Ring Mod | mix | carrier frequency (20 Hz–2 kHz) |
| Fuzz | clipping (1×–40×, level-compensated) | tone (600 Hz–15 kHz) |

Because the effect is inside the loop, it compounds: each repeat is chorused, crushed or ring-modulated again.
Every effect is gain-compensated, so it changes the colour of the repeats but not how long they last: Feedback
still decides that.

## Programs

| Program | Character |
|---|---|
| Dub Echo | Dotted-eighth ping-pong, darkening repeats |
| Siren Sweep | Ringing band-pass repeats that decay slowly: sweep Cutoff |
| Chorus Wash | Quarter-note repeats that widen and smear |
| Crushed Repeats | High-passed, bit-crushed dotted-sixteenths |
| Flange Spiral | Repeats spiral into a jet flange |
| Lo-Fi Tape | Heavy wobble and downsampling |
| Self-Oscillation | 110% feedback into a ringing band-pass: play Cutoff like an instrument |

## Tips

- **Dub throws:** leave Mix at 0 and push it up for a beat to throw a single hit into the delay, then pull it back down.
- **Self-oscillation:** Feedback above 100%, or Resonance near full with Feedback above ~75%, then ride Cutoff. Drive and Limit shape how it sounds when it takes off; Feedback back below 100% lets it die away.
- **Pitch warps:** in Free mode, sweep Time while repeats are playing.
- Changing FX Type restarts the effect from silence, so the switch is clean but the old effect's tail is cut.

## How the limiting works

Every stage inside the loop is held to about unity gain (the filter's resonant peak may reach +3.5 dB at full
Resonance), so Feedback behaves predictably whatever filter and effect you pick.

The drive stage is always in the loop and its output is bounded, so even at 120% feedback the repeats cannot grow
without limit. The repeats then pass a stereo-linked limiter (0.3 ms attack, 80 ms release) with a −3 dBFS ceiling
and a soft knee, and the final output has a soft knee from −1 dBFS that never passes −0.1 dBFS. The dry signal is
not limited, so quiet repeats sound natural and only loud ones are clamped. Nothing adds latency.

## Not implemented yet (phase 2 ideas)

- Octave up / down inside the loop
- Reverse
- Freeze / hold

## CPU

Per sample: one delay read per channel, a state-variable filter, the selected effect, two saturators and the
limiter. Filter and phaser coefficients are updated every 16 samples; there are no transcendental functions per
sample. Delay memory is about 1.8 MB per instance (5 s of stereo at 44.1 kHz).

## Licence

MIT. See the repository's LICENSE. Uses the MPC Plugin SDK framework (MIT).

## Build

```
python tools/build.py plugins/Undertow
python tools/package.py plugins/Undertow
```
