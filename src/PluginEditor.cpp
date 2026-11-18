#include "PluginEditor.h"

namespace
{
enum Column { colIndex = 1, colName, colIO, colRate, colState, colPos, colSamples, colMs, colMetres, colPol, colConf };
constexpr double speedOfSound = 343.0; // m/s at 20 degrees C

const juce::Colour background { 0xff15181d }, panel { 0xff1e232b }, text { 0xffe6e9ee },
                   dim { 0xff8b95a5 }, accent { 0xff4fb3ff }, own { 0xff263445 };
}

ArrayLabEditor::ArrayLabEditor (ArrayLabProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      delayAttachment (p.params, "testDelay", delaySlider),
      invertAttachment (p.params, "testInvert", invertButton)
{
    auto& h = table.getHeader();
    h.addColumn ("#", colIndex, 36);
    h.addColumn ("Mic", colName, 170);
    h.addColumn ("In>Out", colIO, 60);
    h.addColumn ("Rate / block", colRate, 110);
    h.addColumn ("Transport", colState, 90);
    h.addColumn ("Timeline (samples)", colPos, 130);
    h.addColumn ("Delay (samples)", colSamples, 110);
    h.addColumn ("ms", colMs, 70);
    h.addColumn ("metres", colMetres, 70);
    h.addColumn ("Polarity", colPol, 90);
    h.addColumn ("Confidence", colConf, 90);
    table.setColour (juce::ListBox::backgroundColourId, panel);
    table.setRowHeight (24);
    addAndMakeVisible (table);

    for (auto* l : { &header, &hint, &nameLabel, &delayLabel })
    {
        l->setColour (juce::Label::textColourId, l == &hint ? dim : text);
        addAndMakeVisible (*l);
    }
    hint.setText ("Delay is relative to the reference: positive = this mic hears the source later. "
                  "Press play; the estimate sharpens the longer the music runs. Double-click a row to make it the reference.",
                  juce::dontSendNotification);
    hint.setFont (juce::FontOptions (13.0f));

    nameEditor.setText (proc.getUserName(), false);
    nameEditor.setTextToShowWhenEmpty ("name (empty = track name from host)", dim);
    nameEditor.onTextChange = [this] { proc.setUserName (nameEditor.getText()); };
    addAndMakeVisible (nameEditor);
    addAndMakeVisible (delaySlider);
    addAndMakeVisible (invertButton);

    refButton.onClick = [this]
    {
        if (juce::isPositiveAndBelow (table.getSelectedRow(), rows.size()))
            proc.hub().setReference (rows[table.getSelectedRow()].index);
    };
    resetButton.onClick = [this] { proc.hub().resetAnalysis(); };
    copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (buildReport()); };
    for (auto* b : { &refButton, &resetButton, &copyButton })
        addAndMakeVisible (*b);

    setResizable (true, true);
    setResizeLimits (860, 320, 1800, 1400);
    setSize (1040, 460);
    startTimerHz (5);
    timerCallback();
}

ArrayLabEditor::~ArrayLabEditor() { stopTimer(); }

void ArrayLabEditor::timerCallback()
{
    rows = proc.hub().snapshot();
    reference = proc.hub().getReference();

    juce::String refName = "none";
    for (auto& r : rows)
        if (r.index == reference) refName = r.name;

    header.setText ("ArrayLab Probe 0.1   |   host: " + juce::String (juce::PluginHostType().getHostDescription())
                        + "   |   hub " + juce::String::toHexString ((juce::pointer_sized_int) &proc.hub())
                        + "   |   " + juce::String (rows.size()) + " instance(s) linked   |   reference: " + refName,
                    juce::dontSendNotification);
    table.updateContent();
    table.repaint();
}

