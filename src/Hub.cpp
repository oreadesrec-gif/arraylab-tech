#include "Hub.h"
#include "PhaseFir.h"

namespace arraylab
{
namespace
{
double wrapPhase (double p) { return p - 2.0 * kPi * std::round (p / (2.0 * kPi)); }
}

//==============================================================================
Effective effectiveResult (const MicResults& res, const MicState& m, int r, bool stereoPair)
{
    Effective e;
    const auto& v = res.vs[r];
    if (! v.valid)
        return e;

    e.valid = true;
    e.reliable = v.reliable;
    e.delaySamples = v.delaySamples;
    e.polarity = v.polarity;
    e.confidence = v.confidence;

    if (m.peakLocked[r])
    {
        // Follow the picked peak if the analysis still sees one there, else hold the picked delay.
        double lag = m.peakLag[r], nearest = 24.0;
        int polarity = v.polarity;
        for (int p = 0; p < v.numPeaks; ++p)
            if (const double d = std::abs (v.peaks[p].lag - m.peakLag[r]); d < nearest)
            {
                nearest = d;
                lag = v.peaks[p].lag;
                polarity = v.peaks[p].value < 0.0f ? -1 : 1;
            }
        e.shift = lag - v.delaySamples;
        e.delaySamples = lag;
        e.polarity = polarity;
        e.locked = e.reliable = true;
    }

    const auto& right = res.vsRight[r];
    if (stereoPair && right.valid)
    {
        e.delaySamples += 0.5 * (right.delaySamples - v.delaySamples);
        e.confidence = juce::jmin (e.confidence, right.confidence);
        if (! e.locked) e.reliable = e.reliable && right.reliable;
    }
    return e;
}

int alignmentRef (const Effective* e, AlignTo a)
{
    const int wanted = a == AlignTo::left ? 0 : a == AlignTo::centre ? 1 : a == AlignTo::right ? 2 : a == AlignTo::sum ? 3 : -1;
    if (wanted >= 0 && e[wanted].valid)
        return wanted;

    // Nearest: the main microphone the sound reaches first, which is the one needing the least delay.
    // A reference whose peak the user picked by hand comes first, then the sure ones, then any.
    int best = -1;
    for (int pass = 0; pass < 3 && best < 0; ++pass)
        for (int r = 0; r < 3; ++r)
            if (e[r].valid && (pass == 2 || (pass == 1 && e[r].reliable) || e[r].locked)
                && (best < 0 || e[r].delaySamples > e[best].delaySamples))
                best = r;
    return best;
}

int Hub::latencyFor (int precision, double sampleRate)
{
    return precision == 1 && sampleRate > 0.0 ? juce::nextPowerOfTwo (juce::roundToInt (4096.0 * sampleRate / 48000.0)) : 0;
}

//==============================================================================
Hub::Hub() : juce::Thread ("ArrayLab analysis")
{
    scene.groupNames[0] = "Main";
    for (int g = 1; g < kMaxGroups; ++g)
        scene.groupNames[g] = "Group " + juce::String (g + 1);
    startThread (juce::Thread::Priority::low);
}

Hub::~Hub()
{
    stopThread (4000);
}

int Hub::registerInstance()
{
    const juce::ScopedLock sl (registerLock);
    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (! s.active.load())
        {
            {
                const juce::ScopedLock l (stateLock);
                s.state = {};
                s.state.name = "Mic " + juce::String (i + 1);
                s.state.uid = juce::Random::getSystemRandom().nextInt64() | 1;
                s.state.x = ((i % 10) - 4.5) * 0.8; // parked in a row until analysed or placed
                s.state.y = 2.0 + (i / 10);
                s.results = {};
                s.applied = {};
                s.sink = nullptr;
                s.firLength = 0;
                s.firSignature = 0;
            }
            s.writeEnd = 0; s.runStart = 0;
            s.playing = false; s.hasPlayhead = false;
            s.expected = -1;
            s.active = true;
            ++changes;
            return i;
        }
    }
    return -1;
}

void Hub::unregisterInstance (int slot)
{
    if (slot < 0) return;
    const juce::ScopedLock sl (registerLock);
    {
        const juce::ScopedLock l (stateLock);
        slots[(size_t) slot].sink = nullptr;
        slots[(size_t) slot].active = false;
        recomputeAll();
    }
    ++changes;
}

void Hub::setFirSink (int slot, FirSink sink)
{
    if (slot < 0) return;
    const juce::ScopedLock l (stateLock);
    slots[(size_t) slot].sink = std::move (sink);
    slots[(size_t) slot].firSignature = 0;
    recompute (slot);
}

void Hub::prepare (int slot, double sampleRate, int blockSize, int numIn, int numOut)
{
    if (slot < 0) return;
    auto& s = slots[(size_t) slot];

    // Frame of about 0.7 s, ring of eight frames.
    const int size = juce::nextPowerOfTwo ((int) (sampleRate * 0.5)) * 8;
    {
        const juce::ScopedLock l (s.ringLock);
        if (size != s.ringSize)
        {
            s.ring[0].calloc ((size_t) size);
            s.ring[1].calloc ((size_t) size);
            s.ringSize = size;
        }
        s.expected = -1;
        s.playing = false;
    }
    s.sampleRate = sampleRate;
    s.blockSize = blockSize;
    s.numIn = numIn;
    s.numOut = numOut;

    const juce::ScopedLock l (stateLock);
    s.firSignature = 0;
    recompute (slot);
    ++changes;
}

void Hub::push (int slot, const float* ch0, const float* ch1, int numSamples,
                bool hasPlayhead, bool playing, juce::int64 pos) noexcept
{
    if (slot < 0) return;
    auto& s = slots[(size_t) slot];
    s.hasPlayhead = hasPlayhead;

    if (! hasPlayhead || ! playing || pos < 0 || s.ringSize == 0 || ch0 == nullptr)
    {
        s.playing = false;
        s.expected = -1;
        return;
    }

    if (pos != s.expected) // transport start, locate or loop
        s.runStart = pos;

    const int mask = s.ringSize - 1;
    auto* r0 = s.ring[0].get();
    auto* r1 = s.ring[1].get();
    if (ch1 == nullptr) ch1 = ch0;
    for (int i = 0; i < numSamples; ++i)
    {
        const auto at = (size_t) ((pos + i) & mask);
        r0[at] = ch0[i];
        r1[at] = ch1[i];
    }

    s.expected = pos + numSamples;
    s.writeEnd = pos + numSamples;
    s.playing = true;
}

//==============================================================================
MicState Hub::getMic (int slot) const
{
    const juce::ScopedLock l (stateLock);
    return slot >= 0 ? slots[(size_t) slot].state : MicState {};
}

void Hub::setMic (int slot, const MicState& m)
{
    if (slot < 0) return;
    const juce::ScopedLock l (stateLock);
    auto& s = slots[(size_t) slot];
    const bool refsChanged = s.state.role != m.role || s.state.group != m.group;
    const auto uid = s.state.uid, partner = s.state.partner;
    s.state = m;
    s.state.uid = uid;         // identity and link are not edited through here
    s.state.partner = partner;
    s.state.group = juce::jlimit (0, kMaxGroups - 1, m.group);
    shareWithPartner (slot);
    if (refsChanged || partnerOf (slot) >= 0) recomputeAll(); else recompute (slot);
    ++changes;
}

void Hub::setHostName (int slot, const juce::String& name)
{
    if (slot < 0) return;
    const juce::ScopedLock l (stateLock);
    auto& st = slots[(size_t) slot].state;
    if (st.hostNamed || st.name.startsWith ("Mic "))
    {
        st.name = name;
        st.hostNamed = true;
        ++changes;
    }
}

int Hub::partnerOf (int slot) const
{
    const auto& m = slots[(size_t) slot].state;
    if (m.partner == 0 || isMain (m.role))
        return -1;
    for (int i = 0; i < maxSlots; ++i)
        if (i != slot && slots[(size_t) i].active.load() && slots[(size_t) i].state.uid == m.partner
            && slots[(size_t) i].state.partner == m.uid && ! isMain (slots[(size_t) i].state.role))
            return i;
    return -1;
}

/** The two microphones of a linked pair always carry the same alignment settings. */
void Hub::shareWithPartner (int slot)
{
    const int other = partnerOf (slot);
    if (other < 0) return;
    const auto& a = slots[(size_t) slot].state;
    auto& b = slots[(size_t) other].state;
    b.alignTo = a.alignTo;
    b.delayOn = a.delayOn; b.polarityOn = a.polarityOn; b.phaseOn = a.phaseOn;
    b.offsetMs = a.offsetMs;
    b.phaseAmount = a.phaseAmount; b.phaseLowHz = a.phaseLowHz; b.phaseHighHz = a.phaseHighHz;
    b.phaseRotationDeg = a.phaseRotationDeg;
    b.measureLocked = a.measureLocked;
}

