#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "Hub.h"
#include "Theme.h"

/** The working view: one line per microphone, like the strips of a console. What matters
    for a session is on that line: what there is to gain, the delay, polarity, phase, the
    microphone's level in the mix, and switches to hear the difference. */
class StripView : public juce::Component
{
public:
    explicit StripView (arraylab::Hub&);
    ~StripView() override;
    void update (const juce::Array<arraylab::MicInfo>&, const arraylab::Scene&, int selectedSlot, int groupFilter);
    std::function<void (int slot)> onSelect;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Column positions, shared by the header and every row.
    struct Columns
    {
        static constexpr int name = 8, on = 160, gain = 204, delay = 276, minus = 386, plus = 410, flip = 440,
                             rotation = 478, measured = 566, byFreq = 622, level = 678, steady = 772, solo = 852, lock = 902, end = 952;
    };

private:
    class Row;
    arraylab::Hub& hub;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<Row> rows;
    juce::Slider lead { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
    juce::Label leadLabel { {}, "The main array leads every lined-up mic by" };
    bool refreshing = false;
};
