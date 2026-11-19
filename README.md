# ArrayLab

Time and phase alignment of spot, outrigger and ambience microphones against a
main pair or Decca tree, measured on the music itself. VST3, aimed at Pyramix.

## Status: 0.3

One instance per track, one shared window.

- **Stage**: top view of the session. Main arrays (AB, ORTF, NOS, XY, Blumlein,
  Decca Tree or custom spacing, angle and pattern) and every microphone, with
  its polar pattern and aim. Click a microphone to edit it, drag to move it.
- **Analyze**: measures on the music itself, only while you ask for it. The
  result is kept, saved with the session, and never redone until you press
  Reset analysis.
- **Alignment**: delay, polarity and multiband phase per microphone, each with
  its own bypass, plus a global bypass. A mono spot on a stereo output gets its
  own delay towards the left and right main microphones, and a level pan
  derived from where it sits on the plan.
- **Stereo spots**: a spot on a stereo track is a pair with its own spacing
  and angle. Both capsules are measured and the pair is moved as one block,
  so its own image is kept.
- **Groups**: up to eight, each with its own main array as reference, or
  following the Main group when it has none.
- **Align**: the same data as a table, with one-click bypass switches
  (delay, polarity, phase, pan).
- **Check**: while the music plays, the sum of the microphones as recorded
  against the sum with the corrections, by frequency, with a plain verdict:
  do the corrections make the sum fuller or thinner? For everything, or for
  the main array plus one microphone.
- **Colours**: each microphone can take its own colour.

Not yet tried in Pyramix by the author: host behaviour (mono-to-stereo inserts,
timeline reporting) still needs confirming there.

## Install (Windows)

Download `ArrayLab-VST3-Windows` from the latest run under the Actions
tab, unzip, and copy `ArrayLab.vst3` to
`C:\Program Files\Common Files\VST3\`. Rescan plugins in Pyramix.

## Build locally

    cmake -S . -B build -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release

JUCE 8 is fetched automatically.

## Layout

- `src/Fft.*`, `src/DelayEstimator.*` - the engine, plain C++ without JUCE
- `src/Scene.h`, `src/PhaseFir.*` - plan geometry, panning and the phase filter, also plain C++
- `src/Hub.*` - the link between instances, the analysis thread and what gets applied
- `src/PluginProcessor.*` - capture, phase filter, delays and gains
- `src/PluginEditor.*`, `src/StageView.*`, `src/Inspector.*` - the window
- `tests/` - engine test on synthetic signals, and a headless multi-instance test
