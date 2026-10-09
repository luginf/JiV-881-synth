/*
  ==============================================================================

    PatchBrowser.cpp
    Created: 18 Aug 2024 1:01:38pm
    Author:  Giulio Zausa

  ==============================================================================
*/

#include "PatchBrowser.h"
#include <JuceHeader.h>

//==============================================================================
PatchBrowser::PatchBrowser(VirtualJVProcessor &p)
    : processor(p), categoriesListModel(), categoriesListBox("Categories", &categoriesListModel)
{
  addAndMakeVisible(revertButton);
  revertButton.onClick = [this] { processor.revertCurrentPatch(); };

  addAndMakeVisible(importButton);
  importButton.setTooltip("Import the patches of a JV-880 SysEx (.syx) or MIDI (.mid) dump into the User bank");
  importButton.onClick = [this]
  {
    if (!processor.loaded)
      return;
    importChooser = std::make_unique<juce::FileChooser>(
        "Import JV-880 patches (SysEx or MIDI file)...",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.syx;*.mid;*.midi");
    importChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                               [this](const juce::FileChooser &fc)
                               {
                                 const auto file = fc.getResult();
                                 if (file != juce::File{})
                                   startSysexImport(file);
                               });
  };

  addAndMakeVisible(saveAsButton);
  saveAsButton.onClick = [this]
  {
    if (!processor.loaded)
      return;

    auto dir = VirtualJVProcessor::userPatchesDir();
    dir.createDirectory();

    // Tone patches carry their own name (first 12 bytes of status.patch, same field the
    // Common tab's Patch Name box edits) - offered as the default so re-saving an already
    //-named patch doesn't make the user retype it. Rhythm sets have no such field.
    juce::String defaultName = processor.status.isDrums
        ? "New Rhythm Set"
        : juce::String(reinterpret_cast<const char *>(processor.status.patch), 12).trim();
    if (defaultName.isEmpty())
      defaultName = "New Patch";

    saveAsChooser = std::make_unique<juce::FileChooser>(
        "Save patch as...", dir.getChildFile(defaultName + ".jvp"), "*.jvp");

    saveAsChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser &fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{})
              return;
            // Not every OS/dialog re-appends the filter extension when the whole filename
            // field was retyped rather than edited in place (confirmed on Linux/GTK) - enforce
            // it here so refreshUserPatches()'s "*.jvp" scan always finds what was just saved.
            if (!file.hasFileExtension("jvp"))
              file = file.withFileExtension("jvp");

            processor.saveCurrentPatchAs(file);

            // The save may have landed in the currently-visible User bank (or the user may
            // browse to it later) - refresh unconditionally rather than tracking whether it's
            // the active category right now.
            refreshAfterPatchesChanged();
        });
  };

  categoriesListBox.setRowHeight(34);
  addAndMakeVisible(categoriesListBox);
  categoriesListModel.addChangeListener(this);

  for (int i = 0; i < maxColumnsPool; i++)
  {
    // Real startI/endI are set by the first reflowPatchColumns() call (triggered by the initial
    // resized() pass before anything is shown) - these are just placeholders.
    patchesListModels[i] = new PatchesListModel(0, 0, this, &categoriesListBox, &categoriesListModel);
    patchesListBoxes[i] = new juce::ListBox("Patches", patchesListModels[i]);
    patchesListModels[i]->owner = patchesListBoxes[i];

    patchesListBoxes[i]->setRowHeight(rowHeight);
    addChildComponent(*patchesListBoxes[i]); // visibility (in/active) decided by reflowPatchColumns()
    // true = also hear the rows nested inside each ListBox - see mouseDown()'s own comment.
    patchesListBoxes[i]->addMouseListener(this, true);
  }

  if (processor.loaded)
  {
    if (processor.status.selectedRom < 0)
    {
      processor.status.selectedRom = 0;
    }

    categoriesListBox.selectRow(processor.status.selectedRom);

    /* this doesn't seem to work for some reason
    const auto col = processor.status.selectedPatch / currentRowsPerColumn;
    const auto row = processor.status.selectedPatch % currentRowsPerColumn;

    patchesListBoxes[col]->selectRow(row);
    */
  }
}

