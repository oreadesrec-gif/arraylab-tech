#include "StripView.h"

using namespace arraylab;

namespace
{
juce::String signedDb (double v)
{
    if (std::abs (v) < 0.05) v = 0.0;
    return juce::String (v >= 0.0 ? "+" : "") + juce::String (v, 1) + " dB";
}
}

//==============================================================================
class StripView::Row : public juce::Component
{
public:
    Row (Hub& h, std::function<void (int)> select) : hub (h), onSelect (std::move (select))
    {
        for (auto* l : { &name, &gain, &steady })
        {
            l->setInterceptsMouseClicks (false, false);
            l->setFont (juce::FontOptions (14.0f));
            addAndMakeVisible (*l);
        }
        steady.setFont (juce::FontOptions (12.5f));

        const auto bar = [this] (juce::Slider& s, double lo, double hi, double step, const juce::String& suffix)
        {
            s.setSliderStyle (juce::Slider::LinearBar);
            s.setRange (lo, hi, step);
            s.setTextValueSuffix (suffix);
            s.setColour (juce::Slider::trackColourId, ui::accent.withAlpha (0.35f));
            s.setColour (juce::Slider::backgroundColourId, ui::panel2);
            addAndMakeVisible (s);
        };
        bar (delay, 0.0, 200.0, 0.001, " ms");
        bar (rotation, -180.0, 180.0, 1.0, juce::String::fromUTF8 (" \xc2\xb0"));
        bar (level, -30.0, 12.0, 0.5, " dB");
        delay.setNumDecimalPlacesToDisplay (3);

        // The delay shown is the one applied; moving it adds to or takes from what the analysis found.
        delay.onValueChange = [this] { edit ([this] (MicState& m) { m.offsetMs += delay.getValue() - shown; }); };
        rotation.onValueChange = [this] { edit ([this] (MicState& m) { m.phaseRotationDeg = rotation.getValue(); }); };
        level.onValueChange = [this] { edit ([this] (MicState& m) { m.mixLevelDb = level.getValue(); }); };

        for (auto* b : { &on, &flip, &solo, &lock })
        {
            b->setClickingTogglesState (true);
            addAndMakeVisible (*b);
        }
        on.setColour (juce::TextButton::buttonOnColourId, ui::good.darker (0.5f));
        flip.setColour (juce::TextButton::buttonOnColourId, ui::warn.darker (0.3f));
        solo.setColour (juce::TextButton::buttonOnColourId, ui::accent.darker (0.2f));
        lock.setColour (juce::TextButton::buttonOnColourId, ui::warn.darker (0.5f));
        on.onClick = [this] { edit ([this] (MicState& m) { m.enabled = on.getToggleState(); }); };
        flip.onClick = [this] { edit ([] (MicState& m) { m.flip = ! m.flip; }); };
        lock.onClick = [this] { edit ([this] (MicState& m) { m.measureLocked = lock.getToggleState(); }); };
        solo.onClick = [this] { hub.setSolo (hub.solo() == slot ? -1 : slot); };

        // One sample at a time, for setting a delay by ear while the music plays.
        minus.onClick = [this] { edit ([this] (MicState& m) { m.offsetMs -= 1000.0 / juce::jmax (1.0, rate); }); };
        plus.onClick = [this] { edit ([this] (MicState& m) { m.offsetMs += 1000.0 / juce::jmax (1.0, rate); }); };
        addAndMakeVisible (minus);
        addAndMakeVisible (plus);
    }

