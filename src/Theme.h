#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Scene.h"

namespace ui
{
const juce::Colour bg { 0xff14171c }, panel { 0xff1c2128 }, panel2 { 0xff262d37 }, line { 0xff333c49 },
                   text { 0xffe6e9ee }, dim { 0xff8b95a5 }, accent { 0xff4fb3ff },
                   good { 0xff6fd08c }, warn { 0xffffa94d }, bad { 0xffe36a6a };

inline juce::Colour groupColour (int group)
{
    static const juce::uint32 c[arraylab::kMaxGroups] = { 0xff4fb3ff, 0xffffa94d, 0xff6fd08c, 0xffd58cff,
                                                          0xffff7b9c, 0xffe8d35a, 0xff5fd6c8, 0xffb0b8c4 };
    return juce::Colour (c[juce::jlimit (0, arraylab::kMaxGroups - 1, group)]);
}

inline juce::StringArray roleNames()    { return { "Spot", "Main L", "Main R", "Main C", "Main pair (stereo track)", "Outrigger", "Ambience" }; }
inline juce::StringArray patternNames() { return { "Omni", "Wide cardioid", "Cardioid", "Supercardioid", "Hypercardioid", "Figure-8" }; }
inline juce::StringArray arrayNames()   { return { "AB", "ORTF", "NOS", "XY", "Blumlein", "Decca Tree", "Custom" }; }
}
