// Headless host: several plugin instances on a shared timeline, as a DAW would run them.
#include "../src/PluginProcessor.h"
#include <random>

struct FakePlayHead : juce::AudioPlayHead
{
    juce::int64 pos = 0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo i;
        i.setTimeInSamples (pos);
        i.setIsPlaying (playing);
        return i;
    }
};

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double sr = 96000.0;
    const int block = 512, numMics = 4;
    const int delays[numMics] = { 0, 731, 2500, 96 };
    const bool inverted[numMics] = { false, false, true, false };

    FakePlayHead head;
    juce::OwnedArray<ArrayLabProcessor> mics;
    for (int m = 0; m < numMics; ++m)
    {
        auto* p = mics.add (new ArrayLabProcessor());
        p->setPlayHead (&head);
        p->setPlayConfigDetails (1, 2, sr, block);
        p->prepareToPlay (sr, block);
        p->params.getParameter ("testDelay")->setValueNotifyingHost (p->params.getParameter ("testDelay")->convertTo0to1 ((float) delays[m]));
        p->params.getParameter ("testInvert")->setValueNotifyingHost (inverted[m] ? 1.0f : 0.0f);
    }

    std::mt19937 rng (7);
    std::normal_distribution<float> g (0.0f, 0.2f);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    float lp = 0.0f;
    std::vector<float> src ((size_t) block);

    auto runFor = [&] (double seconds, juce::int64 startPos)
    {
        head.pos = startPos;
        for (int b = 0; b < (int) (seconds * sr / block); ++b)
        {
            for (auto& v : src) { lp += 0.2f * (g (rng) - lp); v = lp; }
            for (auto* p : mics)
            {
                buf.clear();
                buf.copyFrom (0, 0, src.data(), block);
                p->processBlock (buf, midi);
            }
            head.pos += block;
            juce::Thread::sleep (1); // let the analysis thread keep up, roughly like real time
        }
    };

    runFor (6.0, 480000);
    runFor (6.0, 96000);   // locate backwards mid-session, as when auditioning another passage
    juce::Thread::sleep (500);

    bool ok = true;
    for (auto& s : mics[0]->hub().snapshot())
    {
        const int m = s.index;
        const bool isRef = m == mics[0]->hub().getReference();
        const bool good = isRef || (s.result.valid && std::abs (s.result.delaySamples - delays[m]) < 0.1
                                    && (s.result.polarity < 0) == inverted[m]);
        ok &= good && s.numIn == 1 && s.numOut == 2;
        std::printf ("%-6s in>out %d>%d  expected %5d  got %9.3f  pol %+d  conf %.2f  frames %d  %s\n",
                     s.name.toRawUTF8(), s.numIn, s.numOut, delays[m], s.result.delaySamples, s.result.polarity,
                     s.result.confidence, s.result.frames, isRef ? "(reference)" : good ? "ok" : "FAIL");
    }
    mics.clear();
    return ok ? 0 : 1;
}
