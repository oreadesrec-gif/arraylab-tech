#pragma once
#include "Fft.h"
#include "Scene.h"
#include <complex>
#include <vector>

namespace arraylab
{
/** Result of comparing one microphone against a reference. */
struct DelayResult
{
    bool   valid = false;
    double delaySamples = 0.0; // > 0: this mic hears the source later than the reference
    int    polarity = 1;       // -1: inverted relative to the reference
    double confidence = 0.0;   // 0..1, share of the spectrum that agrees on this delay
    int    frames = 0;         // number of analysis frames accumulated
};

/** Windowed spectrum of one analysis frame. */
class FrameSpectrum
{
public:
    explicit FrameSpectrum (int frameSize);
    /** `samples` must hold frameSize values. */
    void compute (const Fft& fft, const float* samples);
    const std::vector<std::complex<float>>& bins() const noexcept { return data; }
    double energy() const noexcept { return frameEnergy; }

private:
    std::vector<std::complex<float>> data;
    std::vector<float> window;
    double frameEnergy = 0.0;
};

/** Accumulates the cross-spectrum between a reference and a target over as
    many frames of programme material as you feed it, then reads the delay off
    the coherence-weighted (SCOT) cross-correlation. Longer accumulation makes
    the estimate steadier; it never needs a click or a test signal. */
class DelayEstimator
{
public:
    DelayEstimator() = default;
    void reset (int frameSize);
    void accumulate (const FrameSpectrum& reference, const FrameSpectrum& target);
    DelayResult estimate (const Fft& fft, double sampleRate, std::vector<std::complex<float>>& scratch) const;

    /** Phase left over in each band once `delaySamples` and `polarity` are
        compensated. Each value's angle is the residual phase of the target
        against the reference; its length (0..1) says how far to trust it. */
    void phaseBands (double sampleRate, double delaySamples, int polarity, std::complex<float>* bands) const;

    int frames() const noexcept { return numFrames; }

private:
    int n = 0, numFrames = 0;
    std::vector<std::complex<float>> sxy;
    std::vector<float> sxx, syy;
};
}
