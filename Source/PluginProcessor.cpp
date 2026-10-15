/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include <map>
#include <tuple>
#include "SysexImporter.h"
#include "PluginEditor.h"
#include "sequencer/JivSequencerSongsFile.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
// Nuked-SC55-derived cores like this one emulate the real chip's output level bit-for-bit
// rather than going through an adjustable per-voice constant (contrast the D-50 emulator's own
// kOutputMakeupGain, tuned against its own internal 0.37 level), so there's no internal knob to
// raise without risking accuracy. Alan reported the JV-880 sounding noticeably quieter than the
// D-110 emulator by ear; applied post-synthesis, same technique and reasoning as the D-50's own
// makeup gain. 1.5x is a starting estimate (this session has no working audio output to verify
// against a reference by ear) - the Settings tab's Master Volume slider stacks on top of it as
// a pure attenuator, so if this still isn't enough Alan can say so and it can be raised further.
constexpr float kOutputMakeupGain = 1.5f;

// Base app-data directory for everything this build stores here (settings, saved patches,
// performances - ROMs are resolved separately in rom.cpp with the same logic, keep both in sync).
// Prefers the new "JiV881" name (2026-09-09 rebrand, see CLAUDE.md) but falls back to the
// pre-rebrand "JV880" folder if that's the only one that already exists on disk - existing
// installs keep working untouched, no migration step needed. A fresh install (neither exists yet)
// gets the new name.
juce::File appDataBaseDir() {
  auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
  auto newDir = base.getChildFile("JiV881");
  auto oldDir = base.getChildFile("JV880");
  if (!newDir.isDirectory() && oldDir.isDirectory())
    return oldDir;
  return newDir;
}

// --- User settings: one settings.ini in the app-data folder -----------------------------------
// [section] / key=value lines. Always at this fixed location (never inside a folder the settings
// themselves name), so it stays findable even when e.g. the ROM folder it records has gone.
// Before this file existed each setting had its own small file (display_mode.txt, rom_folder.txt,
// sequencer_enabled.txt, extra_polyphony.txt, sequencer_view.txt/sequencer_grid.txt,
// layout_heights.txt, sequencer_retro.txt, keyboard_settings.xml): the first read of a missing
// settings.ini imports them, then deletes them.
using IniData = std::map<juce::String, std::map<juce::String, juce::String>>;

juce::File settingsIniFile() { return appDataBaseDir().getChildFile("settings.ini"); }

void writeIni(const IniData &data) {
  juce::String text = "; JiV-881 settings\n";
  for (auto &section : data) {
    if (section.second.empty())
      continue;
    text << "\n[" << section.first << "]\n";
    for (auto &kv : section.second)
      text << kv.first << "=" << kv.second.replace("\n", " ") << "\n";
  }
  auto file = settingsIniFile();
  file.getParentDirectory().createDirectory();
  file.replaceWithText(text);
}

void importLegacySettings(IniData &d) {
  auto dir = appDataBaseDir();
  auto text = [&](const char *name) {
    auto f = dir.getChildFile(name);
    return f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();
  };
  auto has = [&](const char *name) { return dir.getChildFile(name).existsAsFile(); };
  auto flag = [](const juce::String &v) { return v.getIntValue() != 0 ? "1" : "0"; };

  if (has("display_mode.txt"))
    d["display"]["mode"] = juce::String(text("display_mode.txt").getIntValue());
  if (has("rom_folder.txt") && text("rom_folder.txt").isNotEmpty())
    d["rom"]["folder"] = text("rom_folder.txt");
  if (has("sequencer_enabled.txt"))
    d["sequencer"]["enabled"] = flag(text("sequencer_enabled.txt"));
  if (has("extra_polyphony.txt"))
    d["audio"]["extra_polyphony"] = flag(text("extra_polyphony.txt"));

  // Sequencer view: "<view> <rowHeight>"; the oldest build wrote sequencer_grid.txt, "<0|1> <rowHeight>".
  const char *viewFile = has("sequencer_view.txt") ? "sequencer_view.txt" : has("sequencer_grid.txt") ? "sequencer_grid.txt" : nullptr;
  if (viewFile != nullptr) {
    auto tokens = juce::StringArray::fromTokens(text(viewFile), " ", "");
    const int view = tokens[0].getIntValue();
    d["sequencer"]["view"] = juce::String(std::string(viewFile) == "sequencer_grid.txt" ? (view != 0 ? 2 : 0) : view);
    d["sequencer"]["grid_row_height"] = juce::String(tokens[1].getIntValue());
  }
  if (has("layout_heights.txt")) {
    auto tokens = juce::StringArray::fromTokens(text("layout_heights.txt"), " ", "");
    if (tokens.size() >= 2) {
      d["layout"]["keyboard_height"] = juce::String(tokens[0].getIntValue());
      d["layout"]["sequencer_height"] = juce::String(tokens[1].getIntValue());
    }
    if (tokens.size() >= 4) {
      d["layout"]["keyboard_collapsed"] = flag(tokens[2]);
      d["layout"]["sequencer_collapsed"] = flag(tokens[3]);
    }
  }
  if (has("sequencer_retro.txt")) {
    juce::StringArray lines;
    lines.addLines(dir.getChildFile("sequencer_retro.txt").loadFileAsString());
    d["sequencer"]["retro_lcd_compact"] = flag(lines[0]);
    d["sequencer"]["retro_key_bindings"] = lines[1].trim();
  }
  if (has("keyboard_settings.xml")) {
    if (auto xml = juce::XmlDocument::parse(dir.getChildFile("keyboard_settings.xml"))) {
      d["keyboard"]["pc_input"] = xml->getBoolAttribute("pcInput", false) ? "1" : "0";
      d["keyboard"]["pc_layout"] = juce::String(xml->getIntAttribute("pcLayout", 0));
    }
  }
}

IniData readIni() {
  IniData data;
  auto file = settingsIniFile();
  if (!file.existsAsFile()) {
    importLegacySettings(data);
    if (!data.empty()) {
      writeIni(data);
      for (auto *name : {"display_mode.txt", "rom_folder.txt", "sequencer_enabled.txt", "extra_polyphony.txt",
                         "sequencer_view.txt", "sequencer_grid.txt", "layout_heights.txt", "sequencer_retro.txt",
                         "keyboard_settings.xml"})
        appDataBaseDir().getChildFile(name).deleteFile();
    }
    return data;
  }
  juce::StringArray lines;
  lines.addLines(file.loadFileAsString());
  juce::String section;
  for (auto line : lines) {
    line = line.trim();
    if (line.isEmpty() || line.startsWithChar(';') || line.startsWithChar('#'))
      continue;
    if (line.startsWithChar('[') && line.endsWithChar(']')) {
      section = line.substring(1, line.length() - 1).trim();
      continue;
    }
    const int eq = line.indexOfChar('=');
    if (eq > 0)
      data[section][line.substring(0, eq).trim()] = line.substring(eq + 1).trim();
  }
  return data;
}

bool iniHas(const IniData &d, const char *section, const char *key) {
  auto s = d.find(section);
  return s != d.end() && s->second.count(key) != 0;
}

juce::String iniGet(const IniData &d, const char *section, const char *key, const juce::String &fallback = {}) {
  auto s = d.find(section);
  if (s == d.end())
    return fallback;
  auto k = s->second.find(key);
  return k == s->second.end() ? fallback : k->second;
}

// Read-modify-write the whole (small) file, so several instances in one DAW don't clobber each
// other's unrelated keys.
void iniUpdate(std::initializer_list<std::tuple<const char *, const char *, juce::String>> entries) {
  auto data = readIni();
  for (auto &e : entries)
    data[std::get<0>(e)][std::get<1>(e)] = std::get<2>(e);
  writeIni(data);
}

void loadPersistedKeyboardSettings(bool &pcInput, int &pcLayout) {
  auto d = readIni();
  pcInput = iniGet(d, "keyboard", "pc_input", pcInput ? "1" : "0").getIntValue() != 0;
  pcLayout = iniGet(d, "keyboard", "pc_layout", juce::String(pcLayout)).getIntValue();
}

int loadPersistedKeyboardOctaves() { return iniGet(readIni(), "keyboard", "octaves", "2").getIntValue(); }

void savePersistedKeyboardOctaves(int n) { iniUpdate({{"keyboard", "octaves", juce::String(n)}}); }

void loadPersistedWindowSize(int &w, int &h) {
  auto d = readIni();
  w = iniGet(d, "layout", "window_w", "0").getIntValue();
  h = iniGet(d, "layout", "window_h", "0").getIntValue();
}

void savePersistedWindowSize(int w, int h) {
  iniUpdate({{"layout", "window_w", juce::String(w)}, {"layout", "window_h", juce::String(h)}});
}

juce::File favoritesFile() { return appDataBaseDir().getChildFile("favorites.txt"); }

void savePersistedKeyboardSettings(bool pcInput, int pcLayout) {
  iniUpdate({{"keyboard", "pc_input", pcInput ? "1" : "0"}, {"keyboard", "pc_layout", juce::String(pcLayout)}});
}

juce::File loadPersistedRomFolderOverride() {
  auto path = iniGet(readIni(), "rom", "folder").trim();
  return path.isEmpty() ? juce::File{} : juce::File(path);
}

void savePersistedRomFolderOverride(const juce::File &dir) {
  auto data = readIni();
  if (dir == juce::File{})
    data["rom"].erase("folder");
  else
    data["rom"]["folder"] = dir.getFullPathName();
  writeIni(data);
}

VirtualJVProcessor::DisplayMode loadPersistedDisplayMode() {
  const int n = iniGet(readIni(), "display", "mode", "0").getIntValue();
  if (n == (int)VirtualJVProcessor::DisplayMode::PanelCompact)
    return VirtualJVProcessor::DisplayMode::PanelCompact;
  if (n == (int)VirtualJVProcessor::DisplayMode::PanelFull)
    return VirtualJVProcessor::DisplayMode::PanelFull;
  return VirtualJVProcessor::DisplayMode::LcdOnly;
}

void savePersistedDisplayMode(VirtualJVProcessor::DisplayMode mode) {
  iniUpdate({{"display", "mode", juce::String((int)mode)}});
}

bool loadPersistedSequencerEnabled() { return iniGet(readIni(), "sequencer", "enabled", "0").getIntValue() != 0; }

void savePersistedSequencerEnabled(bool enabled) {
  iniUpdate({{"sequencer", "enabled", enabled ? "1" : "0"}});
}

// Extra polyphony on/off (GitHub issue #7).
bool loadPersistedExtraPolyphony() {
  return iniGet(readIni(), "audio", "extra_polyphony", "0").getIntValue() != 0;
}

void savePersistedExtraPolyphony(bool enabled) {
  iniUpdate({{"audio", "extra_polyphony", enabled ? "1" : "0"}});
}

// Whether the plugin's "Part n" output buses start enabled (GitHub issue #3). Read when the plugin is created, so a
// change only applies to instances created afterwards.
bool loadPersistedMultiOut() {
  return iniGet(readIni(), "audio", "multi_out", "0").getIntValue() != 0;
}

void savePersistedMultiOut(bool enabled) {
  iniUpdate({{"audio", "multi_out", enabled ? "1" : "0"}});
}

// Separate outputs layout: 0 = one pair per Part, 1 = Main + Sub like the hardware's "4 OUT" mode.
int loadPersistedOutputMode() {
  return iniGet(readIni(), "audio", "output_mode", "0").getIntValue() != 0 ? 1 : 0;
}

void savePersistedOutputMode(int mode) {
  iniUpdate({{"audio", "output_mode", juce::String(mode)}});
}

// Audio device type chosen in the Standalone ("ALSA", "JACK", our own JACK type...), restored at the next launch.
juce::String loadPersistedAudioDeviceType() {
  return iniGet(readIni(), "audio", "device_type", "");
}

void savePersistedAudioDeviceType(const juce::String &type) {
  iniUpdate({{"audio", "device_type", type}});
}

// Main output keeps the full mix, Parts copied to their own outputs (instead of being taken out of the main).
bool loadPersistedMainFullMix() {
  return iniGet(readIni(), "audio", "main_full_mix", "0").getIntValue() != 0;
}

void savePersistedMainFullMix(bool enabled) {
  iniUpdate({{"audio", "main_full_mix", enabled ? "1" : "0"}});
}

// Sequencer view (seqview::View: 0 classic, 1 retro, 2 grid) and the grid view's row height.
void loadPersistedSequencerView(bool &gridMode, bool &retroMode, int &rowHeight) {
  auto d = readIni();
  if (!iniHas(d, "sequencer", "view"))
    return;
  const int view = iniGet(d, "sequencer", "view").getIntValue();
  retroMode = view == (int)seqview::View::retro;
  gridMode = view == (int)seqview::View::grid;
  rowHeight = juce::jlimit(0, 200, iniGet(d, "sequencer", "grid_row_height", "0").getIntValue());
}

void savePersistedSequencerView(bool gridMode, bool retroMode, int rowHeight) {
  const auto view = retroMode ? seqview::View::retro : gridMode ? seqview::View::grid : seqview::View::classic;
  iniUpdate({{"sequencer", "view", juce::String((int)view)}, {"sequencer", "grid_row_height", juce::String(rowHeight)}});
}

// Keyboard and sequencer pane heights and folded state, clamped by the editor's own limits.
void loadPersistedPaneHeights(int &keyboard, int &sequencer, bool &keyboardCollapsed,
                              bool &sequencerCollapsed) {
  auto d = readIni();
  if (iniHas(d, "layout", "keyboard_height"))
    keyboard = juce::jlimit(40, 400, iniGet(d, "layout", "keyboard_height").getIntValue());
  if (iniHas(d, "layout", "sequencer_height"))
    sequencer = juce::jlimit(120, 900, iniGet(d, "layout", "sequencer_height").getIntValue());
  keyboardCollapsed = iniGet(d, "layout", "keyboard_collapsed", keyboardCollapsed ? "1" : "0").getIntValue() != 0;
  sequencerCollapsed = iniGet(d, "layout", "sequencer_collapsed", sequencerCollapsed ? "1" : "0").getIntValue() != 0;
}

void savePersistedPaneHeights(int keyboard, int sequencer, bool keyboardCollapsed,
                              bool sequencerCollapsed) {
  iniUpdate({{"layout", "keyboard_height", juce::String(keyboard)},
             {"layout", "sequencer_height", juce::String(sequencer)},
             {"layout", "keyboard_collapsed", keyboardCollapsed ? "1" : "0"},
             {"layout", "sequencer_collapsed", sequencerCollapsed ? "1" : "0"}});
}

// Retro view's own settings: LCD compact flag and encoded key bindings.
void loadPersistedSequencerRetro(bool &compact, juce::String &bindings) {
  auto d = readIni();
  compact = iniGet(d, "sequencer", "retro_lcd_compact", compact ? "1" : "0").getIntValue() != 0;
  bindings = iniGet(d, "sequencer", "retro_key_bindings", bindings);
}

void savePersistedSequencerRetro(bool compact, const juce::String &bindings) {
  iniUpdate({{"sequencer", "retro_lcd_compact", compact ? "1" : "0"}, {"sequencer", "retro_key_bindings", bindings}});
}
} // namespace

//==============================================================================
// getActiveEditor() asserts (and a Debug build then crashes) when called off the message thread, which hosts such
// as Ardour do for state/program changes. Without an editor to update from here, the editor reads the processor's
// state itself when it is next created or refreshed.
juce::AudioProcessorEditor *VirtualJVProcessor::safeActiveEditor() const {
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    return nullptr;
  return getActiveEditor();
}

juce::AudioProcessor::BusesProperties VirtualJVProcessor::makeBusesProperties() {
  // Standalone never starts with them on (it routes Parts through a multi-channel main bus instead).
  auto stereo = juce::AudioChannelSet::stereo();
  // The Standalone has no auxiliary buses at all: its player only ever offers ONE output bus to the processor, and
  // JUCE rejects any layout whose bus count differs from ours, silently falling back to stereo (every device channel
  // then carried the same copy of the main mix). Its Parts go through a multi-channel main bus instead.
  if (juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Standalone)
    // ...and a DISABLED input bus: the player always hands over a layout with one input bus entry, and JUCE rejects it
    // when our input bus count differs (0 vs 1). Disabled, it adds no input channel and no "input muted" notice.
    return BusesProperties().withInput("Input", stereo, false).withOutput("Output", stereo, true);

  const bool partBusesOn = loadPersistedMultiOut();
  return BusesProperties()
      .withOutput("Output", stereo, true)
      .withOutput("Part 1", stereo, partBusesOn)
      .withOutput("Part 2", stereo, partBusesOn)
      .withOutput("Part 3", stereo, partBusesOn)
      .withOutput("Part 4", stereo, partBusesOn)
      .withOutput("Part 5", stereo, partBusesOn)
      .withOutput("Part 6", stereo, partBusesOn)
      .withOutput("Part 7", stereo, partBusesOn)
      .withOutput("Part 8 (Rhythm)", stereo, partBusesOn);
}

VirtualJVProcessor::VirtualJVProcessor()
    : AudioProcessor(
          // Output only: this is a synth with no audio input, and an input bus makes the Standalone wrapper
          // mute the input at first launch ("audio input is muted to avoid feedback loop").
          makeBusesProperties()) {

  loadPersistedKeyboardSettings(keyboardPcInput, keyboardPcLayout);
  keyboardNumOctaves = juce::jlimit(0, 4, loadPersistedKeyboardOctaves());
  loadPersistedWindowSize(savedWindowW, savedWindowH);
  {
    juce::StringArray lines;
    favoritesFile().readLines(lines);
    for (auto &l : lines)
      if (l.isNotEmpty())
        favoriteKeys.insert(l);
  }
  displayMode = loadPersistedDisplayMode();

  ownedNames.reserve(52);

  mcu = new MCU();

  {
    auto overrideDir = loadPersistedRomFolderOverride();
    if (overrideDir != juce::File{})
      setRomsDirectoryOverride(overrideDir.getFullPathName().toStdString());
  }

  // Sequencer channel source: each track IS a Performance Part, so unlike the D-110 project
  // this was ported from, there's no firmware RAM to poll per-block - performanceParts[] is
  // already plain processor state, read directly. Set once here (the lambda itself never
  // changes) rather than refreshed per-block, since it captures `this` and always reads the
  // live array. Prefers the track's own stored channel override (see Track::channelOverride's
  // own comment - decoupled-until-PLAY, same as patch/volume/pan) so the CH readout already
  // shows what will actually apply at the next PLAY/REC edge; falls back to the Part's current
  // live channel while no override is set.
  sequencerEngine.setChannelSource([this](int track) {
    if (track < 0 || track >= kNumPerformanceParts)
      return 1;
    const int override_ = sequencerEngine.getTrackChannelOverride(track);
    if (override_ >= 1 && override_ <= 16)
      return override_;
    return performanceParts[track].midiChannel;
  });

  if (wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
    sequencerEnabled = loadPersistedSequencerEnabled();
    loadPersistedSequencerView(sequencerGridMode, sequencerRetroMode, gridRowHeight);
    loadPersistedPaneHeights(keyboardPaneH, sequencerPaneH, keyboardCollapsed, sequencerCollapsed);
    loadPersistedSequencerRetro(retroLcdCompactMode, retroKeyBindings);
  }

  extraPolyphony = loadPersistedExtraPolyphony();
  mainFullMix = loadPersistedMainFullMix();
  outputMode = loadPersistedOutputMode();

  attemptLoadRoms();

  // Deliberately after attemptLoadRoms(), not before: loadSequencerState() re-resolves each
  // track's stored patch against patchInfos[] (see its own comment), which doesn't exist yet
  // until ROMs/expansions/User patches have actually been loaded.
  if (sequencerEnabled)
    loadSequencerState();
}

