# Virtual JiV-881 - Documentation

This is a cycle-accurate emulation of the Roland JV-880's actual CPU running its original
firmware (based on [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55)) - not a re-implementation
from scratch. When something behaves a certain way here, it's usually because that's what the
real hardware's ROM does.

## How the JV-880 works (quick primer)

- **Tone**: the smallest sound-generating unit. A Tone has one waveform (PCM sample) plus its own
  envelope, filter, LFO, etc.
- **Patch**: a full playable sound, made of up to **4 Tones** layered/split/velocity-crossfaded
  together, plus effects (reverb, chorus) and a shared TVF/TVA structure. This is what you'd
  normally call "a preset" or "a sound." 128 factory Patches ship in two ROM banks (**Preset A**
  and **Preset B**, 64 each), plus a rewritable **Internal** bank (64 slots) you can save your own
  Patches into - and optionally more from an SR-JV80 expansion board.
- **Rhythm Set**: like a Patch, but each of the 61 keys plays an independent Tone/sample instead
  of one sound across the keyboard - for drums/percussion.
- **Performance**: a multitimbral setup combining **8 Parts** - 7 Parts, each playing one Patch on
  its own MIDI receive channel, plus a fixed 8th Part playing a Rhythm Set. Reverb and chorus are
  shared across the whole Performance (one effects bus), not per-Part. This is how the JV-880 does
  multi-instrument arrangements (e.g. one MIDI track per Part from a DAW/sequencer).
- **Patch mode vs Performance mode**: the front panel's PATCH/PERFORM button switches the whole
  unit between playing a single Patch (or Rhythm Set) and playing a full 8-part Performance.

## Tabs

- **Browse**: lists every Patch/Rhythm Set available - the two ROM Preset banks, your own saved
  "User" patches, and any loaded SR-JV80 expansion board's patches. Click to load it into the
  currently edited Patch/Rhythm. "Save As..." stores the current Patch into your own User bank
  (`~/.config/JV880/User/*.jvp`). Right-click a patch for "Send to Performance Part N" to assign
  it directly to a Performance Part without leaving Browse. The search box at the top filters by
  name across every bank and expansion (results show the bank/expansion).
- **Performance**: builds and plays a full 8-Part Performance. Per Part: assigned Patch/Rhythm
  Set, MIDI receive channel, level, pan, on/off. "All On/Off" toggles every Part at once. "Part
  details" exposes the rest of each Part (coarse/fine tune, reverb/chorus switches, output
  Main/Sub/Patch, Program Change/Volume/Hold-1 reception, voice reserve) and "Performance common"
  the key mode and the shared reverb/chorus. The **Edit** button of a Part sends its patch to the
  Common/Tone tabs so you can edit what that Part plays. A Performance loaded on the front panel
  (Interface/Panel buttons, or Bank Select + Program Change on MIDI channel 16) is mirrored into
  this tab. Runs on
  the same single emulated engine as Patch mode (no extra CPU cost per Part). Save/load your own
  Performances (`~/.config/JV880/Performances/*.jvpf`), separate from the JV-880's own internal
  Performance memory.
  Assigning Parts from two different expansion boards works but only the most recently assigned
  one actually sounds right (same limit as the real hardware: one expansion slot).
- **Common**: Patch-wide settings shared by all 4 Tones - name, effects (reverb/chorus depth,
  type), structure, and other global Patch parameters.
- **Tone 1-4** (Patch mode) / **Rhythm Set** (Rhythm mode): per-Tone editing - waveform, pitch,
  filter (TVF), amplifier (TVA), LFO, and so on. Which set of tabs you get depends on whether the
  currently loaded program is a Patch or a Rhythm Set.
- **Settings**: Master Tune, Master Volume, Reverb/Chorus on-off, DSP Load meter, ROM folder
  (Browse/Use Default/Reload ROMs - can be changed and reloaded without restarting), and the
  on-screen display mode (see below). In Standalone builds only, this is also where you pick your
  audio/MIDI device and buffer size/sample rate (in a plugin, your DAW/host owns that instead).
  The **Extra features** block (off by default, beyond the original JV-880) holds **Separate drum
  outputs**, **Extra polyphony** (a second engine for 56 voices in Performance mode, about 2x the
  CPU) and, Standalone only, **Audio input through effects** - see "Separate outputs" and "Audio
  input" below. All settings are kept in one `settings.ini` next to the ROMs (`~/.config/JV880/` or
  `~/.config/JiV881/`).
- **Panel**: only shown when Display is set to "LCD only" (see below) - a row of clickable
  physical panel buttons plus the DATA dial, for driving the real firmware's own screens directly
  (EDIT/SYSTEM/RHYTHM/UTILITY, cursor, TONE SELECT, etc.), same as pressing them on real hardware.

