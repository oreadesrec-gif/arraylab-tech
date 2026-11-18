#pragma once
#include <juce_core/juce_core.h>
#include "DelayEstimator.h"
#include <array>
#include <atomic>
#include <memory>

namespace arraylab
{
/** Process-wide meeting point for every plugin instance.

    Pyramix has no side-chain, so instances cannot exchange audio through the
    mixer. Instead each instance writes its input into a ring buffer addressed
    by the host timeline position, and a background thread compares every
    instance against the chosen reference. */
class Hub : private juce::Thread
{
public:
    static constexpr int maxSlots = 128;

    struct SlotInfo
    {
        int index = -1;
        juce::String name;
        bool hostNamed = false;
        double sampleRate = 0.0;
        int blockSize = 0, numIn = 0, numOut = 0;
        bool hasPlayhead = false, playing = false;
        juce::int64 timelinePos = 0;
        juce::int64 blocks = 0;
        DelayResult result;
    };

    Hub();
    ~Hub() override;

    int  registerInstance();
    void unregisterInstance (int slot);

    // Not real-time: call from prepareToPlay.
    void prepare (int slot, double sampleRate, int blockSize, int numIn, int numOut);
    void setName (int slot, const juce::String& name, bool fromHost);

    // Real-time safe: call from processBlock.
    void push (int slot, const float* samples, int numSamples,
               bool hasPlayhead, bool playing, juce::int64 timelinePos) noexcept;

    juce::Array<SlotInfo> snapshot() const;
    int  getReference() const noexcept        { return reference.load(); }
    void setReference (int slot) noexcept     { reference = slot; }
    void resetAnalysis() noexcept             { resetRequested = true; }

private:
    struct Slot
    {
        std::atomic<bool> active { false };
        std::atomic<bool> playing { false }, hasPlayhead { false };
        std::atomic<juce::int64> writeEnd { 0 }, runStart { 0 }, lastPos { 0 }, blocks { 0 };
        std::atomic<double> sampleRate { 0.0 };
        std::atomic<int> blockSize { 0 }, numIn { 0 }, numOut { 0 };
        juce::int64 expected = -1; // audio thread only

        juce::CriticalSection lock; // guards ring reallocation, name and result
        juce::HeapBlock<float> ring;
        int ringSize = 0;
        juce::String name;
        bool hostNamed = false;
        DelayResult result;

        // analysis thread only
        DelayEstimator estimator;
    };

    void run() override;
    void analyse();
    bool readFrame (Slot&, juce::int64 start, int length, float* dest);

    std::array<Slot, maxSlots> slots;
    std::atomic<int> reference { -1 };
    std::atomic<bool> resetRequested { false };
    juce::CriticalSection registerLock;

    // analysis thread state
    int frameSize = 0, analysedRef = -1;
    juce::int64 framePos = -1;
    std::unique_ptr<Fft> fft;
    std::unique_ptr<FrameSpectrum> refSpectrum, tgtSpectrum;
    std::vector<float> frame;
    std::vector<std::complex<float>> scratch;
};
}
