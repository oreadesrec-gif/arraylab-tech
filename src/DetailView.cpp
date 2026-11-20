#include "DetailView.h"

using namespace arraylab;

namespace
{
const char* const refNames[kNumRefs] = { "Main L", "Main C", "Main R", "Main sum" };
double wrapPhase (double p) { return p - 2.0 * kPi * std::round (p / (2.0 * kPi)); }
}

DetailView::DetailView (Hub& h) : hub (h)
{
    for (int r = 0; r < kNumRefs; ++r)
    {
        auto& b = refButtons[r];
        b.setButtonText (refNames[r]);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (11);
        b.setColour (juce::TextButton::buttonOnColourId, ui::accent.withAlpha (0.55f));
        b.setConnectedEdges ((r > 0 ? juce::Button::ConnectedOnLeft : 0) | (r < kNumRefs - 1 ? juce::Button::ConnectedOnRight : 0));
        b.onClick = [this, r] { chooseRef (r); };
        addAndMakeVisible (b);
    }
    autoButton.onClick = [this]
    {
        if (! hasMic) return;
        auto m = hub.getMic (mic.index);
        m.peakLocked[ref] = false;
        hub.setMic (mic.index, m);
    };
    rangeButton.setClickingTogglesState (true);
    rangeButton.onClick = [this] { wholeRange = rangeButton.getToggleState(); repaint(); };
    addAndMakeVisible (autoButton);
    addAndMakeVisible (rangeButton);
}

void DetailView::chooseRef (int r)
{
    ref = r;
    followApplied = false;
    repaint();
}

void DetailView::update (const MicInfo* info, const Scene& scene, const MicDetail& d)
{
    hasMic = info != nullptr && ! isMain (info->state.role);
    if (hasMic)
    {
        if (info->index != shownSlot) { shownSlot = info->index; followApplied = true; }
        mic = *info;
        if (followApplied && mic.applied.ref >= 0) ref = mic.applied.ref;
    }
    detail = d;
    speed = speedOfSound (scene.tempC);
    for (int r = 0; r < kNumRefs; ++r)
    {
        refButtons[r].setToggleState (r == ref, juce::dontSendNotification);
        refButtons[r].setEnabled (hasMic && mic.results.vs[r].valid);
    }
    autoButton.setEnabled (hasMic && mic.state.peakLocked[ref]);
    repaint();
}

void DetailView::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto top = area.removeFromTop (28);
    for (auto& b : refButtons) b.setBounds (top.removeFromLeft (92));
    top.removeFromLeft (16);
    autoButton.setBounds (top.removeFromLeft (150));
    rangeButton.setBounds (top.removeFromRight (110));
    area.removeFromTop (58);
    correlationArea = area.removeFromTop (area.getHeight() * 45 / 100).toFloat().withTrimmedLeft (44.0f).withTrimmedBottom (22.0f);
    area.removeFromTop (46);
    phaseArea = area.toFloat().withTrimmedLeft (44.0f).withTrimmedBottom (34.0f);
}

void DetailView::mouseDown (const juce::MouseEvent& e)
{
    if (! hasMic) return;
    for (auto& mk : markers)
        if (mk.at.getDistanceFrom (e.position) < 14.0f)
        {
            auto m = hub.getMic (mic.index);
            m.peakLocked[ref] = true;
            m.peakLag[ref] = mk.lag;
            hub.setMic (mic.index, m);
            return;
        }
}