## On-screen display modes (Settings tab)

- **LCD only** (default): just the small dot-matrix screen, like the original small-form displays
  built for this hardware.
- **Panel Compact** / **Panel Full**: a photo of the JV-880's actual front panel across the top of
  the window, with the real LCD screen live-embedded into it and every button/knob clickable in
  place, instead of the separate "Panel" tab. Full shows the whole rack unit (needs a wide enough
  window); Compact crops to just the controls.

On any panel skin, useful gestures beyond a plain click:
- **Ctrl+click** a button to latch it down (stays pressed until Ctrl-clicked again) - needed for
  real two-button combos the JV-880 expects, like holding TONE SELECT while pressing a TONE
  SWITCH, or holding TONE SELECT's second function (PARAM SHIFT) while pressing -/+.
  Right-clicking TONE SELECT does the same thing as Ctrl+clicking it.
- **Ctrl+click or right-click the DATA dial** to hold it down while turning it (same idea, for the
  "hold DATA and rotate" combos the manual describes); drag up/down or use the mouse wheel to
  actually turn it.
- **Right-click the LCD** (in any display mode) to change its color.
- Small red LEDs mirror PATCH/PERFORM's real state and which of the 4 Tones in the current Patch
  are actually sounding (TONE SWITCH lights = active tone).

## Patch/Rhythm storage

- **Preset A / Preset B**: 64+64 factory ROM patches, read-only.
- **Internal**: 64 rewritable slots. Slot 0 is whatever's currently loaded in Patch mode; slots
  1-63 (and Performance Parts 1-7) can hold patches you assign, including User patches and
  expansion-board patches.
- **User**: patches you've saved yourself via "Save As...", stored on disk and listed in Browse
  alongside the ROM banks.
- **Expansions (SR-JV80)**: if a supported expansion board dump is present in the ROM folder, its
  patches show up in Browse and are assignable like any other patch (Performance Parts included).

## Android

A Standalone Android build (`android/`) - same real firmware core, front panel, keyboard and
sequencer as the desktop app, no plugin wrapper. Not the tabbed editor: only Panel/Keyboard,
Browse and Sequencer, reached from a single hamburger menu (☰), top-right.

LCD colour: press and hold the panel's LCD (~0.5 s) for the colour picker, the touch equivalent of the desktop right-click.

Views and navigation: **Front Panel**, **Sequencer** and **Browse patches**, switched by the button
just left of the ☰ (it cycles through them in that order; its icon shows the current view), or picked
directly in the ☰ menu. The switch and the ☰ look identical in every view, including inside the two
sequencer panels' own transport rows. In portrait the sequencer's transport is stacked in larger rows
(strip view: STOP/PLAY/REC, BPM, signature, METRO, PRECOUNT + switch + ☰; then LOOP/bar/REC mode/NEW/songs;
then UNDO/REDO/SYNC/LOAD/SAVE; piano-roll view: two rows).

Build: `cd android && ./gradlew assembleDebug` (needs the Android SDK/NDK - see
`android/local.properties`). Install with `adb install -r
app/build/outputs/apk/debug/app-debug.apk`.

ROMs: hamburger menu -> **Choose ROM files...** - select all your JV-880 ROM files at once
(long-press the first to enter multi-select, then tap the rest). Works before the synth has ever
started. Alternatively `adb push` the same files into
`/storage/emulated/0/Android/data/com.jiv881.android/files/roms/` - a file manager app can't
reach that folder on Android 11+ (scoped storage blocks every app but the owner), which is why
the in-app picker is the recommended route.

