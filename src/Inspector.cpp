#include "Inspector.h"

using namespace arraylab;

//==============================================================================
void Swatches::paint (juce::Graphics& g)
{
    const float cell = (float) getWidth() / (ui::numMicColours + 1);
    for (int i = -1; i < ui::numMicColours; ++i)
    {
        auto r = juce::Rectangle<float> ((i + 1) * cell, 0.0f, cell, (float) getHeight()).reduced (2.0f);
        g.setColour (i < 0 ? automatic : ui::paletteColour (i));
        g.fillRoundedRectangle (r, 3.0f);
        if (i < 0)
        {
            g.setColour (juce::Colours::black.withAlpha (0.75f));
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText ("A", r, juce::Justification::centred, false);
        }
        if (i == current)
        {
            g.setColour (ui::text);
            g.drawRoundedRectangle (r.expanded (1.0f), 3.0f, 2.0f);
        }
    }
}

void Swatches::mouseDown (const juce::MouseEvent& e)
{
    const int i = (int) (e.position.x / ((float) getWidth() / (ui::numMicColours + 1))) - 1;
    if (onPick) onPick (juce::jlimit (-1, ui::numMicColours - 1, i));
}

//==============================================================================
Inspector::Inspector (Hub& h) : hub (h)
{
    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);

    const auto mic = [this] { commitMic(); };
    const auto sc = [this] { commitScene (false); };
    const auto deg = juce::String::fromUTF8 (" \xc2\xb0");

    header ("1  WHAT IT IS");
    name.onTextChange = mic;
    add ("Name", name, 24, always);
    swatches.onPick = [this] (int i) { if (slot >= 0) { auto m = hub.getMic (slot); m.colour = i; hub.setMic (slot, m); } };
    add ("Colour", swatches, 22, always);
    role = &combo ("Role", ui::roleNames(), mic);
    roleHelp = &help ({}, always, 32);
    groupBox = &combo ("Group", {}, mic);

    header ("2  WHERE IT IS", notMain);
    x = &slider ("Left / right", -60.0, 60.0, 0.05, " m", mic, notMain);
    y = &slider ("In front of array", -60.0, 60.0, 0.05, " m", mic, notMain);
    z = &slider ("Height", 0.0, 30.0, 0.05, " m", mic, notMain);
    angle = &slider ("Points towards", -180.0, 180.0, 1.0, deg, mic, notMain);
    help (juce::String::fromUTF8 ("0\xc2\xb0 = up the plan, 90\xc2\xb0 = right, 180\xc2\xb0 = down, towards the main array. The arrow on the plan shows it."), notMain);
    pattern = &combo ("Polar pattern", ui::patternNames(), mic, notMain);
    pairSpacing = &slider ("Pair: spacing", 0.0, 300.0, 1.0, " cm", mic, pairOnly);
    pairAngle = &slider ("Pair: angle", 0.0, 180.0, 1.0, deg, mic, pairOnly);
    sourceDistance = &slider ("Mic to instrument", 0.0, 10.0, 0.05, " m", mic, notMain);
    placeButton.onClick = [this] { hub.placeFromAnalysis (slot); };
    add ("", placeButton, 24, notMain);

    header ("3  WHAT WAS MEASURED", notMain);
    measured = &help ({}, notMain, 52);
    measured->setColour (juce::Label::textColourId, ui::text);

    header ("4  WHAT IS CORRECTED", notMain);
    delayOn = &toggle (mic, notMain);
    polarityOn = &toggle (mic, notMain);
    phaseOn = &toggle (mic, notMain);
    offset = &slider ("Extra delay", -20.0, 60.0, 0.1, " ms", mic, notMain);
    help ("Above zero, this mic arrives after the main array (more depth). Untick a box to bypass that correction.", notMain);
    note = &help ({}, notMain, 18);
    note->setColour (juce::Label::textColourId, ui::warn);

    header ("5  PAN", notMain);
    panBlocked = &help ("This track's output is mono, so ArrayLab cannot pan it: use the mixer's panner. "
                        "To let ArrayLab pan it, put the mic on a stereo track and tick the box below.", cannotPan, 58);
    panBlocked->setColour (juce::Label::textColourId, ui::warn);
    monoSource = &toggle (mic, stereoTrack);
    monoSource->setButtonText ("One mic on the left channel of a stereo track");
    panOn = &toggle (mic, canPan);
    panAmount = &slider ("Level difference", 0.0, 200.0, 1.0, " %", mic, canPan);
    timePan = &slider ("Time difference", 0.0, 100.0, 1.0, " %", mic, canPan);
    help ("100 % = the level and time differences the main pair itself would give this position. Move the mic on the plan to pan it.", canPan, 44);

    arrayHeader = &header ("2  THE MAIN ARRAY", mainOnly);
    help ("The reference everything else is aligned to. It is never delayed or changed.", mainOnly);
    arrayType = &combo ("Type", ui::arrayNames(), [this] { commitScene (true); }, mainOnly);
    arrayPattern = &combo ("Polar pattern", ui::patternNames(), sc, mainOnly);
    spacing = &slider ("Spacing L to R", 0.0, 1000.0, 1.0, " cm", sc, mainOnly);
    arrayAngle = &slider ("Angle between mics", 0.0, 180.0, 1.0, deg, sc, mainOnly);
    forward = &slider ("Centre mic forward", 0.0, 500.0, 1.0, " cm", sc, mainOnly);
    arrayHeight = &slider ("Height", 0.0, 30.0, 0.05, " m", sc, mainOnly);
    rotation = &slider ("Faces towards", -180.0, 180.0, 1.0, deg, sc, mainOnly);

    header ("SESSION");
    groupName.onTextChange = sc;
    add ("This group's name", groupName, 24, always);
    panLaw = &combo ("Pan law", { "-3 dB", "-4.5 dB", "-6 dB" }, sc);
    temperature = &slider ("Air temperature", -10.0, 40.0, 0.5, juce::String::fromUTF8 (" \xc2\xb0""C"), sc);
}

