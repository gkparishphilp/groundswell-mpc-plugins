# Groundswell MPC plugins

Open-source instrument and effect plugins for Akai MPC standalone hardware (MPC Live, X, One, Key, Force and
others), built with the MPC Plugin SDK. Each plugin's README says what it does, what is not implemented, and what
it costs in CPU; anything borrowed is credited there with its licence.

| Plugin | Kind | Based on | Licence |
|---|---|---|---|
| [Undertow](Undertow/) | Effect: dub delay with filter, FX, pitch shift, reverse and modulation in the feedback loop; custom skin | Original design | MIT |
| [Tidepools](Tidepools/) | Effect: granular call-and-response — listens to a phrase, waits for the beat, answers it through one of seven grain engines; pitch tracking, 21 programs, custom skin | Original design | MIT |

## Installing on an MPC

1. Copy the `Groundswell - VST - <Name>` folder into a folder named `VST2 Plugins` on a drive the MPC can see.
   The internal drive is the safest choice: removable drives are often mounted without permission to run plugins.
2. The MPC needs **MPC OS 3.10.1 or later**.
3. Turn on **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support**.
4. Restart the MPC. The plugin is listed under **External Plugins** in the plugin browser.

No modification of the MPC is needed: this is the official route for third-party plugins.

## Building from source

Each folder is a project for the MPC Plugin SDK. Inside a checkout of the SDK, place this repository at `plugins/`
and run:

```
python tools/build.py plugins/<Name>
```

This compiles the plugin for 32-bit and 64-bit ARM MPCs (and natively, for testing) and writes the
`Groundswell - VST - <Name>` folder to `plugins/<Name>/build/`.

## Licence

MIT unless a plugin's README says otherwise; see [LICENSE](LICENSE). Ported code keeps its original copyright
notice in the plugin's README.
