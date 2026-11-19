# Fathom

A filter that breathes with the music. Fathom started as an auto-wah for guitar that doesn't just quack on each
pick stroke: a slow envelope follower opens the filter as you dig in and lets it sink back as you ease off, the
way a player might rock a wah pedal slowly with the song. It grew into two filters moved by three sources, so
it also covers tempo-synced wobbles, vowel sounds, random steps and slow sweeps.

Insert effect, stereo in / stereo out. Groundswell, MIT licence.

## How it works

```
in ─┬──────────────────────────────────────────────────────────────────┐ dry
    └─► Drive ─► Filter A ─► Filter B (Series) ──────► soft knee ─► Mix ┴─► Output ─► out
                 └──────── Filter B (Parallel: A and B side by side) ─┘
```

Three sources move the two filters. All of them listen to the dry input and work the same on both channels:

- **Follower**: how loud you're playing (RMS, in dB) with its own attack and release. Slow settings make the
  filter breathe; fast settings give the classic auto-wah.
- **LFO A and LFO B**: seven shapes each. They run free in Hz or lock to the MPC's tempo and bar line, so a
  synced LFO has the filter closed on the downbeat. LFO A moves filter A, and LFO B moves filter B. With LFO B
  set to **Link A** (the default), B simply follows LFO A. Set it to **Own** to give B its own shape and rhythm.
  **Fol>LFO** lets the follower set both LFOs' depth: with a positive value, the harder you play, the more the
  filters move.
- **Envelope**: fires on each new note (a transient detector), rises and decays. With a short **Env Swell**
  it snaps open: the quack. With a long one (a few hundred ms) the filter swells open after each note and sinks
  back, like a foot rocking the pedal for you.

Each filter's cutoff, in octaves, is `Freq + Follower × Fol>X + LFO X × LFO>X + Envelope × Env>X`.

In **Parallel**, **A Pan** and **B Pan** place each filter in the stereo field. Even a mono guitar becomes two
moving voices, one on each side. In Series there is only one output, so the pans do nothing and are dimmed
on screen.

**Breathe** doesn't move the filters itself. It adds *inertia*: every filter movement glides (5 ms at 0%,
about 600 ms at 100%), so movements swell instead of snapping. It also adds a slow *drift* (a smooth random
wander, up to ±0.8 octave, roughly every 11 seconds), so the filter keeps moving even on a held chord.

### Filter types

| Type | Sound |
|---|---|
| Wah | A resonant band-pass with some low end kept underneath, louder at the peak: the shape of a pedal wah. Never fully flat, even at 0% resonance |
| Lowpass | 12 dB/oct, resonant |
| Bandpass | Unity gain at the peak; two in parallel make vowel sounds |
| Highpass | 12 dB/oct, resonant |
| Notch | A moving dip, like a one-stage phaser |

## Parameters

| # | Name | Range | Default | What it does |
|---|---|---|---|---|
| 0 | Output | −24..+12 dB | 0 | Level after everything |
| 1 | Mix | 0..100 % | 100 | Dry ↔ filtered |
| 2 | Drive | 0..100 % | 15 | Soft saturation before the filters: grit, and it evens out dynamics |
| 3 | Breathe | 0..100 % | 40 | Inertia and slow drift on the filter movement |
| 4 | A Type | Wah / Lowpass / Bandpass / Highpass / Notch | Wah | |
| 5 | A Freq | 60 Hz..8 kHz | 380 Hz | Filter A's resting cutoff; the sources push it up and down from here |
| 6 | A Reso | 0..100 % | 60 | |
| 7 | Routing | Series / Parallel | Series | Series: A then B. Parallel: A and B side by side |
| 8 | B Type | Off / Wah / Lowpass / Bandpass / Highpass / Notch | Off | |
| 9 | B Freq | 60 Hz..12 kHz | 1.5 kHz | |
| 10 | B Reso | 0..100 % | 40 | |
| 11 | Fol Attack | 1..1000 ms | 150 | How quickly the follower rises as you play louder |
| 12 | Fol Release | 10..5000 ms | 1500 | How slowly it sinks back when you play softer |
| 13 | Fol Gain | −12..+36 dB | +12 | Input gain into the follower. Raise it for quiet sources, lower it for hot drum buses |
| 14 | Fol>LFO | −100..+100 % | +60 | +: louder playing means a deeper LFO. −: quieter playing means a deeper LFO. 0: fixed depth |
| 15 | LFO A Shape | Sine / Triangle / Ramp Up / Ramp Down / Square / Steps / Drift | Sine | Steps = sample & hold, Drift = smooth random |
| 16 | LFO A Sync | Free / Sync | Sync | |
| 17 | LFO A Rate | 0.02..20 Hz, or 8 bars..1/32 | 2 bars | One control; the display follows LFO Sync |
| 18 | Spread | 0..180° | 20 | Puts the right channel's LFO and drift out of phase with the left, for stereo movement |
| 19 | Env Sens | 0..100 % | 50 | How easily a new note fires the envelope |
| 20 | Env Decay | 20..3000 ms | 300 | How long the envelope takes to fall back |
| 21–23 | Fol>A, LFO>A, Env>A | −4..+4 oct | +2.5, +0.6, 0 | How far each source moves filter A. Negative values move it down |
| 24–26 | Fol>B, LFO>B, Env>B | −4..+4 oct | 0, 0, 0 | The same for filter B |
| 27–28 | A Cutoff, B Cutoff | 20 Hz..20 kHz | — | Meters: where each filter is right now. The plugin writes them (at most ~33 times a second); they aren't saved or automatable |
| 29 | A Pan | L 100..C..R 100 | C | Parallel only. A balance that folds the far side in, so a mono source keeps its level |
| 30 | B Pan | L 100..C..R 100 | C | Parallel only |
| 31 | LFO B | Link A / Own | Link A | Link A: filter B follows LFO A. Own: LFO B's own settings below |
| 32–34 | LFO B Shape, Sync, Rate | as LFO A | Sine, Sync, 1 bar | |
| 35 | Env Swell | 1 ms..2 s | 1.5 ms | How long the envelope takes to rise after a note. Short: a snap (the quack). Long: a swell that eases in and out |

