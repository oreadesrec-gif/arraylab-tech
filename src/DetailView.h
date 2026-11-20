#pragma once
#include "Hub.h"
#include "Theme.h"

/** What the analysis found for one microphone: the candidate delays, with the one
    in use (click another to choose it), and the phase by frequency before and after. */
class DetailView : public juce::Component
{
public:
    explicit DetailView (arraylab::Hub&);
    void update (const arraylab::MicInfo*, const arraylab::Scene&, const arraylab::MicDetail&);
    int shownRef() const noexcept { return ref; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void chooseRef (int);

    arraylab::Hub& hub;
    arraylab::MicInfo mic;
    arraylab::MicDetail detail;
    bool hasMic = false, followApplied = true, wholeRange = false;
    int ref = 0, shownSlot = -1;
    double speed = 343.0;

    juce::TextButton refButtons[arraylab::kNumRefs], autoButton { "Back to automatic" }, rangeButton { "Whole range" };
    juce::Rectangle<float> correlationArea, phaseArea;
    struct Marker { juce::Point<float> at; double lag; };
    std::vector<Marker> markers;
};