void DetailView::paint (juce::Graphics& g)
{
    g.fillAll (ui::bg);
    markers.clear();
    auto textArea = getLocalBounds().reduced (12);
    textArea.removeFromTop (34);

    if (! hasMic)
    {
        g.setColour (ui::warn);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("Select a spot, outrigger or ambience microphone on the Stage or Align view.", textArea.removeFromTop (26), juce::Justification::left);
        return;
    }

    const double sr = mic.sampleRate;
    const auto& res = mic.results.vs[ref];
    const auto used = effectiveResult (mic.results, mic.state, ref, mic.isStereoPair());
    const auto ms = [&] (double samples) { return 1000.0 * samples / sr; };

    // Headline.
    g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    juce::String headline;
    if (! res.valid)            { g.setColour (ui::warn); headline = mic.state.name + ": nothing measured against " + refNames[ref] + " yet. Press Analyze, then play."; }
    else if (used.locked)       { g.setColour (ui::accent); headline = mic.state.name + ": you picked " + juce::String (std::abs (ms (used.delaySamples)), 3) + " ms " + (used.delaySamples <= 0 ? "before " : "after ") + refNames[ref] + "."; }
    else if (res.reliable)      { g.setColour (ui::good); headline = mic.state.name + " is " + juce::String (std::abs (ms (used.delaySamples)), 3) + " ms " + (used.delaySamples <= 0 ? "before " : "after ") + refNames[ref] + ". The peak is clear (" + juce::String (juce::roundToInt (res.snr)) + " times the floor)."; }
    else                        { g.setColour (ui::bad); headline = mic.state.name + ": no clear peak against " + refNames[ref] + " (" + juce::String (res.snr, 1) + " times the floor, 8 needed). Nothing is applied."; }
    g.drawText (headline, textArea.removeFromTop (24), juce::Justification::left);
    g.setColour (ui::dim);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText ("Top: how well the two microphones match at each delay. Each dot is a candidate; the ringed one is in use. Click another dot to use it instead."
                + juce::String (mic.applied.ref == ref ? "" : "   (This reference is not the one in use for this mic.)"),
                textArea.removeFromTop (18), juce::Justification::left);

    //==========================================================================
    // Correlation.
    auto plot = correlationArea;
    g.setColour (ui::panel);
    g.fillRect (plot);
    if (res.valid)
    {
        double lo = 0.0, hi = 0.0;
        if (detail.available && detail.slot == mic.index && detail.ref == ref) { lo = detail.curve.minLag; hi = detail.curve.maxLag; }
        else { lo = hi = res.peaks[0].lag; }
        double viewLo = lo, viewHi = hi;
        if (! wholeRange || hi <= lo)
        {
            viewLo = viewHi = used.delaySamples;
            for (int p = 0; p < res.numPeaks; ++p) { viewLo = juce::jmin (viewLo, res.peaks[p].lag); viewHi = juce::jmax (viewHi, res.peaks[p].lag); }
            const double pad = juce::jmax (0.003 * sr, 0.25 * (viewHi - viewLo));
            viewLo -= pad; viewHi += pad;
        }
        float top = 0.05f;
        for (int p = 0; p < res.numPeaks; ++p) top = juce::jmax (top, std::abs (res.peaks[p].value));
        top *= 1.15f;
        const auto xOf = [&] (double lag) { return plot.getX() + plot.getWidth() * (float) ((lag - viewLo) / (viewHi - viewLo)); };
        const auto yOf = [&] (float v) { return plot.getCentreY() - plot.getHeight() * 0.5f * juce::jlimit (-1.0f, 1.0f, v / top); };

        g.setColour (ui::line);
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
        g.setFont (juce::FontOptions (11.0f));
        const double spanMs = ms (viewHi - viewLo);
        const double tick = spanMs > 80 ? 20.0 : spanMs > 30 ? 10.0 : spanMs > 12 ? 5.0 : spanMs > 5 ? 2.0 : 1.0;
        for (double t = std::ceil (ms (viewLo) / tick) * tick; t <= ms (viewHi); t += tick)
        {
            const float x = xOf (t * sr / 1000.0);
            g.setColour (ui::line.withAlpha (0.7f));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (ui::dim);
            g.drawText (juce::String (t, 0) + " ms", (int) x - 30, (int) plot.getBottom() + 3, 60, 14, juce::Justification::centred);
        }
        g.setColour (ui::dim);
        g.drawText ("same", (int) plot.getX() - 44, (int) plot.getY() - 2, 40, 14, juce::Justification::right);
        g.drawText ("inverted", (int) plot.getX() - 48, (int) plot.getBottom() - 12, 44, 14, juce::Justification::right);

        if (detail.available && detail.slot == mic.index && detail.ref == ref && detail.curve.points.size() > 1)
        {
            juce::Path path;
            const auto& pts = detail.curve.points;
            bool started = false;
            for (size_t i = 0; i < pts.size(); ++i)
            {
                const double lag = detail.curve.minLag + (detail.curve.maxLag - detail.curve.minLag) * (double) i / (double) (pts.size() - 1);
                if (lag < viewLo || lag > viewHi) continue;
                const juce::Point<float> q (xOf (lag), yOf (pts[i]));
                if (! started) { path.startNewSubPath (q); started = true; } else path.lineTo (q);
            }
            g.setColour (ui::text.withAlpha (0.55f));
            g.strokePath (path, juce::PathStrokeType (1.2f));
        }
        else
        {
            g.setColour (ui::dim);
            g.drawText ("The full curve is only kept while the session is open: press Analyze again to see it. The candidates below were saved.",
                        plot.reduced (8.0f).removeFromTop (16.0f), juce::Justification::left, false);
        }

        for (int p = 0; p < res.numPeaks; ++p)
        {
            const auto& pk = res.peaks[p];
            if (pk.lag < viewLo || pk.lag > viewHi) continue;
            const juce::Point<float> q (xOf (pk.lag), yOf (pk.value));
            const bool inUse = used.locked ? std::abs (pk.lag - (res.delaySamples + used.shift)) < 1.5 : p == 0;
            g.setColour (ui::line);
            g.drawLine (q.x, plot.getCentreY(), q.x, q.y, 1.0f);
            g.setColour (inUse ? ui::accent : ui::text);
            g.fillEllipse (q.x - 5.0f, q.y - 5.0f, 10.0f, 10.0f);
            if (inUse) { g.setColour (ui::text); g.drawEllipse (q.x - 8.5f, q.y - 8.5f, 17.0f, 17.0f, 1.8f); }
            g.setColour (inUse ? ui::text : ui::dim);
            g.setFont (juce::FontOptions (11.5f));
            g.drawText (juce::String (ms (pk.lag), 2) + " ms  " + juce::String (std::abs (speed * pk.lag / sr), 2) + " m",
                        juce::Rectangle<float> (q.x - 70.0f, pk.value >= 0 ? q.y - 26.0f : q.y + 11.0f, 140.0f, 14.0f), juce::Justification::centred, false);
            markers.push_back ({ q, pk.lag });
        }
    }

    //==========================================================================
    // Phase by frequency.
    auto caption = juce::Rectangle<float> (12.0f, correlationArea.getBottom() + 26.0f, (float) getWidth() - 24.0f, 34.0f);
    g.setColour (ui::dim);
    g.setFont (juce::FontOptions (12.5f));
    juce::String zoneText = "Bottom: phase left once the delay is set, by frequency. Grey: measured. Blue: after the correction. Green strip: how far each band is trusted.";
    g.drawText (zoneText, caption.removeFromTop (16.0f), juce::Justification::left, false);
    if (const auto& z = mic.results.zones[ref]; z.valid)
    {
        const auto part = [] (double v) { return juce::String (std::abs (v), 2) + (v >= 0.0 ? " ms late" : " ms early"); };
        g.setColour (ui::text);
        g.drawText ("Around that delay: lows (below 250 Hz) " + part (z.lowMs) + ",  mids " + part (z.midMs) + ",  highs (above 2 kHz) " + part (z.highMs) + ".",
                    caption.removeFromTop (16.0f), juce::Justification::left, false);
    }

    plot = phaseArea;
    g.setColour (ui::panel);
    g.fillRect (plot);
    const double fLo = bandFrequency (0), fHi = bandFrequency (kFineBands - 1);
    const auto xOf = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / fLo) / std::log (fHi / fLo)); };
    const auto yOf = [&] (double radians) { return plot.getCentreY() - plot.getHeight() * 0.5f * (float) (radians / kPi); };

    g.setFont (juce::FontOptions (11.0f));
    for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        g.setColour (ui::line.withAlpha (0.7f));
        g.drawVerticalLine ((int) xOf (f), plot.getY(), plot.getBottom());
        g.setColour (ui::dim);
        g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 0) + " k" : juce::String (f, 0), (int) xOf (f) - 20, (int) plot.getBottom() + 15, 40, 14, juce::Justification::centred);
    }
    for (int deg = -180; deg <= 180; deg += 90)
    {
        const float y = yOf (deg * kPi / 180.0);
        g.setColour (ui::line.withAlpha (deg == 0 ? 1.0f : 0.6f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (ui::dim);
        g.drawText (juce::String (deg) + juce::String::fromUTF8 ("\xc2\xb0"), (int) plot.getX() - 44, (int) y - 7, 40, 14, juce::Justification::right);
    }

    const auto& curve = mic.results.curve[ref];
    if (! curve.valid)
    {
        g.setColour (ui::dim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("No phase curve yet for this reference: press Analyze, then play.", plot.reduced (10.0f), juce::Justification::centredTop, false);
        return;
    }

    // The range the user lets the correction act in.
    if (mic.state.phaseOn)
    {
        g.setColour (ui::accent.withAlpha (0.07f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xOf (juce::jlimit (fLo, fHi, mic.state.phaseLowHz)), plot.getY(),
                                                                xOf (juce::jlimit (fLo, fHi, mic.state.phaseHighHz)), plot.getBottom()));
    }

    const bool correcting = mic.applied.ref == ref && mic.applied.phaseTaps > 0;
    const double flip = used.polarity != res.polarity ? kPi : 0.0;
    juce::Path before, after;
    bool started = false;
    float lastBefore = 0.0f, lastAfter = 0.0f;
    for (int b = 0; b < kFineBands; ++b)
    {
        const double f = bandFrequency (b);
        if (f > 0.47 * sr) break;
        const float x = xOf (f);
        const double seen = wrapPhase (curve.phase[b] + flip + 2.0 * kPi * f * used.shift / sr);
        const double left = correcting ? wrapPhase (seen - mic.applied.correction[b]) : seen;
        const float yb = yOf (seen), ya = yOf (left);

        // A jump across the +/-180 line is a wrap, not a slope: lift the pen.
        const bool breakBefore = ! started || std::abs (yb - lastBefore) > plot.getHeight() * 0.5f;
        const bool breakAfter = ! started || std::abs (ya - lastAfter) > plot.getHeight() * 0.5f;
        if (breakBefore) before.startNewSubPath (x, yb); else before.lineTo (x, yb);
        if (breakAfter) after.startNewSubPath (x, ya); else after.lineTo (x, ya);
        lastBefore = yb; lastAfter = ya; started = true;

        g.setColour (ui::good.withAlpha (0.15f + 0.75f * curve.trust[b]));
        g.fillRect (x - 2.5f, plot.getBottom() + 3.0f, 5.0f, 7.0f * curve.trust[b] + 1.0f);
    }
    g.setColour (ui::dim);
    g.strokePath (before, juce::PathStrokeType (1.5f));
    if (correcting)
    {
        g.setColour (ui::accent);
        g.strokePath (after, juce::PathStrokeType (2.0f));
    }
    else
    {
        g.setColour (ui::dim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (mic.applied.ref != ref ? "Not the reference in use: shown for comparison only."
                                           : "Phase by frequency is bypassed: tick it in section 4 to see the corrected curve.",
                    plot.reduced (10.0f, 6.0f).removeFromTop (16.0f), juce::Justification::right, false);
    }
}
