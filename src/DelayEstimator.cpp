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
    if (averages < 2.0)
        return 0.0;

    // Two signals with nothing in common still show a coherence of about 1/sqrt(averages).
    // A reading has to stand well clear of that before its phase means anything.
    const double g2 = std::min (coherence * coherence, 0.999999);
    const double clean = (g2 - 1.0 / averages) / (1.0 - 1.0 / averages);
    if (clean <= 0.0)
        return 0.0;
    const double significant = smoothstep (coherence * std::sqrt (averages), 3.0, 6.0);

    // Standard error of the phase, radians.
    const double sigma = std::sqrt ((1.0 - clean) / (2.0 * averages * clean));
    const double precise = 1.0 - smoothstep (sigma, 0.12, 0.5);

    // Where the two microphones barely share anything, lining up their phase gains nothing
    // in the sum and only colours the spot: leave those bands alone.
    const double worthwhile = smoothstep (std::sqrt (clean), 0.08, 0.3);
    return significant * precise * worthwhile;
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
namespace { constexpr int kGroup = 8; } // bins judged together

void DelayEstimator::reset (int frameSize)
{
    n = frameSize;
    numFrames = numSkipped = 0;
    recentPeak = 0.0;
    kept = offered = 0.0;
    sxy.clear(); sxx.clear(); syy.clear(); // reallocated on first use
    allXx.clear(); allYy.clear();
}

bool DelayEstimator::accumulate (const FrameSpectrum& ref, const FrameSpectrum& tgt, bool ownSourceOnly)
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

    const int bins = n / 2 + 1, groups = bins / kGroup;
    if (sxy.empty())
    {
        sxy.assign ((size_t) bins, {});
        sxx.assign ((size_t) bins, 0.0f);
        syy.assign ((size_t) bins, 0.0f);
        allXx.assign ((size_t) groups, 0.0f);
        allYy.assign ((size_t) groups, 0.0f);
    }

    const auto& x = ref.bins();
    const auto& y = tgt.bins();

    // The usual balance between the two microphones, octave by octave, from everything heard
    // so far. Judging each narrow slice against itself would not do: where only the spill ever
    // sounds, the spill would pass for normal.
    static thread_local std::vector<double> runX, runY;
    runX.assign ((size_t) groups + 1, 0.0);
    runY.assign ((size_t) groups + 1, 0.0);
    for (int g = 0; g < groups; ++g)
    {
        runX[(size_t) g + 1] = runX[(size_t) g] + allXx[(size_t) g];
        runY[(size_t) g + 1] = runY[(size_t) g] + allYy[(size_t) g];
    }

    for (int g = 0; g < groups; ++g)
    {
        const int k0 = g * kGroup;
        float ex = 0.0f, ey = 0.0f;
        for (int k = k0; k < k0 + kGroup; ++k) { ex += std::norm (x[(size_t) k]); ey += std::norm (y[(size_t) k]); }

        // Is the target louder than usual against the reference here and now? Then its own
        // source is what it is hearing. The balance needs a few frames before it means anything.
        float weight = 1.0f;
        if (ownSourceOnly && numFrames >= 4 && ex > 0.0f)
        {
            const int g0 = std::clamp ((int) (g * 0.7071), 0, groups - 1), g1 = std::clamp ((int) (g * 1.4142) + 1, g0, groups - 1);
            const double usualX = runX[(size_t) g1 + 1] - runX[(size_t) g0], usualY = runY[(size_t) g1 + 1] - runY[(size_t) g0];
            if (usualX > 0.0 && usualY > 0.0)
            {
                const double dominance = ((double) ey / ex) / (usualY / usualX); // 1 = the usual balance
                weight = dominance >= 1.0 ? 1.0f : (float) (dominance * dominance);
            }
        }
        allXx[(size_t) g] += ex;
        allYy[(size_t) g] += ey;
        offered += 1.0;
        kept += weight;
        if (weight < 1.0e-3f)
            continue;

        for (int k = k0; k < k0 + kGroup; ++k)
        {
            sxy[(size_t) k] += weight * y[(size_t) k] * std::conj (x[(size_t) k]);
            sxx[(size_t) k] += weight * std::norm (x[(size_t) k]);
            syy[(size_t) k] += weight * std::norm (y[(size_t) k]);
        }
    }
    ++numFrames;
    return true;
}

