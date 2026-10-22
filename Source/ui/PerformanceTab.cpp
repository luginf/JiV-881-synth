/*
  ==============================================================================

    PerformanceTab.cpp

  ==============================================================================
*/

#include "PerformanceTab.h"

//==============================================================================
PerformanceTab::PerformanceTab(VirtualJVProcessor &p) : processor(p)
{
  addAndMakeVisible(enablePerformanceToggle);
  enablePerformanceToggle.onClick = [this]
  {
    processor.setPerformanceModeEnabled(enablePerformanceToggle.getToggleState());
  };

  nameLabel.setText("Performance Name", juce::dontSendNotification);
  addAndMakeVisible(nameLabel);

  nameEditor.setInputRestrictions(12);
  addAndMakeVisible(nameEditor);
  nameEditor.onFocusLost = [this] { processor.setPerformanceName(nameEditor.getText()); };
  nameEditor.onReturnKey = [this] { processor.setPerformanceName(nameEditor.getText()); };

  boardHeader.setText("Board", juce::dontSendNotification);
  channelHeader.setText("Channel", juce::dontSendNotification);
  levelHeader.setText("Level", juce::dontSendNotification);
  panHeader.setText("Pan", juce::dontSendNotification);
  for (auto *header : {&boardHeader, &channelHeader, &levelHeader, &panHeader})
  {
    header->setJustificationType(juce::Justification::centred);
    addAndMakeVisible(*header);
  }

  addAndMakeVisible(allOnOffButton);
  allOnOffButton.onClick = [this]
  {
    int onCount = 0;
    for (auto &row : partRows)
      if (row.enabledToggle.getToggleState())
        onCount++;
    // Fewer than half on -> turn everything on; otherwise turn everything off. Either way at
    // least one Part's state visibly changes, unlike a plain "toggle current state" which could
    // silently do nothing if Parts were already mixed.
    const bool target = onCount * 2 < (int)partRows.size();
    for (int i = 0; i < VirtualJVProcessor::kNumPerformanceParts; i++)
    {
      partRows[(size_t)i].enabledToggle.setToggleState(target, juce::dontSendNotification);
      pushPartParams(i);
    }
  };

  for (int i = 0; i < VirtualJVProcessor::kNumPerformanceParts; i++)
  {
    auto &row = partRows[(size_t)i];
    const bool isRhythmPart = (i == VirtualJVProcessor::kNumPerformanceParts - 1);

    addAndMakeVisible(row.nameLabel);
    row.nameLabel.setText("Part " + juce::String(i + 1) + (isRhythmPart ? " (Rhythm): Empty" : ": Empty"),
                          juce::dontSendNotification);

    addAndMakeVisible(row.boardLabel);
    row.boardLabel.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(row.channelCombo);
    for (int ch = 1; ch <= 16; ch++)
      row.channelCombo.addItem(juce::String(ch), ch);
    row.channelCombo.setSelectedId(1, juce::dontSendNotification);
    row.channelCombo.onChange = [this, i] { pushPartParams(i); };

    addAndMakeVisible(row.levelSlider);
    row.levelSlider.onValueChange = [this, i] { pushPartParams(i); };

    addAndMakeVisible(row.panSlider);
    row.panSlider.onValueChange = [this, i] { pushPartParams(i); };

    addAndMakeVisible(row.enabledToggle);
    row.enabledToggle.setToggleState(true, juce::dontSendNotification);
    row.enabledToggle.onClick = [this, i] { pushPartParams(i); };

    addAndMakeVisible(row.clearButton);
    row.clearButton.onClick = [this, i] { processor.clearPerformancePart(i); };

    if (i < VirtualJVProcessor::kNumPerformanceParts - 1)
    {
      addAndMakeVisible(row.editButton);
      row.editButton.setClickingTogglesState(false);
      row.editButton.setTooltip("Edit this Part's patch in the Common and Tone tabs");
      row.editButton.onClick = [this, i]
      {
        if (processor.getEditingPerformancePart() == i)
          processor.endEditPerformancePart();
        else
          processor.beginEditPerformancePart(i);
        refreshFromProcessor();
      };
    }
  }

  // --- Part details -----------------------------------------------------------------------
  detailsHeaderLabel.setText("Part details", juce::dontSendNotification);
  detailsHeaderLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
  addAndMakeVisible(detailsHeaderLabel);
  coarseHeader.setText("Coarse tune", juce::dontSendNotification);
  fineHeader.setText("Fine tune", juce::dontSendNotification);
  outputHeader.setText("Output", juce::dontSendNotification);
  rxHeader.setText("FX sends / receive", juce::dontSendNotification);
  reserveHeader.setText("Voice reserve", juce::dontSendNotification);
  for (auto *header : {&coarseHeader, &fineHeader, &outputHeader, &rxHeader, &reserveHeader})
  {
    header->setJustificationType(juce::Justification::centred);
    addAndMakeVisible(*header);
  }

  for (int i = 0; i < VirtualJVProcessor::kNumPerformanceParts; i++)
  {
    auto &row = detailRows[(size_t)i];
    row.nameLabel.setText("Part " + juce::String(i + 1), juce::dontSendNotification);
    addAndMakeVisible(row.nameLabel);
    row.outputCombo.addItem("Main", 1);
    row.outputCombo.addItem("Sub", 2);
    row.outputCombo.addItem("Patch", 3);
    row.outputCombo.setSelectedId(1, juce::dontSendNotification);
    for (auto *c : std::initializer_list<juce::Component *>{&row.coarseSlider, &row.fineSlider, &row.reverbToggle,
                                                            &row.chorusToggle, &row.outputCombo, &row.pcToggle,
                                                            &row.volumeToggle, &row.holdToggle, &row.reserveSlider})
      addAndMakeVisible(*c);
    row.coarseSlider.onValueChange = [this, i] { pushDetailParams(i); };
    row.fineSlider.onValueChange = [this, i] { pushDetailParams(i); };
    row.reserveSlider.onValueChange = [this, i] { pushDetailParams(i); };
    row.outputCombo.onChange = [this, i] { pushDetailParams(i); };
    for (auto *b : {&row.reverbToggle, &row.chorusToggle, &row.pcToggle, &row.volumeToggle, &row.holdToggle})
      b->onClick = [this, i] { pushDetailParams(i); };
  }

  // --- Performance Common -----------------------------------------------------------------
  effectsHeaderLabel.setText("Performance common (shared by all Parts)", juce::dontSendNotification);
  effectsHeaderLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
  addAndMakeVisible(effectsHeaderLabel);

  auto setupCombo = [this](LabelledCombo &ctl, const char *label, std::initializer_list<const char *> items)
  {
    ctl.label.setText(label, juce::dontSendNotification);
    addAndMakeVisible(ctl.label);
    int id = 1;
    for (auto *item : items)
      ctl.combo.addItem(item, id++);
    ctl.combo.setSelectedId(1, juce::dontSendNotification);
    ctl.combo.onChange = [this] { pushCommonParams(); };
    addAndMakeVisible(ctl.combo);
  };
  setupCombo(keyModeCtl, "Key mode", {"Layer", "Zone", "Single"});
  setupCombo(reverbTypeCtl, "Reverb type",
             {"Room 1", "Room 2", "Stage 1", "Stage 2", "Hall 1", "Hall 2", "Delay", "Pan Delay"});
  setupCombo(chorusTypeCtl, "Chorus type", {"Chorus 1", "Chorus 2", "Chorus 3"});
  setupCombo(chorusOutputCtl, "Chorus output", {"Mix", "Reverb"});

  auto setupSlider = [this](LabelledSlider &ctl, const char *label)
  {
    ctl.label.setText(label, juce::dontSendNotification);
    addAndMakeVisible(ctl.label);
    ctl.slider.onValueChange = [this] { pushCommonParams(); };
    addAndMakeVisible(ctl.slider);
  };
  setupSlider(reverbLevelCtl, "Reverb level");
  setupSlider(reverbTimeCtl, "Reverb time");
  setupSlider(reverbFeedbackCtl, "Reverb feedback");
  setupSlider(chorusLevelCtl, "Chorus level");
  setupSlider(chorusDepthCtl, "Chorus depth");
  setupSlider(chorusRateCtl, "Chorus rate");
  setupSlider(chorusFeedbackCtl, "Chorus feedback");

  bankHeaderLabel.setText("Performance Bank", juce::dontSendNotification);
  addAndMakeVisible(bankHeaderLabel);

  addAndMakeVisible(saveAsButton);
  saveAsButton.onClick = [this]
  {
    auto dir = VirtualJVProcessor::performancesDir();
    dir.createDirectory();

    saveAsChooser = std::make_unique<juce::FileChooser>(
        "Save performance as...", dir.getChildFile("New Performance.jvpf"), "*.jvpf");

    saveAsChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser &fc)
        {
          auto file = fc.getResult();
          if (file == juce::File{})
            return;
          // Not every OS/dialog re-appends the filter extension when the whole filename field
          // was retyped rather than edited in place (same caveat as PatchBrowser's Save As...).
          if (!file.hasFileExtension("jvpf"))
            file = file.withFileExtension("jvpf");

          processor.savePerformanceAs(file);
          bankListBox.updateContent();
          bankListBox.repaint();
        });
  };

  addAndMakeVisible(deleteButton);
  deleteButton.onClick = [this]
  {
    const int row = bankListBox.getSelectedRow();
    if (row < 0 || row >= (int)processor.performanceBank.size())
      return;
    processor.performanceBank[(size_t)row].file.deleteFile();
    processor.refreshPerformanceBank();
    bankListBox.updateContent();
    bankListBox.repaint();
  };

  bankListBox.setRowHeight(20);
  addAndMakeVisible(bankListBox);

  refreshFromProcessor();
  startTimerHz(4);
}

