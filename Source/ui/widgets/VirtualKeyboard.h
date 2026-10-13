#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <set>
#include <vector>

#include "VirtualKeyboardHost.h"

// On-screen test keyboard: a two-octave mouse piano plus optional tracker-style PC
// keyboard input (QWERTY/AZERTY, FastTracker2/Impulse Tracker convention), MIDI
// channel/remap set via right-click. Talks to its owner only through VirtualKeyboardHost.
//
// Ported from the D-110 emulator's D110Keyboard
// (~/src/D110/d110-vst-emulator/plugin/Source/D110Keyboard.h) - same widget, renamed, with
// its UiTheme dependency replaced by a self-contained palette (see VirtualKeyboard.cpp).
//
// Two ways to strike a key: the mouse, on the drawn keys, and - opt-in, right-click to
// enable - the computer keyboard, in the layout every tracker (FastTracker2, Impulse
// Tracker, OpenMPT, Renoise...) has used since the 1990s: two overlapping rows, the lower
// one ZSXDCVGBHNJM,L.;/ starting at the current base octave, the upper one Q2W3ER5T6Y7UI9O0P
// one octave above it. QWERTY and AZERTY differ only in which CHARACTER a given physical
// key sends, not in the note it plays, so trackerKeys() carries both and the chosen layout
// just picks which column to compare incoming key text against.
class VirtualKeyboard : public juce::Component, private juce::Timer {
public:
	explicit VirtualKeyboard(VirtualKeyboardHost &);
	~VirtualKeyboard() override;

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseExit(const juce::MouseEvent &) override;
	bool keyStateChanged(bool isKeyDown) override;
	void focusLost(juce::Component::FocusChangeType) override;

	// Reference height in pixels for the strip this widget wants at the bottom of the editor.
	static constexpr float kRefH = 120.0f;

	// Two octaves is the default. 1-4 = fixed number of octaves, 0 = resizable: keys keep a constant
	// width and their number follows the width of the component (GitHub issue #9).
	void setNumOctaves(int n) {
		n = juce::jlimit(0, 4, n);
		if (n == numOctaves) return;
		numOctaves = n;
		rebuildKeys();
		repaint();
	}
	int getNumOctaves() const { return numOctaves; }

	// Right-click on desktop. `noteForHold`, when >= 0, is which key the click actually landed
	// on - prepends a "Hold note" item ahead of everything else, letting that one note sustain
	// indefinitely rather than only for as long as the mouse button stays down.
	void showContextMenu(int noteForHold = -1);

private:
	int numOctaves = 2;
	static constexpr int kLowestNote = 48; // C3
	static constexpr float kResizableKeyW = 34.0f; // white key width in resizable mode

	enum class PcLayout { qwerty, azerty };

	struct KeyRect { juce::Rectangle<float> bounds; int note; bool black; };

	// One physical key of the tracker layout: the note it plays, relative to the keyboard's
	// current base octave, and which character it sends under each PC layout this offers.
	struct TrackerKey { int semitoneFromBase; juce::juce_wchar qwerty; juce::juce_wchar azerty; };
	static const std::vector<TrackerKey> &trackerKeys();

	void rebuildKeys();
	int keyAt(juce::Point<float>) const;
	// -1 releases. Keyed by MouseInputSource::getIndex() (0 for the mouse; each simultaneous
	// touch gets its own distinct index on a multi-touch platform) rather than one shared note,
	// so several fingers can each hold their own key at once.
	void setHeldNoteForSource(int sourceIndex, int note);
	void releaseAllTouchNotes(); // every source at once - octave change, losing the component, ...
	void changeOctave(int delta);
	void sendNote(int note, float velocity, bool on); // honours channel/midiRemap
	void releaseAllPcNotes();
	bool isPcKeyDownForNote(int note) const;
	// showContextMenu()'s own "Hold note" item. Independent of heldNoteBySource/pcKeyDown (a
	// note here keeps sounding regardless of what the mouse/touch/PC keyboard are doing).
	void toggleHoldNote(int note);
	void releaseAllHeldNotes(); // every menu-held note at once - component destruction
	void timerCallback() override; // polls host.isNoteActive() for remote/incoming activity

	VirtualKeyboardHost &host;
	int octaveShift = 0;
	std::map<int, int> heldNoteBySource; // touch/mouse source index -> the note it's holding
	// Right-click menu's "Hold note" - a note in here keeps sounding until toggleHoldNote()
	// removes it again (menu toggle) or timerCallback() notices host.isNoteActive() has already
	// gone false on its own and drops it to keep the menu's own checkbox honest.
	std::set<int> heldNotes;

	int midiChannel = 1;        // 1..16 - which channel injectTestNote() targets
	bool midiRemap = false;
	bool pcKeyboardEnabled = false;
	PcLayout pcLayout = PcLayout::qwerty;
	// One flag per trackerKeys() entry, so keyStateChanged() (a single "something changed"
	// callback with no indication of which key) can diff against last-known state and fire
	// note-on/off only for the keys that actually moved.
	std::vector<bool> pcKeyDown;

	juce::Rectangle<float> captionBounds, octaveDownBounds, octaveUpBounds, keysBounds;
	std::vector<KeyRect> whiteKeys, blackKeys;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VirtualKeyboard)
};
