/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"

#include "sequencer/JivSequencerPanel.h"
#include "sequencer/JivSequencerGridPanel.h"
#include "sequencer/JivSequencerRetroPanel.h"
#include "ui/widgets/CollapseHandle.h"
#include "ui/widgets/LCDisplay.h"
#include "ui/widgets/TabBar.h"
#include "ui/widgets/VirtualKeyboard.h"
#include "ui/PanelSkin.h"
#include "ui/PatchBrowser.h"
#include "ui/PerformanceTab.h"
#include "ui/InterfaceTab.h"
#include "ui/EditCommonTab.h"
#include "ui/EditToneTab.h"
#include "ui/EditRhythmTab.h"
#include "ui/SettingsTab.h"

//==============================================================================
/**
*/
class VirtualJVEditor  : public juce::AudioProcessorEditor
{
public:
    VirtualJVEditor (VirtualJVProcessor&);
    ~VirtualJVEditor() override;

    //==============================================================================
    void resized() override;
    void parentHierarchyChanged() override;
    // Standalone only: window/taskbar icon, see the .cpp.
    void applyWindowIcon();

    uint8_t getSelectedRomIdx();
    void updateEditTabs();
    void updatePerformanceTab();
    void showToneOrRhythmEditTabs(const bool isRhythm);

    // Called by VirtualJVProcessor::retryLoadRoms() (Alan's request, 2026-09-08) once ROMs that
    // previously failed to load succeed - swaps the reduced "ROM setup" tab set (see
    // showRomSetupOnly()) for the real one, same content the constructor would have shown had
    // ROMs been found the first time.
    void romsBecameAvailable();

    // Called by VirtualJVProcessor::setDisplayMode() (Alan's request, 2026-09-08) whenever the
    // Settings-tab display-mode choice changes - swaps which of lcd/panelDisplay is visible and
    // re-lays-out the top strip. See resized()'s own comment for the actual mode logic.
    void refreshDisplayMode();

    // Called by VirtualJVProcessor::setSequencerEnabled() (Alan's request, 2026-09-09) whenever
    // the Settings-tab sequencer toggle changes - creates/destroys the drawer (see
    // sequencerPanel's own comment) and re-lays-out.
    void refreshSequencerVisibility();

