// Android Standalone build of the emulator (Alan's request, 2026-09-09 - "comme on a fait pour
// le D110"), same shape as the D-110 Android port this was copied from
// (~/src/D110/d110-vst-emulator/android, see its own docs/android.md and
// .claude/dev-notes/android.md for the lessons this file leans on): one app, no plugin wrapper,
// CMake target `jiv881Android`, Gradle module `android/app`, app id `com.jiv881.android`.
//
// The real firmware/native core (VirtualJVProcessor, unchanged), the real photographed front
// panel (PanelSkin, kCompact variant - unlike D110Panel this one already self-scales to
// whatever width it's given, see PanelSkin::resized()/heightForWidth(), so no parent
// setTransform trick is needed here), the on-screen keyboard (VirtualKeyboard, which also
// already draws its own octave +/- buttons - no separate Options control needed for that), the
// patch browser (PatchBrowser, replacing the D110 port's SoundbankBrowser - JV-880 patches come
// from ROM/expansion boards, not imported SysEx banks, so there is no soundbank-import flow
// here) and the sequencer (JivSequencerPanel) - all reused unchanged from the desktop plugin.
// Deliberately NOT included: the tabbed extended editor (Common/Tone/Rhythm/Settings/Interface/
// Performance tabs) - same "as simple as possible" scope call the D110 port made, and nothing
// here has needed it. PluginEditor.cpp is still linked in (see app/CMakeLists.txt's own
// comment): VirtualJVProcessor::createEditor() references VirtualJVEditor's constructor, so its
// whole dependency graph has to be present at link time even though this app never calls it.
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "AppModeIcons.h"
#include "ui/PanelSkin.h"
#include "ui/PatchBrowser.h"
#include "ui/widgets/VirtualKeyboard.h"
#include "sequencer/JivSequencerPanel.h"
#include "sequencer/JivSequencerGridPanel.h"
#include "sequencer/JivSequencerRetroPanel.h"
#include "sequencer/SequencerViewMenu.h"

// Browse view's own test-note button (Alan's request, 2026-09-21, same as the D-110 Android port's
// Soundbanks view: no on-screen keyboard is visible in Browse, so there was no way to hear a
// patch without leaving the screen). Plays while held, like a piano key: mouseDown -> note on,
// mouseUp -> note off. The pitch is set separately by the PITCH button next to it.
class HeldNoteButton : public juce::TextButton {
public:
	std::function<void()> onPress;
	std::function<void()> onRelease;

	void mouseDown(const juce::MouseEvent &e) override {
		juce::TextButton::mouseDown(e);
		if (onPress) onPress();
	}
	void mouseUp(const juce::MouseEvent &e) override {
		juce::TextButton::mouseUp(e);
		if (onRelease) onRelease();
	}
};

class MainComponent : public juce::Component, private juce::Timer {
public:
	// The app's own external files dir - the one location the native core can always read with
	// a plain filesystem path on Android (content:// SAF results can't be opened by a raw
	// FileInputStream - see chooseRomFolder()'s own comment). Same convention the D110 Android
	// port uses for its own romBringUpDir().
	static juce::File romBringUpDir() {
		return juce::File("/storage/emulated/0/Android/data/com.jiv881.android/files/roms");
	}