PerformanceTab::~PerformanceTab() { stopTimer(); }

void PerformanceTab::pushPartParams(int partIndex)
{
  auto &row = partRows[(size_t)partIndex];
  processor.setPerformancePartParams(partIndex, row.channelCombo.getSelectedId(),
                                     (int)row.levelSlider.getValue(),
                                     (int)row.panSlider.getValue(),
                                     row.enabledToggle.getToggleState());
}

void PerformanceTab::pushDetailParams(int partIndex)
{
  if (updatingFromState)
    return;
  auto &row = detailRows[(size_t)partIndex];
  auto &part = processor.performanceParts[(size_t)partIndex];
  part.coarseTune = (int)row.coarseSlider.getValue();
  part.fineTune = (int)row.fineSlider.getValue();
  part.reverbSwitch = row.reverbToggle.getToggleState();
  part.chorusSwitch = row.chorusToggle.getToggleState();
  part.outputSelect = juce::jmax(0, row.outputCombo.getSelectedId() - 1);
  part.rxProgramChange = row.pcToggle.getToggleState();
  part.rxVolume = row.volumeToggle.getToggleState();
  part.rxHold1 = row.holdToggle.getToggleState();
  processor.performanceCommon.voiceReserve[partIndex] = (int)row.reserveSlider.getValue();
  processor.applyPerformancePart(partIndex);
  processor.applyPerformanceCommon(); // voice reserve lives in the Common record
}