Browse: a long-press (~0.5 s, without moving) on a patch opens the same "Send to Performance Part
N" / "Send to Sequencer" menu a desktop right-click does. Browse also has NOTE / HOLD / PITCH buttons on its top row (no keyboard is visible there): NOTE
plays the chosen pitch while pressed, HOLD sustains it and re-strikes it on every newly picked
patch, PITCH opens a slider to choose the note. Sequencer views: hamburger menu (or the
☰ button in the sequencer's own transport row) -> **Sequencer** -> Classic / Retro (D-20 style LCD)
/ Grid (piano roll); the grid's pitch rows start at a finger-sized height (ROW button to change).
On desktop (Standalone, sequencer enabled) the same submenu is on right-click on the LCD or on the
drawer handles, and in Settings.

Not yet done: no app store distribution (releases ship a debug-keystore-signed APK); tested on one
real phone (see `.claude/dev-notes/android.md` for the D-110 sibling
project's own catalogue of JUCE/Android gotchas this port leans on).

## On-screen keyboard

Right-click (long press on Android) on a key: *Hold note* (sustains that key), **Velocity** (60, 80,
100, 127 or a custom value; default 100, kept in `settings.ini`), MIDI channel/remap, PC-keyboard input,
keyboard size. Without *MIDI Remap* in Patch mode, a note is sent once (it used to go out on all 16
channels, which stacked 16 identical notes).

## Separate outputs (Performance mode)

Each Part can be sent to its own stereo output. The chip's 28 voices are shared, but the firmware
records which Part owns each voice, so every Part's dry signal is split out; reverb/chorus (one
effects bus on the JV-880) stay on the main output. Main + Parts adds up to the normal mix.

- **Plugin (VST3/LV2)**: buses "Part 1".."Part 8", off by default. Tick *Part outputs on* in Settings,
  reload the plugin, then enable them in your host (a few hosts, e.g. Carla, ignore extra buses).
- **Standalone**: any audio device with more than 2 outputs: channels 1-2 main, then one pair per Part.
  On Linux the *JACK (JiV-881 Part outputs)* device type offers 18 free named ports (`main_L`,
  `part1_L`...) that you route yourself; JUCE's plain JACK type is limited to the ports of `system`.
- **Settings**: *Outputs: Main + Parts / Main + Sub* (Sub = dry sum of the Parts whose Output is Sub,
  like the hardware's SUB jack; "Patch" counts as Main) and *Main out = full mix* (Parts are copied
  instead of removed from the main).
- Part outputs work only in Performance mode; in Patch mode everything stays on the main.
  Windows/macOS: same plugin buses and multi-channel Standalone devices; the JACK type is Linux only.

### Separate drum outputs (one output per drum note)

An extra feature (Settings > Extra features > *Separate drum outputs (reload plugin)*, off by
default, not part of the original JV-880, which only has Main and Sub). It splits the rhythm Part
by drum note: the notes 36-51 (kick, rim, snare, clap, toms, hi-hats, crash, ride) each get a stereo
output; any other note stays on the "Part 8 (Rhythm)" output. Reverb/chorus stay on the main output.

- **Where**: 16 more plugin buses, "Drum 1 - kick (note 36)" to "Drum 16 - ride (note 51)" (off until your
  host enables them). Standalone: channels 19-50, after the 8 Part pairs; the audio device must
  offer 50 outputs. The *JACK (JiV-881 Part outputs)* type then has 50 ports (`drum01_kick_L/R`,
  `drum02_rim_L/R`, `drum03_snare_L/R`...), against the Part ports `part1_L`..`part8_R`.
- **Names**: fixed General MIDI names of the notes 36-51, a JV-880 kit follows that layout only
  loosely; the note number is always exact.
- **Apply**: tick the option, then reload the plugin / restart the app.
- **When it works**: in **Performance mode** (rhythm set on Part 8, rhythm Part receiving on its
  MIDI channel) and also in **Patch mode when the current patch is a Rhythm Set**. A Patch-mode tone
  patch has no drum notes, so the drum outputs stay silent then (and the Part 1-7 outputs stay
  silent in Patch mode with a rhythm set). To check a drum output, untick *Main out = full mix*:
  with it ticked the main output also carries the full drum sound, which hides an unplugged
  drum output.
- **JACK**: only the ports that have a connection count as outputs in use. A drum note whose
  `drumNN_...` port is not connected stays in the Part 8 output (same for unconnected `partN_` ports,
  which stay in the main mix). Connect a port and its note leaves Part 8 on the next audio cycle.
- Limits: the effects are not split; a voice stolen without its voice flag changing may stay on the
  previous output for a moment.

## Audio input (use as an effects processor)

The JV-880 reverb and chorus can process external audio. Plugin (VST3/AU/LV2): enable the "Input"
bus in your host (disabled by default, so no "input muted" notice). Standalone: tick Settings > Extra
features > *Audio input through effects (restart)*; the input device is then unmuted and its channels
appear in the audio selector (beware of feedback). On Linux, the *JACK (JiV-881 Part outputs)* type
then also registers `input_L` / `input_R` ports. The input is mixed into the dry signal and sent to
the effects; there is no send level of its own, so it follows the Performance/patch reverb and
chorus levels (a patch with no reverb leaves it nearly dry).

## Desktop Standalone: names, icon, audio settings

The Linux Standalone binary and its window title are now just **JiV-881** (the plugin bundles are
`JiV-881.vst3` / `JiV-881.lv2`). The window and taskbar icon is the Virtual JiV logo (set on the native
window at startup). Right-click on the panel/background: LCD colour, Sequencer view, *Reset panel
heights* and, Standalone only, **Audio/MIDI Settings...** (the same device dialog the Settings tab embeds).
