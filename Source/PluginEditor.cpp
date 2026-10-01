/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#include "PluginEditor.h"
// Standalone only (the "Audio/MIDI Settings..." entry of the right-click menu reaches the
// StandaloneFilterWindow's plugin holder) - guarded because the plugin formats compile this file
// without juce_audio_utils' standalone wrapper being visible.
#include <juce_core/system/juce_TargetPlatform.h>
#if JucePlugin_Build_Standalone
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif
#include "PluginProcessor.h"
#include "rom.h"
#include <algorithm>

//==============================================================================
VirtualJVEditor::VirtualJVEditor(VirtualJVProcessor &p)
    : AudioProcessorEditor(&p), processor(p),
      lcd(p),
      panelDisplay(p,
                   p.displayMode == VirtualJVProcessor::DisplayMode::PanelFull
                       ? PanelSkin::Variant::kFull : PanelSkin::Variant::kCompact,
                   &lcd),
      tabs(), patchBrowser(p), performanceTab(p), editCommonTab(p),
      editTone1Tab(p, this, 0U), editTone2Tab(p, this, 1U), editTone3Tab(p, this, 2U), editTone4Tab(p, this, 3U), editRhythmTab(p, this),
      settingsTab(p), interfaceTab(p), virtualKeyboard(p)
{
    addAndMakeVisible(lcd);
    addAndMakeVisible(panelDisplay);
    lcd.setVisible(processor.displayMode == VirtualJVProcessor::DisplayMode::LcdOnly);
    panelDisplay.setVisible(!lcd.isVisible());
    addAndMakeVisible(tabsHandle);
    addAndMakeVisible(tabs);
    addAndMakeVisible(keyboardHandle);
    addAndMakeVisible(virtualKeyboard);

    tabsHandle.setLabel("Patches");
    tabsHandle.onRightClick = [this] { showAppContextMenu(); };
    tabsHandle.onClick = [this]
    {
        if (!tabsCollapsed)
        {
            savedTabsH = tabs.getHeight();
            tabsCollapsed = true;
            tabsHandle.setExpanded(false);
            tabs.setVisible(false);
            setSize(getWidth(), juce::jmax(0, getHeight() - savedTabsH));
        }
        else
        {
            tabsCollapsed = false;
            tabsHandle.setExpanded(true);
            tabs.setVisible(true);
            setSize(getWidth(), getHeight() + savedTabsH);
        }
        resized();
    };
    keyboardPaneH = processor.keyboardPaneH;
    sequencerPaneH = processor.sequencerPaneH;
    addAndMakeVisible(bottomGrip);

    keyboardHandle.onDragStart = [this] { dragStartWindowH = getHeight(); };
    keyboardHandle.onDrag = [this](int dy)
    {
        if (tabsCollapsed) return;
        // Tabs pane: no stored height, the window itself carries it.
        setSize(getWidth(), juce::jmax(dragStartWindowH + dy, getHeight() - (int)tabs.getHeight() + kMinTabsH));
    };
    sequencerHandle.onDragStart = [this] { dragStartValue = keyboardPaneH; dragStartWindowH = getHeight(); };
    sequencerHandle.onDrag = [this](int dy)
    {
        if (!keyboardCollapsed)
            resizePane(keyboardPaneH, dragStartValue + dy, kMinKeyboardPaneH, kMaxKeyboardPaneH);
    };
    sequencerHandle.onDragEnd = [this] { processor.setPaneHeights(keyboardPaneH, sequencerPaneH); };
    bottomGrip.onDragStart = [this]
    {
        dragStartValue = gripTarget() == 2 ? sequencerPaneH : keyboardPaneH;
        dragStartWindowH = getHeight();
    };
    bottomGrip.onDrag = [this](int dy)
    {
        if (gripTarget() == 2)
            resizePane(sequencerPaneH, dragStartValue + dy, kMinSequencerPaneH, kMaxSequencerPaneH);
        else if (gripTarget() == 1)
            resizePane(keyboardPaneH, dragStartValue + dy, kMinKeyboardPaneH, kMaxKeyboardPaneH);
    };
    bottomGrip.onDragEnd = [this] { processor.setPaneHeights(keyboardPaneH, sequencerPaneH); };

    keyboardHandle.setExpanded(!keyboardCollapsed);
    keyboardHandle.onClick = [this]
    {
        keyboardCollapsed = !keyboardCollapsed;
        keyboardHandle.setExpanded(!keyboardCollapsed);
        virtualKeyboard.setVisible(!keyboardCollapsed);
        resized();
    };

    // Second drawer, below the keyboard - see PluginEditor.h's own comment on sequencerPanel.
    // The handle itself always exists (cheap); the panel it collapses is only ever created by
    // refreshSequencerVisibility() (called at the very end of this constructor, once `tabs` is
    // actually populated - see that call site's own comment for why the ordering matters).
    sequencerHandle.setLabel("Sequencer");
    sequencerHandle.onRightClick = [this] { showAppContextMenu(); };
    keyboardHandle.onRightClick = [this] { showAppContextMenu(); };
    lcd.onContextMenu = [this] { showAppContextMenu(); };
    panelDisplay.onContextMenu = [this] { showAppContextMenu(); };
    sequencerHandle.setExpanded(!sequencerCollapsed);
    sequencerHandle.onClick = [this]
    {
        sequencerCollapsed = !sequencerCollapsed;
        sequencerHandle.setExpanded(!sequencerCollapsed);
        if (auto *view = activeSequencerView()) view->setVisible(!sequencerCollapsed);
        resized();
    };

    tabs.tabChangedFunction =
        [this](int index)
        {
            processor.status.selectedTab = index;
        };

    // Pin each fixed-layout tab at its natural size inside its own Viewport (see the members'
    // own comment in PluginEditor.h) - `false` for deleteComponentWhenNoLongerNeeded, since
    // these components are owned as plain members here, not by the viewport.
    auto pinInViewport = [](juce::Viewport &vp, juce::Component &content)
    {
        content.setSize(kTabAreaW, kTabAreaH);
        vp.setViewedComponent(&content, false);
        vp.setScrollBarsShown(true, false);
    };
    pinInViewport(performanceViewport, performanceTab);
    pinInViewport(editCommonViewport, editCommonTab);
    pinInViewport(editTone1Viewport, editTone1Tab);
    pinInViewport(editTone2Viewport, editTone2Tab);
    pinInViewport(editTone3Viewport, editTone3Tab);
    pinInViewport(editTone4Viewport, editTone4Tab);
    pinInViewport(editRhythmViewport, editRhythmTab);
    pinInViewport(settingsViewport, settingsTab);
    pinInViewport(interfaceViewport, interfaceTab);

    // Height is free to shrink well below the default: the tabs area scrolls (PatchBrowser's
    // ListBoxes natively, the other tabs via the Viewports above) rather than clipping. Width
    // can also grow past 820 (Alan's request, 2026-09-07) - the tabs area stays pinned at its
    // native 820px regardless (pinned per-Viewport, see pinInViewport above), but the keyboard
    // strip is fully responsive (VirtualKeyboard::rebuildKeys() lays out from getLocalBounds())
    // so it stretches to fill the extra width instead of leaving it blank. Minimum width still
    // can't go below 820 without clipping the tabs.
    //
    // The top strip's own width behaviour depends on processor.displayMode (Alan's request,
    // 2026-09-08 - "retirer la limite de resize en largeur"): `lcd` (LcdOnly) still draws its
    // emulated dot-matrix bitmap at a fixed 820x100, unscaled, same as always: but `panelDisplay`
    // (PanelCompact/PanelFull) scales to the window's own current width (see resized()), and
    // PanelFull's own native artwork is 3280px wide - so the old maxWidth=2400 cap (chosen back
    // when nothing in this window could usefully be wider than the 820px tabs anyway) is raised
    // well past that instead of removed outright (setResizeLimits has no "no limit" - some finite
    // bound is required), leaving plenty of headroom beyond even a 1:1 full-panel display.
    setResizable(true, true);
    setResizeLimits(820, 200, 6000, 4000);

    setSize(820, 900 + keyboardPaneH + kHandleH);

    // ROMs not found (Alan's report, 2026-09-08: this used to be a blocking OS alert dialog and
    // an otherwise-empty window - no visible path forward besides guessing the app-data folder
    // and restarting). The main interface now shows regardless, with a reduced tab set (just
    // Settings, which has its own ROM Folder section to point at the right place and retry
    // without restarting) - see showRomSetupOnly()/romsBecameAvailable().
    if (!processor.loaded)
    {
        showRomSetupOnly();
    }
    else
    {
        showToneOrRhythmEditTabs(processor.status.isDrums);
        setSelectedTab(processor.status.selectedTab);
        updateEditTabs();
    }

    // Called last, deliberately - it can call resized() internally (see its own body), and
    // doing that any earlier than this crashed in a Debug build (2026-09-09): tabs.setBounds()
    // inside a premature resized() touched TabbedComponent/Viewport internals before `tabs` had
    // any tabs added yet or the fixed-layout Viewports above were configured, tripping a JUCE
    // juce::Array bounds assertion (juce_ArrayBase.h:163) deep in there. Constructing everything
    // else in this editor first, THEN wiring up the sequencer drawer, avoids the whole class of
    // problem rather than only hiding it (jassert is compiled out in Release, so this would have
    // kept silently relying on undefined behaviour there instead of actually being fixed).
    refreshSequencerVisibility();
}

