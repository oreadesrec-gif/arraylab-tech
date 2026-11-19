#include "Hub.h"
#include "PhaseFir.h"

namespace arraylab
{
Hub::Hub() : juce::Thread ("ArrayLab analysis")
{
    scene.groupNames[0] = "Main";
    for (int g = 1; g < kMaxGroups; ++g)
        scene.groupNames[g] = "Group " + juce::String (g + 1);
    startThread (juce::Thread::Priority::low);
}

Hub::~Hub()
{
    stopThread (2000);
}

//==============================================================================
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
                s.state.x = ((i % 10) - 4.5) * 0.8; // parked in a row until analysed or placed
                s.state.y = 2.0 + (i / 10);
                s.results = {};
                s.applied = {};
                s.sink = nullptr;
                s.firLength = 0; s.firBuilt = -1; ++s.firVersion;
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
    slots[(size_t) slot].firBuilt = -1;
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
    s.firBuilt = -1;
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
    s.state = m;
    s.state.group = juce::jlimit (0, kMaxGroups - 1, m.group);
    if (refsChanged) recomputeAll(); else recompute (slot);
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
    scene.stamp = juce::Time::currentTimeMillis();
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

    MicApplied a;
    double dL = 0.0, dR = 0.0; // samples
    float gL = 1.0f, gR = 1.0f;
    bool phase = false;
    int firLength = 0;
    const bool fromMono = numOut == 2 && (numIn == 1 || (numIn == 2 && m.monoSource));
    const bool pair = numIn == 2 && ! m.monoSource;
    const DelayResult vs[3] = { combinedResult (res, 0, pair), combinedResult (res, 1, pair), combinedResult (res, 2, pair) };

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

        // Common delay: from the analysis when there is one, else from the plan if the user placed the mic.
        double base = 0.0;
        bool known = true;
        if (vs[0].valid && vs[2].valid)  base = -0.5 * (vs[0].delaySamples + vs[2].delaySamples);
        else if (vs[1].valid)            base = -vs[1].delaySamples;
        else if (vs[0].valid)            base = -vs[0].delaySamples;
        else if (vs[2].valid)            base = -vs[2].delaySamples;
        else if (m.manualPosition)             { base = (0.5 * (rL + rR) - m.sourceDistance) / c * sr; a.note = "Delay from the plan (not analysed)"; }
        else                                   { known = false; a.note = "Not analysed yet"; }

        if (m.delayOn && known)
        {
            base += m.offsetMs * 0.001 * sr;
            const double split = (fromMono && m.panOn) ? 0.5 * (rL - rR) / c * sr * m.timePan : 0.0;
            dL = base + split;
            dR = base - split;
            if (juce::jmin (dL, dR) < -0.5)
                a.note = "Arrives after the reference: cannot be advanced";
            dL = juce::jmax (0.0, dL);
            dR = juce::jmax (0.0, dR);
        }

        if (m.polarityOn && res.primary >= 0 && res.vs[res.primary].valid && res.vs[res.primary].polarity < 0)
            a.polarity = -1;

        if (fromMono)
        {
            if (m.panOn) panGains (geo, arr.pattern, p, rL, rR, scene.panLaw, m.panAmount, gL, gR);
            else         gL = gR = 0.70710678f;
        }

