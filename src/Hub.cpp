#include "Hub.h"

namespace arraylab
{
Hub::Hub() : juce::Thread ("ArrayLab analysis")
{
    startThread (juce::Thread::Priority::low);
}

Hub::~Hub()
{
    stopThread (2000);
}

int Hub::registerInstance()
{
    const juce::ScopedLock sl (registerLock);
    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (! s.active.load())
        {
            {
                const juce::ScopedLock l (s.lock);
                s.name = "Mic " + juce::String (i + 1);
                s.hostNamed = false;
                s.result = {};
            }
            s.writeEnd = 0; s.runStart = 0; s.lastPos = 0; s.blocks = 0;
            s.playing = false; s.hasPlayhead = false;
            s.expected = -1;
            s.active = true;
            if (reference.load() < 0)
                reference = i;
            return i;
        }
    }
    return -1;
}

void Hub::unregisterInstance (int slot)
{
    if (slot < 0) return;
    const juce::ScopedLock sl (registerLock);
    slots[(size_t) slot].active = false;
    if (reference.load() == slot)
    {
        reference = -1;
        for (int i = 0; i < maxSlots; ++i)
            if (slots[(size_t) i].active.load()) { reference = i; break; }
    }
}

void Hub::prepare (int slot, double sampleRate, int blockSize, int numIn, int numOut)
{
    if (slot < 0) return;
    auto& s = slots[(size_t) slot];

    // Frame of about 0.7 s, ring of eight frames.
    const int size = juce::nextPowerOfTwo ((int) (sampleRate * 0.5)) * 8;

    const juce::ScopedLock l (s.lock);
    if (size != s.ringSize)
    {
        s.ring.calloc ((size_t) size);
        s.ringSize = size;
    }
    s.sampleRate = sampleRate;
    s.blockSize = blockSize;
    s.numIn = numIn;
    s.numOut = numOut;
    s.expected = -1;
    s.playing = false;
}

void Hub::setName (int slot, const juce::String& name, bool fromHost)
{
    if (slot < 0) return;
    auto& s = slots[(size_t) slot];
    const juce::ScopedLock l (s.lock);
    s.name = name;
    s.hostNamed = fromHost;
}

void Hub::push (int slot, const float* samples, int numSamples,
                bool hasPlayhead, bool playing, juce::int64 pos) noexcept
{
    if (slot < 0) return;
    auto& s = slots[(size_t) slot];
    s.blocks.fetch_add (1, std::memory_order_relaxed);
    s.hasPlayhead = hasPlayhead;
    s.lastPos = pos;

    if (! hasPlayhead || ! playing || pos < 0 || s.ringSize == 0 || samples == nullptr)
    {
        s.playing = false;
        s.expected = -1;
        return;
    }

    if (pos != s.expected) // transport start, locate or loop
        s.runStart = pos;

    const int mask = s.ringSize - 1;
    auto* ring = s.ring.get();
    for (int i = 0; i < numSamples; ++i)
        ring[(size_t) ((pos + i) & mask)] = samples[i];

    s.expected = pos + numSamples;
    s.writeEnd = pos + numSamples;
    s.playing = true;
}

bool Hub::readFrame (Slot& s, juce::int64 start, int length, float* dest)
{
    const juce::ScopedLock l (s.lock);
    if (s.ringSize == 0 || ! s.playing.load())
        return false;

    const auto end = s.writeEnd.load();
    if (s.runStart.load() > start || end < start + length || end - start > s.ringSize - s.blockSize.load())
        return false;

    const int mask = s.ringSize - 1;
    for (int i = 0; i < length; ++i)
        dest[i] = s.ring[(size_t) ((start + i) & mask)];

    // The audio thread may have lapped us while copying.
    return s.runStart.load() <= start && s.writeEnd.load() - start <= s.ringSize;
}

void Hub::run()
{
    while (! threadShouldExit())
    {
        analyse();
        wait (100);
    }
}

void Hub::analyse()
{
    const int ref = reference.load();
    if (ref < 0 || ! slots[(size_t) ref].active.load())
        return;

    auto& r = slots[(size_t) ref];
    const double sr = r.sampleRate.load();
    if (sr <= 0.0)
        return;

    const int wanted = juce::nextPowerOfTwo ((int) (sr * 0.5));
    const bool reset = resetRequested.exchange (false) || wanted != frameSize || ref != analysedRef;

    if (wanted != frameSize)
    {
        frameSize = wanted;
        fft = std::make_unique<Fft> (frameSize);
        refSpectrum = std::make_unique<FrameSpectrum> (frameSize);
        tgtSpectrum = std::make_unique<FrameSpectrum> (frameSize);
        frame.assign ((size_t) frameSize, 0.0f);
    }

    if (reset)
    {
        analysedRef = ref;
        framePos = -1;
        for (auto& s : slots)
        {
            s.estimator.reset (frameSize);
            const juce::ScopedLock l (s.lock);
            s.result = {};
        }
    }

    if (! r.playing.load())
        return;

    // Leave a little room so instances processed later in the host cycle have caught up.
    const auto guard = (juce::int64) (sr * 0.1);
    bool changed = false;

    for (int n = 0; n < 4 && ! threadShouldExit(); ++n)
    {
        const auto end = r.writeEnd.load() - guard;
        const auto start = r.runStart.load();

        if (framePos < start || framePos > end || end - framePos > r.ringSize - frameSize * 2)
            framePos = juce::jmax (start, end - frameSize);

        if (framePos + frameSize > end)
            break;

        if (readFrame (r, framePos, frameSize, frame.data()))
        {
            refSpectrum->compute (*fft, frame.data());

            for (int i = 0; i < maxSlots; ++i)
            {
                auto& s = slots[(size_t) i];
                if (i == ref || ! s.active.load() || ! juce::approximatelyEqual (s.sampleRate.load(), sr))
                    continue;
                if (! readFrame (s, framePos, frameSize, frame.data()))
                    continue;
                tgtSpectrum->compute (*fft, frame.data());
                s.estimator.accumulate (*refSpectrum, *tgtSpectrum);
                changed = true;
            }
        }
        framePos += frameSize / 2;
    }

    if (changed)
        for (int i = 0; i < maxSlots; ++i)
        {
            auto& s = slots[(size_t) i];
            if (i == ref || ! s.active.load() || s.estimator.frames() == 0)
                continue;
            const auto res = s.estimator.estimate (*fft, sr, scratch);
            const juce::ScopedLock l (s.lock);
            s.result = res;
        }
}

juce::Array<Hub::SlotInfo> Hub::snapshot() const
{
    juce::Array<SlotInfo> out;
    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = const_cast<Slot&> (slots[(size_t) i]);
        if (! s.active.load())
            continue;
        SlotInfo info;
        info.index = i;
        info.sampleRate = s.sampleRate.load();
        info.blockSize = s.blockSize.load();
        info.numIn = s.numIn.load();
        info.numOut = s.numOut.load();
        info.hasPlayhead = s.hasPlayhead.load();
        info.playing = s.playing.load();
        info.timelinePos = s.lastPos.load();
        info.blocks = s.blocks.load();
        {
            const juce::ScopedLock l (s.lock);
            info.name = s.name;
            info.hostNamed = s.hostNamed;
            info.result = s.result;
        }
        out.add (info);
    }
    return out;
}
}
