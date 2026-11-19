#include "DelayEstimator.h"
#include <algorithm>
#include <cmath>

namespace arraylab
{
FrameSpectrum::FrameSpectrum (int frameSize) : data ((size_t) frameSize), window ((size_t) frameSize)
{
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < frameSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / frameSize));
}

void FrameSpectrum::compute (const Fft& fft, const float* samples)
{
    double e = 0.0;
    for (size_t i = 0; i < data.size(); ++i)
    {
        const float v = samples[i] * window[i];
        e += (double) v * v;
        data[i] = { v, 0.0f };
    }
    frameEnergy = e / (double) data.size();
    fft.forward (data.data());
}

void DelayEstimator::reset (int frameSize)
{
    n = frameSize;
    numFrames = 0;
    sxy.clear(); sxx.clear(); syy.clear(); // reallocated on first use
}

void DelayEstimator::accumulate (const FrameSpectrum& ref, const FrameSpectrum& tgt)
{
    // Skip silence: it only adds noise to the average.
    if (n == 0 || ref.energy() < 1e-10 || tgt.energy() < 1e-10)
        return;

    if (sxy.empty())
    {
        sxy.assign ((size_t) n / 2 + 1, {});
        sxx.assign ((size_t) n / 2 + 1, 0.0f);
        syy.assign ((size_t) n / 2 + 1, 0.0f);
    }

    const auto& x = ref.bins();
    const auto& y = tgt.bins();
    for (size_t k = 0; k < sxy.size(); ++k)
    {
        sxy[k] += y[k] * std::conj (x[k]);
        sxx[k] += std::norm (x[k]);
        syy[k] += std::norm (y[k]);
    }
    ++numFrames;
}

void DelayEstimator::phaseBands (double sampleRate, double delaySamples, int polarity, std::complex<float>* bands) const
{
    std::complex<double> sum[kNumBands] {};
    double weight[kNumBands] {};
    if (numFrames > 0 && ! sxy.empty())
    {
        const double step = std::log (kBandHigh / kBandLow) / (kNumBands - 1);
        const int lo = std::max (1, (int) std::ceil (kBandLow * n / sampleRate));
        const int hi = std::min (n / 2 - 1, (int) std::floor (kBandHigh * n / sampleRate));
        for (int k = lo; k <= hi; ++k)
        {
            const double den = std::sqrt ((double) sxx[(size_t) k] * (double) syy[(size_t) k]);
            if (den <= 0.0) continue;
            const double turn = 2.0 * kPi * k * delaySamples / n;
            const auto v = std::complex<double> (sxy[(size_t) k]) / den
                           * std::complex<double> (std::cos (turn), std::sin (turn)) * (double) polarity;
            const double pos = std::clamp (std::log (k * sampleRate / n / kBandLow) / step, 0.0, (double) kNumBands - 1.0);
            const int b = std::min ((int) pos, kNumBands - 2);
            const double f = pos - b;
            sum[b] += v * (1.0 - f);      weight[b] += 1.0 - f;
            sum[b + 1] += v * f;          weight[b + 1] += f;
        }
    }
    for (int b = 0; b < kNumBands; ++b)
        bands[b] = weight[b] > 0.0 ? std::complex<float> (sum[b] / weight[b]) : std::complex<float> {};
}

DelayResult DelayEstimator::estimate (const Fft& fft, double sampleRate, std::vector<std::complex<float>>& c) const
{
    DelayResult r;
    r.frames = numFrames;
    if (numFrames == 0 || n == 0 || sxy.empty())
        return r;

    c.assign ((size_t) n, {});
    const int lo = std::max (1, (int) std::ceil (40.0 * n / sampleRate));
    const int hi = std::min (n / 2 - 1, (int) std::floor (16000.0 * n / sampleRate));
    if (hi <= lo)
        return r;

    for (int k = lo; k <= hi; ++k)
    {
        const double den = std::sqrt ((double) sxx[(size_t) k] * (double) syy[(size_t) k]);
        if (den <= 0.0) continue;
        const auto w = std::complex<double> (sxy[(size_t) k]) / den; // complex coherence, |w| <= 1
        c[(size_t) k] = { (float) w.real(), (float) w.imag() };
        c[(size_t) (n - k)] = std::conj (c[(size_t) k]);
    }

    fft.inverse (c.data());

    int peak = 0;
    float peakAbs = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = std::abs (c[(size_t) i].real());
        if (a > peakAbs) { peakAbs = a; peak = i; }
    }
    if (peakAbs <= 0.0f)
        return r;

    // Parabolic interpolation around the peak for sub-sample precision.
    const auto at = [&] (int i) { return (double) c[(size_t) ((i + n) % n)].real(); };
    const double sign = at (peak) < 0.0 ? -1.0 : 1.0;
    const double a = sign * at (peak - 1), b = sign * at (peak), d = sign * at (peak + 1);
    const double den = a - 2.0 * b + d;
    const double frac = std::abs (den) > 1e-20 ? std::clamp (0.5 * (a - d) / den, -0.5, 0.5) : 0.0;

    const int lag = peak > n / 2 ? peak - n : peak;
    r.valid = true;
    r.delaySamples = lag + frac;
    r.polarity = sign < 0.0 ? -1 : 1;
    r.confidence = std::clamp ((double) peakAbs * n / (2.0 * (hi - lo + 1)), 0.0, 1.0);
    return r;
}
}