void Hub::linkMics (int slot, int other)
{
    if (slot < 0) return;
    const juce::ScopedLock l (stateLock);
    const auto unlink = [this] (int i)
    {
        const int old = partnerOf (i);
        if (old >= 0) slots[(size_t) old].state.partner = 0;
        slots[(size_t) i].state.partner = 0;
    };
    unlink (slot);
    if (other >= 0 && other != slot && slots[(size_t) other].active.load())
    {
        unlink (other);
        slots[(size_t) slot].state.partner = slots[(size_t) other].state.uid;
        slots[(size_t) other].state.partner = slots[(size_t) slot].state.uid;
        shareWithPartner (slot);
    }
    recomputeAll();
    ++changes;
}

void Hub::setAllLocked (bool locked)
{
    const juce::ScopedLock l (stateLock);
    for (auto& s : slots)
        if (s.active.load() && ! isMain (s.state.role))
            s.state.measureLocked = locked;
    ++changes;
}

Scene Hub::getScene() const
{
    const juce::ScopedLock l (stateLock);
    return scene;
}

void Hub::setScene (const Scene& sc)
{
    const juce::ScopedLock l (stateLock);
    scene = sc;
    scene.numGroups = juce::jlimit (1, kMaxGroups, sc.numGroups);
    scene.precision = juce::jlimit (0, 1, sc.precision);
    scene.stamp = juce::Time::currentTimeMillis();
    precisionMode = scene.precision;
    recomputeAll();
    ++changes;
}

juce::Array<MicInfo> Hub::snapshot() const
{
    juce::Array<MicInfo> out;
    const juce::ScopedLock l (stateLock);
    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (! s.active.load())
            continue;
        MicInfo info;
        info.index = i;
        info.partnerIndex = partnerOf (i);
        info.state = s.state;
        info.results = s.results;
        info.applied = s.applied;
        info.sampleRate = s.sampleRate.load();
        info.blockSize = s.blockSize.load();
        info.numIn = s.numIn.load();
        info.numOut = s.numOut.load();
        info.hasPlayhead = s.hasPlayhead.load();
        info.playing = s.playing.load();
        out.add (info);
    }
    return out;
}

//==============================================================================
Hub::RefSet Hub::refsForGroup (int group, int* arrayGroup) const
{
    RefSet r;
    bool any = false;
    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (! s.active.load() || s.state.group != group)
            continue;
        switch (s.state.role)
        {
            case Role::mainPair: r.slot[0] = i; r.channel[0] = 0; r.slot[2] = i; r.channel[2] = 1; any = true; break;
            case Role::mainL:    r.slot[0] = i; r.channel[0] = 0; any = true; break;
            case Role::mainC:    r.slot[1] = i; r.channel[1] = 0; any = true; break;
            case Role::mainR:    r.slot[2] = i; r.channel[2] = 0; any = true; break;
            case Role::spot: case Role::outrigger: case Role::ambience: break;
        }
    }
    if (! any && group != 0) // a group without its own main microphones follows the Main group
        return refsForGroup (0, arrayGroup);
    r.group = group;
    r.slot[3] = any ? -2 : -1; // -2: built from the others
    if (arrayGroup != nullptr)
        *arrayGroup = group;
    return r;
}

void Hub::recomputeAll()
{
    for (int i = 0; i < maxSlots; ++i)
        if (slots[(size_t) i].active.load())
            recompute (i);
}

