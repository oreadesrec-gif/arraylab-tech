#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "Hub.h"
#include "Theme.h"

/** Settings of the selected microphone, its group's main array, and the session. */
class Inspector : public juce::Component
{
public:
    explicit Inspector (arraylab::Hub&);
    void update (const juce::Array<arraylab::MicInfo>&, const arraylab::Scene&, int selectedSlot);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Row { juce::Label* label; juce::Component* control; int height; };

    juce::Label& header (const juce::String&);
    juce::Slider& slider (const juce::String& name, double lo, double hi, double step, const juce::String& suffix, std::function<void()> onChange);
    juce::ComboBox& combo (const juce::String& name, const juce::StringArray& items, std::function<void()> onChange);
    juce::ToggleButton& toggle (const juce::String& name, std::function<void()> onChange);
    juce::Label& readout (const juce::String& name, int height = 22);
    void add (const juce::String& name, juce::Component&, int height);

    void commitMic();
    void commitScene (bool presetChosen);
    static bool busy (juce::Component&);

    arraylab::Hub& hub;
    int slot = -1, group = 0;
    bool refreshing = false;
    arraylab::Role lastRole = arraylab::Role::spot;

    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<juce::Component> owned;
    std::vector<Row> rows;

    juce::TextEditor name, groupName;
    juce::TextButton placeButton { "Place from analysis" };
    juce::ComboBox *role, *groupBox, *pattern, *arrayType, *arrayPattern, *panLaw;
    juce::Slider *x, *y, *z, *angle, *sourceDistance, *pairSpacing, *pairAngle, *offset, *panAmount, *timePan,
                 *spacing, *arrayAngle, *forward, *arrayHeight, *rotation, *temperature;
    juce::ToggleButton *delayOn, *polarityOn, *phaseOn, *panOn, *monoSource;
    juce::Label *measured, *applied, *note, *arrayHeader;
};