// See this method's own comment in PluginProcessor.h.
bool VirtualJVProcessor::attemptLoadRoms() {
  if (loaded)
    return true;

  if (!preloadAll(loadedRoms))
    return false;

  mcu->startSC55(loadedRoms[getRomIndex("jv880_rom1.bin")],
                 loadedRoms[getRomIndex("jv880_rom2.bin")],
                 loadedRoms[getRomIndex("jv880_waverom1.bin")],
                 loadedRoms[getRomIndex("jv880_waverom2.bin")],
                 loadedRoms[getRomIndex("jv880_nvram.bin")]);

  // Headless self-test for the native Performance Temp NVRAM hunt (see CLAUDE.md's "Piste
  // future" section) - drives the emulator's own tick function directly instead of going through
  // a real audio device, so it works even with no audio backend available (e.g. this sandbox).
  // Exits before any window/audio device is created, so it needs no display either. Temporary
  // investigation tool, not part of the plugin's normal behavior.
  if (const char *selfTestButtonEnv = std::getenv("JV880_SELFTEST_BUTTON")) {
    // Comma-separated button ids, pressed one after another (e.g. "10,11" = PATCH_PERFORM then
    // EDIT), each held for holdMs and separated by a postMs settle period.
    const int preMs = std::getenv("JV880_SELFTEST_PRE_MS") ? std::atoi(std::getenv("JV880_SELFTEST_PRE_MS")) : 3000;
    const int holdMs = std::getenv("JV880_SELFTEST_HOLD_MS") ? std::atoi(std::getenv("JV880_SELFTEST_HOLD_MS")) : 150;
    const int postMs = std::getenv("JV880_SELFTEST_POST_MS") ? std::atoi(std::getenv("JV880_SELFTEST_POST_MS")) : 3000;

    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);

    auto runMs = [&](int ms) {
      int totalFrames = sampleRate * ms / 1000;
      int done = 0;
      while (done < totalFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        done += (int)blockFrames;
      }
    };

    std::fprintf(stderr, "[selftest] booting %d ms...\n", preMs);
    runMs(preMs);

    if (auto *f = std::fopen("/tmp/jv880_nvram_before.bin", "wb")) {
      std::fwrite(mcu->nvram, 1, sizeof(mcu->nvram), f);
      std::fclose(f);
    }

    // Tokens are either a plain button id ("10") or "e0"/"e1" for one MCU_EncoderTrigger pulse
    // in that direction (the data entry dial has no button id of its own).
    juce::StringArray tokens;
    tokens.addTokens(juce::String(selfTestButtonEnv), ",", "");
    for (auto &rawTok : tokens) {
      auto tok = rawTok.trim();
      if (tok.startsWithIgnoreCase("e")) {
        const int dir = tok.substring(1).getIntValue();
        std::fprintf(stderr, "[selftest] encoder pulse dir=%d\n", dir);
        mcu->MCU_EncoderTrigger(dir);
      } else {
        const int buttonId = tok.getIntValue();
        std::fprintf(stderr, "[selftest] pressing button %d for %d ms\n", buttonId, holdMs);
        mcu->lcd.LCD_SendButton((uint8_t)buttonId, 1);
        runMs(holdMs);
        mcu->lcd.LCD_SendButton((uint8_t)buttonId, 0);
      }
      std::fprintf(stderr, "[selftest] settling %d ms\n", postMs);
      runMs(postMs);
    }

    if (auto *f = std::fopen("/tmp/jv880_nvram_after.bin", "wb")) {
      std::fwrite(mcu->nvram, 1, sizeof(mcu->nvram), f);
      std::fclose(f);
    }

    std::fprintf(stderr, "[selftest] done, exiting\n");
    std::exit(0);
  }

  // Temporary RE tool (2026-09-08): two follow-up questions from Alan's LED-photo report - (1)
  // do EDIT/SYSTEM/RHYTHM/UTILITY have a clean single "current screen" flag the same way PATCH/
  // PERFORM's nvram[0x11] does (JV880_SELFTEST_LED below found candidates sram[0xb5]/sram[0xde]
  // mixed in with a pile of LCD-text-buffer noise for EDIT alone, not yet checked against the
  // other three mode buttons), and (2) does holding DATA while rotating actually produce a
  // bigger parameter step than rotating alone, matching the Owner's Manual's own description of
  // the gesture (p.49-ish, "faster changes... if you press the dial while turning it").
  if (std::getenv("JV880_SELFTEST_DATAACCEL")) {
    const int preMs = 3000, holdMs = 150, postMs = 800;
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);
    auto runMs = [&](int ms) {
      int totalFrames = sampleRate * ms / 1000;
      int done = 0;
      while (done < totalFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        done += (int)blockFrames;
      }
    };
    auto pressButton = [&](int id) {
      mcu->lcd.LCD_SendButton((uint8_t)id, 1);
      runMs(holdMs);
      mcu->lcd.LCD_SendButton((uint8_t)id, 0);
      runMs(postMs);
    };
    auto reportByte = [&](const char *label, uint32_t off) {
      std::fprintf(stderr, "[selftest-dataaccel] %s: sram[0x%04x] = %02x\n", label, off, mcu->sram[off]);
    };

    std::fprintf(stderr, "[selftest-dataaccel] booting %d ms...\n", preMs);
    runMs(preMs);

    std::fprintf(stderr, "[selftest-dataaccel] --- mode-button candidate flags across EDIT/SYSTEM/RHYTHM/UTILITY ---\n");
    reportByte("baseline", 0x00b5); reportByte("baseline", 0x00de);
    pressButton(MCU_BUTTON_EDIT);
    reportByte("after EDIT", 0x00b5); reportByte("after EDIT", 0x00de);
    pressButton(MCU_BUTTON_SYSTEM);
    reportByte("after SYSTEM", 0x00b5); reportByte("after SYSTEM", 0x00de);
    pressButton(MCU_BUTTON_RHYTHM);
    reportByte("after RHYTHM", 0x00b5); reportByte("after RHYTHM", 0x00de);
    pressButton(MCU_BUTTON_UTILITY);
    reportByte("after UTILITY", 0x00b5); reportByte("after UTILITY", 0x00de);
    pressButton(MCU_BUTTON_PATCH_PERFORM);
    reportByte("after PATCH_PERFORM (back to Patch Play)", 0x00b5); reportByte("after PATCH_PERFORM (back to Patch Play)", 0x00de);

    std::fprintf(stderr, "[selftest-dataaccel] --- DATA hold-while-rotating acceleration ---\n");
    auto dumpLcd = [&](const std::string &name) {
      if (auto *bitmapResult = (uint8_t *)mcu->lcd.LCD_Update()) {
        for (size_t i = 0; i < 1024 * 1024; i++) bitmapResult[i * 4 + 3] = 0xff;
        juce::Image image(juce::Image::PixelFormat::ARGB, 820, 100, false);
        juce::Image::BitmapData pixelMap(image, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < pixelMap.height; y++)
          std::memcpy(pixelMap.getLinePointer(y), bitmapResult + (y * 1024 * 4), (size_t)pixelMap.lineStride);
        juce::PNGImageFormat png;
        juce::File outFile("/tmp/jv880_dataaccel_" + name + ".png");
        juce::FileOutputStream stream(outFile);
        if (stream.openedOk()) { stream.setPosition(0); stream.truncate(); png.writeImageToStream(image, stream); }
      }
    };
    auto dumpPatchTemp = [&](const char *label) {
      // Patch Temp lives at nvram[0x0d70..0x0d70+0x16a) - see setCurrentProgram()'s own comment.
      std::fprintf(stderr, "[selftest-dataaccel] %s: patchTemp[0..15] =", label);
      for (int i = 0; i < 16; i++) std::fprintf(stderr, " %02x", mcu->nvram[0x0d70 + i]);
      std::fprintf(stderr, "\n");
    };

    // Force Patch mode first (a stray persisted Performance-mode session would otherwise land
    // EDIT on Perf:Common instead of Patch Common, throwing off patchTemp entirely).
    std::fprintf(stderr, "[selftest-dataaccel] nvram[0x11] (mode byte) before = %02x\n", mcu->nvram[0x11]);
    if (mcu->nvram[0x11] == 0) pressButton(MCU_BUTTON_PATCH_PERFORM);
    pressButton(MCU_BUTTON_EDIT);
    dumpLcd("00_edit");
    pressButton(MCU_BUTTON_CURSOR_R);
    dumpLcd("01_cursor");
    dumpPatchTemp("before any turn");

    mcu->MCU_EncoderTrigger(1);
    runMs(postMs);
    dumpLcd("02_after_plain_turn");
    dumpPatchTemp("after 1 plain turn (no DATA held)");

    mcu->lcd.LCD_SendButton(MCU_BUTTON_DATA, 1);
    runMs(50);
    mcu->MCU_EncoderTrigger(1);
    runMs(postMs);
    mcu->lcd.LCD_SendButton(MCU_BUTTON_DATA, 0);
    runMs(postMs);
    dumpLcd("03_after_held_turn");
    dumpPatchTemp("after 1 turn WITH DATA held");

    std::fprintf(stderr, "[selftest-dataaccel] done, exiting\n");
    std::exit(0);
  }

  // Temporary RE tool (2026-09-08, Alan's report of small LEDs on TONE SWITCH/EDIT/SYSTEM/RHYTHM
  // in the panel photo - wants TONE SWITCH's mute LEDs live) - like JV880_SELFTEST_BUTTON above
  // but diffs nvram/sram/ram incrementally (previous step, not just the very first baseline) and
  // dumps an LCD screenshot after every single button press, to find exactly which byte(s) light
  // up for each one. JV880_SELFTEST_LED=<comma-separated MCU_BUTTON_* ids>.
  if (const char *selfTestLedEnv = std::getenv("JV880_SELFTEST_LED")) {
    const int preMs = 3000, holdMs = 150, postMs = 1000;
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);
    auto runMs = [&](int ms) {
      int totalFrames = sampleRate * ms / 1000;
      int done = 0;
      while (done < totalFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        done += (int)blockFrames;
      }
    };
    auto dumpLcd = [&](const std::string &name) {
      if (auto *bitmapResult = (uint8_t *)mcu->lcd.LCD_Update()) {
        for (size_t i = 0; i < 1024 * 1024; i++) bitmapResult[i * 4 + 3] = 0xff;
        juce::Image image(juce::Image::PixelFormat::ARGB, 820, 100, false);
        juce::Image::BitmapData pixelMap(image, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < pixelMap.height; y++)
          std::memcpy(pixelMap.getLinePointer(y), bitmapResult + (y * 1024 * 4), (size_t)pixelMap.lineStride);
        juce::PNGImageFormat png;
        juce::File outFile("/tmp/jv880_led_" + name + ".png");
        juce::FileOutputStream stream(outFile);
        if (stream.openedOk()) { stream.setPosition(0); stream.truncate(); png.writeImageToStream(image, stream); }
      }
    };
    std::vector<uint8_t> prevNvram(mcu->nvram, mcu->nvram + sizeof(mcu->nvram));
    std::vector<uint8_t> prevSram(mcu->sram, mcu->sram + sizeof(mcu->sram));
    std::vector<uint8_t> prevRam(mcu->ram, mcu->ram + sizeof(mcu->ram));
    auto diffAndAdvance = [&](const std::string &label) {
      for (size_t i = 0; i < sizeof(mcu->nvram); i++)
        if (mcu->nvram[i] != prevNvram[i])
          std::fprintf(stderr, "[selftest-led] %s: nvram[0x%04zx]: %02x -> %02x\n", label.c_str(), i, prevNvram[i], mcu->nvram[i]);
      for (size_t i = 0; i < sizeof(mcu->sram); i++)
        if (mcu->sram[i] != prevSram[i])
          std::fprintf(stderr, "[selftest-led] %s: sram[0x%04zx]: %02x -> %02x\n", label.c_str(), i, prevSram[i], mcu->sram[i]);
      for (size_t i = 0; i < sizeof(mcu->ram); i++)
        if (mcu->ram[i] != prevRam[i])
          std::fprintf(stderr, "[selftest-led] %s: ram[0x%04zx]: %02x -> %02x\n", label.c_str(), i, prevRam[i], mcu->ram[i]);
      prevNvram.assign(mcu->nvram, mcu->nvram + sizeof(mcu->nvram));
      prevSram.assign(mcu->sram, mcu->sram + sizeof(mcu->sram));
      prevRam.assign(mcu->ram, mcu->ram + sizeof(mcu->ram));
    };

    std::fprintf(stderr, "[selftest-led] booting %d ms...\n", preMs);
    runMs(preMs);
    dumpLcd("00_boot");
    diffAndAdvance("boot"); // establish the post-boot baseline (expect lots of noise, ignored)

    juce::StringArray tokens;
    tokens.addTokens(juce::String(selfTestLedEnv), ",", "");
    int step = 1;
    for (auto &rawTok : tokens) {
      const int buttonId = rawTok.trim().getIntValue();
      std::fprintf(stderr, "[selftest-led] step %d: pressing button %d\n", step, buttonId);
      mcu->lcd.LCD_SendButton((uint8_t)buttonId, 1);
      runMs(holdMs);
      mcu->lcd.LCD_SendButton((uint8_t)buttonId, 0);
      runMs(postMs);
      dumpLcd(juce::String(step).paddedLeft('0', 2).toStdString() + "_btn" + std::to_string(buttonId));
      diffAndAdvance("step" + std::to_string(step) + " (button " + std::to_string(buttonId) + ")");
      step++;
    }

    std::fprintf(stderr, "[selftest-led] done, exiting\n");
    std::exit(0);
  }

  // Headless validation for driving the firmware's *native* Performance mode over SysEx DT1
  // instead of button-press NVRAM diffing - address map transcribed from
  // ~/src/D110/edisyn/edisyn/synth/rolandjv880/RolandJV880Multi.java (Sean Luke's Edisyn, already
  // reverse-engineered against real JV-880/JV-80 hardware). Temporary investigation tool, see
  // CLAUDE.md's "Mode Performance" section.
  if (std::getenv("JV880_SELFTEST_PERF")) {
    const int preMs = std::getenv("JV880_SELFTEST_PRE_MS") ? std::atoi(std::getenv("JV880_SELFTEST_PRE_MS")) : 3000;
    const int settleMs = std::getenv("JV880_SELFTEST_POST_MS") ? std::atoi(std::getenv("JV880_SELFTEST_POST_MS")) : 300;

    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);

    auto runMs = [&](int ms) {
      int totalFrames = sampleRate * ms / 1000;
      int done = 0;
      while (done < totalFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        done += (int)blockFrames;
      }
    };

    auto sendDT1 = [&](uint8_t AA, uint8_t BB, uint8_t CC, uint8_t DD, const std::vector<uint8_t> &payload) {
      std::vector<uint8_t> msg = { 0xF0, 0x41, 0x10, 0x46, 0x12, AA, BB, CC, DD };
      uint32_t sum = AA + BB + CC + DD;
      for (auto b : payload) { msg.push_back(b); sum += b; }
      uint8_t checksum = (uint8_t)((0x80 - (sum & 0x7F)) & 0x7F);
      msg.push_back(checksum);
      msg.push_back(0xF7);
      mcu->postMidiSC55(msg.data(), (int)msg.size());
    };

    std::fprintf(stderr, "[selftest-perf] booting %d ms...\n", preMs);
    runMs(preMs);

    std::fprintf(stderr, "[selftest-perf] switching to Performance mode (System 00 00 00 00 = 0)\n");
    sendDT1(0, 0, 0, 0, { 0x00 });
    runMs(settleMs);

    std::fprintf(stderr, "[selftest-perf] writing Performance Common (name marker)\n");
    std::vector<uint8_t> common(31, 0);
    const char *marker = "TESTPERF1234";
    for (int i = 0; i < 12; i++) common[(size_t)i] = (uint8_t)marker[i];
    sendDT1(0, 0, 0x10, 0x00, common);
    runMs(settleMs);

    // Verify whether this project's "Internal A"/"Internal B" ROM categories are really the
    // firmware's own read-only Preset A / Preset B patch banks (bank 2 / bank 3 in the
    // bank*64+number patchnumber encoding) - if so, Performance Parts can reference any of those
    // 128 factory patches with zero extra work (no need to also write into the real writable
    // Internal Patch Memory bank). Part 1 -> Preset A #1 (value 128), Part 2 -> Preset B #1
    // (value 192) - both should read back on the LCD as "A.Piano 1" per patchInfos[]'s own
    // Internal A/B index 0 (see the Browse tab's first two columns).
    int part1PatchValue = std::getenv("JV880_SELFTEST_PART1_PATCH") ? std::atoi(std::getenv("JV880_SELFTEST_PART1_PATCH")) : 128;
    int part2PatchValue = std::getenv("JV880_SELFTEST_PART2_PATCH") ? std::atoi(std::getenv("JV880_SELFTEST_PART2_PATCH")) : 192;

    std::fprintf(stderr, "[selftest-perf] writing Part 1 (patch=%d, ch=1, level=127)\n", part1PatchValue);
    std::vector<uint8_t> part1(35, 0);
    part1[21] = 1;                    // receiveswitch on
    part1[22] = 0;                    // receivechannel 1 (0-indexed)
    part1[23] = (part1PatchValue >> 4) & 0x7F; // patchnumber MSB nibble
    part1[24] = part1PatchValue & 0x0F;        // patchnumber LSB nibble
    part1[25] = 127;                  // partlevel
    part1[26] = 64;                   // partpan centre
    part1[29] = 1;                    // reverbswitch
    part1[30] = 1;                    // chorusswitch
    sendDT1(0, 0, 0x18, 0x00, part1);
    runMs(settleMs);

    std::fprintf(stderr, "[selftest-perf] writing Part 2 (patch=%d, ch=2, level=100)\n", part2PatchValue);
    std::vector<uint8_t> part2(35, 0);
    part2[21] = 1;
    part2[22] = 1;                    // receivechannel 2
    part2[23] = (part2PatchValue >> 4) & 0x7F;
    part2[24] = part2PatchValue & 0x0F;
    part2[25] = 100;
    part2[26] = 64;
    part2[29] = 1;
    part2[30] = 1;
    sendDT1(0, 0, 0x19, 0x00, part2);
    runMs(settleMs);

    runMs(2000); // let it settle further

    // Quick audio sanity check: does Part 1 (channel 1) actually make sound with default (all-
    // zero) voice reserve / Common effects settings, or does an unreserved part stay silent?
    {
      uint8_t noteOn[3] = { 0x90, 60, 100 };
      mcu->postMidiSC55(noteOn, 3);
      float peakL = 0, peakR = 0;
      double sumSq = 0;
      int totalFrames = sampleRate * 1; // 1 second
      int done = 0;
      while (done < totalFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        for (unsigned int i = 0; i < blockFrames; i++) {
          peakL = std::max(peakL, std::abs(l[i]));
          peakR = std::max(peakR, std::abs(r[i]));
          sumSq += (double)l[i] * l[i] + (double)r[i] * r[i];
        }
        done += (int)blockFrames;
      }
      uint8_t noteOff[3] = { 0x80, 60, 0 };
      mcu->postMidiSC55(noteOff, 3);
      double rms = std::sqrt(sumSq / (2.0 * totalFrames));
      std::fprintf(stderr, "[selftest-perf] audio check: note on ch1, 1s render: peakL=%.4f peakR=%.4f rms=%.6f\n", peakL, peakR, rms);
    }

    // Verify the bank hypothesis: search for the ROM's own "Internal A" patch #1 name and
    // "Internal B" patch #1 name (12 bytes each, straight from ROM2 - same offsets the
    // constructor's own patchInfos[] loop uses) anywhere in RAM, which would mean the firmware
    // actually loaded that factory patch into Part 1 / Part 2's working Tone Temp mirror.
    const char *romName1 = (const char *)&loadedRoms[getRomIndex("jv880_rom2.bin")][0x010ce0];
    const char *romName2 = (const char *)&loadedRoms[getRomIndex("jv880_rom2.bin")][0x018ce0];
    const char *romNameUser = (const char *)&loadedRoms[getRomIndex("jv880_rom2.bin")][0x008ce0];
    std::fprintf(stderr, "[selftest-perf] Internal A patch#1 name in ROM: \"%.12s\"\n", romName1);
    std::fprintf(stderr, "[selftest-perf] Internal B patch#1 name in ROM: \"%.12s\"\n", romName2);
    std::fprintf(stderr, "[selftest-perf] Internal User patch#1 name in ROM: \"%.12s\"\n", romNameUser);
    auto searchFor12 = [&](const char *label, const char *needle) {
      bool found = false;
      for (size_t i = 0; i + 12 <= sizeof(mcu->sram); i++) {
        if (std::memcmp(&mcu->sram[i], needle, 12) == 0) {
          std::fprintf(stderr, "[selftest-perf] FOUND %s at sram offset 0x%04zx\n", label, i);
          found = true;
        }
      }
      if (!found)
        std::fprintf(stderr, "[selftest-perf] %s NOT found in sram\n", label);
    };
    searchFor12("Internal A patch#1 name", romName1);
    searchFor12("Internal B patch#1 name", romName2);
    searchFor12("Internal User patch#1 name", romNameUser);

    if (auto *f = std::fopen("/tmp/jv880_nvram_perf.bin", "wb")) {
      std::fwrite(mcu->nvram, 1, sizeof(mcu->nvram), f);
      std::fclose(f);
    }
    if (auto *f = std::fopen("/tmp/jv880_sram_perf.bin", "wb")) {
      std::fwrite(mcu->sram, 1, sizeof(mcu->sram), f);
      std::fclose(f);
    }

    auto searchRegion = [&](const char *label, const uint8_t *base, size_t size) {
      bool found = false;
      for (size_t i = 0; i + 12 <= size; i++) {
        if (std::memcmp(base + i, marker, 12) == 0) {
          std::fprintf(stderr, "[selftest-perf] FOUND marker \"%s\" in %s at offset 0x%04zx\n", marker, label, i);
          found = true;
        }
      }
      return found;
    };
    bool foundMarker = false;
    foundMarker |= searchRegion("nvram", mcu->nvram, sizeof(mcu->nvram));
    foundMarker |= searchRegion("sram", mcu->sram, sizeof(mcu->sram));
    foundMarker |= searchRegion("ram", mcu->ram, sizeof(mcu->ram));
    foundMarker |= searchRegion("cardram", mcu->cardram, sizeof(mcu->cardram));
    if (!foundMarker)
      std::fprintf(stderr, "[selftest-perf] marker NOT found in nvram/sram/ram/cardram\n");

    // Look for Part 1's distinctive byte-21..30 run (receiveswitch, receivechannel, patch MSB/
    // LSB nibbles, level, pan, coarse/fine tune, reverbswitch, chorusswitch) to pin the exact
    // in-RAM stride between Common and Part 1, and each part's own size.
    const uint8_t part1Pattern[10] = { 1, 0, 0, 5, 0x7F, 0x40, 0, 0, 1, 1 };
    for (size_t i = 0; i + 10 <= sizeof(mcu->sram); i++) {
      if (std::memcmp(&mcu->sram[i], part1Pattern, 10) == 0)
        std::fprintf(stderr, "[selftest-perf] FOUND part1 byte21..30 pattern in sram at offset 0x%04zx (part base ~0x%04zx)\n", i, i - 21);
    }
    const uint8_t part2Pattern[10] = { 1, 1, (70 >> 4) & 0x7F, 70 & 0x0F, 100, 0x40, 0, 0, 1, 1 };
    for (size_t i = 0; i + 10 <= sizeof(mcu->sram); i++) {
      if (std::memcmp(&mcu->sram[i], part2Pattern, 10) == 0)
        std::fprintf(stderr, "[selftest-perf] FOUND part2 byte21..30 pattern in sram at offset 0x%04zx (part base ~0x%04zx)\n", i, i - 21);
    }

    std::fprintf(stderr, "[selftest-perf] nvram[0x11] (patch/perform mode byte) = %02x\n", mcu->nvram[0x11]);

    // Dump the firmware's own rendered LCD screen as a PNG - the strongest possible check that
    // Performance mode and our injected data actually took, since it's literally what the
    // firmware itself chose to display. No window/audio device needed: LCD_Update() just returns
    // its offscreen pixel buffer, same call LCDisplay::paint() makes from the message thread.
    if (auto *bitmapResult = (uint8_t *)mcu->lcd.LCD_Update()) {
      for (size_t i = 0; i < 1024 * 1024; i++) bitmapResult[i * 4 + 3] = 0xff;
      juce::Image image(juce::Image::PixelFormat::ARGB, 820, 100, false);
      juce::Image::BitmapData pixelMap(image, juce::Image::BitmapData::readWrite);
      for (int y = 0; y < pixelMap.height; y++)
        std::memcpy(pixelMap.getLinePointer(y), bitmapResult + (y * 1024 * 4), (size_t)pixelMap.lineStride);
      juce::PNGImageFormat png;
      juce::File outFile("/tmp/jv880_lcd_perf.png");
      juce::FileOutputStream stream(outFile);
      if (stream.openedOk()) {
        stream.setPosition(0);
        stream.truncate();
        png.writeImageToStream(image, stream);
        std::fprintf(stderr, "[selftest-perf] wrote LCD screenshot to /tmp/jv880_lcd_perf.png\n");
      }
    }

    std::fprintf(stderr, "[selftest-perf] done, exiting\n");
    std::exit(0);
  }

  int currentPatchI = 0;

  patchInfoPerGroup.push_back(std::vector<PatchInfo *>());

  // Internal A
  for (int j = 0; j < 64; j++) {
      patchInfos[currentPatchI].name =
          (const char*)&loadedRoms[getRomIndex("jv880_rom2.bin")]
          [0x010ce0 + j * 0x16a];
      patchInfos[currentPatchI].nameLength = 0xc;
      patchInfos[currentPatchI].expansionI = 0xff;
      patchInfos[currentPatchI].patchI = j;
      patchInfos[currentPatchI].present = true;
      patchInfos[currentPatchI].drums = false;
      patchInfos[currentPatchI].iInList = currentPatchI;
      patchInfoPerGroup[0].push_back(&patchInfos[currentPatchI]);
      currentPatchI++;
  }

  // Internal B
  for (int j = 0; j < 64; j++) {
      patchInfos[currentPatchI].name =
          (const char*)&loadedRoms[getRomIndex("jv880_rom2.bin")]
          [0x018ce0 + j * 0x16a];
      patchInfos[currentPatchI].nameLength = 0xc;
      patchInfos[currentPatchI].expansionI = 0xff;
      patchInfos[currentPatchI].patchI = j;
      patchInfos[currentPatchI].present = true;
      patchInfos[currentPatchI].drums = false;
      patchInfos[currentPatchI].iInList = currentPatchI;
      patchInfoPerGroup[0].push_back(&patchInfos[currentPatchI]);
      currentPatchI++;
  }

  // Internal User
  for (int j = 0; j < 64; j++) {
    patchInfos[currentPatchI].name =
        (const char *)&loadedRoms[getRomIndex("jv880_rom2.bin")]
                                 [0x008ce0 + j * 0x16a];
    patchInfos[currentPatchI].nameLength = 0xc;
    patchInfos[currentPatchI].expansionI = 0xff;
    patchInfos[currentPatchI].patchI = j;
    patchInfos[currentPatchI].present = true;
    patchInfos[currentPatchI].drums = false;
    patchInfos[currentPatchI].iInList = currentPatchI;
    patchInfoPerGroup[0].push_back(&patchInfos[currentPatchI]);
    currentPatchI++;
  }

  patchInfos[currentPatchI].name = "Rhythm Set Int A";
  patchInfos[currentPatchI].ptr =
      (char *)&loadedRoms[getRomIndex("jv880_rom2.bin")][0x016760];
  patchInfos[currentPatchI].nameLength = 21;
  patchInfos[currentPatchI].expansionI = 0xff;
  patchInfos[currentPatchI].patchI = 0;
  patchInfos[currentPatchI].present = true;
  patchInfos[currentPatchI].drums = true;
  patchInfos[currentPatchI].iInList = currentPatchI;
  patchInfoPerGroup[0].push_back(&patchInfos[currentPatchI]);
  currentPatchI++;

  patchInfos[currentPatchI].name = "Rhythm Set Int B";
  patchInfos[currentPatchI].ptr =
      (char *)&loadedRoms[getRomIndex("jv880_rom2.bin")][0x01e760];
  patchInfos[currentPatchI].nameLength = 21;
  patchInfos[currentPatchI].expansionI = 0xff;
  patchInfos[currentPatchI].patchI = 0;
  patchInfos[currentPatchI].present = true;
  patchInfos[currentPatchI].drums = true;
  patchInfos[currentPatchI].iInList = currentPatchI;
  patchInfoPerGroup[0].push_back(&patchInfos[currentPatchI]);
  currentPatchI++;

  patchInfos[currentPatchI].name = "Rhythm Set User";
  patchInfos[currentPatchI].ptr =
      (char*)&loadedRoms[getRomIndex("jv880_rom2.bin")][0x00e760];
  patchInfos[currentPatchI].nameLength = 21;
  patchInfos[currentPatchI].expansionI = 0xff;
  patchInfos[currentPatchI].patchI = 0;
  patchInfos[currentPatchI].present = true;
  patchInfos[currentPatchI].drums = true;
  patchInfos[currentPatchI].iInList = currentPatchI;
  patchInfoPerGroup[0].push_back(&patchInfos[currentPatchI]);
  currentPatchI++;

  for (int i = 0; i < NUM_EXPS; i++) {
    const int isRD500 = (i == 0);

    patchInfoPerGroup.push_back(std::vector<PatchInfo *>());

    if ((isRD500 && !romInfos[romCountRequired].loaded) || !romInfos[i + romCountRequired + 1].loaded)
      continue;

    expansionsDescr[i] = loadedRoms[i + 6];

    // get patches
    int nPatches = isRD500 ? 192 : expansionsDescr[i][0x67] | expansionsDescr[i][0x66] << 8;

    for (int j = 0; j < nPatches; j++) {
      size_t patchesOffset =
          expansionsDescr[i][0x8f] | expansionsDescr[i][0x8e] << 8 |
          expansionsDescr[i][0x8d] << 16 | expansionsDescr[i][0x8c] << 24;

      if (isRD500)
      {
        if (j < 64)
          patchesOffset = 0x0ce0;
        else if (j < 128)
          patchesOffset = 0x8370;
        else
          patchesOffset = 0x12b82;
      }

      patchInfos[currentPatchI].name =
          (char *)&expansionsDescr[i][patchesOffset + j * 0x16a];

      if (isRD500)
        patchInfos[currentPatchI].name =
            (char *)&loadedRoms[getRomIndex("rd500_patches.bin")]
                               [patchesOffset + (j % 64) * 0x16a];

      patchInfos[currentPatchI].nameLength = 0xc;
      patchInfos[currentPatchI].expansionI = i;
      patchInfos[currentPatchI].patchI = j;
      patchInfos[currentPatchI].present = true;
      patchInfos[currentPatchI].drums = false;
      patchInfos[currentPatchI].iInList = currentPatchI;
      patchInfoPerGroup[i + 1].push_back(&patchInfos[currentPatchI]);
      currentPatchI++;
    }

    // get drumkits
    int nDrumkits = isRD500 ? 3 : expansionsDescr[i][0x69] | expansionsDescr[i][0x68] << 8;

    for (int j = 0; j < nDrumkits; j++) {
      size_t patchesOffset =
          expansionsDescr[i][0x93] | expansionsDescr[i][0x92] << 8 |
          expansionsDescr[i][0x91] << 16 | expansionsDescr[i][0x90] << 24;

      if (isRD500)
      {
        if (j < 64)
          patchesOffset = 0x6760;
        else if (j < 128)
          patchesOffset = 0xd2a0;
        else
          patchesOffset = 0x18602;
      }

      ownedNames.push_back("Rhythm Set " + std::to_string(j + 1));
      patchInfos[currentPatchI].name = ownedNames.back().c_str();

      patchInfos[currentPatchI].ptr =
          (const char *)&expansionsDescr[i][patchesOffset + j * 0xa7c];

      if (isRD500)
        patchInfos[currentPatchI].ptr =
            (const char *)&loadedRoms[getRomIndex("rd500_patches.bin")]
                                     [patchesOffset];

      patchInfos[currentPatchI].nameLength = (int)ownedNames.back().size();
      patchInfos[currentPatchI].expansionI = i;
      patchInfos[currentPatchI].patchI = j;
      patchInfos[currentPatchI].present = true;
      patchInfos[currentPatchI].drums = true;
      patchInfos[currentPatchI].iInList = currentPatchI;
      patchInfoPerGroup[i + 1].push_back(&patchInfos[currentPatchI]);
      currentPatchI++;
    }

    // total count
    totalPatchesExp += nPatches;
    totalPatchesExp += nDrumkits;
  }

  // The "User" bank - one flat group appended after every ROM-based one (indices 0..NUM_EXPS
  // above), populated from disk by refreshUserPatches().
  patchInfoPerGroup.push_back(std::vector<PatchInfo *>());
  userToneBuffers.reserve(userPatchCapacity);
  userDrumBuffers.reserve(userPatchCapacity);
  userPatchNames.reserve(userPatchCapacity);
  refreshUserPatches();
  refreshPerformanceBank();

  performanceParts[kNumPerformanceParts - 1].isRhythm = true;

  loaded = true;

  loadPerformanceSessionState();

  // GitHub issue #7: extra polyphony. Plays a 14-note chord of a sustained patch through the real
  // processBlock() with the second engine off then on and prints the RMS of each.
  if (std::getenv("JV880_SELFTEST_POLY")) {
    setRateAndBufferSizeDetails(44100.0, 512);
    prepareToPlay(44100.0, 512);
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    auto runMs = [&](int ms) {
      double sum = 0;
      long n = 0;
      for (int done = 0; done < 44100 * ms / 1000; done += 512) {
        buf.clear();
        processBlock(buf, midi);
        midi.clear();
        for (int i = 0; i < 512; i++) { sum += buf.getSample(0, i) * buf.getSample(0, i); n++; }
      }
      return std::sqrt(sum / std::max<long>(n, 1));
    };
    runMs(2000);
    sendPatchToPerformancePart(128, 0); // "JV Strings"
    setPerformancePartParams(0, 1, 127, 64, true);
    setPerformanceModeEnabled(true);
    runMs(3000);
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
    runMs(500);
    std::fprintf(stderr, "[selftest-poly] single note rms=%.4f\n", runMs(500));
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    runMs(3000);
    for (int pass = 0; pass < 3; pass++) {
      if (pass == 1) {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        setExtraPolyphony(true);
        std::fprintf(stderr, "[selftest-poly] engine 2 built in %.0f ms\n", juce::Time::getMillisecondCounterHiRes() - t0);
        runMs(500);
      } else if (pass == 2) {
        setExtraPolyphony(false);
        runMs(500);
      }
      for (int n = 0; n < 14; n++) midi.addEvent(juce::MidiMessage::noteOn(1, 40 + n * 2, (juce::uint8)100), 0);
      runMs(1500);
      std::fprintf(stderr, "[selftest-poly] extra=%d mcu2=%d rms(14-note chord, 1.5s)=%.4f\n", pass == 1, mcu2 != nullptr, runMs(500));
      for (int n = 0; n < 14; n++) midi.addEvent(juce::MidiMessage::noteOff(1, 40 + n * 2), 0);
      runMs(4000);
    }
    std::exit(0);
  }

  // GitHub issue #3, end to end: real processBlock() with an 18-channel main bus (main + 8 Part stereo pairs).
  if (std::getenv("JV880_SELFTEST_SEPOUT")) {
    const int sampleRate = 44100, blockFrames = 512;
    {
      // The shape the Standalone player offers: one (empty) input bus list and ONE output bus.
      BusesLayout asPlayer{ { juce::AudioChannelSet::canonicalChannelSet(0) }, { juce::AudioChannelSet::canonicalChannelSet(9) } };
      std::fprintf(stderr, "[sepout] player-style 9-channel layout accepted=%d (buses: %d in, %d out, input channels %d)\n", (int)checkBusesLayoutSupported(asPlayer), getBusCount(true), getBusCount(false), getTotalNumInputChannels());
    }
    BusesLayout lay = getBusesLayout();
    lay.outputBuses.getReference(0) = juce::AudioChannelSet::discreteChannels(18);
    std::fprintf(stderr, "[sepout] layout supported=%d set=%d\n", (int)isBusesLayoutSupported(lay), (int)setBusesLayout(lay));
    setRateAndBufferSizeDetails(sampleRate, blockFrames);
    prepareToPlay(sampleRate, blockFrames);
    juce::AudioBuffer<float> buf(18, blockFrames);
    juce::MidiBuffer midi;
    auto run = [&](int ms, double *e) {
      for (int c = 0; c < 9; c++) e[c] = 0;
      for (int done = 0; done < sampleRate * ms / 1000; done += blockFrames) {
        buf.clear();
        processBlock(buf, midi);
        midi.clear();
        for (int c = 0; c < 9; c++)
          for (int i = 0; i < blockFrames; i++) e[c] += buf.getSample(2 * c, i) * buf.getSample(2 * c, i);
      }
    };
    double e[9];
    run(2500, e);
    for (int i = 0; i < kNumPerformanceParts - 1; i++) {
      sendPatchToPerformancePart(i == 0 ? 0 : 1 + i, i);
      setPerformancePartParams(i, i + 1, 100, 64, true);
    }
    setPerformanceModeEnabled(true);
    run(1500, e);
    for (int k : {1, 4}) {
      midi.addEvent(juce::MidiMessage::noteOn(k + 1, 60, (juce::uint8)100), 0);
      run(800, e);
      std::fprintf(stderr, "[sepout] note on Part %d, energies main/p1..p8:", k + 1);
      for (int c = 0; c < 9; c++) std::fprintf(stderr, " %.3f", e[c]);
      std::fprintf(stderr, "\n");
      midi.addEvent(juce::MidiMessage::noteOff(k + 1, 60), 0);
      run(3000, e);
    }
    // Main + Sub mode: Part 2 set to Sub must show up on channels 2-3 and leave the main.
    performanceParts[1].outputSelect = 1;
    applyPerformancePart(1);
    setOutputMode(1);
    run(500, e);
    for (int k : {1, 4}) {
      midi.addEvent(juce::MidiMessage::noteOn(k + 1, 60, (juce::uint8)100), 0);
      run(800, e);
      std::fprintf(stderr, "[sepout] SUB mode, note on Part %d (Part 2 = Sub), energies main/ch2-3/ch4-5/...:", k + 1);
      for (int c = 0; c < 4; c++) std::fprintf(stderr, " %.3f", e[c]);
      std::fprintf(stderr, "\n");
      midi.addEvent(juce::MidiMessage::noteOff(k + 1, 60), 0);
      run(3000, e);
    }
    setOutputMode(0);
    // plain stereo again: separation must switch off cleanly
    BusesLayout stereo = getBusesLayout();
    stereo.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    std::fprintf(stderr, "[sepout] back to stereo: set=%d\n", (int)setBusesLayout(stereo));
    prepareToPlay(sampleRate, blockFrames);
    juce::AudioBuffer<float> sbuf(2, blockFrames);
    midi.addEvent(juce::MidiMessage::noteOn(2, 60, (juce::uint8)100), 0);
    double se = 0;
    for (int done = 0; done < sampleRate; done += blockFrames) {
      sbuf.clear();
      processBlock(sbuf, midi);
      midi.clear();
      for (int i = 0; i < blockFrames; i++) se += sbuf.getSample(0, i) * sbuf.getSample(0, i);
    }
    std::fprintf(stderr, "[sepout] stereo energy %.3f separateParts=%d\n", se, (int)mcu->separateParts);
    std::exit(0);
  }

  // GitHub issue #2: does Channel Aftertouch reach the firmware and change the sound? Headless.
  if (std::getenv("JV880_SELFTEST_AFT")) {
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);
    auto run = [&](int ms, double *rms, double *bright) {
      double e = 0, d = 0; long n = 0;
      for (int done = 0; done < sampleRate * ms / 1000; done += (int)blockFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        for (unsigned int i = 1; i < blockFrames; i++) { e += l[i] * l[i]; d += (l[i] - l[i - 1]) * (l[i] - l[i - 1]); n++; }
      }
      if (rms) *rms = std::sqrt(e / std::max(1L, n));
      if (bright) *bright = std::sqrt(d / std::max(1L, n));
    };
    run(2500, nullptr, nullptr);
    int idx = -1;
    for (int i = 0; i < 195; i++) if (patchInfos[i].present && juce::String(patchInfos[i].name, patchInfos[i].nameLength).trim() == "Analog Pad 2") idx = i;
    std::fprintf(stderr, "[aft] Analog Pad 2 index %d\n", idx);
    if (idx >= 0) setCurrentProgram(idx);
    run(1500, nullptr, nullptr);
    const Patch *pt = (const Patch *)status.patch;
    for (int t = 0; t < 4; t++)
      std::fprintf(stderr, "[aft] tone %d aft dest %02x %02x sens %02x %02x %02x %02x\n", t, pt->tones[t].matrixAftDestAB, pt->tones[t].matrixAftDestCD,
                   pt->tones[t].matrixAftSensA, pt->tones[t].matrixAftSensB, pt->tones[t].matrixAftSensC, pt->tones[t].matrixAftSensD);
    for (int mode = 0; mode < 3; mode++) {
      uint8_t on[3] = {0x90, 60, 100}, off[3] = {0x80, 60, 0};
      uint8_t at0[2] = {0xD0, 0}, at127[2] = {0xD0, 127};
      uint8_t pa0[3] = {0xA0, 60, 0}, pa127[3] = {0xA0, 60, 127};
      mcu->postMidiSC55(on, 3);
      double r0, b0, r1, b1;
      run(1200, &r0, &b0);
      if (mode == 0) mcu->postMidiSC55(at127, 2); else if (mode == 1) mcu->postMidiSC55(pa127, 3);
      run(800, &r1, &b1);
      std::fprintf(stderr, "[aft] mode %s: before rms=%.4f bright=%.4f  after rms=%.4f bright=%.4f\n", mode == 0 ? "channel" : mode == 1 ? "poly" : "none", r0, b0, r1, b1);
      mcu->postMidiSC55(mode == 0 ? at0 : pa0, mode == 0 ? 2 : 3);
      mcu->postMidiSC55(off, 3);
      run(3000, nullptr, nullptr);
    }
    std::exit(0);
  }

  // RE of the firmware's Performance Temp RAM: change ONE Part field over SysEx and diff sram.
  if (std::getenv("JV880_SELFTEST_PERFRE")) {
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);
    auto runMs = [&](int ms) {
      for (int done = 0; done < sampleRate * ms / 1000; done += (int)blockFrames)
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
    };
    runMs(2500);
    for (int i = 0; i < kNumPerformanceParts; i++) {
      performanceParts[i].present = true;
      performanceParts[i].midiChannel = i + 1;
      performanceParts[i].number = (uint8_t)i;
    }
    setPerformanceModeEnabled(true);
    runMs(1000);
    auto snap = [&]() { return std::vector<uint8_t>(mcu->sram, mcu->sram + SRAM_SIZE); };
    std::vector<bool> noisy(SRAM_SIZE, false);
    for (int k = 0; k < 6; k++) {
      auto a = snap();
      runMs(250);
      for (size_t i = 0; i < a.size(); i++) if (a[i] != mcu->sram[i]) noisy[i] = true;
    }
    {
      int c = 0; for (bool b : noisy) c += b;
      std::fprintf(stderr, "[perfre] noisy bytes: %d of %d\n", c, (int)SRAM_SIZE);
    }
    auto diffTo = [&](const char *label, const std::vector<uint8_t> &a) {
      std::fprintf(stderr, "[perfre] %s:", label);
      int n = 0;
      for (size_t i = 0; i < a.size(); i++)
        if (i >= 0x100 && i < 0x4800 && !noisy[i] && a[i] != mcu->sram[i] && n++ < 60) std::fprintf(stderr, " %04zx:%02x>%02x", i, a[i], mcu->sram[i]);
      std::fprintf(stderr, " (%d diffs)\n", n);
    };
    auto nvSnap = [&]() { return std::vector<uint8_t>(mcu->nvram, mcu->nvram + NVRAM_SIZE); };
    auto nvDiff = [&](const std::vector<uint8_t> &a) {
      std::fprintf(stderr, "[perfre]    nvram:");
      int n = 0;
      for (size_t i = 0; i < a.size(); i++) if (a[i] != mcu->nvram[i] && n++ < 20) std::fprintf(stderr, " %04zx:%02x>%02x", i, a[i], mcu->nvram[i]);
      std::fprintf(stderr, " (%d)\n", n);
    };
    for (int part : {3}) {
      auto &p = performanceParts[part];
      struct F { const char *n; int *i; int v; };
      int lvl = p.level, pan = p.pan, ch = p.midiChannel, ct = p.coarseTune, ft = p.fineTune, num = p.number;
      (void)lvl; (void)pan; (void)ch; (void)ct; (void)ft; (void)num;
      auto test = [&](const char *name, auto setter) {
        auto before = snap();
        auto nvBefore = nvSnap();
        setter();
        pushPerformancePartToEngine(part);
        runMs(300);
        (void)before; (void)nvBefore;
        std::fprintf(stderr, "[perfre] %-14s rec:", name);
        for (int k = -1; k < 21; k++) std::fprintf(stderr, " %02x", mcu->sram[0x2096 + 22 * part + k]);
        std::fprintf(stderr, "\n");
      };
      test("baseline", [&] {});
      test("level 101->91", [&] { p.level = 91; });
      test("pan 64->33", [&] { p.pan = 33; });
      test("channel->12", [&] { p.midiChannel = 12; });
      test("coarse 64->70", [&] { p.coarseTune = 70; });
      test("fine 64->80", [&] { p.fineTune = 80; });
      test("number->50", [&] { p.number = 50; });
      test("rev off", [&] { p.reverbSwitch = false; });
      test("cho off", [&] { p.chorusSwitch = false; });
      test("enabled off", [&] { p.enabled = false; });
      test("output->1", [&] { p.outputSelect = 1; });
      test("rxvol off", [&] { p.rxVolume = false; });
      test("output->2", [&] { p.outputSelect = 2; });
      test("rxpc off", [&] { p.rxProgramChange = false; });
      test("rxhold off", [&] { p.rxHold1 = false; });
      test("all on again", [&] { p.enabled = true; p.reverbSwitch = true; p.chorusSwitch = true; p.rxVolume = true; p.rxProgramChange = true; p.rxHold1 = true; p.outputSelect = 0; });
    }
    auto comm = [&](const char *label, auto setter) {
      setter();
      applyPerformanceCommon();
      runMs(300);
      std::fprintf(stderr, "[perfre] common %-18s 2060:", label);
      for (int k = 0; k < 0x30; k++) std::fprintf(stderr, " %02x", mcu->sram[0x2060 + k]);
      std::fprintf(stderr, "\n");
    };
    comm("baseline", [&] {});
    comm("keyMode=1", [&] { performanceCommon.keyMode = 1; });
    comm("reverbType=1", [&] { performanceCommon.reverbType = 1; });
    comm("reverbLevel=21", [&] { performanceCommon.reverbLevel = 21; });
    comm("reverbTime=22", [&] { performanceCommon.reverbTime = 22; });
    comm("reverbFb=23", [&] { performanceCommon.reverbFeedback = 23; });
    comm("chorusType=2", [&] { performanceCommon.chorusType = 2; });
    comm("chorusLevel=24", [&] { performanceCommon.chorusLevel = 24; });
    comm("chorusDepth=25", [&] { performanceCommon.chorusDepth = 25; });
    comm("chorusRate=26", [&] { performanceCommon.chorusRate = 26; });
    comm("chorusFb=27", [&] { performanceCommon.chorusFeedback = 27; });
    comm("chorusOutput=1", [&] { performanceCommon.chorusOutput = 1; });
    comm("reserve[0]=5", [&] { performanceCommon.voiceReserve[0] = 5; });
    comm("reserve[7]=6", [&] { performanceCommon.voiceReserve[7] = 6; });
    // Now let the firmware load a stored Performance: Bank Select 81 + Program Change (Preset A) then Internal.
    auto dumpRecs = [&](const char *label) {
      std::fprintf(stderr, "[perfre] %s name='", label);
      for (int k = 0; k < 12; k++) std::fprintf(stderr, "%c", mcu->sram[0x206a + k] >= 32 ? mcu->sram[0x206a + k] : '?');
      std::fprintf(stderr, "' common:");
      for (int k = 0; k < 0x10; k++) std::fprintf(stderr, " %02x", mcu->sram[0x2076 + k - 0 ]);
      std::fprintf(stderr, "\n");
      for (int pI = 0; pI < 8; pI++) {
        std::fprintf(stderr, "[perfre]    part %d:", pI);
        for (int k = -1; k < 21; k++) std::fprintf(stderr, " %02x", mcu->sram[0x2096 + 22 * pI + k]);
        std::fprintf(stderr, "\n");
      }
    };
    for (int ct = 0; ct < 3; ct++) {
      performanceCommon.keyMode = 0; performanceCommon.reverbType = 0; performanceCommon.chorusType = ct;
      applyPerformanceCommon(); runMs(300);
      std::fprintf(stderr, "[perfre] chorusType=%d -> raw byte %02x\n", ct, mcu->sram[0x2076]);
    }
    // Round trip: what we wrote must be read back identically.
    performanceCommon.keyMode = 2; performanceCommon.chorusType = 1; performanceCommon.reverbType = 7;
    applyPerformanceCommon(); runMs(300); importPerformanceFromFirmware();
    std::fprintf(stderr, "[perfre] roundtrip common: key=%d chorusType=%d reverbType=%d rl=%d rt=%d rfb=%d cl=%d cd=%d cr=%d cfb=%d cout=%d res0=%d res7=%d\n",
                 performanceCommon.keyMode, performanceCommon.chorusType, performanceCommon.reverbType, performanceCommon.reverbLevel,
                 performanceCommon.reverbTime, performanceCommon.reverbFeedback, performanceCommon.chorusLevel, performanceCommon.chorusDepth,
                 performanceCommon.chorusRate, performanceCommon.chorusFeedback, performanceCommon.chorusOutput,
                 performanceCommon.voiceReserve[0], performanceCommon.voiceReserve[7]);
    for (int i = 0; i < kNumPerformanceParts; i++) {
      auto &p = performanceParts[i];
      std::fprintf(stderr, "[perfre] part %d: '%s' bank=%d num=%d ch=%d lvl=%d pan=%d coarse=%d fine=%d en=%d rev=%d cho=%d pc=%d vol=%d hold=%d\n", i, p.name,
                   p.bank, p.number, p.midiChannel, p.level, p.pan, p.coarseTune, p.fineTune, p.enabled, p.reverbSwitch, p.chorusSwitch,
                   p.rxProgramChange, p.rxVolume, p.rxHold1);
    }
    // Panel-style load: Bank Select + Program Change on the control channel (16), then poll.
    lastPerfSysexMs = 0;
    for (int bankSel : {81, 80}) for (int prog : {1, 2}) {
      uint8_t msg[5] = {0xBF, 0, (uint8_t)bankSel, 0xCF, (uint8_t)prog};
      mcu->postMidiSC55(msg, 3);
      runMs(100);
      mcu->postMidiSC55(msg + 3, 2);
      runMs(600);
      const bool changed = pollPerformanceFromFirmware();
      std::fprintf(stderr, "[perfre] panel load bank%d pc%d: poll imported=%d name='%s' p0='%s'(b%d n%d) p1='%s' p7='%s' rev=%d lvl0=%d\n", bankSel, prog, changed,
                   performanceName, performanceParts[0].name, performanceParts[0].bank, performanceParts[0].number, performanceParts[1].name,
                   performanceParts[7].name, performanceCommon.reverbType, performanceParts[0].level);
    }
    std::exit(0);
  }

  // GitHub issue #2: Performance mode must survive a patch click in Browse (setCurrentProgram()), and a
  // Part must obey Channel Volume (CC7). Headless: prints the mode byte and the audio peaks.
  // Expansion swaps: does the firmware report "Wave number error" on the LCD after changing board?
  if (std::getenv("JV880_SELFTEST_WAVEERR")) {
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);
    auto runMs = [&](int ms) {
      for (int done = 0; done < sampleRate * ms / 1000; done += (int)blockFrames) {
        servicePendingPatch((int)blockFrames);
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
      }
    };
    auto lcdText = [&] { return juce::String((const char *)mcu->lcd.LCD_Data, 80); };
    runMs(3000);
    std::vector<int> picks;
    for (int g = 2; g < userGroupIndex; g++)
      if (romInfos[g + romCountRequired].loaded && !patchInfoPerGroup[g].empty())
        picks.push_back(g);
    int bad = 0, total = 0;
    const int rounds = std::getenv("JV880_SELFTEST_ROUNDS") ? atoi(std::getenv("JV880_SELFTEST_ROUNDS")) : 3;
    for (int round = 0; round < rounds; round++)
      for (size_t i = 0; i < picks.size(); i++) {
        const int g = picks[(i * 7 + round * 3) % picks.size()];
        const auto &grp = patchInfoPerGroup[g];
        const int idx = grp[(size_t)((round * 41 + i * 13 + 5) % (int)grp.size())]->iInList;
        setCurrentProgram(idx);
        runMs(std::getenv("JV880_SELFTEST_POST_MS") ? atoi(std::getenv("JV880_SELFTEST_POST_MS")) : 3000);
        const auto t = lcdText();
        total++;
        if (t.containsIgnoreCase("wave")) {
          bad++;
          setCurrentProgram(idx); // same board already loaded: no swap this time
          runMs(1500);
          fprintf(stderr, "[waveerr]   retry without swap: %s\n", lcdText().toRawUTF8());
        }
        fprintf(stderr, "[waveerr] patch %d exp %d: %s\n", idx, patchInfos[idx].expansionI, t.toRawUTF8());
      }
    fprintf(stderr, "[waveerr] %d / %d swaps ended in an error\n", bad, total);
    exit(0);
  }

  if (std::getenv("JV880_SELFTEST_PERF3")) {
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);
    auto runMs = [&](int ms) {
      float peak = 0;
      for (int done = 0; done < sampleRate * ms / 1000; done += (int)blockFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        for (unsigned int i = 0; i < blockFrames; i++)
          peak = std::max(peak, std::max(std::abs(l[i]), std::abs(r[i])));
      }
      return peak;
    };
    auto note = [&](uint8_t ch) {
      uint8_t on[3] = {(uint8_t)(0x90 | ch), 60, 100}, off[3] = {(uint8_t)(0x80 | ch), 60, 0};
      mcu->postMidiSC55(on, 3);
      const float p = runMs(800);
      mcu->postMidiSC55(off, 3);
      runMs(600);
      return p;
    };
    runMs(2000);
    sendPatchToPerformancePart(0, 0);
    setPerformancePartParams(0, 1, 127, 64, true);
    sendPatchToPerformancePart(64, 1);
    setPerformancePartParams(1, 2, 127, 64, true);
    setPerformanceModeEnabled(true);
    runMs(1500);
    std::fprintf(stderr, "[selftest-perf3] perf on: nvram[0x11]=%02x flag=%d peak(ch1)=%.4f\n", mcu->nvram[0x11], (int)performanceModeEnabled, note(0));
    setCurrentProgram(10); // a tone patch, as a click in Browse
    std::fprintf(stderr, "[selftest-perf3] right after click: nvram[0x11]=%02x\n", mcu->nvram[0x11]);
    for (int i = 0; i < 6; i++) { runMs(100); std::fprintf(stderr, "[selftest-perf3]   +%d00ms: nvram[0x11]=%02x\n", i + 1, mcu->nvram[0x11]); }
    runMs(900);
    std::fprintf(stderr, "[selftest-perf3] after tone click: nvram[0x11]=%02x flag=%d peak(ch1)=%.4f peak(ch2)=%.4f\n", mcu->nvram[0x11], (int)performanceModeEnabled, note(0), note(1));
    setCurrentProgram(192); // a Rhythm Set: engine reset path
    runMs(3000);
    std::fprintf(stderr, "[selftest-perf3] after rhythm click: nvram[0x11]=%02x flag=%d peak(ch1)=%.4f peak(ch2)=%.4f\n", mcu->nvram[0x11], (int)performanceModeEnabled, note(0), note(1));
    uint8_t cc0[3] = {0xB0, 7, 0}, cc127[3] = {0xB0, 7, 127};
    mcu->postMidiSC55(cc0, 3);
    runMs(100);
    const float quiet = note(0);
    mcu->postMidiSC55(cc127, 3);
    runMs(100);
    std::fprintf(stderr, "[selftest-perf3] CC7=0 peak=%.4f, CC7=127 peak=%.4f\n", quiet, note(0));
    // GitHub issue #5: master volume survives a state save/restore, and an old-size blob leaves it alone.
    setMasterVolume(0.37f);
    juce::MemoryBlock block;
    getStateInformation(block);
    setMasterVolume(1.0f);
    setStateInformation(block.getData(), (int)block.getSize());
    std::fprintf(stderr, "[selftest-perf3] state %zu bytes, master volume after restore = %.2f (expected 0.37)\n", block.getSize(), getMasterVolume());
    setMasterVolume(0.5f);
    setStateInformation(block.getData(), (int)sizeof(DataToSave));
    std::fprintf(stderr, "[selftest-perf3] old-format blob keeps the volume: %.2f (expected 0.50)\n", getMasterVolume());
    std::exit(0);
  }

  // GitHub issue #4: JV880_SELFTEST_SYXIMPORT=<file.syx|.mid> runs the SysEx import with the emulator pumped by hand.
  if (const char *syxPath = std::getenv("JV880_SELFTEST_SYXIMPORT")) {
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);
    float peak = 0;
    auto runMs = [&](int ms) {
      for (int done = 0; done < sampleRate * ms / 1000; done += (int)blockFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        for (unsigned int i = 0; i < blockFrames; i++) peak = std::max(peak, std::abs(l[i]));
      }
    };
    runMs(2000);
    setCurrentProgram(0);
    runMs(500);
    const auto report = importSysexFile(juce::File(syxPath),
        [](int i, int n, const juce::String &name) { std::fprintf(stderr, "[selftest-syx] %d/%d %s\n", i + 1, n, name.toRawUTF8()); return true; },
        runMs);
    std::fprintf(stderr, "[selftest-syx] imported=%d failed=%d error=%s\n", report.imported, report.failed, report.error.toRawUTF8());
    for (auto &n : report.notes) std::fprintf(stderr, "[selftest-syx] note: %s\n", n.toRawUTF8());
    for (auto &n : report.names) std::fprintf(stderr, "[selftest-syx] name: %s\n", n.toRawUTF8());
    // Load the first imported patch and play it.
    if (report.imported > 0 && !patchInfoPerGroup[userGroupIndex].empty()) {
      for (auto *info : patchInfoPerGroup[userGroupIndex]) {
        if (!info->present || info->drums) continue;
        std::fprintf(stderr, "[selftest-syx] user patch \"%.*s\" (index %d)\n", info->nameLength, info->name, info->iInList);
      }
      setCurrentProgram(patchInfoPerGroup[userGroupIndex].back()->iInList);
      runMs(500);
      peak = 0;
      uint8_t on[3] = {0x90, 60, 100};
      mcu->postMidiSC55(on, 3);
      runMs(800);
      std::fprintf(stderr, "[selftest-syx] last imported patch plays: peak=%.4f\n", peak);
    }
    std::exit(0);
  }

  // Does the Performance Part 8 "Internal" Rhythm Set (bank 0) live at Rhythm Temp (nvram[0x67f0])? If so,
  // an expansion/User rhythm set can be injected there like tone patches go into Internal slots 1-7
  // (GitHub issue #2, last comment). Proof by sound: Part 8 on Preset B (bank 3) vs Part 8 on the Internal
  // set after copying Preset B's bytes into Rhythm Temp must render the same drums.
  if (std::getenv("JV880_SELFTEST_RHYMEM")) {
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);
    struct Stats { double rms; float peak; };
    auto runMs = [&](int ms, bool measure) {
      double sum = 0; float peak = 0; long n = 0;
      for (int done = 0; done < sampleRate * ms / 1000; done += (int)blockFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        if (measure)
          for (unsigned int i = 0; i < blockFrames; i++) { sum += l[i] * l[i]; peak = std::max(peak, std::abs(l[i])); n++; }
      }
      return Stats{ n ? std::sqrt(sum / n) : 0.0, peak };
    };
    auto hit = [&](int note) {
      uint8_t on[3] = {0x99, (uint8_t)note, 110}, off[3] = {0x89, (uint8_t)note, 0};
      mcu->postMidiSC55(on, 3);
      const Stats st = runMs(700, true);
      mcu->postMidiSC55(off, 3);
      runMs(500, false);
      return st;
    };
    runMs(2000, false);
    sendPatchToPerformancePart(192, 7);
    setPerformancePartParams(7, 10, 127, 64, true);
    setPerformanceModeEnabled(true);
    runMs(1500, false);
    for (int note : {36, 38, 42}) {
      sendPatchToPerformancePart(193, 7); // Preset B rhythm set
      runMs(500, false);
      const Stats b = hit(note);
      sendPatchToPerformancePart(194, 7); // Internal rhythm set (bank 0)
      runMs(500, false);
      const Stats internal = hit(note);
      mcuLock.enter();
      memcpy(&mcu->nvram[0x67f0], patchInfos[193].ptr, 0xa7c); // Preset B's bytes into Rhythm Temp
      mcuLock.exit();
      sendPatchToPerformancePart(194, 7);
      runMs(500, false);
      const Stats injected = hit(note);
      std::fprintf(stderr, "[selftest-rhymem] note %d: PresetB rms=%.5f | Internal rms=%.5f | Internal after copy rms=%.5f\n",
                   note, b.rms, internal.rms, injected.rms);
    }
    // An expansion rhythm set through the real API.
    int exp = -1;
    for (int i = 195; i < romPatchCapacity + userPatchCapacity; i++)
      if (patchInfos[i].present && patchInfos[i].drums && patchInfos[i].expansionI != 0xff) { exp = i; break; }
    if (exp < 0) {
      // No expansion loads in this sandbox: save Preset B's rhythm set as a User rhythm set instead (a non-native
      // rhythm set all the same, so it takes the same injection path).
      std::fprintf(stderr, "[selftest-rhymem] no expansion rhythm set here, using a saved User rhythm set\n");
      setCurrentProgram(193);
      if (saveCurrentPatchAs(userPatchesDir().getChildFile("SelfTestRhythm.jvp")) && !patchInfoPerGroup[userGroupIndex].empty())
        exp = patchInfoPerGroup[userGroupIndex].back()->iInList;
    }
    if (exp >= 0) {
      std::fprintf(stderr, "[selftest-rhymem] expansion rhythm set index %d (%s), eligible=%d\n", exp,
                   std::string(patchInfos[exp].name, (size_t)std::min(patchInfos[exp].nameLength, 16)).c_str(),
                   (int)isEligibleForPerformancePart(exp));
      sendPatchToPerformancePart(exp, 7);
      runMs(800, false);
      for (int note : {36, 38, 42})
        std::fprintf(stderr, "[selftest-rhymem]   expansion set, note %d: rms=%.5f\n", note, hit(note).rms);
      std::fprintf(stderr, "[selftest-rhymem] Part 8: bank=%d number=%d name=%s\n", performanceParts[7].bank, performanceParts[7].number, performanceParts[7].name);
    }
    std::exit(0);
  }

  // Headless validation for the shipped Performance mode v2 API (sendPatchToPerformancePart(),
  // setPerformanceModeEnabled(), etc.) - as opposed to JV880_SELFTEST_PERF above, which only
  // exercised raw DT1 messages to find the address map in the first place. Same technique: drive
  // the emulator directly, no audio device or window needed, dump the firmware's own rendered
  // LCD screen as a PNG - see CLAUDE.md's "Mode Performance" section.
  if (std::getenv("JV880_SELFTEST_PERF2")) {
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);

    auto runMs = [&](int ms) {
      int totalFrames = sampleRate * ms / 1000;
      int done = 0;
      while (done < totalFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        done += (int)blockFrames;
      }
    };

    std::fprintf(stderr, "[selftest-perf2] booting 2000 ms...\n");
    runMs(2000);

    setPerformanceName("MyBand");
    sendPatchToPerformancePart(0, 0);   // Internal A #1 -> Part 1
    setPerformancePartParams(0, 1, 127, 64, true);
    sendPatchToPerformancePart(64, 1);  // Internal B #1 -> Part 2
    setPerformancePartParams(1, 2, 100, 64, true);
    sendPatchToPerformancePart(192, 7); // Rhythm Set Int A -> Part 8 (Rhythm)
    setPerformancePartParams(7, 10, 110, 64, true);

    std::fprintf(stderr, "[selftest-perf2] enabling Performance mode\n");
    setPerformanceModeEnabled(true);
    runMs(1500);

    // Audio check: note on channel 1 (Part 1) and channel 2 (Part 2) simultaneously - both
    // should be audible together through the single engine.
    {
      uint8_t note1On[3] = {0x90, 60, 100};
      uint8_t note2On[3] = {0x91, 64, 100};
      mcu->postMidiSC55(note1On, 3);
      mcu->postMidiSC55(note2On, 3);
      float peakL = 0, peakR = 0;
      int totalFrames = sampleRate;
      int done = 0;
      while (done < totalFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        for (unsigned int i = 0; i < blockFrames; i++) {
          peakL = std::max(peakL, std::abs(l[i]));
          peakR = std::max(peakR, std::abs(r[i]));
        }
        done += (int)blockFrames;
      }
      std::fprintf(stderr, "[selftest-perf2] audio check (Part1 ch1 + Part2 ch2 together): peakL=%.4f peakR=%.4f\n", peakL, peakR);
    }

    if (auto *bitmapResult = (uint8_t *)mcu->lcd.LCD_Update()) {
      for (size_t i = 0; i < 1024 * 1024; i++) bitmapResult[i * 4 + 3] = 0xff;
      juce::Image image(juce::Image::PixelFormat::ARGB, 820, 100, false);
      juce::Image::BitmapData pixelMap(image, juce::Image::BitmapData::readWrite);
      for (int y = 0; y < pixelMap.height; y++)
        std::memcpy(pixelMap.getLinePointer(y), bitmapResult + (y * 1024 * 4), (size_t)pixelMap.lineStride);
      juce::PNGImageFormat png;
      juce::File outFile("/tmp/jv880_lcd_perf2.png");
      juce::FileOutputStream stream(outFile);
      if (stream.openedOk()) {
        stream.setPosition(0);
        stream.truncate();
        png.writeImageToStream(image, stream);
        std::fprintf(stderr, "[selftest-perf2] wrote LCD screenshot to /tmp/jv880_lcd_perf2.png\n");
      }
    }

    std::fprintf(stderr, "[selftest-perf2] disabling Performance mode\n");
    setPerformanceModeEnabled(false);
    std::fprintf(stderr, "[selftest-perf2] nvram[0x11] right after disable = %02x\n", mcu->nvram[0x11]);
    runMs(3000);
    std::fprintf(stderr, "[selftest-perf2] nvram[0x11] after 3s settle = %02x\n", mcu->nvram[0x11]);
    if (auto *bitmapResult = (uint8_t *)mcu->lcd.LCD_Update()) {
      for (size_t i = 0; i < 1024 * 1024; i++) bitmapResult[i * 4 + 3] = 0xff;
      juce::Image image(juce::Image::PixelFormat::ARGB, 820, 100, false);
      juce::Image::BitmapData pixelMap(image, juce::Image::BitmapData::readWrite);
      for (int y = 0; y < pixelMap.height; y++)
        std::memcpy(pixelMap.getLinePointer(y), bitmapResult + (y * 1024 * 4), (size_t)pixelMap.lineStride);
      juce::PNGImageFormat png;
      juce::File outFile("/tmp/jv880_lcd_backtopatch.png");
      juce::FileOutputStream stream(outFile);
      if (stream.openedOk()) {
        stream.setPosition(0);
        stream.truncate();
        png.writeImageToStream(image, stream);
        std::fprintf(stderr, "[selftest-perf2] wrote back-to-Patch LCD screenshot to /tmp/jv880_lcd_backtopatch.png\n");
      }
    }

    std::fprintf(stderr, "[selftest-perf2] done, exiting\n");
    std::exit(0);
  }

  // Headless investigation for unlocking expansion/User patches in Performance Parts (Alan's
  // request, 2026-09-08 - see CLAUDE.md's "Mode Performance v2" section). A Performance Part can
  // only reference the firmware's own Patch Memory by bank/number, never inline bytes - the 195
  // ROM-native patches already live there verbatim (see performancePatchMapping()), but
  // expansion/User patches only exist as raw bytes this project injects directly, so they'd need
  // to actually be WRITTEN into the real writable "Internal" Patch Memory bank first.
  // RolandJV880.java's own prepareBuffer() (the single-Patch editor, not Multi) gives the write
  // address for that bank: AA=1 (Internal), BB=number+0x40, CC=0x20, DD=0 for the Patch Common
  // record, CC+=(t+7) for each of the 4 Tones (t=1..4) - this locates where that lands in `nvram`
  // by writing just a 12-byte name marker (plain ASCII on the wire, no nibble encoding unlike
  // most other Patch fields) and searching for it, rather than assuming the wire layout matches
  // this project's own internal Patch struct (dataStructures.h) byte-for-byte.
  if (std::getenv("JV880_SELFTEST_PATCHMEM")) {
    const int sampleRate = 44100;
    const unsigned int blockFrames = 512;
    std::vector<float> l(blockFrames), r(blockFrames);

    auto runMs = [&](int ms) {
      int totalFrames = sampleRate * ms / 1000;
      int done = 0;
      while (done < totalFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        done += (int)blockFrames;
      }
    };

    auto sendDT1 = [&](uint8_t AA, uint8_t BB, uint8_t CC, uint8_t DD, const std::vector<uint8_t> &payload) {
      std::vector<uint8_t> msg = {0xF0, 0x41, 0x10, 0x46, 0x12, AA, BB, CC, DD};
      uint32_t sum = AA + BB + CC + DD;
      for (auto b : payload) { msg.push_back(b); sum += b; }
      uint8_t checksum = (uint8_t)((0x80 - (sum & 0x7F)) & 0x7F);
      msg.push_back(checksum);
      msg.push_back(0xF7);
      mcu->postMidiSC55(msg.data(), (int)msg.size());
    };

    std::fprintf(stderr, "[selftest-patchmem] booting 2000 ms...\n");
    runMs(2000);

    // Slot 0 gets "PATCHMEMTEST", slot 1 gets "SECONDSLOT01" - two different markers so a
    // found stride between them confirms per-slot spacing, not just the base.
    std::fprintf(stderr, "[selftest-patchmem] writing name markers to Internal Patch Memory slots 0 and 1\n");
    std::vector<uint8_t> name0(12, ' ');
    { const char *n = "PATCHMEMTEST"; for (int i = 0; i < 12; i++) name0[(size_t)i] = (uint8_t)n[i]; }
    sendDT1(1, 0x40, 0x20, 0x00, name0); // AA=1 Internal, BB=0+0x40, CC=0x20 Patch Memory Common
    runMs(300);

    std::vector<uint8_t> name1(12, ' ');
    { const char *n = "SECONDSLOT01"; for (int i = 0; i < 12; i++) name1[(size_t)i] = (uint8_t)n[i]; }
    sendDT1(1, 0x41, 0x20, 0x00, name1); // slot 1
    runMs(2000);

    // Control: RolandJV880.java's prepareBuffer() targets AA=0,BB=8,CC=0x20,DD=0 for
    // "toWorkingMemory" (i.e. Patch Temp itself) - a DIFFERENT address than AA=1,BB=0x40 (Internal
    // Patch Memory slot 0) above. If this also lands at 0x0d70, slot 0 of Internal Patch Memory
    // and Patch Temp are genuinely the same NVRAM cells on this firmware; if it lands somewhere
    // else, the slot0 result above was something else entirely (e.g. the write silently fell
    // through to Temp regardless of AA/BB).
    std::vector<uint8_t> name2(12, ' ');
    { const char *n = "WORKINGMEM01"; for (int i = 0; i < 12; i++) name2[(size_t)i] = (uint8_t)n[i]; }
    sendDT1(0, 0x08, 0x20, 0x00, name2);
    runMs(2000);

    auto searchFor12 = [&](const char *label, const char *needle) {
      for (size_t i = 0; i + 12 <= sizeof(mcu->nvram); i++) {
        if (std::memcmp(&mcu->nvram[i], needle, 12) == 0)
          std::fprintf(stderr, "[selftest-patchmem] FOUND %s in nvram at offset 0x%04zx\n", label, i);
      }
      for (size_t i = 0; i + 12 <= sizeof(mcu->sram); i++) {
        if (std::memcmp(&mcu->sram[i], needle, 12) == 0)
          std::fprintf(stderr, "[selftest-patchmem] FOUND %s in sram at offset 0x%04zx\n", label, i);
      }
    };
    searchFor12("slot0 marker (PATCHMEMTEST)", "PATCHMEMTEST");
    searchFor12("slot1 marker (SECONDSLOT01)", "SECONDSLOT01");
    searchFor12("working-memory marker (WORKINGMEM01)", "WORKINGMEM01");

    // End-to-end check of the real shipped API: assign the first expansion-ROM patch found
    // (patchInfoPerGroup[1], if any expansion is installed), else fall back to a freshly-saved
    // User patch (saveCurrentPatchAs() on whatever's currently loaded - this sandbox has no
    // expansion ROM installed), to Performance Part 3, enable Performance mode, and confirm it
    // actually plays.
    int expansionPatchIndex = -1;
    if (patchInfoPerGroup.size() > 1 && !patchInfoPerGroup[1].empty()) {
      expansionPatchIndex = patchInfoPerGroup[1][0]->iInList;
    } else {
      std::fprintf(stderr, "[selftest-patchmem] no expansion ROM installed, falling back to a User patch\n");
      setCurrentProgram(0); // status.patch is all-zero (silent) until a real patch is loaded once
      auto file = userPatchesDir().getChildFile("SelfTestUser.jvp");
      if (saveCurrentPatchAs(file) && !patchInfoPerGroup[userGroupIndex].empty())
        expansionPatchIndex = patchInfoPerGroup[userGroupIndex].back()->iInList;
    }

    if (expansionPatchIndex >= 0) {
      std::fprintf(stderr, "[selftest-patchmem] assigning patch \"%.*s\" (index %d) to Part 3\n",
                   patchInfos[expansionPatchIndex].nameLength, patchInfos[expansionPatchIndex].name,
                   expansionPatchIndex);
      sendPatchToPerformancePart(expansionPatchIndex, 2);
      setPerformancePartParams(2, 3, 127, 64, true);
      setPerformanceModeEnabled(true);
      runMs(3000);

      uint8_t noteOn[3] = {0x92, 60, 100};
      mcu->postMidiSC55(noteOn, 3);
      float peakL = 0, peakR = 0;
      int totalFrames = sampleRate;
      int done = 0;
      while (done < totalFrames) {
        mcu->updateSC55WithSampleRate(l.data(), r.data(), blockFrames, sampleRate);
        for (unsigned int i = 0; i < blockFrames; i++) {
          peakL = std::max(peakL, std::abs(l[i]));
          peakR = std::max(peakR, std::abs(r[i]));
        }
        done += (int)blockFrames;
      }
      std::fprintf(stderr, "[selftest-patchmem] audio check (expansion patch on Part 3, ch3): peakL=%.4f peakR=%.4f\n", peakL, peakR);

      if (auto *bitmapResult = (uint8_t *)mcu->lcd.LCD_Update()) {
        for (size_t i = 0; i < 1024 * 1024; i++) bitmapResult[i * 4 + 3] = 0xff;
        juce::Image image(juce::Image::PixelFormat::ARGB, 820, 100, false);
        juce::Image::BitmapData pixelMap(image, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < pixelMap.height; y++)
          std::memcpy(pixelMap.getLinePointer(y), bitmapResult + (y * 1024 * 4), (size_t)pixelMap.lineStride);
        juce::PNGImageFormat png;
        juce::File outFile("/tmp/jv880_lcd_expansion.png");
        juce::FileOutputStream stream(outFile);
        if (stream.openedOk()) {
          stream.setPosition(0);
          stream.truncate();
          png.writeImageToStream(image, stream);
          std::fprintf(stderr, "[selftest-patchmem] wrote LCD screenshot to /tmp/jv880_lcd_expansion.png\n");
        }
      }
    } else {
      std::fprintf(stderr, "[selftest-patchmem] no patch available for the end-to-end check, skipping\n");
    }

    std::fprintf(stderr, "[selftest-patchmem] done, exiting\n");
    std::exit(0);
  }

  // Alan's report, 2026-09-09: "Send to Sequencer: 1" from Browse doesn't stick, the track
  // stays "(Empty)". Isolates the storage plumbing (setSequencerTrackPatch()/
  // getTrackPatch()) from the GUI menu-click routing, to tell whether the bug is in the data
  // layer or purely in PatchBrowser's popup-menu handling.
  if (std::getenv("JV880_SELFTEST_SEQPATCH")) {
    std::fprintf(stderr, "[selftest-seqpatch] patchInfoPerGroup[0].size()=%zu\n",
                 patchInfoPerGroup.empty() ? (size_t)0 : patchInfoPerGroup[0].size());
    if (!patchInfoPerGroup.empty() && !patchInfoPerGroup[0].empty()) {
      const int firstIndex = patchInfoPerGroup[0][0]->iInList;
      std::fprintf(stderr, "[selftest-seqpatch] first patch iInList=%d present=%d drums=%d name=%.*s\n",
                   firstIndex, (int)patchInfos[firstIndex].present, (int)patchInfos[firstIndex].drums,
                   patchInfos[firstIndex].nameLength, patchInfos[firstIndex].name);

      const auto before = sequencerEngine.getTrackPatch(0);
      std::fprintf(stderr, "[selftest-seqpatch] before: index=%d name=%s\n", before.index,
                   before.name.toRawUTF8());

      setSequencerTrackPatch(0, firstIndex);

      const auto after = sequencerEngine.getTrackPatch(0);
      std::fprintf(stderr, "[selftest-seqpatch] after: index=%d name=%s expansionI=%d isRhythm=%d\n",
                   after.index, after.name.toRawUTF8(), (int)after.expansionI, (int)after.isRhythm);
    } else {
      std::fprintf(stderr, "[selftest-seqpatch] no patches in group 0, can't test\n");
    }
    std::fprintf(stderr, "[selftest-seqpatch] done, exiting\n");
    std::exit(0);
  }

  return true;
}