/** Turns measurements, stage position and switches into what the audio thread applies. */
void Hub::recompute (int slot)
{
    auto& s = slots[(size_t) slot];
    const auto& m = s.state;
    const auto& res = s.results;
    const double sr = s.sampleRate.load();
    const int numIn = s.numIn.load(), numOut = s.numOut.load();
    const int latency = latencyFor (scene.precision, sr);

    MicApplied a;
    double dL = 0.0, dR = 0.0; // alignment delays, samples
    float gL = 1.0f, gR = 1.0f, crossLR = 0.0f, crossRL = 0.0f;
    bool phase = false;
    int firLength = 0;
    const bool fromMono = numOut == 2 && (numIn == 1 || (numIn == 2 && m.monoSource));
    const bool pair = numIn == 2 && ! m.monoSource;
    float bypassL = fromMono ? 0.70710678f : 1.0f, bypassR = bypassL;

    if (isMain (m.role))
    {
        a.isReference = true;
        a.note = "Reference: left untouched";
        if (fromMono) // a mono main mic on a stereo output goes to its own side
        {
            gL = m.role == Role::mainL ? 1.0f : m.role == Role::mainR ? 0.0f : 0.70710678f;
            gR = m.role == Role::mainR ? 1.0f : m.role == Role::mainL ? 0.0f : 0.70710678f;
            bypassL = gL;
            bypassR = gR;
        }
    }
    else if (sr > 0.0)
    {
        int arrayGroup = 0;
        refsForGroup (m.group, &arrayGroup);
        const auto& arr = scene.arrays[arrayGroup];
        const auto geo = arrayGeometry (arr);
        const double c = speedOfSound (scene.tempC);
        const double dz = arr.height - m.z;
        const Vec2 p { m.x, m.y };
        const double rL = std::hypot (distance (p, geo.pos[0]), dz);
        const double rR = std::hypot (distance (p, geo.pos[2]), dz);

        // This microphone's own measurements, and those of the microphone it is paired with, if any.
        const int partner = partnerOf (slot);
        const bool leads = partner < 0 || slot < partner; // the lower track speaks for the pair
        Effective own[kNumRefs], other[kNumRefs], e[kNumRefs];
        bool polarityDiffers = false;
        for (int r = 0; r < kNumRefs; ++r)
        {
            e[r] = own[r] = effectiveResult (res, m, r, pair);
            if (partner < 0) continue;

            // A linked pair moves as one block: the mean of its two delays, one polarity, and only
            // where both microphones were measured.
            const auto& ps = slots[(size_t) partner];
            other[r] = effectiveResult (ps.results, ps.state, r, ps.numIn.load() == 2 && ! ps.state.monoSource);
            e[r].valid = own[r].valid && other[r].valid;
            e[r].reliable = own[r].reliable && other[r].reliable;
            e[r].delaySamples = 0.5 * (own[r].delaySamples + other[r].delaySamples);
            e[r].confidence = juce::jmin (own[r].confidence, other[r].confidence);
            e[r].polarity = leads ? own[r].polarity : other[r].polarity;
            polarityDiffers |= e[r].valid && own[r].polarity != other[r].polarity;
        }
        int ref = alignmentRef (e, m.alignTo);

        // The delay. By default, the one the analysis found to make the sum with the main array
        // fullest, and only if that is worth it. A peak picked by hand, or a reference chosen by
        // hand, takes over.
        double base = 0.0;
        bool known = false, picked = false;
        for (auto& o : own) picked |= o.locked;
        Benefit best = res.best;
        if (partner >= 0) // a linked pair moves as one block here too
        {
            const auto& pb = slots[(size_t) partner].results.best;
            best.valid = best.valid && pb.valid;
            best.lag = 0.5 * (best.lag + pb.lag);
            best.polarity = leads ? best.polarity : pb.polarity;
            best.gainDb = juce::jmin (best.gainDb, pb.gainDb);
            best.asRecordedDb = juce::jmax (best.asRecordedDb, pb.asRecordedDb);
        }

        if (m.alignTo == AlignTo::best && ! picked && best.valid)
        {
            // For the phase curve, lean on the main microphone whose own measurement sits closest.
            ref = -1;
            for (int r = 0; r < 3; ++r)
                if (res.vs[r].valid && (ref < 0 || std::abs (res.vs[r].delaySamples - best.lag) < std::abs (res.vs[ref].delaySamples - best.lag)))
                    ref = r;
            if (ref >= 0)
            {
                own[ref].shift = res.best.lag - res.vs[ref].delaySamples;
                e[ref].polarity = best.polarity;
            }
            base = -best.lag;
            known = best.worthwhile();
            if (! known)
                a.note = "Lining this mic up would change the sum by " + juce::String (best.gainDb - best.asRecordedDb, 2) + " dB: left as recorded.";
        }
        else if (ref >= 0)
        {
            const bool mean = m.alignTo == AlignTo::average && e[0].valid && e[2].valid;
            base = mean ? -0.5 * (e[0].delaySamples + e[2].delaySamples) : -e[ref].delaySamples;
            known = mean ? e[0].reliable && e[2].reliable : e[ref].reliable;
            if (! known)
                a.note = "Measured, but not sure enough to apply. Analyse a longer passage, or pick a peak in Detail.";
        }
        else if (m.manualPosition)
        {
            base = (0.5 * (rL + rR) - m.sourceDistance) / c * sr;
            known = true;
            a.note = "Delay from the plan (not analysed)";
        }
        else
            a.note = "Not analysed yet";
        a.ref = ref;

        if (m.delayOn && known)
        {
            const double offset = m.offsetMs * 0.001 * sr;
            dL = dR = base + offset;
            if (fromMono && m.panOn && m.timeDifference && e[0].valid && e[0].reliable && e[2].valid && e[2].reliable)
            {
                dL = offset - e[0].delaySamples; // each side lined up on its own main microphone
                dR = offset - e[2].delaySamples;
            }
            if (juce::jmin (dL, dR) < -0.5)
                a.note = "Arrives after the reference: cannot be advanced";
            dL = juce::jmax (0.0, dL);
            dR = juce::jmax (0.0, dR);
        }

        if (m.polarityOn && known && ref >= 0 && e[ref].polarity < 0)
            a.polarity = -1;

        a.suggestedPan = naturalPan (geo, p);
        a.pan = m.panFollowsPlan ? a.suggestedPan : juce::jlimit (-1.0, 1.0, m.pan);
        if (fromMono)
        {
            a.panActive = m.panOn;
            if (m.panOn) panLawGains (a.pan, scene.panLaw, gL, gR);
            else         gL = gR = 0.70710678f;
        }
        else if (pair && numOut == 2 && m.pairPanOn)
        {
            // Two pan pots: the left capsule sits at pan - width, the right one at pan + width.
            // Centre at full width is exactly the untouched pair.
            a.panActive = true;
            panLawGains (juce::jlimit (-1.0, 1.0, a.pan - m.pairWidth), scene.panLaw, gL, crossLR);
            panLawGains (juce::jlimit (-1.0, 1.0, a.pan + m.pairWidth), scene.panLaw, crossRL, gR);
        }

        // The phase left once the delay is set, as it stands against the delay actually in use.
        const bool haveCurve = known && ref >= 0 && res.curve[ref].valid;
        float seen[kFineBands] {}, seenTrust[kFineBands] {}, trim[kFineBands] {};
        for (int b = 0; b < kFineBands; ++b) // a linked pair shares its refinement too
            trim[b] = partner >= 0 ? 0.5f * (res.phaseTrim[b] + slots[(size_t) partner].results.phaseTrim[b]) : res.phaseTrim[b];
        if (partner >= 0 && polarityDiffers)
            a.note = "The two microphones of this pair measure opposite polarities: check the cabling";
        if (haveCurve)
        {
            // Each microphone's curve stands against its own delay; a linked pair keeps only what
            // its two microphones have in common, so both get exactly the same correction.
            const auto standing = [&] (const MicResults& mr, const Effective& mine, int b)
            {
                const double flip = e[ref].polarity != mr.vs[ref].polarity ? kPi : 0.0;
                return std::polar ((double) mr.curve[ref].trust[b],
                                   wrapPhase (mr.curve[ref].phase[b] + flip + 2.0 * kPi * bandFrequency (b) * mine.shift / sr));
            };
            const bool shared = partner >= 0 && slots[(size_t) partner].results.curve[ref].valid;
            std::complex<double> sum {};
            for (int b = 0; b < kFineBands; ++b)
            {
                auto v = standing (res, own[ref], b);
                if (shared) v = 0.5 * (v + standing (slots[(size_t) partner].results, other[ref], b));
                seen[b] = (float) std::arg (v);
                seenTrust[b] = (float) std::abs (v);
                const double f = bandFrequency (b);
                if (f >= m.phaseLowHz && f <= m.phaseHighHz)
                    sum += v;
            }
            if (std::abs (sum) > 1.0) // the single rotation that would best line it up, over the user's range
            {
                a.hasSuggestedRotation = true;
                a.suggestedRotationDeg = std::round (-std::arg (sum) * 180.0 / kPi);
            }
            if (m.alignTo == AlignTo::best && ! picked && best.valid && partner < 0) // judged on the sum itself
            {
                a.hasSuggestedRotation = true;
                a.suggestedRotationDeg = res.best.rotationDeg;
            }
        }

        // Phase: by frequency from the analysis, and/or turned by hand to the degree. The filter's
        // own latency comes out of the alignment delay (plus the session latency in full-resolution
        // mode), so alignment is unchanged.
        const bool byFrequency = m.phaseOn && m.delayOn && haveCurve;
        const bool byHand = std::abs (m.phaseRotationDeg) > 0.01;
        if (byFrequency || byHand)
        {
            const int room = latency + (int) std::floor (juce::jmin (dL, dR));
            const int most = latency > 0 ? latency : juce::nextPowerOfTwo ((int) (1024.0 * sr / 48000.0));
            int half = 32;
            while (half * 2 <= room && half * 2 <= most) half *= 2;
            if (room >= 32)
            {
                firLength = half * 2;
                phase = true;
                if (byFrequency)
                    correctionCurve (seen, seenTrust, trim, m.phaseAmount, m.phaseLowHz, m.phaseHighHz, a.correction);
                for (auto& v : a.correction)
                    v -= (float) (m.phaseRotationDeg * kPi / 180.0); // a positive rotation advances the phase
            }
            else
                a.note = "Too little delay to shape the phase in Live mode: use Full resolution (Session)";
        }
    }

    a.phaseTaps = firLength;
    if (phase && s.sink != nullptr)
    {
        juce::int64 signature = firLength + (juce::int64) sr * 131;
        for (float v : a.correction)
            signature = signature * 31 + (juce::int64) std::lround (v * 20000.0f);
        if (signature != s.firSignature)
        {
            std::vector<float> taps;
            designPhaseFir (a.correction, sr, firLength, taps);
            s.sink (taps);
            s.firSignature = signature;
            s.firLength = firLength;
        }
    }

    a.delayLms = sr > 0.0 ? 1000.0 * dL / sr : 0.0;
    a.delayRms = sr > 0.0 ? 1000.0 * dR / sr : 0.0;
    a.gainL = gL;
    a.gainR = gR;
    a.crossLR = crossLR;
    a.crossRL = crossRL;
    s.applied = a;
    s.alignL = dL;
    s.alignR = dR;

    s.live.delayL = (float) juce::jmax (0.0, latency + dL - firLength / 2);
    s.live.delayR = (float) juce::jmax (0.0, latency + dR - firLength / 2);
    s.live.bypassDelay = latency;
    s.live.gainL = gL * (float) a.polarity;
    s.live.gainR = gR * (float) a.polarity;
    s.live.crossLR = crossLR * (float) a.polarity;
    s.live.crossRL = crossRL * (float) a.polarity;
    s.live.bypassGainL = bypassL;
    s.live.bypassGainR = bypassR;
    s.live.fromMono = fromMono;
    s.live.phaseOn = phase;
}

/** Puts a microphone on the plan where the measured delays say its source is. */
void Hub::place (int slot)
{
    auto& s = slots[(size_t) slot];
    auto& m = s.state;
    const double sr = s.sampleRate.load();
    if (isMain (m.role) || sr <= 0.0)
        return;

    int arrayGroup = 0;
    refsForGroup (m.group, &arrayGroup);
    const auto& arr = scene.arrays[arrayGroup];
    const auto geo = arrayGeometry (arr);
    const double c = speedOfSound (scene.tempC);
    const double dz = arr.height - m.z;
    const auto horizontal = [&] (const Effective& r)
    {
        const double d = juce::jmax (0.1, c * -r.delaySamples / sr + m.sourceDistance);
        return std::sqrt (juce::jmax (0.01, d * d - dz * dz));
    };

    const bool pair = s.numIn.load() == 2 && ! m.monoSource;
    Effective v[3];
    for (int r = 0; r < 3; ++r)
    {
        v[r] = effectiveResult (s.results, m, r, pair);
        v[r].valid = v[r].valid && v[r].reliable;
    }
    Vec2 p { m.x, m.y };
    if (v[0].valid && v[2].valid)   p = locate (geo, horizontal (v[0]), horizontal (v[2]));
    else if (v[1].valid)          { const double r = horizontal (v[1]); p = { geo.pos[1].x + geo.forward.x * r, geo.pos[1].y + geo.forward.y * r }; }
    else if (v[0].valid)          { const double r = horizontal (v[0]); p = { geo.pos[0].x + geo.forward.x * r, geo.pos[0].y + geo.forward.y * r }; }
    else if (v[2].valid)          { const double r = horizontal (v[2]); p = { geo.pos[2].x + geo.forward.x * r, geo.pos[2].y + geo.forward.y * r }; }
    else return;

    m.x = p.x;
    m.y = p.y;
    m.manualPosition = false;
}

void Hub::placeFromAnalysis (int slot)
{
    if (slot < 0) return;
    const juce::ScopedLock l (stateLock);
    place (slot);
    recompute (slot);
    ++changes;
}

//==============================================================================
void Hub::startAnalysis()
{
    analysing = true;
    ++changes;
}

