/*
  ==============================================================================

    SettingsTab.h
    Created: 18 Aug 2024 1:05:04pm
    Author:  Giulio Zausa

  ==============================================================================
*/

#pragma once

#include <memory>
#include <JuceHeader.h>
#include "../JackMultiOut.h"
#include "../PluginProcessor.h"

#include "widgets/Button.h"
#include "widgets/Slider.h"

//==============================================================================

class SettingsTab  : public juce::Component,
                     public juce::Slider::Listener,
                     public juce::Button::Listener,
                     private juce::Timer
{
public:
    SettingsTab(VirtualJVProcessor&);
    ~SettingsTab() override;

    void updateValues();
    void resized() override;
    void sliderValueChanged (juce::Slider*) override;
    void buttonClicked (juce::Button*) override;
    void buttonStateChanged(juce::Button*) override {}

private:
    // Polls VirtualJVProcessor::dspLoadMeasurer a few times a second to refresh dspLoadLabel -
    // that measurer is updated from the audio thread but is itself lock-free/atomic to read.
    void timerCallback() override;

private:
    VirtualJVProcessor& processor;

    enum SettingsWidgets
    {
        MasterTune       = 10U,
        Reverb           = 11U,
        Chorus           = 12U,
        MasterVolume     = 13U,
        SequencerEnabled = 14U,
        ExtraPolyphony   = 15U,
        MultiOut         = 16U,
        DrumOutputs      = 19U,
        AudioInput       = 20U,
        MainFullMix      = 17U,
        OutputMode       = 18U,
    };

    Slider masterTuneSlider{ MasterTune, 1, 127, 1, 64, true };
    juce::Label masterTuneLabel;
    Button reverbToggle{ Reverb, "Reverb" };
    Button chorusToggle{ Chorus, "Chorus" };
    // Second engine for 56 voices instead of 28 (GitHub issue #7); only has an effect while
    // Performance mode is on, and costs about 2x the CPU then. See VirtualJVProcessor::mcu2.
    // Plugin formats only: start with the 8 "Part n" stereo output buses enabled (GitHub issue #3).
    // Part outputs are copies, the main output keeps the full mix (otherwise Parts leave the main).
    // Separate outputs layout: one pair per Part, or Main + Sub (hardware 4-OUT style).
    juce::ComboBox outputModeCombo;
    Button mainFullMixToggle{ MainFullMix, "Main out = full mix" };
    Button multiOutToggle{ MultiOut, "Part outputs on (reload plugin)" };
    // 16 extra stereo outputs, one per drum note 36-51 of the rhythm Part (Part 8); needs a reload / restart.
    Button drumOutputsToggle{ DrumOutputs, "Separate drum outputs (reload plugin)" };
    // Standalone only: audio input through the effects (use as a reverb/chorus processor), needs a restart.
    Button audioInputToggle{ AudioInput, "Audio input through effects (restart)" };
    Button extraPolyphonyToggle{ ExtraPolyphony, "Extra polyphony (Performance, 2x CPU)" };
    // Plugin-side output trim, not a real JV-880 hardware/MIDI parameter (the real unit's
    // volume knob is an analog attenuator, nothing SysEx/nvram reaches) - applied as a plain
    // gain in VirtualJVProcessor::processBlock(). Session-only, like the VirtualKeyboard
    // settings: not part of DataToSave (see that struct's own comment on why).
    Slider masterVolumeSlider{ MasterVolume, 0, 100, 1, 100 };
    juce::Label masterVolumeLabel;
    juce::Label buildDateLabel;

    // Audio/MIDI device settings (Alan's request, 2026-09-07): embedded here instead of behind
    // the Standalone wrapper's own separate "Settings..." popup window/dialog, which Alan
    // doesn't want. Only meaningful when actually running as the Standalone app (a plugin format
    // has no device/buffer-size of its own to control - that's the host's) - see the .cpp for
    // how that's detected at runtime. `audioDeviceSelector` is a plain juce::Component* (not the
    // concrete juce::AudioDeviceSelectorComponent type) so this header doesn't need to drag in
    // the Standalone-only JUCE header at all - only SettingsTab.cpp does, guarded.
    juce::Label audioSettingsHeaderLabel;
    juce::Label audioSettingsUnavailableLabel;
    std::unique_ptr<juce::Component> audioDeviceSelector;
    std::unique_ptr<DeviceTypeWatcher> deviceTypeWatcher; // remembers the chosen audio device type (JackMultiOut.h)

    // DSP load meter (Alan's request, 2026-09-07) - see VirtualJVProcessor::dspLoadMeasurer.
    juce::Label dspLoadLabel;

    // ROM folder configuration (Alan's request, 2026-09-08) - see VirtualJVProcessor::
    // getRomsFolder()/setRomsFolderOverride()/retryLoadRoms() for where this actually lives.
    // Shown regardless of whether ROMs are currently loaded: this tab is reachable even when
    // they aren't (see VirtualJVEditor::showRomSetupOnly()), which is the whole point - a user
    // on a fresh machine can point this at wherever they put their ROM dump and retry without
    // restarting the app/host.
    juce::Label romSectionHeaderLabel;
    juce::Label romStatusLabel;
    juce::Label romPathLabel;
    juce::TextButton romBrowseButton{"Browse..."};
    juce::TextButton romResetButton{"Use Default"};
    juce::TextButton romReloadButton{"Reload ROMs"};
    std::unique_ptr<juce::FileChooser> romFolderChooser;

    void refreshRomSection();

    // Top-of-window display mode (Alan's request, 2026-09-08) - see VirtualJVProcessor::
    // DisplayMode/setDisplayMode() and PanelSkin.h for what each choice actually shows.
    juce::Label displaySectionHeaderLabel;
    juce::Label extraFeaturesHeaderLabel; // optional, non-default features: separate drum outputs, extra polyphony
    juce::ComboBox displayModeCombo;

    // Sequencer on/off (Alan's request, 2026-09-09) - Standalone build only, same runtime
    // gating as audioDeviceSelector above; not even shown as a checkbox in VST3/AU/LV2, since
    // it could never do anything there. See VirtualJVProcessor::setSequencerEnabled().
    juce::Label sequencerSectionHeaderLabel;
    Button sequencerToggle{ SequencerEnabled, "Enable Sequencer" };
    // Which view the drawer shows (Classic / Retro / Grid) - see VirtualJVProcessor::
    // setSequencerView(). Also reachable by right-click, so timerCallback() keeps it in sync.
    juce::ComboBox sequencerViewCombo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsTab)
};
