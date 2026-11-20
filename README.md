# ArrayLab

Time and phase alignment of spot, outrigger and ambience microphones against a
main pair or Decca tree, measured on the music itself. VST3, aimed at Pyramix.

## Status: 0.8

One instance per track, one shared window with four views.

- **Mix**: the working view, one line per microphone like the strips of a
  console: what there is to gain, the delay (with one-sample nudges, also on
  the left and right arrow keys), polarity, a phase turn in degrees, the
  microphone's level in the mix, how well its delay holds through the passage,
  solo with the main array, lock, and an on/off switch. Above the strips, one
  setting makes the main array lead every lined-up microphone by a few
  milliseconds. A, B and C hold three complete sets of settings to compare.

- **Stage**: top view of the session. Main arrays (AB, ORTF, NOS, XY, Blumlein,
  Decca Tree or custom spacing, angle and pattern) and every microphone, with
  its polar pattern, an arrow for where it points, and its own colour.
- **Analyze**: measures on the music itself, only while you ask for it. The
  result is kept, saved with the session, and never redone until you press
  Reset analysis. Each microphone is measured against main L, C, R and their
  sum; frames where its own instrument is not playing are left out; a spot is
  only searched where it can physically be; and a result whose peak does not
  stand clear of the floor is shown but not applied.
- **Its own instrument**: a spot is lined up on the instrument it is there for.
  Only the moments and frequencies where the spot is louder than usual against
  the main array are analysed, so spill from the other players is left out.
- **Late microphones**: one that hears the source after the main array is lined
  up by delaying every other track by what it needs. A polarity is only turned
  over on clear evidence, and a microphone whose delay wanders through the
  passage is left as recorded.
- **The yardstick**: for every candidate delay, how many dB fuller the sum of
  the microphone and the main array gets, measured on the music between 63 Hz
  and 4 kHz. By default the delay used is the one that gains most, and a
  microphone with less than 0.2 dB to gain is left as recorded. The figure is
  shown for each microphone and each candidate.
- **Alignment**: delay to a fraction of a sample, polarity, and phase per
  1/12 octave, each with its own bypass. You choose the reference (nearest
  main mic, L, C, R, their average or their sum), how much of the phase is
  corrected and over which frequency range.
- **Detail**: for the selected microphone, the correlation with its candidate
  delays (click one to use it instead of the automatic choice) and the phase by
  frequency before and after correction, with how far each band is trusted.
- **Check**: while the music plays, the stereo bus as recorded against the bus
  with the corrections, by frequency, with a plain verdict. "Refine phase on
  the sum" nudges each phase-corrected microphone towards everything else;
  "Match level" makes the comparison with Bypass all equally loud.
- **Pan**: an ordinary pan slider for a mono mic on a stereo output, preset from
  the microphone's direction on the plan, with a bypass. On a mono output the
  suggested position is shown for the mixer's own panner.
- **Precision**: Live (no latency) or Full resolution (a longer phase filter;
  every instance reports the same latency to the host).
- **Stereo spots** are pairs with their own spacing and angle, moved as one
  block and given one phase correction; two mono tracks can be linked as a pair
  to the same effect. A pair can be panned (position and width). Phase can also
  be turned by hand, to the degree. A measurement can be locked. **Groups**: up to eight, each with its own main array as reference.

Not yet tried in Pyramix by the author: host behaviour (mono-to-stereo inserts,
timeline reporting, latency changes) still needs confirming there.

## Install (Windows)

Download [`Update-ArrayLab.bat`](https://github.com/oreadesrec-gif/arraylab-tech/releases/download/latest/Update-ArrayLab.bat)
once and keep it. Close Pyramix, double-click it, accept the administrator
prompt: it fetches the newest build and puts `ArrayLab.vst3` in
`C:\Program Files\Common Files\VST3\`. Run it again for each new version.

By hand: download `ArrayLab-VST3-Windows.zip` from the
[latest build](https://github.com/oreadesrec-gif/arraylab-tech/releases/tag/latest),
unzip it into that folder, and rescan plugins in Pyramix.

## Build locally

    cmake -S . -B build -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release

JUCE 8 is fetched automatically.

## Layout

- `src/Fft.*`, `src/DelayEstimator.*` - the engine, plain C++ without JUCE
- `src/Scene.h`, `src/PhaseFir.*`, `src/Fractional.h` - plan geometry, panning, the phase filter and sub-sample delay, also plain C++
- `src/Hub.*` - the link between instances, the analysis thread and what gets applied
- `src/PluginProcessor.*` - capture, phase filter, delays and gains
- `src/PluginEditor.*`, `src/StageView.*`, `src/Inspector.*`, `src/DetailView.*`, `src/CheckView.*` - the window
- `tests/` - engine test on synthetic signals, and a headless multi-instance test
