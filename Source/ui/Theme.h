#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Dark / light interface theme (Settings > Display > Theme). One global flag, set by
// VirtualJVProcessor before any editor exists and when the user changes it; the colours below
// are what the custom-painted widgets read in paint(). Stock JUCE widgets follow the editor's
// LookAndFeel colour scheme instead (see scheme()).
namespace jivtheme
{
inline bool &lightFlag()
{
  static bool light = false;
  return light;
}
inline bool isLight() { return lightFlag(); }
inline void setLight(bool light) { lightFlag() = light; }

inline juce::LookAndFeel_V4::ColourScheme scheme()
{
  return isLight() ? juce::LookAndFeel_V4::getLightColourScheme() : juce::LookAndFeel_V4::getDarkColourScheme();
}

// Lists (Browse, Performance bank, search results)
inline juce::Colour listBg() { return isLight() ? juce::Colour(0xfff4f6f7) : juce::Colour(0xff263238); }
inline juce::Colour listSelected() { return juce::Colour(0xff42A2C8); }
inline juce::Colour listText(bool selected) { return selected ? juce::Colours::black : (isLight() ? juce::Colour(0xff1d2327) : juce::Colours::white); }
inline juce::Colour listSubText(bool selected) { return selected ? juce::Colours::black : (isLight() ? juce::Colour(0xff1f6f8f) : juce::Colours::lightblue); }
inline juce::Colour listModifiedText() { return isLight() ? juce::Colour(0xffb36b00) : juce::Colour(0xffffb238); }
inline juce::Colour starOn() { return isLight() ? juce::Colour(0xffd4900a) : juce::Colour(0xffffc83d); }
// Faint lines drawn over any background (row dividers, outlines).
inline juce::Colour line(float alpha) { return (isLight() ? juce::Colours::black : juce::Colours::white).withAlpha(alpha); }

// Collapse handles / resize grip
inline juce::Colour handleBg() { return isLight() ? juce::Colour(0xffd5d9dc) : juce::Colour(0xff1a1a1e); }
inline juce::Colour handleText() { return isLight() ? juce::Colour(0xff4a5459) : juce::Colour(0xff8a8a94); }

// Envelope graphs, link toggle
inline juce::Colour plotBg() { return isLight() ? juce::Colour(0xfffafbfb) : juce::Colour(0xff1d2327); }
inline juce::Colour plotBorder() { return isLight() ? juce::Colour(0xffb0b8bc) : juce::Colour(0xff4a5459); }
inline juce::Colour plotTitle() { return isLight() ? juce::Colour(0xff333b3f) : juce::Colours::lightgrey; }
inline juce::Colour mutedText() { return isLight() ? juce::Colour(0xff555d61) : juce::Colours::lightgrey; }

// Idle fill of small text buttons that are coloured by hand
inline juce::Colour buttonIdle() { return isLight() ? juce::Colour(0xffdddddd) : juce::Colour(0xff323e44); }

inline juce::Colour okText() { return isLight() ? juce::Colour(0xff1b7a2b) : juce::Colours::lightgreen; }
inline juce::Colour warnText() { return isLight() ? juce::Colour(0xffb35900) : juce::Colours::orange; }
} // namespace jivtheme
