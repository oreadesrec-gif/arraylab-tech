# ArrayLab

Time and phase alignment of spot, outrigger and ambience microphones against a
main pair or Decca tree, measured on the music itself. VST3, aimed at Pyramix.

## Status: Probe 0.1

The first build is a probe. It answers three questions about the host before
the real plugin is built on top of them:

1. Do instances on different tracks see each other? (Pyramix has no side-chain.)
2. Are they sample-accurate with each other on the host timeline?
3. Can a mono track carry a mono-in / stereo-out plugin?

It already contains the delay engine: insert it on several tracks, press play,
and it shows each microphone's delay (samples, ms, metres), polarity and a
confidence figure against the reference microphone.

## Install (Windows)

Download `ArrayLab-Probe-VST3-Windows` from the latest run under the Actions
tab, unzip, and copy `ArrayLab Probe.vst3` to
`C:\Program Files\Common Files\VST3\`. Rescan plugins in Pyramix.

## Build locally

    cmake -S . -B build -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release

JUCE 8 is fetched automatically.

## Layout

- `src/Fft.*`, `src/DelayEstimator.*` - the engine, plain C++ without JUCE
- `src/Hub.*` - the link between instances and the analysis thread
- `src/PluginProcessor.*`, `src/PluginEditor.*` - the plugin and its window
- `tests/` - engine test on synthetic signals, and a headless multi-instance test