juce::File VirtualJVProcessor::getRomsFolder() {
  return juce::File(getEffectiveRomsDirectory());
}

void VirtualJVProcessor::setRomsFolderOverride(const juce::File &dir) {
  setRomsDirectoryOverride(dir == juce::File{} ? std::string() : dir.getFullPathName().toStdString());
  savePersistedRomFolderOverride(dir);

  // Only meaningful to actually re-point the live engine at while it's still unloaded - see
  // attemptLoadRoms()'s own comment for why switching ROM sets under an already-running engine
  // isn't supported. Once loaded, this has already done its job (persisted + updated rom.cpp's
  // override for next launch); nothing here to unwind.
  if (loaded)
    return;

  for (auto &ptr : loadedRoms) {
    if (ptr != nullptr) {
      free(ptr);
      ptr = nullptr;
    }
  }
  for (auto &info : romInfos)
    info.loaded = false;
}

bool VirtualJVProcessor::retryLoadRoms() {
  if (loaded)
    return true;

  if (!attemptLoadRoms())
    return false;

  if (auto editor = safeActiveEditor())
    if (auto e = dynamic_cast<VirtualJVEditor *>(editor))
      e->romsBecameAvailable();

  return true;
}

void VirtualJVProcessor::setDisplayMode(DisplayMode mode) {
  displayMode = mode;
  savePersistedDisplayMode(mode);

  if (auto editor = safeActiveEditor())
    if (auto e = dynamic_cast<VirtualJVEditor *>(editor))
      e->refreshDisplayMode();
}