void VirtualJVEditor::showRomSetupOnly()
{
    const auto bgColor = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    tabs.clearTabs();
    tabs.addTab("Settings", bgColor, &settingsViewport, false);
    settingsTab.updateValues();
    // Not 0/1 (see this field's own comment) - forces romsBecameAvailable()'s call into
    // showToneOrRhythmEditTabs() to do a real rebuild instead of being short-circuited as a
    // no-op "already configured for this mode".
    tabsConfiguredForRhythm = -1;
}

void VirtualJVEditor::romsBecameAvailable()
{
    showToneOrRhythmEditTabs(processor.status.isDrums);
    setSelectedTab(processor.status.selectedTab);
    updateEditTabs();
}

VirtualJVEditor::~VirtualJVEditor()
{
    processor.status.selectedTab = tabs.getCurrentTabIndex();
}

void VirtualJVEditor::updateEditTabs()
{
    editCommonTab.updateValues();
    editTone1Tab.updateValues();
    editTone2Tab.updateValues();
    editTone3Tab.updateValues();
    editTone4Tab.updateValues();
    editRhythmTab.updateValues();
    settingsTab.updateValues();
}

void VirtualJVEditor::updatePerformanceTab()
{
    performanceTab.refreshFromProcessor();
}

void VirtualJVEditor::showToneOrRhythmEditTabs(const bool isRhythm)
{
    // Rebuilding the tabs (clearTabs() + re-addTab() below) is only needed when the tone/rhythm
    // set actually changed - it was previously called unconditionally from
    // VirtualJVProcessor::setCurrentProgram() on every single patch pick, which tore down and
    // rebuilt the TabbedComponent's content each time and stole keyboard focus back from
    // whichever patches ListBox the user had just clicked (Alan's report: arrow keys navigated
    // the bank list instead of moving between patches after clicking one).
    const int wantRhythm = isRhythm ? 1 : 0;
    if (tabsConfiguredForRhythm == wantRhythm)
    {
        editCommonTab.rhythmSetMode(isRhythm);
        return;
    }
    tabsConfiguredForRhythm = wantRhythm;

    const auto bgColor = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    auto selTab = processor.status.selectedTab;

    tabs.clearTabs();

    // The Interface (now labelled "Panel") tab only makes sense when the top-of-window display
    // is LCD-only (Alan's request, 2026-09-08): it exists to give button/DATA-dial access when
    // there's no photo-based panel already on screen. Renamed to "Panel" since that's exactly
    // what it now is together with the always-visible LCD above it in that mode - a photo-based
    // panel interface, just without duplicating the LCD/Volume that mode already shows elsewhere.
    // Always the LAST tab in both branches, so hiding it never shifts any other tab's index.
    const bool showPanelTab = processor.displayMode == VirtualJVProcessor::DisplayMode::LcdOnly;

    if (isRhythm)
    {
        tabs.addTab("Browse", bgColor, &patchBrowser, false);
        tabs.addTab("Performance", bgColor, &performanceViewport, false);
        tabs.addTab("Common", bgColor, &editCommonViewport, false);
        tabs.addTab("Rhythm Set", bgColor, &editRhythmViewport, false);
        tabs.addTab("Settings", bgColor, &settingsViewport, false);
        if (showPanelTab)
            tabs.addTab("Panel", bgColor, &interfaceViewport, false);
    }
    else
    {
        tabs.addTab("Browse", bgColor, &patchBrowser, false);
        tabs.addTab("Performance", bgColor, &performanceViewport, false);
        tabs.addTab("Common", bgColor, &editCommonViewport, false);
        tabs.addTab("Tone 1", bgColor, &editTone1Viewport, false);
        tabs.addTab("Tone 2", bgColor, &editTone2Viewport, false);
        tabs.addTab("Tone 3", bgColor, &editTone3Viewport, false);
        tabs.addTab("Tone 4", bgColor, &editTone4Viewport, false);
        tabs.addTab("Settings", bgColor, &settingsViewport, false);
        if (showPanelTab)
            tabs.addTab("Panel", bgColor, &interfaceViewport, false);
    }

    // just in case... - index 3 is "Rhythm Set" in the isRhythm branch (Browse=0, Performance=1,
    // Common=2, Rhythm Set=3, Settings=4) - Settings moved to the end (Alan's request,
    // 2026-09-07), which happens to put Rhythm Set back at its original pre-Performance-tab
    // index since Settings no longer sits between Performance and Common.
    if (selTab > 3 && processor.status.isDrums)
    {
        selTab = 3;
        tabs.setCurrentTabIndex(selTab);
    }

    processor.status.selectedTab = selTab;

    editCommonTab.rhythmSetMode(isRhythm);
}

