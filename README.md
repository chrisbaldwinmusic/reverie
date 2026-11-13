> **This repository has moved.** Reverie is now developed in the Channel Audio monorepo, [chrisbaldwinmusic/channelaudio](https://github.com/chrisbaldwinmusic/channelaudio), in the `Reverie/` folder, with its full history. This repository is archived and kept for reference only.

# Reverie

A boutique delay and reverb plugin: one unit that covers slapback to shimmer. AU, VST3 and Standalone, built with JUCE. A sibling of Atelier, the channel strip, and built the same way.

Two effects, three ways to connect them:

- **Delay into Reverb** — every echo lands in the same space as the dry sound.
- **Parallel** — echoes stay dry and distinct, the reverb stays clean.
- **Reverb into Delay** — the whole tail is repeated, for swells and washes.

## What's in it

- **Delay** — five characters that differ in how each repeat is coloured on its way round the feedback loop: Digital, Tape (wow, flutter, saturation, head bump), Analog (a modelled bucket-brigade pedal: two MN3005 chips run by a clock that follows the delay time, their anti-aliasing and reconstruction filters, charge-transfer loss and an NE570 compander, so it breathes and darkens with every pass), Lo-Fi (sample-rate and bit reduction) and Reverse. Stereo, Ping-Pong and Cross modes, tempo sync with dotted and triplet values, L/R offset, modulation, filters in the loop, and feedback past 100 % held by a loop limiter.
- **Reverb** — six types on an eight-line feedback delay network: Room, Chamber, Hall, Plate, Spring (with a dispersive chirp stage) and Shimmer (octave-up feedback). Pre-delay, size, decay as a true RT60, damping, diffusion, modulation, width, and a Freeze that holds the tail without it fading.
- **Ducker** — turns the effects down while the source plays and lets them bloom in the gaps. Can target the delay, the reverb or both, and can be keyed from the sidechain input.
- **Feed filters** — low and high cut on what is sent into the effects; the dry signal is never filtered.
- **Timeline** — a picture of what the settings will do: each echo with its level and side, and the reverb tail, against time or beats.
- **Guide** — explains whichever control is under the pointer, gives tips and a starting point per source and style, and comments live on what the effects are doing.
- **Interface** — each control is shaped by its job: switches for types and modes, large knobs for the controls that define the sound, small ones for fine-tuning, the filters drawn as curves you drag, a note-value grid for synced time, the ducker threshold set against the level it hears, and faders for the effect levels.
- **Workflow** — click-free switching of everything, zero latency, Wet Only for use on sends, A/B, undo/redo, presets. Sessions and presets record their format version, so later versions of Reverie load them as they sounded.

## Building (macOS)

Needs the Xcode Command Line Tools, plus CMake and Ninja:

```sh
pip3 install --user cmake ninja
./build.sh
```

`build.sh` finds or fetches JUCE, runs the offline test suite, and installs the AU and VST3 into `~/Library/Audio/Plug-Ins`. It then runs [pluginval](https://github.com/Tracktion/pluginval) at strictness 10 on the VST3 and AU, and Apple's `auval` on the AU; logs go to the build directory's `validation` folder. Use `./build.sh --no-install` to build and test without installing, or `--quick` to skip validation. The build directory is `~/Library/Caches/Reverie-build`.

After installing, quit your DAW, run `killall -9 AudioComponentRegistrar`, and reopen it. The plugin appears under the manufacturer "Channel Audio".

## Tests

`Tests/Main.cpp` runs audio through the processor offline and checks bypass nulling, echo timing against milliseconds and tempo, the stereo modes, that every delay type stays bounded at 110 % feedback, that each reverb type decays in the time set, freeze, pre-delay, ducking and sidechain keying, routing, the bucket-brigade model (timing, compander level, bandwidth, hiss, sample-rate changes), loading states from older and newer versions, click-free switching, the Guide's starting points and CPU use, then renders screenshots of the interface.

`Tests/spice/check_bbd.py` checks the Analog delay's bucket-brigade model against the same circuit solved by [ngspice](https://ngspice.sourceforge.io): the anti-aliasing and reconstruction filters, the clock ticks between host samples, the hold and its images, aliasing at slow clocks, and the NE570 compander. The model matches the circuit to within 55 dB or better. It takes about a minute and a half and isn't part of `build.sh`; run it with `python3 Tests/spice/check_bbd.py` after changing `BucketBrigade.h`.

Built on [JUCE](https://juce.com), which has its own licence terms (AGPLv3 or commercial).
