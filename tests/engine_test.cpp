// Checks the analysis engine on synthetic "music-like" signals with known answers.
#include "DelayEstimator.h"
#include "Fractional.h"
#include "PhaseFir.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <random>
#include <vector>

using namespace arraylab;

static const int kFrame = 32768, kHop = kFrame / 2;
static const double kRate = 48000.0;

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

static std::vector<float> delayed (const std::vector<float>& x, double delay, float gain = 1.0f)
{
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        y[i] = gain * fractionalRead (x, (double) i - delay);
    return y;
}

static void feed (DelayEstimator& est, const Fft& fft, const std::vector<float>& ref, const std::vector<float>& tgt)
{
    FrameSpectrum fx (kFrame), fy (kFrame);
    est.reset (kFrame);
    for (int p = 0; p + kFrame <= (int) ref.size(); p += kHop)
    {
        fx.compute (fft, ref.data() + p);
        fy.compute (fft, tgt.data() + p);
        est.accumulate (fx, fy);
    }
}

static bool report (const char* name, bool ok, const char* format, ...)
{
    char text[256];
    va_list args;
    va_start (args, format);
    std::vsnprintf (text, sizeof (text), format, args);
    va_end (args);
    std::printf ("%-36s %-78s %s\n", name, text, ok ? "ok" : "FAIL");
    return ok;
}

static bool delayCase (const Fft& fft, const char* name, double delay, int polarity, float noise, double tol)
{
    const int len = 48000 * 20;
    const auto ref = programme (len, 1);
    const auto spill = programme (len, 2);
    auto tgt = delayed (ref, delay, (float) polarity);
    for (int i = 0; i < len; ++i) tgt[(size_t) i] += noise * spill[(size_t) i];

    DelayEstimator est;
    feed (est, fft, ref, tgt);
    std::vector<std::complex<float>> scratch;
    const auto r = est.estimate (fft, kRate, scratch);
    const double fine = est.refineDelay (kRate, r.delaySamples, r.polarity);
    return report (name, r.valid && r.reliable && std::abs (fine - delay) <= tol && r.polarity == polarity,
                   "want %9.3f  peak %9.3f  refined %9.3f  pol %+d  snr %5.0f", delay, r.delaySamples, fine, r.polarity, r.snr);
}

// Two arrivals of the same source: both must show up as candidates, strongest first.
static bool peaksCase (const Fft& fft)
{
    const int len = 48000 * 20;
    const auto ref = programme (len, 3);
    auto tgt = delayed (ref, -300.0);
    const auto echo = delayed (ref, -900.0, 0.6f);
    for (int i = 0; i < len; ++i) tgt[(size_t) i] += echo[(size_t) i];

    DelayEstimator est;
    feed (est, fft, ref, tgt);
    std::vector<std::complex<float>> scratch;
    CorrelationCurve curve;
    const auto r = est.estimate (fft, kRate, scratch, { -4000.0, 200.0 }, &curve);
    const bool ok = r.numPeaks >= 2 && std::abs (r.peaks[0].lag + 300.0) < 0.5 && std::abs (r.peaks[1].lag + 900.0) < 0.5
                    && std::abs (r.peaks[0].value) > std::abs (r.peaks[1].value) && ! curve.points.empty();
    return report ("two arrivals, two candidates", ok, "peaks %.1f (%.2f) and %.1f (%.2f), %d found, curve of %d points",
                   r.peaks[0].lag, r.peaks[0].value, r.peaks[1].lag, r.peaks[1].value, r.numPeaks, (int) curve.points.size());
}

// A louder neighbour reaches the main mic first; without a range it wins, with the spot's range it cannot.
static bool rangeCase (const Fft& fft)
{
    const int len = 48000 * 20;
    const auto own = programme (len, 4), neighbour = programme (len, 5);
    std::vector<float> ref ((size_t) len), tgt ((size_t) len);
    const auto ownAtMain = delayed (own, 400.0), neighbourAtSpot = delayed (neighbour, 2000.0, 1.6f);
    for (int i = 0; i < len; ++i)
    {
        ref[(size_t) i] = ownAtMain[(size_t) i] + 2.0f * neighbour[(size_t) i]; // main hears both
        tgt[(size_t) i] = own[(size_t) i] + neighbourAtSpot[(size_t) i];        // spot hears its own first, the neighbour late and loud
    }
    DelayEstimator est;
    feed (est, fft, ref, tgt);
    std::vector<std::complex<float>> scratch;
    const auto open = est.estimate (fft, kRate, scratch);
    const auto spot = est.estimate (fft, kRate, scratch, { -8400.0, 144.0 });
    return report ("loud neighbour kept out by the range", std::abs (open.delaySamples - 2000.0) < 0.5 && std::abs (spot.delaySamples + 400.0) < 0.5 && spot.reliable,
                   "unrestricted picks %.1f, a spot's range picks %.1f (want -400)", open.delaySamples, spot.delaySamples);
}

