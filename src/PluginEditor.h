#pragma once
#include "PluginProcessor.h"
#include "StageView.h"
#include "Inspector.h"
#include "CheckView.h"
#include "DetailView.h"
#include "StripView.h"

class ArrayLabEditor : public juce::AudioProcessorEditor,
                       private juce::TableListBoxModel,
                       private juce::Timer
{
public:
    explicit ArrayLabEditor (ArrayLabProcessor&);
    ~ArrayLabEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    int getNumRows() override { return rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void timerCallback() override;

    void select (int slot);
    void showView (int);
    juce::String cellText (const arraylab::MicInfo&, int column) const;
    juce::String buildReport() const;

    ArrayLabProcessor& proc;
    arraylab::Hub& hub;
    juce::LookAndFeel_V4 look { juce::LookAndFeel_V4::getMidnightColourScheme() };
    juce::Array<arraylab::MicInfo> mics, rows;
    arraylab::Scene scene;
    int selected = -1, groupFilter = -1;

    juce::Label title { {}, "ArrayLab" }, status, hint;
    juce::TextButton analyseButton { "Analyze" }, resetButton { "Reset analysis" }, stageTab { "Stage" }, alignTab { "Mix" }, detailTab { "Detail" }, checkTab { "Check" },
                     addGroupButton { "+ Group" }, copyButton { "Copy report" }, lockButton { "Lock all" }, exportButton { "Export" },
                     shotButtons[3] { juce::TextButton ("A"), juce::TextButton ("B"), juce::TextButton ("C") };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::ToggleButton bypassButton { "Bypass all" };
    juce::ComboBox groupBox;
    StageView stage;
    Inspector inspector;
    CheckView checkView;
    DetailView detailView;
    StripView stripView;
    int view = 0;
    juce::TableListBox table { "mics", this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrayLabEditor)
};