// --- Sequencer (Standalone only) -----------------------------------------------------------

juce::File VirtualJVProcessor::sequencerStateFile() {
  return appDataBaseDir().getChildFile("sequencer_state.midiseq");
}

void VirtualJVProcessor::saveSequencerState() {
  jivseq::exportSongsFile(sequencerEngine, sequencerStateFile());
}

void VirtualJVProcessor::loadSequencerState() {
  auto file = sequencerStateFile();
  if (!file.existsAsFile())
    return;
  jivseq::importSongsFile(sequencerEngine, file);

  // Re-resolve each track's stored patch (see JivSequencerEngine::TrackPatch's own comment):
  // the raw index saved last session may no longer point at the same patch if the set of
  // loaded expansion ROMs/User patches has changed since. Only needed once here, right after
  // import - every other read of a track's patch (handleAsyncUpdate(), getTrackPatchName())
  // just trusts whatever index setTrackPatch() last stored, same as PerformancePart does with
  // its own bank/number.
  for (int t = 0; t < kNumPerformanceParts; ++t) {
    auto patch = sequencerEngine.getTrackPatch(t);
    if (patch.index < 0)
      continue;
    const bool stillValid = patch.index < romPatchCapacity + userPatchCapacity
                             && patchInfos[patch.index].present
                             && patchInfos[patch.index].drums == patch.isRhythm
                             && (int)patchInfos[patch.index].expansionI == patch.expansionI
                             && juce::String(patchInfos[patch.index].name,
                                              (size_t)patchInfos[patch.index].nameLength) == patch.name;
    if (stillValid)
      continue;

    patch.index = findPatchInfoIndexByIdentity(patch.name, patch.expansionI, patch.isRhythm);
    sequencerEngine.setTrackPatch(t, patch);
  }
}

