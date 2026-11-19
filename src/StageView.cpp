#include "StageView.h"

using namespace arraylab;

StageView::StageView (Hub& h) : hub (h) {}

void StageView::update (const juce::Array<MicInfo>& m, const Scene& s, int sel, int groupFilter)
{
    mics = m; scene = s; selected = sel; filter = groupFilter;
    repaint();
}

juce::Point<float> StageView::toScreen (Vec2 p) const
{
    return { (float) (getWidth() * 0.5 + (p.x - centre.x) * scale), (float) (getHeight() * 0.5 - (p.y - centre.y) * scale) };
}

Vec2 StageView::toWorld (juce::Point<float> p) const
{
    return { centre.x + (p.x - getWidth() * 0.5) / scale, centre.y - (p.y - getHeight() * 0.5) / scale };
}

int StageView::arrayGroupOf (int group) const
{
    for (auto& m : mics)
        if (m.state.group == group && isMain (m.state.role))
            return group;
    return 0;
}

void StageView::drawMic (juce::Graphics& g, juce::Point<float> p, double axisDeg, Pattern pattern, juce::Colour colour,
                         const juce::String& label, const juce::String& sub, bool isSelected, bool handle)
{
    const float lobe = 30.0f;
    juce::Path shape;
    for (int a = 0; a <= 360; a += 6)
    {
        const float r = lobe * (float) std::abs (patternGain (pattern, a * kPi / 180.0));
        const auto d = direction (axisDeg + a);
        const juce::Point<float> q { p.x + r * (float) d.x, p.y - r * (float) d.y };
        if (a == 0) shape.startNewSubPath (q); else shape.lineTo (q);
    }
    shape.closeSubPath();
    g.setColour (colour.withAlpha (isSelected ? 0.30f : 0.14f));
    g.fillPath (shape);
    g.setColour (colour.withAlpha (isSelected ? 0.9f : 0.45f));
    g.strokePath (shape, juce::PathStrokeType (1.0f));

    const auto d = direction (axisDeg);
    const juce::Point<float> tip { p.x + (lobe + 8.0f) * (float) d.x, p.y - (lobe + 8.0f) * (float) d.y };
    g.drawLine ({ p, tip }, 1.0f);
    if (handle)
    {
        g.setColour (isSelected ? ui::text : colour);
        g.fillEllipse (tip.x - 3.5f, tip.y - 3.5f, 7.0f, 7.0f);
    }

    g.setColour (colour);
    g.fillEllipse (p.x - 7.0f, p.y - 7.0f, 14.0f, 14.0f);
    if (isSelected)
    {
        g.setColour (ui::text);
        g.drawEllipse (p.x - 10.0f, p.y - 10.0f, 20.0f, 20.0f, 1.6f);
    }

    drawLabel (g, p, label, sub, isSelected);
}

void StageView::drawLabel (juce::Graphics& g, juce::Point<float> p, const juce::String& label, const juce::String& sub, bool isSelected)
{
    g.setColour (isSelected ? ui::text : ui::text.withAlpha (0.85f));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (label, juce::Rectangle<float> (p.x - 80.0f, p.y + 11.0f, 160.0f, 16.0f), juce::Justification::centred, false);
    if (sub.isNotEmpty())
    {
        g.setColour (ui::dim);
        g.setFont (juce::FontOptions (11.5f));
        g.drawText (sub, juce::Rectangle<float> (p.x - 90.0f, p.y + 26.0f, 180.0f, 14.0f), juce::Justification::centred, false);
    }
}