void Hub::stopAnalysis()
{
    if (! analysing.exchange (false))
        return;

    const juce::ScopedLock al (analysisLock); // let a running pass finish
    for (auto& t : targets)                   // then read every microphone one last time
        if (slots[(size_t) t.slot].active.load())
            readout (t, lastRate);

    const juce::ScopedLock l (stateLock);
    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (s.active.load() && ! s.state.manualPosition && ! s.state.measureLocked)
            place (i);
    }
    recomputeAll();
    ++changes;
}

void Hub::resetAnalysis()
{
    analysing = false;
    refinePassesLeft = 0;
    resetRequested = true;
    const juce::ScopedLock al (analysisLock);
    const juce::ScopedLock l (stateLock);
    for (auto& s : slots)
        if (! s.state.measureLocked) // locked measurements survive a reset
            s.results = {};
    detailData = {};
    analysedTime = 0.0;
    recomputeAll();
    ++changes;
}

SearchRange Hub::rangeFor (Role role, double sr) const
{
    // Nothing further than 60 m; a spot hears its own instrument before the main array does.
    const double far = 60.0 / 343.0 * sr;
    return { -far, role == Role::spot ? 0.003 * sr : far };
}

bool Hub::readFrame (Slot& s, int channel, juce::int64 start, int length, float* dest)
{
    const juce::ScopedLock l (s.ringLock);
    if (s.ringSize == 0 || ! s.playing.load())
        return false;

    const auto end = s.writeEnd.load();
    if (s.runStart.load() > start || end < start + length || end - start > s.ringSize - s.blockSize.load())
        return false;

    const int mask = s.ringSize - 1;
    const auto* ring = s.ring[channel].get();
    for (int i = 0; i < length; ++i)
        dest[i] = ring[(size_t) ((start + i) & mask)];

    // The audio thread may have lapped us while copying.
    return s.runStart.load() <= start && s.writeEnd.load() - start <= s.ringSize;
}

void Hub::run()
{
    while (! threadShouldExit())
    {
        analyse();
        detail();
        check();
        wait (100);
    }
}

bool Hub::prepareFrame (double sr)
{
    const int wanted = juce::nextPowerOfTwo ((int) (sr * 0.5));
    if (wanted == frameSize)
        return false;
    frameSize = wanted;
    fft = std::make_unique<Fft> (frameSize);
    tgtSpectrum = std::make_unique<FrameSpectrum> (frameSize);
    refSpectra.clear();
    frame.assign ((size_t) frameSize, 0.0f);
    for (auto& s : slots)
        s.resetEstimators (frameSize);
    framePos = -1;
    checkPos = -1;
    return true;
}

void Hub::analyse()
{
    const juce::ScopedLock al (analysisLock);

    if (resetRequested.exchange (false))
    {
        for (auto& s : slots)
            s.resetEstimators (frameSize);
        framePos = -1;
    }

    if (! analysing.load())
        return;

    // The plan: who is measured against whom.
    int clock = -1;
    {
        const juce::ScopedLock l (stateLock);
        targets.clear();
        for (int i = 0; i < maxSlots; ++i)
        {
            auto& s = slots[(size_t) i];
            if (! s.active.load()) continue;
            if (clock < 0 && s.playing.load() && s.sampleRate.load() > 0.0) clock = i;
            if (! isMain (s.state.role) && ! s.state.measureLocked)
                targets.push_back ({ i, refsForGroup (s.state.group), s.numIn.load() == 2 && ! s.state.monoSource, s.state.role });
        }
    }
    if (clock < 0)
        return;

    auto& ck = slots[(size_t) clock];
    const double sr = ck.sampleRate.load();
    prepareFrame (sr);
    lastRate = sr;

    // Spectrum of a reference at the current frame, computed once and shared by every target.
    std::function<const FrameSpectrum* (const RefSet&, int)> refSpectrum = [&] (const RefSet& refs, int r) -> const FrameSpectrum*
    {
        const int key = refs.key (r);
        for (auto& rs : refSpectra)
            if (rs.key == key) return rs.ok ? rs.spectrum.get() : nullptr;

        const FrameSpectrum* parts[3];
        int numParts = 0;
        bool ok = true;
        if (r == 3) // the sum of the main array: add up its members
            for (int q = 0; q < 3 && ok; ++q)
                if (refs.slot[q] >= 0)
                {
                    parts[numParts] = refSpectrum (refs, q);
                    ok = parts[numParts++] != nullptr;
                }

        RefSpectrum* entry = nullptr;
        for (auto& rs : refSpectra)
            if (rs.key < 0) { entry = &rs; break; }
        if (entry == nullptr)
        {
            refSpectra.emplace_back();
            entry = &refSpectra.back();
            entry->spectrum = std::make_unique<FrameSpectrum> (frameSize);
        }
        entry->key = key;
        if (r == 3)
        {
            entry->ok = ok && numParts > 0;
            if (entry->ok) entry->spectrum->setSum (parts, numParts);
        }
        else
        {
            entry->ok = readFrame (slots[(size_t) refs.slot[r]], refs.channel[r], framePos, frameSize, frame.data());
            if (entry->ok) entry->spectrum->compute (*fft, frame.data());
        }
        return entry->ok ? entry->spectrum.get() : nullptr;
    };

    // Leave a little room so instances processed later in the host cycle have caught up.
    const auto guard = (juce::int64) (sr * 0.1);
    const auto began = juce::Time::getMillisecondCounterHiRes();
    bool changed = false;

    for (int n = 0; n < 4 && ! threadShouldExit() && analysing.load(); ++n)
    {
        const auto end = ck.writeEnd.load() - guard;
        const auto start = ck.runStart.load();
        if (framePos < start || framePos > end || end - framePos > ck.ringSize - frameSize * 2)
            framePos = juce::jmax (start, end - frameSize);
        if (framePos + frameSize > end)
            break;

        for (auto& rs : refSpectra) rs.key = -1;

        for (auto& t : targets)
        {
            auto& s = slots[(size_t) t.slot];
            const auto has = [&] (int r) { return r == 3 ? t.refs.slot[3] == -2 : t.refs.slot[r] >= 0 && t.refs.slot[r] != t.slot; };

            bool haveTarget = false;
            for (int r = 0; r < kNumRefs; ++r)
            {
                if (! has (r)) continue;
                const int key = t.refs.key (r);
                if (s.estimatorRef[r] != key) { s.estimator[r].reset (frameSize); s.estimatorRight[r].reset (frameSize); s.estimatorRef[r] = key; }
                auto* ref = refSpectrum (t.refs, r);
                if (ref == nullptr) continue;
                if (! haveTarget)
                {
                    if (! readFrame (s, 0, framePos, frameSize, frame.data())) break;
                    tgtSpectrum->compute (*fft, frame.data());
                    haveTarget = true;
                }
                changed |= s.estimator[r].accumulate (*ref, *tgtSpectrum);
            }

            // Second channel of a stereo spot, against the same references.
            if (t.pair && haveTarget && readFrame (s, 1, framePos, frameSize, frame.data()))
            {
                tgtSpectrum->compute (*fft, frame.data());
                for (int r = 0; r < kNumRefs; ++r)
                    if (has (r))
                        if (auto* ref = refSpectrum (t.refs, r))
                            s.estimatorRight[r].accumulate (*ref, *tgtSpectrum);
            }
        }
        framePos += frameSize / 2;
        if (juce::Time::getMillisecondCounterHiRes() - began > 150.0)
            break;
    }

    if (! changed || targets.empty())
        return;

    // Bring the figures up to date, a few microphones at a time so the thread stays responsive.
    const auto readBegan = juce::Time::getMillisecondCounterHiRes();
    int mostFrames = 0;
    for (size_t i = 0; i < targets.size(); ++i)
    {
        readCursor = (readCursor + 1) % (int) targets.size();
        readout (targets[(size_t) readCursor], sr);
        if (juce::Time::getMillisecondCounterHiRes() - readBegan > 80.0)
            break;
    }
    for (auto& t : targets)
        for (auto& e : slots[(size_t) t.slot].estimator)
            mostFrames = juce::jmax (mostFrames, e.frames());
    analysedTime = mostFrames * (frameSize / 2) / sr;
    ++changes;
}