	MainComponent()
		: panelDisplay(processor, PanelSkin::Variant::kCompact),
		  keyboard(processor) {
		bringUpRoms();
		loadPersistedState();

		// No Settings tab exists in this app to flip the sequencer on - it's the whole reason
		// Alan asked for this port, so just turn it on unconditionally. Safe to call before
		// ROMs are loaded (loadSequencerState() just finds nothing to resolve yet); calling it
		// again once ROMs actually become available isn't needed - existing tracks keep
		// whatever they already had, and newly-assignable patches only matter once the user
		// visits Browse anyway.
		processor.setSequencerEnabled(true);
		// The piano-roll view's pitch rows default to the 14px mouse size, far too small for a
		// finger - give a phone a taller first-run default (the ROW button steps it from there,
		// and the choice is persisted by the processor).
		if (processor.getGridRowHeight() == 0)
			processor.setGridRowHeight(28);

		addAndMakeVisible(panelDisplay);
		addAndMakeVisible(keyboard);
		createPatchDependentViews();
		wireUpBrowseTestNote();

		// The hamburger and the mode switch are drawn in the sequencer's own button style
		// (AppModeIcons.h) so they look the same in every view, including the two sequencer
		// panels that carry their own copies in their transport rows (onModeButton below).
		menuButton.onClick = [this] { showMainMenu(); };
		addAndMakeVisible(menuButton);
		modeButton.onClick = [this] { cycleMode(); };
		addAndMakeVisible(modeButton);

		statusLabel.setJustificationType(juce::Justification::centredRight);
		statusLabel.setColour(juce::Label::textColourId, juce::Colours::white);
		updateStatus();
		addAndMakeVisible(statusLabel);

		// AudioProcessorPlayer is what JUCE's own desktop Standalone wrapper
		// (StandaloneFilterWindow) uses internally to bridge a real AudioProcessor to a live
		// device - same idea here, with no window chrome/audio-settings dialog to go with it.
		// Its audioDeviceAboutToStart() is what actually calls processor.prepareToPlay().
		// Registered as both the audio callback AND every MIDI input device's callback (it
		// implements MidiInputCallback too) - the same object routes MIDI input straight into
		// processBlock()'s buffer, no bespoke bridge of our own needed.
		deviceManager.initialiseWithDefaultDevices(0, 2);
		player.setProcessor(&processor);
		deviceManager.addAudioCallback(&player);
		scanForMidiInputs();
		// android.media.midi has no "device changed" callback exposed through JUCE, only a
		// snapshot list (JUCE MidiInput::getAvailableDevices()) - polled here rather than once,
		// so a USB keyboard plugged in after launch is picked up too. Same technique the D110
		// Android port uses for its own scanForMidiInputs().
		startTimer(2000);

		setSize(900, 500);
	}

	~MainComponent() override {
		saveState();
		stopTimer();
		deviceManager.removeAudioCallback(&player);
		player.setProcessor(nullptr);
		for (const auto &id : knownMidiInputs) {
			deviceManager.removeMidiInputDeviceCallback(id, &player);
			deviceManager.setMidiInputDeviceEnabled(id, false);
		}
	}

	// See D110AndroidApp::suspended()'s own comment for why this has to be reachable from
	// outside a clean destructor chain too - Android can kill this whole process without
	// warning once backgrounded.
	void saveState() {
		juce::MemoryBlock data;
		processor.getStateInformation(data);
		stateFile().getParentDirectory().createDirectory();
		stateFile().replaceWithData(data.getData(), data.getSize());
		if (processor.getSequencerEnabled())
			processor.saveSequencerState();
	}

	void paint(juce::Graphics &g) override { g.fillAll(juce::Colour(0xff1e1e22)); }

