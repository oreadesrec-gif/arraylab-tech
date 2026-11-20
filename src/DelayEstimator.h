#pragma once
#include "Fft.h"
#include "Scene.h"
#include <complex>
#include <vector>

namespace arraylab
{
/** One candidate delay: where the correlation peaks, and how high (negative = inverted polarity). */
struct Peak
{
    double lag = 0.0;
    float value = 0.0f;
    float gainDb = 0.0f; // how much fuller the sum with the main array gets if this delay is used
};

/** Result of comparing one microphone against a reference. */
struct DelayResult
{
    bool   valid = false;
    double delaySamples = 0.0; // > 0: this mic hears the source later than the reference
    int    polarity = 1;       // -1: inverted relative to the reference
    double confidence = 0.0;   // 0..1, share of the spectrum that agrees on this delay
    int    frames = 0;         // analysis frames that went into it
    bool   reliable = false;   // the peak stands clear of the floor: safe to apply
    double snr = 0.0;          // peak height over the correlation floor
    int    numPeaks = 0;       // candidates, strongest first; peaks[0] is the one chosen
    Peak   peaks[kMaxPeaks];
};

/** Delays outside this range are not considered (in samples). */
struct SearchRange
{
    double minLag = -1.0e9, maxLag = 1.0e9;
};

/** The correlation over the search range, thinned for display. */
struct CorrelationCurve
{
    double minLag = 0.0, maxLag = 0.0;
    std::vector<float> points;
};

/** Phase of the microphone against its reference once delay and polarity are taken out,
    per 1/12 octave, and how far each value can be trusted (0..1). */
struct PhaseCurve
{
    bool valid = false;
    float phase[kFineBands] {};
    float trust[kFineBands] {};
};

/** Extra delay of the lows, mids and highs around the overall delay, in ms. */
struct ZoneDelays
{
    bool valid = false;
    double lowMs = 0.0, midMs = 0.0, highMs = 0.0;
};

ZoneDelays zoneDelays (const PhaseCurve&);

/** How far a phase value can be trusted (0..1), from the coherence it was read at
    and the number of independent averages behind it. */
double phaseTrust (double coherence, double averages);

/** Windowed spectrum of one analysis frame. */
class FrameSpectrum
{
public:
    explicit FrameSpectrum (int frameSize);
    /** `samples` must hold frameSize values. */
    void compute (const Fft& fft, const float* samples);
    /** Becomes the spectrum of the sum of several signals framed at the same position. */
    void setSum (const FrameSpectrum* const* parts, int numParts);
    const std::vector<std::complex<float>>& bins() const noexcept { return data; }
    double energy() const noexcept { return frameEnergy; }

private:
    std::vector<std::complex<float>> data;
    std::vector<float> window;
    double frameEnergy = 0.0;
};

/** Accumulates the cross-spectrum between a reference and a target over as
    many frames of programme material as you feed it. From it come the delay
    (coherence-weighted cross-correlation), the candidate peaks, and the phase
    left over in each 1/12 octave. Longer accumulation makes every figure
    steadier; it never needs a click or a test signal. */
class DelayEstimator
{
public:
    DelayEstimator() = default;
    void reset (int frameSize);

    /** Returns false when the frame was left out: silence, or the target far
        below its recent level (its instrument is not playing, only spill).

        With `ownSourceOnly`, only the moments and frequencies where the target is
        louder than usual against the reference are kept. A spot is close to its
        instrument, so that is where its instrument is what it hears; spill from the
        other players, which the reference hears better, is left out. Without it the
        loudest thing the two microphones share wins, whoever is playing. */
    bool accumulate (const FrameSpectrum& reference, const FrameSpectrum& target, bool ownSourceOnly = true);

    /** `weighted` = false keeps to the plain first pass, for comparison. */
    DelayResult estimate (const Fft& fft, double sampleRate, std::vector<std::complex<float>>& scratch,
                          SearchRange range = {}, CorrelationCurve* curve = nullptr, bool weighted = true) const;

    /** Sharpens a delay to a fraction of a sample from the slope of the phase. */
    double refineDelay (double sampleRate, double delaySamples, int polarity) const;

    PhaseCurve phaseCurve (double sampleRate, double delaySamples, int polarity) const;

    /** What lining up is for. If this microphone is moved by -lag, given this polarity and
        turned by `rotation` radians, how much fuller does its sum with the reference get?
        In dB, averaged over the 1/12 octaves between lowHz and highHz so that no register
        outweighs another. +3 dB: the two add up perfectly. 0 dB: they have nothing in common.
        Below 0: they cancel. `limit` = true answers for a perfect phase correction in every band.
        `level` is the microphone's gain in the mix against the reference (1 = as recorded). */
    double sumGain (double sampleRate, double lag, int polarity, double rotation = 0.0, bool limit = false,
                    double lowHz = 63.0, double highHz = 4000.0, double level = 1.0) const;

    /** The delays most likely to make that sum fullest: peaks of the correlation weighted exactly as
        sumGain weighs the spectrum. Their `value` only carries the polarity; score them with sumGain. */
    int gainCandidates (const Fft& fft, double sampleRate, std::vector<std::complex<float>>& scratch, SearchRange range,
                        Peak* found, int maxFound, double lowHz = 63.0, double highHz = 4000.0, double level = 1.0) const;

    /** Direction of the leftover phase, weighted the same way: minus its angle is the best single rotation. */
    std::complex<double> sumVector (double sampleRate, double lag, int polarity, double lowHz = 63.0, double highHz = 4000.0, double level = 1.0) const;

    int frames() const noexcept { return numFrames; }
    int skipped() const noexcept { return numSkipped; }

private:
    void transfer (double sampleRate, double delaySamples, int polarity, std::complex<double>* gamma, double* averages) const;
    bool band (int b, double sampleRate, double delaySamples, std::complex<double>& cross, double& xx, double& yy, int& bins) const;

    int n = 0, numFrames = 0, numSkipped = 0;
    double recentPeak = 0.0;
    std::vector<std::complex<float>> sxy;
    std::vector<float> sxx, syy;
    std::vector<float> allXx, allYy; // everything heard so far, per group of bins: the usual balance
    double kept = 0.0, offered = 0.0; // share of the material that passed the own-source test
};
}
