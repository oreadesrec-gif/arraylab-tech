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

    // Alignment.
    AlignTo alignTo = AlignTo::nearest;
    bool delayOn = true, polarityOn = true, phaseOn = false;
    double offsetMs = 0.0;              // added after alignment (lets the main pair lead)
    double phaseAmount = 1.0;           // 0..1: how much of the measured phase is taken out
    double phaseLowHz = 20.0, phaseHighHz = 2000.0; // higher up, phase between distant mics is rarely stable
    double phaseRotationDeg = 0.0;      // turns the whole microphone's phase by hand, to the degree
    bool peakLocked[kNumRefs] {};       // the user picked a delay by hand for this reference
    double peakLag[kNumRefs] {};

    // Pan: an ordinary panner for a mono source on a stereo output.
    bool panOn = true;
    bool panFollowsPlan = true;         // the pan is where the main pair hears this spot on the plan
    double pan = 0.0;                   // -1 left .. +1 right, used when not following the plan
    bool timeDifference = false;        // left and right each lined up on their own main microphone
    bool monoSource = false;            // stereo track carrying a mono mic on its left channel

    // A stereo pair is left untouched unless asked: then `pan` moves it and the width narrows it.
    bool pairPanOn = false;
    double pairWidth = 1.0;             // 1 = left capsule fully left, right capsule fully right

    bool measureLocked = false;         // Analyze and Reset analysis leave this microphone's results alone

    // A spot on a stereo track is a pair: its two capsules sit either side of x/y, aimed around angleDeg.
    double pairSpacingCm = 17.0, pairAngleDeg = 110.0;

    int colour = -1;                    // index in the palette, or -1 to follow the group's colour

    // Two mono tracks that are the two microphones of one pair are linked: they then share
    // one delay, one polarity and one phase correction, so the pair's own image is untouched.
    juce::int64 uid = 0;                // identifies this microphone across sessions
    juce::int64 partner = 0;            // uid of the other microphone of the pair, or 0
};

/** What the analysis measured for one microphone, against main L, C, R and their sum. */
struct MicResults
{
    DelayResult vs[kNumRefs];
    DelayResult vsRight[kNumRefs];      // second channel of a stereo spot
    PhaseCurve curve[kNumRefs];
    ZoneDelays zones[kNumRefs];
    float phaseTrim[kFineBands] {};     // extra phase found by refining against the whole sum

    bool any() const noexcept { for (auto& v : vs) if (v.valid) return true; return false; }
};

/** A measurement as it will be used: the user's own peak if they picked one,
    and a stereo spot moved as one block (mean of its two capsules). */
struct Effective
{
    bool valid = false, reliable = false, locked = false;
    double delaySamples = 0.0;
    int polarity = 1;
    double confidence = 0.0;
    double shift = 0.0;                 // how far a picked peak sits from the automatic one
};

Effective effectiveResult (const MicResults&, const MicState&, int ref, bool stereoPair);

/** The reference a microphone is lined up with (index into L, C, R, sum), or -1. */
int alignmentRef (const Effective* e, AlignTo);

/** What is actually applied to the audio, for display. */
struct MicApplied
{
    bool isReference = false;
    int ref = -1;                       // reference in use
    double delayLms = 0.0, delayRms = 0.0;
    float gainL = 1.0f, gainR = 1.0f;
    float crossLR = 0.0f, crossRL = 0.0f; // stereo pair: left capsule into the right bus, and the reverse
    bool panActive = false;
    int polarity = 1;
    int phaseTaps = 0;
    double pan = 0.0, suggestedPan = 0.0;
    bool hasSuggestedRotation = false;  // the single rotation that would best line this microphone up
    double suggestedRotationDeg = 0.0;
    float correction[kFineBands] {};    // phase taken out per band
    juce::String note;
};

struct MicInfo
{
    int index = -1;
    int partnerIndex = -1;              // slot of the linked microphone, if any
    MicState state;
    MicResults results;
    MicApplied applied;
    double sampleRate = 0.0;
    int blockSize = 0, numIn = 0, numOut = 0;
    bool hasPlayhead = false, playing = false;