// Nothing in common: the engine must say so rather than hand back a delay.
static bool unrelatedCase (const Fft& fft)
{
    const int len = 48000 * 20;
    DelayEstimator est;
    feed (est, fft, programme (len, 6), programme (len, 7));
    std::vector<std::complex<float>> scratch;
    const auto r = est.estimate (fft, kRate, scratch, { -8400.0, 144.0 });
    return report ("unrelated signals are not trusted", ! r.reliable, "snr %.1f, reliable %d", r.snr, (int) r.reliable);
}

// The instrument plays for half the passage; the silent half (spill only) must be left out.
static bool gateCase (const Fft& fft)
{
    const int len = 48000 * 24;
    const auto own = programme (len, 8), spill = programme (len, 9);
    std::vector<float> ref ((size_t) len), tgt ((size_t) len);
    const auto ownAtMain = delayed (own, 500.0), spillAtSpot = delayed (spill, 1500.0, 0.05f);
    for (int i = 0; i < len; ++i)
    {
        const float playing = i < len / 2 ? 1.0f : 0.0f;
        ref[(size_t) i] = playing * ownAtMain[(size_t) i] + spill[(size_t) i];
        tgt[(size_t) i] = playing * own[(size_t) i] + spillAtSpot[(size_t) i];
    }
    DelayEstimator est;
    feed (est, fft, ref, tgt);
    std::vector<std::complex<float>> scratch;
    const auto r = est.estimate (fft, kRate, scratch);
    return report ("listens only while the mic's source plays", est.skipped() > 20 && std::abs (r.delaySamples + 500.0) < 0.5 && r.reliable,
                   "%d frames used, %d left out, delay %.1f (want -500)", est.frames(), est.skipped(), r.delaySamples);
}

// A phase-shifted copy of the reference: the fine curve and its filter should bring it back in line.
static bool phaseCase (const Fft& fft)
{
    const int len = 48000 * 20, firLen = 2048;
    const auto ref = programme (len, 10);
    std::vector<float> tgt ((size_t) len, 0.0f);
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    for (int i = 0; i < len; ++i) // two all-pass stages: the phase turns a full circle across the spectrum
    {
        const float x = ref[(size_t) i];
        const float a = -0.6f * x + x1 + 0.6f * y1;   x1 = x; y1 = a;
        const float b = -0.6f * a + x2 + 0.6f * y2;   x2 = a; y2 = b;
        tgt[(size_t) i] = b;
    }

    DelayEstimator est;
    feed (est, fft, ref, tgt);
    std::vector<std::complex<float>> scratch;
    const auto r = est.estimate (fft, kRate, scratch);
    const int d = (int) std::lround (r.delaySamples);
    const auto curve = est.phaseCurve (kRate, (double) d, r.polarity);

    float correction[kFineBands], half[kFineBands];
    correctionCurve (curve.phase, curve.trust, nullptr, 1.0, 20.0, 20000.0, correction);
    correctionCurve (curve.phase, curve.trust, nullptr, 1.0, 20.0, 500.0, half);
    std::vector<float> taps, lowOnly;
    designPhaseFir (correction, kRate, firLen, taps);
    designPhaseFir (half, kRate, firLen, lowOnly);

    const auto match = [&] (const std::vector<float>* fir)
    {
        double xy = 0, xx = 0, yy = 0;
        for (int i = 48000; i < 48000 * 4; ++i)
        {
            double y = 0.0;
            if (fir != nullptr) for (int k = 0; k < firLen; ++k) y += (*fir)[(size_t) k] * tgt[(size_t) (i + d + firLen / 2 - k)];
            else                y = tgt[(size_t) (i + d)];
            y *= r.polarity;
            xy += y * ref[(size_t) i]; xx += (double) ref[(size_t) i] * ref[(size_t) i]; yy += y * y;
        }
        return xy / std::sqrt (xx * yy);
    };
    int trusted = 0;
    for (float t : curve.trust) trusted += t > 0.9f ? 1 : 0;
    const double before = match (nullptr), after = match (&taps), partial = match (&lowOnly);
    return report ("phase per 1/12 octave", after > 0.97 && partial > before + 0.05 && partial < after - 0.05 && trusted > 90,
                   "match: delay only %.3f, range to 500 Hz %.3f, full %.3f; %d of %d bands trusted", before, partial, after, trusted, kFineBands);
}

