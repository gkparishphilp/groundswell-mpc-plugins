# Hammerhead

**Hit something.** Struck, plucked, bowed and blown physical modelling for Akai MPC standalone hardware, by
Groundswell.

Hammerhead plays marimba, vibraphone, xylophone and glockenspiel bars, kalimba tines, bells, glass, a steel pan,
and plucked or hammered strings. It is built for percussion. Every note is a mallet (or a plectrum) hitting
something that rings, so the attack carries the sound. **Hardness** is how long the mallet stays on the bar,
**Strike Pos** is where it lands, **Click** is the knock of the mallet itself, **Decay** and **Damping** decide
what the object is made of, and **Mute** says whether a hand comes down on it when you let go. Hit harder and it
gets brighter, not just louder. Roll on one pad and the bar keeps ringing while each hit adds to it. It is cheap
enough to play chords on the MPC: 16 voices, about 0.2 % of a desktop core with all 16 ringing.

Since 1.1 the same objects can also be **bowed** or **blown**: a bow or a breath keeps feeding the bar, glass or
string for as long as you hold the note, so it sustains and sings, and pad pressure plays it. 1.2 adds **Buzz** (the
rattle of kalimba bottle caps and balafon gourds) and **modulation**: an LFO on the four controls that change the
sound most, and per-hit randomness, so a sequence of hits keeps moving.

## New in 1.2

- **Buzz**: a loose buzzer on the object (kalimba bottle caps, the spider-silk membranes on balafon gourds, a
  sitar-like bridge). It rattles while the note is loud and cuts out as the ring decays, as the real thing does.
  It works on every model and exciter. It's on the STRIKE page, where Drop was (Drop is still on PLAY).
- **An LFO**, free (0.05–20 Hz) or locked to the song (4 bars to 1/16T), in six shapes (Sine, Triangle, Square,
  Ramp, S&H, Drift). It has one depth knob for each of four destinations:
  - **Position** and **Hardness** are read at each hit, so a groove walks across the bar and gets brighter and
    darker. On a bowed bar the bow moves along it while the note sustains.
  - **Stretch** bends the overtones while the note rings (glass that wobbles, bells that warp).
  - **Pressure** makes bow and breath swell and fade.
- **Per-hit randomness**: **Position**, **Hardness**, **Pitch** (up to ±25 cents per note) and **Level** (up to
  ±6 dB), so repeated notes and Note Repeat rolls never sound copy-pasted.
- A **MOD** page on the panel, and **five new programs**: Barnacle Kalimba, Coral Balafon, Tide Machine, Rip
  Current, Undercurrent (20 in all).
- 1.0 and 1.1 projects load unchanged: every new control starts at off.

## New in 1.1

- **Exciter: Mallet, Bow or Blow.** Bow is stick-slip friction, so bowed vibes, glass, bells and a bowed string
  sing on their own. Blow is a breath jet, so bottles, pipes, a conch and blown glass speak once the breath is
  strong enough. Both work on every model. Mallet is the 1.0 sound.
- **Pressure** (bow pressure or breath pressure; **pad aftertouch** adds to it), **Swell** (how long the bow or
  breath takes to build), **Air** (rosin scratch or breath noise) and **Vibrato** (pitch, at Motor Rate, fading in
  after each note starts).
- A **BOW/BLOW** page on the panel, and **Exciter** on the STRIKE page in place of Octave (Octave is still on PLAY
  and BOW/BLOW).
- **Five new programs**: Bowed Vibes, Moon Jelly, Kelp Cello, Conch, Bottle Breath (15 in all).
- **Fix: a new Model didn't take effect on a note that was still ringing.** A re-hit reused the ringing voice so
  it could add to the ring, and that voice kept its old model, so after changing Model a pad that was still
  sounding (a bell rings for up to 20 s) played the old model until it died away. Now a re-hit with a different
  Model or Exciter fades the old voice out over 8 ms and starts the new object.
- 1.0 projects load unchanged: the new parameters start at Mallet and settings that add nothing.

