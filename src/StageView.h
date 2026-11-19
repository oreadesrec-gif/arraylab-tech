#pragma once
#include "Hub.h"
#include "Theme.h"

/** The plan: main arrays and microphones seen from above. Click to select,
    drag to move, drag the small handle to turn, wheel to zoom, drag the floor to scroll. */
class StageView : public juce::Component
{
public:
    explicit StageView (arraylab::Hub&);

    void update (const juce::Array<arraylab::MicInfo>&, const arraylab::Scene&, int selectedSlot, int groupFilter);
    std::function<void (int slot)> onSelect;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum class Kind { mic, micHandle, array, arrayHandle };
    struct Hit { juce::Point<float> at; Kind kind; int slot; int group; };
    enum class Drag { none, mic, micAngle, array, arrayAngle, floor };

    juce::Point<float> toScreen (arraylab::Vec2) const;
    arraylab::Vec2 toWorld (juce::Point<float>) const;
    int arrayGroupOf (int group) const;
    void drawMic (juce::Graphics&, juce::Point<float>, double axisDeg, arraylab::Pattern, juce::Colour,
                  const juce::String& label, const juce::String& sub, bool selected, bool handle);
    void drawLabel (juce::Graphics&, juce::Point<float>, const juce::String& label, const juce::String& sub, bool selected);

    arraylab::Hub& hub;
    juce::Array<arraylab::MicInfo> mics;
    arraylab::Scene scene;
    int selected = -1, filter = -1;
    std::vector<Hit> hits;

    double scale = 42.0;             // pixels per metre
    arraylab::Vec2 centre { 0.0, 4.0 };
    Drag drag = Drag::none;
    int dragSlot = -1, dragGroup = 0;
    arraylab::Vec2 dragOffset, dragCentre;
};