	void resized() override {
		// Same fixed-guess safe-area handling the D110 Android port settled on (see its own
		// resized() comment for the full story of why a LIVE getDisplayForRect() lookup was
		// abandoned): a status-bar-sized inset on top always, and a nav-bar-sized inset on
		// whichever edge closes the current orientation (right in landscape, bottom in
		// portrait), with four independent manual overrides (Options menu) as an escape hatch
		// for devices that guess wrong (e.g. a tablet keeping its nav bar bottom-anchored even
		// in landscape).
		auto area = getLocalBounds();
		const bool isLandscape = getWidth() > getHeight();
		constexpr int kStatusBarInset = 32;
		constexpr int kNavBarInset = 64;
		juce::BorderSize<int> insets;
		if (navTop || navBottom || navLeft || navRight) {
			insets = {navTop ? kStatusBarInset : 0, navLeft ? kNavBarInset : 0,
			          navBottom ? kNavBarInset : 0, navRight ? kNavBarInset : 0};
		} else {
			insets = isLandscape ? juce::BorderSize<int>(kStatusBarInset, 0, 0, kNavBarInset)
			                     : juce::BorderSize<int>(kStatusBarInset, 0, kNavBarInset, 0);
		}
		area = insets.subtractedFrom(area);

		// The sequencer has its own transport row with a bar-menu button that already carries
		// the app's own hamburger (see onBarMenuButtonExtra below), so hide this row while it's
		// showing and give it the full height instead - same reasoning as the D110 port's own
		// grid-sequencer treatment. The retro view has no bar-menu button of its own, so there
		// the row stays (its hamburger is then the way back to this app's menu).
		const bool retroView = processor.getSequencerRetroMode();
		const bool inSequencer = currentView == View::Sequencer && !retroView;
		menuButton.setVisible(!inSequencer);
		modeButton.setVisible(!inSequencer);
		modeButton.setMode(currentMode());
		statusLabel.setVisible(!inSequencer);
		// Decided outside the strip block, so they can never linger over another view.
		const bool inBrowse = currentView == View::Browse;
		testNoteButton.setVisible(inBrowse);
		holdButton.setVisible(inBrowse);
		pitchButton.setVisible(inBrowse);
		if (!inSequencer) {
			// Same proportions as the sequencer's own bar-menu button (40 tall, rowWidth * 0.042 - 4
			// wide, flush top-right) so the strip looks like the sequencer's transport in every view.
			auto row = area.removeFromTop(40);
			const int buttonW = juce::jmax(28, juce::roundToInt(row.getWidth() * 0.042f - 4.0f));
			menuButton.setBounds(row.removeFromRight(buttonW));
			modeButton.setBounds(row.removeFromRight(buttonW));
			// NOTE / HOLD / PITCH, leftmost on this same row, only in Browse (see
			// wireUpBrowseTestNote()) - the status text keeps whatever is left.
			if (inBrowse) {
				const int btnW = juce::jmax(64, juce::roundToInt(row.getWidth() * 0.2f));
				testNoteButton.setBounds(row.removeFromLeft(btnW).reduced(2, 0));
				holdButton.setBounds(row.removeFromLeft(btnW).reduced(2, 0));
				pitchButton.setBounds(row.removeFromLeft(btnW).reduced(2, 0));
			}
			statusLabel.setBounds(row.reduced(8, 0));
		}

		panelDisplay.setVisible(currentView == View::Keyboard);
		keyboard.setVisible(currentView == View::Keyboard);
		patchBrowser->setVisible(currentView == View::Browse);
		// Classic strip, retro LCD or piano roll, per processor.getSequencerRetroMode()/
		// getSequencerGridMode() (all three exist; only the chosen one is ever shown).
		const bool pianoRoll = !retroView && processor.getSequencerGridMode();
		const bool inSeq = currentView == View::Sequencer;
		sequencerPanel->setVisible(inSeq && !retroView && !pianoRoll);
		sequencerGridPanel->setVisible(inSeq && pianoRoll);
		sequencerRetroPanel->setVisible(inSeq && retroView);

		if (currentView == View::Browse) {
			patchBrowser->setBounds(area);
		} else if (currentView == View::Sequencer) {
			sequencerPanel->setBounds(area);
			sequencerGridPanel->setBounds(area);
			sequencerRetroPanel->setBounds(area);
		} else {
			const int panelH = juce::roundToInt(panelDisplay.heightForWidth((float)area.getWidth()));
			panelDisplay.setBounds(area.removeFromTop(panelH));
			// Giving it the WHOLE remaining area (as landscape's wide-but-short leftover space
			// warrants) made portrait's tall leftover space stretch the keys absurdly high
			// (Alan's report) - VirtualKeyboard::kRefH (120, the desktop reference height) is
			// too short for comfortable touch targets, so this caps it at a modest multiple of
			// that instead of either extreme.
			const int kbH = juce::jmin(area.getHeight(), juce::roundToInt(VirtualKeyboard::kRefH * 2.2f));
			keyboard.setBounds(area.removeFromTop(kbH));
		}
	}

private:
	enum class View { Keyboard, Browse, Sequencer };