// Convolution with a long response, for a room.
static std::vector<float> convolve (const std::vector<float>& x, const std::vector<float>& h)
{
    const int size = 131072, block = size / 2;
    Fft big (size);
    std::vector<std::complex<float>> hf ((size_t) size), buf ((size_t) size);
    for (size_t i = 0; i < h.size(); ++i) hf[i] = { h[i], 0.0f };
    big.forward (hf.data());
    std::vector<float> y (x.size() + h.size(), 0.0f);
    for (size_t start = 0; start < x.size(); start += (size_t) block)
    {
        std::fill (buf.begin(), buf.end(), std::complex<float> {});
        for (size_t i = 0; i < (size_t) block && start + i < x.size(); ++i) buf[i] = { x[start + i], 0.0f };
        big.forward (buf.data());
        for (size_t i = 0; i < (size_t) size; ++i) buf[i] *= hf[i];
        big.inverse (buf.data());
        for (size_t i = 0; i < (size_t) size && start + i < y.size(); ++i) y[start + i] += buf[i].real();
    }
    y.resize (x.size());
    return y;
}

// As in a real hall: the main pair hears the instrument's lows as the spot does, but its
// highs come off the instrument differently, with reverberation and another player on top.
// Only a small part of the spectrum is shared.
static bool hallCase (const Fft& fft)
{
    const int len = 48000 * 30;
    const auto own = programme (len, 20), highsAtMain = programme (len, 21), other = programme (len, 22);
    std::mt19937 rng (23);
    std::normal_distribution<float> g (0.0f, 1.0f);
    std::vector<float> tail (48000);
    for (int i = 0; i < 48000; ++i) tail[(size_t) i] = i < 900 ? 0.0f : g (rng) * std::exp (-i / 9000.0f) * 0.012f; // about 1.3 s of reverberation, a little under the direct sound
    const auto reverb = convolve (own, tail);

    std::vector<float> ref ((size_t) len, 0.0f), tgt = own;
    float lp1 = 0, lp2 = 0, hp1 = 0, hp2 = 0;
    const int delay = 640; // the main pair is 4.6 m further away
    for (int i = delay; i < len; ++i)
    {
        lp1 += 0.12f * (own[(size_t) (i - delay)] - lp1); lp2 += 0.12f * (lp1 - lp2);                 // shared: below about 900 Hz
        hp1 += 0.12f * (highsAtMain[(size_t) i] - hp1);   hp2 += 0.12f * (hp1 - hp2);
        ref[(size_t) i] = lp2 + (highsAtMain[(size_t) i] - hp2) + reverb[(size_t) i] + 0.8f * other[(size_t) i];
    }
    for (int i = 0; i < len; ++i) tgt[(size_t) i] += 0.3f * other[(size_t) std::max (0, i - 300)];

    DelayEstimator est;
    feed (est, fft, ref, tgt);
    std::vector<std::complex<float>> scratch;
    const SearchRange spot { -8400.0, 144.0 };
    const auto plain = est.estimate (fft, kRate, scratch, spot, nullptr, false);
    const auto weighted = est.estimate (fft, kRate, scratch, spot);
    const auto curve = est.phaseCurve (kRate, weighted.delaySamples, weighted.polarity);
    double lowTrust = 0.0, highTrust = 0.0;
    int lows = 0, highs = 0;
    for (int b = 0; b < kFineBands; ++b)
    {
        const double f = bandFrequency (b);
        if (f > 100.0 && f < 500.0) { lowTrust += curve.trust[b]; ++lows; }
        if (f > 4000.0 && f < 16000.0) { highTrust += curve.trust[b]; ++highs; }
    }
    lowTrust /= lows; highTrust /= highs;
    // The low-pass that models the shared lows adds about 10 samples of its own delay.
    const bool ok = weighted.delaySamples < -(delay + 2.0) && weighted.delaySamples > -(delay + 16.0) && weighted.reliable
                    && weighted.snr > 1.5 * plain.snr && lowTrust > 0.5 && highTrust < 0.02;
    return report ("hall: only the lows are shared", ok, "delay %.1f (want about -650)  plain: height %.3f snr %.0f  weighted: height %.3f snr %.0f  trust lows %.2f highs %.3f",
                   weighted.delaySamples, std::abs (plain.peaks[0].value), plain.snr, std::abs (weighted.peaks[0].value), weighted.snr, lowTrust, highTrust);
}

// Nothing in common: no band of the phase curve may be trusted.
static bool noiseCurveCase (const Fft& fft)
{
    const int len = 48000 * 60; // a long passage: plenty of averages, still nothing to find
    DelayEstimator est;
    feed (est, fft, programme (len, 30), programme (len, 31));
    const auto curve = est.phaseCurve (kRate, -400.0, 1);
    float most = 0.0f;
    for (float t : curve.trust) most = std::max (most, t);
    return report ("unrelated signals: no phase is trusted", most < 0.01f, "highest trust over 111 bands: %.4f", most);
}

