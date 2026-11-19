#pragma once
#include "Scene.h"
#include <complex>
#include <vector>

namespace arraylab
{
/** Builds the FIR that cancels the residual phase measured in each band.
    `length` must be a power of two; the filter's latency is length / 2.
    Bands with weak coherence are corrected less, down to not at all. */
void designPhaseFir (const std::complex<float>* bands, double sampleRate, int length, std::vector<float>& taps);
}
