/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <vector>
#include <JuceHeader.h>
#include "emulator/mcu.h"
#include "rom.h"
#include "sequencer/JivSequencerEngine.h"
#include "sequencer/JivSequencerHost.h"
#include "sequencer/SequencerViewMenu.h"
#include "ui/widgets/VirtualKeyboardHost.h"

constexpr int NUM_EXPS = romCount - 6;

// patchInfos[]/patchInfoPerGroup capacity for ROM-sourced patches, and a reserved tail for
// user-saved ones (Alan's request, 2026-09-04: Save As + a single flat "User" bank). Both are
// plain arrays/reserved vectors sized once at compile time rather than fully dynamic
// containers, matching how the ROM side of this same array already works - see
// VirtualJVProcessor::refreshUserPatches() for how that tail gets populated from disk.
constexpr int romPatchCapacity = 192 + 256 * NUM_EXPS;
constexpr int userPatchCapacity = 256;
// patchInfoPerGroup's last group, appended once in the constructor right after the ROM-based
// ones (which occupy indices 0..NUM_EXPS - see the constructor's own loop).
constexpr int userGroupIndex = NUM_EXPS + 1;

//==============================================================================
/**
*/

class VirtualJVEditor;

class VirtualJVProcessor  : public juce::AudioProcessor, public VirtualKeyboardHost, public JivSequencerHost,
                             public juce::AsyncUpdater
{
public:
    //==============================================================================
    VirtualJVProcessor();
    ~VirtualJVProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    void sendSysexParamChange(uint32_t address, uint8_t value);

    std::vector<std::string> readMultisampleNames(uint8_t romIdx);

    // Discards edits made to the currently-loaded patch since it was picked, restoring it to
    // the snapshot taken when setCurrentProgram() last loaded it from ROM.
    void revertCurrentPatch();

    // True if patchInfos[index] differs from its own pristine ROM/file source - Alan's
    // request, so PatchBrowser can colour modified patches differently in the list. Computed
    // on demand (a bounded memcmp, at most 2684 bytes) rather than tracked with a side flag
    // that every edit call site would need to remember to set: for the currently-loaded patch,
    // compares the live edit buffer against originalPatchSnapshot/originalDrumsSnapshot; for
    // any other patch, compares its entry in editedPatchCache/editedDrumsCache (if any) against
    // patchInfos[index]'s own pristine bytes - both already exist for revertCurrentPatch() and
    // the switch-away-and-back cache respectively, so this adds no new state.
    bool isPatchModified(int index) const;

    //==============================================================================
    // User patch library (Alan's request, 2026-09-04): one flat "User" bank, one file per
    // patch, so a modified patch/rhythm-set can be saved under a new name instead of only
    // living in the current DAW project/session. `file` should be under userPatchesDir() with
    // a ".jvp" extension - PatchBrowser's Save As... button uses a juce::FileChooser in save
    // mode to pick both the name and (implicitly) that. Returns false if the write failed.
    bool saveCurrentPatchAs(const juce::File &file);

    // Imports the patches of a JV-880 SysEx dump (.syx, or the SysEx events of a .mid) into the User bank
    // (userPatchesDir(), one .jvp per patch) - GitHub issue #4. Each patch is sent over MIDI to a scratch
    // slot of the firmware's Internal Patch Memory (the last one, restored afterwards) and read back, so the
    // firmware itself does the conversion into its own record format. Meant for a worker thread: `waitMs`
    // must let the audio thread run for that long (Thread::sleep), `progress` (index, count, name) returns
    // false to cancel. Tone patches only; Performances, Rhythm sets and System data are counted and ignored.
    struct SysexImportReport
    {
        int imported = 0;
        int failed = 0;
        juce::StringArray names;
        juce::StringArray notes;
        juce::String error;
    };
    SysexImportReport importSysexFile(const juce::File &file,
                                      const std::function<bool(int, int, const juce::String &)> &progress,
                                      const std::function<void(int)> &waitMs);
    static juce::File userPatchesDir();
    int numUserPatches = 0; // how many of the reserved patchInfos[] tail slots are populated

    //==============================================================================
    // VirtualKeyboardHost - feeds the on-screen VirtualKeyboard through the same
    // MidiMessageCollector path a real MIDI IN port would use, merged into the host's own
    // MIDI in processBlock(). Most of this is session-only (not part of DataToSave/getState
    // Information: that struct is a fixed-size raw memcpy blob loaded back with memcpy(&status,
    // data, sizeof(DataToSave)) regardless of the actual saved size, so appending fields to it
    // would read past the end of an older, smaller saved blob). The PC-keyboard-input toggle and
    // its QWERTY/AZERTY layout are the exception (Alan's request) - persisted to a small XML
    // file of their own under the same JV880 app-data folder as the ROMs, sidestepping that
    // blob entirely - see loadPersistedKeyboardSettings()/savePersistedKeyboardSettings() in the
    // .cpp.
    void injectTestNote(int channel, int note, float velocity, bool on) override;
    int getKeyboardMidiChannel() const override { return keyboardMidiChannel; }
    void setKeyboardMidiChannel(int channel) override { keyboardMidiChannel = juce::jlimit(1, 16, channel); }
    bool getMidiRemap() const override { return keyboardMidiRemap; }
    void setMidiRemap(bool remap) override { keyboardMidiRemap = remap; }
    bool getKeyboardPcInputEnabled() const override { return keyboardPcInput; }
    void setKeyboardPcInputEnabled(bool enabled) override;
    int getKeyboardPcLayout() const override { return keyboardPcLayout; }
    void setKeyboardPcLayout(int layout) override;
    int getKeyboardNumOctaves() const override { return keyboardNumOctaves; }
    void setKeyboardNumOctaves(int numOctaves) override { keyboardNumOctaves = juce::jlimit(1, 4, numOctaves); }
    bool isNoteActive(int note) const override {
        return note >= 0 && note < 128 && noteActiveTable[static_cast<size_t>(note)].load();
    }

    //==============================================================================
    // Plugin-side output trim (Settings tab) - see SettingsTab.h's own comment on why this
    // isn't a real JV-880 parameter and isn't part of DataToSave.
    float getMasterVolume() const { return masterVolume; }
    void setMasterVolume(float v) { masterVolume = juce::jlimit(0.0f, 1.0f, v); }

    //==============================================================================
    // ROM folder configuration (Alan's request, 2026-09-08): the default ROM location - the
    // OS's own per-user app-data folder - isn't obvious to find on every machine, and "I copied
    // the ROM files and it still didn't work" was hard to self-diagnose (no visible current
    // path, no way to point elsewhere or retry without restarting). This exposes that as a
    // Settings-tab control instead: getRomsFolder() for the resolved path to show, setRoms
    // FolderOverride() to point at a different one (persisted; empty juce::File{} resets to the
    // default), retryLoadRoms() to attempt loading again in place. See rom.h's own override
    // functions for where this actually lives, and attemptLoadRoms()'s comment for why a retry
    // is only meaningful (and only touches loadedRoms/romInfos) while not yet loaded.
    static juce::File getRomsFolder();
    void setRomsFolderOverride(const juce::File &dir);
    bool retryLoadRoms();

    //==============================================================================
    // Top-of-window display mode (Alan's request, 2026-09-08 - "fais pareil que pour le D110"):
    // LcdOnly is the classic small dot-matrix strip alone (unchanged default behaviour);
    // PanelCompact/PanelFull replace it with a PanelSkin (see PanelSkin.h) showing the whole photo
    // -based front panel, with that same live LCD embedded into the photo's own LCD opening -
    // VirtualJVEditor::resized() reads this to decide what to lay out up top. Persisted like the
    // ROM folder override just above (a small file under the JV880 app-data folder, not
    // DataToSave), loaded once at construction; setDisplayMode() also pushes the change to the
    // active editor, same convention as setPerformanceModeEnabled().
    enum class DisplayMode { LcdOnly = 0, PanelCompact = 1, PanelFull = 2 };
    DisplayMode displayMode = DisplayMode::LcdOnly;
    void setDisplayMode(DisplayMode mode);

    struct PatchInfo
    {
        const char* name;
        const char* ptr;
        int nameLength;
        int expansionI; // 0xff: no expansion
        int patchI;
        bool present = false;
        bool drums = false;
        int iInList;
    };

    //==============================================================================
    // Performance mode v2 (Alan's request, 2026-09-08): drives the JV-880 firmware's OWN native
    // 8-part Performance Play mode via documented SysEx DT1 writes into the SAME single `mcu`
    // engine Patch mode uses - no more parallel engines (the previous 4-engine clone described in
    // CLAUDE.md's "Mode Performance" section, ~4x DSP cost). Address map transcribed from
    // ~/src/D110/edisyn/edisyn/synth/rolandjv880/RolandJV880Multi.java (Sean Luke's Edisyn,
    // matching the real JV-880 MIDI implementation) and confirmed empirically (LCD screenshot +
    // RAM search + audio render - see CLAUDE.md).
    //
    // A Performance Part references a factory patch by its real Roland Patch Memory bank/number
    // (0 = Internal, 2 = Preset A ["Internal A" in this project's own Browse-tab naming], 3 =
    // Preset B ["Internal B"]) rather than by patchInfos[] index or inline bytes: the real
    // firmware only ever stores a *reference* into Patch Memory for a Part, never patch data
    // inline. That covers all 192 ROM-native tone patches and all 3 ROM-native rhythm sets this
    // project already exposes (patchInfos[] index 0..194 - see performancePatchMapping() in the
    // .cpp) - Card/expansion-ROM patches and custom-saved .jvp User patches aren't assignable to
    // a Part yet, since that would additionally require writing them into the real writable
    // Internal Patch Memory bank over SysEx (a further, not-yet-done step - see CLAUDE.md).
    struct PerformancePart
    {
        bool present = false;   // false = left at Init Tone, not addressed by sendPatchToPerformancePart()
        bool isRhythm = false;  // fixed true for part index 7 (the 8th/last part), false otherwise
        uint8_t bank = 2;       // 0 = Internal, 2 = Preset A, 3 = Preset B (see performancePatchMapping())
        uint8_t number = 0;     // 0-63
        char name[16] = {0};    // display name, cached at assignment time
        // 0xff: ROM-native (Internal/Preset A/B) or none - matches PatchInfo::expansionI's own
        // convention. Tracked here (Alan's request, 2026-09-08) purely so sendPatchToPerformance
        // Part() can warn - non-blockingly - when a Performance ends up mixing tone Parts from
        // two different expansion boards: this single engine can only have one waverom_exp
        // loaded at a time (see injectCustomPatchIntoInternalMemory()'s own comment), so whichever
        // board was assigned most recently silently "wins" and the other Part(s) will sound
        // wrong - a real hardware limitation, not a bug, but worth flagging since it's easy to
        // trigger by accident from Browse's per-patch right-click menu.
        uint8_t expansionI = 0xff;
        int midiChannel = 1;    // 1-16 - real firmware Parts always answer a single channel, no "all"
        int level = 100;        // 0-127
        int pan = 64;           // 0-127, 64 = centre (matches the firmware's own partpan field range)
        bool enabled = true;    // false = receiveswitch off (Part stays configured but silent)
    };

    static constexpr int kNumPerformanceParts = 8; // matches the real hardware: 7 Patch parts + Part 8 fixed to Rhythm
    PerformancePart performanceParts[kNumPerformanceParts];
    char performanceName[13] = "Performance "; // 12 chars + NUL, matches the firmware's own name field width
    bool performanceModeEnabled = false;

    void sendPatchToPerformancePart(int patchInfoIndex, int partIndex);
    void clearPerformancePart(int partIndex);
    void setPerformancePartParams(int partIndex, int midiChannel, int level, int pan, bool enabled);
    void setPerformanceModeEnabled(bool enabled);
    void setPerformanceName(const juce::String &name);

    // patchInfos[] index 0..194 only (the ROM-native factory tones/rhythm-sets) - see
    // performancePatchMapping() in the .cpp for why everything else is excluded.
    bool isEligibleForPerformancePart(int patchInfoIndex) const;

    static juce::File performancesDir();
    bool savePerformanceAs(const juce::File &file);
    void refreshPerformanceBank();
    void loadPerformance(int bankIndex);

    struct PerformanceBankInfo
    {
        juce::String name;
        juce::File file;
    };
    std::vector<PerformanceBankInfo> performanceBank;

    // Session persistence (Alan's request, 2026-09-07): unlike the rest of the Performance
    // state, the 8 in-progress parts + mode-enabled flag now survive an app/plugin restart -
    // written to their own small file (same reasoning as keyboardSettingsFile(): DataToSave is
    // a fixed-size blob that can't safely grow). This is deliberately NOT the same file a "Save
    // As..." performance uses, and lives outside performancesDir() so it never shows up in the
    // Performance Bank list. Loaded once at construction, saved after every mutation.
    static juce::File performanceSessionFile();
    void savePerformanceSessionState();
    void loadPerformanceSessionState();

    struct DataToSave
    {
        int8_t masterTune{0};
        bool reverbEnabled{1};
        bool chorusEnabled{1};

        int currentExpansion{0};
        bool isDrums{false};

        uint8_t patch[0x16a] = {0};
        uint8_t drums[0xa7c] = {0};

        int selectedTab{0};
        int selectedRom{-1};
        int selectedPatch{-1};

        uint8_t selectedLCDColor{0};
    };

    DataToSave status;
    MCU *mcu;
    // Zero-initialised so the nullptr checks in setStateInformation()/setCurrentProgram() (an
    // expansion index this build doesn't have that ROM loaded for - a saved DAW project on a
    // different machine, or a user patch saved while a since-removed expansion was active) see
    // a reliable nullptr rather than an indeterminate pointer for the slots the constructor's
    // own loading loop never assigns.
    const uint8_t* expansionsDescr[NUM_EXPS] = {nullptr};
    PatchInfo patchInfos[romPatchCapacity + userPatchCapacity] = {0};
    std::vector<std::vector<PatchInfo*>> patchInfoPerGroup;
    int totalPatchesExp = 0;

    std::array<uint8_t*, romCount> loadedRoms = {0};
    std::vector<std::string> ownedNames;
    bool loaded = false;

    // CriticalSection, not SpinLock: this is held across most of processBlock() (see its own
    // comment there), so a UI-thread caller can be waiting a while whenever DSP load is high -
    // a SpinLock would busy-wait that whole time, pegging a core and starving the audio thread
    // it's waiting on (Alan's report, 2026-09-22: freezes, unresponsive buttons, ~90% CPU).
    // CriticalSection sleeps instead.
    juce::CriticalSection mcuLock;

    // DSP load meter (Alan's request, 2026-09-07) - measures the proportion of each audio block's
    // real-time budget spent inside processBlock(), covering both Patch mode (1 engine) and
    // Performance mode (up to 4 engines) alike. Thread-safe/lock-free to read (internally atomic),
    // polled by SettingsTab on a Timer - see AudioProcessLoadMeasurer's own header for details.
    juce::AudioProcessLoadMeasurer dspLoadMeasurer;

    // --- Sequencer (Standalone build only, Alan's request, 2026-09-09) ---------------------
    // A D-20-style multitrack step sequencer, ported from the D-110 emulator's own embedded
    // sequencer drawer (~/src/D110/d110-vst-emulator, docs/sequencer.md) - see
    // Source/sequencer/JivSequencerEngine.h's own header comment for the porting story. Always
    // exists as a plain member in every build (JivSequencerEngine has no plugin-format
    // dependency, and this project's Standalone/VST3/AU/LV2 builds share one compilation - see
    // SettingsTab.cpp's own comment on JucePlugin_Build_Standalone), but the UI drawer that
    // would ever start/arm/play it is only ever created in VirtualJVEditor when
    // wrapperType == wrapperType_Standalone (same runtime-only gating already used for the
    // Audio/MIDI Settings block) - so in VST3/AU/LV2 this member just sits there, permanently
    // idle, never driven by anything.
    //
    // Each of the 8 tracks IS one of the 8 Performance Parts (kNumPerformanceParts above) in
    // the sense that it drives that Part's channel/patch/level/pan - but its OWN sound/channel
    // choice is stored independently of the Part's live state (Alan's request, 2026-09-09:
    // "les sons assignés au mode performances devraient être décorrélés des sons assignés au
    // séquenceur"), so changing a Part's patch from the Performance tab doesn't silently change
    // what a song plays back, and vice versa. Assigned from PatchBrowser's "Send to Sequencer"
    // submenu (setSequencerTrackPatch() below, NOT sendPatchToPerformancePart() directly - see
    // that function's own call site in PatchBrowser.h), only actually pushed into the live
    // Performance Part at the PLAY/REC edge - see handleAsyncUpdate() below. Deliberately no
    // *numeric* Program Change UI (JivSequencerHost::supportsProgramChange() stays at its
    // default false) - see JivSequencerHost.h's own top comment for why that's a real design
    // choice, not a missing feature. Channel, unlike patch/volume/pan, is NOT decoupled -
    // editing it from the sequencer (supportsTrackChannelEdit()) writes straight through to
    // the Part's own live receivechannel, same as editing it from the Performance tab would
    // (Alan's own call: "cela le répercutera dans le mode performance").
    jivseq::JivSequencerEngine sequencerEngine;

    jivseq::JivSequencerEngine &getSequencer() override { return sequencerEngine; }
    void exportSequencerSongs(const juce::File &file) override;
    void importSequencerSongs(const juce::File &file) override;
    void midiPanic() override;
    juce::File getLastDialogDir() const override { return lastSequencerDialogDir; }
    void setLastDialogDir(const juce::File &dir) override { lastSequencerDialogDir = dir; }

    // Stored on the track, same decoupled-until-PLAY treatment as patch/volume/pan below - NOT
    // written straight into the live PerformancePart (Alan's explicit correction, 2026-09-09:
    // "je veux que lors du PLAY du seq, cela assigne le canal 5 sur la Part 3", not
    // immediately). See handleAsyncUpdate() for the actual push, and the constructor's channel
    // source lambda for why the CH readout already shows this override once set (falls back to
    // the Part's own live channel only while unset).
    bool supportsTrackChannelEdit() const override { return true; }
    void setTrackChannel(int track, int channel) override { sequencerEngine.setTrackChannelOverride(track, channel); }

    bool supportsTrackVolumePan() const override { return true; }
    bool supportsTrackVolumePanForTrack(int) const override { return true; } // Rhythm included - real LEVEL/PAN same as any Part
    int getTrackVolume(int track) const override { return sequencerEngine.getTrackVolume(track); }
    void setTrackVolume(int track, int volume) override { sequencerEngine.setTrackVolume(track, volume); }
    int getTrackPan(int track) const override { return sequencerEngine.getTrackPan(track); }
    void setTrackPan(int track, int pan) override { sequencerEngine.setTrackPan(track, pan); }
    // "Now playing" hint for the CC Change dialog's placeholder - the Part's own live LEVEL/PAN,
    // same reasoning as the D-110 host's own getTrackProgramHint() this was ported alongside.
    int getTrackVolumeHint(int track) const override;
    int getTrackPanHint(int track) const override;

    // Called by PatchBrowser's "Send to Sequencer -> Part N" (Alan's request, 2026-09-09) -
    // resolves patchInfoIndex into the same bank/number/expansionI/name identity
    // PerformancePart itself uses (see sendPatchToPerformancePart()'s own comment) and stores
    // it on the track, WITHOUT touching the live performanceParts[] - see this class's own
    // sequencerEngine comment above.
    void setSequencerTrackPatch(int track, int patchInfoIndex);
    juce::String getTrackPatchName(int track) const override;

    // The SYNC button (top-right of the sequencer drawer, Alan's request, 2026-09-10: "comme on
    // a fait sur le D110") - JivSequencerPanel.cpp's own showResyncInfo()/confirmCaptureLivePatch()
    // already implement the UI for both directions, ported unmodified and simply never reachable
    // until now (supportsCaptureLivePatch() stayed at its false default). "Program Change" in
    // that ported UI text means "a track's assigned patch" here, same substitution
    // supportsProgramChange()'s own comment above already explains - see resyncProgramChanges()/
    // captureLivePatchIntoTracks() in the .cpp for what each direction actually moves.
    bool supportsCaptureLivePatch() const override { return true; }
    void captureLivePatchIntoTracks() override;
    void resyncProgramChanges() override;

    void ensurePerformanceMode() override {
      if (!performanceModeEnabled)
        setPerformanceModeEnabled(true);
    }

    // AsyncUpdater: the PLAY/REC-edge patch/volume/pan push detected in processBlock()
    // (audio thread) is deferred here (message thread) since applying a track's patch can mean
    // injectCustomPatchIntoInternalMemory()'s multi-megabyte waverom_exp copy - not remotely
    // safe to do on the audio thread. A few tens of milliseconds' latency between pressing
    // PLAY and a changed instrument actually sounding is the accepted tradeoff (see
    // processBlock()'s own comment).
    void handleAsyncUpdate() override;

    // Persisted enable/disable (Alan's request: "il faudra pouvoir le supprimer") - separate
    // small file, same convention as displayModeSettingsFile()/romFolderSettingsFile() (NOT
    // DataToSave - see that struct's own comment on why it can't grow). Disabling doesn't
    // discard any recorded songs, only hides the drawer and stops the transport (see
    // setSequencerEnabled()'s own .cpp comment) - re-enabling brings everything back exactly
    // as it was.
    bool sequencerEnabled = false;
    bool getSequencerEnabled() const { return sequencerEnabled; }
    void setSequencerEnabled(bool enabled);

    // Which of the three views the sequencer drawer shows - classic strip (JivSequencerPanel),
    // retro D-20-style LCD (JivSequencerRetroPanel) or piano roll (JivSequencerGridPanel). Stored
    // as the two mutually exclusive bools every host of the shared "Sequencer > Classic / Retro /
    // Grid" menu (SequencerViewMenu.h) exposes; setSequencerView() sets both at once, persists
    // them (with the grid row height) in sequencer_view.txt - same convention as
    // sequencer_enabled.txt - and tells the active desktop editor to swap its panel once. The
    // Android app re-reads them itself. Only ever a UI choice: all views drive the same engine.
    bool sequencerGridMode = false;
    bool sequencerRetroMode = false;
    bool getSequencerGridMode() const { return sequencerGridMode; }
    bool getSequencerRetroMode() const { return sequencerRetroMode; }
    void setSequencerGridMode(bool grid) { setSequencerView(grid ? seqview::View::grid : seqview::View::classic); }
    void setSequencerRetroMode(bool retro) { setSequencerView(retro ? seqview::View::retro : seqview::View::classic); }
    void setSequencerView(seqview::View view);

    // Retro view settings (JivSequencerHost), persisted in sequencer_retro.txt.
    juce::String retroKeyBindings;
    bool retroLcdCompactMode = false;
    juce::String getRetroKeyBindings() const override { return retroKeyBindings; }
    void setRetroKeyBindings(const juce::String &encoded) override;
    bool getRetroLcdCompactMode() const override { return retroLcdCompactMode; }
    void setRetroLcdCompactMode(bool compact) override;

    // Heights of the keyboard and sequencer panes in the desktop editor (dragged with the bars
    // between the panes, see VirtualJVEditor), persisted in layout_heights.txt.
    static constexpr int kDefaultKeyboardPaneH = 120;
    static constexpr int kDefaultSequencerPaneH = 347;
    int keyboardPaneH = kDefaultKeyboardPaneH;
    int sequencerPaneH = kDefaultSequencerPaneH;
    void setPaneHeights(int keyboard, int sequencer);

    int gridRowHeight = 0;
    int getGridRowHeight() const override { return gridRowHeight; }
    void setGridRowHeight(int pixels) override;
    // Same path as the on-screen keyboard's own notes (injectTestNote()), on the track's live
    // channel - lets the grid editor sound a note as it is placed or grabbed.
    void auditionTrackNote(int track, int note, int velocity, bool on) override;

    // Where the sequencer's own 4 song slots survive an app restart - standalone-only state
    // (see this whole block's own top comment), so unlike Performance sessions this has no DAW
    // project to also round-trip through; a plain file is the only persistence that makes
    // sense here. Reuses JivSequencerSongsFile's own .midiseq XML format (jivseq::
    // exportSongsFile/importSongsFile) at a fixed path instead of a user-chosen one - see
    // saveSequencerState()/loadSequencerState() in the .cpp for when these actually run.
    static juce::File sequencerStateFile();
    void saveSequencerState();
    void loadSequencerState();

    // Shared by loadSequencerState()'s own stale-index re-resolution and
    // captureLivePatchIntoTracks() below - a patch's real identity is its name/expansionI/isRhythm
    // triple (patchInfos[] index isn't stable across ROM/expansion set changes), so both need the
    // same "find the current index for this identity" lookup.
    int findPatchInfoIndexByIdentity(const juce::String &name, uint8_t expansionI, bool isRhythm) const;
    // Shared by handleAsyncUpdate()'s PLAY/REC-edge loop and resyncProgramChanges()'s manual
    // "Send" action - pushes one track's stored patch/channel/volume/pan into its live
    // PerformancePart right now. Message-thread only (see handleAsyncUpdate()'s own comment).
    void pushSequencerTrackToPerformance(int track);

private:
    // See getLastDialogDir()/setLastDialogDir() above.
    juce::File lastSequencerDialogDir;

    // Edge-detects sequencerEngine.isPlaying() in processBlock() (audio thread) to trigger
    // handleAsyncUpdate()'s patch/volume/pan push exactly once per PLAY/REC start, not every
    // block while playing.
    bool wasSequencerPlayingForPatchPush = false;

    // Metronome click synthesis state (ported from the D-110 project's own processBlock() -
    // see JivSequencerEngine::MetronomeClick), audible only when getMetronomeUseChannel10() is
    // false (the alternative - real note on/off pairs on the rhythm channel - just flows
    // through the normal MIDI path above, no extra state needed). Persists across blocks since
    // a single click's ~30ms decay outlives one audio block.
    std::vector<jivseq::JivSequencerEngine::MetronomeClick> sequencerClicks;
    int metronomeSamplesRemaining = 0;
    double metronomePhase = 0.0;
    double metronomeFreq = 1000.0;

    // VirtualKeyboard support - see the VirtualKeyboardHost overrides above.
    juce::MidiMessageCollector keyboardCollector;
    // Scratch buffer for the MIDI Remap pass in processBlock() - see its comment there.
    juce::MidiBuffer midiRemapScratch;
    std::array<std::atomic<bool>, 128> noteActiveTable{};
    int keyboardMidiChannel = 1;
    bool keyboardMidiRemap = false;
    bool keyboardPcInput = false;
    int keyboardPcLayout = 0;
    int keyboardNumOctaves = 2;
    float masterVolume = 1.0f;

    // revertCurrentPatch()'s own pristine copy - see setCurrentProgram()'s and
    // revertCurrentPatch()'s own comments.
    uint8_t originalPatchSnapshot[0x16a] = {0};
    uint8_t originalDrumsSnapshot[0xa7c] = {0};

    // In-session edit cache, keyed by patchInfos[] index (PatchInfo::iInList) - Alan's report,
    // 2026-09-04: picking a different patch and coming back silently discarded whatever had
    // just been edited, reloading the pristine ROM/file version instead. setCurrentProgram()
    // now saves the outgoing patch's live bytes here before loading the new one, and checks
    // here first (falling back to the pristine source) when loading any patch. Deliberately
    // NOT part of DataToSave/persisted to disk - this is "don't lose my place while I'm
    // comparing patches this session", not a substitute for Save As (which is explicit and
    // permanent) - so it starts empty every relaunch.
    int currentPatchIndex = -1;
    std::map<int, std::array<uint8_t, 0x16a>> editedPatchCache;
    std::map<int, std::array<uint8_t, 0xa7c>> editedDrumsCache;

    // Backing storage for the user patches refreshUserPatches() loads from disk - reserved
    // upfront to userPatchCapacity so the vectors never reallocate while patchInfos[] entries
    // hold raw pointers into them (a tone patch's PatchInfo::name points directly at its
    // buffer's first byte, same convention as a ROM tone patch; a rhythm set's PatchInfo::ptr
    // points at its buffer, since Rhythm has no embedded name field to double as one - see
    // dataStructures.h).
    void refreshUserPatches();
    std::vector<std::array<uint8_t, 0x16a>> userToneBuffers;
    std::vector<std::array<uint8_t, 0xa7c>> userDrumBuffers;
    std::vector<std::string> userPatchNames;

    // Performance mode v2 internals - pushes one Part's (or Common's) record to the single
    // `mcu` engine over SysEx DT1. Callers must already hold mcuLock. See PerformancePart's own
    // comment above for the address map and its sourcing.
    void pushPerformanceCommonToEngine();
    void pushPerformancePartToEngine(int partIndex);
    // Low-level Roland DT1 send (F0 41 10 46 12 <4-byte address> <data...> <checksum> F7) -
    // sendSysexParamChange() is just this with a 1-byte payload. Callers must already hold
    // mcuLock.
    void sendSysexBlock(uint32_t address, const uint8_t *data, size_t length);

    // Unlocks expansion-ROM/User tone patches for a Performance Part (Alan's request,
    // 2026-09-08): a Part can only reference the firmware's real Patch Memory by bank/number,
    // never inline bytes - the 195 ROM-native patches (performancePatchMapping()) already live
    // there verbatim, but anything else has to actually be copied in first. Confirmed empirically
    // (see CLAUDE.md) that the real writable "Internal" Patch Memory bank is 64 contiguous 0x16a-
    // byte slots at nvram[0x0d70..0x67f0) - slot 0 (0x0d70) is what this project already calls
    // "Patch Temp" (setCurrentProgram()'s own target), so slots 1..7 are reserved here, one per
    // tone Part, for whichever expansion/User patch that Part currently plays - plain memcpy,
    // same technique setCurrentProgram() already uses, no SysEx involved. Rhythm sets (Part 8)
    // aren't supported yet - real hardware stores those in a different, per-note memory area.
    // Callers must already hold mcuLock.
    void injectCustomPatchIntoInternalMemory(int patchInfoIndex, int internalSlot);

    // Everything the constructor used to do unconditionally from "ROMs preloaded OK" onward
    // (mcu->startSC55(), building patchInfos[]/patchInfoPerGroup, refreshUserPatches(),
    // refreshPerformanceBank(), loadPerformanceSessionState(), setting loaded=true) - factored
    // out (Alan's request, 2026-09-08) so retryLoadRoms() can re-run the exact same sequence
    // after the user fixes the ROM folder, instead of requiring an app/host restart. Guarded by
    // `if (loaded) return true;` at the top - safe to call repeatedly. Deliberately NOT used to
    // support switching ROM folders on an ALREADY-loaded engine: patchInfos[] entries hold raw
    // pointers into loadedRoms/expansionsDescr, performanceParts/status.patch may already
    // reference them, and a live audio thread may be reading mcu concurrently - unwinding all of
    // that safely is a bigger job than "fresh install, ROMs not found yet" warranted. See
    // setRomsFolderOverride()'s own comment for the same boundary from the other side.
    bool attemptLoadRoms();

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VirtualJVProcessor)
};