/** Turns one microphone's accumulated spectra into delays, candidates and phase curves. */
void Hub::readout (const Target& t, double sr)
{
    auto& s = slots[(size_t) t.slot];
    if (fft == nullptr || sr <= 0.0)
        return;

    int total = 0;
    for (int r = 0; r < kNumRefs; ++r)
        total += s.estimator[r].frames() + s.estimatorRight[r].frames();
    if (total == s.readFrames)
        return;
    s.readFrames = total;

    const auto range = rangeFor (t.role, sr);
    bool fresh[kNumRefs] {};
    DelayResult left[kNumRefs], right[kNumRefs];
    PhaseCurve curve[kNumRefs];

    // 1. Candidate delays against every reference.
    for (int r = 0; r < kNumRefs; ++r)
    {
        if (s.estimatorRef[r] < 0 || s.estimator[r].frames() == 0)
            continue;
        left[r] = s.estimator[r].estimate (*fft, sr, scratch, range);
        if (t.pair && s.estimatorRight[r].frames() > 0)
            right[r] = s.estimatorRight[r].estimate (*fft, sr, scratch, range);
        fresh[r] = true;
    }

    // 2. Polarity belongs to the microphone, not to the reference it is compared with.
    //    Where a weaker measurement disagrees with the clearest one, prefer a candidate
    //    of the agreeing polarity if there is one nearly as strong.
    const auto agree = [] (DelayResult* results)
    {
        int clearest = -1;
        for (int r = 0; r < kNumRefs; ++r)
            if (results[r].valid && results[r].reliable && (clearest < 0 || results[r].snr > results[clearest].snr))
                clearest = r;
        if (clearest < 0) return;
        for (int r = 0; r < kNumRefs; ++r)
        {
            auto& d = results[r];
            if (! d.valid || d.polarity == results[clearest].polarity) continue;
            for (int p = 1; p < d.numPeaks; ++p)
                if ((d.peaks[p].value < 0.0f ? -1 : 1) == results[clearest].polarity && std::abs (d.peaks[p].value) >= 0.7f * std::abs (d.peaks[0].value))
                {
                    std::swap (d.peaks[0], d.peaks[p]);
                    d.delaySamples = d.peaks[0].lag;
                    d.polarity = results[clearest].polarity;
                    break;
                }
        }
    };
    agree (left);
    agree (right);

    // 3. Sharpen the chosen delay and read the phase left over.
    for (int r = 0; r < kNumRefs; ++r)
    {
        if (! fresh[r]) continue;
        auto& d = left[r];
        if (d.valid)
        {
            d.delaySamples = s.estimator[r].refineDelay (sr, d.delaySamples, d.polarity);
            d.peaks[0].lag = d.delaySamples;
            curve[r] = s.estimator[r].phaseCurve (sr, d.delaySamples, d.polarity);
        }
        auto& dr = right[r];
        if (dr.valid)
        {
            dr.delaySamples = s.estimatorRight[r].refineDelay (sr, dr.delaySamples, dr.polarity);
            dr.peaks[0].lag = dr.delaySamples;

            // A stereo spot gets one filter for both capsules, so its own image is untouched:
            // keep only the phase the two capsules have in common, each against its own delay.
            if (curve[r].valid)
            {
                const auto other = s.estimatorRight[r].phaseCurve (sr, dr.delaySamples, dr.polarity);
                for (int b = 0; b < kFineBands; ++b)
                {
                    const auto both = 0.5 * (std::polar ((double) curve[r].trust[b], (double) curve[r].phase[b])
                                             + std::polar ((double) other.trust[b], (double) other.phase[b]));
                    curve[r].phase[b] = (float) std::arg (both);
                    curve[r].trust[b] = (float) std::abs (both);
                }
            }
        }
    }

    // 4. Which candidate makes the sum with the main array fullest, and is it worth it?
    Benefit best;
    findBest (t, sr, left, right, best);

    const juce::ScopedLock l (stateLock);
    if (best.valid)
        s.results.best = best;
    for (int r = 0; r < kNumRefs; ++r)
        if (fresh[r])
        {
            s.results.vs[r] = left[r];
            s.results.vsRight[r] = right[r];
            s.results.curve[r] = curve[r];
            s.results.zones[r] = zoneDelays (curve[r]);
        }
}

/** Tries every candidate delay on the yardstick that matters: how much fuller the sum of this
    microphone and the main array gets. A mono microphone is judged against each main microphone
    it will be mixed with; a stereo spot, capsule by capsule, left against main L and right against
    main R, both moved by the same amount. */
void Hub::findBest (const Target& t, double sr, DelayResult* left, DelayResult* right, Benefit& best)
{
    auto& s = slots[(size_t) t.slot];
    std::vector<const DelayEstimator*> judges;
    const auto use = [&] (const DelayEstimator& e) { if (e.frames() > 0) judges.push_back (&e); };
    if (t.pair)
    {
        use (s.estimator[0]);       // left capsule with main L
        use (s.estimatorRight[2]);  // right capsule with main R
        use (s.estimator[1]);       // both with main C
        use (s.estimatorRight[1]);
    }
    if (judges.empty())
        for (int r = 0; r < 3; ++r)
            use (s.estimator[r]);
    if (judges.empty())
        return;

    const auto score = [&] (double lag, int polarity, double rotation = 0.0, bool limit = false)
    {
        double total = 0.0;
        for (auto* j : judges) total += j->sumGain (sr, lag, polarity, rotation, limit);
        return total / (double) judges.size();
    };

    best.valid = true;
    best.asRecordedDb = best.gainDb = score (0.0, 1);
    for (auto* results : { left, right })
        for (int r = 0; r < kNumRefs; ++r)
            for (int p = 0; p < juce::jmin (4, results[r].numPeaks); ++p)
            {
                auto& peak = results[r].peaks[p];
                const int polarity = peak.value < 0.0f ? -1 : 1;
                const double gain = score (peak.lag, polarity);
                peak.gainDb = (float) gain;
                if (gain > best.gainDb) { best.gainDb = gain; best.lag = peak.lag; best.polarity = polarity; }
            }

    // The one rotation that would add most, and the ceiling a perfect phase correction would reach.
    std::complex<double> direction {};
    for (auto* j : judges) direction += j->sumVector (sr, best.lag, best.polarity);
    const double rotation = std::abs (direction) > 0.0 ? -std::arg (direction) : 0.0;
    best.rotationDeg = std::round (rotation * 180.0 / kPi);
    best.withRotationDb = score (best.lag, best.polarity, best.rotationDeg * kPi / 180.0);
    best.limitDb = score (best.lag, best.polarity, 0.0, true);
}

MicDetail Hub::getDetail() const
{
    const juce::ScopedLock l (stateLock);
    return detailData;
}

/** The correlation curve of the microphone open in the Detail view. */
void Hub::detail()
{
    const juce::ScopedLock al (analysisLock);
    const int slot = detailSlot.load(), ref = juce::jlimit (0, kNumRefs - 1, detailRef.load());
    if (slot < 0 || fft == nullptr || lastRate <= 0.0 || ! slots[(size_t) slot].active.load())
        return;

    auto& s = slots[(size_t) slot];
    const int frames = s.estimatorRef[ref] >= 0 ? s.estimator[ref].frames() : 0;
    if (detailDone[0] == slot && detailDone[1] == ref && detailDone[2] == frames)
        return;
    detailDone[0] = slot; detailDone[1] = ref; detailDone[2] = frames;

    MicDetail d;
    d.slot = slot;
    d.ref = ref;
    if (frames > 0)
    {
        Role role;
        { const juce::ScopedLock l (stateLock); role = s.state.role; }
        d.available = s.estimator[ref].estimate (*fft, lastRate, scratch, rangeFor (role, lastRate), &d.curve).valid;
    }
    const juce::ScopedLock l (stateLock);
    detailData = d;
}

//==============================================================================
CheckData Hub::getCheck() const
{
    const juce::ScopedLock l (stateLock);
    return checkData;
}

void Hub::startRefine (int passes)
{
    refinePassesTotal = passes;
    refinePassesLeft = passes;
    ++changes;
}

void Hub::clearRefine()
{
    refinePassesLeft = 0;
    const juce::ScopedLock al (analysisLock);
    const juce::ScopedLock l (stateLock);
    for (auto& s : slots)
        std::fill (std::begin (s.results.phaseTrim), std::end (s.results.phaseTrim), 0.0f);
    recomputeAll();
    ++changes;
}

/** Adds the microphones up twice on a stereo bus, as they arrive and as they
    leave the plugins, so the user can see whether the corrections make the sum
    fuller or thinner. While refining, it also measures each microphone against
    everything else on the bus. */
