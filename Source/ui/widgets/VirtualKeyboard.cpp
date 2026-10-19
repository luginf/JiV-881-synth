#include "VirtualKeyboard.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace {
constexpr int kWhiteKeysPerOctave = 7;
constexpr int kWhiteSemitones[kWhiteKeysPerOctave] = { 0, 2, 4, 5, 7, 9, 11 };
// True for the white key immediately to the LEFT of a black key (C, D, F, G, A).
constexpr bool kHasBlackToRight[kWhiteKeysPerOctave] = { true, true, false, true, true, true, false };

// Self-contained replacement for the D-110 emulator's UiTheme::palette() (dark variant) -
// jv880 has no light/dark toggle, so this is just fixed constants. The held-key green
// happens to match the emulator's own LCD colour.
struct Palette {
	juce::Colour panelBg { 0xff141416 };
	juce::Colour keyWhite { 0xffe8e8ec };
	juce::Colour keyWhiteHeld { 0xff6ab81f };
	juce::Colour keyWhiteBorder { 0xff0a0a0c };
	juce::Colour keyBlack { 0xff1a1a1e };
	juce::Colour keyBlackHeld { 0xff3f7a10 };
	juce::Colour keyCaption { 0xff6a6a74 };
	juce::Colour keyButtonFill { 0xff26262c };
	juce::Colour keyButtonText { 0xff8a8a94 };
};
const Palette &palette() {
	static const Palette pal;
	return pal;
}
} // namespace

// The classic two-row tracker layout (FastTracker2, Impulse Tracker, OpenMPT, Renoise: all
// the same table since the 90s). Lower row starts at the keyboard's current base octave,
// upper row one octave above that - the two overlap by an octave, same as every tracker.
// AZERTY's characters are what a French keyboard's PHYSICAL key at that same position
// actually sends: unshifted digits on AZERTY are punctuation (&é"'(-è_çà), not digits, so
// the accidentals' upper row differs there, and three letters move (A/Q, W/Z, M/;).
const std::vector<VirtualKeyboard::TrackerKey> &VirtualKeyboard::trackerKeys() {
	static const std::vector<TrackerKey> keys = {
		// lower row: Z S X D C V G B H N J M , L . ; /  (base octave, semitones 0..16)
		{ 0, 'z', 'w' }, { 1, 's', 's' }, { 2, 'x', 'x' }, { 3, 'd', 'd' },
		{ 4, 'c', 'c' }, { 5, 'v', 'v' }, { 6, 'g', 'g' }, { 7, 'b', 'b' },
		{ 8, 'h', 'h' }, { 9, 'n', 'n' }, { 10, 'j', 'j' }, { 11, 'm', ',' },
		{ 12, ',', ';' }, { 13, 'l', 'l' }, { 14, '.', ':' }, { 15, ';', 'm' }, { 16, '/', '!' },
		// upper row: Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P  (one octave up, semitones 12..28)
		{ 12, 'q', 'a' }, { 13, '2', (juce::juce_wchar)0x00E9 /* é */ }, { 14, 'w', 'z' },
		{ 15, '3', '"' }, { 16, 'e', 'e' }, { 17, 'r', 'r' }, { 18, '5', '(' },
		{ 19, 't', 't' }, { 20, '6', '-' }, { 21, 'y', 'y' },
		{ 22, '7', (juce::juce_wchar)0x00E8 /* è */ }, { 23, 'u', 'u' }, { 24, 'i', 'i' },
		{ 25, '9', (juce::juce_wchar)0x00E7 /* ç */ }, { 26, 'o', 'o' },
		{ 27, '0', (juce::juce_wchar)0x00E0 /* à */ }, { 28, 'p', 'p' },
	};
	return keys;
}

VirtualKeyboard::VirtualKeyboard(VirtualKeyboardHost &h) : host(h) {
	// Restores whatever config the processor was last set to - see VirtualKeyboardHost's own
	// accessors for why the host, not this component, is the actual source of truth.
	midiChannel = host.getKeyboardMidiChannel();
	midiRemap = host.getMidiRemap();
	pcKeyboardEnabled = host.getKeyboardPcInputEnabled();
	pcLayout = host.getKeyboardPcLayout() == 1 ? PcLayout::azerty : PcLayout::qwerty;
	numOctaves = juce::jlimit(0, 4, host.getKeyboardNumOctaves());

	pcKeyDown.assign(trackerKeys().size(), false);
	setWantsKeyboardFocus(true);
	rebuildKeys();

	// Just fast enough that a short note visibly lights its key without repainting this small
	// strip needlessly often - remote activity (external MIDI In, a DAW host's own track) is
	// asynchronous, so nothing else would otherwise trigger a repaint when it starts or stops.
	startTimerHz(30);
}