	// (Re)creates the two views whose content is only meaningful once ROMs are loaded -
	// PatchBrowser snapshots processor.patchInfos[] at construction with no live-refresh hook
	// of its own, and the sequencer's track patches are resolved against that same table (see
	// loadSequencerState()'s own comment). Called once from the constructor (before ROMs are
	// necessarily present - a fresh install shows empty lists until "Choose ROM files..."
	// succeeds) and again from retryRoms() the moment loading actually succeeds.
	void createPatchDependentViews() {
		if (patchBrowser) removeChildComponent(patchBrowser.get());
		patchBrowser = std::make_unique<PatchBrowser>(processor);
		addChildComponent(*patchBrowser);
		patchBrowser->onPatchSelected = [this] { retriggerHeldTestNote(); };

		if (sequencerPanel) removeChildComponent(sequencerPanel.get());
		sequencerPanel = std::make_unique<JivSequencerPanel>(processor);
		addChildComponent(*sequencerPanel);
		// See buildAppMenu()'s own comment: the sequencer hides the app's own hamburger row to
		// get its full height, so its bar-menu button becomes the only way back to it.
		sequencerPanel->onBarMenuButtonExtra = [this](juce::PopupMenu &m) { buildAppMenu(m); };
		sequencerPanel->onModeButton = [this] { cycleMode(); };
		sequencerPanel->modeButtonMode = [this] { return int(currentMode()); };

		// Same again for the piano-roll view (its transport row has the identical bar-menu
		// button, fed the same app menu - which is also where it is switched back to the strip).
		if (sequencerGridPanel) removeChildComponent(sequencerGridPanel.get());
		sequencerGridPanel = std::make_unique<JivSequencerGridPanel>(processor);
		addChildComponent(*sequencerGridPanel);
		sequencerGridPanel->onBarMenuButtonExtra = [this](juce::PopupMenu &m) { buildAppMenu(m); };
		sequencerGridPanel->onModeButton = [this] { cycleMode(); };
		sequencerGridPanel->modeButtonMode = [this] { return int(currentMode()); };

		if (sequencerRetroPanel) removeChildComponent(sequencerRetroPanel.get());
		sequencerRetroPanel = std::make_unique<JivSequencerRetroPanel>(processor);
		addChildComponent(*sequencerRetroPanel);
	}