void PerformanceTab::pushCommonParams()
{
  if (updatingFromState)
    return;
  auto &c = processor.performanceCommon;
  c.keyMode = juce::jmax(0, keyModeCtl.combo.getSelectedId() - 1);
  c.reverbType = juce::jmax(0, reverbTypeCtl.combo.getSelectedId() - 1);
  c.chorusType = juce::jmax(0, chorusTypeCtl.combo.getSelectedId() - 1);
  c.chorusOutput = juce::jmax(0, chorusOutputCtl.combo.getSelectedId() - 1);
  c.reverbLevel = (int)reverbLevelCtl.slider.getValue();
  c.reverbTime = (int)reverbTimeCtl.slider.getValue();
  c.reverbFeedback = (int)reverbFeedbackCtl.slider.getValue();
  c.chorusLevel = (int)chorusLevelCtl.slider.getValue();
  c.chorusDepth = (int)chorusDepthCtl.slider.getValue();
  c.chorusRate = (int)chorusRateCtl.slider.getValue();
  c.chorusFeedback = (int)chorusFeedbackCtl.slider.getValue();
  processor.applyPerformanceCommon();
}

// 0xff = ROM-native (880 factory), 0 = RD-500 board, 1..19 = SR-JV80-NN expansion cards.
static juce::String boardTag(uint8_t expansionI)
{
  if (expansionI == 0xff)
    return "[880fa]";
  if (expansionI == 0)
    return "[rd500]";
  return "[exp" + juce::String(expansionI).paddedLeft('0', 2) + "]";
}