void Hub::check()
{
    const juce::ScopedLock al (analysisLock);
    const bool refining = refinePassesLeft.load() > 0;
    if (! checkActive.load() && ! refining)
    {
        if (checkPrimed) { checkPrimed = false; const juce::ScopedLock l (stateLock); checkData = {}; }
        return;
    }

    struct Entry
    {
        int slot; bool main, stereo, monoOut, phase, refine;
        double dL, dR; float gL, gR, bL, bR, xLR, xRL;
        float correction[kFineBands];
    };
    std::vector<Entry> entries;
    const int focus = refining ? -1 : checkFocus.load();
    const bool bypass = bypassAll.load();
    bool anyTrim = false;
    int clock = -1;
    {
        const juce::ScopedLock l (stateLock);
        for (int i = 0; i < maxSlots; ++i)
        {
            auto& s = slots[(size_t) i];
            if (! s.active.load() || s.numIn.load() > 2 || s.numOut.load() > 2) continue;
            if (clock < 0 && s.playing.load() && s.sampleRate.load() > 0.0) clock = i;
            for (float t : s.results.phaseTrim) anyTrim |= std::abs (t) > 0.0f;
            const bool main = isMain (s.state.role);
            if (focus >= 0 && ! main && i != focus) continue;
            Entry e;
            e.slot = i; e.main = main;
            e.monoOut = s.numOut.load() == 1;
            e.stereo = s.numIn.load() == 2 && ! s.live.fromMono.load();
            e.phase = s.live.phaseOn.load();
            e.refine = refining && ! main && e.phase && ! s.state.measureLocked;
            e.xLR = s.live.crossLR.load(); e.xRL = s.live.crossRL.load();
            e.dL = s.alignL; e.dR = s.alignR;
            e.gL = s.live.gainL.load();  e.gR = s.live.gainR.load();
            e.bL = s.live.bypassGainL.load(); e.bR = s.live.bypassGainR.load();
            std::copy (std::begin (s.applied.correction), std::end (s.applied.correction), e.correction);
            entries.push_back (e);
        }
    }
    if (clock < 0 || entries.empty())
        return;

    auto& ck = slots[(size_t) clock];
    const double sr = ck.sampleRate.load();
    prepareFrame (sr);
    lastRate = sr;
    const int n = frameSize, bins = n / 2 + 1;
    const auto pos = ck.writeEnd.load() - (juce::int64) (sr * 0.1) - n;
    if (pos < ck.runStart.load() || (checkPos >= 0 && std::abs (pos - checkPos) < n / 2))
        return;
    checkPos = pos;

    for (int bus = 0; bus < 2; ++bus)
    {
        for (auto* v : { &busBefore[bus], &busAfter[bus], &busMain[bus], &busFocusBefore[bus], &busFocusAfter[bus] })
            v->assign ((size_t) bins, {});
        if (refining) micBus[bus].resize (entries.size());
    }
    if (refining && refine.size() != (size_t) maxSlots)
    {
        refine.assign ((size_t) maxSlots, {});
        refineFrames = 0;
    }

    for (size_t index = 0; index < entries.size(); ++index)
    {
        auto& e = entries[index];
        if (e.refine)
            for (int bus = 0; bus < 2; ++bus) micBus[bus][index].assign ((size_t) bins, {});

        for (int ch = 0; ch < (e.stereo ? 2 : 1); ++ch)
        {
            if (! readFrame (slots[(size_t) e.slot], ch, pos, n, frame.data())) continue;
            tgtSpectrum->compute (*fft, frame.data());
            const auto& x = tgtSpectrum->bins();

            // What this channel sends to the left and right buses, bypassed and corrected.
            // A mono track is taken as panned to the centre by the mixer.
            double beforeGain[2], afterGain[2];
            if (e.monoOut)       { beforeGain[0] = beforeGain[1] = 0.70710678 * e.bL; afterGain[0] = afterGain[1] = 0.70710678 * e.gL; }
            else if (e.stereo)   { beforeGain[0] = ch == 0 ? e.bL : 0.0; beforeGain[1] = ch == 1 ? e.bR : 0.0;
                                   afterGain[0] = ch == 0 ? e.gL : e.xRL; afterGain[1] = ch == 1 ? e.gR : e.xLR; }
            else                 { beforeGain[0] = e.bL; beforeGain[1] = e.bR; afterGain[0] = e.gL; afterGain[1] = e.gR; }

            const std::complex<double> step[2] = { std::polar (1.0, -2.0 * kPi * e.dL / n),
                                                   std::polar (1.0, -2.0 * kPi * (e.monoOut ? e.dL : e.dR) / n) };
            std::complex<double> turn[2] = { { 1.0, 0.0 }, { 1.0, 0.0 } };
            for (int k = 0; k < bins; ++k)
            {
                const std::complex<double> xk (x[(size_t) k]);
                const auto shaped = e.phase && ! bypass ? xk * correctionResponse (e.correction, k * sr / n) : xk;
                for (int bus = 0; bus < 2; ++bus)
                {
                    const auto before = std::complex<float> (xk * beforeGain[bus]);
                    const auto after = bypass ? before : std::complex<float> (shaped * afterGain[bus] * turn[bus]);
                    busBefore[bus][(size_t) k] += before;
                    busAfter[bus][(size_t) k] += after;
                    if (e.main)                 busMain[bus][(size_t) k] += before;
                    else if (e.slot == focus) { busFocusBefore[bus][(size_t) k] += before; busFocusAfter[bus][(size_t) k] += after; }
                    if (e.refine)               micBus[bus][index][(size_t) k] += after;
                    turn[bus] *= step[bus];
                }
            }
        }
    }

    // Refining: how each phase-corrected microphone sits against everything else on each bus.
    if (refining)
    {
        for (size_t index = 0; index < entries.size(); ++index)
        {
            if (! entries[index].refine) continue;
            auto& acc = refine[(size_t) entries[index].slot];
            for (int k = 1; k < bins - 1; ++k)
            {
                const int b = (int) std::lround (bandPosition (k * sr / n));
                if (b < 0 || b >= kFineBands) continue;
                for (int bus = 0; bus < 2; ++bus)
                {
                    const std::complex<double> mine (micBus[bus][index][(size_t) k]);
                    const auto rest = std::complex<double> (busAfter[bus][(size_t) k]) - mine;
                    acc.cross[b] += rest * std::conj (mine);
                    acc.own[b] += std::norm (mine);
                    acc.others[b] += std::norm (rest);
                }
            }
        }
        if (++refineFrames >= 14) // about five seconds of music per pass
        {
            const juce::ScopedLock l (stateLock);
            for (auto& e : entries)
            {
                if (! e.refine) continue;
                auto& acc = refine[(size_t) e.slot];
                auto& trim = slots[(size_t) e.slot].results.phaseTrim;
                for (int b = 0; b < kFineBands; ++b)
                {
                    const double den = std::sqrt (acc.own[b] * acc.others[b]);
                    if (den <= 0.0) continue;
                    const double binsInBand = juce::jmax (1.0, bandFrequency (b) * 0.0578 * n / sr); // a 1/12 octave is 5.78 % wide
                    const double trust = phaseTrust (std::abs (acc.cross[b]) / den, refineFrames * binsInBand / 3.0);
                    // The rest of the bus leads this microphone by arg(cross): turn the microphone that way, gently.
                    trim[b] = (float) wrapPhase (trim[b] - 0.6 * trust * std::arg (acc.cross[b]));
                }
            }
            refine.clear();
            refineFrames = 0;
            --refinePassesLeft;
            anyTrim = true;
            recomputeAll();
            ++changes;
        }
    }

    // Fold into display bands and smooth over a few frames.
    if (checkedFocus != focus || ! checkPrimed)
    {
        for (auto& b : bandBefore) b = 0.0;
        for (auto& b : bandAfter) b = 0.0;
        for (auto& c : corrState) c = 0.0;
        checkedFocus = focus;
    }
    double pb[CheckData::numBands] {}, pa[CheckData::numBands] {}, corr[5] {};
    const double span = std::log (CheckData::high / CheckData::low);
    for (int k = 1; k < bins - 1; ++k)
    {
        const double f = k * sr / n;
        if (f < CheckData::low || f >= CheckData::high) continue;
        const int b = juce::jlimit (0, CheckData::numBands - 1, (int) (std::log (f / CheckData::low) / span * CheckData::numBands));
        for (int bus = 0; bus < 2; ++bus)
        {
            const std::complex<double> mainHere (busMain[bus][(size_t) k]);
            pb[b] += std::norm (busBefore[bus][(size_t) k]);
            pa[b] += std::norm (busAfter[bus][(size_t) k]);
            corr[0] += (mainHere * std::conj (std::complex<double> (busFocusBefore[bus][(size_t) k]))).real();
            corr[1] += (mainHere * std::conj (std::complex<double> (busFocusAfter[bus][(size_t) k]))).real();
            corr[2] += std::norm (mainHere);
            corr[3] += std::norm (busFocusBefore[bus][(size_t) k]);
            corr[4] += std::norm (busFocusAfter[bus][(size_t) k]);
        }
    }
    const double mix = checkPrimed && bandBefore[CheckData::numBands / 2] > 0.0 ? 0.25 : 1.0;
    for (int b = 0; b < CheckData::numBands; ++b)
    {
        bandBefore[b] += mix * (pb[b] - bandBefore[b]);
        bandAfter[b] += mix * (pa[b] - bandAfter[b]);
    }
    for (int i = 0; i < 5; ++i) corrState[i] += mix * (corr[i] - corrState[i]);
    checkPrimed = true;

    CheckData d;
    d.valid = true;
    double tot[2][4] {};
    for (int b = 0; b < CheckData::numBands; ++b)
    {
        const double f = CheckData::low * std::exp (span * (b + 0.5) / CheckData::numBands);
        const int zone = f < 250.0 ? 1 : f < 2000.0 ? 2 : 3;
        tot[0][0] += bandBefore[b]; tot[1][0] += bandAfter[b];
        tot[0][zone] += bandBefore[b]; tot[1][zone] += bandAfter[b];
        d.before[b] = (float) (10.0 * std::log10 (bandBefore[b] + 1e-12));
        d.after[b] = (float) (10.0 * std::log10 (bandAfter[b] + 1e-12));
    }
    const auto gain = [&] (int z) { return 10.0 * std::log10 ((tot[1][z] + 1e-12) / (tot[0][z] + 1e-12)); };
    d.gainAll = gain (0); d.gainLow = gain (1); d.gainMid = gain (2); d.gainHigh = gain (3);
    if (focus >= 0 && corrState[2] > 0.0 && corrState[3] > 0.0)
    {
        d.hasCorrelation = true;
        d.correlationBefore = corrState[0] / std::sqrt (corrState[2] * corrState[3]);
        d.correlationAfter = corrState[1] / std::sqrt (corrState[2] * juce::jmax (1e-30, corrState[4]));
    }
    d.refinePasses = refinePassesTotal.load();
    d.refinePass = refinePassesLeft.load() > 0 ? d.refinePasses - refinePassesLeft.load() + 1 : 0;
    d.refined = anyTrim;

    // For an equal-loudness comparison with Bypass all: undo the level the corrections add.
    if (focus < 0 && ! bypass)
        matchGain = (float) juce::jlimit (0.25, 2.0, std::pow (10.0, -d.gainAll / 20.0));

    const juce::ScopedLock l (stateLock);
    checkData = d;
}

