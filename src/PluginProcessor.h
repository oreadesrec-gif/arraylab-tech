#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "Fractional.h"
#include "Hub.h"

/** One read position in a delay ring. Delays may fall between two samples; a
    change of delay is crossfaded over a few milliseconds. */
class DelayTap
{
public:
    void prepare (int fadeSamples) noexcept
    {
        fadeLength = juce::jmax (1, fadeSamples);
        current = target = {};
        wanted = pendingDelay = -1.0f;
        fade = 0;
    }

    void setTarget (float delay) noexcept
    {
        if (std::abs (delay - wanted) < 1.0e-4f) return;
        wanted = delay;
        if (fade > 0) { pendingDelay = delay; return; }
        target = make (delay);
        fade = fadeLength;
    }

    float read (const float* ring, int mask, int writePos) noexcept
    {
        float out = readAt (current, ring, mask, writePos);
        if (fade > 0)
        {
            out += (readAt (target, ring, mask, writePos) - out) * (1.0f - (float) fade / (float) fadeLength);
            if (--fade == 0)
            {
                current = target;
                if (pendingDelay >= 0.0f) { const float p = pendingDelay; pendingDelay = -1.0f; target = make (p); fade = fadeLength; }
            }
        }
        return out;
    }

private:
    struct Setting
    {
        int whole = 0;
        bool fractional = false;
        float kernel[arraylab::kSincTaps];
    };

    static Setting make (float delay) noexcept
    {
        Setting s;
        s.whole = (int) std::floor (delay);
        const double fraction = (double) delay - s.whole;
        if (fraction > 0.999) ++s.whole;
        else if (fraction > 0.001 && s.whole >= arraylab::kSincCentre)
        {
            s.fractional = true;
            arraylab::makeSincKernel (fraction, s.kernel);
        }
        else if (fraction >= 0.5) ++s.whole; // too short a delay to interpolate: nearest sample
        return s;
    }

    static float readAt (const Setting& s, const float* ring, int mask, int writePos) noexcept
    {
        if (! s.fractional)
            return ring[(size_t) ((writePos - s.whole) & mask)];
        float sum = 0.0f;
        const int first = writePos - (s.whole - arraylab::kSincCentre);
        for (int k = 0; k < arraylab::kSincTaps; ++k)
            sum += s.kernel[k] * ring[(size_t) ((first - k) & mask)];
        return sum;
    }

    Setting current, target;
    float wanted = -1.0f, pendingDelay = -1.0f;
    int fade = 0, fadeLength = 1;
};

class ArrayLabProcessor : public juce::AudioProcessor,
                          private juce::Timer
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

    /** Tells the host the latency of the current precision mode (message thread). */
    void updateLatency();

    arraylab::Hub& hub() { return *sharedHub; }
    int slot() const noexcept { return slotIndex; }

private:
    void timerCallback() override { updateLatency(); }

    juce::SharedResourcePointer<arraylab::Hub> sharedHub;
    int slotIndex = -1;

    std::vector<float> ring[2];
    int ringMask = 0, writePos = 0;
    DelayTap tap[2];
    juce::SmoothedValue<float> gain[2], cross[2];
    juce::dsp::Convolution phaseFilter { juce::dsp::Convolution::Latency { 0 } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrayLabProcessor)
};