// White when the Part's board is the one the engine has loaded (or the built-in 880 sounds, which
// are always there), grey when another external board is loaded and this Part will sound wrong.
void PerformanceTab::updateBoardColour(int partIndex)
{
  const auto &part = processor.performanceParts[(size_t)partIndex];
  const bool active = part.expansionI == 0xff || part.expansionI == processor.status.currentExpansion;
  const auto on = jivtheme::isLight() ? juce::Colour(0xff1d2327) : juce::Colours::white;
  partRows[(size_t)partIndex].boardLabel.setColour(juce::Label::textColourId, active ? on : juce::Colours::grey);
}

void PerformanceTab::updatePartRowFromState(int partIndex)
{
  auto &part = processor.performanceParts[(size_t)partIndex];
  auto &row = partRows[(size_t)partIndex];
  const bool isRhythmPart = (partIndex == VirtualJVProcessor::kNumPerformanceParts - 1);

  juce::String label = "Part " + juce::String(partIndex + 1) + (isRhythmPart ? " (Rhythm): " : ": ")
                      + (part.present ? juce::String(part.name) : "Empty");
  row.nameLabel.setText(label, juce::dontSendNotification);
  row.boardLabel.setText(part.present ? boardTag(part.expansionI) : juce::String(), juce::dontSendNotification);
  updateBoardColour(partIndex);
  row.channelCombo.setSelectedId(part.midiChannel, juce::dontSendNotification);
  row.levelSlider.setValue(part.level, juce::dontSendNotification);
  row.panSlider.setValue(part.pan, juce::dontSendNotification);
  row.enabledToggle.setToggleState(part.enabled, juce::dontSendNotification);
  row.editButton.setColour(juce::TextButton::buttonColourId,
                           processor.getEditingPerformancePart() == partIndex ? juce::Colour(0xff42A2C8)
                                                                              : jivtheme::buttonIdle());
  row.editButton.setEnabled(part.present);

  auto &d = detailRows[(size_t)partIndex];
  d.coarseSlider.setValue(part.coarseTune, juce::dontSendNotification);
  d.fineSlider.setValue(part.fineTune, juce::dontSendNotification);
  d.reverbToggle.setToggleState(part.reverbSwitch, juce::dontSendNotification);
  d.chorusToggle.setToggleState(part.chorusSwitch, juce::dontSendNotification);
  d.outputCombo.setSelectedId(part.outputSelect + 1, juce::dontSendNotification);
  d.pcToggle.setToggleState(part.rxProgramChange, juce::dontSendNotification);
  d.volumeToggle.setToggleState(part.rxVolume, juce::dontSendNotification);
  d.holdToggle.setToggleState(part.rxHold1, juce::dontSendNotification);
  d.reserveSlider.setValue(processor.performanceCommon.voiceReserve[partIndex], juce::dontSendNotification);
}

void PerformanceTab::timerCallback()
{
  if (isShowing() && processor.pollPerformanceFromFirmware())
    refreshFromProcessor();
  else if (isShowing())
    for (int i = 0; i < VirtualJVProcessor::kNumPerformanceParts; i++)
      updateBoardColour(i);
}