void PatchBrowser::showSendMenuForPatch(int index)
{
  if (index < 0 || !processor.loaded || !processor.isEligibleForPerformancePart(index))
    return;
  const bool isDrums = processor.patchInfos[index].drums;

  // Deferred to the next message-loop turn (confirmed necessary while testing this
  // feature): selecting a row that also flips Patch/Rhythm mode makes
  // PatchesListModel::selectedRowsChanged() - called synchronously, as part of the same
  // mouse event that leads here - rebuild the whole TabbedComponent
  // (tabs.clearTabs()/addTab() inside VirtualJVEditor::showToneOrRhythmEditTabs()).
  // Showing the popup inline, mid-rebuild, made it silently fail to appear. Letting that
  // settle first sidesteps relying on JUCE's reparenting-during-event-dispatch behaviour.
  juce::Component::SafePointer<PatchBrowser> safeParent(this);
  juce::MessageManager::callAsync([safeParent, index, isDrums] {
    if (safeParent == nullptr) {
      return;
    }

    constexpr int numParts = VirtualJVProcessor::kNumPerformanceParts;
    auto menu = juce::PopupMenu();
    for (int s = 0; s < numParts; s++) {
      const bool partIsRhythm = (s == numParts - 1);
      if (partIsRhythm != isDrums)
        continue; // rhythm sets only go to the fixed Rhythm Part, tones only to the other 7
      auto &part = safeParent->processor.performanceParts[s];
      juce::String label = "Send to Performance Part " + juce::String(s + 1) +
                           (partIsRhythm ? " (Rhythm)" : "") +
                           (part.present ? " (" + juce::String(part.name) + ")" : " (Empty)");
      menu.addItem(s + 1, label);
    }

    // "Send to Sequencer" submenu (Alan's request, 2026-09-09) - only offered once the
    // sequencer is turned on in Settings. Deliberately calls setSequencerTrackPatch(), NOT
    // sendPatchToPerformancePart() - a sequencer track's own patch is decoupled from
    // PerformancePart's live state (see PluginProcessor.h's own comment on sequencerEngine),
    // only pushed into the live Part at the PLAY/REC edge, so choosing a patch here never
    // changes what's currently sounding in Performance mode nor what's stored under "Send
    // to Performance Part N" above. Item ids offset by numParts so a single popup result
    // can tell the two submenus apart.
    if (safeParent->processor.getSequencerEnabled()) {
      juce::PopupMenu seqMenu;
      for (int s = 0; s < numParts; s++) {
        const bool partIsRhythm = (s == numParts - 1);
        if (partIsRhythm != isDrums)
          continue;
        const auto trackPatch = safeParent->processor.getSequencer().getTrackPatch(s);
        juce::String label = "Part " + juce::String(s + 1) +
                             (partIsRhythm ? " (Rhythm)" : "") +
                             (trackPatch.index >= 0 ? " (" + trackPatch.name + ")" : " (Empty)");
        seqMenu.addItem(numParts + s + 1, label);
      }
      menu.addSubMenu("Send to Sequencer", seqMenu);
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
                       [safeParent, index](int result) {
      if (safeParent == nullptr) {
        return;
      }
      if (result >= 1 && result <= VirtualJVProcessor::kNumPerformanceParts)
        safeParent->processor.sendPatchToPerformancePart(index, result - 1);
      else if (result > VirtualJVProcessor::kNumPerformanceParts
               && result <= 2 * VirtualJVProcessor::kNumPerformanceParts)
        // setSequencerTrackPatch() takes (track, patchInfoIndex) - the OPPOSITE order from
        // sendPatchToPerformancePart() just above (patchInfoIndex, partIndex). Mixing the two
        // up here (index, track) is exactly why every "Send to Sequencer" click used to fail
        // silently: the clicked patch's own index (often >7) landed in the `track` parameter
        // and got rejected by its range guard - see setSequencerTrackPatch()'s own comment.
        safeParent->processor.setSequencerTrackPatch(
            result - 1 - VirtualJVProcessor::kNumPerformanceParts, index);
    });
  });
}