void Inspector::add (const juce::String& labelText, juce::Component& control, int height, int need)
{
    auto* label = new juce::Label ({}, labelText);
    label->setColour (juce::Label::textColourId, ui::dim);
    label->setFont (juce::FontOptions (13.0f));
    owned.add (label);
    content.addChildComponent (label);
    content.addChildComponent (control);
    rows.push_back ({ label, &control, height, need });
}

juce::Label& Inspector::header (const juce::String& text, int need)
{
    auto* l = new juce::Label ({}, text);
    l->setColour (juce::Label::textColourId, ui::accent);
    l->setFont (juce::FontOptions (12.5f, juce::Font::bold));
    l->setJustificationType (juce::Justification::bottomLeft);
    owned.add (l);
    content.addChildComponent (l);
    rows.push_back ({ nullptr, l, 30, need });
    return *l;
}

juce::Label& Inspector::help (const juce::String& text, int need, int height)
{
    auto* l = new juce::Label ({}, text);
    l->setColour (juce::Label::textColourId, ui::dim);
    l->setFont (juce::FontOptions (12.0f));
    l->setJustificationType (juce::Justification::topLeft);
    l->setMinimumHorizontalScale (1.0f);
    owned.add (l);
    content.addChildComponent (l);
    rows.push_back ({ nullptr, l, height, need });
    return *l;
}

juce::Slider& Inspector::slider (const juce::String& labelText, double lo, double hi, double step, const juce::String& suffix, std::function<void()> onChange, int need)
{
    auto* s = new juce::Slider (juce::Slider::LinearBar, juce::Slider::TextBoxLeft);
    s->setRange (lo, hi, step);
    s->setTextValueSuffix (suffix);
    s->setColour (juce::Slider::trackColourId, ui::accent.withAlpha (0.35f));
    s->setColour (juce::Slider::backgroundColourId, ui::panel2);
    s->onValueChange = std::move (onChange);
    owned.add (s);
    add (labelText, *s, 24, need);
    return *s;
}

juce::ComboBox& Inspector::combo (const juce::String& labelText, const juce::StringArray& items, std::function<void()> onChange, int need)
{
    auto* c = new juce::ComboBox();
    c->addItemList (items, 1);
    c->onChange = std::move (onChange);
    owned.add (c);
    add (labelText, *c, 24, need);
    return *c;
}

juce::ToggleButton& Inspector::toggle (std::function<void()> onChange, int need)
{
    auto* t = new juce::ToggleButton();
    t->onClick = std::move (onChange);
    owned.add (t);
    add ("", *t, 24, need);
    return *t;
}