    // Right-click on the interface (the whole front panel or LCD strip, the drawer handles, the
    // editor background): "LCD" (colours) plus the shared "Sequencer > Classic / Retro / Grid"
    // submenu - see SequencerViewMenu.h. The Sequencer entry only exists in a Standalone build
    // with the sequencer enabled (the drawer doesn't exist otherwise).
    void mouseDown(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;

    // "Reset panel heights" (context menu): keyboard and sequencer panes back to their defaults.
    void resetPaneHeights();

    void setSelectedTab(const int index) { tabs.setCurrentTabIndex(index); }
    void setSelectedROM(const int index) { patchBrowser.categoriesListBox.selectRow(index); }
    void setLCDColor(const LCDisplay::Color color) { lcd.setLCDColor(color); }

private:
    // The Common/Tone/Rhythm/Settings tabs lay themselves out with fixed pixel row positions
    // (see e.g. EditCommonTab::resized()), not proportionally to the space they're given - so
    // rather than teach every one of those layouts to reflow, each is held at this fixed
    // "natural" size (matching what the tabs area has always been) inside its own Viewport, and
    // window resizing just clips/scrolls around it instead. Browse (PatchBrowser) needs none of
    // this: its juce::ListBoxes already scroll internally at whatever size they're given.
    static constexpr int kTabAreaW = 820;
    static constexpr int kTabAreaH = 800;

    VirtualJVProcessor& processor;

    // Top-of-window display (Alan's request, 2026-09-08): exactly one of these two is visible at
    // a time, chosen by processor.displayMode (Settings tab) - `lcd` for the classic fixed
    // 820x100 dot-matrix-only strip (LcdOnly, default/unchanged), `panelDisplay` for the photo-
    // based skin (PanelCompact/PanelFull - see PanelSkin.h), which spans the window's own current
    // width and embeds that same live LCD into the photo. See resized()/refreshDisplayMode().
    LCDisplay lcd;
    PanelSkin panelDisplay;
    // Bar above the tabs pane: click folds the whole Browse/Performance/Edit/Settings pane (the
    // window shrinks by its height, and grows back by it on unfolding). No drag: nothing above
    // it has a stored height (the top strip follows the window's width).
    CollapseHandle tabsHandle;
    bool tabsCollapsed = false;
    int savedTabsH = 400;
    TabBar tabs;
    PatchBrowser patchBrowser;
    PerformanceTab performanceTab;
    EditCommonTab editCommonTab;
    EditToneTab editTone1Tab;
    EditToneTab editTone2Tab;
    EditToneTab editTone3Tab;
    EditToneTab editTone4Tab;
    EditRhythmTab editRhythmTab;
    SettingsTab settingsTab;
    InterfaceTab interfaceTab;
    CollapseHandle keyboardHandle;
    VirtualKeyboard virtualKeyboard;
    bool keyboardCollapsed = false;

    // Pane heights, D-110 style (Alan's request, 2026-09-24): each bar is a click-to-fold AND a
    // drag handle. Dragging the keyboard bar resizes the tabs pane above it, the sequencer bar
    // the keyboard pane above it, and `bottomGrip` (under the last open drawer) that drawer -
    // the window grows or shrinks with the dragged pane. The tabs pane has no stored height: it
    // is whatever the window leaves, as before.
    static constexpr int kMinTabsH = 100;
    static constexpr int kMinKeyboardPaneH = 40, kMaxKeyboardPaneH = 400;
    static constexpr int kMinSequencerPaneH = 120, kMaxSequencerPaneH = 900;
    static constexpr int kHandleH = 18, kGripH = 10;
    int keyboardPaneH = VirtualJVProcessor::kDefaultKeyboardPaneH;
    int sequencerPaneH = VirtualJVProcessor::kDefaultSequencerPaneH;
    ResizeGrip bottomGrip;
    int dragStartValue = 0;
    int dragStartWindowH = 0;
    int topAreaHeight() const;
    // The drawer the bottom grip resizes: 1 keyboard, 2 sequencer, 0 none (nothing open below).
    int gripTarget() const;
    void resizePane(int &pane, int wanted, int minH, int maxH);

    // A second collapsible drawer below the keyboard (Alan's request, 2026-09-09), Standalone
    // build only - see JivSequencerPanel.h's own top comment. Lazily created/destroyed by
    // refreshSequencerVisibility() rather than always existing hidden, same convention as
    // SettingsTab's own audioDeviceSelector (std::unique_ptr<juce::Component>, standalone-only):
    // nullptr in every VST3/AU/LV2 instance, and in a Standalone instance too until Alan turns
    // the feature on in Settings.
    CollapseHandle sequencerHandle;
    // Exactly one of these three exists while the sequencer is on: the classic strip, the
    // retro LCD view or the piano roll, per processor.getSequencerRetroMode()/
    // getSequencerGridMode() (see refreshSequencerVisibility()).
    std::unique_ptr<JivSequencerPanel> sequencerPanel;
    std::unique_ptr<JivSequencerGridPanel> sequencerGridPanel;
    std::unique_ptr<JivSequencerRetroPanel> sequencerRetroPanel;
    void appendSequencerMenu(juce::PopupMenu &menu);
    void showAppContextMenu();
    bool sequencerCollapsed = false;
    juce::Component *activeSequencerView() const
    {
        if (sequencerGridPanel != nullptr) return sequencerGridPanel.get();
        if (sequencerRetroPanel != nullptr) return sequencerRetroPanel.get();
        return sequencerPanel.get();
    }

    juce::Viewport performanceViewport;
    juce::Viewport editCommonViewport, editTone1Viewport, editTone2Viewport, editTone3Viewport,
                   editTone4Viewport, editRhythmViewport, settingsViewport, interfaceViewport;

    // showToneOrRhythmEditTabs() tears down and rebuilds the whole TabbedComponent (clearTabs()
    // + re-addTab() for every tab) - fine when the tone/rhythm mode actually changes, but
    // setCurrentProgram() used to call it on EVERY patch pick regardless. That stole keyboard
    // focus back from whichever patches ListBox the user had just clicked, so arrow keys ended
    // up navigating the bank list instead of the patch that was just loaded (Alan's report).
    // -1 (neither 0 nor 1) so the very first call, from the constructor, always goes through.
    int tabsConfiguredForRhythm = -1;

    // Reduced tab set shown while processor.loaded is false (Alan's report, 2026-09-08: on a
    // fresh machine, ROMs not found used to mean an OS alert dialog and an otherwise-empty
    // window, with no way to fix it short of guessing the app-data folder and restarting). Only
    // Settings is added (it's the one tab that's safe/useful with no ROM data at all - see its
    // own ROM Folder section) - see showRomSetupOnly().
    void showRomSetupOnly();

    bool nativeTitleBarRequested = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VirtualJVEditor)
};
