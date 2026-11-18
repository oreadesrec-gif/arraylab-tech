// Checks the delay engine on synthetic "music-like" signals with known offsets.
#include "DelayEstimator.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace arraylab;
static const double kPi = 3.14159265358979323846;

static std::vector<float> programme (int len, unsigned seed)
{
    // Coloured noise with slow amplitude swells: no clicks, no clean transients.
    std::mt19937 rng (seed);
    std::normal_distribution<float> g (0.0f, 1.0f);
    std::vector<float> v ((size_t) len);
    float lp = 0.0f;
    for (int i = 0; i < len; ++i)
    {
        lp += 0.15f * (g (rng) - lp);
        v[(size_t) i] = lp * (0.6f + 0.4f * std::sin (i * 0.00013f));
    }
    return v;
}

static float fractionalRead (const std::vector<float>& x, double pos)
{
    // Windowed-sinc read, good enough to synthesise sub-sample delays.
    const int c = (int) std::floor (pos);
    double acc = 0.0;
    for (int k = c - 32; k <= c + 32; ++k)
    {
        if (k < 0 || k >= (int) x.size()) continue;
        const double t = pos - k;
        const double s = std::abs (t) < 1e-9 ? 1.0 : std::sin (kPi * t) / (kPi * t);
        const double w = 0.5 + 0.5 * std::cos (kPi * t / 33.0);
        acc += x[(size_t) k] * s * w;
    }
    return (float) acc;
}

static bool run (const char* name, double delay, int polarity, float noise, double tol)
{
    const int n = 32768, hop = n / 2, len = 48000 * 20;
    const double sr = 48000.0;
    const auto ref = programme (len, 1);
    const auto bleed = programme (len, 2);
    std::vector<float> tgt ((size_t) len);
    for (int i = 0; i < len; ++i)
        tgt[(size_t) i] = (float) polarity * fractionalRead (ref, i - delay) + noise * bleed[(size_t) i];

    Fft fft (n);
    FrameSpectrum fx (n), fy (n);
    DelayEstimator est;
    est.reset (n);
    for (int p = 0; p + n <= len; p += hop)
    {
        fx.compute (fft, ref.data() + p);
        fy.compute (fft, tgt.data() + p);
        est.accumulate (fx, fy);
    }
    std::vector<std::complex<float>> scratch;
    const auto r = est.estimate (fft, sr, scratch);
    const bool ok = r.valid && std::abs (r.delaySamples - delay) <= tol && r.polarity == polarity;
    std::printf ("%-34s expected %9.3f  got %9.3f  pol %+d  conf %.2f  frames %d  %s\n",
                 name, delay, r.delaySamples, r.polarity, r.confidence, r.frames, ok ? "ok" : "FAIL");
    return ok;
}

int main()
{
    bool ok = true;
    ok &= run ("identical", 0.0, 1, 0.0f, 0.02);
    ok &= run ("spot 3 m ahead of main", -420.0, 1, 0.0f, 0.02);
    ok &= run ("sub-sample", 137.37, 1, 0.0f, 0.05);
    ok &= run ("inverted polarity", 512.25, -1, 0.0f, 0.05);
    ok &= run ("far ambience (30 m)", 4198.6, 1, 0.0f, 0.05);
    ok &= run ("heavy bleed, 0 dB uncorrelated", 250.5, 1, 1.0f, 0.1);
    ok &= run ("bleed 12 dB above the source", -88.8, -1, 4.0f, 0.2);
    return ok ? 0 : 1;
}