bool Inspector::busy (juce::Component& c)
{
    return c.isMouseButtonDown (true) || c.hasKeyboardFocus (true);
}

void Inspector::paint (juce::Graphics& g) { g.fillAll (ui::panel); }

void Inspector::resized()
{
    viewport.setBounds (getLocalBounds());
    layout();
}

void Inspector::layout()
{
    const int w = getWidth() - viewport.getScrollBarThickness() - 4;
    int top = 0;
    for (auto& r : rows)
    {
        const bool show = context >= 0 && (r.need & ~context) == 0;
        r.control->setVisible (show);
        if (r.label != nullptr) r.label->setVisible (show);
        if (! show) continue;

        if (r.label == nullptr || r.label->getText().isEmpty())
            r.control->setBounds (10, top, w - 14, r.height);
        else
        {
            r.label->setBounds (10, top, 126, juce::jmin (24, r.height));
            r.control->setBounds (138, top, w - 142, r.height);
        }
        top += r.height + 4;
    }
    content.setSize (juce::jmax (10, w), top + 10);
}

void Inspector::update (const juce::Array<MicInfo>& mics, const Scene& scene, int selectedSlot)
{
    const MicInfo* info = nullptr;
    for (auto& m : mics)
        if (m.index == selectedSlot) info = &m;

    const bool changedSelection = selectedSlot != slot;
    slot = info != nullptr ? selectedSlot : -1;
    if (info == nullptr)
    {
        if (context != -1) { context = -1; layout(); }
        return;
    }

    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto& m = info->state;
    const bool main = isMain (m.role);
    group = m.group;
    lastRole = m.role;

    const int now = (main ? mainOnly : notMain) | (info->isStereoPair() ? pairOnly : 0) | (info->canPan() ? canPan : 0)
                  | (! main && info->numIn == 2 ? stereoTrack : 0) | (! main && ! info->canPan() ? cannotPan : 0);
    if (now != context) { context = now; layout(); }

    const auto set = [&] (juce::Slider* s, double v) { if (changedSelection || ! busy (*s)) s->setValue (v, juce::dontSendNotification); };
    const auto pick = [&] (juce::ComboBox* c, int i) { if (changedSelection || ! busy (*c)) c->setSelectedItemIndex (i, juce::dontSendNotification); };
    const auto tick = [] (juce::ToggleButton* t, bool on, const juce::String& text) { t->setToggleState (on, juce::dontSendNotification); t->setButtonText (text); };

    // 1
    if ((changedSelection || ! busy (name)) && name.getText() != m.name) name.setText (m.name, false);
    swatches.set (m.colour, ui::groupColour (m.group));
    if (groupBox->getNumItems() != scene.numGroups || changedSelection || ! busy (*groupBox))
    {
        groupBox->clear (juce::dontSendNotification);
        for (int g = 0; g < scene.numGroups; ++g) groupBox->addItem (scene.groupNames[g], g + 1);
    }
    pick (role, (int) m.role); pick (groupBox, m.group);
    static const char* roleText[] = {
        "A close microphone. It is delayed so its sound lines up with the main array.",
        "Left microphone of the main array: a reference.", "Right microphone of the main array: a reference.",
        "Centre microphone of the main array: a reference.", "Both microphones of the main pair, on one stereo track: the reference.",
        "Measured, but left free unless you tick Delay below.", "Measured, but left free to keep the room natural, unless you tick Delay below." };
    roleHelp->setText (roleText[(int) m.role], juce::dontSendNotification);

    // 2
    pick (pattern, (int) m.pattern);
    set (x, m.x); set (y, m.y); set (z, m.z); set (angle, m.angleDeg); set (sourceDistance, m.sourceDistance);
    set (pairSpacing, m.pairSpacingCm); set (pairAngle, m.pairAngleDeg);

    // 3
    static const char* refName[3] = { "main L", "main C", "main R" };
    juce::String meas;
    for (int r = 0; r < 3; ++r)
    {
        const auto v = combinedResult (info->results, r, info->isStereoPair());
        if (! v.valid) continue;
        const double msec = 1000.0 * v.delaySamples / info->sampleRate;
        meas << juce::String (std::abs (msec), 2) << " ms " << (msec <= 0.0 ? "before " : "after ") << refName[r]
             << "  (" << juce::String (std::abs (speedOfSound (scene.tempC) * msec * 0.001), 2) << " m)"
             << (v.polarity < 0 ? "  inverted" : "") << "  " << juce::roundToInt (v.confidence * 100.0) << " % sure\n";
    }
    measured->setText (meas.isEmpty() ? "Nothing yet. Press Analyze, then play the music." : meas.trimEnd(), juce::dontSendNotification);

    // 4
    const auto& a = info->applied;
    const bool split = info->numOut == 2 && std::abs (a.delayLms - a.delayRms) > 0.005;
    tick (delayOn, m.delayOn, "Delay:  " + (m.delayOn ? (split ? "L " + juce::String (a.delayLms, 2) + " / R " + juce::String (a.delayRms, 2) : juce::String (a.delayLms, 2)) + " ms" : juce::String ("bypassed")));
    tick (polarityOn, m.polarityOn, "Polarity:  " + juce::String (! m.polarityOn ? "bypassed" : a.polarity < 0 ? "flipped" : "already right"));
    tick (phaseOn, m.phaseOn, "Phase by frequency:  " + juce::String (! m.phaseOn ? "bypassed" : a.phaseTaps > 0 ? "on" : "waiting for an analysis"));
    set (offset, m.offsetMs);
    note->setText (a.note, juce::dontSendNotification);

    // 5
    monoSource->setToggleState (m.monoSource, juce::dontSendNotification);
    tick (panOn, m.panOn, "Pan:  " + (m.panOn ? "L " + juce::String (juce::Decibels::gainToDecibels (a.gainL), 1) + " / R " + juce::String (juce::Decibels::gainToDecibels (a.gainR), 1) + " dB"
                                             : juce::String ("bypassed (centre)")));
    set (panAmount, m.panAmount * 100.0); set (timePan, m.timePan * 100.0);
    panAmount->setEnabled (m.panOn);
    timePan->setEnabled (m.panOn);

    // Main array of this microphone's group, and session.
    const auto& arr = scene.arrays[group];
    arrayHeader->setText ("2  THE MAIN ARRAY  of group " + scene.groupNames[group], juce::dontSendNotification);
    pick (arrayType, (int) arr.type); pick (arrayPattern, (int) arr.pattern);
    set (spacing, arr.spacingCm); set (arrayAngle, arr.angleDeg); set (forward, arr.centreForwardCm);
    set (arrayHeight, arr.height); set (rotation, arr.rotationDeg);
    if ((changedSelection || ! busy (groupName)) && groupName.getText() != scene.groupNames[group]) groupName.setText (scene.groupNames[group], false);
    pick (panLaw, scene.panLaw); set (temperature, scene.tempC);
}