    bool isStereoPair() const noexcept { return numIn == 2 && ! state.monoSource && ! isMain (state.role); }
    /** Anything that leaves the plugin in stereo can be panned by it. */
    bool canPan() const noexcept       { return numOut == 2 && ! isMain (state.role); }
    bool panIsOn() const noexcept      { return canPan() && (isStereoPair() ? state.pairPanOn : state.panOn); }
};

/** How the microphones add up, with and without the corrections. Levels are those
    reaching the plugins (before the mixer's faders). */
struct CheckData
{
    static constexpr int numBands = 64;
    static constexpr double low = 30.0, high = 18000.0;
    bool valid = false;
    float before[numBands] {}, after[numBands] {};   // dB, power of the left and right buses together
    double gainAll = 0.0, gainLow = 0.0, gainMid = 0.0, gainHigh = 0.0; // after minus before, dB
    bool hasCorrelation = false;                     // only when one microphone is in focus
    double correlationBefore = 0.0, correlationAfter = 0.0;
    int refinePass = 0, refinePasses = 0;            // progress of "refine on the sum"
    bool refined = false;                            // some microphone carries a refinement
};

/** The correlation behind one measurement, for the Detail view. */
struct MicDetail
{
    bool available = false;
    int slot = -1, ref = 0;
    CorrelationCurve curve;
};

/** Session-wide settings shared by every instance. */
struct Scene
{
    ArraySettings arrays[kMaxGroups];
    juce::String groupNames[kMaxGroups];
    int numGroups = 1;
    double tempC = 20.0;
    int panLaw = 0;     // 0: -3 dB, 1: -4.5 dB, 2: -6 dB
    int precision = 0;  // 0: live, no latency. 1: full-resolution phase, latency reported to the host
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
        std::atomic<float> delayL { 0.0f }, delayR { 0.0f }; // samples, fractional, session latency included
        std::atomic<float> gainL { 1.0f }, gainR { 1.0f };   // polarity included
        std::atomic<float> bypassGainL { 1.0f }, bypassGainR { 1.0f };
        std::atomic<float> crossLR { 0.0f }, crossRL { 0.0f }; // stereo pair being panned: left into right, right into left
        std::atomic<int> bypassDelay { 0 };                  // the session latency alone
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
    float outputTrim() const noexcept                 { return matchOn.load() && ! bypassAll.load() ? matchGain.load() : 1.0f; }

    /** Latency every instance reports to the host in a given precision mode. */
    static int latencyFor (int precision, double sampleRate);
    int precision() const noexcept                    { return precisionMode.load(); }

    // Editing (message thread).
    MicState getMic (int slot) const;
    void setMic (int slot, const MicState&);
    void setHostName (int slot, const juce::String&);
    Scene getScene() const;
    void setScene (const Scene&);
    void setBypassed (bool b)                         { bypassAll = b; ++changes; }
    void setMatchLevel (bool b)                       { matchOn = b; ++changes; }
    void setAllLocked (bool locked);
    /** Links two microphones as the two halves of a pair; other < 0 unlinks. */
    void linkMics (int slot, int other);
    bool matchLevel() const noexcept                  { return matchOn.load(); }
    juce::Array<MicInfo> snapshot() const;
    int changeCount() const noexcept                  { return changes.load(); }

    // Analysis.
    void startAnalysis();
    void stopAnalysis();
    void resetAnalysis();
    bool isAnalysing() const noexcept                 { return analysing.load(); }
    double analysedSeconds() const noexcept           { return analysedTime.load(); }
    void placeFromAnalysis (int slot);

    // Detail: the correlation of one microphone against one reference.
    void setDetailFocus (int slot, int ref)           { detailSlot = slot; detailRef = ref; }
    MicDetail getDetail() const;

