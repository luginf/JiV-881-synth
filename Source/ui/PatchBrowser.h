/*
  ==============================================================================

    PatchBrowser.h
    Created: 18 Aug 2024 1:01:38pm
    Author:  Giulio Zausa

  ==============================================================================
*/

#pragma once

#include "../PluginProcessor.h"
#include <JuceHeader.h>
#include "../rom.h"

static const char *groupNames[] = {
    "880 Factory",
    "500 Factory",
    "Exp 01 Pop",
    "Exp 02 Orchestral",
    "Exp 03 Piano",
    "Exp 04 Vintage Synth",
    "Exp 05 World",
    "Exp 06 Dance",
    "Exp 07 Super Sound Set",
    "Exp 08 60s/70s Keyboards",
    "Exp 09 Session",
    "Exp 10 Bass & Drum",
    "Exp 11 Techno",
    "Exp 12 Hip-Hop",
    "Exp 13 Vocal",
    "Exp 14 Asia",
    "Exp 15 Special FX",
    "Exp 16 Orchestral II",
    "Exp 17 Country",
    "Exp 18 Latin",
    "Exp 19 House",
    "Exp Custom",
    "User" // patches saved via Save As... (userGroupIndex in PluginProcessor.h) - not a ROM
           // bank, so the romInfos-loaded checks below all skip this row.
};

// Fixed pool of ListBoxes PatchBrowser ever allocates - reflowPatchColumns() shows/hides and
// resizes however many of these (activeColumns, <= this pool size) the current window height
// and category actually need, rather than a fixed 6x44 grid (Alan's request, 2026-09-07: a
// short window used to just force scrolling within a fixed 44-row column instead of using the
// extra width for more, shorter columns). Comfortably more than minColumnWidth would ever allow
// to actually fit side by side even in a very short window.
const int maxColumnsPool = 10;
const int rowHeight = 17;
const int minColumnWidth = 90;

//==============================================================================
/*
 */
class PatchBrowser : public juce::Component, public juce::ChangeListener {
public:
  PatchBrowser(VirtualJVProcessor &);
  ~PatchBrowser() override;

  void resized() override;

  // Recomputes how many columns are needed to show the current category's full patch list
  // without scrolling in the available height, and how many rows each then holds - called from
  // resized() (window/tab-area size changed) and, via changeListenerCallback() below, whenever
  // the selected category changes (different categories have different patch counts, so the
  // same window height can need a different column count).
  void reflowPatchColumns();
  void changeListenerCallback(juce::ChangeBroadcaster *) override { reflowPatchColumns(); }

  VirtualJVProcessor &processor;

  // Called right after a click/tap on a patch row has loaded it (setCurrentProgram()) - the
  // Android app's HOLD button uses it to re-strike its held note on each newly picked patch.
  std::function<void()> onPatchSelected;

  int activeColumns = 1;
  int currentRowsPerColumn = 1;

  // Discards edits made to the current patch since it was picked - Alan's request. A plain
  // juce::TextButton, not the custom widgets/Button.h (that one's a ToggleButton for on/off
  // switches, not a one-shot action).
  juce::TextButton revertButton { "Revert to Original" };
  // Saves the current patch/rhythm-set to the "User" bank under a new name - Alan's request.
  juce::TextButton saveAsButton { "Save As..." };
  std::unique_ptr<juce::FileChooser> saveAsChooser; // kept alive until launchAsync()'s callback fires

  // Imports the patches of a JV-880 .syx / .mid dump into the "User" bank (GitHub issue #4).
  juce::TextButton importButton { "Import SysEx..." };
  std::unique_ptr<juce::FileChooser> importChooser;
  std::unique_ptr<juce::ThreadWithProgressWindow> importJob; // runs VirtualJVProcessor::importSysexFile()
  void startSysexImport(const juce::File &file);
  void refreshAfterPatchesChanged();

