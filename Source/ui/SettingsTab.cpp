/*
  ==============================================================================

    SettingsTab.cpp
    Created: 18 Aug 2024 1:05:04pm
    Author:  Giulio Zausa

  ==============================================================================
*/

#include "SettingsTab.h"
#include <JuceHeader.h>
#include "../JackMultiOut.h"

// Only declares StandalonePluginHolder/AudioDeviceSelectorComponent wiring - this project builds
// all of AU/LV2/Standalone/VST3 from one shared "Shared Code" library (see Builds/LinuxMakefile),
// so JucePlugin_Build_Standalone is 1 for that shared compilation regardless of which actual
// wrapper links it (each format's own small wrapper .cpp is what's format-specific). The real
// per-instance check is the runtime one below (StandalonePluginHolder::getInstance() only
// returns non-null inside the actual standalone executable, since that's the only wrapper that
// ever constructs one).
#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

//==============================================================================
SettingsTab::SettingsTab(VirtualJVProcessor &p) : processor(p)
{
  addAndMakeVisible(masterTuneSlider);
  masterTuneSlider.addListener(this);
  masterTuneSlider.setTextValueSuffix(" Hz");
  masterTuneSlider.textFromValueFunction =
      [this](double value)
      {
        double floatValue = (value - 1) / 126;
        return juce::String(floatValue * (452.6 - 427.4) + 427.4, 0, false);
      };
  masterTuneSlider.valueFromTextFunction = 
      [this](const juce::String &text)
      {
        double floatValue = text.getDoubleValue();
        return (floatValue - 427.4) / (452.6 - 427.4) * 126 + 1;
      };

  addAndMakeVisible(masterTuneLabel);
  masterTuneLabel.setText("Master Tune", juce::dontSendNotification);
  masterTuneLabel.attachToComponent(&masterTuneSlider, true);

  addAndMakeVisible(reverbToggle);
  reverbToggle.addListener(this);

  addAndMakeVisible(chorusToggle);
  chorusToggle.addListener(this);

  addAndMakeVisible(masterVolumeSlider);
  masterVolumeSlider.addListener(this);
  masterVolumeSlider.setTextValueSuffix(" %");

  addAndMakeVisible(masterVolumeLabel);
  masterVolumeLabel.setText("Master Volume", juce::dontSendNotification);
  masterVolumeLabel.attachToComponent(&masterVolumeSlider, true);

  const auto buildTime = juce::Time::getCompilationDate();
  const juce::String buildInfo = "Build Date: " + buildTime.formatted("%d %b %Y, %H:%M:%S");

  addAndMakeVisible(buildDateLabel);
  buildDateLabel.setText(buildInfo, juce::dontSendNotification);
  buildDateLabel.setJustificationType(juce::Justification::centredRight);

  addAndMakeVisible(audioSettingsHeaderLabel);
  audioSettingsHeaderLabel.setText("Audio/MIDI Settings", juce::dontSendNotification);
  audioSettingsHeaderLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));

#if JucePlugin_Build_Standalone
  if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
  {
    if (auto *holder = juce::StandalonePluginHolder::getInstance())
    {
#if JUCE_LINUX
      // Our own JACK device type with 18 free, named output ports (JUCE's offers 8 auto-connected ones).
      bool alreadyAdded = false;
      for (auto *type : holder->deviceManager.getAvailableDeviceTypes())
        alreadyAdded = alreadyAdded || type->getTypeName() == jivjack::kTypeName;
      if (!alreadyAdded)
      {
        holder->deviceManager.addAudioDeviceType(std::make_unique<jivjack::DeviceType>());
        // The device state saved by the Standalone was restored before this type existed: switch back to it.
        if (processor.getPreferredAudioDeviceType() == jivjack::kTypeName)
          holder->deviceManager.setCurrentAudioDeviceType(jivjack::kTypeName, true);
      }
#endif
      deviceTypeWatcher = std::make_unique<DeviceTypeWatcher>(
          holder->deviceManager, [this](const juce::String &type) { processor.setPreferredAudioDeviceType(type); });

      auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
          holder->deviceManager,
          0, holder->getNumInputChannels(),
          // Up to 18 outputs: main + one stereo pair per Performance Part (separate outputs, issue #3)
          0, juce::jmax(holder->getNumOutputChannels(), 2 + 2 * VirtualJVProcessor::kNumPerformanceParts),
          true,                    // showMidiInputOptions
          processor.producesMidi(), // showMidiOutputSelector
          true,                    // showChannelsAsStereoPairs
          false);                  // hideAdvancedOptionsWithButton - keep sample rate/buffer size visible directly
      addAndMakeVisible(*selector);
      audioDeviceSelector = std::move(selector);
    }
  }
