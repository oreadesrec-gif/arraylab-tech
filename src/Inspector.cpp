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
    partnerBox = &combo ("Pair with", {}, [this]
    {
        if (refreshing || slot < 0) return;
        const int i = partnerBox->getSelectedItemIndex();
        hub.linkMics (slot, i > 0 && i - 1 < partnerSlots.size() ? partnerSlots[i - 1] : -1);
    }, notMain);
    help ("If this mic and another are the two halves of one pair on two tracks, link them: they then share one delay, one polarity and one phase correction.", notMain, 44);

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
    measured = &help ({}, notMain, 132);
    measured->setColour (juce::Label::textColourId, ui::text);
    measureLock = &toggle (mic, notMain);

    header ("4  WHAT IS CORRECTED", notMain);
    alignTo = &combo ("Line up with", { "Whatever makes the sum fullest", "Nearest main mic (first arrival)", "Main L", "Main C", "Main R", "Average of L and R", "Sum of the main array" }, mic, notMain);
    delayOn = &toggle (mic, notMain);
    polarityOn = &toggle (mic, notMain);
    phaseOn = &toggle (mic, notMain);
    phaseAmount = &slider ("Phase: how much", 0.0, 100.0, 1.0, " %", mic, notMain);
    phaseLow = &slider ("Phase: from", 20.0, 2000.0, 1.0, " Hz", mic, notMain);
    phaseHigh = &slider ("Phase: up to", 200.0, 20000.0, 1.0, " Hz", mic, notMain);
    phaseLow->setSkewFactorFromMidPoint (200.0);
    phaseHigh->setSkewFactorFromMidPoint (2000.0);
    phaseRotation = &slider ("Phase: turn by hand", -180.0, 180.0, 1.0, deg, mic, notMain);
    rotationButton.onClick = [this] { phaseRotation->setValue (suggestedRotation, juce::sendNotificationSync); };
    add ("", rotationButton, 24, notMain);
    help ("Turns the whole microphone's phase, to the degree; 180 is a polarity flip. It adds to Phase by frequency, so use one or the other. Type a value by double-clicking the slider.", notMain, 44);
    offset = &slider ("Extra delay", -20.0, 60.0, 0.1, " ms", mic, notMain);
    help ("Untick a box to bypass that correction. The Detail view shows the peak chosen and the phase before and after.", notMain);
    note = &help ({}, notMain, 62);
    note->setColour (juce::Label::textColourId, ui::warn);

    header ("5  PAN", notMain);
    panStatus = &help ({}, notMain, 62);
    monoSource = &toggle (mic, stereoTrack);
    monoSource->setButtonText ("This stereo track carries one mic, on its left channel");
    panOn = &toggle (mic, canPan);
    pan = &slider ("Position", -100.0, 100.0, 1.0, {}, [this] { followPlan->setToggleState (false, juce::dontSendNotification); commitMic(); }, canPan);
    pan->textFromValueFunction = [] (double v) { return panText (v * 0.01); };
    pan->valueFromTextFunction = [] (const juce::String& t)
    {
        const double v = t.retainCharacters ("0123456789.").getDoubleValue();
        return t.containsIgnoreCase ("L") ? -v : v;
    };
    pairWidth = &slider ("Width", 0.0, 100.0, 1.0, " %", mic, pairOnly);
    followPlan = &toggle (mic, canPan);
    followPlan->setButtonText ("Follow the plan");
    help (juce::String::fromUTF8 ("Ticked, Position follows the mic on the plan: straight ahead of the main array is centre, 45\xc2\xb0 to one side is fully that side. Move the slider to set it by hand."), canPan, 58);
    timeDifference = &toggle (mic, monoPan);
    timeDifference->setButtonText ("Line up left and right separately in time");
    help ("Left is timed on main L and right on main R, instead of both on one reference. Subtle; try it with a spaced main pair.", monoPan, 44);

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
    allowAdvance = &toggle (sc);
    allowAdvance->setButtonText ("Line up mics that arrive after the main array");
    help ("Such a mic cannot be moved earlier, so every other track is moved later by the same amount (a few ms, not reported to the host).", always, 44);
    precision = &combo ("Phase precision", { "Live: no latency", "Full resolution: adds latency" }, sc);
    help ("Full resolution corrects the phase precisely down to the lowest notes. Every ArrayLab instance then reports the same latency (about 85 ms) to the host.", always, 44);
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

