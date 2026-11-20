#include "DelayEstimator.h"
#include <algorithm>
#include <cmath>

namespace arraylab
{
namespace
{
constexpr double kReliableSnr = 8.0;   // a peak this far above the floor is not chance
constexpr int kPeakSeparation = 12;    // samples between two distinct candidates

double smoothstep (double x, double lo, double hi)
{
    const double t = std::clamp ((x - lo) / (hi - lo), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

}

double phaseTrust (double coherence, double averages)
{
    if (coherence < 0.03 || averages < 1.0)
        return 0.0;
    const double g2 = std::min (coherence * coherence, 0.999999);
    const double sigma = std::sqrt ((1.0 - g2) / (2.0 * averages * g2)); // standard error of the phase, radians
    return 1.0 - smoothstep (sigma, 0.12, 0.5);
}

FrameSpectrum::FrameSpectrum (int frameSize) : data ((size_t) frameSize), window ((size_t) frameSize)
{
    for (int i = 0; i < frameSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / frameSize));
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

void FrameSpectrum::setSum (const FrameSpectrum* const* parts, int numParts)
{
    std::fill (data.begin(), data.end(), std::complex<float> {});
    frameEnergy = 0.0;
    for (int p = 0; p < numParts; ++p)
    {
        const auto& src = parts[p]->data;
        for (size_t i = 0; i < data.size(); ++i)
            data[i] += src[i];
        frameEnergy += parts[p]->frameEnergy;
    }
}

//==============================================================================
void DelayEstimator::reset (int frameSize)
{
    n = frameSize;
    numFrames = numSkipped = 0;
    recentPeak = 0.0;
    sxy.clear(); sxx.clear(); syy.clear(); // reallocated on first use
}

bool DelayEstimator::accumulate (const FrameSpectrum& ref, const FrameSpectrum& tgt)
{
    if (n == 0 || ref.energy() < 1e-10 || tgt.energy() < 1e-10)
        return false;

    // Only listen while the microphone's own instrument plays: frames more than
    // 15 dB under its recent level hold mostly spill from the neighbours.
    recentPeak = std::max (tgt.energy(), recentPeak * 0.98);
    if (tgt.energy() < recentPeak * 0.03)
    {
        ++numSkipped;
        return false;
    }

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
    return true;
}

DelayResult DelayEstimator::estimate (const Fft& fft, double sampleRate, std::vector<std::complex<float>>& c,
                                      SearchRange range, CorrelationCurve* curve) const
{
    DelayResult r;
    r.frames = numFrames;
    if (numFrames == 0 || n == 0 || sxy.empty())
        return r;

    c.assign ((size_t) n, {});
    const int kLo = std::max (1, (int) std::ceil (40.0 * n / sampleRate));
    const int kHi = std::min (n / 2 - 1, (int) std::floor (16000.0 * n / sampleRate));
    if (kHi <= kLo)
        return r;

    for (int k = kLo; k <= kHi; ++k)
    {
        const double den = std::sqrt ((double) sxx[(size_t) k] * (double) syy[(size_t) k]);
        if (den <= 0.0) continue;
        const auto w = std::complex<double> (sxy[(size_t) k]) / den; // complex coherence, |w| <= 1
        c[(size_t) k] = { (float) w.real(), (float) w.imag() };
        c[(size_t) (n - k)] = std::conj (c[(size_t) k]);
    }
    fft.inverse (c.data());

    const int lo = (int) std::ceil (std::max (range.minLag, -(double) (n / 2 - 2)));
    const int hi = (int) std::floor (std::min (range.maxLag, (double) (n / 2 - 2)));
    if (hi < lo)
        return r;
    const auto at = [&] (int lag) { return (double) c[(size_t) ((lag + n) % n)].real(); };

    double best = 0.0;
    int bestLag = lo;
    for (int i = lo; i <= hi; ++i)
        if (const double v = std::abs (at (i)); v > best) { best = v; bestLag = i; }
    if (best <= 0.0)
        return r;

    // Candidates: local maxima within reach of the best one, kept apart from each other.
    struct Candidate { int lag; double height; };
    std::vector<Candidate> found { { bestLag, best } };
    for (int i = lo; i <= hi; ++i)
    {
        const double v = std::abs (at (i));
        if (i != bestLag && v >= 0.2 * best && v >= std::abs (at (i - 1)) && v > std::abs (at (i + 1)))
            found.push_back ({ i, v });
    }
    std::sort (found.begin() + 1, found.end(), [] (const Candidate& a, const Candidate& b) { return a.height > b.height; });

    for (auto& cand : found)
    {
        if (r.numPeaks == kMaxPeaks) break;
        bool tooClose = false;
        for (int p = 0; p < r.numPeaks; ++p)
            tooClose |= std::abs (r.peaks[p].lag - cand.lag) < kPeakSeparation;
        if (tooClose) continue;

        // Parabolic interpolation gives a first sub-sample position.
        const double sign = at (cand.lag) < 0.0 ? -1.0 : 1.0;
        const double a = sign * at (cand.lag - 1), b = sign * at (cand.lag), d = sign * at (cand.lag + 1);
        const double den = a - 2.0 * b + d;
        const double frac = std::abs (den) > 1e-20 ? std::clamp (0.5 * (a - d) / den, -0.5, 0.5) : 0.0;
        r.peaks[r.numPeaks++] = { cand.lag + frac, (float) (sign * cand.height) };
    }

    double floorSq = 0.0;
    int floorCount = 0;
    for (int i = lo; i <= hi; ++i)
        if (std::abs (i - bestLag) > 2 * kPeakSeparation) { floorSq += at (i) * at (i); ++floorCount; }
    const double floor = floorCount > 32 ? std::sqrt (floorSq / floorCount) : 0.0;

    r.valid = true;
    r.delaySamples = r.peaks[0].lag;
    r.polarity = r.peaks[0].value < 0.0f ? -1 : 1;
    r.confidence = std::clamp (best * n / (2.0 * (kHi - kLo + 1)), 0.0, 1.0);
    r.snr = floor > 0.0 ? best / floor : 0.0;
    r.reliable = numFrames >= 3 && r.snr >= kReliableSnr;

    if (curve != nullptr)
    {
        const int span = hi - lo + 1, count = std::min (span, 8192);
        curve->minLag = lo;
        curve->maxLag = hi;
        curve->points.assign ((size_t) count, 0.0f);
        for (int i = lo; i <= hi; ++i)
        {
            auto& slot = curve->points[(size_t) std::min (count - 1, (int) ((long long) (i - lo) * count / span))];
            if (const float v = (float) at (i); std::abs (v) > std::abs (slot)) slot = v;
        }
    }
    return r;
}

/** Complex coherence per 1/12 octave, turned back by `delaySamples` and `polarity`:
    what is left is the phase a plain delay cannot explain. */
void DelayEstimator::transfer (double sampleRate, double delaySamples, int polarity, std::complex<double>* gamma, double* averages) const
{
    const double edge = std::pow (2.0, 1.0 / 24.0);
    for (int b = 0; b < kFineBands; ++b)
    {
        gamma[b] = {};
        averages[b] = 0.0;
        const double f = bandFrequency (b);
        if (sxy.empty() || f > 0.47 * sampleRate)
            continue;

        int k0 = (int) std::ceil (f / edge * n / sampleRate), k1 = (int) std::floor (f * edge * n / sampleRate);
        if (k1 < k0) k0 = k1 = (int) std::lround (f * n / sampleRate);
        k0 = std::clamp (k0, 1, n / 2 - 1);
        k1 = std::clamp (k1, 1, n / 2 - 1);

        std::complex<double> sum {};
        double xx = 0.0, yy = 0.0;
        const auto step = std::polar (1.0, 2.0 * kPi * delaySamples / n);
        auto turn = std::polar (1.0, 2.0 * kPi * delaySamples * k0 / n);
        for (int k = k0; k <= k1; ++k)
        {
            sum += std::complex<double> (sxy[(size_t) k]) * turn;
            xx += sxx[(size_t) k];
            yy += syy[(size_t) k];
            turn *= step;
        }
        if (xx > 0.0 && yy > 0.0)
        {
            gamma[b] = sum / std::sqrt (xx * yy) * (double) polarity;
            averages[b] = std::max (1.0, numFrames * (k1 - k0 + 1) / 3.0); // overlapping, windowed frames are not independent
        }
    }
}

PhaseCurve DelayEstimator::phaseCurve (double sampleRate, double delaySamples, int polarity) const
{
    PhaseCurve curve;
    if (numFrames == 0 || sxy.empty())
        return curve;

    std::complex<double> gamma[kFineBands];
    double averages[kFineBands];
    transfer (sampleRate, delaySamples, polarity, gamma, averages);
    for (int b = 0; b < kFineBands; ++b)
    {
        curve.phase[b] = (float) std::arg (gamma[b]);
        curve.trust[b] = (float) phaseTrust (std::abs (gamma[b]), averages[b]);
    }
    curve.valid = true;
    return curve;
}

double DelayEstimator::refineDelay (double sampleRate, double delaySamples, int polarity) const
{
    if (numFrames == 0 || sxy.empty())
        return delaySamples;

    std::complex<double> gamma[kFineBands];
    double averages[kFineBands];
    double delay = delaySamples;
    for (int pass = 0; pass < 3; ++pass)
    {
        // A pure delay error is a phase that grows in proportion to frequency:
        // fit that straight line through the trusted bands.
        transfer (sampleRate, delay, polarity, gamma, averages);
        double num = 0.0, den = 0.0;
        for (int b = 0; b < kFineBands; ++b)
        {
            const double f = bandFrequency (b), phi = std::arg (gamma[b]);
            const double w = phaseTrust (std::abs (gamma[b]), averages[b]);
            if (f < 100.0 || f > 8000.0 || w < 0.3 || std::abs (phi) > 1.0) continue;
            num += w * f * phi;
            den += w * f * f;
        }
        if (den <= 0.0)
            break;
        delay += std::clamp (-sampleRate / (2.0 * kPi) * num / den, -0.5, 0.5);
    }
    return std::clamp (delay, delaySamples - 1.0, delaySamples + 1.0);
}

ZoneDelays zoneDelays (const PhaseCurve& curve)
{
    ZoneDelays z;
    if (! curve.valid)
        return z;

    const double edges[4] = { 31.0, 250.0, 2000.0, 18000.0 };
    double* out[3] = { &z.lowMs, &z.midMs, &z.highMs };
    int solved = 0;
    for (int zone = 0; zone < 3; ++zone)
    {
        // Slope of the unwrapped phase against frequency = group delay.
        double sw = 0, sf = 0, sp = 0, sff = 0, sfp = 0, last = 0.0;
        int used = 0;
        for (int b = 0; b < kFineBands; ++b)
        {
            const double f = bandFrequency (b), w = curve.trust[b];
            if (f < edges[zone] || f >= edges[zone + 1] || w < 0.3) continue;
            double phi = curve.phase[b];
            if (used > 0) phi -= 2.0 * kPi * std::round ((phi - last) / (2.0 * kPi));
            last = phi;
            sw += w; sf += w * f; sp += w * phi; sff += w * f * f; sfp += w * f * phi;
            ++used;
        }
        const double den = sw * sff - sf * sf;
        if (used >= 4 && std::abs (den) > 1e-9)
        {
            *out[zone] = -(sw * sfp - sf * sp) / den / (2.0 * kPi) * 1000.0;
            ++solved;
        }
    }
    z.valid = solved > 0;
    return z;
}
}