//==============================================================================
namespace
{
juce::String pack (const float* v, int n)
{
    juce::StringArray parts;
    for (int i = 0; i < n; ++i) parts.add (juce::String (v[i], 4));
    return parts.joinIntoString (" ");
}

void unpack (const juce::String& text, float* v, int n)
{
    const auto parts = juce::StringArray::fromTokens (text, " ", "");
    for (int i = 0; i < n; ++i) v[i] = i < parts.size() ? parts[i].getFloatValue() : 0.0f;
}

void saveResult (juce::ValueTree& t, const juce::String& k, const DelayResult& v)
{
    t.setProperty ("valid" + k, v.valid, nullptr);         t.setProperty ("delay" + k, v.delaySamples, nullptr);
    t.setProperty ("polarity" + k, v.polarity, nullptr);   t.setProperty ("confidence" + k, v.confidence, nullptr);
    t.setProperty ("frames" + k, v.frames, nullptr);       t.setProperty ("reliable" + k, v.reliable, nullptr);
    t.setProperty ("snr" + k, v.snr, nullptr);
    juce::StringArray peaks;
    for (int p = 0; p < v.numPeaks; ++p) { peaks.add (juce::String (v.peaks[p].lag, 3)); peaks.add (juce::String (v.peaks[p].value, 4)); }
    t.setProperty ("peaks" + k, peaks.joinIntoString (" "), nullptr);
}

DelayResult loadResult (const juce::ValueTree& t, const juce::String& k, bool oldFormat)
{
    DelayResult v;
    v.valid = t.getProperty ("valid" + k, false);
    v.delaySamples = t.getProperty ("delay" + k, 0.0);
    v.polarity = (int) t.getProperty ("polarity" + k, 1) < 0 ? -1 : 1;
    v.confidence = t.getProperty ("confidence" + k, 0.0);
    v.frames = t.getProperty ("frames" + k, 0);
    v.reliable = t.getProperty ("reliable" + k, oldFormat && v.valid);
    v.snr = t.getProperty ("snr" + k, 0.0);
    const auto peaks = juce::StringArray::fromTokens (t.getProperty ("peaks" + k).toString(), " ", "");
    for (int p = 0; p < kMaxPeaks && p * 2 + 1 < peaks.size(); ++p)
        v.peaks[v.numPeaks++] = { peaks[p * 2].getDoubleValue(), peaks[p * 2 + 1].getFloatValue() };
    if (v.valid && v.numPeaks == 0)
        v.peaks[v.numPeaks++] = { v.delaySamples, (float) v.polarity };
    return v;
}
}

juce::ValueTree Hub::saveInstance (int slot) const
{
    juce::ValueTree t ("ArrayLab");
    if (slot < 0) return t;
    const juce::ScopedLock l (stateLock);
    const auto& s = slots[(size_t) slot];
    const auto& m = s.state;

    juce::ValueTree mic ("Mic");
    mic.setProperty ("name", m.name, nullptr);        mic.setProperty ("hostNamed", m.hostNamed, nullptr);
    mic.setProperty ("role", (int) m.role, nullptr);  mic.setProperty ("group", m.group, nullptr);
    mic.setProperty ("pattern", (int) m.pattern, nullptr);
    mic.setProperty ("x", m.x, nullptr); mic.setProperty ("y", m.y, nullptr); mic.setProperty ("z", m.z, nullptr);
    mic.setProperty ("angle", m.angleDeg, nullptr);   mic.setProperty ("sourceDistance", m.sourceDistance, nullptr);
    mic.setProperty ("manualPosition", m.manualPosition, nullptr);
    mic.setProperty ("alignTo", (int) m.alignTo, nullptr);
    mic.setProperty ("decision", 1, nullptr);
    mic.setProperty ("delayOn", m.delayOn, nullptr);  mic.setProperty ("polarityOn", m.polarityOn, nullptr);
    mic.setProperty ("phaseOn", m.phaseOn, nullptr);  mic.setProperty ("offsetMs", m.offsetMs, nullptr);
    mic.setProperty ("phaseAmount", m.phaseAmount, nullptr); mic.setProperty ("phaseRotation", m.phaseRotationDeg, nullptr);
    mic.setProperty ("phaseLow", m.phaseLowHz, nullptr); mic.setProperty ("phaseHigh", m.phaseHighHz, nullptr);
    for (int r = 0; r < kNumRefs; ++r)
    {
        mic.setProperty ("peakLocked" + juce::String (r), m.peakLocked[r], nullptr);
        mic.setProperty ("peakLag" + juce::String (r), m.peakLag[r], nullptr);
    }
    mic.setProperty ("panOn", m.panOn, nullptr);      mic.setProperty ("panFollowsPlan", m.panFollowsPlan, nullptr);
    mic.setProperty ("pan", m.pan, nullptr);          mic.setProperty ("timeDifference", m.timeDifference, nullptr);
    mic.setProperty ("monoSource", m.monoSource, nullptr);
    mic.setProperty ("pairPanOn", m.pairPanOn, nullptr); mic.setProperty ("pairWidth", m.pairWidth, nullptr);
    mic.setProperty ("measureLocked", m.measureLocked, nullptr);
    mic.setProperty ("pairSpacing", m.pairSpacingCm, nullptr); mic.setProperty ("pairAngle", m.pairAngleDeg, nullptr);
    mic.setProperty ("colour", m.colour, nullptr);
    mic.setProperty ("uid", m.uid, nullptr);       mic.setProperty ("partner", m.partner, nullptr);
    t.addChild (mic, -1, nullptr);

    juce::ValueTree res ("Results");
    res.setProperty ("format", 2, nullptr);
    for (int r = 0; r < kNumRefs; ++r)
    {
        const auto k = juce::String (r);
        saveResult (res, k, s.results.vs[r]);
        saveResult (res, "Right" + k, s.results.vsRight[r]);
        const auto& c = s.results.curve[r];
        res.setProperty ("curveValid" + k, c.valid, nullptr);
        if (c.valid)
        {
            res.setProperty ("phase" + k, pack (c.phase, kFineBands), nullptr);
            res.setProperty ("trust" + k, pack (c.trust, kFineBands), nullptr);
        }
    }
    res.setProperty ("trim", pack (s.results.phaseTrim, kFineBands), nullptr);
    {
        const auto& b = s.results.best;
        const float values[8] = { b.valid ? 1.0f : 0.0f, (float) b.lag, (float) b.polarity, (float) b.asRecordedDb, (float) b.gainDb,
                                  (float) b.withRotationDb, (float) b.limitDb, (float) b.rotationDeg };
        res.setProperty ("best", pack (values, 8), nullptr);
        for (int r = 0; r < kNumRefs; ++r)
        {
            float gains[kMaxPeaks] {};
            for (int p = 0; p < s.results.vs[r].numPeaks; ++p) gains[p] = s.results.vs[r].peaks[p].gainDb;
            res.setProperty ("peakGains" + juce::String (r), pack (gains, kMaxPeaks), nullptr);
        }
    }
    t.addChild (res, -1, nullptr);

    juce::ValueTree sc ("Scene");
    sc.setProperty ("stamp", scene.stamp, nullptr);   sc.setProperty ("numGroups", scene.numGroups, nullptr);
    sc.setProperty ("tempC", scene.tempC, nullptr);   sc.setProperty ("panLaw", scene.panLaw, nullptr);
    sc.setProperty ("precision", scene.precision, nullptr);
    for (int g = 0; g < kMaxGroups; ++g)
    {
        const auto& a = scene.arrays[g];
        juce::ValueTree gt ("Group");
        gt.setProperty ("name", scene.groupNames[g], nullptr);
        gt.setProperty ("type", (int) a.type, nullptr);   gt.setProperty ("pattern", (int) a.pattern, nullptr);
        gt.setProperty ("spacing", a.spacingCm, nullptr); gt.setProperty ("angle", a.angleDeg, nullptr);
        gt.setProperty ("forward", a.centreForwardCm, nullptr);
        gt.setProperty ("x", a.x, nullptr); gt.setProperty ("y", a.y, nullptr);
        gt.setProperty ("height", a.height, nullptr);     gt.setProperty ("rotation", a.rotationDeg, nullptr);
        sc.addChild (gt, -1, nullptr);
    }
    t.addChild (sc, -1, nullptr);
    return t;
}

