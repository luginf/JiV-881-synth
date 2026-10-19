#pragma once

// Abstract interface VirtualKeyboard talks to its owner through - ported from the D-110
// emulator's D110KeyboardHost (~/src/D110/d110-vst-emulator/plugin/Source/D110KeyboardHost.h),
// same decoupling pattern. VirtualJVProcessor implements this.
class VirtualKeyboardHost {
public:
	virtual ~VirtualKeyboardHost() = default;

	virtual void injectTestNote(int channel, int note, float velocity, bool on) = 0;

	// VirtualKeyboard's own config (MIDI channel/remap, PC-keyboard tracker input,
	// QWERTY/AZERTY) - read once at construction and written back on every change, so it
	// survives as long as the host does.
	virtual int getKeyboardMidiChannel() const = 0;
	virtual void setKeyboardMidiChannel(int channel) = 0;
	// When true, every note this keyboard plays is forced onto getKeyboardMidiChannel()
	// instead of being broadcast to all 16 channels. Note: the JV-880 emulator core always
	// forces incoming notes onto channel 1 (or 10 for rhythm) regardless of this setting -
	// see VirtualJVProcessor::processBlock() - so this mainly matters if that ever changes.
	virtual bool getMidiRemap() const = 0;
	virtual void setMidiRemap(bool remap) = 0;
	virtual bool getKeyboardPcInputEnabled() const = 0;
	virtual void setKeyboardPcInputEnabled(bool enabled) = 0;
	// 0 = QWERTY, 1 = AZERTY.
	virtual int getKeyboardPcLayout() const = 0;
	virtual void setKeyboardPcLayout(int layout) = 0;
	// How many octaves showContextMenu()'s "4-octave keyboard (wide)" toggle currently shows.
	virtual int getKeyboardNumOctaves() const = 0;
	virtual void setKeyboardNumOctaves(int numOctaves) = 0;

	// Velocity (1-127) of the notes the on-screen keyboard plays (GitHub issue #13).
	virtual int getKeyboardVelocity() const = 0;
	virtual void setKeyboardVelocity(int velocity) = 0;

	// For the on-screen keyboard's own activity LEDs: true if `note` (0-127, any channel) is
	// currently sounding anywhere in the app, as opposed to what was struck directly on this
	// keyboard, which it already tracks itself.
	virtual bool isNoteActive(int note) const = 0;
};
