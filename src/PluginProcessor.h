#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "Hub.h"

/** One read position in a delay ring, with a short crossfade when the delay changes. */
class DelayTap
{
public:
    void prepare (int fadeSamples) noexcept { fadeLength = juce::jmax (1, fadeSamples); current = target = 0; pending = -1; fade = 0; }

    void setTarget (int delay) noexcept
    {
        if (delay == target) { pending = -1; return; }
        if (fade > 0)        { pending = delay; return; }
        target = delay;
        fade = fadeLength;
    }

    float read (const float* ring, int mask, int writePos) noexcept
    {
        float out = ring[(size_t) ((writePos - current) & mask)];
        if (fade > 0)
        {
            const float next = ring[(size_t) ((writePos - target) & mask)];
            out += (next - out) * (1.0f - (float) fade / (float) fadeLength);
            if (--fade == 0)
            {
                current = target;
                if (pending >= 0) { const int p = pending; pending = -1; setTarget (p); }
            }
        }
        return out;
    }

private:
    int current = 0, target = 0, pending = -1, fade = 0, fadeLength = 1;
};

class ArrayLabProcessor : public juce::AudioProcessor
{
public:
    ArrayLabProcessor();
    ~ArrayLabProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ArrayLab"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;
    void updateTrackProperties (const TrackProperties&) override;

    arraylab::Hub& hub() { return *sharedHub; }
    int slot() const noexcept { return slotIndex; }

private:
    juce::SharedResourcePointer<arraylab::Hub> sharedHub;
    int slotIndex = -1;

    std::vector<float> ring[2];
    int ringMask = 0, writePos = 0;
    DelayTap tap[2];
    juce::SmoothedValue<float> gain[2];
    juce::dsp::Convolution phaseFilter { juce::dsp::Convolution::Latency { 0 } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrayLabProcessor)
};
