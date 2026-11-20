#pragma once
#include "Scene.h"
#include <complex>
#include <vector>

namespace arraylab
{
/** The phase to take out of each 1/12 octave: the measured phase weighted by its
    trust, plus a trim, scaled by the user's amount and kept to their frequency range. */
void correctionCurve (const float* phase, const float* trust, const float* trim,
                      double amount, double lowHz, double highHz, float* correction);

/** Response of the correction at `hz` (unit magnitude). */
std::complex<double> correctionResponse (const float* correction, double hz);

/** Linear-phase-latency FIR that applies the correction. `length` must be a
    power of two; the filter's latency is length / 2. */
void designPhaseFir (const float* correction, double sampleRate, int length, std::vector<float>& taps);
}