#endif

  if (audioDeviceSelector == nullptr)
  {
    addAndMakeVisible(audioSettingsUnavailableLabel);
    audioSettingsUnavailableLabel.setText(
        "Audio device and buffer size are controlled by your DAW/host when running as a plugin.",
        juce::dontSendNotification);
  }

  addAndMakeVisible(dspLoadLabel);
  dspLoadLabel.setText("DSP Load: -- %", juce::dontSendNotification);

  addAndMakeVisible(romSectionHeaderLabel);
  romSectionHeaderLabel.setText("ROM Folder", juce::dontSendNotification);
  romSectionHeaderLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));

  addAndMakeVisible(romStatusLabel);
  addAndMakeVisible(romPathLabel);
  romPathLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
  romPathLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

  addAndMakeVisible(romBrowseButton);
  romBrowseButton.onClick = [this]
  {
    romFolderChooser = std::make_unique<juce::FileChooser>(
        "Choose the folder containing your JV-880 ROM files", VirtualJVProcessor::getRomsFolder());
    romFolderChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser &fc)
        {
          auto dir = fc.getResult();
          if (dir == juce::File{})
            return;
          processor.setRomsFolderOverride(dir);
          if (!processor.loaded)
            processor.retryLoadRoms();
          refreshRomSection();
        });
  };

  addAndMakeVisible(romResetButton);
  romResetButton.onClick = [this]
  {
    processor.setRomsFolderOverride(juce::File{});
    if (!processor.loaded)
      processor.retryLoadRoms();
    refreshRomSection();
  };

  addAndMakeVisible(romReloadButton);
  romReloadButton.onClick = [this]
  {
    processor.retryLoadRoms();
    refreshRomSection();
  };

  refreshRomSection();

  addAndMakeVisible(displaySectionHeaderLabel);
  displaySectionHeaderLabel.setText("Display", juce::dontSendNotification);
  displaySectionHeaderLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));

  addAndMakeVisible(displayModeCombo);
  displayModeCombo.addItem("LCD only", 1);
  displayModeCombo.addItem("Panel (Compact)", 2);
  displayModeCombo.addItem("Panel (Full)", 3);
  displayModeCombo.setSelectedId((int)processor.displayMode + 1, juce::dontSendNotification);
  displayModeCombo.onChange = [this]
  {
    processor.setDisplayMode((VirtualJVProcessor::DisplayMode)(displayModeCombo.getSelectedId() - 1));
  };

  addAndMakeVisible(extraPolyphonyToggle);
  extraPolyphonyToggle.setToggleState(processor.getExtraPolyphony(), juce::dontSendNotification);
  extraPolyphonyToggle.addListener(this);

  addAndMakeVisible(outputModeCombo);
  outputModeCombo.addItem("Outputs: Main + Parts", 1);
  outputModeCombo.addItem("Outputs: Main + Sub", 2);
  outputModeCombo.setSelectedId(processor.getOutputMode() + 1, juce::dontSendNotification);
  outputModeCombo.setTooltip("Main + Parts: one stereo pair per Performance Part. Main + Sub: one extra pair with the dry sum "
                             "of the Parts whose Output is Sub (Performance tab > Part details), like the JV-880's SUB jack.");
  outputModeCombo.onChange = [this] { processor.setOutputMode(outputModeCombo.getSelectedId() - 1); };

  addAndMakeVisible(mainFullMixToggle);
  mainFullMixToggle.setToggleState(processor.getMainFullMix(), juce::dontSendNotification);
  mainFullMixToggle.setTooltip("Separate Part outputs: keep the complete mix on the main output and send each Part to its own "
                               "output as a copy. Off: Parts leave the main output, which then only has the effects return.");
  mainFullMixToggle.addListener(this);

  if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
  {
    addAndMakeVisible(multiOutToggle);
    multiOutToggle.setToggleState(processor.getMultiOutBuses(), juce::dontSendNotification);
    multiOutToggle.setTooltip("Start with 8 extra stereo outputs, one per Performance Part (effects stay on the main output). "
                              "Applies to plugin instances created after this change.");
    multiOutToggle.addListener(this);
  }

  if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
  {
    addAndMakeVisible(sequencerSectionHeaderLabel);
    sequencerSectionHeaderLabel.setText("Sequencer", juce::dontSendNotification);
    sequencerSectionHeaderLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));

    addAndMakeVisible(sequencerToggle);
    sequencerToggle.setToggleState(processor.getSequencerEnabled(), juce::dontSendNotification);
    sequencerToggle.addListener(this);

    addAndMakeVisible(sequencerViewCombo);
    sequencerViewCombo.addItem("Classic", 1);
    sequencerViewCombo.addItem("Retro", 2);
    sequencerViewCombo.addItem("Grid", 3);
    sequencerViewCombo.setSelectedId((int)seqview::current(processor) + 1, juce::dontSendNotification);
    sequencerViewCombo.onChange = [this]
    {
      processor.setSequencerView((seqview::View)(sequencerViewCombo.getSelectedId() - 1));
    };
  }

  startTimerHz(4);
}