  class CategoriesListModel : public juce::ListBoxModel,
                              public juce::ChangeBroadcaster {
    int getNumRows() override { return userGroupIndex + 1; }

    void paintListBoxItem(int rowNumber, juce::Graphics &g, int width,
                          int height, bool rowIsSelected) override {
      g.fillAll(rowIsSelected ? juce::Colour(0xff42A2C8)
                              : juce::Colour(0xff263238));

      g.setColour(rowIsSelected ? juce::Colours::black : juce::Colours::white);

      if (rowNumber < userGroupIndex && rowNumber > 0)
      {
        g.setOpacity(romInfos[romCountRequired + rowNumber].loaded ? 1.f : 0.25f);
      }
      // else: row 0 (880 Factory, always loaded) and userGroupIndex (User, not ROM-backed at
      // all) both stay at full opacity.

      if (rowNumber <= userGroupIndex)
      {
        g.drawFittedText(groupNames[rowNumber], {5, 0, width, height - 2},
                         juce::Justification::left, 1);
      }

      g.setColour(juce::Colours::white.withAlpha(0.4f));
      g.drawRect(0, height - 1, width, 2);
      g.drawRect(width - 2, 0, width, height);
    }

    void selectedRowsChanged(int /* lastRowSelected */) override {
      sendChangeMessage();
    }
  };

  CategoriesListModel categoriesListModel;
  juce::ListBox categoriesListBox;

  class PatchesListModel : public juce::ListBoxModel,
                           public juce::ChangeListener {
  public:
    PatchesListModel(int startI, int endI, PatchBrowser *parent,
                     juce::ListBox *categoriesListBox,
                     CategoriesListModel *categoriesListModel)
        : startI(startI), endI(endI), categoriesListBox(categoriesListBox),
          categoriesListModel(categoriesListModel), parent(parent) {
      categoriesListModel->addChangeListener(this);
    }

    ~PatchesListModel() override {
      categoriesListModel->removeChangeListener(this);
    }

    int getNumRows() override {
      // The User bank isn't backed by any ROM (userGroupIndex, see PluginProcessor.h) - it's
      // however many patches are actually on disk, with no romInfos[]-loaded gate to check
      // (indexing romInfos[groupI + romCountRequired] for that group would in fact run past
      // the end of the array, since it's one past the last real ROM-backed group).
      if (groupI == userGroupIndex)
        return std::min(endI - startI,
                        (int)parent->processor.patchInfoPerGroup[groupI].size() - startI);

      if (!parent->processor.loaded ||
          (groupI > 0 && !romInfos[std::max(groupI, 0) + romCountRequired].loaded) ||
          (groupI == 1 && !romInfos[std::max(groupI, 0) + romCountRequired - 1].loaded))
      {
        return 0;
      }

      return std::min(
          endI - startI,
          (int)parent->processor.patchInfoPerGroup[groupI].size() -
              startI);
    }

    void paintListBoxItem(int rowNumber, juce::Graphics &g, int width,
                          int height, bool rowIsSelected) override {
      g.fillAll(rowIsSelected ? juce::Colour(0xff42A2C8)
                              : juce::Colour(0xff263238));

      if (!parent->processor.loaded) {
        return;
      }

      auto *info = parent->processor.patchInfoPerGroup[groupI][rowNumber + startI];

      // Edited-but-not-(re)saved patches show in amber instead of white - Alan's request, so
      // they're easy to spot while browsing. Not shown once selected: the row's already black
      // on the selection highlight, and that's a stronger enough cue on its own.
      const bool modified = !rowIsSelected && parent->processor.isPatchModified(info->iInList);
      g.setColour(rowIsSelected ? juce::Colours::black
                  : modified    ? juce::Colour(0xffffb238)
                                : juce::Colours::white);

      juce::String str = juce::String(info->name, info->nameLength);
      g.drawFittedText(str, {5, 0, width, height - 2},
                       juce::Justification::left, 1);

      g.setColour(juce::Colours::white.withAlpha(0.4f));
      g.drawRect(0, height - 1, width, 1);
    }

    void changeListenerCallback(juce::ChangeBroadcaster *source) override {
      if (source == categoriesListModel) {
        groupI = categoriesListBox->getSelectedRow();
        parent->processor.status.selectedRom = groupI;
        owner->updateContent();
        owner->deselectAllRows();
        owner->repaint();
      }
    }

    // All 6 columns scroll together as one (Alan's request, 2026-09-07) instead of each having
    // its own independent scrollbar/position - they're all slices of the very same list, so
    // scrolling any one of them keeps the whole grid's rows vertically aligned across columns.
    // listWasScrolled() fires for both user-driven and programmatic position changes, so
    // parent->syncingScroll guards against the setVerticalPosition() calls below re-triggering
    // this same callback on every other column (infinite mutual recursion otherwise).
    void listWasScrolled() override {
      if (parent->syncingScroll || owner == nullptr)
        return;

      parent->syncingScroll = true;
      const double position = owner->getVerticalPosition();
      for (int i = 0; i < parent->activeColumns; i++)
        if (parent->patchesListBoxes[i] != owner)
          parent->patchesListBoxes[i]->setVerticalPosition(position);
      parent->syncingScroll = false;
    }

    void selectedRowsChanged(int lastRowSelected) override {
      if (!parent->processor.loaded) {
        return;
      }

      for (int i = 0; i < maxColumnsPool; i++) {
        if (lastRowSelected != -1 && parent->patchesListBoxes[i] != owner)
          parent->patchesListBoxes[i]->deselectAllRows();
      }

      int selected = owner->getSelectedRow() + startI;
      if (selected >= 0 &&
          selected < parent->processor.patchInfoPerGroup[groupI].size())
      {
        parent->processor.setCurrentProgram(
            parent->processor.patchInfoPerGroup[groupI][selected]
                ->iInList);
        if (parent->onPatchSelected)
          parent->onPatchSelected();
      }
    }

    // Right-click -> "Send to Performance Part N" (Alan's request, 2026-09-07/08). JUCE's
    // ListBox selects the clicked row (triggering selectedRowsChanged() above, i.e. also loading
    // it into the main single-patch editor) before calling this, even on a right-click - not
    // worth fighting for a standard ListBox short of a fully custom row component, and arguably
    // useful anyway (you can hear the patch while sending it to a part).
    //
    // Only offered for patches VirtualJVProcessor::isEligibleForPerformancePart() accepts (the
    // 195 ROM-native factory tones/rhythm-sets - see PerformancePart's own comment in
    // PluginProcessor.h for why Card/expansion/User patches aren't there yet), and only for the
    // 7 tone Parts or the 1 fixed Rhythm Part (index 7) matching the clicked patch's own kind -
    // real hardware Parts can't mix the two.
    void listBoxItemClicked(int row, const juce::MouseEvent &e) override {
      if (!e.mods.isPopupMenu())
        return;
      parent->showSendMenuForPatch(patchIndexForRow(row));
    }

    // Index into processor.patchInfos[] of the patch shown on `row`, or -1 if out of range or
    // ROMs aren't loaded. Shared by the right-click path above and PatchBrowser's touch
    // long-press (see PatchBrowser::mouseDown()'s own comment).
    int patchIndexForRow(int row) const {
      if (!parent->processor.loaded)
        return -1;
      const int selected = row + startI;
      if (row < 0 || selected < 0 ||
          selected >= (int)parent->processor.patchInfoPerGroup[groupI].size())
        return -1;
      return parent->processor.patchInfoPerGroup[groupI][selected]->iInList;
    }

    int groupI = 0;
    int startI;
    int endI;
    juce::ListBox *owner = nullptr;
    juce::ListBox *categoriesListBox;
    CategoriesListModel *categoriesListModel;
    PatchBrowser *parent;
  };