	// Browse view's NOTE (left: plays testNotePitch while pressed), HOLD (sustains it, and
	// re-strikes it on every newly picked patch so each sound's attack can be compared) and
	// PITCH (opens a slider to set testNotePitch) buttons - ported from the D-110 Android port's
	// Soundbanks view (wireUpSoundbankTestNote()). The note goes out exactly like a keyboard key:
	// on the keyboard's MIDI channel when remap is on, else on all 16 channels.
	void wireUpBrowseTestNote() {
		testNoteButton.setButtonText(juce::MidiMessage::getMidiNoteName(testNotePitch, true, true, 4));
		testNoteButton.onPress = [this] { sendTestNote(testNotePitch, 0.9f, true); testButtonNote = testNotePitch; };
		testNoteButton.onRelease = [this] {
			if (testButtonNote < 0) return;
			sendTestNote(testButtonNote, 0.0f, false);
			testButtonNote = -1;
		};
		addChildComponent(testNoteButton);

		holdButton.setButtonText("HOLD");
		holdButton.setClickingTogglesState(true);
		holdButton.onClick = [this] {
			if (holdButton.getToggleState()) startHeldTestNote();
			else stopHeldTestNote();
			updateHoldButtonColour();
		};
		addChildComponent(holdButton);
		updateHoldButtonColour();

		pitchButton.setButtonText("PITCH");
		pitchButton.onClick = [this] {
			auto *aw = new juce::AlertWindow("Test note pitch", {}, juce::AlertWindow::NoIcon);
			// A slider over the full MIDI range whose text box shows the note name. Applied
			// silently on "Set" (no live audition while dragging - the D-110 port found that
			// sounded artefacted). addCustomComponent() takes no ownership, so both are
			// deleted explicitly in the callback.
			auto *slider = new juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::TextBoxBelow);
			slider->setSize(280, 60);
			slider->setRange(0.0, 127.0, 1.0);
			slider->setValue(testNotePitch, juce::dontSendNotification);
			slider->textFromValueFunction = [](double v) {
				return juce::MidiMessage::getMidiNoteName(int(v), true, true, 4);
			};
			slider->updateText();
			aw->addCustomComponent(slider);
			aw->addButton("Set", 1, juce::KeyPress(juce::KeyPress::returnKey));
			aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
			aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, slider](int result) {
				if (result == 1) {
					testNotePitch = int(slider->getValue());
					testNoteButton.setButtonText(juce::MidiMessage::getMidiNoteName(testNotePitch, true, true, 4));
				}
				delete slider;
				delete aw;
			}));
		};
		addChildComponent(pitchButton);
	}

	void sendTestNote(int note, float velocity, bool on) {
		if (processor.getMidiRemap()) {
			processor.injectTestNote(processor.getKeyboardMidiChannel(), note, velocity, on);
		} else {
			for (int ch = 1; ch <= 16; ++ch) processor.injectTestNote(ch, note, velocity, on);
		}
	}

	void startHeldTestNote() {
		holdActive = true;
		heldNote = testNotePitch;
		sendTestNote(heldNote, 0.9f, true);
	}

	void stopHeldTestNote() {
		++retriggerToken;
		if (heldNote >= 0) sendTestNote(heldNote, 0.0f, false);
		holdActive = false;
		heldNote = -1;
		holdButton.setToggleState(false, juce::dontSendNotification);
		updateHoldButtonColour();
	}

	// While HOLD is on, picking another patch kills the held note and strikes it again on the
	// new sound. Delayed: loading a patch can make the firmware reset (Patch <-> Rhythm switch,
	// or another expansion ROM - see VirtualJVProcessor::setCurrentProgram()), and a note sent
	// during that reset would be lost. retriggerToken coalesces rapid successive picks.
	void retriggerHeldTestNote() {
		if (!holdActive) return;
		if (heldNote >= 0) sendTestNote(heldNote, 0.0f, false);
		heldNote = -1;
		const int token = ++retriggerToken;
		juce::Component::SafePointer<MainComponent> safeThis(this);
		juce::Timer::callAfterDelay(400, [safeThis, token] {
			auto *self = safeThis.getComponent();
			if (self == nullptr || token != self->retriggerToken || !self->holdActive) return;
			self->heldNote = self->testNotePitch;
			self->sendTestNote(self->heldNote, 0.9f, true);
		});
	}

	// Explicit colours rather than relying on the LookAndFeel's own toggle colouring.
	void updateHoldButtonColour() {
		const auto fill = holdActive ? juce::Colour(0xff6ab81f) : juce::Colour(0xff26262c);
		const auto text = holdActive ? juce::Colour(0xff0a0a0c) : juce::Colour(0xff8ede4a);
		holdButton.setColour(juce::TextButton::buttonColourId, fill);
		holdButton.setColour(juce::TextButton::buttonOnColourId, fill);
		holdButton.setColour(juce::TextButton::textColourOffId, text);
		holdButton.setColour(juce::TextButton::textColourOnId, text);
		for (juce::TextButton *b : {static_cast<juce::TextButton *>(&testNoteButton), &pitchButton}) {
			b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff26262c));
			b->setColour(juce::TextButton::textColourOffId, juce::Colour(0xff8ede4a));
			b->setColour(juce::TextButton::textColourOnId, juce::Colour(0xff8ede4a));
		}
		holdButton.repaint();
	}

	void setView(View v) {
		if (v == currentView) return;
		// A note held for Browse auditioning would otherwise keep sounding with no visible way
		// to stop it once the HOLD button is hidden.
		if (currentView == View::Browse && holdActive) stopHeldTestNote();
		currentView = v;
		resized();
	}

	jivui::AppMode currentMode() const {
		return currentView == View::Browse ? jivui::AppMode::browse
		       : currentView == View::Sequencer ? jivui::AppMode::sequencer
		                                        : jivui::AppMode::frontPanel;
	}

	void setMode(jivui::AppMode mode) {
		setView(mode == jivui::AppMode::browse ? View::Browse
		        : mode == jivui::AppMode::sequencer ? View::Sequencer
		                                            : View::Keyboard);
		repaint();
	}

	// Front panel -> sequencer -> browse patches -> front panel.
	void cycleMode() { setMode(jivui::nextAppMode(currentMode())); }

	static juce::File stateFile() {
		return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
			.getChildFile("jiv881-state.bin");
	}

	void loadPersistedState() {
		auto f = stateFile();
		if (!f.existsAsFile()) return;
		juce::MemoryBlock data;
		if (f.loadFileAsData(data) && data.getSize() > 0)
			processor.setStateInformation(data.getData(), (int)data.getSize());
	}

	// Points the native core at romBringUpDir() if it already has files in it (a previous
	// "Choose ROM files..." run, or the user `adb push`-ed them there directly - see
	// docs/android.md) and retries loading. Harmless no-op once processor.loaded is already
	// true (retryLoadRoms() guards on that itself), and harmless if the folder doesn't exist
	// yet either.
	void bringUpRoms() {
		const auto dest = romBringUpDir();
		if (dest.isDirectory()) {
			processor.setRomsFolderOverride(dest);
			processor.retryLoadRoms();
		}
	}

	void updateStatus(const juce::String &override_ = {}) {
		if (!override_.isEmpty()) {
			statusLabel.setText(override_, juce::dontSendNotification);
			return;
		}
		statusLabel.setText(processor.loaded ? "Ready" : "ROMs not found - use menu to choose files",
		                     juce::dontSendNotification);
	}

	// Android has no working way to list a picked SAF folder's contents (see the D110 Android
	// port's own dev-notes for why AndroidDocumentIterator's tree-walking is dead code in this
	// JUCE version) - so this asks for the ROM files themselves (multi-select) and copies them
	// into a real, plain-filesystem staging folder instead. loadRom() matches files by their
	// EXACT expected filename (see rom.cpp's own romInfos[] table), so - unlike the D110 port,
	// which sniffs file content - the copy has to preserve each file's real display name.
	void chooseRomFolder() {
		fileChooser = std::make_unique<juce::FileChooser>(
			"Choose your JV-880 ROM files (select all of them at once)", juce::File());
		fileChooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
				| juce::FileBrowserComponent::canSelectMultipleItems,
			[this](const juce::FileChooser &fc) { copyRomFilesAndReload(fc.getURLResults()); });
	}

	void copyRomFilesAndReload(const juce::Array<juce::URL> &urls) {
		const auto dest = romBringUpDir();
		dest.createDirectory();

		int copied = 0;
		for (const auto &url : urls) {
			auto doc = juce::AndroidDocument::fromDocument(url);
			auto in = doc.hasValue() ? doc.createInputStream() : nullptr;
			if (in == nullptr) continue;

			// url.getFileName() is unreliable for a content:// SAF result (just whatever
			// follows the URI STRING's last literal '/', not the real display name) -
			// AndroidDocumentInfo::getName() is backed by SAF's real DISPLAY_NAME column
			// instead. Same lesson the D110 Android port already learned once (see its own
			// dev-notes/android.md).
			juce::String name = doc.getInfo().getName();
			if (name.isEmpty()) name = url.getFileName();

			auto outFile = dest.getChildFile(name);
			outFile.deleteFile();
			auto out = outFile.createOutputStream();
			if (out == nullptr || out->writeFromInputStream(*in, -1) <= 0) {
				outFile.deleteFile();
				continue;
			}
			out.reset();
			++copied;
		}

		if (copied == 0) {
			updateStatus("None of those files could be read");
			return;
		}

		// Matches the desktop Settings tab's own "Reload ROMs" button, which likewise disables
		// itself once processor.loaded is true (see PluginProcessor.h's own comment on
		// attemptLoadRoms() - switching ROM folders on an already-loaded engine isn't
		// supported, only a fresh boot re-scans the folder). Files just copied here ARE
		// already sitting in romBringUpDir() correctly named - see chooseRomFolder()'s own
		// comment - they just won't actually load until the app restarts, so say that
		// explicitly instead of silently doing nothing (Alan's report, 2026-09-09: picking
		// expansion-board files after the app was already running from a prior ROM boot copied
		// them fine but Browse still only showed the original factory banks).
		if (processor.loaded) {
			updateStatus(juce::String(copied) + " file(s) copied - close and reopen the app to load them");
			return;
		}

		retryRoms();
	}

	void retryRoms() {
		const bool wasLoaded = processor.loaded;
		bringUpRoms();
		if (!wasLoaded && processor.loaded)
			createPatchDependentViews();
		updateStatus();
		resized();
	}

	// Shared between the app's own hamburger button and the sequencer's own bar-navigation
	// menu button (see createPatchDependentViews()'s own comment) - every item is a
	// self-contained action callback so it can be dropped into either menu unchanged.
	void buildAppMenu(juce::PopupMenu &m) {
		// Reachable unconditionally, not gated behind processor.loaded - this is exactly the
		// thing to reach for when ROMs are missing and the panel/keyboard are sitting empty.
		m.addItem("Choose ROM files...", [this] { chooseRomFolder(); });
		if (!processor.loaded)
			m.addItem("Retry loading ROMs", [this] { retryRoms(); });
		m.addSeparator();
		// The same three views the mode-switch button cycles through, picked directly.
		for (auto mode : { jivui::AppMode::frontPanel, jivui::AppMode::sequencer, jivui::AppMode::browse })
			m.addItem(jivui::appModeName(mode), true, currentMode() == mode, [this, mode] { setMode(mode); });
		// "Sequencer > Classic / Retro / Grid" - the same submenu every front end of the family
		// offers on right-click (SequencerViewMenu.h).
		seqview::addSubmenu(m, seqview::current(processor), [this](seqview::View v) {
			processor.setSequencerView(v);
			resized();
		});
		// VirtualKeyboard::showContextMenu() is the exact same channel/remap/PC-keyboard menu
		// the desktop keyboard's right-click shows - reached here directly instead of
		// reimplementing it, since there's no right mouse button on a touchscreen.
		m.addItem("Keyboard channel...", [this] { keyboard.showContextMenu(); });
		m.addSeparator();

		// Manual escape hatch for resized()'s own nav-bar-avoidance guess - see its own
		// comment. Flat items rather than a further-nested submenu, same reasoning as the D110
		// Android port's own Options menu: one fewer nesting level for a popup that's already
		// two deep when reached from the sequencer's own bar menu.
		juce::PopupMenu options;
		options.addItem("Auto margins (Recommended)", true, !navTop && !navBottom && !navLeft && !navRight,
		                 [this] { navTop = navBottom = navLeft = navRight = false; resized(); });
		options.addItem("Margin: top", true, navTop, [this] { navTop = !navTop; resized(); });
		options.addItem("Margin: bottom", true, navBottom, [this] { navBottom = !navBottom; resized(); });
		options.addItem("Margin: left", true, navLeft, [this] { navLeft = !navLeft; resized(); });
		options.addItem("Margin: right", true, navRight, [this] { navRight = !navRight; resized(); });
		m.addSubMenu("Options", options);
	}

	void showMainMenu() {
		juce::PopupMenu m;
		buildAppMenu(m);
		m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(menuButton));
	}

	// Every discovered device is enabled and its callback pointed straight at `player`
	// (AudioProcessorPlayer already implements MidiInputCallback, forwarding incoming messages
	// into processor.processBlock()'s own MidiBuffer) - class-compliant USB MIDI needs no
	// pairing/permission dialog the way Bluetooth would.
	void scanForMidiInputs() {
		for (const auto &device : juce::MidiInput::getAvailableDevices()) {
			if (knownMidiInputs.contains(device.identifier)) continue;
			knownMidiInputs.add(device.identifier);
			deviceManager.setMidiInputDeviceEnabled(device.identifier, true);
			deviceManager.addMidiInputDeviceCallback(device.identifier, &player);
		}
	}

	void timerCallback() override { scanForMidiInputs(); }

	// Declared first so its default constructor runs before `processor` below is constructed
	// (member init order follows declaration order) - makes VirtualJVProcessor::wrapperType
	// read as wrapperType_Standalone, exactly like JUCE's own Standalone plugin-client wrapper
	// does right before constructing its filter (setTypeOfNextNewPlugin() feeds a thread-local
	// value the AudioProcessor base constructor reads once, during its own init list). Unlike
	// the D110 port (whose sequencer processing has no such gate), this project's
	// processBlock() explicitly requires wrapperType_Standalone before the sequencer/metronome
	// logic runs at all (see PluginProcessor.cpp's own
	// `wrapperType == wrapperType_Standalone && sequencerEnabled` checks) - without this,
	// PLAY/REC on Android would silently do nothing.
	struct WrapperTypeSetter {
		WrapperTypeSetter() {
			juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
		}
	};
	WrapperTypeSetter wrapperTypeSetter;

	VirtualJVProcessor processor;

	View currentView = View::Keyboard;
	bool navTop = false, navBottom = false, navLeft = false, navRight = false;

	PanelSkin panelDisplay;
	VirtualKeyboard keyboard;
	std::unique_ptr<PatchBrowser> patchBrowser;
	std::unique_ptr<JivSequencerPanel> sequencerPanel;
	std::unique_ptr<JivSequencerGridPanel> sequencerGridPanel;
	std::unique_ptr<JivSequencerRetroPanel> sequencerRetroPanel;

	jivui::HamburgerButton menuButton;
	jivui::ModeButton modeButton;
	juce::Label statusLabel;

	// Browse view's NOTE / HOLD / PITCH buttons - see wireUpBrowseTestNote().
	HeldNoteButton testNoteButton;
	juce::TextButton holdButton, pitchButton;
	int testNotePitch = 48; // C3 in this app's convention (getMidiNoteName(n, true, true, 4))
	int testButtonNote = -1; // what the NOTE button last struck, -1 = nothing sounding
	bool holdActive = false;
	int heldNote = -1;
	int retriggerToken = 0;

	std::unique_ptr<juce::FileChooser> fileChooser;

	juce::AudioDeviceManager deviceManager;
	juce::AudioProcessorPlayer player;
	juce::StringArray knownMidiInputs;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

