/*
  ==============================================================================

    InterfaceTab.cpp

  ==============================================================================
*/

#include "InterfaceTab.h"

//==============================================================================
InterfaceTab::InterfaceTab(VirtualJVProcessor &p) : skin(p, PanelSkin::Variant::kCommands)
{
    addAndMakeVisible(skin);
}

void InterfaceTab::resized()
{
    auto area = getLocalBounds().reduced(20).withSizeKeepingCentre(juce::jmin(getWidth() - 40, 780), getHeight() - 40);
    const int skinW = area.getWidth();
    const int skinH = (int)skin.heightForWidth((float)skinW);
    skin.setBounds(area.getX(), area.getY(), skinW, skinH);
}