int VirtualJVProcessor::findPatchInfoIndexByIdentity(const juce::String &name, uint8_t expansionI,
                                                       bool isRhythm) const {
  for (int i = 0; i < romPatchCapacity + userPatchCapacity; ++i) {
    auto &info = patchInfos[i];
    if (!info.present || info.drums != isRhythm || (int)info.expansionI != expansionI)
      continue;
    if (juce::String(info.name, (size_t)info.nameLength) == name)
      return i;
  }
  return -1;
}

// Doesn't discard any recorded songs either way - disabling only hides the drawer/stops the
// transport (see VirtualJVEditor::refreshSequencerVisibility()), re-enabling just shows it
// again with whatever was already there. State is saved right here on the way to disabled
// (there'll be no destructor-time save to rely on once sequencerEnabled is false - see
// ~VirtualJVProcessor()) and reloaded on the way to enabled, in case it was disabled, edited
// externally (unlikely but cheap to guard against), then re-enabled within the same session.
void VirtualJVProcessor::setSequencerEnabled(bool enabled) {
  if (enabled == sequencerEnabled)
    return;

  if (!enabled)
    saveSequencerState();

  sequencerEnabled = enabled;
  savePersistedSequencerEnabled(enabled);

  if (enabled)
    loadSequencerState();

  if (auto editor = safeActiveEditor())
    if (auto e = dynamic_cast<VirtualJVEditor *>(editor))
      e->refreshSequencerVisibility();
}

void VirtualJVProcessor::setSequencerView(seqview::View view) {
  const bool retro = view == seqview::View::retro;
  const bool grid = view == seqview::View::grid;
  if (retro == sequencerRetroMode && grid == sequencerGridMode)
    return;
  sequencerRetroMode = retro;
  sequencerGridMode = grid;
  savePersistedSequencerView(sequencerGridMode, sequencerRetroMode, gridRowHeight);

  if (auto editor = safeActiveEditor())
    if (auto e = dynamic_cast<VirtualJVEditor *>(editor))
      e->refreshSequencerVisibility();
}

void VirtualJVProcessor::setKeyboardNumOctaves(int numOctaves) {
  keyboardNumOctaves = juce::jlimit(0, 4, numOctaves);
  savePersistedKeyboardOctaves(keyboardNumOctaves);
}

void VirtualJVProcessor::setWindowSize(int w, int h) {
  if (w == savedWindowW && h == savedWindowH)
    return;
  savedWindowW = w;
  savedWindowH = h;
  savePersistedWindowSize(w, h);
}

juce::String VirtualJVProcessor::favoriteKey(int patchIndex) const {
  if (patchIndex < 0 || patchIndex >= romPatchCapacity + userPatchCapacity || !patchInfos[patchIndex].present)
    return {};
  const auto &p = patchInfos[patchIndex];
  return juce::String(p.name, p.nameLength) + "|" + juce::String(p.expansionI) + "|" + (p.drums ? "R" : "T");
}

bool VirtualJVProcessor::isFavorite(int patchIndex) const {
  const auto k = favoriteKey(patchIndex);
  return k.isNotEmpty() && favoriteKeys.count(k) > 0;
}

void VirtualJVProcessor::toggleFavorite(int patchIndex) {
  const auto k = favoriteKey(patchIndex);
  if (k.isEmpty())
    return;
  if (!favoriteKeys.erase(k))
    favoriteKeys.insert(k);
  juce::StringArray lines;
  for (auto &s : favoriteKeys)
    lines.add(s);
  favoritesFile().replaceWithText(lines.joinIntoString("\n") + "\n");
}

void VirtualJVProcessor::setPaneHeights(int keyboard, int sequencer) {
  keyboardPaneH = keyboard;
  sequencerPaneH = sequencer;
  savePersistedPaneHeights(keyboard, sequencer, keyboardCollapsed, sequencerCollapsed);
}

void VirtualJVProcessor::setPaneCollapsed(bool keyboard, bool sequencer) {
  keyboardCollapsed = keyboard;
  sequencerCollapsed = sequencer;
  savePersistedPaneHeights(keyboardPaneH, sequencerPaneH, keyboardCollapsed, sequencerCollapsed);
}

void VirtualJVProcessor::setRetroKeyBindings(const juce::String &encoded) {
  if (encoded == retroKeyBindings)
    return;
  retroKeyBindings = encoded;
  savePersistedSequencerRetro(retroLcdCompactMode, retroKeyBindings);
}

void VirtualJVProcessor::setRetroLcdCompactMode(bool compact) {
  if (compact == retroLcdCompactMode)
    return;
  retroLcdCompactMode = compact;
  savePersistedSequencerRetro(retroLcdCompactMode, retroKeyBindings);
}

void VirtualJVProcessor::setGridRowHeight(int pixels) {
  pixels = juce::jlimit(0, 200, pixels);
  if (pixels == gridRowHeight)
    return;
  gridRowHeight = pixels;
  savePersistedSequencerView(sequencerGridMode, sequencerRetroMode, gridRowHeight);
}

void VirtualJVProcessor::auditionTrackNote(int track, int note, int velocity, bool on) {
  injectTestNote(sequencerEngine.channelForTrack(track), note, static_cast<float>(velocity) / 127.0f, on);
}

void VirtualJVProcessor::exportSequencerSongs(const juce::File &file) {
  jivseq::exportSongsFile(sequencerEngine, file);
}

void VirtualJVProcessor::importSequencerSongs(const juce::File &file) {
  jivseq::importSongsFile(sequencerEngine, file);
}

// Right-click STOP - all notes off on every channel, straight into the firmware's own MIDI IN,
// same mechanism as any other direct-injected message in this file (e.g. the self-test code
// above). Called from the message thread (a mouse click), so - unlike the audio-thread-only
// processBlock() sequencer block below - this needs mcuLock itself, same convention
// sendSysexParamChange() already follows for its own UI-thread callers.
void VirtualJVProcessor::midiPanic() {
  mcuLock.enter();
  for (int channel = 1; channel <= 16; ++channel) {
    uint8_t allNotesOff[3] = {static_cast<uint8_t>(0xB0 | (channel - 1)), 123, 0};
    mcu->postMidiSC55(allNotesOff, 3);
  }
  mcuLock.exit();
}

int VirtualJVProcessor::getTrackVolumeHint(int track) const {
  if (track < 0 || track >= kNumPerformanceParts)
    return -1;
  return performanceParts[track].level;
}

int VirtualJVProcessor::getTrackPanHint(int track) const {
  if (track < 0 || track >= kNumPerformanceParts)
    return -1;
  return performanceParts[track].pan;
}

// See sequencerEngine's own comment in PluginProcessor.h - resolves patchInfoIndex into the
// same content-identity PerformancePart itself stores (bank/number aren't kept here: unlike
// PerformancePart, an expansion/User patch's actual bytes still need injecting at push time -
// see handleAsyncUpdate() - and that's index-driven, not bank/number-driven, see
// sendPatchToPerformancePart()'s own comment on why injected patches land at bank=0,
// number=partIndex+1 regardless of their real identity).
void VirtualJVProcessor::setSequencerTrackPatch(int track, int patchInfoIndex) {
  if (track < 0 || track >= kNumPerformanceParts)
    return;
  if (patchInfoIndex < 0 || patchInfoIndex >= romPatchCapacity + userPatchCapacity
      || !patchInfos[patchInfoIndex].present)
    return;

  auto &info = patchInfos[patchInfoIndex];
  const bool wantsRhythmPart = (track == kNumPerformanceParts - 1);
  if (info.drums != wantsRhythmPart)
    return;

  jivseq::JivSequencerEngine::TrackPatch patch;
  patch.index = patchInfoIndex;
  patch.name = juce::String(info.name, (size_t)info.nameLength);
  patch.expansionI = (uint8_t)info.expansionI;
  patch.isRhythm = info.drums;
  sequencerEngine.setTrackPatch(track, patch);
}

juce::String VirtualJVProcessor::getTrackPatchName(int track) const {
  if (track < 0 || track >= kNumPerformanceParts)
    return {};
  return sequencerEngine.getTrackPatch(track).name;
}

// See this method's own declaration comment in PluginProcessor.h. Runs on the message thread,
// so - unlike the audio-thread edge detection that triggered it - it's safe to do everything
// sendPatchToPerformancePart() already does (including a multi-megabyte waverom_exp copy for
// an expansion patch, and the mixed-expansion AlertWindow) and to call setPerformancePartParams()
// for volume/pan.
void VirtualJVProcessor::handleAsyncUpdate() {
  for (int t = 0; t < kNumPerformanceParts; ++t)
    pushSequencerTrackToPerformance(t);
}

void VirtualJVProcessor::pushSequencerTrackToPerformance(int t) {
  const auto patch = sequencerEngine.getTrackPatch(t);
  if (patch.index >= 0)
    sendPatchToPerformancePart(patch.index, t);

  const int channel = sequencerEngine.getTrackChannelOverride(t);
  const int volume = sequencerEngine.getTrackVolume(t);
  const int pan = sequencerEngine.getTrackPan(t);
  if (channel >= 0 || volume >= 0 || pan >= 0) {
    auto &part = performanceParts[t];
    setPerformancePartParams(t, channel >= 0 ? channel : part.midiChannel,
                              volume >= 0 ? volume : part.level,
                              pan >= 0 ? pan : part.pan, part.enabled);
  }
}

// SYNC -> "Send stored settings to patch now" (JivSequencerPanel.cpp's ported UI, see this
// method's own declaration comment in PluginProcessor.h). Same push handleAsyncUpdate() already
// does at the PLAY/REC edge, just on demand and immediately - already on the message thread here
// (a UI click), so no need to defer through triggerAsyncUpdate() first.
void VirtualJVProcessor::resyncProgramChanges() {
  ensurePerformanceMode();
  for (int t = 0; t < kNumPerformanceParts; ++t)
    pushSequencerTrackToPerformance(t);
}

// SYNC -> "Capture patch into song..." - the reverse pull: overwrites every track's stored
// patch/channel/volume/pan with whatever its PerformancePart actually has live right now. A
// Part with nothing assigned (present == false) clears the matching track the same way, so the
// two stay a true mirror of each other rather than a one-directional merge.
void VirtualJVProcessor::captureLivePatchIntoTracks() {
  for (int t = 0; t < kNumPerformanceParts; ++t) {
    auto &part = performanceParts[t];
    if (!part.present) {
      sequencerEngine.clearTrackPatch(t);
      sequencerEngine.setTrackChannelOverride(t, -1);
      sequencerEngine.setTrackVolume(t, -1);
      sequencerEngine.setTrackPan(t, -1);
      continue;
    }

    jivseq::JivSequencerEngine::TrackPatch patch;
    patch.name = juce::String(part.name);
    patch.expansionI = part.expansionI;
    patch.isRhythm = part.isRhythm;
    patch.index = findPatchInfoIndexByIdentity(patch.name, patch.expansionI, patch.isRhythm);
    sequencerEngine.setTrackPatch(t, patch);
    sequencerEngine.setTrackChannelOverride(t, part.midiChannel);
    sequencerEngine.setTrackVolume(t, part.level);
    sequencerEngine.setTrackPan(t, part.pan);
  }
}

VirtualJVProcessor::~VirtualJVProcessor() {
  if (wrapperType == juce::AudioProcessor::wrapperType_Standalone && sequencerEnabled)
    saveSequencerState();

  mcuLock.enter();
  delete mcu;
  delete mcu2;
  mcu2 = nullptr;
  mcuLock.exit();
}

//==============================================================================
const juce::String VirtualJVProcessor::getName() const {
  return JucePlugin_Name;
}

bool VirtualJVProcessor::acceptsMidi() const { return true; }

bool VirtualJVProcessor::producesMidi() const { return false; }

bool VirtualJVProcessor::isMidiEffect() const { return false; }

double VirtualJVProcessor::getTailLengthSeconds() const { return 0.0; }

int VirtualJVProcessor::getNumPrograms() {
  // The CONTIGUOUS ROM range starting at index 0 - what a host iterating 0..N-1 through
  // getProgramName() can actually see. User-saved patches are NOT counted: they live in
  // patchInfos[]'s separate tail at romPatchCapacity+ (see PluginProcessor.h), far beyond
  // this range, so adding them here only produced N empty names at the end of the host's
  // list without ever exposing the user patches themselves.
  return 65                // internal
         + 65              // bank A
         + 65              // bank B
         + totalPatchesExp // expansions
      ;
}

int VirtualJVProcessor::getCurrentProgram() {
  // Only meaningful to a host within 0..getNumPrograms()-1 - a user patch (index >=
  // romPatchCapacity) has no host-visible program number, report 0 like "nothing loaded".
  if (currentPatchIndex >= 0 && currentPatchIndex < getNumPrograms())
    return currentPatchIndex;
  return 0;
}

void VirtualJVProcessor::servicePendingPatch(int numFrames) {
  if (pendingPatchFrames <= 0)
    return;
  pendingPatchFrames -= numFrames;
  if (pendingPatchFrames > 0)
    return;
  pendingPatchFrames = 0;
  memcpy(&mcu->nvram[0x0d70], pendingPatch.data(), 0x16a);
  uint8_t buffer[2] = {0xC0, 0x00};
  mcu->postMidiSC55(buffer, sizeof(buffer));
}

void VirtualJVProcessor::setCurrentProgram(int index) {
  // Not getNumPrograms(): that's a rough count of the CONTIGUOUS ROM range starting at 0
  // (195 + totalPatchesExp), but user patches live in patchInfos[]'s separate reserved tail
  // starting at romPatchCapacity (see PluginProcessor.h) - a much higher, unrelated index that
  // getNumPrograms() was always going to reject. .present is what actually says a slot is
  // populated, regardless of which range it's in (Alan's report, 2026-09-04: clicking a saved
  // User patch didn't reload it - this guard was silently returning before doing anything).
  if (index < 0 || index >= romPatchCapacity + userPatchCapacity || !patchInfos[index].present)
    return;

  if (!loaded)
    return;

  // Picking a Browse patch replaces status.patch: any Part edit session ends (nothing to restore).
  editingPerformancePart = -1;

  // GitHub issue #2: a click on a patch in Browse used to switch Performance mode off. While it is on, the
  // patch is just stored into Patch Temp (slot 0, which no Performance Part uses): no mode byte rewrite, no
  // Program Change (it would reach Part 1 now that Parts receive them) and no engine reset - except when an
  // expansion board has to be swapped in, which resets the engine and needs Common + Parts pushed again.
  const bool keepPerformance = performanceModeEnabled;
  bool engineReset = false;

  mcuLock.enter();

  // Cache the OUTGOING patch's live edits before overwriting the edit buffer, keyed by its own
  // index, so switching back to it later in this session restores those edits instead of
  // silently reloading the pristine ROM/file version (Alan's report, 2026-09-04: clicking
  // another patch and back lost every unsaved change). Skipped on the very first call
  // (currentPatchIndex still -1, nothing loaded yet to cache).
  if (currentPatchIndex >= 0) {
    if (status.isDrums)
      memcpy(editedDrumsCache[currentPatchIndex].data(), status.drums, 0xa7c);
    else
      memcpy(editedPatchCache[currentPatchIndex].data(), status.patch, 0x16a);
  }

  int expansionI = patchInfos[index].expansionI;

  // The extra bounds/nullptr check is for user-saved patches (Alan's request, 2026-09-04):
  // saveCurrentPatchAs() records which expansion was active so the right waverom_exp comes
  // back on reload (see its own comment), but that expansion might not be loaded on whatever
  // machine/ROM-set the patch is later opened with - fall back to leaving waverom_exp alone
  // (silently wrong/no expansion samples for that patch, rather than reading a garbage or
  // out-of-range expansionsDescr[] pointer).
  if (expansionI != 0xff && expansionI >= 0 && expansionI < NUM_EXPS
      && expansionsDescr[expansionI] != nullptr
      && status.currentExpansion != expansionI) {
    status.currentExpansion = expansionI;
    memcpy(mcu->pcm.waverom_exp, expansionsDescr[expansionI], 0x800000);
    // Boot on a ROM-native patch: the real one is applied after the boot (see servicePendingPatch()).
    if (patchInfos[0].present && !patchInfos[0].drums)
      memcpy(&mcu->nvram[0x0d70], (const uint8_t *)patchInfos[0].name, 0x16a);
    mcu->SC55_Reset();
    engineReset = true;
  }
  pendingPatchFrames = 0; // a newer click supersedes any patch still waiting for a boot

  if (patchInfos[index].drums) {
    status.isDrums = true;
    mcu->nvram[0x11] = 0;
    // Pristine copy for revertCurrentPatch() - always straight from the (read-only) ROM/file
    // source, regardless of whether an in-session edit (below) is what actually gets loaded.
    memcpy(originalDrumsSnapshot, (uint8_t *)patchInfos[index].ptr, 0xa7c);
    {
      const auto cached = editedDrumsCache.find(index);
      const uint8_t *source = cached != editedDrumsCache.end() ? cached->second.data() : originalDrumsSnapshot;
      // In Performance mode with Part 8 on the Internal rhythm set, Rhythm Temp belongs to that Part: only the
      // app-side copy is updated here (written back to Rhythm Temp when Performance mode is left).
      const bool protectRhythmTemp = keepPerformance && performanceParts[kNumPerformanceParts - 1].present
                                     && performanceParts[kNumPerformanceParts - 1].bank == 0;
      if (!protectRhythmTemp)
        memcpy(&mcu->nvram[0x67f0], source, 0xa7c);
      memcpy(status.drums, source, 0xa7c);
    }
    if (!keepPerformance)
      mcu->SC55_Reset();
  } else {
    status.isDrums = false;
    memcpy(originalPatchSnapshot, (uint8_t *)patchInfos[index].name, 0x16a);
    const auto cached = editedPatchCache.find(index);
    const uint8_t *source = cached != editedPatchCache.end() ? cached->second.data() : originalPatchSnapshot;
    if (keepPerformance) {
      memcpy(&mcu->nvram[0x0d70], source, 0x16a);
      memcpy(status.patch, &mcu->nvram[0x0d70], 0x16a);
    } else if (mcu->nvram[0x11] != 1) {
      mcu->nvram[0x11] = 1;
      memcpy(&mcu->nvram[0x0d70], source, 0x16a);
      memcpy(status.patch, &mcu->nvram[0x0d70], 0x16a);
      mcu->SC55_Reset();
    } else {
      memcpy(status.patch, source, 0x16a);
      if (engineReset) {
        // The engine is rebooting: apply this patch once the boot is done.
        memcpy(pendingPatch.data(), source, 0x16a);
        pendingPatchFrames = kBootFrames;
      } else {
        memcpy(&mcu->nvram[0x0d70], source, 0x16a);
        uint8_t buffer[2] = {0xC0, 0x00};
        mcu->postMidiSC55(buffer, sizeof(buffer));
      }
    }
  }

  currentPatchIndex = index;

  mcuLock.exit();

  // Leaving Performance mode through a patch click is no longer possible (see keepPerformance above); the
  // app-level flag and the firmware stay in step. Only an expansion swap (engine reset) needs the
  // Performance state sent again. setPerformanceModeEnabled() takes mcuLock itself, hence after exit().
  if (keepPerformance && engineReset)
    setPerformanceModeEnabled(true);

  if (auto editor = safeActiveEditor())
  {
      auto e = dynamic_cast<VirtualJVEditor*>(editor);

      e->showToneOrRhythmEditTabs(status.isDrums);
      e->updateEditTabs();
  }
}

// Discards every edit made since the current patch was loaded, restoring the snapshot
// setCurrentProgram() took right after copying it in from its (read-only) ROM source - Alan's
// request. Mirrors setCurrentProgram()'s own "same mode, reload the buffer" path (Program
// Change for tone, a full reset for drums) rather than doing a full SC55_Reset() in both cases,
// so reverting a tone patch doesn't have the audible glitch a full engine reset causes.
void VirtualJVProcessor::revertCurrentPatch() {
  if (!loaded)
    return;

  mcuLock.enter();

  if (status.isDrums) {
    memcpy(&mcu->nvram[0x67f0], originalDrumsSnapshot, 0xa7c);
    memcpy(status.drums, originalDrumsSnapshot, 0xa7c);
    mcu->SC55_Reset();
  } else {
    memcpy(&mcu->nvram[0x0d70], originalPatchSnapshot, 0x16a);
    memcpy(status.patch, originalPatchSnapshot, 0x16a);
    uint8_t buffer[2] = {0xC0, 0x00};
    mcu->postMidiSC55(buffer, sizeof(buffer));
  }

  mcuLock.exit();

  if (auto editor = safeActiveEditor())
  {
      if (auto e = dynamic_cast<VirtualJVEditor*>(editor))
        e->updateEditTabs();
  }
}

bool VirtualJVProcessor::isPatchModified(int index) const {
  if (index < 0 || index >= romPatchCapacity + userPatchCapacity || !patchInfos[index].present)
    return false;

  const PatchInfo &info = patchInfos[index];

  if (index == currentPatchIndex) {
    return info.drums ? memcmp(status.drums, originalDrumsSnapshot, 0xa7c) != 0
                       : memcmp(status.patch, originalPatchSnapshot, 0x16a) != 0;
  }

  if (info.drums) {
    const auto it = editedDrumsCache.find(index);
    return it != editedDrumsCache.end() && memcmp(it->second.data(), info.ptr, 0xa7c) != 0;
  }
  const auto it = editedPatchCache.find(index);
  return it != editedPatchCache.end() && memcmp(it->second.data(), info.name, 0x16a) != 0;
}

juce::File VirtualJVProcessor::userPatchesDir() {
  return appDataBaseDir()
      .getChildFile("UserPatches");
}