    // Check: compare the sum of the microphones with and without corrections.
    // focusSlot < 0 sums everything; otherwise the main array plus that one microphone.
    void setCheck (bool active, int focusSlot)        { checkActive = active; checkFocus = focusSlot; }
    CheckData getCheck() const;
    /** Nudges the phase of every phase-corrected microphone towards the whole sum, over a few passes of music. */
    void startRefine (int passes = 3);
    void clearRefine();
    bool isRefining() const noexcept                  { return refinePassesLeft.load() > 0; }

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
        int firLength = 0;
        juce::int64 firSignature = 0;
        double alignL = 0.0, alignR = 0.0; // alignment delays alone, in samples, for the sum check

        // analysis thread only
        DelayEstimator estimator[kNumRefs], estimatorRight[kNumRefs];
        int estimatorRef[kNumRefs] { -1, -1, -1, -1 };
        int readFrames = -1;
        void resetEstimators (int frameSize)
        {
            for (int r = 0; r < kNumRefs; ++r) { estimator[r].reset (frameSize); estimatorRight[r].reset (frameSize); estimatorRef[r] = -1; }
            readFrames = -1;
        }
    };

    /** Where each reference comes from. The sum has no channel of its own: it is built from the others. */
    struct RefSet
    {
        int slot[kNumRefs] { -1, -1, -1, -1 };
        int channel[kNumRefs] { 0, 0, 0, 0 };
        int group = 0;
        int key (int r) const { return r == 3 ? 2 * maxSlots + group : slot[r] * 2 + channel[r]; }
    };
    struct Target { int slot; RefSet refs; bool pair; Role role; };

    void run() override;
    void analyse();
    void readout (const Target&, double sampleRate);
    void detail();
    void check();
    bool prepareFrame (double sampleRate);
    bool readFrame (Slot&, int channel, juce::int64 start, int length, float* dest);
    SearchRange rangeFor (Role, double sampleRate) const;
    RefSet refsForGroup (int group, int* arrayGroup = nullptr) const; // stateLock held
    int partnerOf (int slot) const;                                   // stateLock held
    void shareWithPartner (int slot);                                 // stateLock held
    void recompute (int slot);                                        // stateLock held
    void recomputeAll();                                              // stateLock held
    void place (int slot);                                            // stateLock held

    std::array<Slot, maxSlots> slots;
    juce::CriticalSection stateLock, analysisLock, registerLock;
    Scene scene;
    std::atomic<bool> analysing { false }, resetRequested { false }, bypassAll { false }, matchOn { false };
    std::atomic<float> matchGain { 1.0f };
    std::atomic<int> precisionMode { 0 };
    std::atomic<double> analysedTime { 0.0 };
    std::atomic<bool> checkActive { false };
    std::atomic<int> checkFocus { -1 }, detailSlot { -1 }, detailRef { 0 }, refinePassesLeft { 0 }, refinePassesTotal { 0 };
    std::atomic<int> changes { 0 };

    // analysis thread state
    int frameSize = 0;
    juce::int64 framePos = -1;
    double lastRate = 0.0;
    std::vector<Target> targets;
    int readCursor = 0;
    std::unique_ptr<Fft> fft;
    std::unique_ptr<FrameSpectrum> tgtSpectrum;
    struct RefSpectrum { int key = -1; bool ok = false; std::unique_ptr<FrameSpectrum> spectrum; };
    std::vector<RefSpectrum> refSpectra;
    std::vector<float> frame;
    std::vector<std::complex<float>> scratch;

    // detail state (results under stateLock)
    MicDetail detailData;
    int detailDone[3] { -1, -1, -1 };

    // check state (analysis thread, results under stateLock)
    CheckData checkData;
    // Everything is added up on a stereo bus, index 0 = left, 1 = right.
    std::vector<std::complex<float>> busBefore[2], busAfter[2], busMain[2], busFocusBefore[2], busFocusAfter[2];
    std::vector<std::vector<std::complex<float>>> micBus[2];
    double bandBefore[CheckData::numBands] {}, bandAfter[CheckData::numBands] {};
    double corrState[5] {};
    juce::int64 checkPos = -1;
    int checkedFocus = -2;
    bool checkPrimed = false;

    struct Refine { std::complex<double> cross[kFineBands]; double own[kFineBands], others[kFineBands]; };
    std::vector<Refine> refine; // one per slot, while refining
    int refineFrames = 0;
};
}