// Lows arriving later than highs must show as such.
static bool zoneCase (const Fft& fft)
{
    const int len = 48000 * 20;
    const auto ref = programme (len, 11);
    // Split at about 700 Hz with a one-pole pair, delay the low part by 24 samples (0.5 ms).
    std::vector<float> low ((size_t) len), high ((size_t) len);
    float lp = 0.0f;
    for (int i = 0; i < len; ++i) { lp += 0.09f * (ref[(size_t) i] - lp); low[(size_t) i] = lp; high[(size_t) i] = ref[(size_t) i] - lp; }
    std::vector<float> tgt ((size_t) len, 0.0f);
    for (int i = 24; i < len; ++i) tgt[(size_t) i] = low[(size_t) (i - 24)] + high[(size_t) i];

    DelayEstimator est;
    feed (est, fft, ref, tgt);
    const auto z = zoneDelays (est.phaseCurve (kRate, 0.0, 1));
    return report ("lows later than highs is seen", z.valid && z.lowMs > 0.3 && z.lowMs < 0.7 && std::abs (z.highMs) < 0.05,
                   "lows %+.2f ms, mids %+.2f ms, highs %+.2f ms (lows were delayed 0.50 ms)", z.lowMs, z.midMs, z.highMs);
}

// The fractional-delay kernel: right delay, flat response.
static bool fractionCase()
{
    float h[kSincTaps];
    makeSincKernel (0.37, h);
    const auto response = [&] (double hz)
    {
        std::complex<double> sum {};
        for (int k = 0; k < kSincTaps; ++k) sum += (double) h[k] * std::polar (1.0, -2.0 * kPi * hz / kRate * k);
        return sum;
    };
    const double db1k = 20.0 * std::log10 (std::abs (response (1000.0))), db18k = 20.0 * std::log10 (std::abs (response (18000.0)));
    const double delay = -std::arg (response (1000.0)) / (2.0 * kPi * 1000.0 / kRate);
    return report ("sub-sample delay kernel", std::abs (delay - (kSincCentre + 0.37)) < 0.002 && std::abs (db1k) < 0.05 && std::abs (db18k) < 0.5,
                   "delay %.4f samples (want %.2f), %+.3f dB at 1 kHz, %+.3f dB at 18 kHz", delay, kSincCentre + 0.37, db1k, db18k);
}

// Geometry and panning.
static bool geometryCase()
{
    ArraySettings arr;
    applyPreset (arr, ArrayType::ortf);
    const auto geo = arrayGeometry (arr);
    const Vec2 src { -2.0, 5.0 };
    const double rL = distance (src, geo.pos[0]), rR = distance (src, geo.pos[2]);
    const auto p = locate (geo, rL, rR);
    const double pan = naturalPan (geo, src);
    float l = 0, r = 0, cl = 0, cr = 0;
    panLawGains (pan, 0, l, r);
    panLawGains (0.0, 0, cl, cr);
    const bool ok = std::abs (p.x - src.x) < 1e-6 && std::abs (p.y - src.y) < 1e-6 && pan < -0.1 && l > r
                    && std::abs (l * l + r * r - 1.0f) < 1e-4f && std::abs (cl - 0.7071f) < 1e-3f;
    return report ("plan geometry and pan", ok, "located (%.2f, %.2f)  natural pan %+.2f  gains L %.2f R %.2f", p.x, p.y, pan, l, r);
}

int main()
{
    Fft fft (kFrame);
    bool ok = true;
    ok &= delayCase (fft, "identical", 0.0, 1, 0.0f, 0.01);
    ok &= delayCase (fft, "spot 3 m ahead of main", -420.0, 1, 0.0f, 0.01);
    ok &= delayCase (fft, "sub-sample", 137.37, 1, 0.0f, 0.01);
    ok &= delayCase (fft, "inverted polarity", 512.25, -1, 0.0f, 0.01);
    ok &= delayCase (fft, "far ambience (30 m)", 4198.6, 1, 0.0f, 0.02);
    ok &= delayCase (fft, "spill as loud as the source", 250.5, 1, 1.0f, 0.03);
    ok &= delayCase (fft, "spill 12 dB above the source", -88.8, -1, 4.0f, 0.1);
    ok &= peaksCase (fft);
    ok &= rangeCase (fft);
    ok &= unrelatedCase (fft);
    ok &= gateCase (fft);
    ok &= phaseCase (fft);
    ok &= hallCase (fft);
    ok &= noiseCurveCase (fft);
    ok &= zoneCase (fft);
    ok &= fractionCase();
    ok &= geometryCase();
    return ok ? 0 : 1;
}
