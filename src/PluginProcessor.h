#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "Hub.h"

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

    const juce::String getName() const override { return "ArrayLab Probe"; }
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

    void setUserName (const juce::String&);
    juce::String getUserName() const { return userName; }

    arraylab::Hub& hub() { return *sharedHub; }
    int slot() const noexcept { return slotIndex; }

    juce::AudioProcessorValueTreeState params;

private:
    juce::SharedResourcePointer<arraylab::Hub> sharedHub;
    int slotIndex = -1;
    juce::String userName, hostName;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> testDelay { 1 << 16 };
    std::atomic<float>* delayParam = nullptr;
    std::atomic<float>* invertParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrayLabProcessor)
};