        // Multiband phase: the filter's latency is taken out of the alignment delay,
        // so the plugin never reports latency to the host.
        if (m.phaseOn && res.phaseValid && m.delayOn && known)
        {
            const int room = (int) juce::jmin (dL, dR);
            const int most = juce::nextPowerOfTwo ((int) (1024.0 * sr / 48000.0));
            int half = 32;
            while (half * 2 <= room && half * 2 <= most) half *= 2;
            if (room >= 32)
            {
                firLength = half * 2;
                phase = true;
                dL -= half;
                dR -= half;
            }
            else
                a.note = "Too close to the reference for phase correction";
        }
    }

    a.phaseTaps = firLength;
    if (phase && s.sink != nullptr && (s.firBuilt != s.firVersion || s.firLength != firLength))
    {
        std::vector<float> taps;
        designPhaseFir (res.phase.data(), sr, firLength, taps);
        s.sink (taps);
        s.firBuilt = s.firVersion;
        s.firLength = firLength;
    }

    const int iL = juce::roundToInt (dL), iR = juce::roundToInt (dR);
    a.delayLms = sr > 0.0 ? 1000.0 * (iL + firLength / 2) / sr : 0.0;
    a.delayRms = sr > 0.0 ? 1000.0 * (iR + firLength / 2) / sr : 0.0;
    a.gainL = gL;
    a.gainR = gR;
    s.applied = a;

    s.live.delayL = iL;
    s.live.delayR = iR;
    s.live.gainL = gL * (float) a.polarity;
    s.live.gainR = gR * (float) a.polarity;
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
    const auto horizontal = [&] (const DelayResult& r)
    {
        const double d = juce::jmax (0.1, c * -r.delaySamples / sr + m.sourceDistance);
        return std::sqrt (juce::jmax (0.01, d * d - dz * dz));
    };

    const bool pair = s.numIn.load() == 2 && ! m.monoSource;
    const DelayResult v[3] = { combinedResult (s.results, 0, pair), combinedResult (s.results, 1, pair), combinedResult (s.results, 2, pair) };
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
    const juce::ScopedLock l (stateLock);
    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (! s.active.load()) continue;
        ++s.firVersion;
        if (! s.state.manualPosition)
            place (i);
    }
    recomputeAll();
    ++changes;
}

