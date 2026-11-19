#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "Hub.h"
#include "Theme.h"

/** A row of colour chips. The first one means "use the group's colour". */
class Swatches : public juce::Component
{
public:
    std::function<void (int)> onPick;
    void set (int index, juce::Colour groupColour) { current = index; automatic = groupColour; repaint(); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    int current = -1;
    juce::Colour automatic;
};

/** Settings of the selected microphone. Only what applies to that microphone is shown. */
class Inspector : public juce::Component
{
public:
    explicit Inspector (arraylab::Hub&);
    void update (const juce::Array<arraylab::MicInfo>&, const arraylab::Scene&, int selectedSlot);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    // A row is shown only when every condition it needs holds for the selected microphone.
    enum Need { always = 0, notMain = 1, pairOnly = 2, mainOnly = 4, canPan = 8, stereoTrack = 16, cannotPan = 32 };
    struct Row { juce::Label* label; juce::Component* control; int height; int need; };

    juce::Label& header (const juce::String&, int need = always);
    juce::Label& help (const juce::String&, int need = always, int height = 30);
    juce::Slider& slider (const juce::String& name, double lo, double hi, double step, const juce::String& suffix, std::function<void()> onChange, int need = always);
    juce::ComboBox& combo (const juce::String& name, const juce::StringArray& items, std::function<void()> onChange, int need = always);
    juce::ToggleButton& toggle (std::function<void()> onChange, int need = always);
    void add (const juce::String& name, juce::Component&, int height, int need);

    void layout();
    void commitMic();
    void commitScene (bool presetChosen);
    static bool busy (juce::Component&);

    arraylab::Hub& hub;
    int slot = -1, group = 0, context = -1;
    bool refreshing = false;
    arraylab::Role lastRole = arraylab::Role::spot;

    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<juce::Component> owned;
    std::vector<Row> rows;

    juce::TextEditor name, groupName;
    Swatches swatches;
    juce::TextButton placeButton { "Put it where the analysis heard it" };
    juce::ComboBox *role, *groupBox, *pattern, *arrayType, *arrayPattern, *panLaw;
    juce::Slider *x, *y, *z, *angle, *sourceDistance, *pairSpacing, *pairAngle, *offset, *panAmount, *timePan,
                 *spacing, *arrayAngle, *forward, *arrayHeight, *rotation, *temperature;
    juce::ToggleButton *delayOn, *polarityOn, *phaseOn, *panOn, *monoSource;
    juce::Label *roleHelp, *measured, *note, *arrayHeader, *panBlocked;
};