void PerformanceTab::refreshFromProcessor()
{
  enablePerformanceToggle.setToggleState(processor.performanceModeEnabled, juce::dontSendNotification);

  if (!nameEditor.hasKeyboardFocus(false))
    nameEditor.setText(juce::String(processor.performanceName).trimEnd(), juce::dontSendNotification);

  for (int i = 0; i < VirtualJVProcessor::kNumPerformanceParts; i++)
    updatePartRowFromState(i);

  const auto &c = processor.performanceCommon;
  keyModeCtl.combo.setSelectedId(c.keyMode + 1, juce::dontSendNotification);
  reverbTypeCtl.combo.setSelectedId(c.reverbType + 1, juce::dontSendNotification);
  chorusTypeCtl.combo.setSelectedId(c.chorusType + 1, juce::dontSendNotification);
  chorusOutputCtl.combo.setSelectedId(c.chorusOutput + 1, juce::dontSendNotification);
  reverbLevelCtl.slider.setValue(c.reverbLevel, juce::dontSendNotification);
  reverbTimeCtl.slider.setValue(c.reverbTime, juce::dontSendNotification);
  reverbFeedbackCtl.slider.setValue(c.reverbFeedback, juce::dontSendNotification);
  chorusLevelCtl.slider.setValue(c.chorusLevel, juce::dontSendNotification);
  chorusDepthCtl.slider.setValue(c.chorusDepth, juce::dontSendNotification);
  chorusRateCtl.slider.setValue(c.chorusRate, juce::dontSendNotification);
  chorusFeedbackCtl.slider.setValue(c.chorusFeedback, juce::dontSendNotification);

  bankListBox.updateContent();
  bankListBox.repaint();
}

