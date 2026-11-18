#include "PluginProcessor.h"
#include "PluginEditor.h"

static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { "testDelay", 1 },
                                                           "Test delay (samples)", 0, 20000, 0));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "testInvert", 1 },
                                                            "Test polarity invert", false));
    return layout;
}

ArrayLabProcessor::ArrayLabProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "params", createLayout())
{
    delayParam = params.getRawParameterValue ("testDelay");
    invertParam = params.getRawParameterValue ("testInvert");
    slotIndex = sharedHub->registerInstance();
}

ArrayLabProcessor::~ArrayLabProcessor()
{
    sharedHub->unregisterInstance (slotIndex);
}

bool ArrayLabProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in.isDisabled() || out.isDisabled())
        return false;
    // Any matching width (mono, stereo, surround...), plus mono in -> stereo out.
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

void ArrayLabProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock,
                                  (juce::uint32) juce::jmax (1, getTotalNumInputChannels()) };
    testDelay.prepare (spec);
    testDelay.reset();
    sharedHub->prepare (slotIndex, sampleRate, samplesPerBlock,
                        getTotalNumInputChannels(), getTotalNumOutputChannels());
}

void ArrayLabProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n = buffer.getNumSamples();

    // Test controls, applied before the capture point so the probe can verify itself.
    testDelay.setDelay ((float) juce::roundToInt (delayParam->load()));
    const float gain = invertParam->load() > 0.5f ? -1.0f : 1.0f;
    for (int ch = 0; ch < numIn; ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            testDelay.pushSample (ch, d[i]);
            d[i] = gain * testDelay.popSample (ch);
        }
    }

    bool hasPlayhead = false, playing = false;
    juce::int64 pos = 0;
    if (auto* ph = getPlayHead())
        if (auto info = ph->getPosition())
        {
            playing = info->getIsPlaying();
            if (auto t = info->getTimeInSamples())
            {
                hasPlayhead = true;
                pos = *t;
            }
        }

    sharedHub->push (slotIndex, numIn > 0 ? buffer.getReadPointer (0) : nullptr, n, hasPlayhead, playing, pos);

    // Mono in -> stereo out: duplicate for now. The real panner will go here.
    for (int ch = numIn; ch < numOut; ++ch)
    {
        if (numIn > 0) buffer.copyFrom (ch, 0, buffer, 0, 0, n);
        else           buffer.clear (ch, 0, n);
    }
}

void ArrayLabProcessor::updateTrackProperties (const TrackProperties& props)
{
    if (props.name.has_value() && props.name->isNotEmpty())
    {
        hostName = *props.name;
        if (userName.isEmpty())
            sharedHub->setName (slotIndex, hostName, true);
    }
}

void ArrayLabProcessor::setUserName (const juce::String& name)
{
    userName = name.trim();
    if (userName.isNotEmpty())      sharedHub->setName (slotIndex, userName, false);
    else if (hostName.isNotEmpty()) sharedHub->setName (slotIndex, hostName, true);
}

void ArrayLabProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = params.copyState();
    state.setProperty ("micName", userName, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void ArrayLabProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        auto state = juce::ValueTree::fromXml (*xml);
        if (state.isValid())
        {
            params.replaceState (state);
            setUserName (state.getProperty ("micName").toString());
        }
    }
}

juce::AudioProcessorEditor* ArrayLabProcessor::createEditor() { return new ArrayLabEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ArrayLabProcessor(); }