DelayResult DelayEstimator::estimate (const Fft& fft, double sampleRate, std::vector<std::complex<float>>& c,
                                      SearchRange range, CorrelationCurve* curve, bool weighted) const
{
    DelayResult r;
    r.frames = numFrames;
    if (numFrames == 0 || n == 0 || sxy.empty())
        return r;

    const int kLo = std::max (1, (int) std::ceil (40.0 * n / sampleRate));
    const int kHi = std::min (n / 2 - 1, (int) std::floor (16000.0 * n / sampleRate));
    if (kHi <= kLo)
        return r;

    const int lo = (int) std::ceil (std::max (range.minLag, -(double) (n / 2 - 2)));
    const int hi = (int) std::floor (std::min (range.maxLag, (double) (n / 2 - 2)));
    if (hi < lo)
        return r;

    // First pass: every frequency counts the same (SCOT).
    const auto correlate = [&] (const std::vector<float>* weight)
    {
        c.assign ((size_t) n, {});
        for (int k = kLo; k <= kHi; ++k)
        {
            const double den = std::sqrt ((double) sxx[(size_t) k] * (double) syy[(size_t) k]);
            if (den <= 0.0) continue;
            const auto w = std::complex<double> (sxy[(size_t) k]) / den * (weight != nullptr ? (double) (*weight)[(size_t) k] : 1.0);
            c[(size_t) k] = { (float) w.real(), (float) w.imag() };
            c[(size_t) (n - k)] = std::conj (c[(size_t) k]);
        }
        fft.inverse (c.data());
    };
    correlate (nullptr);

    // Second pass. In a real room two microphones only resemble each other over part of the
    // spectrum, often the lows and mids; elsewhere the first pass adds nothing but noise.
    // Measure where they agree around the first answer, and let those frequencies decide.
    if (weighted)
    {
        double first = 0.0;
        int firstLag = lo;
        for (int i = lo; i <= hi; ++i)
            if (const double v = std::abs ((double) c[(size_t) ((i + n) % n)].real()); v > first) { first = v; firstLag = i; }

        const int group = kGroup, numGroups = (n / 2) / group;
        const double averages = std::max (2.0, numFrames * group / 3.0 * (offered > 0.0 ? kept / offered : 1.0));
        std::vector<double> agree ((size_t) numGroups + 1, 0.0); // running total, for quick averages
        const auto step = std::polar (1.0, 2.0 * kPi * firstLag / n);
        auto turn = std::complex<double> (1.0, 0.0);
        for (int g = 0; g < numGroups; ++g)
        {
            std::complex<double> sum {};
            double xx = 0.0, yy = 0.0;
            for (int k = g * group; k < (g + 1) * group; ++k)
            {
                sum += std::complex<double> (sxy[(size_t) k]) * turn;
                xx += sxx[(size_t) k];
                yy += syy[(size_t) k];
                turn *= step;
            }
            const double g2 = xx > 0.0 && yy > 0.0 ? std::norm (sum) / (xx * yy) : 0.0;
            agree[(size_t) g + 1] = agree[(size_t) g] + (g2 - 1.0 / averages) / (1.0 - 1.0 / averages);
        }

        std::vector<float> weight ((size_t) n / 2 + 1, 0.0f);
        double total = 0.0;
        const double spread = std::pow (2.0, 1.0 / 6.0); // smooth over a third of an octave
        for (int g = 0; g < numGroups; ++g)
        {
            const int g0 = std::clamp ((int) std::floor ((g + 0.5) / spread - 0.5), 0, numGroups - 1);
            const int g1 = std::clamp ((int) std::ceil ((g + 0.5) * spread - 0.5), g0, numGroups - 1);
            const double clean = std::clamp ((agree[(size_t) g1 + 1] - agree[(size_t) g0]) / (g1 - g0 + 1), 0.0, 0.98);
            const float w = (float) (clean / (1.0 - clean));
            for (int k = std::max (g * group, kLo); k < (g + 1) * group && k <= kHi; ++k) { weight[(size_t) k] = w; total += w; }
        }
        if (total > 1.0e-6)
        {
            const float scale = (float) ((kHi - kLo + 1) / total);
            for (auto& w : weight) w *= scale;
            correlate (&weight);
        }
    }

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

/** Cross- and auto-spectra of one 1/12 octave, the cross-spectrum turned back by `delaySamples`. */
bool DelayEstimator::band (int b, double sampleRate, double delaySamples, std::complex<double>& cross, double& xx, double& yy, int& bins) const
{
    static const double edge = std::pow (2.0, 1.0 / 24.0);
    cross = {};
    xx = yy = 0.0;
    bins = 0;
    const double f = bandFrequency (b);
    if (sxy.empty() || f > 0.47 * sampleRate)
        return false;

    int k0 = (int) std::ceil (f / edge * n / sampleRate), k1 = (int) std::floor (f * edge * n / sampleRate);
    if (k1 < k0) k0 = k1 = (int) std::lround (f * n / sampleRate);
    k0 = std::clamp (k0, 1, n / 2 - 1);
    k1 = std::clamp (k1, 1, n / 2 - 1);

    const auto step = std::polar (1.0, 2.0 * kPi * delaySamples / n);
    auto turn = std::polar (1.0, 2.0 * kPi * delaySamples * k0 / n);
    for (int k = k0; k <= k1; ++k)
    {
        cross += std::complex<double> (sxy[(size_t) k]) * turn;
        xx += sxx[(size_t) k];
        yy += syy[(size_t) k];
        turn *= step;
    }
    bins = k1 - k0 + 1;
    return xx > 0.0 && yy > 0.0;
}

/** Complex coherence per 1/12 octave, turned back by `delaySamples` and `polarity`:
    what is left is the phase a plain delay cannot explain. */
void DelayEstimator::transfer (double sampleRate, double delaySamples, int polarity, std::complex<double>* gamma, double* averages) const
{
    for (int b = 0; b < kFineBands; ++b)
    {
        std::complex<double> cross;
        double xx, yy;
        int bins;
        gamma[b] = {};
        averages[b] = 0.0;
        if (band (b, sampleRate, delaySamples, cross, xx, yy, bins))
        {
            gamma[b] = cross / std::sqrt (xx * yy) * (double) polarity;
            // Overlapping, windowed frames are not independent, and only part of the material was kept.
            averages[b] = std::max (1.0, numFrames * bins / 3.0 * (offered > 0.0 ? kept / offered : 1.0));
        }
    }
}

double DelayEstimator::sumGain (double sampleRate, double lag, int polarity, double rotation, bool limit, double lowHz, double highHz, double level) const
{
    if (numFrames == 0 || sxy.empty())
        return 0.0;
    const auto turn = std::polar ((double) polarity, rotation);
    double total = 0.0;
    int count = 0;
    for (int b = 0; b < kFineBands; ++b)
    {
        const double f = bandFrequency (b);
        std::complex<double> cross;
        double xx, yy;
        int bins;
        if (f < lowHz || f > highHz || ! band (b, sampleRate, lag, cross, xx, yy, bins))
            continue;
        // Power of the sum over the power of the two taken separately: 1 + 2 Re(cross) / (xx + yy).
        const double shared = limit ? std::abs (cross) : (cross * turn).real();
        total += 10.0 * std::log10 (std::max (0.05, 1.0 + 2.0 * level * shared / (xx + level * level * yy)));
        ++count;
    }
    return count > 0 ? total / count : 0.0;
}

int DelayEstimator::gainCandidates (const Fft& fft, double sampleRate, std::vector<std::complex<float>>& c, SearchRange range,
                                    Peak* found, int maxFound, double lowHz, double highHz, double level) const
{
    if (numFrames == 0 || sxy.empty() || maxFound <= 0)
        return 0;

    // Weight of each 1/12 octave: one over the power the two microphones put into it, so that the
    // correlation adds up, band by band, the same ratio sumGain turns into decibels.
    double weightOf[kFineBands] {};
    for (int b = 0; b < kFineBands; ++b)
    {
        std::complex<double> cross;
        double xx, yy;
        int bins;
        const double f = bandFrequency (b);
        if (f >= lowHz && f <= highHz && band (b, sampleRate, 0.0, cross, xx, yy, bins))
            weightOf[b] = 2.0 * level / (xx + level * level * yy);
    }

    c.assign ((size_t) n, {});
    for (int k = 1; k < n / 2; ++k)
    {
        const int b = (int) std::lround (bandPosition (k * sampleRate / n));
        if (b < 0 || b >= kFineBands || weightOf[b] <= 0.0) continue;
        c[(size_t) k] = sxy[(size_t) k] * (float) weightOf[b];
        c[(size_t) (n - k)] = std::conj (c[(size_t) k]);
    }
    fft.inverse (c.data());

    const int lo = (int) std::ceil (std::max (range.minLag, -(double) (n / 2 - 2)));
    const int hi = (int) std::floor (std::min (range.maxLag, (double) (n / 2 - 2)));
    const auto at = [&] (int lag) { return (double) c[(size_t) ((lag + n) % n)].real(); };

    struct Candidate { int lag; double height; };
    std::vector<Candidate> all;
    for (int i = lo; i <= hi; ++i)
    {
        const double v = std::abs (at (i));
        if (v >= std::abs (at (i - 1)) && v > std::abs (at (i + 1)))
            all.push_back ({ i, v });
    }
    std::sort (all.begin(), all.end(), [] (const Candidate& a, const Candidate& b) { return a.height > b.height; });

    int count = 0;
    for (auto& cand : all)
    {
        if (count == maxFound) break;
        bool tooClose = false;
        for (int p = 0; p < count; ++p)
            tooClose |= std::abs (found[p].lag - cand.lag) < kPeakSeparation;
        if (tooClose) continue;
        const double sign = at (cand.lag) < 0.0 ? -1.0 : 1.0;
        const double a = sign * at (cand.lag - 1), b = sign * at (cand.lag), d = sign * at (cand.lag + 1);
        const double den = a - 2.0 * b + d;
        const double frac = std::abs (den) > 1e-20 ? std::clamp (0.5 * (a - d) / den, -0.5, 0.5) : 0.0;
        found[count++] = { cand.lag + frac, (float) sign, 0.0f };
    }
    return count;
}

std::complex<double> DelayEstimator::sumVector (double sampleRate, double lag, int polarity, double lowHz, double highHz, double level) const
{
    std::complex<double> total {};
    if (numFrames == 0 || sxy.empty())
        return total;
    for (int b = 0; b < kFineBands; ++b)
    {
        const double f = bandFrequency (b);
        std::complex<double> cross;
        double xx, yy;
        int bins;
        if (f >= lowHz && f <= highHz && band (b, sampleRate, lag, cross, xx, yy, bins))
            total += cross * ((double) polarity * level) / (xx + level * level * yy);
    }
    return total;
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