## The panel

Five pages:

- **MAIN**: the breathing wah on one screen.
- **FILTERS**: both filters, Drive and Routing.
- **MOTION**: the follower and Breathe on the left; the envelope (Env Sens, Swell, Env Decay) and Spread on the right.
- **LFO**: LFO A on the left, LFO B on the right. B's own settings are dimmed while it's linked to A.
- **DEPTH**: how far each source moves each filter, plus each filter's pan. The pans are dimmed in Series. The display in the middle draws both filters' response curves live
from the two meters: cyan for A, violet for B, with B hidden while it's Off. You see the filter breathe. Controls
for filter B have violet arcs.

The skin is generated by `art/make_skin.py` from `art/background-src.webp`, with the knob and pill art shared
with Whalesong. Render it **before** building, because the build copies `skins/` as it finds it:

```
python3 plugins/Fathom/art/make_skin.py
python3 tools/build.py plugins/Fathom
```

## On the computer

```
python3 tools/build.py plugins/Fathom --target desktop
cp -R plugins/Fathom/build/desktop/Fathom.vst ~/Library/Audio/Plug-Ins/VST/
```

The result is a VST2 for FL Studio, Reaper and similar hosts, with the same DSP and the same panel as on the MPC.
FL Studio doesn't search the user VST folder by default: see Undertow's README, "On the computer". After a
rebuild, quit FL completely before trying the new version; a running host keeps the old binary loaded.

## Presets

| Preset | What it is |
|---|---|
| **Breathing Wah** | The original idea. A slow follower (150 ms up, 1.5 s down) opens a wah as you dig in. A 2-bar LFO sways underneath, and it moves more the harder you play |
| **Classic Quack** | A fast transient envelope: the usual funk auto-wah, for comparison |
| **Slow Tide** | Lowpass, an 8-bar sine, heavy Breathe, wide stereo spread |
| **Tempo Wobble** | Resonant lowpass on a 1/8-note sine, with some drive |
| **Vowel Talk** | Two band-passes in parallel at vowel frequencies, moving in opposite directions on a drifting LFO |
| **Random Steps** | A band-pass jumping to a new random spot every 1/16 note |
| **Notch Sweep** | A 1-bar triangle sweeping a notch; phaser-like |
| **Smoky Room** | A driven wah into a gentle lowpass, slow drifting LFO, lots of Breathe |
| **Twin Tides** | Two wahs in parallel, panned left and right. LFO A runs at 1 bar and LFO B at a dotted half, so the two sides breathe out of step |
| **Ping-Pong Talk** | Two band-passes panned hard left and right, moving against each other at 1/4 and dotted 1/8 |
| **Foot Rock** | Each note swells the wah open over about 350 ms, and it rocks back over about a second: the pedal, without the foot |
| **Parked Wah** | The wah left half-open with high resonance: a honky, nasal lead tone. Only a little slow drift moves it |
| **Bloom** | A reverse wah: each pick snaps a lowpass shut, and it opens again as the note rings |
| **Duck & Glow** | Playing hard turns the tone darker, and easing off lets it brighten again. Lovely on arpeggios left to ring |
| **Deep Water** | Two dark lowpasses in series, drifting slowly with Breathe at maximum and wide stereo: the amp is underwater |
| **Filter Pump** | A lowpass that snaps shut on every beat and opens through it, like sidechain pumping. Made for drum loops |
| **Wide Phase** | Two notches panned apart, each on its own slow, unsynced LFO, so the stereo phasing never repeats |
| **Funk Bass** | A quick, low lowpass envelope for bass |
| **Chop** | A lowpass jumping open and shut every 1/16: stuttering pads and chords |

## Tips

- **For the breathing sound**, raise Fol Attack and Fol Release before reaching for anything else. 100–300 ms
  attack and 1–3 s release is the sweet spot. Then use Fol>A to set how far it opens.
- If the filter is always wide open or never moves, adjust **Fol Gain**: it sets where your playing sits in
  the follower's 36 dB window.
- A negative Fol>A closes the filter as you play harder: a darker, ducking feel.
- For a swell on every note, raise **Env Swell** to 200–500 ms and set Env Decay a little longer than that.
  A negative Env>A with a long decay does the reverse: the pick closes the filter and the note opens it.
- A synced LFO only locks to the bar while the MPC is playing; stopped, it runs free at the tempo's rate.
- Turning **Mix** to about 50% keeps the attack of the dry signal under a moving filter.

## What's not here (yet)

- No sidechain: the movers always follow the track's own audio.
- MPC effects get no MIDI, so nothing is triggered by notes; the Envelope listens for attacks instead.
- The display's curves are drawn at each filter's default resonance: their position is live, but their
  shape doesn't follow the Reso knob. In Series mode it shows A and B separately, not their combined response.

## CPU

Four state-variable filters per sample (two per channel), plus one `tan()` / `exp2()` / `log10()` per filter every 16 samples.
On a desktop it takes about 0.05% of a core; expect a low single-digit percentage of one core on an MPC.

## Credits and licence

MIT. The filters are the SDK's implementation of Andy Simper's (Cytomic) topology-preserving state-variable
filter, also MIT. Everything else is original.
