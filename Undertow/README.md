# Undertow

A dub delay for Akai MPC standalone hardware, by Groundswell. The feedback loop is an effects chain: every
repeat goes back through a resonant filter, a pitch or frequency shifter, a selectable effect and a tube-style
drive stage, so the repeats change as they decay. Reverse plays every repeat backwards. Push the feedback past 100% and the resonance towards the top and it self-oscillates; a
limiter on the repeats keeps that loud but under control.

Undertow is an original design, inspired by the idea of desktop dub delays with filters in the feedback path.
It contains no code from any commercial product.

```
in ─┬──────────────────────────────────────────────────────────────────────────────────────────────┐ dry
    └► Input ─► delay ─► CHARACTER ─► FILTER ─► [PITCH ─► FX] ─► (SPRING) ─► DRIVE ─┬─► REVERSE ─► (SPRING) ─► WET LIMITER ─► mix ┴─► Output ─► out
         ▲      (Digital/Tape/BBD)          level-matched    In Loop          │                 After
         └─────────────────────────────── x Feedback (or Freeze) ◄───────────────┘     (Ping-Pong: left and right swap on every repeat)
```

## The screen

Undertow has its own skin: a black panel with a glowing tube, six pages (**DELAY**, **HEADS**, **TAPE**, **LOOP**, **MOD** and **SPACE**), and three rows
of controls on each page that line up with the Q-Link knobs: the top row is on Q-Links 13–16, the middle row on
9–12, the bottom row on 5–8. Touch a control to bring its row onto the knobs; double-tap a knob (or press Enter)
for a large edit view. The **FEEDBACK SAFETY** lamp lights when Feedback is at 100% or above, where the loop can
self-oscillate.

### Page 1: DELAY

| Control | Range | Default | What it does |
|---|---|---|---|
| Input | −∞, −30…+12 dB | 0 dB | Level into the delay (the dry signal is not affected). Push it to drive the loop harder; pull it to −∞ and back for dub throws. |
| Age | 0–100 % | 50 % | How worn the Tape or BBD character is (see Character). No effect in Digital. |
| Dry / Wet (Mix) | 0–100 % | 35 % | Dry stays at full level up to 50% while the repeats fade in; from 50% the dry fades out. |
| Output | −30…0 dB | 0 dB | Final level. |
| Delay Time | 10–5000 ms, or 1/64–2/1 | 1/8D | Delay time. In Sync the knob snaps to note divisions of the MPC tempo (with dotted and triplet values); in Free it is in milliseconds. The big display shows it too, and can be dragged. Changing it glides like tape, so turning it warps the repeats' pitch. |
| Time Mode | Sync / Free | Sync | Sync: Delay Time follows the MPC tempo. Free: milliseconds. |
| Character | Digital / Tape / BBD | Digital | The delay medium, applied to every repeat, so each one carries more of it. **Digital**: clean. **Tape**: high frequencies roll off more on every pass (12 kHz at Age 0, 3 kHz at 100), a little bass loss, soft tape compression, hiss and extra wow. **BBD**: a bucket-brigade chip of 4096 to 900 stages (Age), so its bandwidth follows the delay time: short delays are clear, long ones dark and gritty with sample-and-hold grain; harder saturation. Tape loses a little level per repeat, as the real thing does. Neither mode adds noise of its own any more: that is the Noise knob on TAPE, which works in every mode, Digital included. |
| Straight / Ping-Pong | | Ping-Pong | Ping-Pong: repeats bounce left and right (the input is summed to mono and starts on the left). |
| Freeze | Off / On | Off | Holds the loop: what is in the delay (one delay time, or two in Ping-Pong) repeats forever at exactly the same level and new input is kept out. Filter, pitch, FX, drive and the MOD page then shape only what you hear, so you can sweep them over a frozen loop without wearing it away. Feedback is ignored while frozen; changing Delay Time warps the frozen loop's pitch. Also on the SPACE page. |
| Feedback | 0–120 % | 55 % | How much of each repeat comes back. Below 100% repeats die away; above 100% they grow until the drive stage saturates them. High Resonance lowers that point. |
| Cutoff | 40 Hz–16 kHz | 1.8 kHz | Filter cutoff, smoothed so fast sweeps sound like sweeps. The RESPONSE display follows it. |
| Filter | Off / LP / BP / HP | LP | Filter inside the loop. Each repeat is filtered again, so they get darker (LP), thinner (HP) or more nasal (BP) as they decay. |
| Resonance | 0–100 % | 35 % | Makes the repeats ring at the cutoff. At full Resonance the loop self-oscillates from about 67–75% Feedback. (The RESPONSE display is drawn at the default Resonance.) |