void Inspector::commitMic()
{
    if (refreshing || slot < 0) return;
    auto m = hub.getMic (slot);
    if (name.getText().trim().isNotEmpty() && name.getText() != m.name) { m.name = name.getText(); m.hostNamed = false; }
    m.role = (Role) juce::jmax (0, role->getSelectedItemIndex());
    m.group = juce::jmax (0, groupBox->getSelectedItemIndex());
    if (! isMain (lastRole))
    {
        m.pattern = (Pattern) juce::jmax (0, pattern->getSelectedItemIndex());
        if (! juce::approximatelyEqual (m.x, x->getValue()) || ! juce::approximatelyEqual (m.y, y->getValue()))
            m.manualPosition = true;
        m.x = x->getValue(); m.y = y->getValue(); m.z = z->getValue();
        m.angleDeg = angle->getValue();
        m.sourceDistance = sourceDistance->getValue();
        m.pairSpacingCm = pairSpacing->getValue();
        m.pairAngleDeg = pairAngle->getValue();
        m.delayOn = delayOn->getToggleState();
        m.polarityOn = polarityOn->getToggleState();
        m.phaseOn = phaseOn->getToggleState();
        m.offsetMs = offset->getValue();
        m.panOn = panOn->getToggleState();
        m.panAmount = panAmount->getValue() * 0.01;
        m.timePan = timePan->getValue() * 0.01;
        m.monoSource = monoSource->getToggleState();
    }

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
    if ((context & mainOnly) != 0)
    {
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
    }
    if (groupName.getText().trim().isNotEmpty()) sc.groupNames[group] = groupName.getText();
    sc.panLaw = juce::jmax (0, panLaw->getSelectedItemIndex());
    sc.tempC = temperature->getValue();
    hub.setScene (sc);
}
