#include "PluginEditor.h"

using namespace arraylab;

namespace
{
enum Column { colName = 1, colGroup, colRole, colL, colC, colR, colConf, colApplied, colDelay, colPolarity, colPhase, colPan, colIO };

juce::String timeText (double seconds)
{
    const int s = (int) seconds;
    return juce::String (s / 60).paddedLeft ('0', 2) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}
}

ArrayLabEditor::ArrayLabEditor (ArrayLabProcessor& p)
    : AudioProcessorEditor (p), proc (p), hub (p.hub()), stage (p.hub()), inspector (p.hub())
{
    setLookAndFeel (&look);
    selected = proc.slot();

    title.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, ui::text);
    status.setColour (juce::Label::textColourId, ui::dim);
    status.setFont (juce::FontOptions (13.0f));
    hint.setColour (juce::Label::textColourId, ui::warn);
    hint.setFont (juce::FontOptions (13.5f));
    for (auto* c : { (juce::Component*) &title, (juce::Component*) &status, (juce::Component*) &hint, (juce::Component*) &analyseButton,
                     (juce::Component*) &resetButton, (juce::Component*) &stageTab, (juce::Component*) &alignTab, (juce::Component*) &checkTab, (juce::Component*) &checkView, (juce::Component*) &addGroupButton,
                     (juce::Component*) &copyButton, (juce::Component*) &bypassButton, (juce::Component*) &groupBox,
                     (juce::Component*) &stage, (juce::Component*) &inspector, (juce::Component*) &table })
        addAndMakeVisible (c);

    analyseButton.onClick = [this] { if (hub.isAnalysing()) hub.stopAnalysis(); else hub.startAnalysis(); timerCallback(); };
    resetButton.onClick = [this] { hub.resetAnalysis(); timerCallback(); };
    bypassButton.onClick = [this] { hub.setBypassed (bypassButton.getToggleState()); };
    copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (buildReport()); };
    addGroupButton.onClick = [this]
    {
        auto sc = hub.getScene();
        if (sc.numGroups < kMaxGroups) { ++sc.numGroups; hub.setScene (sc); }
    };
    groupBox.onChange = [this] { groupFilter = groupBox.getSelectedId() - 2; timerCallback(); };

    for (auto* t : { &stageTab, &alignTab, &checkTab })
    {
        t->setClickingTogglesState (true);
        t->setRadioGroupId (1);
        t->setColour (juce::TextButton::buttonOnColourId, ui::accent.withAlpha (0.55f));
    }
    stageTab.setConnectedEdges (juce::Button::ConnectedOnRight);
    alignTab.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    checkTab.setConnectedEdges (juce::Button::ConnectedOnLeft);
    stageTab.onClick = [this] { showView (0); };
    alignTab.onClick = [this] { showView (1); };
    checkTab.onClick = [this] { showView (2); };
    stageTab.setToggleState (true, juce::dontSendNotification);

    stage.onSelect = [this] (int slot) { select (slot); };

    auto& h = table.getHeader();
    h.addColumn ("Mic", colName, 136);
    h.addColumn ("Group", colGroup, 70);
    h.addColumn ("Role", colRole, 70);
    h.addColumn ("vs L (ms)", colL, 70);
    h.addColumn ("vs C (ms)", colC, 70);
    h.addColumn ("vs R (ms)", colR, 70);
    h.addColumn ("Sure", colConf, 46);
    h.addColumn ("Applied (ms)", colApplied, 132);
    h.addColumn ("Delay", colDelay, 46);
    h.addColumn ("Polarity", colPolarity, 58);
    h.addColumn ("Phase", colPhase, 48);
    h.addColumn ("Pan", colPan, 44);
    table.setColour (juce::ListBox::backgroundColourId, ui::panel);
    table.setRowHeight (24);
    table.setVisible (false);
    checkView.setVisible (false);

    setResizable (true, true);
    setResizeLimits (980, 560, 2400, 1600);
    setSize (1280, 800);
    startTimerHz (10);
    timerCallback();
}

ArrayLabEditor::~ArrayLabEditor()
{
    stopTimer();
    hub.setCheck (false, -1);
    setLookAndFeel (nullptr);
}

void ArrayLabEditor::select (int slot)
{
    selected = slot;
    timerCallback();
}

void ArrayLabEditor::showView (int which)
{
    view = which;
    stage.setVisible (which == 0);
    table.setVisible (which == 1);
    checkView.setVisible (which == 2);
    timerCallback();
}

