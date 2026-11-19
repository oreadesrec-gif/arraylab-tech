// Headless host: a main pair and three spots on a shared timeline, as a DAW would run them.
// Checks the whole chain: analysis on demand, frozen results, placement, and the audio correction.
#include "../src/PluginProcessor.h"
#include <random>

using namespace arraylab;

struct FakePlayHead : juce::AudioPlayHead
{
    juce::int64 pos = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo i;
        i.setTimeInSamples (pos);
        i.setIsPlaying (true);
        return i;
    }
};

static double match (const std::vector<float>& a, const std::vector<float>& b)
{
    double xy = 0, xx = 0, yy = 0;
    for (size_t i = 0; i < a.size(); ++i) { xy += (double) a[i] * b[i]; xx += (double) a[i] * a[i]; yy += (double) b[i] * b[i]; }
    return xy / std::sqrt (xx * yy + 1e-30);
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double sr = 48000.0, c = speedOfSound (20.0);
    const int block = 256, total = (int) (sr * 30.0), numSources = 3;

    // Three instruments, each with its own spot 0.5 m away, heard by an AB pair at the origin.
    const Vec2 sources[numSources] = { { -2.0, 5.0 }, { 1.5, 4.0 }, { 3.0, 6.5 } };
    ArraySettings arr;
    applyPreset (arr, ArrayType::ab);
    arr.height = 1.5;
    const auto geo = arrayGeometry (arr);

    std::mt19937 rng (11);
    std::normal_distribution<float> g (0.0f, 0.2f);
    std::vector<float> src[numSources];
    int toL[numSources], toR[numSources];
    const int toSpot = (int) std::lround (0.5 / c * sr);
    for (int s = 0; s < numSources; ++s)
    {
        src[s].resize ((size_t) total);
        float lp = 0.0f;
        for (auto& v : src[s]) { lp += 0.25f * (g (rng) - lp); v = lp; }
        toL[s] = (int) std::lround (distance (sources[s], geo.pos[0]) / c * sr);
        toR[s] = (int) std::lround (distance (sources[s], geo.pos[2]) / c * sr);
    }
    const auto at = [&] (int s, int i) { return i >= 0 ? src[s][(size_t) i] : 0.0f; };

    FakePlayHead head;
    ArrayLabProcessor mainPair, spot[numSources], pairSpot; // pairSpot: a stereo pair on instrument 1
    auto& hub = mainPair.hub();
    mainPair.setPlayHead (&head);
    mainPair.setPlayConfigDetails (2, 2, sr, block);
    mainPair.prepareToPlay (sr, block);
    {
        auto m = hub.getMic (mainPair.slot());
        m.role = Role::mainPair; m.name = "Main";
        hub.setMic (mainPair.slot(), m);
        auto sc = hub.getScene();
        sc.arrays[0] = arr;
        hub.setScene (sc);
    }
    for (int s = 0; s < numSources; ++s)
    {
        spot[s].setPlayHead (&head);
        spot[s].setPlayConfigDetails (1, 2, sr, block);
        spot[s].prepareToPlay (sr, block);
        auto m = hub.getMic (spot[s].slot());
        m.name = "Spot " + juce::String (s + 1);
        m.phaseOn = s == 2;
        hub.setMic (spot[s].slot(), m);
    }

    pairSpot.setPlayHead (&head);
    pairSpot.setPlayConfigDetails (2, 2, sr, block);
    pairSpot.prepareToPlay (sr, block);
    {
        auto m = hub.getMic (pairSpot.slot());
        m.name = "Pair";
        m.pairSpacingCm = 30.0; m.pairAngleDeg = 90.0;
        hub.setMic (pairSpot.slot(), m);
    }
    const int pairGap = 8; // samples between the pair's two capsules for this source
    std::vector<float> pairOutL, pairOutR;

    // Spot 2 is wired out of phase; spot 3 goes through a phase-shifting stage.
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    std::vector<float> outL[numSources], wantL[numSources], outR0, wantR0;
    int cursor = 0;
    bool record = false;

    const auto run = [&] (double seconds)
    {
        const int end = cursor + (int) (seconds * sr);
        for (; cursor + block <= end; cursor += block, head.pos += block)
        {
            for (int i = 0; i < block; ++i)
            {
                float l = 0, r = 0;
                for (int s = 0; s < numSources; ++s) { l += at (s, cursor + i - toL[s]); r += at (s, cursor + i - toR[s]); }
                buf.setSample (0, i, l);
                buf.setSample (1, i, r);
            }
            mainPair.processBlock (buf, midi);

            for (int s = 0; s < numSources; ++s)
            {
                for (int i = 0; i < block; ++i)
                {
                    float v = at (s, cursor + i - toSpot);
                    if (s == 1) v = -v;
                    if (s == 2)
                    {
                        const float a = -0.6f * v + x1 + 0.6f * y1;   x1 = v; y1 = a;
                        const float b = -0.6f * a + x2 + 0.6f * y2;   x2 = a; y2 = b;
                        v = b;
                    }
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, 0.0f);
                }
                spot[s].processBlock (buf, midi);
                if (record)
                    for (int i = 0; i < block; ++i)
                    {
                        outL[s].push_back (buf.getSample (0, i));
                        wantL[s].push_back (at (s, cursor + i - toL[s]));
                        if (s == 0) { outR0.push_back (buf.getSample (1, i)); wantR0.push_back (at (s, cursor + i - toR[s])); }
                    }
            }
            for (int i = 0; i < block; ++i)
            {
                buf.setSample (0, i, at (0, cursor + i - toSpot));
                buf.setSample (1, i, at (0, cursor + i - toSpot - pairGap));
            }
            pairSpot.processBlock (buf, midi);
            if (record)
                for (int i = 0; i < block; ++i) { pairOutL.push_back (buf.getSample (0, i)); pairOutR.push_back (buf.getSample (1, i)); }

            if ((cursor / block) % 2 == 0) juce::Thread::sleep (1);
        }
    };

    bool ok = true;
    const auto check = [&] (bool cond, const juce::String& what) { ok &= cond; std::printf ("%-58s %s\n", what.toRawUTF8(), cond ? "ok" : "FAIL"); };

    run (3.0);
    bool untouched = true;
    for (auto& m : hub.snapshot()) untouched &= m.results.primary < 0;
    check (untouched, "playing without Analyze measures nothing");

    hub.startAnalysis();
    run (10.0);
    hub.stopAnalysis();
    const auto frozen = hub.snapshot();
    run (4.0);
    bool kept = true;
    {
        const auto now = hub.snapshot();
        for (int i = 0; i < now.size(); ++i)
            for (int r = 0; r < 3; ++r)
                kept &= now[i].results.vs[r].frames == frozen[i].results.vs[r].frames
                        && juce::exactlyEqual (now[i].results.vs[r].delaySamples, frozen[i].results.vs[r].delaySamples);
    }
    check (kept, "playing again after Stop does not re-analyse");

    for (auto& m : frozen)
    {
        if (isMain (m.state.role)) continue;
        if (m.index == pairSpot.slot())
        {
            const auto l = combinedResult (m.results, 0, true);
            std::printf ("  Pair: left capsule vs L %.2f, right capsule vs L %.2f, moved as one by %.2f (want %.1f)  applied L %.3f / R %.3f ms\n",
                         m.results.vs[0].delaySamples, m.results.vsRight[0].delaySamples, l.delaySamples,
                         toSpot + pairGap * 0.5 - toL[0], m.applied.delayLms, m.applied.delayRms);
            check (m.isStereoPair() && std::abs (m.results.vs[0].delaySamples - (toSpot - toL[0])) < 0.6
                       && std::abs (m.results.vsRight[0].delaySamples - (toSpot + pairGap - toL[0])) < 0.6, "stereo spot: each capsule measured");
            check (std::abs (l.delaySamples - (toSpot + pairGap * 0.5 - toL[0])) < 0.6
                       && juce::exactlyEqual (m.applied.delayLms, m.applied.delayRms) && m.applied.delayLms > 1.0, "stereo spot: one common delay for the pair");
            continue;
        }
        const int s = m.index - spot[0].slot();
        const auto& vs = m.results.vs;
        std::printf ("  %s: vs L %.2f (want %d)  vs R %.2f (want %d)  polarity %+d  placed (%.2f, %.2f)  applied L %.2f / R %.2f ms  gains %.2f / %.2f  %s\n",
                     m.state.name.toRawUTF8(), vs[0].delaySamples, toSpot - toL[s], vs[2].delaySamples, toSpot - toR[s],
                     m.results.primary >= 0 ? vs[m.results.primary].polarity : 0, m.state.x, m.state.y,
                     m.applied.delayLms, m.applied.delayRms, m.applied.gainL, m.applied.gainR, m.applied.note.toRawUTF8());
        const double tol = s == 2 ? 4.0 : 0.6; // the phase-shifted spot has no single "true" delay
        check (vs[0].valid && vs[2].valid && std::abs (vs[0].delaySamples - (toSpot - toL[s])) < tol
                   && std::abs (vs[2].delaySamples - (toSpot - toR[s])) < tol, m.state.name + ": delays to L and R");
        if (s != 2) // a phase-shifted signal has no single polarity; its phase filter is checked below
            check ((vs[m.results.primary].polarity < 0) == (s == 1), m.state.name + ": polarity");
        check (std::hypot (m.state.x - sources[s].x, m.state.y - sources[s].y) < (s == 2 ? 0.8 : 0.3), m.state.name + ": placed on the plan");
    }

    // Now listen to the result, with the sum check running as the Check view would.
    hub.setCheck (true, -1);
    record = true;
    run (4.0);
    record = false;
    const auto all = hub.getCheck();
    hub.setCheck (true, spot[0].slot());
    run (3.0);
    const auto one = hub.getCheck();
    hub.setCheck (false, -1);
    std::printf ("  sum of all mics: %+.1f dB with corrections (lows %+.1f, mids %+.1f, highs %+.1f)\n", all.gainAll, all.gainLow, all.gainMid, all.gainHigh);
    std::printf ("  main + spot 1: %+.1f dB, correlation %.2f -> %.2f\n", one.gainAll, one.correlationBefore, one.correlationAfter);
    // The gain shows in the lows; higher up, the main pair's own L-R spacing limits what a mono fold can add.
    check (all.valid && all.gainLow > 1.5 && all.gainAll > 0.3, "check: aligned microphones add up fuller");
    check (one.valid && one.hasCorrelation && one.correlationAfter > one.correlationBefore + 0.3, "check: spot 1 correlates with the main array once aligned");
    const auto info = hub.snapshot();
    const auto gainOf = [&] (int s, bool right) { for (auto& m : info) if (m.index == spot[s].slot()) return right ? m.applied.gainR : m.applied.gainL; return 1.0f; };
    const auto tail = [] (std::vector<float> v) { return std::vector<float> (v.end() - 48000, v.end()); };

    check (match (tail (outL[0]), tail (wantL[0])) > 0.999, "spot 1 left output lines up with main L");
    check (match (tail (outR0), tail (wantR0)) > 0.999, "spot 1 right output lines up with main R (own delay)");
    check (gainOf (0, false) > gainOf (0, true), "spot 1 (left of centre) is panned left");
    check (gainOf (1, true) > gainOf (1, false), "spot 2 (right of centre) is panned right");
    check (match (tail (outL[1]), tail (wantL[1])) > 0.999, "spot 2 polarity corrected and aligned");
    {
        // The pair's own image must survive: right still trails left by the same gap.
        const auto l = tail (pairOutL), r = tail (pairOutR);
        check (match (std::vector<float> (l.begin(), l.end() - pairGap), std::vector<float> (r.begin() + pairGap, r.end())) > 0.999,
               "stereo spot: its internal image is kept");
    }
    const double withPhase = match (tail (outL[2]), tail (wantL[2]));

    auto m3 = hub.getMic (spot[2].slot());
    m3.phaseOn = false;
    hub.setMic (spot[2].slot(), m3);
    for (auto& v : outL) v.clear();
    for (auto& v : wantL) v.clear();
    record = true;
    run (3.0);
    record = false;
    const double withoutPhase = match (tail (outL[2]), tail (wantL[2]));
    std::printf ("  spot 3 match with main L: delay only %.3f, with multiband phase %.3f\n", withoutPhase, withPhase);
    check (withPhase > 0.95 && withPhase > withoutPhase + 0.05, "multiband phase improves the phase-shifted spot");

    // Bypass switches.
    auto m1 = hub.getMic (spot[0].slot());
    m1.delayOn = false;
    hub.setMic (spot[0].slot(), m1);
    for (auto& mi : hub.snapshot()) if (mi.index == spot[0].slot()) check (mi.applied.delayLms < 1e-9 && mi.applied.delayRms < 1e-9, "delay bypass removes the delay");
    auto m2 = hub.getMic (spot[1].slot());
    m2.polarityOn = false;
    hub.setMic (spot[1].slot(), m2);
    for (auto& mi : hub.snapshot()) if (mi.index == spot[1].slot()) check (mi.applied.polarity > 0, "polarity bypass leaves polarity alone");

    // Save and restore keeps the analysis.
    juce::MemoryBlock saved;
    spot[0].getStateInformation (saved);
    hub.resetAnalysis();
    bool cleared = true;
    for (auto& mi : hub.snapshot()) cleared &= mi.results.primary < 0;
    check (cleared, "Reset analysis clears every result");
    spot[0].setStateInformation (saved.getData(), (int) saved.getSize());
    for (auto& mi : hub.snapshot()) if (mi.index == spot[0].slot()) check (mi.results.vs[0].valid && ! mi.state.delayOn, "session reload restores the analysis and switches");

    return ok ? 0 : 1;
}