juce::String ArrayLabEditor::cellText (const arraylab::Hub::SlotInfo& r, int column) const
{
    const bool isRef = r.index == reference;
    const auto& res = r.result;
    const bool has = ! isRef && res.valid;

    switch (column)
    {
        case colIndex:   return juce::String (r.index + 1);
        case colName:    return r.name + (isRef ? "  [REF]" : "") + (r.index == proc.slot() ? "  (this)" : "")
                                + (r.hostNamed ? "  *" : "");
        case colIO:      return juce::String (r.numIn) + " > " + juce::String (r.numOut);
        case colRate:    return juce::String (r.sampleRate / 1000.0, 1) + " k / " + juce::String (r.blockSize);
        case colState:   return ! r.hasPlayhead ? (r.blocks > 0 ? "no timeline" : "idle")
                                                : (r.playing ? "playing" : "stopped");
        case colPos:     return r.hasPlayhead ? juce::String (r.timelinePos) : "-";
        case colSamples: return isRef ? "0" : (has ? juce::String (res.delaySamples, 2) : "-");
        case colMs:      return isRef ? "0" : (has ? juce::String (1000.0 * res.delaySamples / r.sampleRate, 3) : "-");
        case colMetres:  return isRef ? "0" : (has ? juce::String (speedOfSound * res.delaySamples / r.sampleRate, 2) : "-");
        case colPol:     return isRef ? "+" : (has ? (res.polarity < 0 ? "INVERTED" : "+") : "-");
        case colConf:    return isRef ? "" : (res.frames > 0 ? juce::String (juce::roundToInt (res.confidence * 100.0)) + " %  (" + juce::String (res.frames) + ")" : "-");
        default:         return {};
    }
}

juce::String ArrayLabEditor::buildReport() const
{
    juce::String s;
    s << header.getText() << "\n";
    s << "#\tMic\tIn>Out\tRate/block\tTransport\tTimeline\tDelay(smp)\tms\tm\tPolarity\tConfidence\n";
    for (auto& r : rows)
    {
        for (int c = colIndex; c <= colConf; ++c)
            s << cellText (r, c) << (c == colConf ? "\n" : "\t");
    }
    return s;
}

void ArrayLabEditor::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    if (! juce::isPositiveAndBelow (row, rows.size())) return;
    if (selected)                                 g.fillAll (accent.withAlpha (0.25f));
    else if (rows[row].index == proc.slot())      g.fillAll (own);
}

void ArrayLabEditor::paintCell (juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (! juce::isPositiveAndBelow (row, rows.size())) return;
    const auto& r = rows.getReference (row);
    auto colour = text;
    if (column == colPol && r.result.valid && r.result.polarity < 0 && r.index != reference) colour = juce::Colours::orange;
    if (column == colConf && r.result.frames > 0)
        colour = r.result.confidence > 0.5 ? juce::Colour (0xff6fd08c) : r.result.confidence > 0.2 ? juce::Colours::orange : juce::Colour (0xffe36a6a);
    if (r.index == reference && column == colName) colour = accent;
    g.setColour (colour);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (cellText (r, column), 6, 0, w - 8, h, juce::Justification::centredLeft, true);
}

void ArrayLabEditor::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, rows.size()))
        proc.hub().setReference (rows[row].index);
}

void ArrayLabEditor::paint (juce::Graphics& g) { g.fillAll (background); }

void ArrayLabEditor::resized()
{
    auto area = getLocalBounds().reduced (10);
    header.setBounds (area.removeFromTop (24));
    hint.setBounds (area.removeFromTop (36));

    auto bottom = area.removeFromBottom (30);
    area.removeFromBottom (8);
    nameLabel.setBounds (bottom.removeFromLeft (64));
    nameEditor.setBounds (bottom.removeFromLeft (220));
    bottom.removeFromLeft (12);
    delayLabel.setBounds (bottom.removeFromLeft (76));
    delaySlider.setBounds (bottom.removeFromLeft (220));
    invertButton.setBounds (bottom.removeFromLeft (70));
    copyButton.setBounds (bottom.removeFromRight (100));
    bottom.removeFromRight (6);
    resetButton.setBounds (bottom.removeFromRight (110));
    bottom.removeFromRight (6);
    refButton.setBounds (bottom.removeFromRight (180));

    table.setBounds (area);
}