void Hub::resetAnalysis()
{
    analysing = false;
    resetRequested = true;
    const juce::ScopedLock al (analysisLock);
    const juce::ScopedLock l (stateLock);
    for (auto& s : slots)
    {
        s.results = {};
        ++s.firVersion;
    }
    analysedTime = 0.0;
    recomputeAll();
    ++changes;
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

CheckData Hub::getCheck() const
{
    const juce::ScopedLock l (stateLock);
    return checkData;
}

/** Adds the microphones up twice, as they arrive and as they leave the plugins,
    so the user can see whether the corrections make the sum fuller or thinner. */
void Hub::check()
{
    const juce::ScopedLock al (analysisLock);
    if (! checkActive.load())
    {
        if (checkPrimed) { checkPrimed = false; const juce::ScopedLock l (stateLock); checkData = {}; }
        return;
    }

    struct Entry
    {
        int slot; bool main, stereo, fromMono, phase;
        int dL, dR; float gL, gR, bL, bR; int half;
        std::array<std::complex<float>, kNumBands> bands;
    };
    std::vector<Entry> entries;
    const int focus = checkFocus.load();
    const bool bypass = bypassAll.load();
    int clock = -1;
    {
        const juce::ScopedLock l (stateLock);
        for (int i = 0; i < maxSlots; ++i)
        {
            auto& s = slots[(size_t) i];
            if (! s.active.load() || s.numIn.load() > 2 || s.numOut.load() > 2) continue;
            if (clock < 0 && s.playing.load() && s.sampleRate.load() > 0.0) clock = i;
            const bool main = isMain (s.state.role);
            if (focus >= 0 && ! main && i != focus) continue;
            Entry e;
            e.slot = i; e.main = main;
            e.fromMono = s.live.fromMono.load();
            e.stereo = s.numIn.load() == 2 && ! e.fromMono;
            e.phase = s.live.phaseOn.load();
            e.dL = s.live.delayL.load(); e.dR = s.live.delayR.load();
            e.gL = s.live.gainL.load();  e.gR = s.live.gainR.load();
            e.bL = s.live.bypassGainL.load(); e.bR = s.live.bypassGainR.load();
            e.half = e.phase ? s.applied.phaseTaps / 2 : 0;
            e.bands = s.results.phase;
            if (s.numOut.load() == 1) { e.gR = 0.0f; e.bR = 0.0f; }
            entries.push_back (e);
        }
    }
    if (clock < 0 || entries.empty())
        return;

    auto& ck = slots[(size_t) clock];
    const double sr = ck.sampleRate.load();
    prepareFrame (sr);
    const int n = frameSize, bins = n / 2 + 1;
    const auto pos = ck.writeEnd.load() - (juce::int64) (sr * 0.1) - n;
    if (pos < ck.runStart.load() || (checkPos >= 0 && std::abs (pos - checkPos) < n / 2))
        return;
    checkPos = pos;

    for (auto* v : { &sumBefore, &sumAfter, &sumMain, &focusBefore, &focusAfter })
        v->assign ((size_t) bins, {});

    for (auto& e : entries)
    {
        for (int ch = 0; ch < (e.stereo ? 2 : 1); ++ch)
        {
            if (! readFrame (slots[(size_t) e.slot], ch, pos, n, frame.data())) continue;
            tgtSpectrum->compute (*fft, frame.data());
            const auto& x = tgtSpectrum->bins();

            // Mono fold-down of what this channel sends out, bypassed and corrected.
            const float gBefore = e.stereo ? (ch == 0 ? e.bL : e.bR) : e.bL + e.bR;
            const auto stepL = std::polar (1.0, -2.0 * kPi * (e.dL + e.half) / n);
            const auto stepR = std::polar (1.0, -2.0 * kPi * (e.dR + e.half) / n);
            std::complex<double> rotL (1.0, 0.0), rotR (1.0, 0.0);
            for (int k = 0; k < bins; ++k)
            {
                const std::complex<double> xk (x[(size_t) k]);
                const auto before = xk * (double) gBefore;
                std::complex<double> after;
                if (bypass)          after = before;
                else
                {
                    after = e.stereo ? xk * (ch == 0 ? (double) e.gL * rotL : (double) e.gR * rotR)
                                     : xk * ((double) e.gL * rotL + (double) e.gR * rotR);
                    if (e.phase)     after *= std::polar (1.0, -residualPhaseAt (e.bands.data(), k * sr / n));
                }
                sumBefore[(size_t) k] += std::complex<float> (before);
                sumAfter[(size_t) k] += std::complex<float> (after);
                if (e.main)                   sumMain[(size_t) k] += std::complex<float> (before);
                else if (e.slot == focus)   { focusBefore[(size_t) k] += std::complex<float> (before); focusAfter[(size_t) k] += std::complex<float> (after); }
                rotL *= stepL;
                rotR *= stepR;
            }
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
        pb[b] += std::norm (sumBefore[(size_t) k]);
        pa[b] += std::norm (sumAfter[(size_t) k]);
        corr[0] += (std::complex<double> (sumMain[(size_t) k]) * std::conj (std::complex<double> (focusBefore[(size_t) k]))).real();
        corr[1] += (std::complex<double> (sumMain[(size_t) k]) * std::conj (std::complex<double> (focusAfter[(size_t) k]))).real();
        corr[2] += std::norm (sumMain[(size_t) k]);
        corr[3] += std::norm (focusBefore[(size_t) k]);
        corr[4] += std::norm (focusAfter[(size_t) k]);
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
    const juce::ScopedLock l (stateLock);
    checkData = d;
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
    struct Target { int slot; RefSet refs; bool pair; };
    std::vector<Target> targets;
    int clock = -1;
    {
        const juce::ScopedLock l (stateLock);
        for (int i = 0; i < maxSlots; ++i)
        {
            auto& s = slots[(size_t) i];
            if (! s.active.load()) continue;
            if (clock < 0 && s.playing.load() && s.sampleRate.load() > 0.0) clock = i;
            if (! isMain (s.state.role))
                targets.push_back ({ i, refsForGroup (s.state.group), s.numIn.load() == 2 && ! s.state.monoSource });
        }
    }
    if (clock < 0)
        return;

    auto& ck = slots[(size_t) clock];
    const double sr = ck.sampleRate.load();
    prepareFrame (sr);

    // Leave a little room so instances processed later in the host cycle have caught up.
    const auto guard = (juce::int64) (sr * 0.1);
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
        const auto refSpectrum = [&] (int slot, int channel) -> const FrameSpectrum*
        {
            const int key = slot * 2 + channel;
            RefSpectrum* spare = nullptr;
            for (auto& rs : refSpectra)
            {
                if (rs.key == key) return rs.ok ? rs.spectrum.get() : nullptr;
                if (rs.key < 0 && spare == nullptr) spare = &rs;
            }
            if (spare == nullptr)
            {
                refSpectra.emplace_back();
                spare = &refSpectra.back();
                spare->spectrum = std::make_unique<FrameSpectrum> (frameSize);
            }
            spare->key = key;
            spare->ok = readFrame (slots[(size_t) slot], channel, framePos, frameSize, frame.data());
            if (spare->ok) spare->spectrum->compute (*fft, frame.data());
            return spare->ok ? spare->spectrum.get() : nullptr;
        };

        for (auto& t : targets)
        {
            auto& s = slots[(size_t) t.slot];
            bool haveTarget = false;
            for (int r = 0; r < 3; ++r)
            {
                const int rslot = t.refs.slot[r];
                if (rslot < 0 || rslot == t.slot) continue;
                const int key = rslot * 2 + t.refs.channel[r];
                if (s.estimatorRef[r] != key) { s.estimator[r].reset (frameSize); s.estimatorRight[r].reset (frameSize); s.estimatorRef[r] = key; }
                auto* ref = refSpectrum (rslot, t.refs.channel[r]);
                if (ref == nullptr) continue;
                if (! haveTarget)
                {
                    if (! readFrame (s, 0, framePos, frameSize, frame.data())) break;
                    tgtSpectrum->compute (*fft, frame.data());
                    haveTarget = true;
                }
                s.estimator[r].accumulate (*ref, *tgtSpectrum);
                changed = true;
            }

            // Second channel of a stereo spot, against the same references.
            if (t.pair && haveTarget && readFrame (s, 1, framePos, frameSize, frame.data()))
            {
                tgtSpectrum->compute (*fft, frame.data());
                for (int r = 0; r < 3; ++r)
                {
                    const int rslot = t.refs.slot[r];
                    if (rslot < 0 || rslot == t.slot) continue;
                    if (auto* ref = refSpectrum (rslot, t.refs.channel[r]))
                        s.estimatorRight[r].accumulate (*ref, *tgtSpectrum);
                }
            }
        }
        framePos += frameSize / 2;
    }

    if (! changed)
        return;

    int mostFrames = 0;
    for (auto& t : targets)
    {
        auto& s = slots[(size_t) t.slot];
        MicResults res;
        bool any = false;
        for (int r = 0; r < 3; ++r)
            if (s.estimatorRef[r] >= 0 && s.estimator[r].frames() > 0)
            {
                res.vs[r] = s.estimator[r].estimate (*fft, sr, scratch);
                if (t.pair && s.estimatorRight[r].frames() > 0)
                    res.vsRight[r] = s.estimatorRight[r].estimate (*fft, sr, scratch);
                mostFrames = juce::jmax (mostFrames, res.vs[r].frames);
                any = true;
            }
        if (! any) continue;

        res.primary = res.vs[1].valid ? 1
                    : (res.vs[0].valid && (! res.vs[2].valid || res.vs[0].confidence >= res.vs[2].confidence)) ? 0
                    : res.vs[2].valid ? 2 : -1;
        if (res.primary >= 0)
        {
            const auto& pr = res.vs[res.primary];
            s.estimator[res.primary].phaseBands (sr, pr.delaySamples, pr.polarity, res.phase.data());
            res.phaseValid = true;
        }
        const juce::ScopedLock l (stateLock);
        s.results = res;
    }
    analysedTime = mostFrames * (frameSize / 2) / sr;
    ++changes;
}

//==============================================================================
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
    mic.setProperty ("delayOn", m.delayOn, nullptr);  mic.setProperty ("polarityOn", m.polarityOn, nullptr);
    mic.setProperty ("phaseOn", m.phaseOn, nullptr);  mic.setProperty ("offsetMs", m.offsetMs, nullptr);
    mic.setProperty ("panOn", m.panOn, nullptr);      mic.setProperty ("panAmount", m.panAmount, nullptr);
    mic.setProperty ("timePan", m.timePan, nullptr);  mic.setProperty ("monoSource", m.monoSource, nullptr);
    mic.setProperty ("pairSpacing", m.pairSpacingCm, nullptr); mic.setProperty ("pairAngle", m.pairAngleDeg, nullptr);
    mic.setProperty ("colour", m.colour, nullptr);
    t.addChild (mic, -1, nullptr);

    juce::ValueTree res ("Results");
    res.setProperty ("primary", s.results.primary, nullptr);
    res.setProperty ("phaseValid", s.results.phaseValid, nullptr);
    for (int r = 0; r < 3; ++r)
    {
        const auto& v = s.results.vs[r];
        const auto k = juce::String (r);
        res.setProperty ("valid" + k, v.valid, nullptr);       res.setProperty ("delay" + k, v.delaySamples, nullptr);
        res.setProperty ("polarity" + k, v.polarity, nullptr); res.setProperty ("confidence" + k, v.confidence, nullptr);
        res.setProperty ("frames" + k, v.frames, nullptr);
        res.setProperty ("validRight" + k, s.results.vsRight[r].valid, nullptr);
        res.setProperty ("delayRight" + k, s.results.vsRight[r].delaySamples, nullptr);
        res.setProperty ("confidenceRight" + k, s.results.vsRight[r].confidence, nullptr);
    }
    juce::StringArray bands;
    for (auto& b : s.results.phase) { bands.add (juce::String (b.real(), 5)); bands.add (juce::String (b.imag(), 5)); }
    res.setProperty ("phase", bands.joinIntoString (" "), nullptr);
    t.addChild (res, -1, nullptr);

    juce::ValueTree sc ("Scene");
    sc.setProperty ("stamp", scene.stamp, nullptr);   sc.setProperty ("numGroups", scene.numGroups, nullptr);
    sc.setProperty ("tempC", scene.tempC, nullptr);   sc.setProperty ("panLaw", scene.panLaw, nullptr);
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
        m.delayOn = mic.getProperty ("delayOn", true);   m.polarityOn = mic.getProperty ("polarityOn", true);
        m.phaseOn = mic.getProperty ("phaseOn", false);  m.offsetMs = mic.getProperty ("offsetMs", 0.0);
        m.panOn = mic.getProperty ("panOn", true);       m.panAmount = mic.getProperty ("panAmount", 1.0);
        m.timePan = mic.getProperty ("timePan", 1.0);    m.monoSource = mic.getProperty ("monoSource", false);
        m.pairSpacingCm = mic.getProperty ("pairSpacing", 17.0); m.pairAngleDeg = mic.getProperty ("pairAngle", 110.0);
        m.colour = juce::jlimit (-1, 9, (int) mic.getProperty ("colour", -1));
    }

    if (auto res = t.getChildWithName ("Results"); res.isValid())
    {
        MicResults r;
        r.primary = juce::jlimit (-1, 2, (int) res.getProperty ("primary", -1));
        r.phaseValid = res.getProperty ("phaseValid", false);
        for (int i = 0; i < 3; ++i)
        {
            const auto k = juce::String (i);
            auto& v = r.vs[i];
            v.valid = res.getProperty ("valid" + k, false);    v.delaySamples = res.getProperty ("delay" + k, 0.0);
            v.polarity = (int) res.getProperty ("polarity" + k, 1) < 0 ? -1 : 1;
            v.confidence = res.getProperty ("confidence" + k, 0.0);
            v.frames = res.getProperty ("frames" + k, 0);
            r.vsRight[i].valid = res.getProperty ("validRight" + k, false);
            r.vsRight[i].delaySamples = res.getProperty ("delayRight" + k, 0.0);
            r.vsRight[i].confidence = res.getProperty ("confidenceRight" + k, 0.0);
        }
        const auto bands = juce::StringArray::fromTokens (res.getProperty ("phase").toString(), " ", "");
        for (int b = 0; b < kNumBands && b * 2 + 1 < bands.size(); ++b)
            r.phase[(size_t) b] = { bands[b * 2].getFloatValue(), bands[b * 2 + 1].getFloatValue() };
        s.results = r;
        ++s.firVersion;
    }

    // Every instance carries a copy of the scene; the most recently edited one wins.
    if (auto sc = t.getChildWithName ("Scene"); sc.isValid() && (juce::int64) sc.getProperty ("stamp", 0) > scene.stamp)
    {
        scene.stamp = sc.getProperty ("stamp", 0);
        scene.numGroups = juce::jlimit (1, kMaxGroups, (int) sc.getProperty ("numGroups", 1));
        scene.tempC = sc.getProperty ("tempC", 20.0);
        scene.panLaw = juce::jlimit (0, 2, (int) sc.getProperty ("panLaw", 0));
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