uint8_t VirtualJVEditor::getSelectedRomIdx()
{
    auto idx = patchBrowser.categoriesListBox.getSelectedRow();

    if (idx <= 0)
    {
        return 2; // internal ROM 2 of the 880, contains multisample info table
    }
    else
    {
        return std::min(romCountRequired + idx, romCount - 1); // RD expansion ROM and other SR-JV ROMs henceforth
    }
}

int VirtualJVEditor::topAreaHeight() const
{
    // LcdOnly is the fixed 820x100 it's always been; the panel modes scale with the window's own
    // current width instead, spanning it fully (unlike the tabs below, which stay pinned at 820
    // - see the constructor's setResizeLimits() comment).
    if (processor.displayMode == VirtualJVProcessor::DisplayMode::LcdOnly)
        return 100;
    return (int)panelDisplay.heightForWidth((float)getWidth());
}

int VirtualJVEditor::gripTarget() const
{
    if (activeSequencerView() != nullptr)
        return sequencerCollapsed ? 0 : 2;
    return keyboardCollapsed ? 0 : 1;
}

// Applies a dragged pane height and moves the window by the same amount, so every other pane
// keeps its size. If the window can't change (maximized, tiled), the tabs pane absorbs it.
void VirtualJVEditor::resizePane(int &pane, int wanted, int minH, int maxH)
{
    const int clamped = juce::jlimit(minH, maxH, wanted);
    const int windowH = dragStartWindowH + (clamped - dragStartValue);
    pane = clamped;
    setSize(getWidth(), windowH);
    resized();
}