SettingsTab::~SettingsTab() {}

void SettingsTab::timerCallback()
{
  // dspLoadMeasurer is written from the audio thread but is internally atomic/lock-free to
  // read - see juce::AudioProcessLoadMeasurer's own header.
  const double loadPercent = processor.dspLoadMeasurer.getLoadAsPercentage();
  dspLoadLabel.setText("DSP Load: " + juce::String(loadPercent, 1) + " %",
                       juce::dontSendNotification);

  // The view can also change from the right-click menu.
  const int viewId = (int)seqview::current(processor) + 1;
  if (sequencerViewCombo.getSelectedId() != viewId)
    sequencerViewCombo.setSelectedId(viewId, juce::dontSendNotification);
}

void SettingsTab::updateValues()
{
  masterTuneSlider.setValue(((int8_t *)processor.mcu->nvram)[0x00] + 64, juce::dontSendNotification);
  reverbToggle.setToggleState((processor.mcu->nvram[0x02] >> 0) & 1, juce::dontSendNotification);
  chorusToggle.setToggleState((processor.mcu->nvram[0x02] >> 1) & 1, juce::dontSendNotification);
  masterVolumeSlider.setValue(processor.getMasterVolume() * 100.0, juce::dontSendNotification);
  refreshRomSection();
}

void SettingsTab::refreshRomSection()
{
  if (processor.loaded)
  {
    romStatusLabel.setText("ROMs loaded.", juce::dontSendNotification);
    romStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
    romReloadButton.setEnabled(false);
  }
  else
  {
    romStatusLabel.setText(
        "ROM files not found here. Copy your JV-880 ROM dump into this folder (or pick a "
        "different one below), then click Reload ROMs.",
        juce::dontSendNotification);
    romStatusLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    romReloadButton.setEnabled(true);
  }
  romPathLabel.setText(VirtualJVProcessor::getRomsFolder().getFullPathName(),
                       juce::dontSendNotification);
}

