/*
  ==============================================================================

    PanelSkin.h

    Photo-based JV-880 front panel control, shared by two different places (Alan's request,
    2026-09-08 - "fais pareil que pour le D110"): the top-of-window display (Settings tab lets
    Alan pick "Panel Compact" or "Panel Full" instead of the plain LCD-only strip, replacing
    LCDisplay there and embedding that same live LCD render into the photo, like D110Panel does)
    and the Interface tab (an ultra-compact buttons-only crop, for anyone who keeps "LCD only" up
    top and still wants clickable panel buttons somewhere).

    All three photos this class can be pointed at - jiv881.png (full, 3280x304), jiv881_panel_
    compact.png (2012x304) and jiv881_commands.png (buttons/dial only, no LCD/Volume, 948x304) -
    turned out to be EXACT pixel crops of the same underlying artwork (confirmed by a direct
    numpy diff, not assumed): jiv881_panel_compact.png == jiv881.png's columns [382, 2394), and
    jiv881_commands.png == jiv881.png's columns [1435, 2383). So there is exactly ONE measured
    coordinate table (kButtons, the dial/volume circle centres, the LCD glass rect - all below, in
    "full" reference space, kMasterRefW x kMasterRefH = 3280x304, found by the same connected-
    component analysis used for the original compact-only skin, not eyeballed) and each Variant
    below is just that table windowed by an x-offset and a width - see Variant::kFull/kCompact/
    kCommands.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../PluginProcessor.h"
#include "widgets/LCDisplay.h"

//==============================================================================
class PanelSkin : public juce::Component, private juce::Timer
{
public:
    // Which photo crop to draw and hit-test against - see this file's own header comment for how
    // these three were derived. `hasLcdAndVolume` is false only for kCommands: that crop starts
    // right of the VOLUME knob and the LCD glass, so neither is drawn or clickable there.
    struct Variant
    {
        const void *imageData;
        int imageDataSize;
        float refOffsetX; // this crop's left edge, in the shared 3280-wide reference space
        float refW;       // this crop's own width (== the embedded PNG's actual width)
        bool hasLcdAndVolume;

        static const Variant kFull;
        static const Variant kCompact;
        static const Variant kCommands;
    };

    // `lcdColorMenuOwner` is who to ask to pop the LCD colour-picker menu (Alan's request,
    // 2026-09-08 - right-click on the embedded LCD in Panel mode should do the same thing as
    // right-clicking the standalone LCDisplay strip does) - null where there's no embedded LCD to
    // right-click in the first place (kCommands, the Interface/"Panel" tab's own crop).
    PanelSkin(VirtualJVProcessor &, Variant initialVariant, LCDisplay *lcdColorMenuOwner = nullptr);

    // Right-click anywhere on the panel that isn't one of the controls with a right-click
    // function of their own (DATA, VOLUME, TONE SELECT) asks the owner for its app-wide context
    // menu - LCD colours, sequencer view (Alan's request, 2026-09-21: the whole panel area, not
    // just the LCD). Unset (the Interface tab's skin): right-click keeps acting as a plain click.
    std::function<void()> onContextMenu;

    // Switches to a different crop of the same underlying artwork (e.g. Settings toggling
    // between Panel Full and Panel Compact) - reloads the image and re-triggers resized()/repaint,
    // no need to reconstruct the component.
    void setVariant(Variant v);

    // Reference-space height is always 304 (kMasterRefH) regardless of variant - callers that
    // need to reserve vertical space before this component has been given real bounds (the
    // editor's own resized(), sizing the top strip before laying out everything below it) can
    // compute it from a prospective width without waiting for a paint/resize round-trip.
    float heightForWidth(float width) const { return width * (kMasterRefH / variant.refW); }

    void paint(juce::Graphics &) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

private:
    void timerCallback() override; // ~25Hz repaint while the live LCD is embedded, same rate as
                                    // LCDisplay's own RedrawTimer - stopped when it isn't (kCommands).

    struct PanelButton
    {
        float x, y, w, h; // reference-space rect, in the SHARED (full, 3280-wide) coordinate space
        uint8_t buttonId; // MCU_BUTTON_* (see mcu.h)
    };
    static constexpr int kNumButtons = 12;
    static const PanelButton kButtons[kNumButtons];

    static constexpr float kMasterRefW = 3280.0f, kMasterRefH = 304.0f;
    static constexpr float kDialCx = 1582.5f, kDialCy = 106.0f, kDialR = 47.0f;
    static constexpr float kVolCx = 486.5f, kVolCy = 186.0f, kVolR = 37.5f;
    static constexpr float kLcdX = 649.0f, kLcdY = 98.0f, kLcdW = 656.0f, kLcdH = 108.0f;

    static constexpr float kHitPadRefX = 4.0f, kHitPadRefY = 17.0f, kHitPadRefR = 15.0f;

    juce::Rectangle<float> imageDrawArea; // this component's own coordinates
    float imageScale = 1.0f;

    juce::Point<float> refToComponent(juce::Point<float> ref) const;
    juce::Point<float> componentToRef(juce::Point<float> comp) const;
    juce::Rectangle<float> refRectToComponent(float x, float y, float w, float h) const;

    static constexpr int kHitNone = -1, kHitDataDial = -2, kHitVolumeKnob = -3, kHitLcd = -4;
    int hitTest(juce::Point<float> componentPos) const;

    // Small panel LEDs (Alan's request, 2026-09-08, working from real-hardware reference photos
    // he sent mid-session): every one of them is drawn the same way - a small red rounded rect,
    // centred just above the button's own top edge, matching what the real panel actually looks
    // like (his correction: they're centred, not left-aligned the way the first cut had TONE
    // SWITCH's own). None of these are baked into the photo itself - all synthesized.
    // kLedYOffset nudges the LED down slightly from dead-centre on the button's own top edge
    // (Alan's correction, 2026-09-08: the first cut floated the LED entirely above the button, in
    // the label gap - it should instead sit mostly ON the button, right at its top boundary,
    // without poking up past it the way it did before).
    static constexpr float kLedW = 14.0f, kLedH = 7.0f, kLedYOffset = 2.0f;
    juce::Rectangle<float> ledRect(int buttonIndex) const; // component space - shared by paint()
                                                             // and timerCallback()'s targeted repaint()

    // PATCH/PERFORM's own LED (kButtons[0]): nvram[0x11] != 0 (Patch mode) - the Owner's Manual's
    // own words, "the indicator lights up when Patch mode is selected" - same flag
    // setPerformanceModeEnabled()/the self-test already use elsewhere, so this one's real,
    // firmware-read, 100% confidence.
    bool modeLedLit(int buttonIndex) const; // buttonIndex 0 only now - see activeModeButton below

    // EDIT/SYSTEM/RHYTHM/UTILITY (kButtons[1..4]): the Owner's Manual documents these as a plain
    // mutually-exclusive toggle group - "Press SYSTEM (the indicator lights up)", press it again
    // (or a different one of the four) to leave that screen and turn it off. Originally read from
    // live firmware RAM (sram[0x00b6]/[0x00b2]/[0x00ae]/[0x00bd], found by headless RE - see git
    // history) - dropped after Alan reported it flickering unpredictably in normal use, most
    // likely because that memory is also reused for something unrelated once real voices are
    // playing (the RE itself was done in silence). Tracked app-side instead: this component is
    // the only thing that ever sends these four buttons to the firmware, so mirroring the
    // documented toggle here directly can't flicker, at the cost of being able to drift if the
    // real firmware ever leaves that screen some other way this UI doesn't know about (accepted
    // as the better trade). -1 = none active; toggled in mouseDown, cleared when PATCH_PERFORM is
    // pressed (returning to Patch/Perform Play always exits any of these four on real hardware).
    int activeModeButton = -1;

    // TONE SWITCH 1-4 (kButtons[8..11] - MUTE/MONITOR/COMPARE/ENTER): lit = tone active, not
    // muted (Alan's correction - the opposite of this project's first guess). Live firmware RAM -
    // see toneMutedLive()'s own comment for how that address map was found.
    bool toneMutedLive(int toneIndex) const; // toneIndex 0..3

    LCDisplay *lcdColorMenuOwner = nullptr;

    // Touch has no right button: pressing and holding the embedded LCD (~0.5 s, without moving)
    // opens the same colour/context menu a desktop right-click does (Alan's request, 2026-10-01).
    // Each press bumps the token; mouseUp()/a drag bump it again to void a pending timer.
    int lcdLongPressToken = 0;
    juce::Point<float> lcdLongPressStart;

    // Ctrl+click latching (Alan's request, 2026-09-08 - "comme pour le D110"): a button pressed
    // with Ctrl held stays down (LCD_SendButton(...,1) sent once, never auto-released) until
    // Ctrl-clicked again, instead of the normal momentary down-on-press/up-on-release. Lets two
    // buttons be "held" at once with a mouse - needed for the real combos the Owner's Manual
    // documents (hold TONE SELECT + press a TONE SWITCH to pick which Tone to edit; hold PARAM
    // SHIFT - the same cap's other printed function - + press -/+ to change a value regardless of
    // cursor position). Applies to every button in kButtons except none are excluded - DATA and
    // VOLUME aren't in kButtons and have their own separate latching (dataHeld; VOLUME has
    // no latched/held state at all, only its own right-click-for-PREVIEW).
    bool buttonLatched[kNumButtons] = {false};

    void pressButton(int hit, bool down);
    void fireEncoderStep(int direction); // 0 = Data -, 1 = Data +
    void toggleDataHeld(); // see this method's own comment in PanelSkin.cpp

    void rebuildLcdImage(); // only when variant.hasLcdAndVolume

    VirtualJVProcessor &processor;
    Variant variant;
    juce::Image panelImage;
    juce::Image lcdImage; // the emulator's own live dot-matrix render, re-fetched on each timer tick

    int pressedButtonIndex = kHitNone;
    float dialDragStartY = 0.0f;
    int dialStepsFired = 0;
    float dialAngleDeg = 0.0f;
    static constexpr float kDialPxPerStep = 6.0f;
    static constexpr float kDialDegPerStep = 14.0f;
    static constexpr float kVolumeDragRangePx = 150.0f; // px of vertical drag spanning the full 0..1 range
    // DATA-held state (Alan's request, 2026-09-08): Ctrl-clicking or right-clicking the dial
    // toggles this flag via toggleDataHeld() (which also owns sending MCU_BUTTON_DATA down/up) -
    // so it stays held after mouse-up/Ctrl release too, "comme pour le D110", rather than only
    // for the duration of one drag. No separate on-screen checkbox anymore (Alan's request,
    // 2026-09-08 - redundant with the two click gestures that already toggle it). See
    // mouseDown()'s own comment for the dial.
    bool dataHeld = false;

    // VOLUME/PREVIEW knob (Alan's request, 2026-09-08): unlike DATA, this one has a real
    // continuous parameter of its own (the plugin-side Master Volume, same value the Settings
    // tab's own slider controls - see VirtualJVProcessor::getMasterVolume()/setMasterVolume()) -
    // on the real hardware VOLUME is an analog knob the firmware never sees, PREVIEW is its push
    // function. A left-drag here now adjusts that volume directly (matching what rotating the
    // real knob would do); PREVIEW moved to a right-click (there's no separate "push" gesture
    // free for a knob whose rotation is already spoken for) - deliberately not labelled on the
    // photo itself, per Alan's request.
    float volumeDragStartY = 0.0f;
    float volumeDragStartValue = 0.0f;
    bool volumeRightClickPreview = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PanelSkin)
};