void StageView::paint (juce::Graphics& g)
{
    g.fillAll (ui::bg);
    hits.clear();

    // Floor grid, one line per metre (or 5 m / 0.5 m depending on zoom).
    const double step = scale > 70.0 ? 0.5 : scale > 16.0 ? 1.0 : 5.0;
    const int majorEvery = step < 1.0 ? 2 : step > 1.0 ? 2 : 5;
    const Vec2 tl = toWorld ({ 0.0f, 0.0f }), br = toWorld ({ (float) getWidth(), (float) getHeight() });
    g.setFont (juce::FontOptions (11.0f));
    for (auto k = (juce::int64) std::floor (tl.x / step); k * step <= br.x; ++k)
    {
        const bool major = k % majorEvery == 0;
        const float x = toScreen ({ k * step, 0.0 }).x;
        g.setColour (ui::line.withAlpha (major ? 0.9f : 0.35f));
        g.drawVerticalLine ((int) x, 0.0f, (float) getHeight());
        if (major) { g.setColour (ui::dim); g.drawText (juce::String (k * step, step < 1.0 ? 1 : 0) + " m", (int) x + 3, getHeight() - 16, 60, 14, juce::Justification::left); }
    }
    for (auto k = (juce::int64) std::floor (br.y / step); k * step <= tl.y; ++k)
    {
        const bool major = k % majorEvery == 0;
        const float y = toScreen ({ 0.0, k * step }).y;
        g.setColour (ui::line.withAlpha (major ? 0.9f : 0.35f));
        g.drawHorizontalLine ((int) y, 0.0f, (float) getWidth());
        if (major) { g.setColour (ui::dim); g.drawText (juce::String (k * step, step < 1.0 ? 1 : 0) + " m", 4, (int) y - 15, 60, 14, juce::Justification::left); }
    }

    // Which main microphones exist in each group.
    bool present[kMaxGroups][3] {};
    int owner[kMaxGroups][3];
    for (auto& row : owner) for (auto& o : row) o = -1;
    for (auto& m : mics)
    {
        const int gr = m.state.group;
        const auto mark = [&] (int i) { present[gr][i] = true; owner[gr][i] = m.index; };
        switch (m.state.role)
        {
            case Role::mainPair: mark (0); mark (2); break;
            case Role::mainL:    mark (0); break;
            case Role::mainC:    mark (1); break;
            case Role::mainR:    mark (2); break;
            case Role::spot: case Role::outrigger: case Role::ambience: break;
        }
    }

    // Lines from the selected microphone to its main pair, with distances.
    for (auto& m : mics)
        if (m.index == selected && ! isMain (m.state.role))
        {
            const int ag = arrayGroupOf (m.state.group);
            const auto geo = arrayGeometry (scene.arrays[ag]);
            const auto p = toScreen ({ m.state.x, m.state.y });
            for (int i : { 0, 2 })
            {
                if (! present[ag][i]) continue;
                const auto q = toScreen (geo.pos[i]);
                const float dashes[] = { 5.0f, 4.0f };
                g.setColour (ui::dim.withAlpha (0.7f));
                g.drawDashedLine ({ p, q }, dashes, 2, 1.0f);
                const double dist = std::hypot (distance ({ m.state.x, m.state.y }, geo.pos[i]), scene.arrays[ag].height - m.state.z);
                const auto mid = (p + q) * 0.5f + juce::Point<float> (i == 0 ? -34.0f : 34.0f, 0.0f);
                g.setFont (juce::FontOptions (11.5f));
                g.drawText (juce::String (dist, 2) + " m", juce::Rectangle<float> (mid.x - 34.0f, mid.y - 7.0f, 68.0f, 14.0f), juce::Justification::centred, false);
            }
        }

    // Main arrays.
    for (int gr = 0; gr < kMaxGroups; ++gr)
    {
        if (! (present[gr][0] || present[gr][1] || present[gr][2]) || (filter >= 0 && filter != gr))
            continue;
        const auto& arr = scene.arrays[gr];
        const auto geo = arrayGeometry (arr);
        const auto colour = ui::groupColour (gr);
        const auto c = toScreen ({ arr.x, arr.y });

        if (present[gr][0] && present[gr][2])
        {
            g.setColour (colour.withAlpha (0.6f));
            g.drawLine ({ toScreen (geo.pos[0]), toScreen (geo.pos[2]) }, 1.5f);
        }
        static const char* tags[3] = { "L", "C", "R" };
        bool arraySelected = false;
        for (int i = 0; i < 3; ++i)
        {
            if (! present[gr][i]) continue;
            const bool sel = owner[gr][i] == selected;
            arraySelected |= sel;
            auto p = toScreen (geo.pos[i]);
            if (arr.spacingCm < 1.0 && i != 1) p.x += i == 0 ? -4.0f : 4.0f; // keep a coincident pair readable
            drawMic (g, p, geo.axisDeg[i], arr.pattern, colour, tags[i], {}, sel, false);
            hits.push_back ({ p, Kind::array, owner[gr][i], gr });
        }
        const juce::Point<float> handle { c.x + 58.0f * (float) geo.forward.x, c.y - 58.0f * (float) geo.forward.y };
        g.setColour (colour.withAlpha (arraySelected ? 1.0f : 0.5f));
        g.drawLine ({ c, handle }, 1.0f);
        g.fillEllipse (handle.x - 4.0f, handle.y - 4.0f, 8.0f, 8.0f);
        hits.push_back ({ handle, Kind::arrayHandle, -1, gr });
        g.setFont (juce::FontOptions (11.5f));
        g.setColour (ui::dim);
        g.drawText (scene.groupNames[gr] + "  " + ui::arrayNames()[(int) arr.type] + "  " + juce::String (arr.spacingCm, 0) + " cm / "
                        + juce::String (arr.angleDeg, 0) + juce::String::fromUTF8 ("\xc2\xb0"),
                    juce::Rectangle<float> (c.x - 120.0f, c.y + 30.0f, 240.0f, 14.0f), juce::Justification::centred, false);
    }

    // Spots, outriggers and ambience microphones.
    for (auto& m : mics)
    {
        if (isMain (m.state.role) || (filter >= 0 && filter != m.state.group))
            continue;
        const auto p = toScreen ({ m.state.x, m.state.y });
        juce::String sub;
        const auto& a = m.applied;
        if (a.delayLms > 0.005 || a.delayRms > 0.005)
            sub = std::abs (a.delayLms - a.delayRms) > 0.02 ? "L " + juce::String (a.delayLms, 2) + " / R " + juce::String (a.delayRms, 2) + " ms"
                                                            : juce::String (a.delayLms, 2) + " ms";
        if (a.polarity < 0) sub << juce::String::fromUTF8 ("  \xc3\x98");
        if (a.phaseTaps > 0) sub << juce::String::fromUTF8 ("  \xcf\x86");
        auto colour = ui::groupColour (m.state.group);
        if (m.state.role == Role::ambience)  colour = colour.withSaturation (0.35f);
        if (m.isStereoPair())
        {
            // Two capsules either side of the pair's centre, splayed around its aim.
            ArraySettings pairShape;
            pairShape.x = m.state.x; pairShape.y = m.state.y; pairShape.rotationDeg = m.state.angleDeg;
            pairShape.spacingCm = m.state.pairSpacingCm; pairShape.angleDeg = m.state.pairAngleDeg;
            const auto pg = arrayGeometry (pairShape);
            for (int i : { 0, 2 })
            {
                auto q = toScreen (pg.pos[i]);
                const float nudge = juce::jmax (0.0f, 5.0f - q.getDistanceFrom (p)); // keep a coincident pair readable
                q += juce::Point<float> ((float) pg.right.x, (float) -pg.right.y) * (i == 0 ? -nudge : nudge);
                drawMic (g, q, pg.axisDeg[i], m.state.pattern, colour, {}, {}, m.index == selected, false);
            }
            g.setColour (colour.withAlpha (0.7f));
            g.drawLine ({ toScreen (pg.pos[0]), toScreen (pg.pos[2]) }, 1.5f);
            const auto dd = direction (m.state.angleDeg);
            const juce::Point<float> tip { p.x + 38.0f * (float) dd.x, p.y - 38.0f * (float) dd.y };
            g.drawLine ({ p, tip }, 1.0f);
            g.setColour (m.index == selected ? ui::text : colour);
            g.fillEllipse (tip.x - 3.5f, tip.y - 3.5f, 7.0f, 7.0f);
            drawLabel (g, p + juce::Point<float> (0.0f, 14.0f), m.state.name + "  (pair)", sub, m.index == selected);
        }
        else
            drawMic (g, p, m.state.angleDeg, m.state.pattern, colour, m.state.name, sub, m.index == selected, true);
        const auto d = direction (m.state.angleDeg);
        hits.push_back ({ { p.x + 38.0f * (float) d.x, p.y - 38.0f * (float) d.y }, Kind::micHandle, m.index, m.state.group });
        hits.push_back ({ p, Kind::mic, m.index, m.state.group });
    }

    g.setColour (ui::dim);
    g.setFont (juce::FontOptions (11.5f));
    g.drawText ("Top view. Drag a mic to move it, drag its small dot to turn it, wheel to zoom, double-click the floor to re-centre.",
                getLocalBounds().reduced (8, 4).removeFromTop (16), juce::Justification::right, false);
}

