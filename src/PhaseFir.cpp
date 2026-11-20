#include "PhaseFir.h"
#include "Fft.h"

namespace arraylab
{
void correctionCurve (const float* phase, const float* trust, const float* trim,
                      double amount, double lowHz, double highHz, float* correction)
{
    // The range fades in and out over a third of an octave rather than cutting off.
    const double lowPos = bandPosition (lowHz), highPos = bandPosition (highHz);
    for (int b = 0; b < kFineBands; ++b)
    {
        double range = 1.0;
        if (lowHz > bandFrequency (0))               range *= std::clamp ((b - lowPos) / 4.0 + 0.5, 0.0, 1.0);
        if (highHz < bandFrequency (kFineBands - 1)) range *= std::clamp ((highPos - b) / 4.0 + 0.5, 0.0, 1.0);
        range = range * range * (3.0 - 2.0 * range);

        // A real acoustic phase moves gently with frequency. Average each band with its
        // neighbours (a third of an octave) as vectors: a jagged stretch cancels itself out.
        std::complex<double> sum {};
        double weights = 0.0;
        for (int i = -2; i <= 2; ++i)
        {
            const int nb = b + i;
            if (nb < 0 || nb >= kFineBands) continue;
            const double w = 3.0 - std::abs (i);
            sum += std::polar (w * trust[nb], (double) phase[nb]);
            weights += w;
        }
        const double smoothTrust = std::abs (sum) / weights, smoothPhase = smoothTrust > 1.0e-9 ? std::arg (sum) : 0.0;
        correction[b] = (float) ((smoothPhase * smoothTrust + (trim != nullptr ? trim[b] : 0.0f)) * amount * range);
    }
}

std::complex<double> correctionResponse (const float* correction, double hz)
{
    const double pos = bandPosition (hz);
    if (pos <= 0.0) // ease to nothing towards DC
        return std::polar (1.0, -(double) correction[0] * std::max (hz, 0.0) / bandFrequency (0));
    if (pos >= kFineBands - 1)
        return std::polar (1.0, -(double) correction[kFineBands - 1]);

    // Blend the two neighbouring bands as vectors, so a wrap from +pi to -pi stays harmless.
    const int b = (int) pos;
    const double fr = pos - b;
    const auto v = std::polar (1.0 - fr, -(double) correction[b]) + std::polar (fr, -(double) correction[b + 1]);
    const double mag = std::abs (v);
    return mag > 1e-9 ? v / mag : std::complex<double> (1.0, 0.0);
}

void designPhaseFir (const float* correction, double sampleRate, int length, std::vector<float>& taps)
{
    Fft fft (length);
    std::vector<std::complex<float>> h ((size_t) length);

    h[0] = { 1.0f, 0.0f };
    h[(size_t) length / 2] = { 1.0f, 0.0f };
    for (int k = 1; k < length / 2; ++k)
    {
        h[(size_t) k] = std::complex<float> (correctionResponse (correction, k * sampleRate / length));
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