### Page 2: HEADS

A second playback head on the same tape, with Delay Time, Feedback, Wobble and Age repeated so a pattern can be
dialled in from one page. The **echo trail** in the middle shows what the two heads are actually doing (see "The
animated display" below).

| Control | Range | Default | What it does |
|---|---|---|---|
| Head 2 | 0–100 % | 0 % | How much of the loop the second head takes over. 0 switches it off; 50 gives two equally loud taps; 100 replaces head 1. Both heads feed the same electronics and the same feedback path, as on a multi-head tape echo, and their levels always add up to one, so a second head never makes the loop louder or less stable. |
| Head Ratio | 1/4 … 2/1 | 2/3 | Where head 2 sits, as a fraction of the delay time. With a dotted-eighth delay, 2/3 gives a straight eighth against it; 1/2 and 1/3 give even subdivisions; above 1/1 head 2 lands after head 1. |
| Head 2 Pan | ±100 % | 0 % | Which side head 2 leans to. At full left or right with Head 2 at 100%, one head plays on each side. |
| Display | Off / On | On | Animates the tube and the echo trail from the audio (see "The animated display" below). |

### Page 3: TAPE

Everything about how worn the delay sounds: the medium, how far gone it is, and the noise it makes.

| Control | Range | Default | What it does |
|---|---|---|---|
| Noise | 0–100 % | 0 % | A noise bed poured into the loop just after the heads, so it is filtered, aged, driven and repeated exactly like a repeat — each crackle echoes. The knob is square-law, so the bottom half is a whisper and the top is openly lo-fi (about −30 dBFS at 100%). Two things keep it civil: the level that goes in is scaled against Feedback, so the bed sits where you put it instead of building up pass after pass, and it is gated by what is actually going round, so a track that is not playing stays silent (it fades about 3 s after the last sound). |
| Noise Type | Hiss / Air / Crackle / Hum / Rain | Hiss | **Hiss**: white, the tape-machine floor. **Air**: pink, a softer and wider wash. **Crackle**: sparse clicks, like a worn record — each one echoes with the repeats. **Hum**: 50 Hz and two harmonics, mains buzz. **Rain**: dense fine clicks, a shower behind the music. |
| LFO › Noise | ±100 % | 0 | Added to Noise, so the bed can breathe or arrive in waves. With a slow synced LFO it is the easiest way to make a part sound like it is surfacing out of something. |
| Character, Age, Wobble, Drive, Limit, Input, Feedback | | | The same controls as on DELAY and LOOP, gathered here. |

### Page 4: LOOP

| Control | Range | Default | What it does |
|---|---|---|---|
| Wobble | 0–100 % | 15 % | Tape wow (slow) and flutter (fast) on the delay time. |
| Drive | 0–100 % | 25 % | Tube-style saturation (slightly asymmetric). Always in the loop; this is what keeps runaway feedback musical. (On DELAY before 1.6.) |
| Limit | 0–100 % | 30 % | How hard the repeats are pushed into the limiter (0 to +18 dB). Higher values give denser, more slammed repeats. |
| Dry / Wet, Output | | | The same controls as on DELAY. |
| FX Amount | 0–100 % | 50 % | How much of the effect. |
| LOOP FX (FX Type) | see below | Off | The effect inside the loop. Tap a button, or touch the name to choose from a list. |
| FX Rate | 0–100 % | 30 % | The effect's second control. |
| Pitch | ±24 st, or ±1000 Hz | 0 | How far each repeat is shifted. It is inside the loop, so it compounds: +12 climbs an octave per repeat, −5 walks down in fourths. In Hz mode the knob is finest near the centre (±1 Hz to ±10 Hz gives slow phasing spirals). 0 switches the shifter off cleanly. |
| Shift Mode | Semitones / Hz | Semitones | **Semitones**: a pitch shifter in whole semitones (musical intervals). **Hz**: a frequency shifter that moves every partial by the same number of Hz, so harmonics go inharmonic: metallic, clangorous, barber-pole. |
| Direction | Forward / Reverse | Forward | Reverse plays every repeat backwards, in chunks as long as the delay time (up to 2 s). The loop itself runs forwards, so filter, pitch and FX still compound normally. Expect the first reversed repeat to arrive up to one delay time later. |
| Pitch Mix | 0–100 % | 100 % | Shifted vs unshifted repeat. Around 50% each repeat keeps its pitch and adds the shifted copy, so intervals stack into chords. |

### Page 5: MOD

One LFO and an envelope follower, wired to fixed destinations. Every depth knob is centred on zero (off) and
works in both directions. The knobs show where you set them, not where the modulation has moved things to.

| Control | Range | Default | What it does |
|---|---|---|---|
| Env Speed | 0–100 % | 30 % | How fast the envelope follower reacts: attack 1–30 ms, release 60 ms–1 s. |
| Env › Cutoff | ±4 oct | 0 | Play harder, the filter opens (or closes, below zero): envelope-filter repeats. |
| Env › FX | ±100 % | 0 | Added to FX Amount as you play harder. |
| Duck | 0–100 % | 0 % | Turns the repeats down while you play and lets them swell back in the gaps. About −10 dB at 50%; at 100% loud input (−6 dBFS) silences them. Env Speed sets how fast they come back. |
| LFO Rate | 0.02–20 Hz, or 8 bars–1/32 | 1 bar | In Sync the LFO locks to the song position while the MPC plays, so a 1-bar sweep starts on the bar. |
| LFO Mode | Sync / Free | Sync | |
| LFO Shape | Sine, Triangle, Saw Up, Saw Down, Square, Random, Drift | Sine | Random = sample & hold (a new value each cycle); Drift = smooth random wandering. |
| Spread | 0–180° | 90° | The right channel's LFO runs this far ahead of the left, so Cutoff and Time move differently on each side. |
| LFO › Cutoff | ±4 oct | 0 | Filter sweep. |
| LFO › Pitch | ±12 st (or ±200 Hz in Hz mode) | 0 | Continuous, not snapped: small amounts give a seasick wobble, large ones sirens. Inside the loop, so each repeat is caught at a different point. |
| LFO › FX | ±100 % | 0 | Added to FX Amount: the chorus breathes, the crush pumps. |
| LFO › Time | ±100 % | 0 | 100% = ±20% of the delay time: a slow, deep tape warp (Wobble is the small, fast one). |

### Page 6: SPACE

A spring reverb, plus Freeze and the filter again so you can freeze a loop and sweep it from one page.

| Control | Range | Default | What it does |
|---|---|---|---|
| Drip | 0–100 % | 50 % | How much the spring "chirps": the lows arrive later than the highs, the drip of a real spring tank. |
| Tone | 0–100 % | 50 % | Darker to brighter (a 1.5–9 kHz low-pass inside the spring). |
| Space | 0–100 % | 0 % | How much spring. 0 switches it off (and saves the CPU). |
| Position | After / In Loop | After | **After**: the spring sits on the repeats, like a tank on the echo's output. **In Loop**: inside the feedback loop, so every repeat is smeared a little more, into a wash. In Loop the spring is built so it cannot add gain at any frequency: the trail lasts longer, but Feedback still decides whether it dies away. |
| Decay | 0.3–6 s | 1.8 s | Spring decay time. |
| Freeze, Cutoff, Filter, Resonance | | | The same controls as on DELAY. |

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
Pitch and FX are wrapped in an automatic level match, so an effect changes the colour of the repeats but not
how long they last: Feedback still decides that (see "Level matching" below).

## Programs

| Program | Character |
|---|---|
| Dub Echo | Dotted-eighth ping-pong, darkening repeats |
| Siren Sweep | Ringing band-pass repeats that decay slowly: sweep Cutoff |
| Chorus Wash | Quarter-note repeats that widen and smear |
| Crushed Repeats | High-passed, bit-crushed dotted-sixteenths |
| Flange Spiral | Repeats spiral into a jet flange |
| Lo-Fi Tape | Heavy wobble and downsampling on worn tape |
| Self-Oscillation | 110% feedback into a ringing band-pass: play Cutoff like an instrument |
| Octave Climb | Each repeat adds an octave above (50% Pitch Mix) |
| Down the Well | Each repeat drops an octave |
| Barber Pole | +25 Hz frequency shift at high feedback: endlessly rising |
| Reverse Dub | Quarter-note repeats played backwards |
| Backwards Fifths | Reverse with a fifth stacked on each repeat and a touch of chorus |
| Auto-Wah Echo | Ringing band-pass that opens the harder you play |
| Sample & Hold | Random filter steps every 1/16, different on each side |
| Seasick Tape | Drifting pitch and time, light downsampling |
| Ducked Dub | Dub echo that ducks out of the way while you play |
| Breathing Flange | Flanger depth swelling over 2 bars |
| Spring Dub | Dub echo into a drippy spring |
| Washout | Chorused repeats smeared by a spring inside the loop |
| Tape Echo | Dotted-eighth tape echo, darkening and wobbling |
| Bucket Brigade | Quarter-note analog-style repeats, no filter: the BBD does the darkening |
| Twin Heads | Two tape heads: a dotted eighth and a straight eighth, head 2 to the right |
| Triplet Machine | A quarter note against its triplet, heads spread across the stereo |
| Dusty 45 | Worn tape with vinyl crackle echoing along with the repeats |
| Rain on the Roof | A shower of fine noise behind the echoes, swelling on a four-bar LFO |

(The programs do not set Display; it stays as you leave it.)

## Tips

- **Dub throws:** leave Mix at 0 and push it up for a beat to throw a single hit into the delay, then pull it back down.
- **Self-oscillation:** Feedback above 100%, or Resonance near full with Feedback above ~75%, then ride Cutoff. Drive and Limit shape how it sounds when it takes off; Feedback back below 100% lets it die away.
- **Pitch warps:** in Free mode, sweep Time while repeats are playing.
- Changing FX Type (or Pitch Mode) restarts that stage from silence, so the switch is clean but its tail is cut.
- **Shimmer:** Pitch +12, Pitch Mix 40–60%, Feedback 70%+, LP filter around 6–8 kHz to stop the climb at the top.
- **Make it old:** Noise 30–50% with Hiss, Character on Tape, Age 60+, Cutoff down around 2.5 kHz. The bed goes through the filter with the repeats, so closing the filter darkens the hiss too.
- **Crackle that plays in time:** Noise Type Crackle with Feedback 60%+ — each click lands in the delay and comes back round, so the noise picks up the tempo.
- **Noise as an effect, not a floor:** LFO › Noise at +60% with a synced LFO on 2 or 4 bars, Noise itself near 0: silence between swells, a wash on top of them.

## The animated display

Nothing in an MPC skin can animate on its own, so anything that moves with the audio has to arrive as a parameter
value. Undertow uses 65 parameters for that: "Level" for the tube and "Echo 1".."Echo 64" for the echo trail in
the middle of the HEADS page. The plugin writes them and tells the host, which is what makes the screen follow
the sound.

The trail is 64 bars, each the peak level of the repeats over one slice of time. The window is **two delay
times** wide, so a steady pattern of echoes stands still: head 1's repeat sits at the left edge and at the "one
delay" mark, head 2's sits wherever Head Ratio puts it, and the whole pattern shrinks in place as the trail
decays. The brightest bar is the newest one, sweeping round like a scope.

Each bar is also **coloured by which head fed it**: cyan for head 1, amber for head 2, pale green where both
land together. The two heads' contributions are measured separately as they come off the tape, so turning Head
Ratio slides the amber peaks between the cyan ones while you watch. The badges under the trail mark where each
head is reading. Freeze stops the pattern shrinking; Reverse, Wobble and pitch modulation all show up in it. The
bars show the repeats before Dry/Wet, so you can watch an echo build with the mix down and then throw it in.

Those host calls happen on the audio thread, so they are kept cheap: the tube is sent at most 25 times a second
and only when its brightness step changes, and the trail sends two bars per slice (the finished one and the
newly lit one), about 15 to 20 a second. All 65 are marked not automatable and are left out of saved state, so
they should not reach automation lanes or projects.

The **Display** switch (on HEADS) turns all of it off: no host calls at all, the tube is drawn as painted, and
the bars are hidden.

## Level matching

Effects that cancel or notch frequencies (chorus, flanger, phaser, ring mod, and the pitch shifter's crossfades)
remove energy from tones on every pass: measured on a sustained chord, they lost an extra 4–9 dB per repeat.
Undertow measures the signal going into the pitch and FX stages and coming out of them (40 ms, stereo-linked)
and makes up the difference, within ±12 dB, so trails last as long with an effect as without one. The filter is
not level-matched: a low-pass darkening the repeats as they decay is part of the sound. While LFO › Pitch is in use
the level match averages over 150 ms instead of 40 ms: the moving shifter's changing latency would otherwise
make it boost the start of every repeat, enough to keep the loop going on its own.

## How the limiting works

Every stage inside the loop is held to about unity gain (the filter's resonant peak may reach +3.5 dB at full
Resonance), so Feedback behaves predictably whatever filter and effect you pick.

The drive stage is always in the loop and its output is bounded, so even at 120% feedback the repeats cannot grow
without limit. The repeats then pass a stereo-linked limiter (0.3 ms attack, 80 ms release) with a −3 dBFS ceiling
and a soft knee, and the final output has a soft knee from −1 dBFS that never passes −0.1 dBFS. The dry signal is
not limited, so quiet repeats sound natural and only loud ones are clamped. Nothing adds latency.

## Not implemented yet

- A variable glide time, and a cross-feed control between straight and ping-pong
- A noise bed with its own filter, independent of the loop filter

## CPU

Per sample: one delay read per channel, a state-variable filter, the pitch stage (two interpolated taps, or an
8-section allpass Hilbert transformer), the selected effect, two saturators, the reverser and the limiter.
Filter, phaser, level-match and modulation updates (cutoff, FX Amount, pitch) run every 16 samples; the LFO, envelope
follower, ducking and time modulation run per sample with no transcendental functions.
The spring reverb costs about three times the rest of the plugin while Space is above 0 (36 allpass stages per side);
at 0 it is skipped. Head 2 is one extra read per sample, and the display four comparisons, which is nothing. Noise costs 3-10% more than the rest of the plugin put together while it is above 0 (Hum is the dearest), and nothing at 0. Memory is about 2.5 MB per instance (5 s delay and 2 s reverse buffer, stereo, at 44.1 kHz).

## Skin

The skin in `skins/` is generated by `art/make_skin.py` from the artwork in `art/` (panel, knobs, lamp and pill
by Groundswell). Edit the artwork or the layout in that script, then run `python3 plugins/Undertow/art/make_skin.py`
before building; it also writes `build/skin-preview-*.png`. `ui.json` is only a fallback and is not used while
`skins/` exists.

## Licence

MIT. See the repository's LICENSE. Uses the MPC Plugin SDK framework (MIT).

## Build

```
python tools/build.py plugins/Undertow
python tools/package.py plugins/Undertow
```