void StageView::mouseDown (const juce::MouseEvent& e)
{
    drag = Drag::floor;
    dragCentre = centre;
    const Hit* best = nullptr;
    float bestDist = 12.0f;
    for (auto& h : hits)
    {
        const float d = h.at.getDistanceFrom (e.position) - ((h.kind == Kind::micHandle || h.kind == Kind::arrayHandle) ? 3.0f : 0.0f);
        if (d < bestDist) { bestDist = d; best = &h; }
    }
    if (best == nullptr)
        return;

    dragSlot = best->slot;
    dragGroup = best->group;
    const auto w = toWorld (e.position);
    switch (best->kind)
    {
        case Kind::mic:
        {
            drag = Drag::mic;
            const auto m = hub.getMic (dragSlot);
            dragOffset = { m.x - w.x, m.y - w.y };
            break;
        }
        case Kind::micHandle:   drag = Drag::micAngle; break;
        case Kind::array:       drag = Drag::array; dragOffset = { scene.arrays[dragGroup].x - w.x, scene.arrays[dragGroup].y - w.y }; break;
        case Kind::arrayHandle: drag = Drag::arrayAngle; break;
    }
    if (dragSlot >= 0 && onSelect)
        onSelect (dragSlot);
}

void StageView::mouseDrag (const juce::MouseEvent& e)
{
    const auto w = toWorld (e.position);
    const auto snap = [] (double v) { return std::round (v * 20.0) / 20.0; }; // 5 cm
    switch (drag)
    {
        case Drag::floor:
            centre = { dragCentre.x - e.getDistanceFromDragStartX() / scale, dragCentre.y + e.getDistanceFromDragStartY() / scale };
            repaint();
            break;
        case Drag::mic:
        {
            auto m = hub.getMic (dragSlot);
            m.x = snap (w.x + dragOffset.x);
            m.y = snap (w.y + dragOffset.y);
            m.manualPosition = true;
            hub.setMic (dragSlot, m);
            break;
        }
        case Drag::micAngle:
        {
            auto m = hub.getMic (dragSlot);
            m.angleDeg = std::round (std::atan2 (w.x - m.x, w.y - m.y) * 180.0 / kPi);
            hub.setMic (dragSlot, m);
            break;
        }
        case Drag::array:
        {
            auto sc = hub.getScene();
            sc.arrays[dragGroup].x = snap (w.x + dragOffset.x);
            sc.arrays[dragGroup].y = snap (w.y + dragOffset.y);
            hub.setScene (sc);
            break;
        }
        case Drag::arrayAngle:
        {
            auto sc = hub.getScene();
            auto& a = sc.arrays[dragGroup];
            a.rotationDeg = std::round (std::atan2 (w.x - a.x, w.y - a.y) * 180.0 / kPi);
            hub.setScene (sc);
            break;
        }
        case Drag::none: break;
    }
}

void StageView::mouseUp (const juce::MouseEvent&) { drag = Drag::none; }

void StageView::mouseDoubleClick (const juce::MouseEvent& e)
{
    for (auto& h : hits)
        if (h.at.getDistanceFrom (e.position) < 12.0f)
            return;
    scale = 42.0;
    centre = { 0.0, 4.0 };
    repaint();
}

void StageView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto before = toWorld (e.position);
    scale = juce::jlimit (6.0, 240.0, scale * std::pow (1.25, (double) wheel.deltaY * 4.0));
    const auto after = toWorld (e.position);
    centre = { centre.x + before.x - after.x, centre.y + before.y - after.y };
    repaint();
}