void VirtualJVEditor::resetPaneHeights()
{
    const int delta = (VirtualJVProcessor::kDefaultKeyboardPaneH - keyboardPaneH) * (keyboardCollapsed ? 0 : 1)
                    + (VirtualJVProcessor::kDefaultSequencerPaneH - sequencerPaneH)
                        * ((activeSequencerView() != nullptr && !sequencerCollapsed) ? 1 : 0);
    keyboardPaneH = VirtualJVProcessor::kDefaultKeyboardPaneH;
    sequencerPaneH = VirtualJVProcessor::kDefaultSequencerPaneH;
    processor.setPaneHeights(keyboardPaneH, sequencerPaneH);
    setSize(getWidth(), getHeight() + delta);
    resized();
}

void VirtualJVEditor::resized()
{
    const int handleH = kHandleH;
    int keyboardH = keyboardCollapsed ? 0 : keyboardPaneH;
    // Second handle+drawer, only when the sequencer is actually turned on (see
    // refreshSequencerVisibility()) - both stay 0 otherwise, so the layout below is identical
    // to before this feature existed whenever it's off.
    auto *sequencerView = activeSequencerView();
    const int sequencerHandleH = sequencerView ? handleH : 0;
    int sequencerH = (sequencerView && !sequencerCollapsed) ? sequencerPaneH : 0;
    const int gripH = gripTarget() != 0 ? kGripH : 0;

    const int topAreaH = topAreaHeight();
    if (processor.displayMode == VirtualJVProcessor::DisplayMode::LcdOnly)
        lcd.setBounds(0, 0, 820, 100);
    else
        panelDisplay.setBounds(0, 0, getWidth(), topAreaH);

    const int fixedH = topAreaH + handleH + handleH + keyboardH + sequencerHandleH + sequencerH + gripH;
    const int tabsH = tabsCollapsed ? 0 : juce::jmax(0, getHeight() - fixedH);
    if (tabsCollapsed)
    {
        // Nothing to absorb the window's spare height: the last open drawer does.
        const int extra = juce::jmax(0, getHeight() - fixedH);
        if (sequencerH > 0) sequencerH += extra;
        else if (keyboardH > 0) keyboardH += extra;
    }

    tabsHandle.setBounds(0, topAreaH, getWidth(), handleH);
    tabs.setBounds(0, topAreaH + handleH, 820, tabsH);
    const int keyboardHandleY = topAreaH + handleH + tabsH;
    keyboardHandle.setBounds(0, keyboardHandleY, getWidth(), handleH);
    virtualKeyboard.setBounds(0, keyboardHandleY + handleH, getWidth(), keyboardH);
    int y = keyboardHandleY + handleH + keyboardH;
    if (sequencerView)
    {
        sequencerHandle.setBounds(0, y, getWidth(), sequencerHandleH);
        sequencerView->setBounds(0, y + sequencerHandleH, getWidth(), sequencerH);
        y += sequencerHandleH + sequencerH;
    }
    bottomGrip.setVisible(gripH > 0);
    bottomGrip.setBounds(0, y, getWidth(), gripH);
}