VirtualKeyboard::~VirtualKeyboard() {
	stopTimer();
	releaseAllTouchNotes();
	releaseAllPcNotes();
	releaseAllHeldNotes();
}

void VirtualKeyboard::sendNote(int note, float velocity, bool on) {
	if (midiRemap) {
		host.injectTestNote(midiChannel, note, velocity, on);
	} else {
		for (int ch = 1; ch <= 16; ++ch) host.injectTestNote(ch, note, velocity, on);
	}
}

void VirtualKeyboard::releaseAllPcNotes() {
	for (size_t i = 0; i < pcKeyDown.size(); ++i) {
		if (!pcKeyDown[i]) continue;
		pcKeyDown[i] = false;
		sendNote(kLowestNote + octaveShift * 12 + trackerKeys()[i].semitoneFromBase, 0.0f, false);
	}
}

void VirtualKeyboard::showContextMenu(int noteForHold) {
	juce::PopupMenu channelMenu;
	for (int ch = 1; ch <= 16; ++ch)
		channelMenu.addItem(1000 + ch, "Channel " + juce::String(ch), true, midiRemap && midiChannel == ch);

	juce::PopupMenu layoutMenu;
	layoutMenu.addItem(2001, "QWERTY", true, pcLayout == PcLayout::qwerty);
	layoutMenu.addItem(2002, "AZERTY", true, pcLayout == PcLayout::azerty);

	const int velocity = host.getKeyboardVelocity();
	const bool presetVelocity = velocity == 60 || velocity == 80 || velocity == 100 || velocity == 127;
	juce::PopupMenu velocityMenu;
	for (int v : {60, 80, 100, 127})
		velocityMenu.addItem(7000 + v, juce::String(v), true, velocity == v);
	velocityMenu.addItem(7000, presetVelocity ? juce::String("Custom...") : "Custom (" + juce::String(velocity) + ")...", true, !presetVelocity);

	juce::PopupMenu m;
	if (noteForHold >= 0) {
		m.addItem(5000, "Hold note (" + juce::MidiMessage::getMidiNoteName(noteForHold, true, true, 4) + ")",
		          true, heldNotes.count(noteForHold) > 0);
	}
#if JUCE_ANDROID
	m.addSectionHeader("Velocity");
	for (int v : {60, 80, 100, 127})
		m.addItem(7000 + v, "Velocity: " + juce::String(v), true, velocity == v);
	m.addItem(7000, "Velocity: custom...", true, !presetVelocity);
#else
	m.addSubMenu("Velocity", velocityMenu);
#endif
	m.addSeparator();
#if JUCE_ANDROID
	// Picking an item inside a submenu deletes the submenu window synchronously (PopupMenu's
	// hide() resets activeSubMenu) while Android's handleMouseUpCallback still has a second
	// event to dispatch to its peer: use-after-free, "Pure virtual function called". Flat menu.
	m.addItem(3000, "MIDI Remap (send on one channel instead of all 16)", true, midiRemap);
	m.addSectionHeader("MIDI Channel");
	for (int ch = 1; ch <= 16; ++ch)
		m.addItem(1000 + ch, "Channel " + juce::String(ch), midiRemap, midiRemap && midiChannel == ch);
	m.addSeparator();
	m.addItem(4000, "PC keyboard input (tracker-style)", true, pcKeyboardEnabled);
	m.addItem(2001, "PC layout: QWERTY", pcKeyboardEnabled, pcLayout == PcLayout::qwerty);
	m.addItem(2002, "PC layout: AZERTY", pcKeyboardEnabled, pcLayout == PcLayout::azerty);
	m.addSeparator();
#else
	m.addSubMenu("MIDI Channel", channelMenu, midiRemap);
	m.addItem(3000, "MIDI Remap (send on one channel instead of all 16)", true, midiRemap);
	m.addSeparator();
	m.addItem(4000, "PC keyboard input (tracker-style)", true, pcKeyboardEnabled);
	m.addSubMenu("PC keyboard layout", layoutMenu, pcKeyboardEnabled);
	m.addSeparator();
#endif
	{
		juce::PopupMenu sizeMenu;
		sizeMenu.addItem(6002, "2 octaves", true, numOctaves == 2);
		sizeMenu.addItem(6004, "4 octaves", true, numOctaves == 4);
		sizeMenu.addItem(6000, "Resizable (more keys when wider)", true, numOctaves == 0);
		m.addSubMenu("Keyboard size", sizeMenu);
	}

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
	                 [this, noteForHold](int result) {
		if (result == 5000) { toggleHoldNote(noteForHold); return; }
		if (result > 7000 && result <= 7127) { host.setKeyboardVelocity(result - 7000); return; }
		if (result == 7000) {
			auto *w = new juce::AlertWindow("Keyboard velocity", "Velocity of the on-screen keyboard notes (1-127):",
			                                juce::MessageBoxIconType::NoIcon);
			w->addTextEditor("velocity", juce::String(host.getKeyboardVelocity()));
			w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
			w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
			juce::Component::SafePointer<VirtualKeyboard> safe(this);
			w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w](int r) {
				if (r == 1 && safe != nullptr)
					safe->host.setKeyboardVelocity(juce::jlimit(1, 127, w->getTextEditorContents("velocity").getIntValue()));
				delete w;
			}), false);
			return;
		}
		if (result >= 1001 && result <= 1016) {
			midiChannel = result - 1000;
			host.setKeyboardMidiChannel(midiChannel);
			return;
		}
		if (result == 3000) {
			midiRemap = !midiRemap;
			host.setMidiRemap(midiRemap);
			return;
		}
		if (result == 4000) {
			pcKeyboardEnabled = !pcKeyboardEnabled;
			host.setKeyboardPcInputEnabled(pcKeyboardEnabled);
			if (pcKeyboardEnabled) grabKeyboardFocus();
			else releaseAllPcNotes();
			return;
		}
		if (result == 2001) {
			pcLayout = PcLayout::qwerty;
			host.setKeyboardPcLayout(0);
			return;
		}
		if (result == 2002) {
			pcLayout = PcLayout::azerty;
			host.setKeyboardPcLayout(1);
			return;
		}
		if (result == 6000 || result == 6002 || result == 6004) {
			setNumOctaves(result == 6000 ? 0 : result - 6000);
			host.setKeyboardNumOctaves(numOctaves);
			return;
		}
	});
}

