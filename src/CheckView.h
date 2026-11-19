#pragma once
#include "Hub.h"
#include "Theme.h"

/** How the microphones add up, before and after the corrections, while the music plays. */
class CheckView : public juce::Component
{
public:
    CheckView();
    void update (const arraylab::CheckData&, const juce::String& focusName, bool playing, bool bypassed);
    bool focusOnSelected() const { return focusButton.getToggleState(); }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    arraylab::CheckData data;
    juce::String focusName;
    bool playing = false, bypassed = false;
    juce::TextButton allButton { "All microphones" }, focusButton { "Main array + selected microphone" };
};
