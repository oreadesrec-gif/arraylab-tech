#pragma once
#include "Scene.h"

namespace arraylab
{
constexpr int kSincTaps = 32;
constexpr int kSincCentre = kSincTaps / 2 - 1; // the kernel itself delays by this many samples, plus the fraction

/** Kernel for a delay of kSincCentre + fraction samples (fraction in 0..1):
    a sinc under a Kaiser window, flat to within a fraction of a dB up to 18 kHz at 48 kHz. */
inline void makeSincKernel (double fraction, float* h)
{
    const auto bessel = [] (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 24; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
        return sum;
    };
    const double beta = 6.5, half = kSincTaps * 0.5;
    double total = 0.0;
    for (int k = 0; k < kSincTaps; ++k)
    {
        const double t = k - kSincCentre - fraction;
        const double sinc = std::abs (t) < 1e-9 ? 1.0 : std::sin (kPi * t) / (kPi * t);
        const double u = std::clamp (t / half, -1.0, 1.0);
        const double v = sinc * bessel (beta * std::sqrt (1.0 - u * u)) / bessel (beta);
        h[k] = (float) v;
        total += v;
    }
    for (int k = 0; k < kSincTaps; ++k)
        h[k] = (float) (h[k] / total);
}
}
