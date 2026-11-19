#pragma once
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "DelayEstimator.h"
#include "Scene.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace arraylab
{
/** Everything the user sets for one microphone. */
struct MicState
{
    juce::String name;
    bool hostNamed = false;
    Role role = Role::spot;
    int group = 0;
    Pattern pattern = Pattern::cardioid;
    double x = 0.0, y = 2.0, z = 1.5;   // metres; y is "forward" from the main array
    double angleDeg = 180.0;            // where the mic points, clockwise from forward
    double sourceDistance = 0.5;        // spot: distance from the mic to its instrument
    bool manualPosition = false;        // true once the user has dragged or typed a position

    bool delayOn = true, polarityOn = true, phaseOn = false;
    double offsetMs = 0.0;              // added after alignment (lets the main pair lead)

    bool panOn = true;                  // mono source into a stereo output
    double panAmount = 1.0;             // level difference: 0 = centre, 1 = as the main pair hears it
    double timePan = 1.0;               // time difference between L and R: 0 = none, 1 = natural
    bool monoSource = false;            // stereo track carrying a mono mic on its left channel

    // A spot on a stereo track is a pair: its two capsules sit either side of x/y, aimed around angleDeg.
    double pairSpacingCm = 17.0, pairAngleDeg = 110.0;
};

/** What the analysis measured for one microphone. Index 0 = vs L, 1 = vs C, 2 = vs R. */
struct MicResults
{
    DelayResult vs[3];
    DelayResult vsRight[3];             // second channel of a stereo spot
    std::array<std::complex<float>, kNumBands> phase {};
    bool phaseValid = false;
    int primary = -1;
};

/** Delay of a microphone against reference `r`. A stereo spot is moved as one
    block, by the mean of its two channels, so its own image is left intact. */
inline DelayResult combinedResult (const MicResults& res, int r, bool stereoPair)
{
    auto v = res.vs[r];
    const auto& b = res.vsRight[r];
    if (stereoPair && v.valid && b.valid)
    {
        v.delaySamples = 0.5 * (v.delaySamples + b.delaySamples);
        v.confidence = juce::jmin (v.confidence, b.confidence);
    }
    return v;
}

/** What is actually applied to the audio, for display. */
struct MicApplied
{
    bool isReference = false;
    double delayLms = 0.0, delayRms = 0.0;
    float gainL = 1.0f, gainR = 1.0f;
    int polarity = 1;
    int phaseTaps = 0;
    juce::String note;
};

struct MicInfo
{
    int index = -1;
    MicState state;
    MicResults results;
    MicApplied applied;
    double sampleRate = 0.0;
    int blockSize = 0, numIn = 0, numOut = 0;
    bool hasPlayhead = false, playing = false;

    bool isStereoPair() const noexcept { return numIn == 2 && ! state.monoSource && ! isMain (state.role); }
};

/** Session-wide settings shared by every instance. */
struct Scene
{
    ArraySettings arrays[kMaxGroups];
    juce::String groupNames[kMaxGroups];
    int numGroups = 1;
    double tempC = 20.0;
    int panLaw = 0; // 0: -3 dB, 1: -4.5 dB, 2: -6 dB
    juce::int64 stamp = 0;
};

/** Process-wide meeting point for every plugin instance.

    Pyramix has no side-chain, so instances cannot exchange audio through the
    mixer. Instead each instance writes its input into a ring buffer addressed
    by the host timeline position. When the user asks for an analysis, a
    background thread compares every microphone against the main microphones
    of its group; the result is kept until it is reset. */
class Hub : private juce::Thread
{
public:
    static constexpr int maxSlots = 128;

    /** Read by the audio thread of each instance. */
    struct Live
    {
        std::atomic<int> delayL { 0 }, delayR { 0 };       // samples
        std::atomic<float> gainL { 1.0f }, gainR { 1.0f }; // polarity included
        std::atomic<bool> phaseOn { false }, fromMono { false };
    };

    using FirSink = std::function<void (const std::vector<float>&)>;

    Hub();
    ~Hub() override;

    int  registerInstance();
    void unregisterInstance (int slot);
    void setFirSink (int slot, FirSink);

    // Not real-time: call from prepareToPlay.
    void prepare (int slot, double sampleRate, int blockSize, int numIn, int numOut);

    // Real-time safe: call from processBlock.
    void push (int slot, const float* ch0, const float* ch1, int numSamples,
               bool hasPlayhead, bool playing, juce::int64 timelinePos) noexcept;
    const Live& live (int slot) const noexcept        { return slots[(size_t) slot].live; }
    bool bypassed() const noexcept                    { return bypassAll.load(); }

    // Editing (message thread).
    MicState getMic (int slot) const;
    void setMic (int slot, const MicState&);
    void setHostName (int slot, const juce::String&);
    Scene getScene() const;
    void setScene (const Scene&);
    void setBypassed (bool b)                         { bypassAll = b; ++changes; }
    juce::Array<MicInfo> snapshot() const;
    int changeCount() const noexcept                  { return changes.load(); }

    // Analysis.
    void startAnalysis();
    void stopAnalysis();
    void resetAnalysis();
    bool isAnalysing() const noexcept                 { return analysing.load(); }
    double analysedSeconds() const noexcept           { return analysedTime.load(); }
    void placeFromAnalysis (int slot);

    // Persistence.
    juce::ValueTree saveInstance (int slot) const;
    void restoreInstance (int slot, const juce::ValueTree&);

private:
    struct Slot
    {
        std::atomic<bool> active { false };
        std::atomic<bool> playing { false }, hasPlayhead { false };
        std::atomic<juce::int64> writeEnd { 0 }, runStart { 0 };
        std::atomic<double> sampleRate { 0.0 };
        std::atomic<int> blockSize { 0 }, numIn { 0 }, numOut { 0 };
        juce::int64 expected = -1; // audio thread only
        Live live;

        juce::CriticalSection ringLock; // guards ring reallocation
        juce::HeapBlock<float> ring[2];
        int ringSize = 0;

        // guarded by Hub::stateLock
        MicState state;
        MicResults results;
        MicApplied applied;
        FirSink sink;
        int firLength = 0, firVersion = 0, firBuilt = -1;

        // analysis thread only
        DelayEstimator estimator[3], estimatorRight[3];
        int estimatorRef[3] { -1, -1, -1 };
        void resetEstimators (int frameSize)
        {
            for (int r = 0; r < 3; ++r) { estimator[r].reset (frameSize); estimatorRight[r].reset (frameSize); estimatorRef[r] = -1; }
        }
    };

    struct RefSet { int slot[3] { -1, -1, -1 }; int channel[3] { 0, 0, 0 }; };

    void run() override;
    void analyse();
    bool readFrame (Slot&, int channel, juce::int64 start, int length, float* dest);
    RefSet refsForGroup (int group, int* arrayGroup = nullptr) const; // stateLock held
    void recompute (int slot);                                        // stateLock held
    void recomputeAll();                                              // stateLock held
    void place (int slot);                                            // stateLock held

    std::array<Slot, maxSlots> slots;
    juce::CriticalSection stateLock, analysisLock, registerLock;
    Scene scene;
    std::atomic<bool> analysing { false }, resetRequested { false }, bypassAll { false };
    std::atomic<double> analysedTime { 0.0 };
    std::atomic<int> changes { 0 };

    // analysis thread state
    int frameSize = 0;
    juce::int64 framePos = -1;
    std::unique_ptr<Fft> fft;
    std::unique_ptr<FrameSpectrum> tgtSpectrum;
    struct RefSpectrum { int key = -1; bool ok = false; std::unique_ptr<FrameSpectrum> spectrum; };
    std::vector<RefSpectrum> refSpectra;
    std::vector<float> frame;
    std::vector<std::complex<float>> scratch;
};
}