juce::String Inspector::panText (double p)
{
    const int v = juce::roundToInt (std::abs (p) * 100.0);
    return v == 0 ? juce::String ("Centre") : (p < 0.0 ? "L " : "R ") + juce::String (v);
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
                  | (! main && info->numIn == 2 ? stereoTrack : 0) | (! main && ! info->canPan() ? cannotPan : 0)
                  | (info->canPan() && ! info->isStereoPair() ? monoPan : 0);
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
    if (changedSelection || ! busy (*partnerBox))
    {
        partnerBox->clear (juce::dontSendNotification);
        partnerSlots.clearQuick();
        partnerBox->addItem ("(not linked)", 1);
        int chosen = 0;
        for (auto& o : mics)
        {
            if (o.index == info->index || isMain (o.state.role)) continue;
            partnerSlots.add (o.index);
            partnerBox->addItem (o.state.name, partnerSlots.size() + 1);
            if (o.index == info->partnerIndex) chosen = partnerSlots.size();
        }
        partnerBox->setSelectedItemIndex (chosen, juce::dontSendNotification);
    }
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
    static const char* refName[kNumRefs] = { "main L", "main C", "main R", "the main sum" };
    const bool pair = info->isStereoPair();
    juce::String meas;
    for (int r = 0; r < kNumRefs; ++r)
    {
        const auto v = effectiveResult (info->results, m, r, pair);
        if (! v.valid) continue;
        const double msec = 1000.0 * v.delaySamples / info->sampleRate;
        meas << juce::String (std::abs (msec), 2) << " ms " << (msec <= 0.0 ? "before " : "after ") << refName[r]
             << "  (" << juce::String (std::abs (speedOfSound (scene.tempC) * msec * 0.001), 2) << " m)"
             << (v.polarity < 0 ? "  inverted" : "") << (v.locked ? "  picked by you" : v.reliable ? "  sure" : "  NOT SURE") << "\n";
    }
    const auto& a = info->applied;
    if (a.ref >= 0 && info->results.zones[a.ref].valid)
    {
        const auto& zone = info->results.zones[a.ref];
        const auto part = [] (double ms) { return juce::String (std::abs (ms), 2) + (ms >= 0.0 ? " ms late" : " ms early"); };
        meas << "By zone: lows " << part (zone.lowMs) << ", mids " << part (zone.midMs) << ", highs " << part (zone.highMs);
    }
    if (const auto& best = info->results.best; best.valid)
    {
        const auto db = [] (double v) { if (std::abs (v) < 0.05) v = 0.0; return juce::String (v >= 0.0 ? "+" : "") + juce::String (v, 1) + " dB"; };
        meas = "Sum with the main array, 63 Hz to 4 kHz:\n"
               "  as recorded " + db (best.asRecordedDb) + ",  lined up " + db (best.gainDb) + "\n"
               "  with a " + juce::String (juce::roundToInt (best.rotationDeg)) + juce::String::fromUTF8 ("\xc2\xb0") + " rotation " + db (best.withRotationDb)
               + ",  at the very most " + db (best.limitDb) + "\n"
               + (best.worthwhile() ? juce::String ("Worth lining up: ") + db (best.gainDb - best.asRecordedDb) + " to gain.\n"
                                    : juce::String ("Nothing to gain by lining it up.\n"))
               + meas;
    }
    measured->setText (meas.isEmpty() ? "Nothing yet. Press Analyze, then play the music." : meas.trimEnd(), juce::dontSendNotification);
    tick (measureLock, m.measureLocked, m.measureLocked ? "Locked: Analyze and Reset leave this mic alone" : "Lock this measurement");

    // 4
    pick (alignTo, m.alignTo == AlignTo::best ? 0 : (int) m.alignTo + 1);
    const bool split = info->numOut == 2 && std::abs (a.delayLms - a.delayRms) > 0.0005;
    tick (delayOn, m.delayOn, "Delay:  " + (m.delayOn ? (split ? "L " + juce::String (a.delayLms, 3) + " / R " + juce::String (a.delayRms, 3) : juce::String (a.delayLms, 3)) + " ms"
                                                        + (a.ref >= 0 ? juce::String (" on ") + refName[a.ref] : juce::String())
                                                      : juce::String ("bypassed")));
    tick (polarityOn, m.polarityOn, "Polarity:  " + juce::String (! m.polarityOn ? "bypassed" : a.polarity < 0 ? "flipped" : "already right"));
    tick (phaseOn, m.phaseOn, "Phase by frequency:  " + juce::String (! m.phaseOn ? "bypassed" : a.phaseTaps > 0 ? "on" : "waiting for a sure analysis"));
    set (phaseAmount, m.phaseAmount * 100.0); set (phaseLow, m.phaseLowHz); set (phaseHigh, m.phaseHighHz);
    set (phaseRotation, m.phaseRotationDeg);
    suggestedRotation = a.suggestedRotationDeg;
    rotationButton.setEnabled (a.hasSuggestedRotation);
    rotationButton.setButtonText (a.hasSuggestedRotation ? "Set it to the measured " + juce::String (juce::roundToInt (a.suggestedRotationDeg)) + juce::String::fromUTF8 ("\xc2\xb0")
                                                         : juce::String ("No measured rotation yet"));
    phaseAmount->setEnabled (m.phaseOn);
    set (offset, m.offsetMs);
    note->setText (a.note, juce::dontSendNotification);

    // 5
    const bool panning = info->panIsOn();
    selectedIsPair = pair;
    panStatus->setColour (juce::Label::textColourId, info->canPan() ? ui::dim : ui::warn);
    panStatus->setText (! info->canPan() ? "This track leaves the plugin in mono, so ArrayLab cannot pan it: use the mixer's panner. From its place on the plan, the position would be " + panText (a.suggestedPan) + "."
                        : pair ? juce::String ("A stereo pair. It stays exactly as recorded until you tick the box below. Then Position moves the pair in the image, and Width narrows it.")
                               : juce::String ("One microphone on a stereo output. Position places it between left and right, like the pan pot of a mixer."),
                        juce::dontSendNotification);
    monoSource->setToggleState (m.monoSource, juce::dontSendNotification);
    if (pair)
        tick (panOn, m.pairPanOn, "Pan this pair:  " + (m.pairPanOn ? panText (a.pan) + ", width " + juce::String (juce::roundToInt (m.pairWidth * 100.0)) + " %" : juce::String ("off, as recorded")));
    else
        tick (panOn, m.panOn, "Pan:  " + (m.panOn ? panText (a.pan) + "   (L " + juce::String (juce::Decibels::gainToDecibels (a.gainL, -60.0f), 1)
                                                      + " / R " + juce::String (juce::Decibels::gainToDecibels (a.gainR, -60.0f), 1) + " dB)"
                                                 : juce::String ("bypassed (centre)")));
    set (pan, a.pan * 100.0);
    set (pairWidth, m.pairWidth * 100.0);
    followPlan->setToggleState (m.panFollowsPlan, juce::dontSendNotification);
    timeDifference->setToggleState (m.timeDifference, juce::dontSendNotification);
    for (juce::Component* c : { (juce::Component*) pan, (juce::Component*) pairWidth, (juce::Component*) followPlan, (juce::Component*) timeDifference })
        c->setEnabled (panning);

    // Main array of this microphone's group, and session.
    const auto& arr = scene.arrays[group];
    arrayHeader->setText ("2  THE MAIN ARRAY  of group " + scene.groupNames[group], juce::dontSendNotification);
    pick (arrayType, (int) arr.type); pick (arrayPattern, (int) arr.pattern);
    set (spacing, arr.spacingCm); set (arrayAngle, arr.angleDeg); set (forward, arr.centreForwardCm);
    set (arrayHeight, arr.height); set (rotation, arr.rotationDeg);
    if ((changedSelection || ! busy (groupName)) && groupName.getText() != scene.groupNames[group]) groupName.setText (scene.groupNames[group], false);
    pick (panLaw, scene.panLaw); pick (precision, scene.precision); set (temperature, scene.tempC);
    allowAdvance->setToggleState (scene.allowAdvance, juce::dontSendNotification);
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
        m.alignTo = alignTo->getSelectedItemIndex() <= 0 ? AlignTo::best : (AlignTo) (alignTo->getSelectedItemIndex() - 1);
        m.phaseAmount = phaseAmount->getValue() * 0.01;
        m.phaseLowHz = phaseLow->getValue();
        m.phaseHighHz = juce::jmax (phaseHigh->getValue(), m.phaseLowHz * 1.5);
        m.phaseRotationDeg = phaseRotation->getValue();
        m.measureLocked = measureLock->getToggleState();
        if (selectedIsPair) m.pairPanOn = panOn->getToggleState(); else m.panOn = panOn->getToggleState();
        m.pairWidth = pairWidth->getValue() * 0.01;
        m.panFollowsPlan = followPlan->getToggleState();
        if (! m.panFollowsPlan) m.pan = pan->getValue() * 0.01;
        m.timeDifference = timeDifference->getToggleState();
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
    sc.precision = juce::jmax (0, precision->getSelectedItemIndex());
    sc.allowAdvance = allowAdvance->getToggleState();
    sc.tempC = temperature->getValue();
    hub.setScene (sc);
}
