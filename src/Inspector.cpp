#include "Inspector.h"

using namespace arraylab;

Inspector::Inspector (Hub& h) : hub (h)
{
    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);

    const auto mic = [this] { commitMic(); };
    const auto sc = [this] { commitScene (false); };
    const auto deg = juce::String::fromUTF8 (" \xc2\xb0");

    header ("MICROPHONE");
    name.onTextChange = mic;
    add ("Name", name, 24);
    role = &combo ("Role", ui::roleNames(), mic);
    groupBox = &combo ("Group", {}, mic);
    pattern = &combo ("Pattern", ui::patternNames(), mic);
    x = &slider ("Left / right", -60.0, 60.0, 0.05, " m", mic);
    y = &slider ("Distance", -60.0, 60.0, 0.05, " m", mic);
    z = &slider ("Height", 0.0, 30.0, 0.05, " m", mic);
    angle = &slider ("Aim", -180.0, 180.0, 1.0, deg, mic);
    sourceDistance = &slider ("Mic to source", 0.0, 10.0, 0.05, " m", mic);

    header ("ALIGNMENT");
    delayOn = &toggle ("Delay", mic);
    polarityOn = &toggle ("Polarity", mic);
    phaseOn = &toggle ("Multiband phase", mic);
    offset = &slider ("Offset", -20.0, 60.0, 0.1, " ms", mic);
    measured = &readout ("Measured vs", 50);
    measured->setFont (juce::FontOptions (12.0f));
    applied = &readout ("Applied", 38);
    note = &readout ("", 20);
    note->setColour (juce::Label::textColourId, ui::warn);
    placeButton.onClick = [this] { hub.placeFromAnalysis (slot); };
    add ("", placeButton, 24);

    header ("PAN  (mono source to stereo output)");
    panOn = &toggle ("Pan from the plan", mic);
    panAmount = &slider ("Level difference", 0.0, 200.0, 1.0, " %", mic);
    timePan = &slider ("Time difference", 0.0, 100.0, 1.0, " %", mic);
    monoSource = &toggle ("Stereo track, mono mic on left", mic);

    arrayHeader = &header ("MAIN ARRAY");
    groupName.onTextChange = sc;
    add ("Group name", groupName, 24);
    arrayType = &combo ("Type", ui::arrayNames(), [this] { commitScene (true); });
    arrayPattern = &combo ("Pattern", ui::patternNames(), sc);
    spacing = &slider ("Spacing L-R", 0.0, 1000.0, 1.0, " cm", sc);
    arrayAngle = &slider ("Angle between mics", 0.0, 180.0, 1.0, deg, sc);
    forward = &slider ("Centre mic forward", 0.0, 500.0, 1.0, " cm", sc);
    arrayHeight = &slider ("Height", 0.0, 30.0, 0.05, " m", sc);
    rotation = &slider ("Facing", -180.0, 180.0, 1.0, deg, sc);

    header ("SESSION");
    panLaw = &combo ("Pan law", { "-3 dB", "-4.5 dB", "-6 dB" }, sc);
    temperature = &slider ("Temperature", -10.0, 40.0, 0.5, juce::String::fromUTF8 (" \xc2\xb0""C"), sc);
}

void Inspector::add (const juce::String& labelText, juce::Component& control, int height)
{
    auto* label = new juce::Label ({}, labelText);
    label->setColour (juce::Label::textColourId, ui::dim);
    label->setFont (juce::FontOptions (13.0f));
    owned.add (label);
    content.addAndMakeVisible (label);
    content.addAndMakeVisible (control);
    rows.push_back ({ label, &control, height });
}

juce::Label& Inspector::header (const juce::String& text)
{
    auto* l = new juce::Label ({}, text);
    l->setColour (juce::Label::textColourId, ui::accent);
    l->setFont (juce::FontOptions (12.0f, juce::Font::bold));
    owned.add (l);
    content.addAndMakeVisible (l);
    rows.push_back ({ nullptr, l, 30 });
    return *l;
}

juce::Slider& Inspector::slider (const juce::String& labelText, double lo, double hi, double step, const juce::String& suffix, std::function<void()> onChange)
{
    auto* s = new juce::Slider (juce::Slider::LinearBar, juce::Slider::TextBoxLeft);
    s->setRange (lo, hi, step);
    s->setTextValueSuffix (suffix);
    s->setColour (juce::Slider::trackColourId, ui::accent.withAlpha (0.35f));
    s->setColour (juce::Slider::backgroundColourId, ui::panel2);
    s->onValueChange = std::move (onChange);
    owned.add (s);
    add (labelText, *s, 24);
    return *s;
}

juce::ComboBox& Inspector::combo (const juce::String& labelText, const juce::StringArray& items, std::function<void()> onChange)
{
    auto* c = new juce::ComboBox();
    c->addItemList (items, 1);
    c->onChange = std::move (onChange);
    owned.add (c);
    add (labelText, *c, 24);
    return *c;
}