void ArrayLabEditor::timerCallback()
{
    mics = hub.snapshot();
    scene = hub.getScene();

    bool found = false, anyMain = false, anyPlaying = false, anyResult = false, anyTimeline = false;
    for (auto& m : mics)
    {
        found |= m.index == selected;
        anyMain |= isMain (m.state.role);
        anyPlaying |= m.playing;
        anyTimeline |= m.hasPlayhead;
        anyResult |= m.results.primary >= 0;
    }
    if (! found) selected = proc.slot();

    rows.clearQuick();
    for (auto& m : mics)
        if (groupFilter < 0 || m.state.group == groupFilter)
            rows.add (m);

    if (groupBox.getNumItems() != scene.numGroups + 1 || ! groupBox.isPopupActive())
    {
        groupBox.clear (juce::dontSendNotification);
        groupBox.addItem ("All groups", 1);
        for (int g = 0; g < scene.numGroups; ++g) groupBox.addItem (scene.groupNames[g], g + 2);
        groupBox.setSelectedId (groupFilter + 2, juce::dontSendNotification);
    }
    addGroupButton.setEnabled (scene.numGroups < kMaxGroups);

    const bool analysing = hub.isAnalysing();
    analyseButton.setButtonText (analysing ? "Stop   " + timeText (hub.analysedSeconds()) : "Analyze");
    analyseButton.setColour (juce::TextButton::buttonColourId, analysing ? ui::bad.darker (0.3f) : ui::accent.darker (0.5f));
    bypassButton.setToggleState (hub.bypassed(), juce::dontSendNotification);

    status.setText (juce::String (mics.size()) + (mics.size() == 1 ? " mic linked" : " mics linked")
                        + "   |   " + (anyPlaying ? "playing" : "stopped")
                        + (anyResult && ! analysing ? "   |   analysis kept" : ""),
                    juce::dontSendNotification);

    juce::String h;
    if (! anyMain)                          h = "Start here: select the track of your main pair and set its Role to Main pair (stereo track), or Main L / Main R / Main C on mono tracks.";
    else if (analysing && ! anyPlaying)     h = anyTimeline ? "Waiting for playback: press play on a passage where everyone is heard." : "Waiting for playback (the host has not sent a timeline position yet).";
    else if (analysing)                     h = "Analysing the music... let it run 20 seconds or more, then press Stop. Nothing is applied until you stop.";
    else if (! anyResult)                   h = "Press Analyze, then play the music. The result stays until you press Reset analysis.";
    hint.setText (h, juce::dontSendNotification);

    juce::String focusName = "the selected microphone";
    bool focusIsMain = false;
    for (auto& m : mics) if (m.index == selected) { focusName = m.state.name; focusIsMain = isMain (m.state.role); }
    hub.setCheck (view == 2, checkView.focusOnSelected() && ! focusIsMain ? selected : -1);
    if (view == 2)
        checkView.update (hub.getCheck(), focusIsMain ? "(select a spot on the Stage or Align view)" : focusName, anyPlaying, hub.bypassed());

    stage.update (mics, scene, selected, groupFilter);
    inspector.update (mics, scene, selected);
    table.updateContent();
    table.repaint();
}

juce::String ArrayLabEditor::cellText (const MicInfo& m, int column) const
{
    const bool main = isMain (m.state.role);
    const auto delay = [&] (int r) -> juce::String
    {
        const auto v = combinedResult (m.results, r, m.isStereoPair());
        return v.valid ? juce::String (1000.0 * v.delaySamples / m.sampleRate, 3) + (v.polarity < 0 ? juce::String::fromUTF8 (" \xc3\x98") : juce::String()) : "-";
    };
    switch (column)
    {
        case colName:     return m.state.name + (m.index == proc.slot() ? "  (this track)" : "");
        case colGroup:    return scene.groupNames[m.state.group];
        case colRole:     return ui::roleNames()[(int) m.state.role].upToFirstOccurrenceOf (" (", false, false) + (m.isStereoPair() ? " pair" : "");
        case colL:        return main ? "ref" : delay (0);
        case colC:        return main ? "ref" : delay (1);
        case colR:        return main ? "ref" : delay (2);
        case colConf:     return m.results.primary >= 0 ? juce::String (juce::roundToInt (m.results.vs[m.results.primary].confidence * 100.0)) + " %" : "-";
        case colApplied:  return main ? "-" : std::abs (m.applied.delayLms - m.applied.delayRms) > 0.005
                                                 ? "L " + juce::String (m.applied.delayLms, 2) + " / R " + juce::String (m.applied.delayRms, 2)
                                                 : juce::String (m.applied.delayLms, 3);
        case colDelay:    return main ? "" : m.state.delayOn ? "ON" : "off";
        case colPolarity: return main ? "" : m.state.polarityOn ? "ON" : "off";
        case colPhase:    return main ? "" : m.state.phaseOn ? "ON" : "off";
        case colPan:      return main ? "" : ! m.canPan() ? "n/a" : m.state.panOn ? "ON" : "off";
        case colIO:       return juce::String (m.numIn) + " > " + juce::String (m.numOut);
        default:          return {};
    }
}

