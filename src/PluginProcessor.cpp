#include "PluginProcessor.h"
#include "PluginEditor.h"

ArrayLabProcessor::ArrayLabProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    slotIndex = sharedHub->registerInstance();
    sharedHub->setFirSink (slotIndex, [this] (const std::vector<float>& taps)
    {
        juce::AudioBuffer<float> ir (1, (int) taps.size());
        ir.copyFrom (0, 0, taps.data(), (int) taps.size());
        phaseFilter.loadImpulseResponse (std::move (ir), getSampleRate(), juce::dsp::Convolution::Stereo::no,
                                         juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    });
    startTimerHz (4);
}

void ArrayLabProcessor::updateLatency()
{
    const int wanted = arraylab::Hub::latencyFor (sharedHub->precision(), getSampleRate());
    if (wanted != getLatencySamples())
        setLatencySamples (wanted);
}

ArrayLabProcessor::~ArrayLabProcessor()
{
    stopTimer();
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
    // Room for 0.6 s of alignment delay plus the full-resolution latency.
    const int size = juce::nextPowerOfTwo ((int) (sampleRate * 0.6) + arraylab::Hub::latencyFor (1, sampleRate));
    for (auto& r : ring) r.assign ((size_t) size, 0.0f);
    ringMask = size - 1;
    writePos = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        tap[ch].prepare ((int) (sampleRate * 0.02));
        gain[ch].reset (sampleRate, 0.02);
        gain[ch].setCurrentAndTargetValue (1.0f);
        cross[ch].reset (sampleRate, 0.02);
        cross[ch].setCurrentAndTargetValue (0.0f);
    }
    phaseFilter.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, 2 });
    setLatencySamples (arraylab::Hub::latencyFor (sharedHub->precision(), sampleRate));
    sharedHub->prepare (slotIndex, sampleRate, samplesPerBlock,
                        getTotalNumInputChannels(), getTotalNumOutputChannels());
}

void ArrayLabProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n = buffer.getNumSamples();
    if (numIn == 0 || slotIndex < 0)
        return;

    // Capture the untouched input for the analysis.
    bool hasPlayhead = false, playing = false;
    juce::int64 pos = 0;
    if (auto* ph = getPlayHead())
        if (auto info = ph->getPosition())
        {
            playing = info->getIsPlaying();
            if (auto t = info->getTimeInSamples()) { hasPlayhead = true; pos = *t; }
        }
    sharedHub->push (slotIndex, buffer.getReadPointer (0), numIn > 1 ? buffer.getReadPointer (1) : nullptr,
                     n, hasPlayhead, playing, pos);

    if (numIn > 2 || numOut > 2 || ringMask == 0)
        return; // surround tracks are measured but passed through untouched

    const auto& live = sharedHub->live (slotIndex);
    const bool bypass = sharedHub->bypassed();
    const bool fromMono = live.fromMono.load();
    // Bypassed, only the session latency remains, so the host's compensation stays valid.
    const float limit = (float) (ringMask - arraylab::kSincTaps);
    const float held = (float) live.bypassDelay.load();
    tap[0].setTarget (juce::jlimit (0.0f, limit, bypass ? held : live.delayL.load()));
    tap[1].setTarget (juce::jlimit (0.0f, limit, bypass ? held : live.delayR.load()));
    const float trim = sharedHub->outputTrim();
    // Solo in context: while one microphone is soloed, only it and the main arrays are heard.
    const int solo = sharedHub->solo();
    const float heard = solo >= 0 && solo != slotIndex && ! live.isMain.load() ? 0.0f : 1.0f;
    gain[0].setTargetValue (heard * (bypass ? live.bypassGainL.load() : live.gainL.load() * trim));
    gain[1].setTargetValue (heard * (bypass ? live.bypassGainR.load() : live.gainR.load() * trim));
    cross[0].setTargetValue (heard * (bypass ? 0.0f : live.crossLR.load() * trim)); // left capsule into the right bus
    cross[1].setTargetValue (heard * (bypass ? 0.0f : live.crossRL.load() * trim)); // right capsule into the left bus

    // Multiband phase correction (its latency is already taken out of the delays).
    {
        juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) (fromMono ? 1 : numIn), (size_t) n);
        juce::dsp::ProcessContextReplacing<float> context (block);
        context.isBypassed = bypass || ! live.phaseOn.load();
        phaseFilter.process (context);
    }

    auto* c0 = buffer.getWritePointer (0);
    auto* c1 = numOut > 1 ? buffer.getWritePointer (1) : nullptr;
    auto* r0 = ring[0].data();
    auto* r1 = ring[1].data();

    for (int i = 0; i < n; ++i)
    {
        r0[(size_t) writePos] = c0[i];
        if (fromMono)
        {
            c0[i] = tap[0].read (r0, ringMask, writePos) * gain[0].getNextValue();
            c1[i] = tap[1].read (r0, ringMask, writePos) * gain[1].getNextValue();
        }
        else if (c1 != nullptr)
        {
            r1[(size_t) writePos] = c1[i];
            const float l = tap[0].read (r0, ringMask, writePos), r = tap[1].read (r1, ringMask, writePos);
            c0[i] = l * gain[0].getNextValue() + r * cross[1].getNextValue();
            c1[i] = r * gain[1].getNextValue() + l * cross[0].getNextValue();
        }
        else
            c0[i] = tap[0].read (r0, ringMask, writePos) * gain[0].getNextValue();

        writePos = (writePos + 1) & ringMask;
    }
}

void ArrayLabProcessor::updateTrackProperties (const TrackProperties& props)
{
    if (props.name.has_value() && props.name->isNotEmpty())
        sharedHub->setHostName (slotIndex, *props.name);
}

void ArrayLabProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = sharedHub->saveInstance (slotIndex).createXml())
        copyXmlToBinary (*xml, dest);
}

void ArrayLabProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        sharedHub->restoreInstance (slotIndex, juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* ArrayLabProcessor::createEditor() { return new ArrayLabEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ArrayLabProcessor(); }