juce::ToggleButton& Inspector::toggle (const juce::String& labelText, std::function<void()> onChange)
{
    auto* t = new juce::ToggleButton (labelText);
    t->onClick = std::move (onChange);
    owned.add (t);
    add ("", *t, 24);
    return *t;
}

juce::Label& Inspector::readout (const juce::String& labelText, int height)
{
    auto* l = new juce::Label();
    l->setColour (juce::Label::textColourId, ui::text);
    l->setFont (juce::FontOptions (13.0f));
    l->setJustificationType (juce::Justification::topLeft);
    owned.add (l);
    add (labelText, *l, height);
    return *l;
}

bool Inspector::busy (juce::Component& c)
{
    return c.isMouseButtonDown (true) || c.hasKeyboardFocus (true);
}

void Inspector::paint (juce::Graphics& g) { g.fillAll (ui::panel); }

void Inspector::resized()
{
    viewport.setBounds (getLocalBounds());
    const int w = getWidth() - viewport.getScrollBarThickness() - 4;
    int top = 4;
    for (auto& r : rows)
    {
        if (r.label == nullptr)
            r.control->setBounds (10, top, w - 14, r.height);
        else if (r.label->getText().isEmpty())
            r.control->setBounds (10, top, w - 14, r.height);
        else
        {
            r.label->setBounds (10, top, 118, juce::jmin (24, r.height));
            r.control->setBounds (130, top, w - 134, r.height);
        }
        top += r.height + 4;
    }
    content.setSize (w, top + 8);
}

void Inspector::update (const juce::Array<MicInfo>& mics, const Scene& scene, int selectedSlot)
{
    const MicInfo* info = nullptr;
    for (auto& m : mics)
        if (m.index == selectedSlot) info = &m;

    const bool changedSelection = selectedSlot != slot;
    slot = info != nullptr ? selectedSlot : -1;
    content.setEnabled (info != nullptr);
    if (info == nullptr)
        return;

    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto& m = info->state;
    group = m.group;
    lastRole = m.role;
    const auto set = [&] (juce::Slider* s, double v) { if (changedSelection || ! busy (*s)) s->setValue (v, juce::dontSendNotification); };
    const auto pick = [&] (juce::ComboBox* c, int i) { if (changedSelection || ! busy (*c)) c->setSelectedItemIndex (i, juce::dontSendNotification); };

    if ((changedSelection || ! busy (name)) && name.getText() != m.name) name.setText (m.name, false);
    if (groupBox->getNumItems() != scene.numGroups || changedSelection || ! busy (*groupBox))
    {
        groupBox->clear (juce::dontSendNotification);
        for (int g = 0; g < scene.numGroups; ++g) groupBox->addItem (scene.groupNames[g], g + 1);
    }
    pick (role, (int) m.role); pick (groupBox, m.group); pick (pattern, (int) m.pattern);
    set (x, m.x); set (y, m.y); set (z, m.z); set (angle, m.angleDeg); set (sourceDistance, m.sourceDistance);
    delayOn->setToggleState (m.delayOn, juce::dontSendNotification);
    polarityOn->setToggleState (m.polarityOn, juce::dontSendNotification);
    phaseOn->setToggleState (m.phaseOn, juce::dontSendNotification);
    set (offset, m.offsetMs);
    panOn->setToggleState (m.panOn, juce::dontSendNotification);
    set (panAmount, m.panAmount * 100.0); set (timePan, m.timePan * 100.0);
    monoSource->setToggleState (m.monoSource, juce::dontSendNotification);

    const bool main = isMain (m.role);
    for (juce::Component* c : { (juce::Component*) pattern, (juce::Component*) x, (juce::Component*) y, (juce::Component*) z,
                                (juce::Component*) angle, (juce::Component*) sourceDistance, (juce::Component*) delayOn,
                                (juce::Component*) polarityOn, (juce::Component*) phaseOn, (juce::Component*) offset,
                                (juce::Component*) panOn, (juce::Component*) panAmount, (juce::Component*) timePan,
                                (juce::Component*) monoSource, (juce::Component*) &placeButton })
        c->setEnabled (! main);
    monoSource->setEnabled (! main && info->numIn == 2);

    // Readouts.
    static const char* tags[3] = { "L", "C", "R" };
    juce::String meas;
    for (int r = 0; r < 3; ++r)
    {
        const auto& v = info->results.vs[r];
        if (! v.valid) continue;
        const double msec = 1000.0 * v.delaySamples / info->sampleRate;
        meas << tags[r] << "   " << juce::String (msec, 3) << " ms   " << juce::String (std::abs (speedOfSound (scene.tempC) * msec * 0.001), 2)
             << " m   " << (v.polarity < 0 ? "inverted   " : "") << juce::roundToInt (v.confidence * 100.0) << " %\n";
    }
    measured->setText (main ? "This is a reference microphone" : meas.isEmpty() ? "Nothing yet. Press Analyze, then play." : meas.trimEnd(), juce::dontSendNotification);

    const auto& a = info->applied;
    juce::String app;
    if (! main)
    {
        const bool split = info->numOut == 2 && std::abs (a.delayLms - a.delayRms) > 0.005;
        app << "Delay  " << (split ? "L " + juce::String (a.delayLms, 3) + " / R " + juce::String (a.delayRms, 3) : juce::String (a.delayLms, 3)) << " ms"
            << (a.polarity < 0 ? "   polarity flipped" : "") << "\n";
        if (info->numOut == 2 && (info->numIn == 1 || m.monoSource))
            app << "Level  L " << juce::String (juce::Decibels::gainToDecibels (a.gainL), 1) << " / R " << juce::String (juce::Decibels::gainToDecibels (a.gainR), 1) << " dB";
        if (a.phaseTaps > 0) app << "   phase " << a.phaseTaps << " taps";
    }
    applied->setText (app, juce::dontSendNotification);
    note->setText (a.note, juce::dontSendNotification);

    // Main array of this microphone's group.
    const auto& arr = scene.arrays[group];
    arrayHeader->setText ("MAIN ARRAY  of group " + scene.groupNames[group], juce::dontSendNotification);
    if ((changedSelection || ! busy (groupName)) && groupName.getText() != scene.groupNames[group]) groupName.setText (scene.groupNames[group], false);
    pick (arrayType, (int) arr.type); pick (arrayPattern, (int) arr.pattern);
    set (spacing, arr.spacingCm); set (arrayAngle, arr.angleDeg); set (forward, arr.centreForwardCm);
    set (arrayHeight, arr.height); set (rotation, arr.rotationDeg);
    pick (panLaw, scene.panLaw); set (temperature, scene.tempC);
}