void VirtualKeyboard::toggleHoldNote(int note) {
	if (note < 0) return;
	if (heldNotes.count(note) > 0) {
		heldNotes.erase(note);
		sendNote(note, 0.0f, false);
	} else {
		heldNotes.insert(note);
		sendNote(note, (host.getKeyboardVelocity() / 127.0f), true);
	}
	repaint();
}

void VirtualKeyboard::releaseAllHeldNotes() {
	for (int note : heldNotes) sendNote(note, 0.0f, false);
	heldNotes.clear();
}

bool VirtualKeyboard::keyStateChanged(bool /*isKeyDown*/) {
	if (!pcKeyboardEnabled) return false;
	const auto &keys = trackerKeys();
	bool used = false;
	for (size_t i = 0; i < keys.size(); ++i) {
		const juce::juce_wchar c = pcLayout == PcLayout::qwerty ? keys[i].qwerty : keys[i].azerty;
		const bool down = juce::KeyPress::isKeyCurrentlyDown((int)c);
		if (down == pcKeyDown[i]) continue;
		pcKeyDown[i] = down;
		sendNote(kLowestNote + octaveShift * 12 + keys[i].semitoneFromBase, (host.getKeyboardVelocity() / 127.0f), down);
		used = true;
	}
	if (used) repaint(); // a tracker key just lit/unlit - see isPcKeyDownForNote()
	return used;
}

// Whether any currently-down PC-tracker key plays this exact note - two physical keys can
// map to the same note (the lower/upper rows overlap by an octave), so this is a search
// rather than a single lookup. Only ever called from paint(), over ~29 visible keys.
bool VirtualKeyboard::isPcKeyDownForNote(int note) const {
	const auto &keys = trackerKeys();
	for (size_t i = 0; i < keys.size(); ++i)
		if (pcKeyDown[i] && kLowestNote + octaveShift * 12 + keys[i].semitoneFromBase == note) return true;
	return false;
}

void VirtualKeyboard::timerCallback() {
	// Channel/remap can now also change from outside this component - re-read every tick,
	// the same cadence isNoteActive() polling already runs at.
	midiChannel = host.getKeyboardMidiChannel();
	midiRemap = host.getMidiRemap();
	for (auto it = heldNotes.begin(); it != heldNotes.end();) {
		if (!host.isNoteActive(*it)) it = heldNotes.erase(it);
		else ++it;
	}
	repaint();
}

void VirtualKeyboard::focusLost(juce::Component::FocusChangeType) { releaseAllPcNotes(); }

