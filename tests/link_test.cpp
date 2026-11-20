// Headless host: a main pair and several spots on a shared timeline, as a DAW would run them.
// Checks the whole chain: analysis on demand, frozen results, reliability, placement, pan,
// the audio correction, phase by frequency, latency mode, refinement on the sum, and reload.
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
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) { xy += (double) a[i] * b[i]; xx += (double) a[i] * a[i]; yy += (double) b[i] * b[i]; }
    return xy / std::sqrt (xx * yy + 1e-30);
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double sr = 48000.0, c = speedOfSound (20.0);
    const int block = 256, total = (int) (sr * 130.0), numSources = 3;

    // Three instruments, each with its own spot 0.5 m away, heard by an AB pair at the origin.
    const Vec2 sources[numSources] = { { -2.0, 5.0 }, { 1.5, 4.0 }, { 3.0, 6.5 } };
    ArraySettings arr;
    applyPreset (arr, ArrayType::ab);
    arr.height = 1.5;
    const auto geo = arrayGeometry (arr);

    std::mt19937 rng (11);
    std::normal_distribution<float> g (0.0f, 0.2f);
    std::vector<float> src[numSources + 1]; // the last one is heard by nobody but a stray mic
    int toL[numSources], toR[numSources];
    const int toSpot = (int) std::lround (0.5 / c * sr);
    for (int s = 0; s <= numSources; ++s)
    {
        src[s].resize ((size_t) total);
        float lp = 0.0f;
        for (auto& v : src[s]) { lp += 0.25f * (g (rng) - lp); v = lp; }
        if (s == numSources) break;
        toL[s] = (int) std::lround (distance (sources[s], geo.pos[0]) / c * sr);
        toR[s] = (int) std::lround (distance (sources[s], geo.pos[2]) / c * sr);
    }
    const auto at = [&] (int s, int i) { return i >= 0 ? src[s][(size_t) i] : 0.0f; };

    FakePlayHead head;
    ArrayLabProcessor mainPair, spot[numSources], pairSpot, stray; // pairSpot: a stereo pair on instrument 1
    auto& hub = mainPair.hub();
    const auto setUp = [&] (ArrayLabProcessor& p, int in, int out, const juce::String& name)
    {
        p.setPlayHead (&head);
        p.setPlayConfigDetails (in, out, sr, block);
        p.prepareToPlay (sr, block);
        auto m = hub.getMic (p.slot());
        m.name = name;
        hub.setMic (p.slot(), m);
    };
    const auto edit = [&] (ArrayLabProcessor& p, const std::function<void (MicState&)>& change)
    {
        auto m = hub.getMic (p.slot());
        change (m);
        hub.setMic (p.slot(), m);
    };
    const auto info = [&] (ArrayLabProcessor& p)
    {
        for (auto& m : hub.snapshot()) if (m.index == p.slot()) return m;
        return MicInfo {};
    };

    setUp (mainPair, 2, 2, "Main");
    edit (mainPair, [] (MicState& m) { m.role = Role::mainPair; });
    { auto sc = hub.getScene(); sc.arrays[0] = arr; hub.setScene (sc); }
    for (int s = 0; s < numSources; ++s)
        setUp (spot[s], 1, 2, "Spot " + juce::String (s + 1));
    edit (spot[2], [] (MicState& m) { m.phaseOn = true; });
    setUp (pairSpot, 2, 2, "Pair");
    setUp (stray, 1, 2, "Stray");
    const int pairGap = 8; // samples between the pair's two capsules for this source

    // Spot 2 is wired out of phase; spot 3 goes through a phase-shifting stage.
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    std::vector<float> outL[numSources], outR[numSources], wantL[numSources], wantR[numSources], pairOutL, pairOutR;
    int cursor = 0, sleepEvery = 2; // blocks per millisecond of real time
    bool record = false;

    const auto run = [&] (double seconds)
    {
        for (int s = 0; s < numSources; ++s) { outL[s].clear(); outR[s].clear(); wantL[s].clear(); wantR[s].clear(); }
        pairOutL.clear(); pairOutR.clear();
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
                        outL[s].push_back (buf.getSample (0, i));           outR[s].push_back (buf.getSample (1, i));
                        wantL[s].push_back (at (s, cursor + i - toL[s]));   wantR[s].push_back (at (s, cursor + i - toR[s]));
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

            for (int i = 0; i < block; ++i) { buf.setSample (0, i, at (numSources, cursor + i)); buf.setSample (1, i, 0.0f); }
            stray.processBlock (buf, midi);

            if ((cursor / block) % sleepEvery == 0) juce::Thread::sleep (1);
        }
    };
    // A new phase filter is prepared on a background thread; give it real time to arrive, as it has in a session.
    const auto listen = [&] (double seconds) { juce::Thread::sleep (300); run (1.0); record = true; run (seconds); record = false; };
    const auto tail = [] (const std::vector<float>& v, int skipEnd = 0) { return std::vector<float> (v.end() - 48000 - skipEnd, v.end() - skipEnd); };

    bool ok = true;
    const auto check = [&] (bool cond, const juce::String& what) { ok &= cond; std::printf ("%-64s %s\n", what.toRawUTF8(), cond ? "ok" : "FAIL"); };

    //==========================================================================
    run (3.0);
    bool untouched = true;
    for (auto& m : hub.snapshot()) untouched &= ! m.results.any();
    check (untouched, "playing without Analyze measures nothing");

    hub.startAnalysis();
    run (12.0);
    hub.stopAnalysis();
    const auto frozen = hub.snapshot();
    run (4.0);
    bool kept = true;
    {
        const auto now = hub.snapshot();
        for (int i = 0; i < now.size(); ++i)
            for (int r = 0; r < kNumRefs; ++r)
                kept &= now[i].results.vs[r].frames == frozen[i].results.vs[r].frames
                        && juce::exactlyEqual (now[i].results.vs[r].delaySamples, frozen[i].results.vs[r].delaySamples);
    }
    check (kept, "playing again after Stop does not re-analyse");

    for (int s = 0; s < numSources; ++s)
    {
        const auto m = info (spot[s]);
        const auto& vs = m.results.vs;
        std::printf ("  %s: vs L %.3f (want %d)  vs R %.3f (want %d)  vs sum %.2f  snr %.0f/%.0f  on ref %d  placed (%.2f, %.2f)  delay %.3f ms  pan %+.2f\n",
                     m.state.name.toRawUTF8(), vs[0].delaySamples, toSpot - toL[s], vs[2].delaySamples, toSpot - toR[s], vs[3].delaySamples,
                     vs[0].snr, vs[2].snr, m.applied.ref, m.state.x, m.state.y, m.applied.delayLms, m.applied.pan);
        const double tol = s == 2 ? 4.0 : 0.05; // the phase-shifted spot has no single "true" delay
        check (vs[0].reliable && vs[2].reliable && std::abs (vs[0].delaySamples - (toSpot - toL[s])) < tol
                   && std::abs (vs[2].delaySamples - (toSpot - toR[s])) < tol, m.state.name + ": delays to L and R");
        check (vs[3].valid && vs[3].reliable, m.state.name + ": also measured against the sum of the main pair");
        if (s != 2) // a phase-shifted signal has no single polarity; its phase filter is checked below
            check ((vs[0].polarity < 0) == (s == 1), m.state.name + ": polarity");
        check (std::hypot (m.state.x - sources[s].x, m.state.y - sources[s].y) < (s == 2 ? 0.8 : 0.3), m.state.name + ": placed on the plan");
        check (m.applied.ref == (toL[s] <= toR[s] ? 0 : 2), m.state.name + ": lined up with the nearest main microphone");
    }
    {
        const auto m = info (pairSpot);
        const auto l = effectiveResult (m.results, m.state, 0, true);
        check (m.isStereoPair() && std::abs (m.results.vs[0].delaySamples - (toSpot - toL[0])) < 0.05
                   && std::abs (m.results.vsRight[0].delaySamples - (toSpot + pairGap - toL[0])) < 0.05, "stereo spot: each capsule measured");
        check (std::abs (l.delaySamples - (toSpot + pairGap * 0.5 - toL[0])) < 0.05
                   && juce::exactlyEqual (m.applied.delayLms, m.applied.delayRms) && m.applied.delayLms > 1.0, "stereo spot: one common delay for the pair");
    }
    {
        const auto m = info (stray);
        std::printf ("  Stray: snr %.1f, note: %s\n", m.results.vs[0].snr, m.applied.note.toRawUTF8());
        check (m.results.vs[0].valid && ! m.results.vs[0].reliable && m.applied.delayLms < 1e-9, "unrelated microphone: measured, judged unsure, left alone");
    }

    //==========================================================================
    // Listen to the result, with the sum check running as the Check view would.
    hub.setCheck (true, -1);
    listen (4.0);
    const auto all = hub.getCheck();
    const auto& nearest0 = toL[0] <= toR[0] ? wantL[0] : wantR[0];
    const auto& nearest1 = toL[1] <= toR[1] ? wantL[1] : wantR[1];
    check (match (tail (outL[0]), tail (nearest0)) > 0.999 && match (tail (outR[0]), tail (nearest0)) > 0.999, "spot 1: both sides line up with its nearest main mic");
    check (match (tail (outL[1]), tail (nearest1)) > 0.999, "spot 2: polarity corrected and aligned");
    {
        const auto a0 = info (spot[0]).applied, a1 = info (spot[1]).applied;
        check (a0.pan < -0.05 && a0.gainL > a0.gainR, "spot 1 (left of centre) is panned left");
        check (a1.pan > 0.05 && a1.gainR > a1.gainL, "spot 2 (right of centre) is panned right");
    }
    {
        // The pair's own image must survive: right still trails left by the same gap.
        const auto l = tail (pairOutL), r = tail (pairOutR);
        check (match (std::vector<float> (l.begin(), l.end() - pairGap), std::vector<float> (r.begin() + pairGap, r.end())) > 0.999,
               "stereo spot: its internal image is kept");
    }
    const auto& nearest2 = toL[2] <= toR[2] ? wantL[2] : wantR[2];
    const double withPhase = match (tail (outL[2]), tail (nearest2));

    hub.setCheck (true, spot[0].slot());
    run (3.0);
    const auto one = hub.getCheck();
    hub.setCheck (false, -1);
    std::printf ("  sum of all mics: %+.1f dB with corrections (lows %+.1f, mids %+.1f, highs %+.1f)\n", all.gainAll, all.gainLow, all.gainMid, all.gainHigh);
    std::printf ("  main + spot 1: %+.1f dB, correlation %.2f -> %.2f\n", one.gainAll, one.correlationBefore, one.correlationAfter);
    check (all.valid && all.gainLow > 1.5 && all.gainAll > 0.3, "check: aligned microphones add up fuller");
    check (one.valid && one.hasCorrelation && one.correlationAfter > one.correlationBefore + 0.3, "check: spot 1 correlates with the main array once aligned");

    // Pan by hand, and its bypass.
    edit (spot[0], [] (MicState& m) { m.panFollowsPlan = false; m.pan = 1.0; });
    check (info (spot[0]).applied.gainL < 0.01f && info (spot[0]).applied.gainR > 0.99f, "pan by hand: hard right sends nothing left");
    edit (spot[0], [] (MicState& m) { m.panOn = false; });
    check (std::abs (info (spot[0]).applied.gainL - info (spot[0]).applied.gainR) < 1e-6f, "pan bypass: back to centre");
    edit (spot[0], [] (MicState& m) { m.panOn = true; m.panFollowsPlan = true; m.timeDifference = true; });
    listen (3.0);
    check (match (tail (outL[0]), tail (wantL[0])) > 0.999 && match (tail (outR[0]), tail (wantR[0])) > 0.999, "time difference on: left on main L, right on main R");
    edit (spot[0], [] (MicState& m) { m.timeDifference = false; });

    // Phase by frequency, its range and its bypass.
    edit (spot[2], [] (MicState& m) { m.phaseOn = false; });
    listen (3.0);
    const double withoutPhase = match (tail (outL[2]), tail (nearest2));
    edit (spot[2], [] (MicState& m) { m.phaseOn = true; m.phaseHighHz = 500.0; });
    listen (3.0);
    const double lowsOnly = match (tail (outL[2]), tail (nearest2));
    edit (spot[2], [] (MicState& m) { m.phaseHighHz = 20000.0; });
    std::printf ("  spot 3 match with its main mic: delay only %.3f, phase up to 500 Hz %.3f, full phase %.3f (%d taps)\n",
                 withoutPhase, lowsOnly, withPhase, info (spot[2]).applied.phaseTaps);
    check (withPhase > 0.95 && withPhase > withoutPhase + 0.05, "phase by frequency brings the phase-shifted spot in line");
    check (lowsOnly > withoutPhase + 0.02 && lowsOnly < withPhase - 0.05, "phase range: limited to the lows, it corrects only part");

    // A delay between two samples.
    edit (spot[0], [&] (MicState& m) { m.offsetMs = 0.5 / sr * 1000.0; });
    listen (3.0);
    {
        // The same signal half a sample later, built with a long sinc.
        const auto& full = nearest0;
        const int first = (int) full.size() - 48000;
        std::vector<float> half (48000);
        for (int i = 0; i < 48000; ++i)
        {
            double acc = 0.0;
            for (int k = -40; k <= 40; ++k)
            {
                const int index = first + i + k;
                if (index < 0 || index >= (int) full.size()) continue;
                const double t = -0.5 - k; // value at position (first + i - 0.5)
                acc += full[(size_t) index] * std::sin (kPi * t) / (kPi * t) * (0.5 + 0.5 * std::cos (kPi * t / 41.0));
            }
            half[(size_t) i] = (float) acc;
        }
        const double between = match (tail (outL[0]), half), whole = match (tail (outL[0]), tail (nearest0));
        std::printf ("  half-sample offset: match with the half-sample-late signal %.4f, with the on-sample one %.4f\n", between, whole);
        check (between > 0.999 && between > whole + 0.003, "delays between two samples are applied as such");
    }
    edit (spot[0], [] (MicState& m) { m.offsetMs = 0.0; });

    // Picking another peak by hand, and going back.
    {
        const double automatic = info (spot[0]).applied.delayLms;
        edit (spot[0], [&] (MicState& m) { m.peakLocked[0] = true; m.peakLag[0] = toSpot - toL[0] - 100.0; }); // further than main R: still used, because picked
        const double picked = info (spot[0]).applied.delayLms;
        edit (spot[0], [] (MicState& m) { m.peakLocked[0] = false; });
        check (std::abs (picked - automatic - 100.0 / sr * 1000.0) < 1e-3 && std::abs (info (spot[0]).applied.delayLms - automatic) < 1e-9,
               "a delay picked by hand is used, and can be undone");
    }

    // Reference choice.
    edit (spot[1], [] (MicState& m) { m.alignTo = AlignTo::left; });
    check (info (spot[1]).applied.ref == 0 && std::abs (info (spot[1]).applied.delayLms - (toL[1] - toSpot) / sr * 1000.0) < 0.002, "reference by hand: lined up with main L");
    edit (spot[1], [] (MicState& m) { m.alignTo = AlignTo::nearest; });

    // Bypass switches.
    edit (spot[0], [] (MicState& m) { m.delayOn = false; });
    check (info (spot[0]).applied.delayLms < 1e-9 && info (spot[0]).applied.delayRms < 1e-9, "delay bypass removes the delay");
    edit (spot[0], [] (MicState& m) { m.delayOn = true; });
    edit (spot[1], [] (MicState& m) { m.polarityOn = false; });
    check (info (spot[1]).applied.polarity > 0, "polarity bypass leaves polarity alone");
    edit (spot[1], [] (MicState& m) { m.polarityOn = true; });

    //==========================================================================
    // Full-resolution mode: every instance takes on the same latency, alignment is unchanged.
    { auto sc = hub.getScene(); sc.precision = 1; hub.setScene (sc); }
    for (auto* p : { &mainPair, &spot[0], &spot[1], &spot[2], &pairSpot, &stray }) p->updateLatency();
    const int latency = Hub::latencyFor (1, sr);
    listen (4.0);
    {
        const auto late = [&] (const std::vector<float>& v) { return tail (v, latency); };
        check (mainPair.getLatencySamples() == latency && spot[0].getLatencySamples() == latency, "full resolution: every instance reports the same latency");
        check (match (tail (outL[0]), late (nearest0)) > 0.999, "full resolution: spot 1 is still aligned, one latency later");
        const double full = match (tail (outL[2]), late (nearest2));
        std::printf ("  full resolution: spot 3 match %.3f with %d taps (live mode: %.3f)\n", full, info (spot[2]).applied.phaseTaps, withPhase);
        check (info (spot[2]).applied.phaseTaps == 2 * latency && full > withPhase + 0.005, "full resolution: longer phase filter, more precise");
    }
    hub.setBypassed (true);
    listen (2.0);
    check (match (tail (outL[0]), tail (std::vector<float> (wantL[0].begin(), wantL[0].end()), 0)) < 0.9, "bypass all: the spot is no longer aligned");
    hub.setBypassed (false);
    { auto sc = hub.getScene(); sc.precision = 0; hub.setScene (sc); }

    //==========================================================================
    // Refine the phase on the whole sum: must finish, and must not make the sum thinner.
    hub.setCheck (true, -1);
    sleepEvery = 1; // nearer real time: the check looks at one frame per tenth of a second
    run (4.0);
    const double lowsBefore = hub.getCheck().gainAll;
    hub.startRefine (2);
    for (int i = 0; i < 12 && hub.isRefining(); ++i) run (2.0);
    const bool finished = ! hub.isRefining();
    listen (4.0);
    const double lowsAfter = hub.getCheck().gainAll, refinedLive = match (tail (outL[2]), tail (nearest2));
    sleepEvery = 2;
    hub.setCheck (false, -1);
    float trimmed = 0.0f;
    for (float t : info (spot[2]).results.phaseTrim) trimmed = juce::jmax (trimmed, std::abs (t));
    std::printf ("  refine on the sum: gain of the sum %+.2f dB -> %+.2f dB, largest trim %.2f rad\n", lowsBefore, lowsAfter, trimmed);
    check (finished && lowsAfter > lowsBefore - 0.05 && refinedLive > withPhase - 0.05, "refine on the sum completes and does not thin the sum");

    //==========================================================================
    // Save and restore keeps the analysis.
    edit (spot[0], [] (MicState& m) { m.alignTo = AlignTo::right; m.phaseAmount = 0.5; });
    juce::MemoryBlock saved;
    spot[0].getStateInformation (saved);
    const auto beforeReset = info (spot[0]);
    hub.resetAnalysis();
    bool cleared = true;
    for (auto& mi : hub.snapshot()) cleared &= ! mi.results.any();
    check (cleared, "Reset analysis clears every result");
    spot[0].setStateInformation (saved.getData(), (int) saved.getSize());
    {
        const auto m = info (spot[0]);
        check (m.results.vs[0].reliable && std::abs (m.results.vs[0].delaySamples - beforeReset.results.vs[0].delaySamples) < 0.01
                   && m.results.curve[0].valid && m.results.vs[0].numPeaks == beforeReset.results.vs[0].numPeaks
                   && m.state.alignTo == AlignTo::right && std::abs (m.state.phaseAmount - 0.5) < 1e-9,
               "session reload restores the analysis, its candidates and the settings");
    }
    return ok ? 0 : 1;
}