void PerformanceTab::resized()
{
  const int margin = 10;
  int y = margin;

  enablePerformanceToggle.setBounds(margin, y, 260, 24);
  nameLabel.setBounds(margin + 270, y, 130, 24);
  nameEditor.setBounds(margin + 270 + 130 + 6, y, 160, 24);
  y += 24 + margin;

  const int nameW = 220, boardW = 64, comboW = 80, sliderW = 120, toggleW = 50, clearW = 70;
  const int colGap = 10;

  int headerX = margin + nameW + colGap;
  boardHeader.setBounds(headerX, y, boardW, 18);
  headerX += boardW + colGap;
  channelHeader.setBounds(headerX, y, comboW, 18);
  headerX += comboW + colGap;
  levelHeader.setBounds(headerX, y, sliderW, 18);
  headerX += sliderW + colGap;
  panHeader.setBounds(headerX, y, sliderW, 18);
  headerX += sliderW + colGap;
  allOnOffButton.setBounds(headerX, y - 2, toggleW + clearW, 22);
  y += 18 + 4;

  const int rowH = 26;
  for (auto &row : partRows)
  {
    int x = margin;
    row.nameLabel.setBounds(x, y, nameW, rowH);
    x += nameW + colGap;
    row.boardLabel.setBounds(x, y, boardW, rowH);
    x += boardW + colGap;
    row.channelCombo.setBounds(x, y + 2, comboW, rowH - 4);
    x += comboW + colGap;
    row.levelSlider.setBounds(x, y + 2, sliderW, rowH - 4);
    x += sliderW + colGap;
    row.panSlider.setBounds(x, y + 2, sliderW, rowH - 4);
    x += sliderW + colGap;
    row.enabledToggle.setBounds(x, y + 2, toggleW, rowH - 4);
    x += toggleW + colGap;
    row.clearButton.setBounds(x, y + 2, clearW, rowH - 4);
    row.editButton.setBounds(x + clearW + colGap, y + 2, clearW, rowH - 4);
    y += rowH + 4;
  }

  // Part details grid
  y += margin;
  detailsHeaderLabel.setBounds(margin, y, 300, 22);
  y += 22 + 2;
  {
    const int labelW = 60, slW = 100, outW = 70, tglW = 46, resW = 100, gap = 6;
    int hx = margin + labelW + gap;
    coarseHeader.setBounds(hx, y, slW, 18);
    hx += slW + gap;
    fineHeader.setBounds(hx, y, slW, 18);
    hx += slW + gap;
    outputHeader.setBounds(hx, y, outW, 18);
    hx += outW + gap;
    rxHeader.setBounds(hx, y, tglW * 5 + gap * 4, 18);
    hx += tglW * 5 + gap * 5;
    reserveHeader.setBounds(hx, y, resW, 18);
    y += 18 + 2;
    for (auto &row : detailRows)
    {
      int x = margin;
      row.nameLabel.setBounds(x, y, labelW, rowH);
      x += labelW + gap;
      row.coarseSlider.setBounds(x, y + 2, slW, rowH - 4);
      x += slW + gap;
      row.fineSlider.setBounds(x, y + 2, slW, rowH - 4);
      x += slW + gap;
      row.outputCombo.setBounds(x, y + 2, outW, rowH - 4);
      x += outW + gap;
      for (auto *b : {&row.reverbToggle, &row.chorusToggle, &row.pcToggle, &row.volumeToggle, &row.holdToggle})
      {
        b->setBounds(x, y + 2, tglW, rowH - 4);
        x += tglW + gap;
      }
      row.reserveSlider.setBounds(x, y + 2, resW, rowH - 4);
      y += rowH + 4;
    }
  }

  // Performance common: two rows of combos/sliders
  y += margin;
  effectsHeaderLabel.setBounds(margin, y, 400, 22);
  y += 22 + 4;
  {
    const int labelW = 90, ctlW = 120, gap = 8;
    int x = margin;
    for (auto *ctl : {&keyModeCtl, &reverbTypeCtl, &chorusTypeCtl, &chorusOutputCtl})
    {
      ctl->label.setBounds(x, y, labelW, 24);
      ctl->combo.setBounds(x + labelW, y, ctlW - 20, 24);
      x += labelW + ctlW - 20 + gap;
    }
    y += 24 + 4;
    x = margin;
    for (auto *ctl : {&reverbLevelCtl, &reverbTimeCtl, &reverbFeedbackCtl})
    {
      ctl->label.setBounds(x, y, 96, 24);
      ctl->slider.setBounds(x + 96, y + 2, 120, 20);
      x += 96 + 120 + gap;
    }
    y += 24 + 4;
    x = margin;
    int n = 0;
    for (auto *ctl : {&chorusLevelCtl, &chorusDepthCtl, &chorusRateCtl, &chorusFeedbackCtl})
    {
      ctl->label.setBounds(x, y, 96 - (n == 3 ? 0 : 20), 24);
      ctl->slider.setBounds(x + 96 - 20, y + 2, 90, 20);
      x += 96 - 20 + 90 + gap;
      ++n;
    }
    y += 24 + 4;
  }

  y += margin;
  bankHeaderLabel.setBounds(margin, y, 200, 22);
  saveAsButton.setBounds(getWidth() - margin - 220, y, 106, 22);
  deleteButton.setBounds(getWidth() - margin - 106, y, 106, 22);
  y += 22 + 4;

  bankListBox.setBounds(margin, y, getWidth() - margin * 2,
                        juce::jmax(0, getHeight() - y - margin));
}

//==============================================================================
int PerformanceTab::BankListModel::getNumRows()
{
  return (int)owner.processor.performanceBank.size();
}

void PerformanceTab::BankListModel::paintListBoxItem(int rowNumber, juce::Graphics &g, int width,
                                                      int height, bool rowIsSelected)
{
  g.fillAll(rowIsSelected ? jivtheme::listSelected() : jivtheme::listBg());
  g.setColour(jivtheme::listText(rowIsSelected));

  if (rowNumber >= 0 && rowNumber < (int)owner.processor.performanceBank.size())
    g.drawFittedText(owner.processor.performanceBank[(size_t)rowNumber].name, {5, 0, width, height - 2},
                     juce::Justification::left, 1);
}

void PerformanceTab::BankListModel::selectedRowsChanged(int lastRowSelected)
{
  if (lastRowSelected < 0)
    return;
  owner.processor.loadPerformance(lastRowSelected);
}