void VirtualKeyboard::changeOctave(int delta) {
	const int shifted = juce::jlimit(-2, 3, octaveShift + delta);
	if (shifted == octaveShift) return;
	releaseAllTouchNotes();
	releaseAllPcNotes(); // held notes are note NUMBERS already sent - shifting octave first
	                     // would leave them stuck on, since the physical key never "changed"
	octaveShift = shifted;
	rebuildKeys();
	repaint();
}

// Geometry only - no drawing, no note math beyond the note NUMBERS each key represents.
// Called on resize and on octave change, both of which invalidate every rectangle.
void VirtualKeyboard::rebuildKeys() {
	whiteKeys.clear();
	blackKeys.clear();

	auto area = getLocalBounds().toFloat();
	// A thin caption strip above everything, full width, so "OCT n" never has to sit on top
	// of a black key to be legible.
	captionBounds = area.removeFromTop(juce::jmin(16.0f, area.getHeight() * 0.16f));
	const float buttonW = juce::jmin(36.0f, area.getWidth() * 0.06f);
	octaveDownBounds = area.removeFromLeft(buttonW);
	octaveUpBounds = area.removeFromRight(buttonW);
	keysBounds = area;

	if (keysBounds.getWidth() < 1.0f || keysBounds.getHeight() < 1.0f) return;

	// Resizable mode: keys keep a fixed width (about what a 2-octave strip gets at the default 820 px
	// window) and the count follows the width, always starting and ending on a C.
	int kNumWhite = numOctaves * kWhiteKeysPerOctave + 1; // trailing C
	if (numOctaves == 0) {
		const int octaves = juce::jlimit(1, juce::jmax(1, (127 - (kLowestNote + octaveShift * 12)) / 12),
		                                     (int)(keysBounds.getWidth() / (kResizableKeyW * kWhiteKeysPerOctave)));
		kNumWhite = octaves * kWhiteKeysPerOctave + 1;
	}
	const float whiteW = keysBounds.getWidth() / float(kNumWhite);
	const float blackW = whiteW * 0.62f;
	const float blackH = keysBounds.getHeight() * 0.6f;

	for (int i = 0; i < kNumWhite; ++i) {
		const int octaveIndex = i / kWhiteKeysPerOctave;
		const int local = i % kWhiteKeysPerOctave;
		const int note = kLowestNote + (octaveShift + octaveIndex) * 12 + kWhiteSemitones[local];
		const float x = keysBounds.getX() + i * whiteW;
		whiteKeys.push_back({ { x, keysBounds.getY(), whiteW, keysBounds.getHeight() }, note, false });

		// i < kNumWhite - 1 excludes the trailing C: it's the terminal key of the range, with
		// no white key after it to sit between - without this guard its "black key to the
		// right" landed outside keysBounds altogether, on top of the OCT+ button.
		if (i < kNumWhite - 1 && kHasBlackToRight[local]) {
			const float bx = x + whiteW - blackW * 0.5f;
			blackKeys.push_back({ { bx, keysBounds.getY(), blackW, blackH }, note + 1, true });
		}
	}
}

int VirtualKeyboard::keyAt(juce::Point<float> p) const {
	for (const auto &k : blackKeys)
		if (k.bounds.contains(p)) return k.note;
	for (const auto &k : whiteKeys)
		if (k.bounds.contains(p)) return k.note;
	return -1;
}

void VirtualKeyboard::setHeldNoteForSource(int sourceIndex, int note) {
	const auto it = heldNoteBySource.find(sourceIndex);
	const int previous = (it != heldNoteBySource.end()) ? it->second : -1;
	if (note == previous) return;
	// Don't cut a note "Hold note" is deliberately sustaining just because the mouse/a touch
	// that also happened to strike it lets go - see toggleHoldNote()'s own comment.
	if (previous >= 0 && heldNotes.count(previous) == 0) sendNote(previous, 0.0f, false);
	if (note >= 0) heldNoteBySource[sourceIndex] = note;
	else if (it != heldNoteBySource.end()) heldNoteBySource.erase(it);
	if (note >= 0) sendNote(note, (host.getKeyboardVelocity() / 127.0f), true);
	repaint();
}

void VirtualKeyboard::releaseAllTouchNotes() {
	for (const auto &kv : heldNoteBySource) sendNote(kv.second, 0.0f, false);
	heldNoteBySource.clear();
	repaint();
}

