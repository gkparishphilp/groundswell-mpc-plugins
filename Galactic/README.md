# Galactic

A huge, dark, shimmering ambient reverb for Akai MPC standalone hardware, by Groundswell.

Galactic is a port of **Airwindows Galactic** by Chris Johnson to the MPC Plugin SDK. The reverb is a 12-delay
feedback network in three blocks of four, cross-fed between left and right, with a slowly swept pre-delay that
gives the tail its drifting, detuned shimmer. It is at its best on pads, keys, vocals and anything you want to
send into space.

## Controls (one page: GALACTIC)

| Control | Range | Default | What it does |
|---|---|---|---|
| Replace | 0–100 % | 50 % | How quickly new sound replaces the tail. Low = long decay. **0 = freeze**: the input is shut off and whatever is in the tail keeps ringing. |
| Bright | 0–100 % | 50 % | Lowpass before and after the tank. Low = dark and distant, high = open. |
| Detune | 0–100 % | 50 % | Speed of the pre-delay vibrato: the shimmer and pitch drift in the tail. 0 = none. |
| Bigness | 0–100 % | 100 % | Size of every delay in the network. Small values give a room; 100 % is the cathedral. |
| Mix | 0–100 % | 100 % | Dry/wet on Airwindows' curve (wet = 1 − (1 − Mix)³): the reverb is already loud at 30–40 %. |
| Output | −24…+6 dB | 0 dB | Level after the mix. |

The top row of Q-Links is Replace, Bright, Detune, Bigness; the second row starts with Mix and Output.

## Programs

| Program | Use |
|---|---|
| Galactic | Upstream defaults: 100 % wet, for a return (send) track |
| Insert Wash | Same character at 30 % mix, straight on a track's insert |
| Dark Matter | Long, murky and dark |
| Nebula Shimmer | Bright tail with heavy drift |
| Small Orbit | Short and roomy, for drums |
| Event Horizon | Near-endless decay for drones and pads |

## Tips

- The default is 100 % wet. Put Galactic on a **return track** and send to it, or pick *Insert Wash* (or turn Mix down to 20–40 %) on an insert.
- For a freeze effect: play a chord, then turn Replace to 0. The tail holds until you turn it back up. Turning Bigness while frozen bends the held sound.
- Moving Bigness while sound is playing changes the delay lengths at once, which produces glitchy jumps. That is how the original behaves and it can be used as an effect; set it before playing for a clean tail.

## What is different from the original

- Delay memory is 32-bit float instead of 64-bit double (about 0.5 MB per instance instead of 1 MB). Not audible.
- The vibrato sine is computed with a rotating phasor instead of `sin()` per sample (same shape and speed, much cheaper on ARM).
- Airwindows' 32-bit floating point dither on the output and its denormal-avoidance noise are left out; the MPC mixes in float and the SDK framework flushes denormals.
- Added: the Output control, smoothing on Mix and Output, and the six programs.

## CPU

Twelve delay lines per channel and a few multiplies per sample, no transcendental functions in the sample loop.
Expect a few percent of one core per instance on the MPC; see the host simulator report for a measured figure.

## Licence and credits

- Galactic algorithm: © Chris Johnson / Airwindows, MIT licence (<https://github.com/airwindows/airwindows>).
- This port: Groundswell, MIT licence. The SDK framework is MIT.

Airwindows' licence, which must stay with copies of this plugin:

```
MIT License

Copyright (c) 2018 Chris Johnson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Build

```
python tools/build.py plugins/Galactic
python tools/package.py plugins/Galactic
```