juce::String ArrayLabEditor::buildReport() const
{
    juce::String s;
    s << "ArrayLab 0.3 | host: " << juce::PluginHostType().getHostDescription() << " | " << status.getText() << "\n";
    s << "Mic\tGroup\tRole\tvs L\tvs C\tvs R\tConfidence\tApplied\tDelay\tPolarity\tPhase\tPan\tIn>Out\tRate\tTimeline\tNote\n";
    for (auto& m : mics)
    {
        for (int c = colName; c <= colIO; ++c)
            s << cellText (m, c) << "\t";
        s << juce::String (m.sampleRate / 1000.0, 1) << "k\t" << (m.hasPlayhead ? (m.playing ? "playing" : "stopped") : "no timeline") << "\t" << m.applied.note << "\n";
    }
    return s;
}

void ArrayLabEditor::paintRowBackground (juce::Graphics& g, int row, int, int, bool)
{
    if (juce::isPositiveAndBelow (row, rows.size()) && rows.getReference (row).index == selected)
        g.fillAll (ui::accent.withAlpha (0.22f));
}

void ArrayLabEditor::paintCell (juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (! juce::isPositiveAndBelow (row, rows.size())) return;
    const auto& m = rows.getReference (row);
    const auto text = cellText (m, column);
    auto colour = ui::text;
    if (column == colName)  colour = ui::micColour (m.state.colour, m.state.group);
    if (column == colConf && m.results.primary >= 0)
    {
        const double c = m.results.vs[m.results.primary].confidence;
        colour = c > 0.5 ? ui::good : c > 0.2 ? ui::warn : ui::bad;
    }
    if (column >= colDelay && column <= colPan) colour = text == "ON" ? ui::good : ui::dim;
    g.setColour (colour);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (text, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
}

void ArrayLabEditor::cellClicked (int row, int column, const juce::MouseEvent&)
{
    if (! juce::isPositiveAndBelow (row, rows.size())) return;
    const int slot = rows.getReference (row).index;
    if (column >= colDelay && column <= colPan && ! isMain (rows.getReference (row).state.role))
    {
        auto m = hub.getMic (slot);
        if (column == colDelay)    m.delayOn = ! m.delayOn;
        if (column == colPolarity) m.polarityOn = ! m.polarityOn;
        if (column == colPhase)    m.phaseOn = ! m.phaseOn;
        if (column == colPan)      m.panOn = ! m.panOn;
        hub.setMic (slot, m);
    }
    select (slot);
}

void ArrayLabEditor::paint (juce::Graphics& g) { g.fillAll (ui::bg); }

void ArrayLabEditor::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (32);
    title.setBounds (top.removeFromLeft (104));
    analyseButton.setBounds (top.removeFromLeft (150));
    top.removeFromLeft (6);
    resetButton.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (12);
    bypassButton.setBounds (top.removeFromLeft (100));
    copyButton.setBounds (top.removeFromRight (100));
    top.removeFromRight (10);
    addGroupButton.setBounds (top.removeFromRight (76));
    top.removeFromRight (6);
    groupBox.setBounds (top.removeFromRight (140));
    top.removeFromRight (12);
    checkTab.setBounds (top.removeFromRight (64));
    alignTab.setBounds (top.removeFromRight (64));
    stageTab.setBounds (top.removeFromRight (64));
    top.removeFromRight (10);
    status.setBounds (top);

    hint.setBounds (area.removeFromTop (26));
    area.removeFromTop (2);
    inspector.setBounds (area.removeFromRight (350));
    area.removeFromRight (8);
    stage.setBounds (area);
    table.setBounds (area);
    checkView.setBounds (area);
}