void VirtualKeyboard::paint(juce::Graphics &g) {
	const auto &pal = palette();
	g.fillAll(pal.panelBg);

	auto paintButton = [&](juce::Rectangle<float> b, const char *label) {
		g.setColour(pal.keyButtonFill);
		g.fillRect(b.reduced(3.0f));
		g.setColour(pal.keyButtonText);
		g.setFont(juce::FontOptions(juce::jlimit(12.0f, 22.0f, b.getWidth() * 0.5f)));
		g.drawText(label, b, juce::Justification::centred);
	};
	// Single glyphs, not "OCT-"/"OCT+": the button is often not much wider than one
	// character once the strip is squeezed down.
	paintButton(octaveDownBounds, "-");
	paintButton(octaveUpBounds, "+");

	// Lit for four reasons, checked cheapest-first: held by the mouse/a touch, "Hold note"-ed
	// from the right-click menu, held by a PC-tracker key, or currently sounding somewhere else
	// in the app (external MIDI In, a DAW host track - see VirtualKeyboardHost::isNoteActive()).
	auto isTouchHeld = [this](int note) {
		for (const auto &kv : heldNoteBySource)
			if (kv.second == note) return true;
		return false;
	};
	auto isLit = [this, isTouchHeld](int note) {
		return isTouchHeld(note) || heldNotes.count(note) > 0 || isPcKeyDownForNote(note)
		    || host.isNoteActive(note);
	};
	for (const auto &k : whiteKeys) {
		g.setColour(isLit(k.note) ? pal.keyWhiteHeld : pal.keyWhite);
		g.fillRect(k.bounds.reduced(1.0f, 0.0f));
		g.setColour(pal.keyWhiteBorder);
		g.drawRect(k.bounds, 1.0f);
	}
	for (const auto &k : blackKeys) {
		g.setColour(isLit(k.note) ? pal.keyBlackHeld : pal.keyBlack);
		g.fillRect(k.bounds);
	}

	// "C3"/"C4"/... above every C key, so a keyboard spanning several octaves can be read at a
	// glance rather than counted. octaveNumForMiddleC = 4, so note 60 is "C4". Left-aligned
	// rather than centred: a C key's right edge often sits under the neighbouring black key
	// (see rebuildKeys()), which would otherwise clip a centred label.
	g.setFont(juce::FontOptions(juce::jlimit(8.0f, 11.0f, keysBounds.getHeight() * 0.12f)));
	for (const auto &k : whiteKeys) {
		if (k.note % 12 != 0) continue;
		g.setColour((isLit(k.note) ? pal.keyWhiteHeld : pal.keyWhite).contrasting(0.7f));
		g.drawText("C" + juce::String(k.note / 12 - 1), k.bounds.withTrimmedLeft(3.0f).removeFromTop(13.0f),
		           juce::Justification::centredLeft);
	}

	// Always shown, not just when shifted: it's the only hint of what -/+ actually do.
	g.setColour(pal.keyCaption);
	g.setFont(juce::FontOptions(11.0f));
	g.drawText("OCT " + (octaveShift > 0 ? juce::String("+") + juce::String(octaveShift)
	                                     : juce::String(octaveShift)),
	           captionBounds, juce::Justification::centred);
}

void VirtualKeyboard::resized() { rebuildKeys(); }

void VirtualKeyboard::mouseDown(const juce::MouseEvent &e) {
	// Grabbed on every click, not only when PC input is on: it costs nothing while off, and
	// it means turning PC input on from the menu (itself a click on this component) leaves
	// typing ready to go immediately, with no extra click needed to focus the strip.
	grabKeyboardFocus();

	const int note = keyAt(e.position);
	if (e.mods.isPopupMenu()) { showContextMenu(note); return; }
	if (octaveDownBounds.contains(e.position)) { changeOctave(-1); return; }
	if (octaveUpBounds.contains(e.position)) { changeOctave(1); return; }
	if (note < 0) return;
	setHeldNoteForSource(e.source.getIndex(), note);
}

void VirtualKeyboard::mouseDrag(const juce::MouseEvent &e) {
	const int sourceIndex = e.source.getIndex();
	if (heldNoteBySource.find(sourceIndex) == heldNoteBySource.end())
		return; // this source's own mouseDown didn't start on a key (e.g. an OCT button)
	const int note = keyAt(e.position);
	setHeldNoteForSource(sourceIndex, note); // -1 when dragged off every key, releases cleanly
}

void VirtualKeyboard::mouseUp(const juce::MouseEvent &e) { setHeldNoteForSource(e.source.getIndex(), -1); }

void VirtualKeyboard::mouseExit(const juce::MouseEvent &e) { setHeldNoteForSource(e.source.getIndex(), -1); }