void Hub::restoreInstance (int slot, const juce::ValueTree& t)
{
    if (slot < 0 || ! t.hasType ("ArrayLab")) return;
    const juce::ScopedLock l (stateLock);
    auto& s = slots[(size_t) slot];

    if (auto mic = t.getChildWithName ("Mic"); mic.isValid())
    {
        auto& m = s.state;
        m.name = mic.getProperty ("name", m.name).toString();
        m.hostNamed = mic.getProperty ("hostNamed", false);
        m.role = (Role) juce::jlimit (0, 6, (int) mic.getProperty ("role", 0));
        m.group = juce::jlimit (0, kMaxGroups - 1, (int) mic.getProperty ("group", 0));
        m.pattern = (Pattern) juce::jlimit (0, 5, (int) mic.getProperty ("pattern", 2));
        m.x = mic.getProperty ("x", m.x); m.y = mic.getProperty ("y", m.y); m.z = mic.getProperty ("z", m.z);
        m.angleDeg = mic.getProperty ("angle", m.angleDeg);
        m.sourceDistance = mic.getProperty ("sourceDistance", m.sourceDistance);
        m.manualPosition = mic.getProperty ("manualPosition", false);
        // Sessions saved before the reference could be chosen lined spots up on the L/R average.
        // Sessions saved before the choice was made on the sum itself move to it.
        m.alignTo = mic.hasProperty ("decision") ? (AlignTo) juce::jlimit (0, 6, (int) mic.getProperty ("alignTo", (int) AlignTo::best)) : AlignTo::best;
        m.delayOn = mic.getProperty ("delayOn", true);   m.polarityOn = mic.getProperty ("polarityOn", true);
        m.phaseOn = mic.getProperty ("phaseOn", false);  m.offsetMs = mic.getProperty ("offsetMs", 0.0);
        m.phaseAmount = mic.getProperty ("phaseAmount", 1.0); m.phaseRotationDeg = mic.getProperty ("phaseRotation", 0.0);
        m.phaseLowHz = mic.getProperty ("phaseLow", 20.0); m.phaseHighHz = mic.getProperty ("phaseHigh", 2000.0);
        for (int r = 0; r < kNumRefs; ++r)
        {
            m.peakLocked[r] = mic.getProperty ("peakLocked" + juce::String (r), false);
            m.peakLag[r] = mic.getProperty ("peakLag" + juce::String (r), 0.0);
        }
        m.panOn = mic.getProperty ("panOn", true);
        m.panFollowsPlan = mic.getProperty ("panFollowsPlan", true);
        m.pan = mic.getProperty ("pan", 0.0);
        m.timeDifference = mic.getProperty ("timeDifference", (double) mic.getProperty ("timePan", 0.0) > 0.5);
        m.monoSource = mic.getProperty ("monoSource", false);
        m.pairPanOn = mic.getProperty ("pairPanOn", false); m.pairWidth = mic.getProperty ("pairWidth", 1.0);
        m.measureLocked = mic.getProperty ("measureLocked", false);
        m.pairSpacingCm = mic.getProperty ("pairSpacing", 17.0); m.pairAngleDeg = mic.getProperty ("pairAngle", 110.0);
        m.colour = juce::jlimit (-1, 9, (int) mic.getProperty ("colour", -1));

        // Identity and link. A duplicated track arrives with a uid already in use: it keeps its
        // fresh one and starts unlinked.
        const juce::int64 savedUid = mic.getProperty ("uid", 0);
        bool taken = savedUid == 0;
        for (int i = 0; i < maxSlots; ++i)
            taken |= i != slot && slots[(size_t) i].active.load() && slots[(size_t) i].state.uid == savedUid;
        if (! taken)
        {
            m.uid = savedUid;
            m.partner = mic.getProperty ("partner", 0);
        }
    }

    if (auto res = t.getChildWithName ("Results"); res.isValid())
    {
        MicResults r;
        const bool old = (int) res.getProperty ("format", 1) < 2;
        for (int i = 0; i < (old ? 3 : kNumRefs); ++i)
        {
            const auto k = juce::String (i);
            r.vs[i] = loadResult (res, k, old);
            if (old)
            {
                r.vsRight[i].valid = r.vsRight[i].reliable = res.getProperty ("validRight" + k, false);
                r.vsRight[i].delaySamples = res.getProperty ("delayRight" + k, 0.0);
                r.vsRight[i].confidence = res.getProperty ("confidenceRight" + k, 0.0);
                continue; // phase curves of earlier versions were coarser: a new analysis brings them back
            }
            r.vsRight[i] = loadResult (res, "Right" + k, false);
            r.curve[i].valid = res.getProperty ("curveValid" + k, false);
            if (r.curve[i].valid)
            {
                unpack (res.getProperty ("phase" + k).toString(), r.curve[i].phase, kFineBands);
                unpack (res.getProperty ("trust" + k).toString(), r.curve[i].trust, kFineBands);
                r.zones[i] = zoneDelays (r.curve[i]);
            }
        }
        if (! old)
            unpack (res.getProperty ("trim").toString(), r.phaseTrim, kFineBands);
        if (res.hasProperty ("best"))
        {
            float v[8];
            unpack (res.getProperty ("best").toString(), v, 8);
            r.best.valid = v[0] > 0.5f; r.best.lag = v[1]; r.best.polarity = v[2] < 0.0f ? -1 : 1;
            r.best.asRecordedDb = v[3]; r.best.gainDb = v[4]; r.best.withRotationDb = v[5]; r.best.limitDb = v[6]; r.best.rotationDeg = v[7];
            for (int i = 0; i < kNumRefs; ++i)
            {
                float gains[kMaxPeaks];
                unpack (res.getProperty ("peakGains" + juce::String (i)).toString(), gains, kMaxPeaks);
                for (int p = 0; p < r.vs[i].numPeaks; ++p) r.vs[i].peaks[p].gainDb = gains[p];
            }
        }
        s.results = r;
    }

    // Every instance carries a copy of the scene; the most recently edited one wins.
    if (auto sc = t.getChildWithName ("Scene"); sc.isValid() && (juce::int64) sc.getProperty ("stamp", 0) > scene.stamp)
    {
        scene.stamp = sc.getProperty ("stamp", 0);
        scene.numGroups = juce::jlimit (1, kMaxGroups, (int) sc.getProperty ("numGroups", 1));
        scene.tempC = sc.getProperty ("tempC", 20.0);
        scene.panLaw = juce::jlimit (0, 2, (int) sc.getProperty ("panLaw", 0));
        scene.precision = juce::jlimit (0, 1, (int) sc.getProperty ("precision", 0));
        precisionMode = scene.precision;
        for (int g = 0; g < kMaxGroups && g < sc.getNumChildren(); ++g)
        {
            const auto gt = sc.getChild (g);
            auto& a = scene.arrays[g];
            scene.groupNames[g] = gt.getProperty ("name", scene.groupNames[g]).toString();
            a.type = (ArrayType) juce::jlimit (0, 6, (int) gt.getProperty ("type", 1));
            a.pattern = (Pattern) juce::jlimit (0, 5, (int) gt.getProperty ("pattern", 2));
            a.spacingCm = gt.getProperty ("spacing", a.spacingCm); a.angleDeg = gt.getProperty ("angle", a.angleDeg);
            a.centreForwardCm = gt.getProperty ("forward", 0.0);
            a.x = gt.getProperty ("x", 0.0); a.y = gt.getProperty ("y", 0.0);
            a.height = gt.getProperty ("height", 3.0); a.rotationDeg = gt.getProperty ("rotation", 0.0);
        }
    }
    recomputeAll();
    ++changes;
}
}
