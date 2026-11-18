#pragma once
#include "PluginProcessor.h"

class ArrayLabEditor : public juce::AudioProcessorEditor,
                       private juce::TableListBoxModel,
                       private juce::Timer
{
public:
    explicit ArrayLabEditor (ArrayLabProcessor&);
    ~ArrayLabEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override { return rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void timerCallback() override;

    juce::String cellText (const arraylab::Hub::SlotInfo&, int column) const;
    juce::String buildReport() const;

    ArrayLabProcessor& proc;
    juce::Array<arraylab::Hub::SlotInfo> rows;
    int reference = -1;

    juce::TableListBox table { "mics", this };
    juce::Label header, hint, nameLabel { {}, "This mic:" }, delayLabel { {}, "Test delay:" };
    juce::TextEditor nameEditor;
    juce::Slider delaySlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::ToggleButton invertButton { "Invert" };
    juce::TextButton refButton { "Set selected as reference" }, resetButton { "Reset analysis" },
                     copyButton { "Copy report" };
    juce::AudioProcessorValueTreeState::SliderAttachment delayAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment invertAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrayLabEditor)
};