VirtualJVProcessor::SysexImportReport VirtualJVProcessor::importSysexFile(
    const juce::File &file, const std::function<bool(int, int, const juce::String &)> &progress,
    const std::function<void(int)> &waitMs) {
  SysexImportReport report;
  if (!loaded) {
    report.error = "The ROMs are not loaded";
    return report;
  }

  juce::String readError;
  const auto messages = sysexio::readMessages(file, readError);
  if (readError.isNotEmpty()) {
    report.error = readError;
    return report;
  }
  const auto patches = sysexio::extractPatches(messages, report.notes);
  if (patches.empty()) {
    report.error = "No JV-880 patch data found in " + file.getFileName();
    return report;
  }

  // Each patch goes through Internal Patch Memory, where the firmware converts the SysEx layout (34 + 4 x 116
  // bytes) into its own 362-byte record. The last Internal slot is the scratch area; its content is put back
  // afterwards. (Patch Temp itself, 00 08 20, is not where the firmware keeps the record.)
  constexpr int kScratchSlot = 63;
  const size_t scratchOffset = 0x0d70 + (size_t)kScratchSlot * 0x16a;
  uint8_t original[0x16a];
  mcuLock.enter();
  memcpy(original, &mcu->nvram[scratchOffset], sizeof(original));
  mcuLock.exit();

  auto dir = userPatchesDir();
  dir.createDirectory();

  const int count = (int)patches.size();
  for (int i = 0; i < count; ++i) {
    const auto &patch = patches[(size_t)i];
    if (progress && !progress(i, count, patch.name)) {
      report.notes.add("Cancelled");
      break;
    }

    mcuLock.enter();
    for (const auto &m : sysexio::internalSlotMessages(patch, kScratchSlot))
      mcu->postMidiSC55(m.data(), (int)m.size());
    mcuLock.exit();

    // Wait for the firmware's UART to take everything (31250 baud pace), then a little for it to act on it.
    for (int waited = 0; waited < 6000 && mcu->uart_read_ptr != mcu->uart_write_ptr; waited += 10)
      waitMs(10);
    waitMs(250);

    uint8_t record[0x16a];
    mcuLock.enter();
    memcpy(record, &mcu->nvram[scratchOffset], sizeof(record));
    mcuLock.exit();

    // The record starts with the 12-character name: if it is the one that was sent, the firmware took the patch.
    if (memcmp(record, patch.blocks[0].data(), 12) != 0) {
      ++report.failed;
      report.notes.add("\"" + patch.name + "\" was not accepted by the firmware (is the audio engine running?)");
      continue;
    }

    juce::String safe = juce::File::createLegalFileName(patch.name);
    if (safe.isEmpty())
      safe = "Imported Patch";
    auto target = dir.getChildFile(safe + ".jvp");
    for (int n = 2; target.exists(); ++n)
      target = dir.getChildFile(safe + " " + juce::String(n) + ".jvp");

    juce::MemoryBlock block;
    const uint8_t header[2] = {0, 0xff}; // tone patch, no expansion board
    block.append(header, 2);
    block.append(record, sizeof(record));
    if (target.replaceWithData(block.getData(), block.getSize())) {
      ++report.imported;
      report.names.add(patch.name);
    } else {
      ++report.failed;
      report.notes.add("Could not write " + target.getFileName());
    }
  }

  mcuLock.enter();
  memcpy(&mcu->nvram[scratchOffset], original, sizeof(original));
  mcuLock.exit();

  refreshUserPatches();
  return report;
}

bool VirtualJVProcessor::saveCurrentPatchAs(const juce::File &file) {
  if (!loaded)
    return false;

  // 1-byte type marker (0 = tone patch, 1 = rhythm set), 1-byte expansion index (0xff = none/
  // internal-only - Alan's question, 2026-09-04: a patch using an expansion board's waveforms
  // needs that same board's waverom_exp loaded again on reload, or its wave numbers resolve
  // against the wrong (or no) samples - see setCurrentProgram()'s own comment on the matching
  // read-back), then the raw bytes.
  juce::MemoryBlock block;
  uint8_t marker = status.isDrums ? 1 : 0;
  block.append(&marker, 1);
  uint8_t expansionByte = (uint8_t)juce::jlimit(0, 0xff, status.currentExpansion);
  block.append(&expansionByte, 1);

  mcuLock.enter();
  if (status.isDrums) {
    block.append(status.drums, sizeof(status.drums));
  } else {
    uint8_t buf[sizeof(status.patch)];
    memcpy(buf, status.patch, sizeof(buf));
    // Embed the chosen name into the patch's own name field (first 12 bytes - Patch::name in
    // dataStructures.h), so it reads back and displays exactly like a factory patch does.
    // Rhythm sets have no equivalent embedded field (see the Rhythm struct's own comment),
    // hence the file's own name being the only source of truth for those.
    char nameBuf[12] = {0};
    auto nameUtf8 = file.getFileNameWithoutExtension().toRawUTF8();
    memcpy(nameBuf, nameUtf8, std::min(sizeof(nameBuf), strlen(nameUtf8)));
    memcpy(buf, nameBuf, sizeof(nameBuf));
    block.append(buf, sizeof(buf));
  }
  mcuLock.exit();

  file.getParentDirectory().createDirectory();
  if (!file.replaceWithData(block.getData(), block.getSize()))
    return false;

  refreshUserPatches();
  return true;
}

// (Re)scans userPatchesDir() and rebuilds the reserved "User" tail of patchInfos[]/
// patchInfoPerGroup[userGroupIndex] from what's there - called once at startup and again after
// every saveCurrentPatchAs(), so a freshly-saved patch shows up in the browser immediately.
void VirtualJVProcessor::refreshUserPatches() {
  constexpr size_t kPatchBytes = 0x16a;
  constexpr size_t kRhythmBytes = 0xa7c;

  userToneBuffers.clear();
  userDrumBuffers.clear();
  userPatchNames.clear();
  numUserPatches = 0;

  auto &group = patchInfoPerGroup[userGroupIndex];
  group.clear();

  auto dir = userPatchesDir();
  dir.createDirectory();

  auto files = dir.findChildFiles(juce::File::findFiles, false, "*.jvp");
  files.sort();

  for (auto &file : files) {
    if (numUserPatches >= userPatchCapacity)
      break; // the reserved tail of patchInfos[] is a fixed size

    juce::MemoryBlock block;
    if (!file.loadFileAsData(block) || block.getSize() < 2)
      continue;

    const auto *bytes = static_cast<const uint8_t *>(block.getData());
    const bool isDrums = bytes[0] != 0;
    const int expansionByte = bytes[1];
    const size_t expected = isDrums ? kRhythmBytes : kPatchBytes;
    if (block.getSize() != 2 + expected)
      continue; // not a file this build wrote (wrong size) - skip rather than risk misreading it

    PatchInfo &info = patchInfos[romPatchCapacity + numUserPatches];
    // Which expansion's waverom_exp to reload alongside this patch (Alan's question,
    // 2026-09-04) - 0xff means none/internal-only. Bounds-checked here too:
    // setCurrentProgram()'s own check additionally requires that expansion's ROM to actually
    // be loaded on THIS run, which may not hold (the patch could have been saved on a machine
    // with a different ROM set).
    info.expansionI = (expansionByte >= 0 && expansionByte < NUM_EXPS) ? expansionByte : 0xff;
    info.patchI = 0;
    info.present = true;
    info.drums = isDrums;
    info.iInList = romPatchCapacity + numUserPatches;

    if (isDrums) {
      auto &buf = userDrumBuffers.emplace_back();
      memcpy(buf.data(), bytes + 2, kRhythmBytes);
      info.ptr = (const char *)buf.data();
      userPatchNames.push_back(file.getFileNameWithoutExtension().toStdString());
      info.name = userPatchNames.back().c_str();
      info.nameLength = (int)userPatchNames.back().size();
    } else {
      auto &buf = userToneBuffers.emplace_back();
      memcpy(buf.data(), bytes + 2, kPatchBytes);
      info.name = (const char *)buf.data(); // first 12 bytes are the embedded patch name
      info.nameLength = 12;
    }

    group.push_back(&info);
    numUserPatches++;
  }
}

namespace {
// See PerformancePart's own comment (PluginProcessor.h) for what this covers and why: only the
// 195 ROM-native factory tones/rhythm-sets (patchInfos[] index 0..194) map onto a real Patch
// Memory bank/number the firmware can actually reference from a Performance Part. Confirmed
// empirically (LCD screenshot + RAM search naming the right factory patch - see CLAUDE.md):
// patchInfos[] 0..63 ("Internal A" in this project's own Browse-tab naming) IS the firmware's
// real read-only Preset A bank; 64..127 ("Internal B") is Preset B; 128..191 ("Internal User",
// despite the name - it's ROM-sourced like the other two) is the real writable Internal bank's
// factory-default content; 192/193/194 are the matching Rhythm Set for each of those 3 banks.
bool performancePatchMapping(int patchInfoIndex, uint8_t &bank, uint8_t &number, bool &isRhythm) {
  if (patchInfoIndex >= 0 && patchInfoIndex < 192) {
    const int group = patchInfoIndex / 64; // 0 = Internal A/Preset A, 1 = Internal B/Preset B, 2 = Internal User/real Internal
    bank = group == 0 ? 2 : group == 1 ? 3 : 0;
    number = (uint8_t)(patchInfoIndex % 64);
    isRhythm = false;
    return true;
  }
  if (patchInfoIndex == 192) { bank = 2; number = 0; isRhythm = true; return true; }
  if (patchInfoIndex == 193) { bank = 3; number = 0; isRhythm = true; return true; }
  if (patchInfoIndex == 194) { bank = 0; number = 0; isRhythm = true; return true; }
  return false;
}

constexpr size_t kPerfPartNameBytes = sizeof(VirtualJVProcessor::PerformancePart{}.name);
// present(1) + isRhythm(1) + bank(1) + number(1) + name + midiChannel(1) + level(1) + pan(1) +
// enabled(1) + expansionI(1)
constexpr size_t kPerfPartRecordBytes = 4 + kPerfPartNameBytes + 5;
constexpr size_t kPerfNameBytes = 12; // matches the firmware's own Common name field width
constexpr size_t kPerfBaseBytes = kPerfNameBytes + VirtualJVProcessor::kNumPerformanceParts * kPerfPartRecordBytes;
// Optional trailer after the base record: "PEX1" + 19 Common bytes + 8 bytes per Part. Files
// written before it existed (base size only) still load, with the default Common/extra fields.
constexpr size_t kPerfCommonBytes = 11 + 8;
constexpr size_t kPerfPartExtraBytes = 8;
constexpr size_t kPerfExtrasBytes = 4 + kPerfCommonBytes + VirtualJVProcessor::kNumPerformanceParts * kPerfPartExtraBytes;

// Shared by the "Save As..." bank format and the session-restore file - both serialize the same
// performance name + 8 PerformanceParts, just under different filenames/directories (see
// performancesDir() vs performanceSessionFile()).
void appendPerformanceParts(
    juce::MemoryBlock &block, const char (&name)[13],
    const VirtualJVProcessor::PerformancePart (&parts)[VirtualJVProcessor::kNumPerformanceParts],
    const VirtualJVProcessor::PerformanceCommon &common) {
  block.append(name, kPerfNameBytes);

  for (auto &part : parts) {
    uint8_t present = part.present ? 1 : 0;
    block.append(&present, 1);
    uint8_t isRhythm = part.isRhythm ? 1 : 0;
    block.append(&isRhythm, 1);
    block.append(&part.bank, 1);
    block.append(&part.number, 1);

    char nameBuf[kPerfPartNameBytes] = {0};
    memcpy(nameBuf, part.name, sizeof(nameBuf));
    block.append(nameBuf, sizeof(nameBuf));

    uint8_t ch = (uint8_t)part.midiChannel;
    block.append(&ch, 1);
    uint8_t lvl = (uint8_t)part.level;
    block.append(&lvl, 1);
    uint8_t pan = (uint8_t)part.pan;
    block.append(&pan, 1);
    uint8_t en = part.enabled ? 1 : 0;
    block.append(&en, 1);
    block.append(&part.expansionI, 1);
  }

  block.append("PEX1", 4);
  const uint8_t commonBytes[kPerfCommonBytes] = {
      (uint8_t)common.keyMode,        (uint8_t)common.reverbType,     (uint8_t)common.reverbLevel,
      (uint8_t)common.reverbTime,     (uint8_t)common.reverbFeedback, (uint8_t)common.chorusType,
      (uint8_t)common.chorusLevel,    (uint8_t)common.chorusDepth,    (uint8_t)common.chorusRate,
      (uint8_t)common.chorusFeedback, (uint8_t)common.chorusOutput,
      (uint8_t)common.voiceReserve[0], (uint8_t)common.voiceReserve[1], (uint8_t)common.voiceReserve[2],
      (uint8_t)common.voiceReserve[3], (uint8_t)common.voiceReserve[4], (uint8_t)common.voiceReserve[5],
      (uint8_t)common.voiceReserve[6], (uint8_t)common.voiceReserve[7]};
  block.append(commonBytes, sizeof(commonBytes));
  for (auto &part : parts) {
    const uint8_t extra[kPerfPartExtraBytes] = {
        (uint8_t)part.coarseTune,   (uint8_t)part.fineTune,        (uint8_t)(part.reverbSwitch ? 1 : 0),
        (uint8_t)(part.chorusSwitch ? 1 : 0), (uint8_t)part.outputSelect,
        (uint8_t)(part.rxProgramChange ? 1 : 0), (uint8_t)(part.rxVolume ? 1 : 0),
        (uint8_t)(part.rxHold1 ? 1 : 0)};
    block.append(extra, sizeof(extra));
  }
}

// Returns false (leaving `name`/`parts` untouched) if `size` doesn't match exactly - a file this
// build didn't write, or written by a since-changed format.
bool readPerformanceParts(
    const uint8_t *bytes, size_t size, char (&name)[13],
    VirtualJVProcessor::PerformancePart (&parts)[VirtualJVProcessor::kNumPerformanceParts],
    VirtualJVProcessor::PerformanceCommon &common) {
  // Anything after the PEX1 trailer is the "PCU1" custom-Part data, read by restoreCustomPartData().
  const bool hasExtras = size >= kPerfBaseBytes + kPerfExtrasBytes;
  if (size != kPerfBaseBytes && !hasExtras)
    return false;

  memcpy(name, bytes, kPerfNameBytes);
  name[kPerfNameBytes] = '\0';
  size_t offset = kPerfNameBytes;

  for (int i = 0; i < VirtualJVProcessor::kNumPerformanceParts; i++) {
    auto &part = parts[i];

    uint8_t present = bytes[offset]; offset += 1;
    uint8_t isRhythm = bytes[offset]; offset += 1;
    uint8_t bank = bytes[offset]; offset += 1;
    uint8_t number = bytes[offset]; offset += 1;
    const char *nameBytes = (const char *)(bytes + offset); offset += kPerfPartNameBytes;
    uint8_t ch = bytes[offset]; offset += 1;
    uint8_t lvl = bytes[offset]; offset += 1;
    uint8_t pan = bytes[offset]; offset += 1;
    uint8_t en = bytes[offset]; offset += 1;
    uint8_t expansionI = bytes[offset]; offset += 1;

    part.present = present != 0;
    part.isRhythm = isRhythm != 0;
    part.bank = bank;
    part.number = number;
    memcpy(part.name, nameBytes, sizeof(part.name));
    part.name[sizeof(part.name) - 1] = '\0'; // defensive against a corrupt/foreign file
    part.midiChannel = ch;
    part.level = lvl;
    part.pan = pan;
    part.enabled = en != 0;
    part.expansionI = expansionI;
  }

  common = VirtualJVProcessor::PerformanceCommon();
  for (auto &part : parts) { // defaults for files without the trailer
    part.coarseTune = 64; part.fineTune = 64; part.reverbSwitch = part.chorusSwitch = true;
    part.outputSelect = 0; part.rxProgramChange = part.rxVolume = part.rxHold1 = true;
  }
  if (hasExtras && memcmp(bytes + offset, "PEX1", 4) == 0) {
    const uint8_t *c = bytes + offset + 4;
    common.keyMode = juce::jlimit(0, 2, (int)c[0]);
    common.reverbType = juce::jlimit(0, 7, (int)c[1]);
    common.reverbLevel = juce::jlimit(0, 127, (int)c[2]);
    common.reverbTime = juce::jlimit(0, 127, (int)c[3]);
    common.reverbFeedback = juce::jlimit(0, 127, (int)c[4]);
    common.chorusType = juce::jlimit(0, 2, (int)c[5]);
    common.chorusLevel = juce::jlimit(0, 127, (int)c[6]);
    common.chorusDepth = juce::jlimit(0, 127, (int)c[7]);
    common.chorusRate = juce::jlimit(0, 127, (int)c[8]);
    common.chorusFeedback = juce::jlimit(0, 127, (int)c[9]);
    common.chorusOutput = juce::jlimit(0, 1, (int)c[10]);
    for (int i = 0; i < 8; ++i)
      common.voiceReserve[i] = juce::jlimit(0, 28, (int)c[11 + i]);
    const uint8_t *e = c + kPerfCommonBytes;
    for (auto &part : parts) {
      part.coarseTune = juce::jlimit(16, 112, (int)e[0]);
      part.fineTune = juce::jlimit(14, 114, (int)e[1]);
      part.reverbSwitch = e[2] != 0;
      part.chorusSwitch = e[3] != 0;
      part.outputSelect = juce::jlimit(0, 2, (int)e[4]);
      part.rxProgramChange = e[5] != 0;
      part.rxVolume = e[6] != 0;
      part.rxHold1 = e[7] != 0;
      e += kPerfPartExtraBytes;
    }
  }
  return true;
}
} // namespace

bool VirtualJVProcessor::isEligibleForPerformancePart(int patchInfoIndex) const {
  if (patchInfoIndex < 0 || patchInfoIndex >= romPatchCapacity + userPatchCapacity
      || !patchInfos[patchInfoIndex].present)
    return false;

  uint8_t bank, number;
  bool isRhythm;
  if (performancePatchMapping(patchInfoIndex, bank, number, isRhythm))
    return true; // ROM-native - already lives in real Patch Memory verbatim

  // Expansion-ROM/User: tone patches go into an Internal Patch Memory slot, rhythm sets into Rhythm Temp
  // (the Part 8 "Internal" set, found by JV880_SELFTEST_RHYMEM) - see injectCustomPatchIntoInternalMemory().
  return true;
}

// See this function's own comment in PluginProcessor.h. Caller must already hold mcuLock.
void VirtualJVProcessor::injectCustomPatchIntoInternalMemory(int patchInfoIndex, int internalSlot) {
  auto &info = patchInfos[patchInfoIndex];
  if (info.drums) {
    // A rhythm set: the Performance Part 8 "Internal" Rhythm Set (bank 0) IS Rhythm Temp, nvram[0x67f0]
    // (measured: Part 8 on Preset B and Part 8 on Internal after copying Preset B's bytes here render the
    // same drums). Part 8 is then pointed at bank 0 / number 0 by the caller.
    memcpy(&mcu->nvram[0x67f0], info.ptr, 0xa7c);
    if (mcu2 != nullptr)
      memcpy(&mcu2->nvram[0x67f0], info.ptr, 0xa7c);
  } else {
    memcpy(&mcu->nvram[0x0d70 + internalSlot * 0x16a], info.name, 0x16a);
    if (mcu2 != nullptr)
      memcpy(&mcu2->nvram[0x0d70 + internalSlot * 0x16a], info.name, 0x16a);
  }

  // Only one waverom_exp can be loaded into this single engine at a time - matches setCurrent
  // Program()'s own handling for Patch mode, and the real hardware's own single wave-expansion-
  // slot limit. A Performance mixing Parts from two *different* expansion boards will have
  // whichever was injected last sound right and the other(s) not - same real-hardware
  // constraint, not something this project's single-engine design adds on top.
  if (info.expansionI != 0xff && info.expansionI < NUM_EXPS
      && expansionsDescr[info.expansionI] != nullptr) {
    memcpy(mcu->pcm.waverom_exp, expansionsDescr[info.expansionI], 0x800000);
    if (mcu2 != nullptr)
      memcpy(mcu2->pcm.waverom_exp, expansionsDescr[info.expansionI], 0x800000);
  }
}

// Pushes the Performance Common record (name + effects + voice reserve) to the single `mcu`
// engine over SysEx DT1, address 00 00 10 00 ("Temporary Performance", common). Caller must
// already hold mcuLock. Effects/voice-reserve fields are left at 0 for now (confirmed by an
// audio render test, see CLAUDE.md, that an unreserved Part still plays - voice reserve only
// affects priority under voice contention); they now come from performanceCommon (Performance tab).
void VirtualJVProcessor::pushPerformanceCommonToEngine() {
  std::vector<uint8_t> common(31, 0);
  memcpy(common.data(), performanceName, kPerfNameBytes);
  const auto &pc = performanceCommon;
  auto put = [&](int index, int value, int hi) { common[(size_t)index] = (uint8_t)juce::jlimit(0, hi, value); };
  put(12, pc.keyMode, 2);
  put(13, pc.reverbType, 7);
  put(14, pc.reverbLevel, 127);
  put(15, pc.reverbTime, 127);
  put(16, pc.reverbFeedback, 127);
  put(17, pc.chorusType, 2);
  put(18, pc.chorusLevel, 127);
  put(19, pc.chorusDepth, 127);
  put(20, pc.chorusRate, 127);
  put(21, pc.chorusFeedback, 127);
  put(22, pc.chorusOutput, 1);
  for (int i = 0; i < 8; ++i)
    put(23 + i, pc.voiceReserve[i], 28);
  sendSysexBlock(0x10u << 7, common.data(), common.size());
}

// Pushes one Part's 35-byte record to the single `mcu` engine over SysEx DT1, address
// 00 00 1n 00 where n = 8+partIndex (see the partOffsets table, matching Edisyn's own
// RolandJV880MultiRec.partOffsets). Caller must already hold mcuLock.
void VirtualJVProcessor::pushPerformancePartToEngine(int partIndex) {
  if (partIndex < 0 || partIndex >= kNumPerformanceParts)
    return;

  auto &part = performanceParts[partIndex];

  std::vector<uint8_t> data(35, 0);
  // Indices below match RolandJV880Multi.java's own allToneParameters ordering exactly - see
  // PerformancePart's comment in PluginProcessor.h for the full field list.
  data[21] = (part.present && part.enabled) ? 1 : 0; // receiveswitch
  data[22] = (uint8_t)juce::jlimit(0, 15, part.midiChannel - 1); // receivechannel, 0-indexed
  const uint8_t patchValue = part.present ? (uint8_t)(part.bank * 64 + part.number) : 0;
  data[23] = (patchValue >> 4) & 0x7F; // patchnumber MSB nibble
  data[24] = patchValue & 0x0F;        // patchnumber LSB nibble
  data[25] = (uint8_t)juce::jlimit(0, 127, part.level); // partlevel
  data[26] = (uint8_t)juce::jlimit(0, 127, part.pan);   // partpan
  data[27] = (uint8_t)juce::jlimit(16, 112, part.coarseTune); // partcoarsetune
  data[28] = (uint8_t)juce::jlimit(14, 114, part.fineTune);   // partfinetune
  data[29] = part.reverbSwitch ? 1 : 0; // reverbswitch - onto the Performance's shared reverb bus
  data[30] = part.chorusSwitch ? 1 : 0; // chorusswitch - ditto
  // Receive switches: all zero meant the Part ignored Channel Volume (CC7), Hold-1 and Program Change
  // (GitHub issue #2: "Channel Volume CC messages stop working in Performance mode"). On: as on a
  // real JV-880's Performance Part.
  data[31] = part.rxProgramChange ? 1 : 0; // receiveprogramchange
  data[32] = part.rxVolume ? 1 : 0;        // receivevolume
  data[33] = part.rxHold1 ? 1 : 0;         // receivehold1
  data[34] = (uint8_t)juce::jlimit(0, 2, part.outputSelect); // outputselect

  static constexpr uint8_t partOffsets[VirtualJVProcessor::kNumPerformanceParts] = {
      0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
  const uint32_t address = (uint32_t)(0x10 + partOffsets[partIndex]) << 7;
  sendSysexBlock(address, data.data(), data.size());
}

void VirtualJVProcessor::sendPatchToPerformancePart(int patchInfoIndex, int partIndex) {
  if (!loaded || partIndex < 0 || partIndex >= kNumPerformanceParts)
    return;
  if (patchInfoIndex < 0 || patchInfoIndex >= romPatchCapacity + userPatchCapacity
      || !patchInfos[patchInfoIndex].present)
    return;

  auto &info = patchInfos[patchInfoIndex];

  // Part 8 is fixed to Rhythm on real hardware, Parts 1-7 fixed to tone - reject a mismatch
  // rather than silently mis-configuring the Part.
  const bool wantsRhythmPart = (partIndex == kNumPerformanceParts - 1);
  if (info.drums != wantsRhythmPart)
    return;

  uint8_t bank, number;
  bool isRhythm;
  const bool isRomNative = performancePatchMapping(patchInfoIndex, bank, number, isRhythm);

  mcuLock.enter();

  if (isRomNative && info.drums && bank == 0) {
    // The factory Internal rhythm set: Rhythm Temp may hold an expansion/User set injected earlier.
    injectCustomPatchIntoInternalMemory(patchInfoIndex, 0);
  }
  if (!isRomNative && info.drums) {
    // Rhythm set from an expansion/User bank: injected into Rhythm Temp, which Part 8 reads as its Internal set.
    bank = 0;
    number = 0;
    injectCustomPatchIntoInternalMemory(patchInfoIndex, 0);
  } else if (!isRomNative) {
    // Expansion-ROM/User tone patch: inject it into this Part's own reserved Internal Patch
    // Memory slot (1..7 - slot 0 is Patch mode's own live buffer, never touched here).
    bank = 0;
    number = (uint8_t)(partIndex + 1);
    injectCustomPatchIntoInternalMemory(patchInfoIndex, partIndex + 1);
  }

  auto &part = performanceParts[partIndex];

  part.present = true;
  part.isRhythm = info.drums;
  part.bank = bank;
  part.number = number;
  part.expansionI = (uint8_t)info.expansionI;

  const int n = std::min(info.nameLength, (int)sizeof(part.name) - 1);
  memcpy(part.name, info.name, (size_t)n);
  part.name[n] = '\0';

  if (performanceModeEnabled)
    pushPerformancePartToEngine(partIndex);
  mcuLock.exit();

  // Non-blocking heads-up (Alan's request, 2026-09-08), not a rejection - see PerformancePart::
  // expansionI's own comment in PluginProcessor.h for why mixing boards is a real single-engine/
  // real-hardware limitation rather than something worth preventing outright.
  bool mixedExpansionWarning = false;
  if (part.expansionI != 0xff) {
    for (int i = 0; i < kNumPerformanceParts; ++i) {
      if (i == partIndex)
        continue;
      auto &other = performanceParts[i];
      if (other.present && other.expansionI != 0xff && other.expansionI != part.expansionI) {
        mixedExpansionWarning = true;
        break;
      }
    }
  }

  savePerformanceSessionState();

  if (auto editor = safeActiveEditor())
    if (auto e = dynamic_cast<VirtualJVEditor *>(editor))
    {
      e->updatePerformanceTab();
      if (mixedExpansionWarning)
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Mixed expansion boards",
            "This Performance now combines Parts from more than one expansion ROM. Only one "
            "expansion board can be loaded into the engine at a time, so only the most recently "
            "assigned board's Part(s) will sound correct - the others will play whatever "
            "expansion is currently loaded instead.",
            "OK", e);
    }
}