class MainWindow : public juce::DocumentWindow {
public:
	explicit MainWindow(const juce::String &name)
		: DocumentWindow(name, juce::Colours::black, juce::DocumentWindow::allButtons) {
		setUsingNativeTitleBar(true);
		auto *content = new MainComponent();
		mainComponent = content;
		setContentOwned(content, true);
		setFullScreen(true);
		setVisible(true);
	}

	void closeButtonPressed() override {
		juce::JUCEApplication::getInstance()->systemRequestedQuit();
	}

	void saveState() { mainComponent->saveState(); }

private:
	MainComponent *mainComponent = nullptr;
};

class JiV881AndroidApp : public juce::JUCEApplication {
public:
	const juce::String getApplicationName() override { return "JiV-881"; }
	const juce::String getApplicationVersion() override { return "0.1"; }

	void initialise(const juce::String &) override {
		mainWindow = std::make_unique<MainWindow>(getApplicationName());
	}

	void shutdown() override {
		if (mainWindow) mainWindow->saveState();
		mainWindow = nullptr;
	}

	// The save point that actually matters on Android: called when the OS backgrounds this
	// app, and the process can be killed with no further warning at any point afterwards -
	// well before shutdown() above would ever run. A plain in-app quit (which does reach
	// shutdown()) is the rare case; going to the home screen or switching apps is the common
	// one, and only this callback reliably fires for that.
	void suspended() override {
		if (mainWindow) mainWindow->saveState();
	}

private:
	std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(JiV881AndroidApp)