void SettingsTab::resized()
{
  const auto top = 10;
  const auto sliderLeft1 = 100;
  const auto width = getWidth() / 3 - sliderLeft1 - 10;
  const auto sliderLeft2 = sliderLeft1 + getWidth() / 3 + 2;
  const auto sliderLeft3 = sliderLeft2 + getWidth() / 3;
  const auto height = 24;
  const auto row2Top = top + height + 20;

  auto sliderLeft = 120;

  reverbToggle      .setBounds(sliderLeft1 - 90, top, width, height);
  chorusToggle      .setBounds(sliderLeft2 - 90, top, width, height);
  masterTuneSlider  .setBounds(sliderLeft3, top, width, height);
  masterVolumeSlider.setBounds(sliderLeft3, row2Top, width, height);
  dspLoadLabel      .setBounds(sliderLeft1 - 90, row2Top, width, height);
  extraPolyphonyToggle.setBounds(sliderLeft2 - 90, row2Top, width + 60, height);

  // Display mode (Alan's request, 2026-09-08) - see PanelSkin.h/VirtualJVProcessor::DisplayMode.
  const auto displaySectionTop = row2Top + height + 20;
  displaySectionHeaderLabel.setBounds(10, displaySectionTop, 200, 22);
  displayModeCombo.setBounds(220, displaySectionTop, 220, 24);

  // Sequencer on/off (Alan's request, 2026-09-09) - same row as Display, right-aligned; only
  // present at all (see the constructor) in a Standalone build.
  multiOutToggle.setBounds(460, displaySectionTop, 260, 24);
  mainFullMixToggle.setBounds(460, displaySectionTop + 28, 150, 24);
  outputModeCombo.setBounds(614, displaySectionTop + 28, 190, 24);
  sequencerSectionHeaderLabel.setBounds(460, displaySectionTop, 90, 22);
  sequencerToggle.setBounds(550, displaySectionTop, 140, 24);
  sequencerViewCombo.setBounds(694, displaySectionTop, 110, 24);

  // ROM Folder section (Alan's request, 2026-09-08), between Display and Audio/MIDI Settings -
  // see refreshRomSection().
  const auto romSectionTop = displaySectionTop + 22 + 20;
  romSectionHeaderLabel.setBounds(10, romSectionTop, 400, 22);
  romStatusLabel.setBounds(10, romSectionTop + 24, getWidth() - 20, 36);
  romPathLabel.setBounds(10, romSectionTop + 62, getWidth() - 20, 20);
  romBrowseButton.setBounds(10, romSectionTop + 86, 110, 24);
  romResetButton.setBounds(126, romSectionTop + 86, 110, 24);
  romReloadButton.setBounds(242, romSectionTop + 86, 140, 24);

  // Below the rest (Alan's request, 2026-09-07) - this tab has plenty of unused vertical space
  // between row2Top and buildDateLabel's own fixed position already.
  const auto audioSectionTop = romSectionTop + 86 + 24 + 20;
  audioSettingsHeaderLabel.setBounds(10, audioSectionTop, 400, 24);

  const auto audioContentTop = audioSectionTop + 30;
  const auto audioContentBottom = 725; // leaves buildDateLabel's own row (735) clear
  if (audioDeviceSelector != nullptr)
    audioDeviceSelector->setBounds(10, audioContentTop, getWidth() - 20,
                                   audioContentBottom - audioContentTop);
  else
    audioSettingsUnavailableLabel.setBounds(10, audioContentTop, getWidth() - 20, 24);

  buildDateLabel    .setBounds(10, 735, 800, 20);
}

void SettingsTab::sliderValueChanged(juce::Slider *slider)
{
  uint32_t id = 0xFFFFFFF;

  if (auto i = dynamic_cast<Slider*>(slider))
  {
    id = i->getID();
  }

  if (id == MasterTune)
  {
    processor.sendSysexParamChange(0x01, (uint8_t)masterTuneSlider.getValue());
  }
  else if (id == MasterVolume)
  {
    processor.setMasterVolume((float)masterVolumeSlider.getValue() / 100.0f);
  }
}

void SettingsTab::buttonClicked(juce::Button *button)
{
  uint32_t id = 0xFFFFFFF;

  if (auto i = dynamic_cast<Button*>(button))
  {
    id = i->getID();
  }

  switch (id)
  {
  case Reverb:
    processor.sendSysexParamChange(0x04, reverbToggle.getToggleState());
    break;
  case Chorus:
    processor.sendSysexParamChange(0x05, chorusToggle.getToggleState());
    break;
  case ExtraPolyphony:
    processor.setExtraPolyphony(extraPolyphonyToggle.getToggleState());
    break;
  case MainFullMix:
    processor.setMainFullMix(mainFullMixToggle.getToggleState());
    break;
  case MultiOut:
    processor.setMultiOutBuses(multiOutToggle.getToggleState());
    break;
  case SequencerEnabled:
    processor.setSequencerEnabled(sequencerToggle.getToggleState());
    break;
  }
}