    void update (const MicInfo& info, bool isSelected, int soloSlot)
    {
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        slot = info.index;
        rate = info.sampleRate;
        selected = isSelected;
        colour = ui::micColour (info.state.colour, info.state.group);
        const auto& m = info.state;
        const auto& a = info.applied;
        const bool main = isMain (m.role);

        name.setText (m.name, juce::dontSendNotification);
        name.setColour (juce::Label::textColourId, isSelected ? ui::text : colour);
        for (juce::Component* c : { (juce::Component*) &on, (juce::Component*) &delay, (juce::Component*) &minus, (juce::Component*) &plus,
                                    (juce::Component*) &flip, (juce::Component*) &rotation, (juce::Component*) &level, (juce::Component*) &steady,
                                    (juce::Component*) &solo, (juce::Component*) &lock })
            c->setVisible (! main);

        if (main)
        {
            gain.setText ("reference", juce::dontSendNotification);
            gain.setColour (juce::Label::textColourId, ui::dim);
            repaint();
            return;
        }

        const auto& best = info.results.best;
        gain.setText (best.valid ? signedDb (best.gainDb - best.asRecordedDb) : juce::String ("-"), juce::dontSendNotification);
        gain.setColour (juce::Label::textColourId, best.worthwhile() ? ui::good : ui::dim);

        on.setToggleState (m.enabled, juce::dontSendNotification);
        shown = juce::jmax (a.delayLms, a.delayRms);
        if (! delay.isMouseButtonDown (true) && ! delay.hasKeyboardFocus (true))
            delay.setValue (shown, juce::dontSendNotification);
        const bool delayed = m.enabled && m.delayOn && (shown > 0.0 || a.ref >= 0 || info.results.best.worthwhile());
        delay.setEnabled (delayed);
        minus.setEnabled (delayed);
        plus.setEnabled (delayed);
        flip.setToggleState (a.polarity < 0, juce::dontSendNotification);
        if (! rotation.isMouseButtonDown (true) && ! rotation.hasKeyboardFocus (true))
            rotation.setValue (m.phaseRotationDeg, juce::dontSendNotification);
        if (! level.isMouseButtonDown (true) && ! level.hasKeyboardFocus (true))
            level.setValue (m.mixLevelDb, juce::dontSendNotification);

        // Does one delay hold for the whole passage?
        const auto& r = info.results;
        if (r.stabilityWindows >= 2 && rate > 0.0)
        {
            const double ms = 1000.0 * r.stabilitySpread / rate;
            steady.setText (juce::String::fromUTF8 ("\xc2\xb1 ") + juce::String (ms, 2) + " ms", juce::dontSendNotification);
            steady.setColour (juce::Label::textColourId, ms < 0.05 ? ui::good : ms < 0.3 ? ui::warn : ui::bad);
        }
        else
            steady.setText ({}, juce::dontSendNotification);

        solo.setToggleState (soloSlot == slot, juce::dontSendNotification);
        lock.setToggleState (m.measureLocked, juce::dontSendNotification);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (selected) g.fillAll (ui::accent.withAlpha (0.16f));
        g.setColour (colour);
        g.fillRect (0, 3, 4, getHeight() - 6);
        g.setColour (ui::line.withAlpha (0.5f));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        const int h = getHeight() - 6, y = 3;
        using C = StripView::Columns;
        name.setBounds (C::name, y, C::on - C::name - 6, h);
        on.setBounds (C::on, y, 38, h);
        gain.setBounds (C::gain, y, C::delay - C::gain - 4, h);
        delay.setBounds (C::delay, y, C::minus - C::delay - 4, h);
        minus.setBounds (C::minus, y, 22, h);
        plus.setBounds (C::plus, y, 22, h);
        flip.setBounds (C::flip, y, 30, h);
        rotation.setBounds (C::rotation, y, C::level - C::rotation - 8, h);
        level.setBounds (C::level, y, C::steady - C::level - 8, h);
        steady.setBounds (C::steady, y, C::solo - C::steady - 4, h);
        solo.setBounds (C::solo, y, 46, h);
        lock.setBounds (C::lock, y, 46, h);
    }

    void mouseDown (const juce::MouseEvent&) override { if (onSelect) onSelect (slot); }

private:
    void edit (const std::function<void (MicState&)>& change)
    {
        if (refreshing || slot < 0) return;
        auto m = hub.getMic (slot);
        change (m);
        hub.setMic (slot, m);
        if (onSelect) onSelect (slot);
    }

