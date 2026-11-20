#include "CheckView.h"

using namespace arraylab;

CheckView::CheckView()
{
    for (auto* b : { &allButton, &focusButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (7);
        b->setColour (juce::TextButton::buttonOnColourId, ui::accent.withAlpha (0.55f));
        addAndMakeVisible (*b);
    }
    allButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    focusButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    allButton.setToggleState (true, juce::dontSendNotification);
    refineButton.onClick = [this] { if (onRefine) onRefine(); };
    clearButton.onClick = [this] { if (onClearRefine) onClearRefine(); };
    addAndMakeVisible (clearButton);
    matchButton.onClick = [this] { if (onMatch) onMatch (matchButton.getToggleState()); };
    addChildComponent (refineButton); // kept for sessions that used it; its gain proved marginal
    addAndMakeVisible (matchButton);
}

void CheckView::update (const CheckData& d, const juce::String& name, bool isPlaying, bool isBypassed, bool matching)
{
    data = d; focusName = name; playing = isPlaying; bypassed = isBypassed;
    matchButton.setToggleState (matching, juce::dontSendNotification);
    refineButton.setEnabled (d.refinePass == 0);
    clearButton.setEnabled (d.refined && d.refinePass == 0);
    refineButton.setButtonText (d.refinePass > 0 ? "Refining: pass " + juce::String (d.refinePass) + " of " + juce::String (d.refinePasses) : juce::String ("Refine phase on the sum"));
    repaint();
}

void CheckView::resized()
{
    auto top = getLocalBounds().reduced (12).removeFromTop (28);
    allButton.setBounds (top.removeFromLeft (150));
    focusButton.setBounds (top.removeFromLeft (270));
    matchButton.setBounds (top.removeFromRight (170));
    top.removeFromRight (8);
    clearButton.setBounds (top.removeFromRight (100));
    top.removeFromRight (6);
    refineButton.setBounds (top.removeFromRight (190));
}

static juce::String signedDb (double v) { return (v >= 0.0 ? "+" : "") + juce::String (v, 1) + " dB"; }

void CheckView::paint (juce::Graphics& g)
{
    g.fillAll (ui::bg);
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (36);

    // Verdict.
    auto verdict = area.removeFromTop (108);
    g.setFont (juce::FontOptions (13.0f));
    g.setColour (ui::dim);
    g.drawText (focusOnSelected() ? "The main array and " + focusName + ", added up on the stereo bus (left and right together)."
                                  : "Every microphone, added up on the stereo bus (left and right together).",
                verdict.removeFromTop (18), juce::Justification::left);

    if (! data.valid)
    {
        g.setColour (ui::warn);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (playing ? "Listening..." : "Press play: the sum is measured on the music as it runs.", verdict.removeFromTop (26), juce::Justification::left);
    }
    else
    {
        const double v = data.gainAll;
        const auto colour = bypassed ? ui::dim : v > 0.3 ? ui::good : v < -0.3 ? ui::bad : ui::text;
        g.setColour (colour);
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawText (bypassed ? "Bypass all is on: nothing is being corrected."
                             : v > 0.3 ? "The corrections help: the sum is " + signedDb (v) + " fuller."
                             : v < -0.3 ? "The corrections hurt: the sum is " + signedDb (v) + " thinner."
                             : "The corrections change little: " + signedDb (v) + ".",
                    verdict.removeFromTop (28), juce::Justification::left);
        g.setFont (juce::FontOptions (14.0f));
        g.setColour (ui::text);
        juce::String detail = "Lows (below 250 Hz) " + signedDb (data.gainLow) + "      Mids " + signedDb (data.gainMid)
                              + "      Highs (above 2 kHz) " + signedDb (data.gainHigh);
        if (focusOnSelected() && data.hasCorrelation)
            detail << "      Correlation with the main array: " << juce::String (data.correlationBefore, 2) << " -> " << juce::String (data.correlationAfter, 2);
        g.drawText (detail, verdict.removeFromTop (22), juce::Justification::left);
    }
    g.setColour (ui::dim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Grey: as recorded.  Blue: with the corrections.  Green: where they add, red: where they cancel.",
                verdict.removeFromTop (16), juce::Justification::left);
    g.drawText ("Match level: equal loudness with and without Bypass all. Levels are those set in the Mix view (Level in mix), else as recorded.",
                verdict.removeFromTop (16), juce::Justification::left);

    // Curves.
    auto plot = area.reduced (0, 6).toFloat();
    plot.removeFromLeft (38.0f);
    plot.removeFromBottom (20.0f);
    g.setColour (ui::panel);
    g.fillRect (plot);

    float top = -200.0f;
    if (data.valid)
        for (int b = 0; b < CheckData::numBands; ++b) top = juce::jmax (top, data.before[b], data.after[b]);
    top = data.valid ? std::ceil (top / 6.0f) * 6.0f + 6.0f : 0.0f;
    const float range = 48.0f;
    const auto xOf = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / CheckData::low) / std::log (CheckData::high / CheckData::low)); };
    const auto yOf = [&] (float db) { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, (top - db) / range); };

    g.setFont (juce::FontOptions (11.0f));
    for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        g.setColour (ui::line.withAlpha (0.7f));
        g.drawVerticalLine ((int) xOf (f), plot.getY(), plot.getBottom());
        g.setColour (ui::dim);
        g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 0) + " k" : juce::String (f, 0), (int) xOf (f) - 20, (int) plot.getBottom() + 3, 40, 14, juce::Justification::centred);
    }
    for (float db = 0.0f; db <= range; db += 12.0f)
    {
        const float y = plot.getY() + plot.getHeight() * db / range;
        g.setColour (ui::line.withAlpha (0.7f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (ui::dim);
        g.drawText (juce::String ((int) -db), (int) plot.getX() - 38, (int) y - 7, 32, 14, juce::Justification::right);
    }
    if (! data.valid)
        return;

    const double span = std::log (CheckData::high / CheckData::low);
    const auto fOf = [&] (int b) { return CheckData::low * std::exp (span * (b + 0.5) / CheckData::numBands); };
    juce::Path before, after;
    for (int b = 0; b < CheckData::numBands; ++b)
    {
        const float x = xOf (fOf (b)), yb = yOf (data.before[b]), ya = yOf (data.after[b]);
        if (b == 0) { before.startNewSubPath (x, yb); after.startNewSubPath (x, ya); }
        else        { before.lineTo (x, yb); after.lineTo (x, ya); }
        if (b + 1 < CheckData::numBands) // shade the difference
        {
            const float x2 = xOf (fOf (b + 1));
            juce::Path quad;
            quad.startNewSubPath (x, yb); quad.lineTo (x2, yOf (data.before[b + 1]));
            quad.lineTo (x2, yOf (data.after[b + 1])); quad.lineTo (x, ya); quad.closeSubPath();
            g.setColour ((data.after[b] + data.after[b + 1] >= data.before[b] + data.before[b + 1] ? ui::good : ui::bad).withAlpha (0.28f));
            g.fillPath (quad);
        }
    }
    g.setColour (ui::dim);
    g.strokePath (before, juce::PathStrokeType (1.5f));
    g.setColour (ui::accent);
    g.strokePath (after, juce::PathStrokeType (2.0f));
}