void VirtualJVProcessor::clearPerformancePart(int partIndex) {
  if (partIndex < 0 || partIndex >= kNumPerformanceParts)
    return;

  performanceParts[partIndex] = PerformancePart();
  performanceParts[partIndex].isRhythm = (partIndex == kNumPerformanceParts - 1);

  mcuLock.enter();
  if (performanceModeEnabled)
    pushPerformancePartToEngine(partIndex);
  mcuLock.exit();

  savePerformanceSessionState();

  if (auto editor = safeActiveEditor())
    if (auto e = dynamic_cast<VirtualJVEditor *>(editor))
      e->updatePerformanceTab();
}

void VirtualJVProcessor::setPerformancePartParams(int partIndex, int midiChannel, int level,
                                                   int pan, bool enabled) {
  if (partIndex < 0 || partIndex >= kNumPerformanceParts)
    return;

  auto &part = performanceParts[partIndex];
  part.midiChannel = juce::jlimit(1, 16, midiChannel);
  part.level = juce::jlimit(0, 127, level);
  part.pan = juce::jlimit(0, 127, pan);
  part.enabled = enabled;

  mcuLock.enter();
  if (performanceModeEnabled)
    pushPerformancePartToEngine(partIndex);
  mcuLock.exit();

  savePerformanceSessionState();
}

void VirtualJVProcessor::applyPerformancePart(int partIndex) {
  if (partIndex < 0 || partIndex >= kNumPerformanceParts)
    return;
  mcuLock.enter();
  if (performanceModeEnabled)
    pushPerformancePartToEngine(partIndex);
  mcuLock.exit();
  savePerformanceSessionState();
}

void VirtualJVProcessor::applyPerformanceCommon() {
  mcuLock.enter();
  if (performanceModeEnabled)
    pushPerformanceCommonToEngine();
  mcuLock.exit();
  savePerformanceSessionState();
}

void VirtualJVProcessor::setPerformanceName(const juce::String &name) {
  auto padded = name + "            ";
  for (int i = 0; i < 12; i++) {
    auto c = padded[i];
    performanceName[i] = (char)((c < 32 || c > 127) ? ' ' : (char)c);
  }
  performanceName[12] = '\0';

  mcuLock.enter();
  if (performanceModeEnabled)
    pushPerformanceCommonToEngine();
  mcuLock.exit();

  savePerformanceSessionState();
}

void VirtualJVProcessor::dropExtraEngine() {
  mcuLock.enter();
  MCU *old = mcu2;
  mcu2 = nullptr;
  mcuLock.exit();
  delete old;
}

// Builds the second engine from the first one's current NVRAM and expansion ROM, boots it
// (outside mcuLock: about 2.5 s of emulated time), then swaps it in and sends it the Performance
// state. Runs on the calling (message) thread, so the UI pauses for the boot.
void VirtualJVProcessor::rebuildExtraEngine() {
  if (!loaded || !extraPolyphony || !performanceModeEnabled)
    return;

  std::vector<uint8_t> nvramCopy(NVRAM_SIZE);
  mcuLock.enter();
  memcpy(nvramCopy.data(), mcu->nvram, NVRAM_SIZE);
  mcuLock.exit();

  auto engine = std::make_unique<MCU>();
  engine->startSC55(loadedRoms[getRomIndex("jv880_rom1.bin")], loadedRoms[getRomIndex("jv880_rom2.bin")],
                    loadedRoms[getRomIndex("jv880_waverom1.bin")], loadedRoms[getRomIndex("jv880_waverom2.bin")],
                    nvramCopy.data());
  mcuLock.enter();
  memcpy(engine->pcm.waverom_exp, mcu->pcm.waverom_exp, 0x800000);
  mcuLock.exit();
  engine->SC55_Reset();

  constexpr int kBlock = 512, kRate = 44100;
  std::vector<float> l(kBlock), r(kBlock);
  for (int done = 0; done < kRate * 5 / 2; done += kBlock)
    engine->updateSC55WithSampleRate(l.data(), r.data(), kBlock, kRate);

  mcuLock.enter();
  MCU *old = mcu2;
  mcu2 = engine.release();
  // Same sequence as setPerformanceModeEnabled(true); the duplicate for `mcu` is harmless.
  const uint8_t modeByte = 0;
  sendSysexBlock(0, &modeByte, 1);
  pushPerformanceCommonToEngine();
  for (int i = 0; i < kNumPerformanceParts; ++i)
    pushPerformancePartToEngine(i);
  mcuLock.exit();
  delete old;
}

void VirtualJVProcessor::setOutputMode(int mode) {
  outputMode = mode != 0 ? 1 : 0;
  savePersistedOutputMode(outputMode);
}

void VirtualJVProcessor::setMainFullMix(bool enabled) {
  mainFullMix = enabled;
  savePersistedMainFullMix(enabled);
}

juce::String VirtualJVProcessor::getPreferredAudioDeviceType() const { return loadPersistedAudioDeviceType(); }

void VirtualJVProcessor::setPreferredAudioDeviceType(const juce::String &type) {
  if (type.isNotEmpty() && type != loadPersistedAudioDeviceType())
    savePersistedAudioDeviceType(type);
}

bool VirtualJVProcessor::getMultiOutBuses() const { return loadPersistedMultiOut(); }

void VirtualJVProcessor::setMultiOutBuses(bool enabled) { savePersistedMultiOut(enabled); }

void VirtualJVProcessor::setExtraPolyphony(bool enabled) {
  if (enabled == extraPolyphony)
    return;
  extraPolyphony = enabled;
  savePersistedExtraPolyphony(enabled);
  if (enabled)
    rebuildExtraEngine();
  else
    dropExtraEngine();
}

bool VirtualJVProcessor::beginEditPerformancePart(int partIndex) {
  if (!loaded || !performanceModeEnabled || partIndex < 0 || partIndex >= kNumPerformanceParts - 1)
    return false;
  auto &part = performanceParts[partIndex];
  if (!part.present)
    return false;

  const int slot = partIndex + 1;
  mcuLock.enter();
  const uint8_t *src = nullptr;
  if (part.bank == 2 || part.bank == 3)
    src = reinterpret_cast<const uint8_t *>(patchInfos[(part.bank - 2) * 64 + part.number].name);
  else if (part.bank == 0 && part.number < 64)
    src = &mcu->nvram[0x0d70 + (size_t)part.number * 0x16a];
  if (src == nullptr) {
    mcuLock.exit();
    return false;
  }
  uint8_t copy[0x16a];
  memcpy(copy, src, sizeof(copy));
  memcpy(&mcu->nvram[0x0d70 + (size_t)slot * 0x16a], copy, sizeof(copy));
  if (mcu2 != nullptr)
    memcpy(&mcu2->nvram[0x0d70 + (size_t)slot * 0x16a], copy, sizeof(copy));
  part.bank = 0;
  part.number = (uint8_t)slot;
  pushPerformancePartToEngine(partIndex);
  mcuLock.exit();

  if (editingPerformancePart < 0)
    memcpy(editPatchBackup, status.patch, sizeof(editPatchBackup));
  memcpy(status.patch, copy, sizeof(copy));
  editingPerformancePart = partIndex;
  savePerformanceSessionState();

  if (auto *e = dynamic_cast<VirtualJVEditor *>(safeActiveEditor()))
    e->updateEditTabs();
  return true;
}

void VirtualJVProcessor::endEditPerformancePart() {
  if (editingPerformancePart < 0)
    return;
  memcpy(status.patch, editPatchBackup, sizeof(editPatchBackup));
  editingPerformancePart = -1;
  if (auto *e = dynamic_cast<VirtualJVEditor *>(safeActiveEditor()))
    e->updateEditTabs();
}

void VirtualJVProcessor::postEditSysex(const uint8_t *data, size_t length) {
  mcuLock.enter();
  mcu->postMidiSC55(data, (int)length);
  if (mcu2 != nullptr)
    mcu2->postMidiSC55(data, (int)length);
  mcuLock.exit();
}

void VirtualJVProcessor::setPerformanceModeEnabled(bool enabled) {
  if (!loaded)
    return;
  if (!enabled)
    endEditPerformancePart();

  mcuLock.enter();

  // System Area offset 0 = the real PATCH/PERFORM front-panel button's own state (0 =
  // Performance, 1 = Patch) - confirmed empirically (LCD screenshot + nvram[0x11] flipping
  // exactly as a simulated physical button press does - see CLAUDE.md). Flipping it over SysEx
  // does exactly what pressing the button does, on the SAME single `mcu` engine Patch mode
  // already uses - no parallel engines, no extra CPU cost versus Patch mode regardless of how
  // many of the 8 Parts are active.
  uint8_t modeByte = enabled ? 0 : 1;
  // Leaving Performance mode: Part 8 may have had an expansion/User rhythm set copied into Rhythm Temp; give
  // Patch mode back the rhythm set it was playing.
  if (!enabled && performanceModeEnabled && status.isDrums)
    memcpy(&mcu->nvram[0x67f0], status.drums, 0xa7c);
  sendSysexBlock(0, &modeByte, 1);

  if (enabled) {
    pushPerformanceCommonToEngine();
    for (int i = 0; i < kNumPerformanceParts; ++i)
      pushPerformancePartToEngine(i);
  }

  performanceModeEnabled = enabled;

  mcuLock.exit();

  if (enabled)
    rebuildExtraEngine(); // no-op unless extra polyphony is on
  else
    dropExtraEngine();

  savePerformanceSessionState();

  if (auto editor = safeActiveEditor())
    if (auto e = dynamic_cast<VirtualJVEditor *>(editor))
      e->updatePerformanceTab();
}

juce::File VirtualJVProcessor::performancesDir() {
  return appDataBaseDir()
      .getChildFile("Performances");
}

bool VirtualJVProcessor::savePerformanceAs(const juce::File &file) {
  juce::MemoryBlock block;
  appendPerformanceParts(block, performanceName, performanceParts, performanceCommon);
  appendCustomPartData(block);

  file.getParentDirectory().createDirectory();
  if (!file.replaceWithData(block.getData(), block.getSize()))
    return false;

  refreshPerformanceBank();
  return true;
}

// (Re)scans performancesDir() and rebuilds performanceBank - same "full rescan, no index file"
// approach as refreshUserPatches(), called at startup and after every savePerformanceAs().
void VirtualJVProcessor::refreshPerformanceBank() {
  performanceBank.clear();

  auto dir = performancesDir();
  dir.createDirectory();

  auto files = dir.findChildFiles(juce::File::findFiles, false, "*.jvpf");
  files.sort();

  for (auto &file : files) {
    if (file.getSize() != (int64_t)kPerfBaseBytes && file.getSize() < (int64_t)(kPerfBaseBytes + kPerfExtrasBytes))
      continue; // not a file this build wrote (wrong size) - skip rather than risk misreading it

    performanceBank.push_back({file.getFileNameWithoutExtension(), file});
  }
}

// Layout: "PCU1", then per Part: 1 byte (0 = nothing, 1 = bytes follow) and, if 1, the Part's Internal Patch
// Memory slot (0x16a bytes, Parts 1-7 with bank 0 and number 1..63) or Rhythm Temp (0xa7c bytes, Part 8, bank 0).
void VirtualJVProcessor::appendCustomPartData(juce::MemoryBlock &block) {
  block.append("PCU1", 4);
  mcuLock.enter();
  for (int i = 0; i < kNumPerformanceParts; ++i) {
    const auto &part = performanceParts[i];
    const bool rhythm = i == kNumPerformanceParts - 1;
    const bool custom = loaded && mcu != nullptr && part.present && part.bank == 0 && (rhythm || (part.number >= 1 && part.number < 64));
    const uint8_t flag = custom ? 1 : 0;
    block.append(&flag, 1);
    if (custom) {
      if (rhythm)
        block.append(&mcu->nvram[0x67f0], 0xa7c);
      else
        block.append(&mcu->nvram[0x0d70 + part.number * 0x16a], 0x16a);
    }
  }
  mcuLock.exit();
}

void VirtualJVProcessor::restoreCustomPartData(const uint8_t *bytes, size_t size) {
  if (!loaded || mcu == nullptr || size < 4 || memcmp(bytes, "PCU1", 4) != 0)
    return;
  size_t offset = 4;
  mcuLock.enter();
  for (int i = 0; i < kNumPerformanceParts; ++i) {
    if (offset >= size)
      break;
    const bool has = bytes[offset++] != 0;
    if (!has)
      continue;
    const auto &part = performanceParts[i];
    const bool rhythm = i == kNumPerformanceParts - 1;
    const size_t len = rhythm ? 0xa7c : 0x16a;
    if (offset + len > size)
      break;
    if (part.bank == 0 && (rhythm || (part.number >= 1 && part.number < 64))) {
      const size_t dest = rhythm ? 0x67f0 : (size_t)(0x0d70 + part.number * 0x16a);
      memcpy(&mcu->nvram[dest], bytes + offset, len);
      if (mcu2 != nullptr)
        memcpy(&mcu2->nvram[dest], bytes + offset, len);
      // Same single-expansion rule as injectCustomPatchIntoInternalMemory(): the last board restored wins.
      if (part.expansionI != 0xff && part.expansionI < NUM_EXPS && expansionsDescr[part.expansionI] != nullptr) {
        memcpy(mcu->pcm.waverom_exp, expansionsDescr[part.expansionI], 0x800000);
        if (mcu2 != nullptr)
          memcpy(mcu2->pcm.waverom_exp, expansionsDescr[part.expansionI], 0x800000);
      }
    }
    offset += len;
  }
  mcuLock.exit();
}

void VirtualJVProcessor::loadPerformance(int bankIndex) {
  if (bankIndex < 0 || bankIndex >= (int)performanceBank.size())
    return;

  juce::MemoryBlock block;
  if (!performanceBank[bankIndex].file.loadFileAsData(block))
    return;

  if (!readPerformanceParts(static_cast<const uint8_t *>(block.getData()), block.getSize(),
                             performanceName, performanceParts, performanceCommon))
    return;
  restoreCustomPartData(static_cast<const uint8_t *>(block.getData()) + kPerfBaseBytes + kPerfExtrasBytes,
                        block.getSize() > kPerfBaseBytes + kPerfExtrasBytes ? block.getSize() - kPerfBaseBytes - kPerfExtrasBytes : 0);

  mcuLock.enter();
  if (performanceModeEnabled) {
    pushPerformanceCommonToEngine();
    for (int i = 0; i < kNumPerformanceParts; ++i)
      pushPerformancePartToEngine(i);
  }
  mcuLock.exit();

  savePerformanceSessionState();

  if (auto editor = safeActiveEditor())
    if (auto e = dynamic_cast<VirtualJVEditor *>(editor))
      e->updatePerformanceTab();
}

juce::File VirtualJVProcessor::performanceSessionFile() {
  return appDataBaseDir()
      .getChildFile("performance_session.dat");
}

void VirtualJVProcessor::savePerformanceSessionState() {
  juce::MemoryBlock block;
  uint8_t modeByte = performanceModeEnabled ? 1 : 0;
  block.append(&modeByte, 1);
  appendPerformanceParts(block, performanceName, performanceParts, performanceCommon);
  appendCustomPartData(block);

  auto file = performanceSessionFile();
  file.getParentDirectory().createDirectory();
  file.replaceWithData(block.getData(), block.getSize());
}

// Called once at construction, after refreshPerformanceBank() - restores the 8 in-progress
// parts and whether Performance mode was on, so closing and reopening the plugin/app picks up
// right where it left off (Alan's request, 2026-09-07). Deliberately separate from the Bank
// (performancesDir()) - this file is never listed there.
void VirtualJVProcessor::loadPerformanceSessionState() {
  auto file = performanceSessionFile();
  if (!file.existsAsFile())
    return;

  juce::MemoryBlock block;
  if (!file.loadFileAsData(block) || block.getSize() < 1)
    return;

  const auto *bytes = static_cast<const uint8_t *>(block.getData());
  const bool wantEnabled = bytes[0] != 0;

  if (!readPerformanceParts(bytes + 1, block.getSize() - 1, performanceName, performanceParts, performanceCommon))
    return;
  restoreCustomPartData(bytes + 1 + kPerfBaseBytes + kPerfExtrasBytes,
                        block.getSize() > 1 + kPerfBaseBytes + kPerfExtrasBytes ? block.getSize() - 1 - kPerfBaseBytes - kPerfExtrasBytes : 0);

  if (wantEnabled)
    setPerformanceModeEnabled(true); // pushes Common + all 8 Parts to the engine from the state just read
}

const juce::String VirtualJVProcessor::getProgramName(int index) {
  // See setCurrentProgram()'s own comment on why this isn't bounded by getNumPrograms().
  if (index < 0 || index >= romPatchCapacity + userPatchCapacity || !patchInfos[index].present)
    return {};
  int length = patchInfos[index].nameLength;
  const char *strPtr = (const char *)patchInfos[index].name;
  return juce::String(strPtr, length);
}

void VirtualJVProcessor::changeProgramName(int /* index */,
                                                 const juce::String& /* newName */) { }

//==============================================================================
void VirtualJVProcessor::prepareToPlay(double sampleRate,
                                             int samplesPerBlock) {
  keyboardCollector.reset(sampleRate);
  midiRemapScratch.ensureSize(4096);
  extraScratchL.assign((size_t)juce::jmax(samplesPerBlock, 8192), 0.0f);
  extraScratchR.assign((size_t)juce::jmax(samplesPerBlock, 8192), 0.0f);
  for (auto &engine : partScratch)
    for (auto &part : engine)
      for (auto &ch : part)
        ch.assign((size_t)juce::jmax(samplesPerBlock, 8192), 0.0f);

  dspLoadMeasurer.reset(sampleRate, samplesPerBlock);
}

void VirtualJVProcessor::releaseResources() {
  // When playback stops, you can use this as an opportunity to free up any
  // spare memory, etc.
}

bool VirtualJVProcessor::isBusesLayoutSupported(
    const BusesLayout &layouts) const {
  // Main bus: stereo, or - for a Standalone audio device with many outputs - a discrete layout whose
  // channels 0-1 are the main mix and channels 2+2k, 3+2k are Part k+1 (processBlock()).
  const auto main = layouts.getMainOutputChannelSet();
  // Only the Standalone accepts it (any audio device with more than two outputs): in a DAW the Parts come
  // through the "Part n" buses instead, and a host must never pick a surround layout for this synth.
  const bool multiChannelMain = wrapperType == juce::AudioProcessor::wrapperType_Standalone && main.size() >= 4;
  if (main != juce::AudioChannelSet::stereo() && !multiChannelMain)
    return false;

  for (int i = 1; i < layouts.outputBuses.size(); i++) {
    const auto &aux = layouts.outputBuses.getReference(i);
    if (!aux.isDisabled() && aux != juce::AudioChannelSet::stereo())
      return false;
    if (multiChannelMain && !aux.isDisabled())
      return false;
  }

  return true;
}

void VirtualJVProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages)
{
  // Spans the whole block (MIDI handling + the actual DSP below) - Patch mode and Performance
  // mode alike now share the same single `mcu` engine (Performance mode v2 - see
  // PerformancePart's own comment in PluginProcessor.h), so this is a flat cost either way, same
  // convention as any other JUCE plugin's CPU meter. Read from SettingsTab via
  // dspLoadMeasurer.getLoadAsPercentage().
  juce::AudioProcessLoadMeasurer::ScopedTimer loadTimer(dspLoadMeasurer, buffer.getNumSamples());

  // The on-screen VirtualKeyboard's notes, queued by injectTestNote() - merged in exactly the
  // same way a real MIDI IN port's messages would be.
  keyboardCollector.removeNextBlockOfMessages(midiMessages, buffer.getNumSamples());

  // MIDI Remap (Alan's report, 2026-09-10): previously only VirtualKeyboard.cpp's own on-screen
  // clicks honoured this - a real external MIDI controller's own channel went straight through
  // untouched, so choosing "channel 2" here never actually routed a real keyboard playing on
  // channel 1 anywhere but channel 1. Stamping every incoming message (real MIDI IN and
  // on-screen alike - re-stamping the latter is a harmless no-op, it's already on this channel)
  // here, before anything downstream reads a channel off it, covers both: normal
  // channel-per-Performance-Part routing below, AND the sequencer's own armed-track channel
  // filter just below that (so what you record matches whichever track you actually meant).
  if (keyboardMidiRemap)
  {
    // midiRemapScratch is a member (pre-sized in prepareToPlay) rather than a local so this
    // doesn't allocate on the audio thread every block; swapWith() exchanges storage, so
    // both buffers keep their capacity across blocks.
    midiRemapScratch.clear();
    for (const auto metadata : midiMessages)
    {
      auto message = metadata.getMessage();
      message.setChannel(keyboardMidiChannel);
      midiRemapScratch.addEvent(message, metadata.samplePosition);
    }
    midiMessages.swapWith(midiRemapScratch);
  }

  mcuLock.enter();
  servicePendingPatch(buffer.getNumSamples());

  // Tracked once regardless of Patch/Performance mode - same value either way, so there's no
  // point recomputing it once per active performance slot below.
  for (const auto metadata : midiMessages)
  {
    auto message = metadata.getMessage();
    if (message.isNoteOnOrOff())
    {
      int note = message.getNoteNumber();
      if (note >= 0 && note < 128)
        noteActiveTable[static_cast<size_t>(note)].store(message.isNoteOn());
    }
  }

  juce::ScopedNoDenormals noDenormals;

  auto totalNumInputChannels = getTotalNumInputChannels();
  auto totalNumOutputChannels = getTotalNumOutputChannels();

  for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
  {
    buffer.clear(i, 0, buffer.getNumSamples());
  }

  if (!loaded)
  {
    mcuLock.exit();
    return;
  }

  const int numSamples = buffer.getNumSamples();

  // --- Sequencer (Standalone build only, Alan's request, 2026-09-09) ---------------------
  // Ported from the D-110 project's own processBlock() sequencer block, simplified: no per-
  // block firmware-RAM channel refresh needed (channelForTrack() already reads
  // performanceParts[] directly, wired once in the constructor) and no Program Change/Bank
  // resync block at all (JivSequencerHost::supportsProgramChange() stays false - see that
  // file's own top comment). Capture reads midiMessages as received (host/keyboard input)
  // BEFORE the sequencer's own playback is merged into it below, so the sequencer never
  // captures its own notes back into whatever track is armed.
  if (wrapperType == juce::AudioProcessor::wrapperType_Standalone && sequencerEnabled)
  {
    // PLAY/REC-edge patch/volume/pan recall (Alan's request, 2026-09-09) - see
    // handleAsyncUpdate()'s own comment for why this only flags the request here rather than
    // doing the actual work on the audio thread.
    const bool nowSequencerPlaying = sequencerEngine.isPlaying();
    if (nowSequencerPlaying && !wasSequencerPlayingForPatchPush)
      triggerAsyncUpdate();
    wasSequencerPlayingForPatchPush = nowSequencerPlaying;

    const double beatsPerSample = (sequencerEngine.getTempo() / 60.0) / getSampleRate();
    const double windowStartBeats = sequencerEngine.getPositionBeats();
    const int armed = sequencerEngine.isRecording() ? sequencerEngine.getArmedTrack() : -1;
    if (armed >= 0)
    {
      const int armedChannel = sequencerEngine.channelForTrack(armed);
      for (const auto metadata : midiMessages)
      {
        const auto &msg = metadata.getMessage();
        if (msg.isNoteOnOrOff() && msg.getChannel() == armedChannel)
          sequencerEngine.captureEvent(
              msg, windowStartBeats + static_cast<double>(metadata.samplePosition) * beatsPerSample);
      }
    }

    const int stepArmed = sequencerEngine.isStepRecording() ? sequencerEngine.getArmedTrack() : -1;
    if (stepArmed >= 0)
    {
      const int armedChannel = sequencerEngine.channelForTrack(stepArmed);
      for (const auto metadata : midiMessages)
      {
        const auto &msg = metadata.getMessage();
        if (msg.getChannel() != armedChannel) continue;
        if (msg.isNoteOn()) sequencerEngine.stepNoteOn(msg.getNoteNumber(), msg.getVelocity());
        else if (msg.isNoteOff()) sequencerEngine.stepNoteOff(msg.getNoteNumber());
      }
    }

    // Rendered straight into midiMessages (rather than a separate buffer merged in later,
    // like the D-110 project does for its own additional direct-MIDI-Out step) - this project
    // has no such external MIDI Out port to also feed (Alan's own call, 2026-09-09: "je
    // n'utilise pas le vrai synthé hardware"), so there's nothing else that needs to tell
    // sequencer-originated notes apart from host/keyboard-originated ones.
    juce::MidiBuffer sequencerOut;
    sequencerClicks.clear();
    sequencerEngine.renderInto(sequencerOut, numSamples, getSampleRate(),
                                sequencerEngine.getMetronomeEnabled() ? &sequencerClicks : nullptr);
    for (const auto metadata : sequencerOut)
      midiMessages.addEvent(metadata.getMessage(), metadata.samplePosition);
  }

  for (const auto metadata : midiMessages)
  {
    auto message = metadata.getMessage();

    // Patch mode always answers on a single fixed channel (matching the current Patch/Rhythm
    // Temp's own RxCH), same as before. Performance mode forwards each message's real incoming
    // channel unchanged - the firmware's own 8 Parts (configured over SysEx by
    // pushPerformancePartToEngine()) each answer their own receivechannel, exactly like a real
    // JV-880's Performance Play mode routes a multitimbral MIDI cable itself.
    if (!performanceModeEnabled)
      message.setChannel(status.isDrums ? 10 : 1);

    // The JV-880 only receives CHANNEL aftertouch (its MIDI implementation chart lists polyphonic key
    // pressure as not received), but many hosts and controllers - FL Studio's aftertouch automation, for
    // one (GitHub issue #2) - send the polyphonic form. Forward it as channel pressure so it is not lost.
    if (message.isAftertouch())
      message = juce::MidiMessage::channelPressureChange(message.getChannel(), message.getAfterTouchValue());

    int samplePos = int(((double)metadata.samplePosition / getSampleRate()) * 64000.0);

    // Extra polyphony: Note Ons alternate between the two engines, everything else (Note Off,
    // CC, Program Change, bend, aftertouch, SysEx) goes to both - a Note Off for a note the other
    // engine never played is harmless.
    if (mcu2 != nullptr && performanceModeEnabled) {
      if (message.isNoteOn()) {
        MCU *target = (extraNoteToggle++ & 1u) ? mcu2 : mcu;
        target->enqueueMidiSC55(message.getRawData(), message.getRawDataSize(), samplePos);
      } else {
        mcu->enqueueMidiSC55(message.getRawData(), message.getRawDataSize(), samplePos);
        mcu2->enqueueMidiSC55(message.getRawData(), message.getRawDataSize(), samplePos);
      }
    } else {
      mcu->enqueueMidiSC55(message.getRawData(), message.getRawDataSize(), samplePos);
    }
  }

  float *channelDataL = buffer.getWritePointer(0);
  float *channelDataR = buffer.getWritePointer(1);

  // Separate outputs (GitHub issue #3). Only Performance mode has Parts to separate, and only when the host
  // enabled at least one "Part n" bus or the Standalone device offers more than two outputs; otherwise the
  // engine renders the plain stereo mix exactly as before.
  const int mainChannels = getChannelCountOfBus(false, 0);
  bool partBusOn[8] = {false};
  bool anyPartBus = false;
  for (int p = 0; p < 8; p++) {
    if (mainChannels == 2 && getBusCount(false) > 1 + p) {
      if (auto *bus = getBus(false, 1 + p))
        partBusOn[p] = bus->isEnabled();
    } else if (mainChannels > 2) {
      partBusOn[p] = 3 + 2 * p < mainChannels;
    }
    anyPartBus = anyPartBus || partBusOn[p];
  }
  const bool splitParts = performanceModeEnabled && anyPartBus;
  if (std::getenv("JV880_DEBUG_BUSES")) {
    static int blocks = 0;
    if (++blocks % 400 == 1)
      std::fprintf(stderr, "[buses] mainChannels=%d bufferChannels=%d buses(out)=%d perfMode=%d anyPartBus=%d split=%d fullMix=%d wrapper=%d\n", mainChannels,
                   buffer.getNumChannels(), getBusCount(false), (int)performanceModeEnabled, (int)anyPartBus, (int)splitParts, (int)mainFullMix.load(),
                   (int)wrapperType);
  }
  mcu->setSeparateParts(splitParts);
  if (mcu2 != nullptr)
    mcu2->setSeparateParts(splitParts);
  for (int c = 2; c < buffer.getNumChannels(); c++)
    buffer.clear(c, 0, numSamples);

  float *partPtrL[2][8], *partPtrR[2][8];
  if (splitParts)
    for (int e = 0; e < 2; e++)
      for (int p = 0; p < 8; p++) {
        auto &l = partScratch[e][p][0];
        auto &r = partScratch[e][p][1];
        if ((int)l.size() < numSamples) { // beyond prepareToPlay's size: rare, allocates once
          l.resize((size_t)numSamples);
          r.resize((size_t)numSamples);
        }
        partPtrL[e][p] = l.data();
        partPtrR[e][p] = r.data();
      }

  mcu->updateSC55WithSampleRate(channelDataL, channelDataR, numSamples, (int)getSampleRate(),
                                splitParts ? partPtrL[0] : nullptr, splitParts ? partPtrR[0] : nullptr);

  if (mcu2 != nullptr && performanceModeEnabled) {
    if ((int)extraScratchL.size() < numSamples) { // beyond prepareToPlay's size: rare, allocates once
      extraScratchL.resize((size_t)numSamples);
      extraScratchR.resize((size_t)numSamples);
    }
    mcu2->updateSC55WithSampleRate(extraScratchL.data(), extraScratchR.data(), numSamples, (int)getSampleRate(),
                                   splitParts ? partPtrL[1] : nullptr, splitParts ? partPtrR[1] : nullptr);
    for (int i = 0; i < numSamples; ++i) {
      channelDataL[i] += extraScratchL[(size_t)i];
      channelDataR[i] += extraScratchR[(size_t)i];
    }
    if (splitParts)
      for (int p = 0; p < 8; p++)
        for (int i = 0; i < numSamples; ++i) {
          partPtrL[0][p][i] += partPtrL[1][p][i];
          partPtrR[0][p][i] += partPtrR[1][p][i];
        }
  }

  if (splitParts && outputMode == 1) {
    // Main + Sub: the Sub pair carries the dry sum of the Parts set to Sub (no effects, like the hardware's SUB
    // jack); the main keeps everything else, effects return included. Needs the first extra pair.
    float *subL = nullptr, *subR = nullptr;
    if (partBusOn[0]) {
      if (mainChannels > 2) {
        subL = buffer.getWritePointer(2);
        subR = buffer.getWritePointer(3);
      } else {
        auto bus = getBusBuffer(buffer, false, 1);
        subL = bus.getWritePointer(0);
        subR = bus.getWritePointer(1);
      }
    }
    if (subL != nullptr) {
      for (int i = 0; i < numSamples; ++i)
        subL[i] = subR[i] = 0.0f;
      for (int p = 0; p < 8; p++) {
        if (performanceParts[p].outputSelect != 1)
          continue;
        for (int i = 0; i < numSamples; ++i) {
          subL[i] += partPtrL[0][p][i];
          subR[i] += partPtrR[0][p][i];
          if (!mainFullMix) {
            channelDataL[i] -= partPtrL[0][p][i];
            channelDataR[i] -= partPtrR[0][p][i];
          }
        }
      }
    }
  } else if (splitParts) {
    // Each routed Part leaves the main mix and goes to its own bus; the main output keeps the effects return,
    // unrouted Parts and anything the firmware did not attribute to a Part.
    for (int p = 0; p < 8; p++) {
      if (!partBusOn[p])
        continue;
      float *outL, *outR;
      if (mainChannels > 2) {
        outL = buffer.getWritePointer(2 + 2 * p);
        outR = buffer.getWritePointer(3 + 2 * p);
      } else {
        auto bus = getBusBuffer(buffer, false, 1 + p);
        outL = bus.getWritePointer(0);
        outR = bus.getWritePointer(1);
      }
      for (int i = 0; i < numSamples; ++i) {
        outL[i] = partPtrL[0][p][i];
        outR[i] = partPtrR[0][p][i];
        if (!mainFullMix) {
          channelDataL[i] -= partPtrL[0][p][i];
          channelDataR[i] -= partPtrR[0][p][i];
        }
      }
    }
  }

  // Internal metronome click synthesis (ported from the D-110 project's own processBlock(),
  // see this file's own MetronomeClick member comments) - the alternative to real notes on the
  // rhythm channel, active whenever getMetronomeUseChannel10() is off. The
  // metronomeSamplesRemaining > 0 half of the condition matters even on blocks with no NEW
  // click: a click's ~30ms decay outlives a typical audio block, so this keeps ringing it out
  // smoothly across block boundaries instead of pausing and dumping the rest late.
  if (wrapperType == juce::AudioProcessor::wrapperType_Standalone && sequencerEnabled
      && (!sequencerClicks.empty() || metronomeSamplesRemaining > 0)
      && !sequencerEngine.getMetronomeUseChannel10())
  {
    constexpr double kClickSeconds = 0.03;
    const int clickTotalSamples = juce::jmax(1, static_cast<int>(getSampleRate() * kClickSeconds));
    const float volume = sequencerEngine.getMetronomeVolume();
    int cursor = 0;
    auto ringUpTo = [&](int endSample) {
      for (int i = cursor; i < endSample; ++i)
      {
        if (metronomeSamplesRemaining <= 0) continue;
        const float amp = static_cast<float>(metronomeSamplesRemaining) / static_cast<float>(clickTotalSamples);
        const float s = std::sin(metronomePhase) * amp * 0.25f * volume;
        metronomePhase += juce::MathConstants<double>::twoPi * metronomeFreq / getSampleRate();
        channelDataL[i] += s;
        channelDataR[i] += s;
        --metronomeSamplesRemaining;
      }
    };
    for (const auto &click : sequencerClicks)
    {
      ringUpTo(click.samplePosition);
      cursor = click.samplePosition;
      metronomeSamplesRemaining = clickTotalSamples;
      metronomeFreq = click.downbeat ? 1500.0 : 1000.0;
      metronomePhase = 0.0;
    }
    ringUpTo(numSamples);
  }

  mcuLock.exit();

  buffer.applyGain(kOutputMakeupGain * masterVolume);
}

//==============================================================================
bool VirtualJVProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor *VirtualJVProcessor::createEditor() {
  return new VirtualJVEditor(*this);
}

//==============================================================================
void VirtualJVProcessor::getStateInformation(juce::MemoryBlock &destData)
{
  mcuLock.enter();
  status.masterTune = mcu->nvram[0x00];
  status.reverbEnabled = ((mcu->nvram[0x02] >> 0) & 1) == 1;
  status.chorusEnabled = ((mcu->nvram[0x02] >> 1) & 1) == 1;
  mcuLock.exit();

  destData.ensureSize(sizeof(DataToSave) + 8);
  destData.replaceAll(&status, sizeof(DataToSave));
  // Master volume rides after the fixed blob, behind a tag: DataToSave is memcpy'd as it is, so a
  // new field there would break every project saved before (GitHub issue #5, volume reset to 100).
  destData.append("MVOL", 4);
  const float volume = masterVolume;
  destData.append(&volume, sizeof(volume));
}

void VirtualJVProcessor::setStateInformation(const void *data, int sizeInBytes)
{
  // A shorter blob (older/foreign plugin state, truncated project) would otherwise be read
  // past its end straight into `status` - keep whatever is currently loaded instead.
  if (data == nullptr || sizeInBytes < static_cast<int>(sizeof(DataToSave)))
    return;
  memcpy(&status, data, sizeof(DataToSave));
  if (sizeInBytes >= static_cast<int>(sizeof(DataToSave)) + 8) {
    const auto *tail = static_cast<const uint8_t *>(data) + sizeof(DataToSave);
    if (std::memcmp(tail, "MVOL", 4) == 0) {
      float volume = 1.0f;
      std::memcpy(&volume, tail + 4, sizeof(volume));
      setMasterVolume(volume); // clamps to 0..1
    }
  }
  // Same defence as setCurrentProgram()'s expansionI check: an out-of-range index would
  // read past expansionsDescr[] below.
  if (status.currentExpansion < 0 || status.currentExpansion >= NUM_EXPS)
    status.currentExpansion = 0;

  mcuLock.enter();

  mcu->nvram[0x0d] |= 1 << 5; // LastSet
  mcu->nvram[0x00] = status.masterTune;
  mcu->nvram[0x02] = status.reverbEnabled | status.chorusEnabled << 1;

  if (expansionsDescr[status.currentExpansion] == nullptr) {
    mcuLock.exit();
    return;
  }

  memcpy(mcu->pcm.waverom_exp, expansionsDescr[status.currentExpansion],
         0x800000);
  mcu->nvram[0x11] = status.isDrums ? 0 : 1;
  memcpy(&mcu->nvram[0x67f0], status.drums, 0xa7c);
  memcpy(&mcu->nvram[0x0d70], status.patch, 0x16a);
  mcu->SC55_Reset();
  mcuLock.exit();

  if (auto editor = safeActiveEditor())
  {
      auto e = dynamic_cast<VirtualJVEditor*>(editor);

      e->setLCDColor((LCDisplay::Color)status.selectedLCDColor);
      e->showToneOrRhythmEditTabs(status.isDrums);
      e->setSelectedTab(status.selectedTab);
      e->updateEditTabs();

      if (status.selectedRom > -1)
      {
          e->setSelectedROM(status.selectedRom);
      }
  }
}

void VirtualJVProcessor::injectTestNote(int channel, int note, float velocity, bool on) {
  auto message = on ? juce::MidiMessage::noteOn(channel, note, velocity)
                     : juce::MidiMessage::noteOff(channel, note, velocity);
  message.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
  keyboardCollector.addMessageToQueue(message);
}

void VirtualJVProcessor::setKeyboardPcInputEnabled(bool enabled) {
  keyboardPcInput = enabled;
  savePersistedKeyboardSettings(keyboardPcInput, keyboardPcLayout);
}

void VirtualJVProcessor::setKeyboardPcLayout(int layout) {
  keyboardPcLayout = juce::jlimit(0, 1, layout);
  savePersistedKeyboardSettings(keyboardPcInput, keyboardPcLayout);
}

// Roland DT1 ("Data Set 1") send: F0 41 10 46 12 <4x7bit address, MSB first> <data...>
// <checksum> F7, checksum = (0x80 - (sum(address bytes + data bytes) & 0x7F)) & 0x7F. Same
// command real hardware/librarians (this project used Sean Luke's Edisyn, see CLAUDE.md, to
// transcribe the JV-880's own address map) use to write directly into the firmware's temporary
// or stored memory over MIDI IN - the firmware's own SysEx receiver resolves the address, so the
// caller never needs to know where a given parameter actually lives in RAM. Caller must already
// hold mcuLock.
void VirtualJVProcessor::sendSysexBlock(uint32_t address, const uint8_t *data, size_t length) {
  uint8_t addrBytes[4] = {
      (uint8_t)((address >> 21) & 127),
      (uint8_t)((address >> 14) & 127),
      (uint8_t)((address >> 7) & 127),
      (uint8_t)((address >> 0) & 127),
  };

  uint32_t checksum = 0;
  for (uint8_t b : addrBytes) checksum += b;
  for (size_t i = 0; i < length; i++) checksum += data[i];
  checksum = (0x80 - (checksum & 0x7f)) & 0x7f;

  std::vector<uint8_t> buf;
  buf.reserve(11 + length);
  buf.push_back(0xf0);
  buf.push_back(0x41);
  buf.push_back(0x10); // unit number
  buf.push_back(0x46); // JV-880 model ID
  buf.push_back(0x12); // command: DT1
  for (uint8_t b : addrBytes) buf.push_back(b);
  for (size_t i = 0; i < length; i++) buf.push_back(data[i]);
  buf.push_back((uint8_t)checksum);
  buf.push_back(0xf7);

  mcu->postMidiSC55(buf.data(), (int)buf.size());
  if (mcu2 != nullptr)
    mcu2->postMidiSC55(buf.data(), (int)buf.size());
  lastPerfSysexMs = juce::Time::getMillisecondCounterHiRes();
  lastFirmwarePerfSnapshot.clear(); // our own write: re-baseline instead of importing it back
}

namespace {
constexpr size_t kFwPerfName = 0x206a, kFwPerfCommon = 0x2076, kFwPerfPart0 = 0x2096;
constexpr size_t kFwPerfPartStride = 22;

std::vector<uint8_t> firmwarePerfSnapshot(const uint8_t *sram) {
  std::vector<uint8_t> v(sram + kFwPerfName, sram + kFwPerfName + 12);
  v.insert(v.end(), sram + kFwPerfCommon, sram + kFwPerfCommon + 16);
  for (int i = 0; i < 8; i++)
    v.insert(v.end(), sram + kFwPerfPart0 + kFwPerfPartStride * i - 1, sram + kFwPerfPart0 + kFwPerfPartStride * i + 6);
  return v;
}
} // namespace

bool VirtualJVProcessor::importPerformanceFromFirmware() {
  if (!loaded)
    return false;
  mcuLock.enter();
  const uint8_t *sram = mcu->sram;

  memcpy(performanceName, sram + kFwPerfName, 12);
  performanceName[12] = '\0';
  for (int i = 0; i < 12; i++)
    if (performanceName[i] < 32 || performanceName[i] > 126) performanceName[i] = ' ';

  const uint8_t *c = sram + kFwPerfCommon;
  performanceCommon.keyMode = (c[0] >> 6) & 3;
  performanceCommon.chorusType = (c[0] >> 3) & 3;
  performanceCommon.reverbType = c[0] & 7;
  performanceCommon.reverbLevel = c[1];
  performanceCommon.reverbTime = c[2];
  performanceCommon.reverbFeedback = c[3];
  performanceCommon.chorusLevel = c[4] & 0x7f;
  performanceCommon.chorusOutput = (c[4] >> 7) & 1;
  performanceCommon.chorusDepth = c[5];
  performanceCommon.chorusRate = c[6];
  performanceCommon.chorusFeedback = c[7];
  for (int i = 0; i < 8; i++)
    performanceCommon.voiceReserve[i] = c[8 + i];

  for (int i = 0; i < kNumPerformanceParts; i++) {
    const uint8_t *r = sram + kFwPerfPart0 + kFwPerfPartStride * (size_t)i - 1;
    auto &part = performanceParts[i];
    part.rxProgramChange = (r[0] & 0x80) != 0;
    part.rxVolume = (r[0] & 0x40) != 0;
    part.rxHold1 = (r[0] & 0x20) != 0;
    const int value = r[1];
    part.bank = (uint8_t)((value >> 6) & 3);
    part.number = (uint8_t)(value & 63);
    part.level = r[2];
    part.pan = r[3];
    part.coarseTune = juce::jlimit(16, 112, (int)(int8_t)r[4] + 64);
    part.fineTune = juce::jlimit(14, 114, (int)(int8_t)r[5] + 64);
    part.enabled = (r[6] & 0x80) != 0;
    part.reverbSwitch = (r[6] & 0x40) != 0;
    part.chorusSwitch = (r[6] & 0x20) != 0;
    part.midiChannel = (r[6] & 0x0f) + 1;
    part.present = true;
    part.isRhythm = (i == kNumPerformanceParts - 1);
    part.expansionI = 0xff;

    juce::String nm;
    if (part.isRhythm) {
      const int idx = part.bank == 2 ? 192 : part.bank == 3 ? 193 : 194;
      nm = juce::String(patchInfos[idx].name, patchInfos[idx].nameLength);
    } else if (part.bank == 2 || part.bank == 3) {
      const auto &info = patchInfos[(part.bank - 2) * 64 + part.number];
      nm = juce::String(info.name, info.nameLength);
    } else if (part.bank == 0) {
      nm = juce::String(reinterpret_cast<const char *>(&mcu->nvram[0x0d70 + (size_t)part.number * 0x16a]), 12);
    } else {
      nm = "Card " + juce::String(part.number + 1);
    }
    nm = nm.trim();
    memset(part.name, 0, sizeof(part.name));
    memcpy(part.name, nm.toRawUTF8(), std::min((size_t)nm.getNumBytesAsUTF8(), sizeof(part.name) - 1));
  }
  lastFirmwarePerfSnapshot = firmwarePerfSnapshot(sram);
  mcuLock.exit();
  savePerformanceSessionState();
  return true;
}

bool VirtualJVProcessor::pollPerformanceFromFirmware() {
  if (!loaded || !performanceModeEnabled || mcu->nvram[0x11] != 0)
    return false;
  // Recent writes of our own are still on their way into the firmware: never read those back.
  if (juce::Time::getMillisecondCounterHiRes() - lastPerfSysexMs < 1500.0)
    return false;
  mcuLock.enter();
  const auto snap = firmwarePerfSnapshot(mcu->sram);
  mcuLock.exit();
  if (lastFirmwarePerfSnapshot.empty()) {
    lastFirmwarePerfSnapshot = snap;
    return false;
  }
  if (snap == lastFirmwarePerfSnapshot)
    return false;
  const bool ok = importPerformanceFromFirmware();
  if (ok && mcu2 != nullptr) {
    // The second engine did not see the Panel's load: mirror the imported state into it.
    pushPerformanceCommonToEngine();
    for (int i = 0; i < kNumPerformanceParts; i++)
      pushPerformancePartToEngine(i);
  }
  return ok;
}

void VirtualJVProcessor::sendSysexParamChange(uint32_t address, uint8_t value) {
  mcuLock.enter();
  sendSysexBlock(address, &value, 1);
  mcuLock.exit();
}

#define BITSWAP16(x) (((x & 0xFF00) >> 8) | ((x & 0x00FF) << 8))
#define BITSWAP32(x) (((x & 0xFF000000) >> 24) | ((x & 0x00FF0000) >> 8) | ((x & 0x0000FF00) << 8) | ((x & 0x000000FF) << 24))

std::vector<std::string> VirtualJVProcessor::readMultisampleNames(uint8_t romIdx)
{
    std::vector<std::string> names;

    if (!romInfos[romIdx].loaded)
    {
        return names;
    }

    auto& msNamePtr = loadedRoms[romIdx];
    const int msOffset = 0x3c;
    uint16_t msCount;
    uint32_t msTableAddr;

    memcpy(&msCount, &msNamePtr[0x62], 2);
    memcpy(&msTableAddr, &msNamePtr[0x84], 4);

    // ROMs are written in big endian format...
    msCount = BITSWAP16(msCount);
    msTableAddr = BITSWAP32(msTableAddr);

    // 880's factory multisamples start from a different place in ROM
    if (romIdx == 2)
    {
        msCount = 129;
        msTableAddr = 4;
    }

    std::string name;

    name.reserve(12u);

    for (int i = 0; i < msCount; i++)
    {
        name.clear();

        for (int c = 0; c < 12; c++)
        {
            name.push_back((char)msNamePtr[msTableAddr + (msOffset * i) + c]);
        }

        names.emplace_back(name);
    }

    return names;
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new VirtualJVProcessor();
}