  // "Send to Performance Part N" / "Send to Sequencer" popup for the patch at
  // processor.patchInfos[index] (no-op for -1 or a patch VirtualJVProcessor::
  // isEligibleForPerformancePart() rejects). Reached by right-click on desktop
  // (PatchesListModel::listBoxItemClicked) and by a long-press on touch screens (mouseDown()
  // below) - same menu either way.
  void showSendMenuForPatch(int index);

  // Touch long-press -> the same menu a right-click opens (Alan's request, 2026-09-21: Android
  // has no right mouse button, so "Send to Performance Part N" was unreachable there). Listens
  // to mouse events of every patch ListBox's nested row components (addMouseListener(this,
  // true) in the constructor - ListBoxModel never sees raw mouseDown). Touch sources only: a
  // real mouse already has right-click, and holding the left button would be a surprise.
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;

  PatchesListModel *patchesListModels[maxColumnsPool];
  juce::ListBox *patchesListBoxes[maxColumnsPool];

  // Guards PatchesListModel::listWasScrolled() against the mutual-recursion its own
  // setVerticalPosition() calls would otherwise cause (see that method's own comment).
  bool syncingScroll = false;

  // Bumped on release / move past the tap threshold / each new touch-down, so the deferred
  // callAfterDelay() of an earlier gesture recognises it is stale (same scheme as
  // JivSequencerPanel::longPressToken).
  int longPressToken = 0;
  juce::Point<float> longPressStartPos;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PatchBrowser)
};