**1.1.1: a quieter, darker Air.** The scratch and breath you hear were a broadband hiss well above the note
(centred about 2.2 kHz on a bowed A3 bar). Now they pass through a wide band-pass that follows the note (two to four
times its frequency, higher with more Pressure). The noise itself is darker, and the Air knob has a gentler curve
(the lower half is a hint, the top is still breathy). At the programs' own settings the air is 4–10 dB quieter and
an octave or more lower. Projects keep their Air value; it just sounds softer.

## Install

1. Copy the `Groundswell - VST - Hammerhead` folder (in `build/` after building, or from the release zip) into a
   folder named `VST2 Plugins` on a drive the MPC can see. The internal drive is the safest choice.
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC and load Hammerhead from **External Plugins** onto a plugin program.

## How it works

Two engines, chosen by **Model**. Each one is the cheapest model that still sounds right for its family of
objects.

**Bars, tines, bells, glass, pan: a modal resonator bank.** These objects are inharmonic, and their sound is a
handful of strong modes, each a decaying sine at its own frequency. Each voice has 8 two-pole resonators tuned to
the object's mode ratios, plus a ninth for the resonator tube. A ringing mode costs 3 multiplies and 2 adds per
sample, with no table and no `sin`.

| Model | Mode ratios (lowest first) | Character |
|---|---|---|
| Marimba | 1, 3.98, 9.38 (tuned 1:4:10) + torsional modes | Wood: overtones die fast (Damping high), tube on. |
| Vibes | 1, 3.99, 9.9 (1:4:10) | Aluminium: long ring, low damping; pair with Motor and Mute. |
| Xylophone | 1, 3.0, 6.0 (tuned to a twelfth) | Hard, dry and bright. |
| Glock | 1, 2.76, 5.40, 8.93, 13.3 (untuned free bar) | Steel bar, glassy and inharmonic. No program uses it yet; pick it with Model. |
| Kalimba | 1, 6.27, 17.55, 34.39 (clamped tine) | A strong fundamental with a quick "ting" overtone. |
| Bell | hum 0.5, prime 1, tierce 1.2, quint 1.5, nominal 2, … | A minor-third church bell; the hum rings longest. |
| Glass | 1, 2.32, 4.25, 6.63 … with split pairs | The pairs beat slowly, which gives the shimmer of real glass. |
| Steel Pan | 1, 2, 3, 4 (tuned octave and twelfth) | Bright and round. |

**Strings: a waveguide.** Strings are harmonic, with dozens of audible partials, so a modal bank would need 30 or
more resonators. **Pluck** (nylon-like) and **Dulcimer** (metal, hammered) use one delay line per voice instead
(Karplus-Strong family): a cubic read, a one-pole loss filter and a gain, about 20 operations per sample at any
pitch. The string is tuned with the loss filter's phase delay compensated, so it plays in tune (within a cent
in the tests).

### The strike

- **The mallet** is a half-sine force pulse whose length is the contact time: about 5 ms for a soft yarn mallet
  (only the low modes ring), down to 0.1 ms for hard plastic (everything rings). Higher notes get shorter
  contacts, as on a real instrument, and the contact never gets long enough to swallow the fundamental.
  **Vel>Hard** makes harder hits use a harder mallet.
- **Click** is a burst of band-passed impact noise sent straight to the output: the thwack of the mallet.
  A little of the same noise also goes into the resonators, so no two hits are identical. Each hit also varies
  by a few percent in level and contact time.
- **Strike Pos**: on the bars it weights each mode by the bar's mode shape where you hit it. Dead centre removes
  the 4:1 overtone (it has a node there). About a third of the way out you hit the fundamental's node and the
  sound goes thin and bright. On bells, glass, tines and pans it tilts from dark (0 %) to bright (100 %). On the
  strings it is the pluck point: the middle is hollow (no even harmonics), near the bridge is thin and bright.
- **Drop**: the pitch starts up to 12 semitones sharp and falls to the note over **Drop Time**, further on harder
  hits. Use it for log drums, toms, tabla-like bends and sonar pings. It works on both engines.

### The ring

