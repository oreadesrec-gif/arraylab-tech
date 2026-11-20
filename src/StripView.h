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
        static constexpr int name = 8, on = 172, gain = 216, delay = 290, minus = 400, plus = 424, flip = 454,
                             rotation = 492, level = 590, steady = 688, solo = 772, lock = 824, end = 876;
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