    Hub& hub;
    std::function<void (int)> onSelect;
    int slot = -1;
    double rate = 48000.0, shown = 0.0;
    bool selected = false, refreshing = false;
    juce::Colour colour;
    juce::Label name, gain, steady;
    juce::Slider delay, rotation, level;
    juce::TextButton on { "On" }, minus { "-" }, plus { "+" }, flip { juce::String::fromUTF8 ("\xc3\x98") }, solo { "Solo" }, lock { "Lock" };
};

//==============================================================================
StripView::StripView (Hub& h) : hub (h)
{
    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);

    leadLabel.setColour (juce::Label::textColourId, ui::text);
    leadLabel.setFont (juce::FontOptions (13.5f));
    addAndMakeVisible (leadLabel);
    lead.setRange (0.0, 40.0, 0.1);
    lead.setTextValueSuffix (" ms");
    lead.setColour (juce::Slider::trackColourId, ui::accent.withAlpha (0.35f));
    lead.setColour (juce::Slider::backgroundColourId, ui::panel2);
    lead.onValueChange = [this]
    {
        if (refreshing) return;
        auto sc = hub.getScene();
        sc.leadMs = lead.getValue();
        hub.setScene (sc);
    };
    addAndMakeVisible (lead);
}

StripView::~StripView() = default;

void StripView::update (const juce::Array<MicInfo>& mics, const Scene& scene, int selectedSlot, int groupFilter)
{
    {
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        if (! lead.isMouseButtonDown (true) && ! lead.hasKeyboardFocus (true))
            lead.setValue (scene.leadMs, juce::dontSendNotification);
    }

    juce::Array<MicInfo> shown;
    for (auto& m : mics)
        if (groupFilter < 0 || m.state.group == groupFilter)
            shown.add (m);

    while (rows.size() < shown.size())
        content.addAndMakeVisible (rows.add (new Row (hub, [this] (int slot) { if (onSelect) onSelect (slot); })));
    while (rows.size() > shown.size())
        rows.removeLast();

    const int solo = hub.solo();
    for (int i = 0; i < shown.size(); ++i)
        rows[i]->update (shown.getReference (i), shown.getReference (i).index == selectedSlot, solo);
    resized();
}

void StripView::resized()
{
    auto area = getLocalBounds().reduced (8, 6);
    auto top = area.removeFromTop (26);
    leadLabel.setBounds (top.removeFromLeft (300));
    lead.setBounds (top.removeFromLeft (120));
    area.removeFromTop (8 + 22); // column titles are painted here
    viewport.setBounds (area);
    const int w = juce::jmax (Columns::end, area.getWidth() - viewport.getScrollBarThickness());
    for (int i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, i * 32, w, 32);
    content.setSize (w, rows.size() * 32);
}

void StripView::paint (juce::Graphics& g)
{
    g.fillAll (ui::bg);
    const int y = 6 + 26 + 8, x0 = 8;
    g.setColour (ui::panel);
    g.fillRect (x0, y, getWidth() - 16, 22);
    g.setColour (ui::dim);
    g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    const auto title = [&] (int x, int w, const char* text) { g.drawText (text, x0 + x, y, w, 22, juce::Justification::centredLeft, false); };
    title (Columns::name, 150, "Microphone");
    title (Columns::on, 40, "In");
    title (Columns::gain, 70, "To gain");
    title (Columns::delay, 160, "Delay   (-/+ one sample)");
    title (Columns::flip, 36, "Pol.");
    title (Columns::rotation, 90, "Phase turn");
    title (Columns::level, 90, "Level in mix");
    title (Columns::steady, 80, "Delay holds");
    title (Columns::solo, 100, "Hear with main");
    g.setColour (ui::dim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("To gain: fuller sum with the main array once lined up.   Delay holds: its drift during the passage.",
                getLocalBounds().reduced (8, 6).removeFromTop (26).withTrimmedLeft (440), juce::Justification::centredLeft, true);
}