- **Decay** is the T60 of the lowest mode at middle C. Higher notes ring shorter and lower notes longer, as on
  real instruments.
- **Damping** makes each higher mode die faster than the one below it: high for wood, low for metal and glass. On
  the strings it is the loss filter: higher is darker and duller.
- **Mute** chokes the ring on note-off. At 0 % note-offs are ignored and every hit rings out (marimba). At 100 %
  it is a palm on the bar (about 60 ms). The **sustain pedal** (CC 64) holds muting off like a vibraphone's
  damper pedal, and lifting it chokes the notes you have let go.
- **Resonator**: a tuned tube under the bar, driven by the fundamental. It makes the low end louder and rounder,
  blooms in slightly after the strike, and takes a little sustain from the fundamental, as a real tube does.
- **Stretch** bends all the mode ratios: down squeezes the overtones together, up spreads them apart (turns bars
  into bells).

### Bow and Blow (1.1)

A mallet is one impulse: however slow its attack, the object then just rings away. A bow or a breath keeps
putting energy in while the note is held (and while the sustain pedal holds it), so the object sustains. Both
build up over **Swell** and stop about 60 ms after the note-off, and then the object rings on with its own
**Decay** (or **Mute**). Each note starts with a soft push (the bow's first grip, or a tongued breath) scaled by
velocity, so short Swell settings speak at once. A long Swell makes the push small, so slow swells still bloom.

- **Bow, bars and the other modal models**: stick-slip friction at the contact point. The bow drags the bar
  along until the bar's own velocity tears it free, then it grips again, once per cycle. The friction curve is
  the classic bow table, a power law in the relative velocity. The contact velocity is the sum of the mode
  velocities.
- **Bow, strings**: the string is split at the bow point into a nut-side and a bridge-side delay line with the
  friction junction between them (the McIntyre–Schumacher–Woodhouse / Smith bowed string). **Strike Pos** is the
  bow point: 0 % is over the middle of the string (soft, flautando), 100 % is near the bridge (bright, glassy).
- **Blow**: a jet. The contact velocity, through a saturating gain, pushes back on the object (on the strings, on
  the waveguide, which then behaves as an air column), so it self-oscillates at its own resonance, plus turbulent
  breath noise. On the strings, a low-pass in the jet keeps the fundamental in charge.
- **Pressure**: on the bow, how hard it presses. More is grittier, and at the top it starts to choke and gets
  quieter, as a real bow does. On the breath, how hard you blow: louder, brighter and noisier. Pad aftertouch
  (channel or poly) adds to it.
- **Air**: rosin scratch (Bow) or breath noise (Blow), into the object and straight to the output. What you hear
  directly is band-passed round two to four times the note, so it sounds like the instrument breathing rather
  than a hiss on top.
- **Vibrato**: up to ±35 cents at **Motor Rate**, fading in over 0.4 s after each note starts. It works on every
  exciter, including the mallet.
- Every mode feels the same bow or breath, scaled to the loss of the note's own mode. So the long-ringing note
  mode speaks first, and short-lived upper modes only take over when you push hard (overblowing). A bowed or
  blown **bell** sings at the note; its hum, an octave below, is left out.

### Buzz (1.2)

A loose buzzer touches the object whenever it swings past a threshold, a fraction of the note's own peak. The
part of the wave beyond the threshold, roughened by contact noise and high-passed at 1.2 kHz, is the rattle, and
it is added to the note. So it rattles while the note is loud and stops by itself as the ring decays below the
threshold. More **Buzz** is louder and lowers the threshold, so it buzzes for longer. Soft notes buzz less,
because the threshold never drops below that of a medium-strength hit.

### Modulation (1.2)

One LFO for the whole instrument (not one per note), so all the voices move together, like one player.

- **LFO Sync**: Free (**LFO Rate**, 0.05–20 Hz) or Sync (**LFO Div**, 4 bars … 1/16T, locked to the song position
  while the MPC plays; at the project tempo otherwise).
- **LFO Shape**: Sine, Triangle, Square, Ramp (falling), S&H (a new random value each cycle), Drift (glides from one
  random value to the next).
- **LFO>Pos** and **LFO>Hard** (±50 % of the range at full depth) are read at each hit. With S&H on 1/16, a roll on
  one pad lands somewhere new each time. On a bowed or blown modal note, LFO>Pos also moves the bow or mouth
  along the bar while it sounds.
- **LFO>Stretch** (±100 % of Stretch at full depth) bends the overtones of every ringing modal note. The note's own
  mode stays put, so the pitch holds while the timbre warps.
- **LFO>Press** (±50 %) moves bow and breath pressure. A long-ringing object smooths a fast swell (the measured
  swing at 1 Hz on a 3 s glass is about 3 dB; at 0.4 Hz it is about 6 dB).
- **Rnd Pos**, **Rnd Hard**, **Rnd Pitch** (±25 cents, chosen once per note) and **Rnd Level** (±6 dB) are picked
  fresh for every hit.

### After the voices

**Motor** (the vibraphone's spinning discs: tremolo depth, plus **Motor Rate**; the mod wheel adds depth) →
**Body** (four resonant band-passes, a wooden box; **Body Size** moves them) → **Room** (an 8-line feedback delay
network, **Room Size** is its T60) → **Volume** → a soft limiter that does nothing below −3 dBFS and holds peaks
under −0.2 dBFS. **Width** pans each note by pitch, like standing in front of a keyboard instrument.

## Parameters

Parameter IDs and order are permanent; new parameters will be added at the end.

| # | Name | Range | Default | What it does |
|---|---|---|---|---|
| 0 | Volume | −40…+6 dB | −3 dB | Output level. |
| 1 | Model | Marimba … Dulcimer | Marimba | What is being hit: Marimba, Vibes, Xylophone, Glock, Kalimba, Bell, Glass, Steel Pan, Pluck, Dulcimer. Applies to new notes. |
| 2 | Hardness | 0–100 % | 45 % | Mallet hardness: contact time from ~5 ms (dark) to 0.1 ms (bright). |
| 3 | Strike Pos | 0–100 % | 25 % | Where it is hit: 0 = centre of the bar / middle of the string, 100 = towards the end. |
| 4 | Click | 0–100 % | 30 % | Level of the mallet's impact noise. |
| 5 | Decay | 50 ms–30 s | 1.6 s | Ring time (T60) of the lowest mode at middle C. |
| 6 | Damping | 0–100 % | 65 % | How much faster the upper modes die (wood high, metal low). |
| 7 | Mute | 0–100 % | 0 % | How hard a note-off (or pedal up) chokes the ring. |
| 8 | Stretch | −100…+100 % | 0 | Squeezes or spreads the mode ratios (modal models). |
| 9 | Resonator | 0–100 % | 55 % | The tuned tube under the bar (modal models). |
| 10 | Body | 0–100 % | 15 % | Wooden-box resonance. |
| 11 | Body Size | 50–200 % | 100 % | Size of the box (bigger is lower). |
| 12 | Motor | 0–100 % | 0 % | Vibraphone tremolo depth (the mod wheel adds to it). |
| 13 | Motor Rate | 1–12 Hz | 5 Hz | Tremolo speed. |
| 14 | Width | 0–100 % | 50 % | Stereo spread of the notes by pitch. |
| 15 | Room | 0–100 % | 20 % | Reverb amount. |
| 16 | Room Size | 0.3–6 s | 1.2 s | Reverb decay time. |
| 17 | Velocity | 0–100 % | 70 % | How much velocity changes the level. |
| 18 | Vel>Hard | 0–100 % | 60 % | How much velocity hardens the mallet. |
| 19 | Octave | −2…+2 | 0 | Transposes by octaves. |
| 20 | Fine | −50…+50 ct | 0 ct | Fine tuning. |
| 21 | Drop | 0–12 st | 0 st | Pitch drop at the strike (scaled by velocity). |
| 22 | Drop Time | 10–500 ms | 60 ms | How fast the drop falls to the note. |
| 23 | Exciter | Mallet / Bow / Blow | Mallet | What sets the object going (1.1). Applies to new notes. |
| 24 | Pressure | 0–100 % | 50 % | Bow or breath pressure; pad aftertouch adds to it (1.1). |
| 25 | Swell | 5 ms–3 s | 150 ms | How long the bow or breath takes to build up (1.1). |
| 26 | Air | 0–100 % | 25 % | Rosin scratch (Bow) or breath noise (Blow) (1.1). |
| 27 | Vibrato | 0–100 % | 0 % | Pitch vibrato, up to ±35 cents at Motor Rate (1.1). |
| 28 | Buzz | 0–100 % | 0 % | A buzzer's rattle while the note is loud (1.2). |
| 29 | LFO Sync | Free / Sync | Free | LFO in Hz, or locked to the song (1.2). |
| 30 | LFO Rate | 0.05–20 Hz | 0.5 Hz | LFO speed when Free (1.2). |
| 31 | LFO Div | 4 bars … 1/16T | 1 bar | LFO cycle when Synced: 4 bars, 2 bars, 1 bar, 1/2, 1/4, 1/8, 1/16, 1/4T, 1/8T, 1/16T (1.2). |
| 32 | LFO Shape | Sine … Drift | Sine | Sine, Triangle, Square, Ramp, S&H, Drift (1.2). |
| 33 | LFO>Pos | −100…+100 % | 0 | LFO to Strike Pos, read at each hit; moves a bow while it sounds (1.2). |
| 34 | LFO>Hard | −100…+100 % | 0 | LFO to Hardness, read at each hit (1.2). |
| 35 | LFO>Stretch | −100…+100 % | 0 | LFO to Stretch on ringing notes (1.2). |
| 36 | LFO>Press | −100…+100 % | 0 | LFO to bow and breath Pressure (1.2). |
| 37 | Rnd Pos | 0–100 % | 0 % | Random strike position per hit (1.2). |
| 38 | Rnd Hard | 0–100 % | 0 % | Random hardness per hit (1.2). |
| 39 | Rnd Pitch | 0–100 % | 0 % | Random detune per note, up to ±25 cents (1.2). |
| 40 | Rnd Level | 0–100 % | 0 % | Random level per hit, up to ±6 dB (1.2). |

Pitch bend is ±2 semitones and bends ringing notes too.

## The panel

A hammerhead swimming through the light rays over a reef, under the HAMMERHEAD / RESONATOR title. Each row is
one Q-Link row: two boxes of two controls, either side of the shark. The panel is drawn by `art/make_skin.py`.

| Page | Rows (left box \| right box) |
|---|---|
| STRIKE | MALLET: Hardness, Position \| RING: Decay, Damping · IMPACT: Click, Buzz \| BAR: Resonator, Mute · BODY: Body, Size \| ROOM: Room, Size · MODEL: Model, Exciter \| OUTPUT: Width, Volume |
| PLAY | DROP: Drop, Time \| MOTOR: Motor, Rate · VELOCITY: Level, Hardness (Vel>Hard) \| TUNING: Octave, Fine · OVERTONES: Stretch, Damping \| DAMPER: Mute, Decay |
| BOW/BLOW | SOURCE: Exciter (Mallet / Bow / Blow buttons), Pressure \| BREATH: Swell, Air · VIBRATO: Vibrato, Rate \| RING: Decay, Mute · MODEL: Model, Position \| TUNING: Octave, Fine |
| MOD | LFO: Sync (Free / Sync buttons), Shape \| SPEED: Rate (free), Div (synced) · LFO TO: Position, Hardness \| LFO TO: Stretch, Pressure · RANDOM: Position, Hardness \| RANDOM: Pitch, Level |

Model and Exciter on the STRIKE page are knobs that open their lists when touched.

## Programs

| Program | Character |
|---|---|
| Rosewood Marimba | The default: medium mallets just off centre, tube on, a short room. |
| Harbour Vibes | Vibraphone an octave up, motor at 45 %, Mute 65 % so releases damp (hold the pedal to let them ring). |
| Reef Xylophone | Hard mallets, short and bright, an octave up. |
| Shell Kalimba | Kalimba an octave up in a small wooden box. |
| Buoy Bell | A long church-bell ring in a big room. |
| Sonar Glass | Glass with a slow one-semitone drop and a long room: a ping. |
| Island Pan | Steel pan, an octave up, with a tiny pitch drop on each hit. |
| Nylon Pluck | A plucked string near the bridge in a guitar-sized body. |
| Hammered Dulcimer | Metal strings, hard hammers, an octave up. |
| Hull Drum | Low marimba bars hit near the centre with a 5-semitone drop: a log drum or tom. |
| Bowed Vibes | A bowed vibraphone bar, an octave up: a pure, singing tone with the tube on (1.1). |
| Moon Jelly | Bowed glass with a slow 0.8 s swell, a little vibrato and a big room (1.1). |
| Kelp Cello | A bowed string near the bridge in a big body, with vibrato (1.1). |
| Conch | The metal string as a blown air column: breathy and round, with vibrato (1.1). |
| Bottle Breath | Glass, blown: lots of breath noise, like blowing across a bottle (1.1). |
| Barnacle Kalimba | Kalimba with bottle-cap buzz, a little random position, hardness and level on each hit (1.2). |
| Coral Balafon | Marimba bars with buzzing gourds and a loose, human feel: random position, hardness, pitch and level (1.2). |
| Tide Machine | Marimba with an S&H LFO locked to 1/16 on strike position and hardness: hold Note Repeat (1.2). |
| Rip Current | Glass whose overtones bend slowly with a triangle LFO on Stretch (1.2). |
| Undercurrent | Bowed vibes with a drifting LFO moving the bow along the bar and swelling its pressure (1.2). |

## Tips

- **On the pads**: the MPC's pads send low notes (C1–D#2 by default). Most programs are set an octave up so they
  sit in a useful range. Use **Octave** to move any program to where your pads are.
- **Q-Links**: the STRIKE page's top row (Hardness, Strike Pos, Click, Decay) is the one to play live. Sweeping
  Strike Pos while rolling on a pad is the most physical-sounding move there is.
- **Rolls and repeats**: a repeated note stays on the same voice and adds to the ring, so Note Repeat on a
  marimba or vibes program sounds like a real roll, not a machine gun.
- **Drums**: Hull Drum is a start. Try Kalimba or Steel Pan with Drop 3–7 st and Drop Time 40–100 ms for
  tuned percussion, or Glass with a long drop for effects.
- **Bowed and blown on the pads**: press harder into a held pad to push the bow or breath (aftertouch). Swell
  sets how fast a note grows; short swells speak at once, long ones bloom. Hold the sustain pedal to keep bowing
  after you let go.
- **Modulation on the pads**: Tide Machine with Note Repeat at 1/16 is the quickest way to hear it. Each repeat
  lands somewhere new on the bar. Then try S&H or Drift on LFO>Pos with any program, and a little Rnd Level and
  Rnd Pos to humanise rolls.
- **Buzz** suits kalimba, marimba (a balafon) and plucked strings (a sitar-ish jangle). Set it high with a short
  Decay for a snare-like rattle.
- **Changing Model or Exciter** affects the next note. A pad that is still ringing switches over on its next hit.
- **Polyphony**: 16 notes ring at once. A 17th note takes over the quietest one, which fades out over 8 ms in a
  spare slot, so the new note is not delayed and nothing clicks.

## CPU

On a desktop, all 16 voices ringing take about **0.18 %** of one core (vibes, 8 modes + tube each) or **0.16 %**
(dulcimer strings). With no notes ringing, the body and room take **0.05 %**. By the ratio seen with the other
Groundswell plugins, that should be roughly **2–4 %** of one MPC core with all 16 voices ringing, and much less
in normal playing, where most voices have died away. This is an estimate from the desktop, not yet measured on
the hardware.

Bowed and blown notes cost more, because the object and the bow or breath have to be worked out together, one
sample at a time. 16 bowed bars take **0.94 %** of a desktop core (about 0.06 % each), and 16 bowed strings
**0.34 %**. On the MPC a bowed bar is roughly **1 %** of a core per note, so a held chord of four to six is fine.
All 16 bowed at once could reach 10–20 % on the slower MPCs. These are estimates from the desktop.

Buzz costs a few operations per sample per voice while it is on (16 buzzing kalimba voices: **0.28 %**). LFO>Stretch
re-tunes the ringing modes every 32 samples (16 voices with it on: **0.20 %**).

How it stays cheap: `exp`, `cos` and `pow` run once per note, every 32 samples while a note's pitch is moving
(Drop, pitch bend, vibrato), or when a knob that changes the ring moves. They never run per sample. A voice is retired once
it has been below −90 dB for 100 ms, so a finished note costs nothing.

## What is not in this version

- **Buzz takes no energy from the note**: a real buzzer shortens the ring a little; here the ring is the same with
  or without it.
- **One LFO, four destinations, shared by all the notes.** There is no envelope or velocity routing beyond the
  built-in ones (Vel>Hard, aftertouch to Pressure, mod wheel to Motor), and no per-note LFO. LFO>Stretch does
  nothing on the two string models.
- **No scraping or rubbing**: Bow is a single bow on one point; there is no rubbed rim (glass harmonica finger) or
  scraper. Bow pressure, not bow speed, is what you play (velocity sets the starting bow speed).
- **The string Blow is a simple air column**: no reed, and no separate jet delay, so it doesn't jump registers
  the way a real flute does when overblown.
- **8 modes per object.** Enough for the character of each object, but very bright, hard hits on low notes are
  rounder than a real bar, because there are no modes above the 8th.
- **Strings are single**: a real dulcimer has pairs of strings that beat against each other; here there is one
  string per note. **Stretch** and **Resonator** do nothing on the two string models.
- **Model, Exciter, Hardness, Strike Pos, Click, Width and velocity apply to new notes**, as they would on a real
  instrument. Decay, Damping, Mute, Stretch, Resonator, Octave and Fine also change notes that are already
  ringing.
- **No sympathetic resonance** between notes.
- **Drop** has one shape, an exponential fall. You can't make the pitch rise.

## Testing

```
python3 plugins/Hammerhead/art/make_skin.py
python3 tools/build.py plugins/Hammerhead
.sdk-tools/host_sim plugins/Hammerhead/build/native/hammerhead.dylib test --seconds 4 --wav plugins/Hammerhead/build/test.wav
.sdk-tools/host_sim plugins/Hammerhead/build/native/hammerhead.dylib test --notes 36,48,55,60,64,67 --seconds 6
cd plugins/Hammerhead && mkdir -p tests/build
clang++ -std=c++17 -O2 -I../../framework tests/listentest.cpp -o tests/build/listentest
tests/build/listentest build/hammerhead-demo.wav
```

host_sim: 19 passed, 0 warnings, 0 failures (also at 48 and 96 kHz). `tests/listentest.cpp` renders with random
block sizes (32–512) and checks (151 checks):

- **every mallet program** is percussive: within 5 ms of the strike it is within 0–5 dB of its peak. A single
  note at velocity 100 peaks between −14 and −22 dBFS, and the voice frees itself when the ring dies. **Every
  bowed and blown program** holds steady while the note is held (within 1 dB) and dies away after it;
- **every model** plays A4 at 440 Hz (within 0.4 cents);
- **Hardness**: a hard mallet rings the overtones 24 dB louder than a soft one, and velocity does the same through
  Vel>Hard (4:1 overtone −32 dB → −10 dB re the fundamental from velocity 30 to 127);
- **Strike Pos**: on the marimba, the centre removes the 4:1 overtone (−15 dB relative) and the node thins the
  fundamental by 17 dB. On a string plucked in the middle the 2nd harmonic drops by 47 dB;
- **Decay** measures right (marimba 2.00 s for 2 s, vibes 6.01 s for 6 s, strings 2.7–2.8 s for 3 s);
- **Mute** chokes a note-off by more than 20 dB in 0.4 s; with the sustain pedal down it rings on unchanged, and
  pedal up chokes it without a click;
- a **roll** on one note stays on one voice and does not click; **24 ringing notes** use at most 16 voices, and
  stealing makes no clicks;
- **Drop** starts sharp and lands within a cent of the note on a bar and on a string; **pitch bend** moves ringing
  notes on both engines;
- a **six-note chord at velocity 127** stays under 0 dBFS in every program (peaks −0.6 to −10.8 dBFS, under
  0.05 % of samples touching the limiter);
- (1.1) re-hitting a ringing note after changing **Model** plays the new model (the old voice fades), and the
  same for **Exciter**;
- (1.1) **Bow and Blow on every one of the 10 models**: each speaks within 0.1 s (within 10 dB of full), holds
  steady, plays A3 within 1 cent (by autocorrelation), and rings away after release;
- (1.1) **Swell** 50 ms is within 1 dB of full after 0.1 s, and 1.5 s is still 38 dB under; full **aftertouch**
  blows 6 dB harder; **Vibrato** 100 % swings the pitch over about 160 cents;
- (1.2) **Buzz** 60 % adds 13 dB above 3 kHz to a loud kalimba note and nothing once it has decayed (1.2 s
  later), and a six-note chord with Buzz at 100 % stays under 0 dBFS; the **LFO** in Sync follows the song
  position; LFO>Pos (S&H, 1/16) spreads 16 hits from 5 % to 73 % of the way out; LFO>Stretch swings a ringing
  glass's 2.32× mode 1123 → 928 Hz while the fundamental holds; LFO>Press swings a blown note 6 dB; **Rnd Pitch**
  lands twelve notes within ±24 cents; **Rnd Level** varies hits over 12 dB;
- (1.1.1) **Air** on a bowed or blown bar: at 25 % the noise is 35 dB under the tone (about 1 dB more than with no
  air); at 100 % it is 24 dB under, centred near 830 Hz, with only −32 dB of it above 3 kHz;
- CPU as above.

It then writes `build/hammerhead-demo.wav` (96 s): each of the 20 programs in turn plays a short pentatonic
phrase and a chord (Hull Drum plays a little groove; the bowed and blown programs play it legato), each ringing
into the next.

## Project layout

- `plugin.json`: manifest (name, manufacturer, uid). The `uid` (GvPT) must match the C++ `uniqueId`.
- `src/Hammerhead.cpp`: the whole plugin (models, parameters, programs, MIDI, exciter, resonators, strings,
  body, room, output stage).
- `art/make_skin.py`: draws the panel over `art/plate-src.webp` into `skins/` (with previews in
  `build/skin-preview-*.png`). Run it **before** `tools/build.py`, and again whenever parameters change.
- `art/plate-src.webp`: the background art. `art/mock-reference.webp`: the mock-up of the look (reference only).
- `art/knob_small.webp`, `art/pill_on.webp`, `art/pill_off.webp`: the shared Groundswell knob and button art.
- `skins/`: the rendered panel (TUI.json, Q-Links.json, page backgrounds, knob images).
- `ui.json`: a stock-kit fallback layout (amber, the same four pages), unused while `skins/` exists.
- `tests/listentest.cpp`: the offline sound, tuning, feature, click, level and CPU checks, and the demo.

## Licence

MIT. Original design and code. The mode ratios come from the published acoustics of each instrument (tuned bars
1:4:10 and 1:3:6, free-bar and clamped-bar beam theory, the minor-third church bell partials, as in Fletcher and
Rossing, *The Physics of Musical Instruments*). The string is the Karplus-Strong / digital waveguide idea
(Karplus and Strong 1983, Jaffe and Smith 1983, Smith 1992). The bowed string is the McIntyre, Schumacher and
Woodhouse (1983) / Smith bowed-string structure with the widely published power-law bow table (as in the
Synthesis ToolKit's description); the modal bow and the jet follow the same friction and jet ideas applied to
modal resonators (Adrien 1991; Essl and Cook 1999). The room follows the general feedback delay network
structure of Jot and Chaigne (1991), as in Groundswell Abyss. No code was borrowed.
