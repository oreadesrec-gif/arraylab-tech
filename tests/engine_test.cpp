// Checks the delay engine on synthetic "music-like" signals with known offsets.
#include "DelayEstimator.h"
#include "PhaseFir.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace arraylab;

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

// A phase-shifted copy of the reference: the multiband filter should bring it back in line.
static bool phaseCase()
{
    const int n = 32768, hop = n / 2, len = 48000 * 20, firLen = 1024;
    const double sr = 48000.0;
    const auto ref = programme (len, 5);
    std::vector<float> tgt ((size_t) len, 0.0f);
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    for (int i = 0; i < len; ++i) // two all-pass stages: the phase turns a full circle across the spectrum
    {
        const float x = ref[(size_t) i];
        const float a = -0.6f * x + x1 + 0.6f * y1;   x1 = x; y1 = a;
        const float b = -0.6f * a + x2 + 0.6f * y2;   x2 = a; y2 = b;
        tgt[(size_t) i] = b;
    }

    Fft fft (n);
    FrameSpectrum fx (n), fy (n);
    DelayEstimator est;
    est.reset (n);
    for (int p = 0; p + n <= len; p += hop) { fx.compute (fft, ref.data() + p); fy.compute (fft, tgt.data() + p); est.accumulate (fx, fy); }
    std::vector<std::complex<float>> scratch;
    const auto r = est.estimate (fft, sr, scratch);
    const int d = (int) std::lround (r.delaySamples);

    std::complex<float> bands[kNumBands];
    est.phaseBands (sr, (double) d, r.polarity, bands);
    std::vector<float> taps;
    designPhaseFir (bands, sr, firLen, taps);

    const auto correlation = [&] (bool filtered)
    {
        double xy = 0, xx = 0, yy = 0;
        for (int i = 48000; i < 48000 * 6; ++i)
        {
            double y = 0.0;
            if (filtered) for (int k = 0; k < firLen; ++k) y += taps[(size_t) k] * tgt[(size_t) (i + d + firLen / 2 - k)];
            else          y = tgt[(size_t) (i + d)];
            y *= r.polarity;
            xy += y * ref[(size_t) i]; xx += (double) ref[(size_t) i] * ref[(size_t) i]; yy += y * y;
        }
        return xy / std::sqrt (xx * yy);
    };
    const double before = correlation (false), after = correlation (true);
    const bool ok = after > 0.97 && after > before + 0.05;
    std::printf ("%-34s delay-only match %.3f  with phase filter %.3f  %s\n", "multiband phase", before, after, ok ? "ok" : "FAIL");
    return ok;
}

// Geometry: a source placed from its two distances lands where it was, and pans to its side.
static bool geometryCase()
{
    ArraySettings arr;
    applyPreset (arr, ArrayType::ortf);
    const auto geo = arrayGeometry (arr);
    const Vec2 src { -2.0, 5.0 };
    const auto p = locate (geo, distance (src, geo.pos[0]), distance (src, geo.pos[2]));
    float gl = 0, gr = 0;
    panGains (geo, arr.pattern, src, distance (src, geo.pos[0]), distance (src, geo.pos[2]), 0, 1.0, gl, gr);
    const bool ok = std::abs (p.x - src.x) < 1e-6 && std::abs (p.y - src.y) < 1e-6 && gl > gr && std::abs (gl * gl + gr * gr - 1.0f) < 1e-4f;
    std::printf ("%-34s located (%.2f, %.2f)  gains L %.2f R %.2f  %s\n", "plan geometry and pan", p.x, p.y, gl, gr, ok ? "ok" : "FAIL");
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
    ok &= phaseCase();
    ok &= geometryCase();
    return ok ? 0 : 1;
}