// Long-press on a patch row -> the right-click "Send to..." menu (see the header's own comment).
// The ordinary tap (select the row, i.e. audition it) still happens exactly as before; only a
// genuine hold additionally opens the menu. The row is selected explicitly when the hold fires
// (a no-op if the tap already selected it) so the finger-lift that follows doesn't then change
// the selection - and possibly rebuild the Patch/Rhythm tab set - while the menu is up.
void PatchBrowser::mouseDown(const juce::MouseEvent &e)
{
  if (!e.source.isTouch() || !processor.loaded)
    return;

  for (int i = 0; i < activeColumns; i++)
  {
    auto *lb = patchesListBoxes[i];
    if (!lb->isVisible() || !(lb == e.originalComponent || lb->isParentOf(e.originalComponent)))
      continue;

    const auto pos = e.getEventRelativeTo(lb).getPosition();
    const int row = lb->getRowContainingPosition(pos.x, pos.y);
    if (patchesListModels[i]->patchIndexForRow(row) < 0)
      return;

    longPressStartPos = e.position;
    const int token = ++longPressToken;
    juce::Component::SafePointer<PatchBrowser> safeThis(this);
    juce::Timer::callAfterDelay(500, [safeThis, token, i, row]
    {
      auto *self = safeThis.getComponent();
      if (self == nullptr || token != self->longPressToken)
        return;
      // Bump so the finger-lift's mouseUp() has nothing left to cancel and a second timer
      // (none today) could not re-fire.
      ++self->longPressToken;
      const int index = self->patchesListModels[i]->patchIndexForRow(row);
      self->patchesListBoxes[i]->selectRow(row);
      self->showSendMenuForPatch(index);
    });
    return;
  }
}

void PatchBrowser::mouseDrag(const juce::MouseEvent &e)
{
  // Touch always wobbles a little; past ~10px it's a scroll, not a hold.
  if (e.position.getDistanceFrom(longPressStartPos) > 10.0f)
    ++longPressToken;
}

void PatchBrowser::mouseUp(const juce::MouseEvent &)
{
  ++longPressToken;
}

PatchBrowser::~PatchBrowser()
{
  processor.status.selectedRom = categoriesListBox.getSelectedRow();
  categoriesListModel.removeChangeListener(this);

  for (int i = 0; i < maxColumnsPool; i++)
  {
    if (patchesListBoxes[i]->getSelectedRow() > -1)
    {
      processor.status.selectedPatch =
          (i * currentRowsPerColumn) + patchesListBoxes[i]->getSelectedRow();
    }

    delete patchesListModels[i];
    delete patchesListBoxes[i];
  }
}

void PatchBrowser::refreshAfterPatchesChanged()
{
  categoriesListBox.updateContent();
  categoriesListBox.repaint();
  for (int i = 0; i < maxColumnsPool; i++)
  {
    patchesListBoxes[i]->updateContent();
    patchesListBoxes[i]->repaint();
  }
}

namespace
{
// Runs VirtualJVProcessor::importSysexFile() on its own thread, behind a progress window the user can cancel.
class SysexImportJob : public juce::ThreadWithProgressWindow
{
public:
  SysexImportJob(VirtualJVProcessor &p, const juce::File &f, std::function<void(VirtualJVProcessor::SysexImportReport)> done)
      : juce::ThreadWithProgressWindow("Importing " + f.getFileName() + "...", true, true), processor(p), file(f),
        onDone(std::move(done))
  {
  }

  void run() override
  {
    report = processor.importSysexFile(
        file,
        [this](int index, int count, const juce::String &name)
        {
          setProgress((double)index / (double)juce::jmax(1, count));
          setStatusMessage(juce::String(index + 1) + " / " + juce::String(count) + "   " + name);
          return !threadShouldExit();
        },
        [this](int ms) { wait(ms); });
  }

  void threadComplete(bool /*userPressedCancel*/) override
  {
    auto done = std::move(onDone);
    auto result = report;
    juce::MessageManager::callAsync([done, result] { if (done) done(result); });
  }

private:
  VirtualJVProcessor &processor;
  juce::File file;
  VirtualJVProcessor::SysexImportReport report;
  std::function<void(VirtualJVProcessor::SysexImportReport)> onDone;
};
} // namespace

void PatchBrowser::startSysexImport(const juce::File &file)
{
  if (!processor.loaded || importJob != nullptr)
    return;

  juce::Component::SafePointer<PatchBrowser> safeThis(this);
  importJob = std::make_unique<SysexImportJob>(
      processor, file,
      [safeThis](VirtualJVProcessor::SysexImportReport report)
      {
        if (safeThis == nullptr)
          return;
        safeThis->importJob.reset();
        safeThis->refreshAfterPatchesChanged();

        juce::String text;
        if (report.error.isNotEmpty())
          text = report.error;
        else
          text = juce::String(report.imported) + " patch(es) imported into the User bank"
                 + (report.failed > 0 ? ", " + juce::String(report.failed) + " failed" : juce::String()) + ".";
        for (const auto &n : report.notes)
          text += "\n- " + n;
        juce::AlertWindow::showMessageBoxAsync(report.error.isNotEmpty() || report.imported == 0
                                                   ? juce::MessageBoxIconType::WarningIcon
                                                   : juce::MessageBoxIconType::InfoIcon,
                                               "SysEx import", text);
      });
  importJob->launchThread();
}

