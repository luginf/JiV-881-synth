/*
  ==============================================================================

    LCDisplay.h
    Created: 18 Aug 2024 12:13:42am
    Author:  Giulio Zausa

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../PluginProcessor.h"

//==============================================================================
/*
*/
class LCDisplay  : public juce::Component
{
public:
    enum struct Color: uint8_t
    {
        Green       = 0U,
        Amber       = 1U,
        Red         = 2U,
        Blue        = 3U,
        WhiteBlack  = 4U,
        WhiteBlue   = 5U,
        BlackWhite  = 6U,
        BlackAmber  = 7U,
        BlackRed    = 8U,
        BlackGreen  = 9U,
        BlackBlue   = 10U,
        BlackVFD    = 11U,
        GreenHighContrast = 12U,
    };

    LCDisplay(VirtualJVProcessor&);
    ~LCDisplay() override;

    void paint (juce::Graphics&) override;
    void resized() override {}
    void mouseDown(const juce::MouseEvent & /* event */) override;
    void setLCDColor(const Color color);

    // Extracted from mouseDown()'s own right-click handling (Alan's request, 2026-09-08) so
    // PanelSkin's embedded live LCD (Panel Compact/Full display mode - see PanelSkin.h) can pop
    // the exact same colour picker on a right-click over the photo's own LCD opening, instead of
    // only being reachable through this always-820x100 standalone component.
    // Right-click on the LCD: runs onContextMenu when the owner set one (VirtualJVEditor shows
    // its app-wide menu, which holds the colours under "LCD"), else pops up the colour list alone.
    void showColorMenu();

    // Adds an "LCD" submenu with the colour choices to `menu`.
    void addColorSubmenu(juce::PopupMenu &menu);

    std::function<void()> onContextMenu;

private:
    class RedrawTimer : public juce::Timer
    {
    public:
        RedrawTimer(LCDisplay* parent) : parent(parent) {}

        void timerCallback() override { parent->repaint(); }

    private:
      LCDisplay* parent;
    };

    RedrawTimer redrawTimer;
    Color lcdColor = Color::Green;
    juce::Image lcdImage; // owned/reused across paint() calls (Alan's report, 2026-09-08: DSP
                           // Load regression) instead of allocating a fresh 820x100 ARGB image
                           // from scratch 25x/second.

    VirtualJVProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LCDisplay)
};
