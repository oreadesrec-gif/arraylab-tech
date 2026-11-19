#include "PhaseFir.h"
#include "Fft.h"

namespace arraylab
{
void designPhaseFir (const std::complex<float>* bands, double sampleRate, int length, std::vector<float>& taps)
{
    Fft fft (length);
    std::vector<std::complex<float>> h ((size_t) length);
    const double step = std::log (kBandHigh / kBandLow) / (kNumBands - 1);

    h[0] = { 1.0f, 0.0f };
    h[(size_t) length / 2] = { 1.0f, 0.0f };
    for (int k = 1; k < length / 2; ++k)
    {
        const double f = k * sampleRate / length;
        const double pos = std::clamp (std::log (std::max (f, 1.0) / kBandLow) / step, 0.0, (double) kNumBands - 1.0);
        const int b = std::min ((int) pos, kNumBands - 2);
        const double fr = pos - b;
        const auto c = std::complex<double> (bands[b]) * (1.0 - fr) + std::complex<double> (bands[b + 1]) * fr;

        // Trust: nothing below 0.2 coherence, everything above 0.6.
        const double t = std::clamp ((std::abs (c) - 0.2) / 0.4, 0.0, 1.0);
        double phi = std::arg (c) * t * t * (3.0 - 2.0 * t);
        if (f < kBandLow) phi *= f / kBandLow; // ease to zero towards DC

        h[(size_t) k] = { (float) std::cos (-phi), (float) std::sin (-phi) };
        h[(size_t) (length - k)] = std::conj (h[(size_t) k]);
    }

    fft.inverse (h.data());

    taps.resize ((size_t) length);
    for (int i = 0; i < length; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * i / length);
        taps[(size_t) i] = (float) (h[(size_t) ((i + length / 2) % length)].real() * w);
    }
}
}