void VirtualJVEditor::refreshSequencerVisibility()
{
    const bool wantIt = processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone
                         && processor.getSequencerEnabled();

    const bool wantRetro = wantIt && processor.getSequencerRetroMode();
    const bool wantGrid = wantIt && !wantRetro && processor.getSequencerGridMode();

    // Swap the view when the choice changed (or drop everything when turned off) - the engine
    // and every song live in the processor, so a view is just a lens and can be destroyed/
    // recreated freely.
    if (!wantIt || wantRetro != (sequencerRetroPanel != nullptr) || wantGrid != (sequencerGridPanel != nullptr))
    {
        sequencerPanel.reset();
        sequencerGridPanel.reset();
        sequencerRetroPanel.reset();
    }

    if (wantIt && activeSequencerView() == nullptr)
    {
        if (wantRetro)
        {
            sequencerRetroPanel = std::make_unique<JivSequencerRetroPanel>(processor);
            addAndMakeVisible(*sequencerRetroPanel);
        }
        else if (wantGrid)
        {
            sequencerGridPanel = std::make_unique<JivSequencerGridPanel>(processor);
            addAndMakeVisible(*sequencerGridPanel);
        }
        else
        {
            sequencerPanel = std::make_unique<JivSequencerPanel>(processor);
            addAndMakeVisible(*sequencerPanel);
        }
        addAndMakeVisible(sequencerHandle);
        activeSequencerView()->setVisible(!sequencerCollapsed);
    }
    else if (!wantIt)
    {
        sequencerHandle.setVisible(false);
    }

    resized();
}

void VirtualJVEditor::refreshDisplayMode()
{
    const bool lcdOnly = processor.displayMode == VirtualJVProcessor::DisplayMode::LcdOnly;
    lcd.setVisible(lcdOnly);
    panelDisplay.setVisible(!lcdOnly);
    if (!lcdOnly)
        panelDisplay.setVariant(processor.displayMode == VirtualJVProcessor::DisplayMode::PanelFull
                                     ? PanelSkin::Variant::kFull : PanelSkin::Variant::kCompact);

    // Forces the tab rebuild below to actually run (see tabsConfiguredForRhythm's own comment) -
    // the Panel tab's presence depends on displayMode too now, not just isRhythm, so a mode
    // change alone (isRhythm unchanged) would otherwise be short-circuited as a no-op.
    tabsConfiguredForRhythm = -1;
    showToneOrRhythmEditTabs(processor.status.isDrums);
    setSelectedTab(processor.status.selectedTab);

    resized();
}