void PatchBrowser::resized()
{
  const int topBarH = 26;
  saveAsButton.setBounds(getWidth() - 110, 2, 106, topBarH - 4);
  revertButton.setBounds(getWidth() - 270, 2, 156, topBarH - 4);
  importButton.setBounds(getWidth() - 430, 2, 156, topBarH - 4);

  const int listsH = getHeight() - topBarH;
  categoriesListBox.setBounds(0, topBarH, 180, listsH);

  reflowPatchColumns();
}

// Spreads the current category's patches across as many columns as fit the available height
// without scrolling, rather than a fixed 44 rows/column that only ever scrolled in place when
// the window was short (Alan's request, 2026-09-07). Called on resize and on category change
// (different categories hold different patch counts, so the same height can need a different
// column count either way).
void PatchBrowser::reflowPatchColumns()
{
  const int topBarH = 26;
  const int listsH = getHeight() - topBarH;
  const int availableWidth = getWidth() - 180;
  if (listsH <= 0 || availableWidth <= 0)
    return;

  const int groupI = categoriesListBox.getSelectedRow();
  const int totalItems =
      (processor.loaded && groupI >= 0 && groupI < (int)processor.patchInfoPerGroup.size())
          ? (int)processor.patchInfoPerGroup[groupI].size()
          : 0;

  const int rowsPerColumnByHeight = juce::jmax(1, listsH / rowHeight);
  const int neededColumnsByHeight =
      juce::jmax(1, (totalItems + rowsPerColumnByHeight - 1) / rowsPerColumnByHeight);
  const int maxColumnsByWidth = juce::jmin(maxColumnsPool, juce::jmax(1, availableWidth / minColumnWidth));

  int rowsPerColumn;
  int newActiveColumns;
  if (neededColumnsByHeight <= maxColumnsByWidth)
  {
    // Everything fits without scrolling - use exactly as many columns as that needs, not more.
    newActiveColumns = neededColumnsByHeight;
    rowsPerColumn = rowsPerColumnByHeight;
  }
  else
  {
    // Not enough width to show it all at once even using every column available - grow each
    // column past what's visibly on-screen instead (falling back to the shared/synced scroll)
    // so every item stays reachable, rather than silently capping the list at
    // maxColumnsByWidth * rowsPerColumnByHeight and hiding the rest with no way to reach it.
    newActiveColumns = maxColumnsByWidth;
    rowsPerColumn = juce::jmax(rowsPerColumnByHeight,
                               (totalItems + maxColumnsByWidth - 1) / maxColumnsByWidth);
  }

  // The grid dimensions actually changed - any existing row selection now maps to a different
  // flat patch index than before (same local row, different startI), so drop it rather than
  // leave a highlight pointing at the wrong patch. The currently loaded patch itself (LCD/edit
  // tabs) is unaffected - this only clears the Browse list's own highlight.
  const bool dimensionsChanged =
      newActiveColumns != activeColumns || rowsPerColumn != currentRowsPerColumn;
  if (dimensionsChanged)
    for (int i = 0; i < maxColumnsPool; i++)
      patchesListBoxes[i]->deselectAllRows();

  activeColumns = newActiveColumns;
  currentRowsPerColumn = rowsPerColumn;

  const int columnWidth = availableWidth / activeColumns;

  for (int i = 0; i < maxColumnsPool; i++)
  {
    if (i >= activeColumns)
    {
      patchesListBoxes[i]->setVisible(false);
      continue;
    }

    const int newStartI = rowsPerColumn * i;
    const int newEndI = rowsPerColumn * (i + 1);
    if (patchesListModels[i]->startI != newStartI || patchesListModels[i]->endI != newEndI)
    {
      patchesListModels[i]->startI = newStartI;
      patchesListModels[i]->endI = newEndI;
      patchesListBoxes[i]->updateContent();
    }

    patchesListBoxes[i]->setVisible(true);
    patchesListBoxes[i]->setBounds(180 + columnWidth * i, topBarH, columnWidth, listsH);

    // Only the last active column keeps a (shared, see listWasScrolled()) scrollbar - the
    // trailing `true` keeps mouse-wheel scrolling working on the others despite their scrollbar
    // being hidden (Viewport::useMouseWheelMoveIfNeeded() otherwise gates on its visibility).
    patchesListBoxes[i]->getViewport()->setScrollBarsShown(i == activeColumns - 1, false, true, false);
  }
}