void Inspector::commitMic()
{
    if (refreshing || slot < 0) return;
    auto m = hub.getMic (slot);
    if (name.getText().trim().isNotEmpty() && name.getText() != m.name) { m.name = name.getText(); m.hostNamed = false; }
    m.role = (Role) juce::jmax (0, role->getSelectedItemIndex());
    m.group = juce::jmax (0, groupBox->getSelectedItemIndex());
    m.pattern = (Pattern) juce::jmax (0, pattern->getSelectedItemIndex());
    if (! juce::approximatelyEqual (m.x, x->getValue()) || ! juce::approximatelyEqual (m.y, y->getValue()))
        m.manualPosition = true;
    m.x = x->getValue(); m.y = y->getValue(); m.z = z->getValue();
    m.angleDeg = angle->getValue();
    m.sourceDistance = sourceDistance->getValue();
    m.delayOn = delayOn->getToggleState();
    m.polarityOn = polarityOn->getToggleState();
    m.phaseOn = phaseOn->getToggleState();
    m.offsetMs = offset->getValue();
    m.panOn = panOn->getToggleState();
    m.panAmount = panAmount->getValue() * 0.01;
    m.timePan = timePan->getValue() * 0.01;
    m.monoSource = monoSource->getToggleState();

    // Outriggers and ambience are measured but left free by default, to keep the room natural.
    if (m.role != lastRole && (m.role == Role::ambience || m.role == Role::outrigger))
        m.delayOn = m.phaseOn = false;
    lastRole = m.role;
    hub.setMic (slot, m);
}

void Inspector::commitScene (bool presetChosen)
{
    if (refreshing || slot < 0) return;
    auto sc = hub.getScene();
    auto& arr = sc.arrays[group];
    if (presetChosen)
        applyPreset (arr, (ArrayType) juce::jmax (0, arrayType->getSelectedItemIndex()));
    else
    {
        const auto pat = (Pattern) juce::jmax (0, arrayPattern->getSelectedItemIndex());
        if (pat != arr.pattern || ! juce::approximatelyEqual (arr.spacingCm, spacing->getValue())
            || ! juce::approximatelyEqual (arr.angleDeg, arrayAngle->getValue())
            || ! juce::approximatelyEqual (arr.centreForwardCm, forward->getValue()))
            arr.type = ArrayType::custom;
        arr.pattern = pat;
        arr.spacingCm = spacing->getValue();
        arr.angleDeg = arrayAngle->getValue();
        arr.centreForwardCm = forward->getValue();
    }
    arr.height = arrayHeight->getValue();
    arr.rotationDeg = rotation->getValue();
    if (groupName.getText().trim().isNotEmpty()) sc.groupNames[group] = groupName.getText();
    sc.panLaw = juce::jmax (0, panLaw->getSelectedItemIndex());
    sc.tempC = temperature->getValue();
    hub.setScene (sc);
}