void VirtualJVEditor::parentHierarchyChanged()
{
    juce::AudioProcessorEditor::parentHierarchyChanged();
    // Plugin builds: no such window exists (the host draws its own). Standalone:
    // StandaloneFilterWindow's ctor hardcodes JUCE's own custom-drawn title bar, with no
    // constructor hook to ask for the native one instead, so it's flipped here, the first time
    // this editor is far enough up the hierarchy to reach that window.
    if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone) return;
    if (nativeTitleBarRequested) return;
    nativeTitleBarRequested = true;

    // Deferred to the next message-loop turn rather than done inline here: this callback
    // fires as soon as the editor is added as the window's content component (see
    // ResizableWindow::setContent(), which adds the child BEFORE resizing to fit it), which is
    // before StandaloneFilterWindow's own constructor has resized/positioned the window to its
    // final bounds. Switching the title bar inline recreates the native window peer at
    // whatever tiny bounds the DocumentWindow still had at that point - confirmed on a fresh
    // Linux build, the window got wedged at 128x128. Deferring past the full constructor (which
    // finishes synchronously before this async callback can run) avoids that race.
    juce::Component::SafePointer<VirtualJVEditor> safeThis(this);
    juce::MessageManager::callAsync([safeThis] {
        if (safeThis == nullptr) return;
        if (auto *dw = dynamic_cast<juce::DocumentWindow *>(safeThis->getTopLevelComponent()))
            if (!dw->isUsingNativeTitleBar()) dw->setUsingNativeTitleBar(true);
        safeThis->applyWindowIcon();
        // The native peer may be recreated or shown a little later - set the icon again once it
        // has settled.
        for (int delayMs : { 400, 2000 })
            juce::Timer::callAfterDelay(delayMs, [safeThis] {
                if (safeThis != nullptr) safeThis->applyWindowIcon();
            });
    });
}

// The Virtual Jiv logo (Assets/VirtualJiV Logo.svg, already embedded as a resource) as the
// Standalone window's / taskbar icon. The Projucer big/small icon settings cover Windows and
// macOS only, and DocumentWindow::setIcon() only paints the custom title bar, so the native peer
// gets it directly (the X11 _NET_WM_ICON property on Linux). Rendered to 256px: an icon property
// as large as the SVG's native size would be megabytes. Standalone only: inside a DAW the window
// belongs to the host.
void VirtualJVEditor::applyWindowIcon()
{
#if !JUCE_ANDROID   // the Android app embeds its own binary data (no logo SVG) and has no window icon
    if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone) return;
    auto *peer = getPeer();
    if (peer == nullptr) return;
    std::unique_ptr<juce::Drawable> logo(juce::Drawable::createFromImageData(
        BinaryData::VirtualJiV_Logo_svg, size_t(BinaryData::VirtualJiV_Logo_svgSize)));
    if (logo == nullptr) return;
    juce::Image icon(juce::Image::ARGB, 256, 256, true);
    juce::Graphics g(icon);
    logo->drawWithin(g, juce::Rectangle<float>(0.0f, 0.0f, 256.0f, 256.0f),
                     juce::RectanglePlacement::centred, 1.0f);
    peer->setIcon(icon);
#endif
}

// The app-wide right-click menu: "LCD" (colours) and, when the drawer exists, "Sequencer > Classic /
// Retro / Grid" (shared with the D-110 project's front ends - see
// SequencerViewMenu.h). Nothing is added when the drawer isn't there (plugin builds, or the
// sequencer switched off in Settings).
void VirtualJVEditor::appendSequencerMenu(juce::PopupMenu &menu)
{
    if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone
        || !processor.getSequencerEnabled())
        return;

    menu.addSeparator();
    seqview::addSubmenu(menu, seqview::current(processor),
                        [this](seqview::View v) { processor.setSequencerView(v); });
}

void VirtualJVEditor::showAppContextMenu()
{
    juce::PopupMenu menu;
    lcd.addColorSubmenu(menu);
    appendSequencerMenu(menu);
    menu.addSeparator();
    menu.addItem("Reset panel heights", [this] { resetPaneHeights(); });
#if JucePlugin_Build_Standalone
    // Audio device + MIDI inputs of the Standalone app (a DAW offers its own, so plugins skip it).
    if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
    {
        menu.addSeparator();
        menu.addItem("Audio/MIDI Settings...", [this] {
            if (auto *win = dynamic_cast<juce::StandaloneFilterWindow *>(getTopLevelComponent()))
                win->getPluginHolder()->showAudioSettingsDialog();
        });
    }
#endif
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void VirtualJVEditor::mouseDown(const juce::MouseEvent &e)
{
    if (e.mods.isPopupMenu())
        showAppContextMenu();
}

// The retro view's D-pad keys (EXIT/ENTER/arrows). Key events go to whichever component has
// focus and bubble up its parents when unconsumed; clicking a piano key (or anything else)
// moves focus away from the retro panel, so it is also offered every key here - the nearest
// shared ancestor. Same idea as the D-110 editor's own keyPressed().
bool VirtualJVEditor::keyPressed(const juce::KeyPress &key)
{
    return sequencerRetroPanel != nullptr && sequencerRetroPanel->keyPressed(key);
}
