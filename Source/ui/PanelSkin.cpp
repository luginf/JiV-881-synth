/*
  ==============================================================================

    PanelSkin.cpp

  ==============================================================================
*/

#include "PanelSkin.h"
#include <cmath>

//==============================================================================
const PanelSkin::Variant PanelSkin::Variant::kFull{
    BinaryData::jiv881_png, BinaryData::jiv881_pngSize, 0.0f, 3280.0f, true};
const PanelSkin::Variant PanelSkin::Variant::kCompact{
    BinaryData::jiv881_panel_compact_png, BinaryData::jiv881_panel_compact_pngSize, 382.0f, 2012.0f, true};
const PanelSkin::Variant PanelSkin::Variant::kCommands{
    BinaryData::jiv881_commands_png, BinaryData::jiv881_commands_pngSize, 1435.0f, 948.0f, false};

// Reference-space (shared 3280x304 space - see PanelSkin.h's own header comment) bounding boxes,
// measured off jiv881.png by connected-component analysis, not eyeballed - same technique/values
// as the original compact-only InterfaceTab skin (cross-checked: those were full-space X minus
// 382, and they matched exactly).
const PanelSkin::PanelButton PanelSkin::kButtons[PanelSkin::kNumButtons] = {
    // Top row, y=104, h=26 (right of the DATA dial).
    {1748.0f, 104.0f, 90.0f, 26.0f, MCU_BUTTON_PATCH_PERFORM},
    {1904.0f, 104.0f, 90.0f, 26.0f, MCU_BUTTON_EDIT},
    {2012.0f, 104.0f, 91.0f, 26.0f, MCU_BUTTON_SYSTEM},
    {2122.0f, 104.0f, 90.0f, 26.0f, MCU_BUTTON_RHYTHM},
    {2231.0f, 104.0f, 91.0f, 26.0f, MCU_BUTTON_UTILITY},
    // Bottom row, y=211, h=26: Cursor </>, Tone Select, then the 4 dual-labelled Tone Switch
    // buttons (Mute/Monitor/Info+Compare/Enter).
    {1485.0f, 211.0f, 90.0f, 26.0f, MCU_BUTTON_CURSOR_L},
    {1592.0f, 211.0f, 89.0f, 26.0f, MCU_BUTTON_CURSOR_R},
    {1748.0f, 211.0f, 90.0f, 26.0f, MCU_BUTTON_TONE_SELECT},
    {1903.0f, 211.0f, 91.0f, 26.0f, MCU_BUTTON_MUTE},
    {2012.0f, 211.0f, 91.0f, 26.0f, MCU_BUTTON_MONITOR},
    {2122.0f, 211.0f, 90.0f, 26.0f, MCU_BUTTON_COMPARE},
    {2231.0f, 211.0f, 91.0f, 26.0f, MCU_BUTTON_ENTER},
};

//==============================================================================
PanelSkin::PanelSkin(VirtualJVProcessor &p, Variant initialVariant, LCDisplay *lcdColorMenuOwnerIn)
    : processor(p), variant(initialVariant), lcdColorMenuOwner(lcdColorMenuOwnerIn)
{
    setVariant(initialVariant);
}

// Flips dataHeld and sends MCU_BUTTON_DATA accordingly - Ctrl-click or right-click on the dial
// (mouseDown below) are the only two ways to trigger this now (Alan's request, 2026-09-08: the
// separate "Hold DATA while rotating" checkbox was redundant with those and removed).
void PanelSkin::toggleDataHeld()
{
    dataHeld = !dataHeld;
    if (processor.loaded && processor.mcu)
        processor.mcu->lcd.LCD_SendButton(MCU_BUTTON_DATA, dataHeld ? 1 : 0);
}

void PanelSkin::setVariant(Variant v)
{
    variant = v;
    panelImage = juce::ImageCache::getFromMemory(variant.imageData, variant.imageDataSize);

    // Runs regardless of variant now (Alan's request, 2026-09-08 added the LED indicators, which
    // need live refreshing in every variant - not just the live-LCD ones) - see timerCallback()'s
    // own comment for why rebuildLcdImage() itself stays conditional. 10Hz, not 25Hz (Alan's
    // request, 2026-09-08, DSP Load investigation - same reasoning as LCDisplay's own timer).
    startTimerHz(10);

    resized();
    repaint();
}

void PanelSkin::resized()
{
    const float w = (float)getWidth();
    imageScale = variant.refW > 0.0f ? w / variant.refW : 1.0f;
    const float h = kMasterRefH * imageScale;
    imageDrawArea = juce::Rectangle<float>(0.0f, 0.0f, w, h);
}

juce::Point<float> PanelSkin::refToComponent(juce::Point<float> ref) const
{
    juce::Point<float> local(ref.x - variant.refOffsetX, ref.y);
    return imageDrawArea.getPosition() + local * imageScale;
}

juce::Point<float> PanelSkin::componentToRef(juce::Point<float> comp) const
{
    auto local = (comp - imageDrawArea.getPosition()) / imageScale;
    return {local.x + variant.refOffsetX, local.y};
}

juce::Rectangle<float> PanelSkin::refRectToComponent(float x, float y, float w, float h) const
{
    auto topLeft = refToComponent({x, y});
    return juce::Rectangle<float>(topLeft.x, topLeft.y, w * imageScale, h * imageScale);
}

int PanelSkin::hitTest(juce::Point<float> componentPos) const
{
    auto ref = componentToRef(componentPos);

    for (int i = 0; i < kNumButtons; i++)
    {
        auto &b = kButtons[i];
        juce::Rectangle<float> padded(b.x - kHitPadRefX, b.y - kHitPadRefY,
                                      b.w + 2.0f * kHitPadRefX, b.h + 2.0f * kHitPadRefY);
        if (padded.contains(ref))
            return i;
    }

    if (ref.getDistanceFrom({kDialCx, kDialCy}) <= kDialR + kHitPadRefR)
        return kHitDataDial;
    if (variant.hasLcdAndVolume && ref.getDistanceFrom({kVolCx, kVolCy}) <= kVolR + kHitPadRefR)
        return kHitVolumeKnob;
    if (variant.hasLcdAndVolume && juce::Rectangle<float>(kLcdX, kLcdY, kLcdW, kLcdH).contains(ref))
        return kHitLcd;

    return kHitNone;
}

// Tone 1-4 mute state (Alan's request, 2026-09-08 - render the TONE SWITCH LEDs the reference
// screenshot showed). Found by headless RE (JV880_SELFTEST_LED, still in PluginProcessor.cpp,
// dormant): pressing MUTE/MONITOR/COMPARE/ENTER (the physical TONE SWITCH 1-4 buttons) from the
// default Patch Play screen each toggles exactly one byte in the emulator's working `sram` -
// 0x35b2/0x3606/0x365a/0x36ae, an even 0x54-byte stride apart (one clean per-tone working-data
// block each) - between 0x80 (the boot/default value) and 0x00. bit 0x80 = "not muted" was this
// project's own inference (no LCD text to cross-check against on the Patch Play screen), since
// confirmed correct by Alan directly - the LED itself is drawn lit for "active" (bit 0x80 set),
// per his correction on the first cut, which had the polarity backwards in the paint() logic
// below (not in this function).
bool PanelSkin::toneMutedLive(int toneIndex) const
{
    if (!processor.loaded || !processor.mcu || toneIndex < 0 || toneIndex > 3)
        return false;
    constexpr uint32_t kToneMuteBase = 0x35b2;
    constexpr uint32_t kToneMuteStride = 0x54;
    return (processor.mcu->sram[kToneMuteBase + (uint32_t)toneIndex * kToneMuteStride] & 0x80) == 0;
}

// See this function's own comment in PanelSkin.h. Only buttonIndex 0 (PATCH/PERFORM) now - EDIT/
// SYSTEM/RHYTHM/UTILITY moved to the app-tracked activeModeButton (its own comment explains why;
// the addresses this used to read for them, sram[0x00b6]/[0x00b2]/[0x00ae]/[0x00bd], are recorded
// in CLAUDE.md if that gets revisited).
bool PanelSkin::modeLedLit(int buttonIndex) const
{
    if (!processor.loaded || !processor.mcu || buttonIndex != 0)
        return false;
    return processor.mcu->nvram[0x11] != 0; // PATCH/PERFORM - lit for Patch mode
}

void PanelSkin::pressButton(int hit, bool down)
{
    if (hit < 0 || hit >= kNumButtons)
        return;

    auto &b = kButtons[hit];

    // PATCH/PERFORM (Alan's request, 2026-09-08 - still desyncing from the Performance tab's own
    // checkbox with the previous one-direction-only fix): goes through the clean
    // setPerformanceModeEnabled() API instead of the raw physical-button forward every other
    // button here uses. That call already sends the same SysEx mode-byte write pressing the real
    // button would trigger (see its own comment in PluginProcessor.cpp), so the firmware ends up
    // in the same state either way - but going through it directly means processor.
    // performanceModeEnabled (and the Performance tab's checkbox) can never drift from what the
    // firmware itself is doing, in *either* direction, instead of only when leaving Performance
    // mode. Only on `down`, same as a real momentary button - the toggle happens once per press,
    // not once on press and again on release.
    if (b.buttonId == MCU_BUTTON_PATCH_PERFORM)
    {
        if (down)
        {
            processor.setPerformanceModeEnabled(!processor.performanceModeEnabled);
            // Returning to Patch/Perform Play always exits any EDIT/SYSTEM/RHYTHM/UTILITY screen
            // on real hardware - keeps the app-tracked LED (see activeModeButton) from going
            // stale across the single transition this UI can actually detect.
            activeModeButton = -1;
        }
        return;
    }

    if (processor.loaded && processor.mcu)
        processor.mcu->lcd.LCD_SendButton(b.buttonId, down ? 1 : 0);
}

void PanelSkin::fireEncoderStep(int direction)
{
    if (processor.loaded && processor.mcu)
        processor.mcu->MCU_EncoderTrigger(direction);
    dialAngleDeg += (direction != 0) ? kDialDegPerStep : -kDialDegPerStep;
    repaint();
}

void PanelSkin::mouseDown(const juce::MouseEvent &e)
{
    int hit = hitTest(e.position);
    pressedButtonIndex = hit;

    ++lcdLongPressToken; // any earlier press's pending long-press is void
    if (hit == kHitLcd && lcdColorMenuOwner != nullptr && !e.mods.isPopupMenu())
    {
        lcdLongPressStart = e.position;
        const int token = lcdLongPressToken;
        juce::Component::SafePointer<PanelSkin> safeThis(this);
        juce::Timer::callAfterDelay(500, [safeThis, token] {
            if (auto *self = safeThis.getComponent())
                if (token == self->lcdLongPressToken && self->lcdColorMenuOwner != nullptr)
                {
                    self->pressedButtonIndex = kHitNone;
                    self->lcdColorMenuOwner->showColorMenu();
                }
        });
    }

    // A real right button (not Mac Ctrl-click, which latches - see below) anywhere except the
    // three controls that use it themselves opens the owner's context menu. pressedButtonIndex
    // is cleared so mouseUp() doesn't release a button that was never pressed.
    const bool ownsRightClick = hit == kHitDataDial || hit == kHitVolumeKnob ||
        (hit >= 0 && hit < kNumButtons && kButtons[hit].buttonId == MCU_BUTTON_TONE_SELECT);
    if (onContextMenu && e.mods.isRightButtonDown() && !e.mods.isCtrlDown() && !ownsRightClick)
    {
        pressedButtonIndex = kHitNone;
        onContextMenu();
        return;
    }

    if (hit == kHitDataDial)
    {
        dialDragStartY = e.position.y;
        dialStepsFired = 0;
        // Ctrl-click or right-click toggles the DATA-held state itself (Alan's request,
        // 2026-09-08 - "comme pour le D110": stays held after release, same as the general
        // per-button latching just below, not just for the duration of one drag).
        // Doesn't need a drag to have happened - a plain click/right-click is enough to toggle.
        if (e.mods.isCtrlDown() || e.mods.isRightButtonDown())
            toggleDataHeld();
    }
    else if (hit == kHitVolumeKnob)
    {
        // Right-click (or Mac ctrl-click - isPopupMenu() covers both) = PREVIEW, matching the
        // real knob's push function; a left-drag adjusts the actual Master Volume instead (see
        // this pair of fields' own comment in the header for why the split).
        volumeRightClickPreview = e.mods.isPopupMenu();
        if (volumeRightClickPreview)
        {
            if (processor.loaded && processor.mcu)
                processor.mcu->lcd.LCD_SendButton(MCU_BUTTON_PREVIEW, 1);
        }
        else
        {
            volumeDragStartY = e.position.y;
            volumeDragStartValue = processor.getMasterVolume();
        }
    }
    else if (hit == kHitLcd)
    {
        // Right-click on the embedded LCD = the same colour picker LCDisplay's own right-click
        // shows (Alan's request, 2026-09-08) - nothing on left-click, matching LCDisplay itself.
        if (e.mods.isPopupMenu() && lcdColorMenuOwner != nullptr)
            lcdColorMenuOwner->showColorMenu();
    }
    else if (hit >= 0 && hit < kNumButtons)
    {
        // TONE SELECT also latches on a plain right-click, not just Ctrl-click (Alan's request,
        // 2026-09-08 - same convenience as DATA's own right-click shortcut): it's the button most
        // likely to actually get held-while-clicking-another in practice (TONE SELECT + a TONE
        // SWITCH to pick which Tone to edit, or its own PARAM SHIFT function + -/+), so a one-
        // handed right-click toggle is worth it there specifically - not generalized to every
        // button, unlike Ctrl-click below.
        const bool wantsLatchToggle = e.mods.isCtrlDown() ||
            (kButtons[hit].buttonId == MCU_BUTTON_TONE_SELECT && e.mods.isRightButtonDown());
        if (wantsLatchToggle)
        {
            // Latching (Ctrl-click on any button, or right-click on TONE SELECT specifically):
            // toggle instead of momentary, fully resolved right here rather than tracked through
            // to mouseUp - pressedButtonIndex is reset to kHitNone so mouseUp doesn't ALSO
            // release it (see this field's own comment in the header).
            buttonLatched[hit] = !buttonLatched[hit];
            pressButton(hit, buttonLatched[hit]);
            pressedButtonIndex = kHitNone;
        }
        else
        {
            pressButton(hit, true);

            // EDIT/SYSTEM/RHYTHM/UTILITY mutually-exclusive toggle (Alan's request, 2026-09-08 -
            // per the Owner's Manual) - see activeModeButton's own comment in the header.
            const auto id = kButtons[hit].buttonId;
            if (id == MCU_BUTTON_EDIT || id == MCU_BUTTON_SYSTEM || id == MCU_BUTTON_RHYTHM ||
                id == MCU_BUTTON_UTILITY)
                activeModeButton = (activeModeButton == hit) ? -1 : hit;
        }
    }
    repaint();
}

void PanelSkin::mouseDrag(const juce::MouseEvent &e)
{
    if (e.position.getDistanceFrom(lcdLongPressStart) > 12.0f)
        ++lcdLongPressToken; // moved: not a long-press any more
    if (pressedButtonIndex == kHitDataDial)
    {
        const float totalUp = dialDragStartY - e.position.y; // positive = dragged upward
        const int stepsWanted = (int)std::floor(std::abs(totalUp) / kDialPxPerStep) *
                                (totalUp >= 0.0f ? 1 : -1);
        while (dialStepsFired < stepsWanted)
        {
            fireEncoderStep(1);
            dialStepsFired++;
        }
        while (dialStepsFired > stepsWanted)
        {
            fireEncoderStep(0);
            dialStepsFired--;
        }
    }
    else if (pressedButtonIndex == kHitVolumeKnob && !volumeRightClickPreview)
    {
        const float dy = volumeDragStartY - e.position.y; // positive = dragged upward
        processor.setMasterVolume(volumeDragStartValue + dy / kVolumeDragRangePx);
        repaint();
    }
}

void PanelSkin::mouseUp(const juce::MouseEvent &)
{
    ++lcdLongPressToken;
    if (pressedButtonIndex == kHitVolumeKnob)
    {
        if (volumeRightClickPreview && processor.loaded && processor.mcu)
            processor.mcu->lcd.LCD_SendButton(MCU_BUTTON_PREVIEW, 0);
        volumeRightClickPreview = false;
    }
    else
    {
        // No-op for kHitDataDial/kHitLcd/kHitNone/an already-fully-handled Ctrl-latch click
        // (pressButton() itself guards the index range) - only actually releases a plain
        // momentary button press.
        pressButton(pressedButtonIndex, false);
    }
    pressedButtonIndex = kHitNone;
    repaint();
}

void PanelSkin::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel)
{
    if (hitTest(e.position) != kHitDataDial)
        return;
    fireEncoderStep(wheel.deltaY > 0.0f ? 1 : 0);
}

void PanelSkin::timerCallback()
{
    // Skip all work while hidden (Alan's report, 2026-09-08: DSP Load pegged at 70%+ at idle,
    // same whether Display mode was LCD only or Panel - root cause was this timer calling
    // rebuildLcdImage() unconditionally even when this exact component was the hidden one, e.g.
    // the main window's own `panelDisplay` sitting invisible behind the plain LCD strip in LCD-
    // only mode, or this same component sitting on a Panel tab that isn't the selected one.
    // repaint() alone is already a no-op on a hidden component - JUCE's own paint pipeline skips
    // it - but rebuildLcdImage() bypasses that entirely by calling LCD_Update() directly instead
    // of from paint(), so it needed its own explicit visibility check.
    if (!isVisible())
        return;

    // Still not enough on its own (Alan's follow-up report, 2026-09-08: DSP Load stayed >60% even
    // for the one legitimately-visible component) - repaint() here was invalidating the WHOLE
    // component, so every tick re-composited the large static background photo (up to 3280x304)
    // just to refresh a handful of small live bits inside it. Narrowed to just those bits' own
    // rects instead - JUCE clips paint()'s actual drawing to the union of invalidated rects, so
    // the background photo's own g.drawImage() call in paint() below becomes a cheap no-op
    // outside them, even though paint() itself still runs in full.
    juce::Rectangle<int> dirty;
    if (variant.hasLcdAndVolume)
    {
        rebuildLcdImage();
        dirty = refRectToComponent(kLcdX, kLcdY, kLcdW, kLcdH).getSmallestIntegerContainer();
    }

    // Only PATCH/PERFORM's LED (firmware-read, kButtons[0]) and the 4 TONE SWITCH LEDs
    // (firmware-read, kButtons[8..11]) can change without a mouse click on this component - the
    // other four mode-button LEDs (kButtons[1..4]) only change from mouseDown, which already
    // triggers its own repaint(), so they don't need to be part of this continuous 25Hz set.
    for (int i : {0, 8, 9, 10, 11})
        dirty = dirty.getUnion(ledRect(i).getSmallestIntegerContainer());

    repaint(dirty);
}

juce::Rectangle<float> PanelSkin::ledRect(int buttonIndex) const
{
    auto &b = kButtons[buttonIndex];
    return refRectToComponent(b.x + (b.w - kLedW) * 0.5f, b.y - kLedH * 0.5f + kLedYOffset, kLedW, kLedH);
}

// Same source as LCDisplay::paint() (processor.mcu->lcd.LCD_Update(), an 1024x1024 offscreen
// buffer of which only the top-left 820x100 is ever live content) - copied into an owned Image
// here instead of drawn directly, since it then needs to be fitted (not 1:1 - see below) into
// the photo's own measured LCD rect.
void PanelSkin::rebuildLcdImage()
{
    if (!processor.loaded || !processor.mcu)
        return;

    auto *bitmapResult = (uint8_t *)processor.mcu->lcd.LCD_Update();
    if (!bitmapResult)
        return;

    // Only the top-left 820x100 of the 1024x1024 buffer is ever live content (see this method's
    // own comment) - the alpha fixup below used to run over the full 1024x1024 regardless (over
    // 1 million writes/tick for nothing - Alan's report, 2026-09-08), scoped down to just the
    // 820x100 actually copied afterwards.
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 820; x++)
            bitmapResult[(y * 1024 + x) * 4 + 3] = 0xff;

    if (!lcdImage.isValid() || lcdImage.getWidth() != 820 || lcdImage.getHeight() != 100)
        lcdImage = juce::Image(juce::Image::PixelFormat::ARGB, 820, 100, false);

    juce::Image::BitmapData pixelMap(lcdImage, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < pixelMap.height; y++)
        memcpy(pixelMap.getLinePointer(y), bitmapResult + (y * 1024 * 4), (size_t)pixelMap.lineStride);
}

void PanelSkin::paint(juce::Graphics &g)
{
    if (panelImage.isValid())
        g.drawImage(panelImage, imageDrawArea, juce::RectanglePlacement::stretchToFit);

    // The live LCD render is 820x100 (8.2:1) but the photo's own LCD opening measures 656x108
    // (6.07:1, see kLcdW/kLcdH) - fitted rather than stretched (RectanglePlacement::centred keeps
    // the emulator's own square-ish dot aspect correct, at the cost of a visible bezel margin on
    // two sides instead of filling the opening exactly). The rect is blacked out first so that
    // margin reads as bezel, not as a mismatched sliver of the photo's own baked-in fake screen.
    if (variant.hasLcdAndVolume && lcdImage.isValid())
    {
        auto lcdRect = refRectToComponent(kLcdX, kLcdY, kLcdW, kLcdH);
        g.setColour(juce::Colours::black);
        g.fillRect(lcdRect);
        g.drawImage(lcdImage, lcdRect, juce::RectanglePlacement::centred);
    }

    // All the small red LEDs (Alan's request, 2026-09-08, working from real-hardware reference
    // photos): PATCH/PERFORM (kButtons[0], via modeLedLit(), firmware-read), EDIT/SYSTEM/RHYTHM/
    // UTILITY (kButtons[1..4], via activeModeButton, app-tracked - see its own comment for why)
    // and each TONE SWITCH (kButtons[8..11], via toneMutedLive()) - same size/position/colour,
    // centred right on the button's own top edge (not floating above it, per Alan's correction).
    {
        auto drawLed = [&](int buttonIndex)
        {
            g.setColour(juce::Colours::red);
            g.fillRoundedRectangle(ledRect(buttonIndex), 1.5f);
        };
        if (processor.loaded && processor.mcu)
        {
            if (modeLedLit(0))
                drawLed(0);
            for (int i = 0; i < 4; i++)
                if (!toneMutedLive(i))
                    drawLed(8 + i);
        }
        if (activeModeButton >= 1 && activeModeButton <= 4)
            drawLed(activeModeButton);
    }

    // Persistent highlight for Ctrl-latched buttons (Alan's request, 2026-09-08 - "comme pour le
    // D110") - shown regardless of pressedButtonIndex, since a latched button stays "pressed"
    // long after the mouse that latched it has moved on.
    for (int i = 0; i < kNumButtons; i++)
    {
        if (!buttonLatched[i])
            continue;
        auto &b = kButtons[i];
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.fillRoundedRectangle(refRectToComponent(b.x, b.y, b.w, b.h), 3.0f);
    }

    if (pressedButtonIndex >= 0 && pressedButtonIndex < kNumButtons && !buttonLatched[pressedButtonIndex])
    {
        auto &b = kButtons[pressedButtonIndex];
        auto rect = refRectToComponent(b.x, b.y, b.w, b.h);
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.fillRoundedRectangle(rect, 3.0f);
    }
    else if (pressedButtonIndex == kHitVolumeKnob)
    {
        auto c = refToComponent({kVolCx, kVolCy});
        float r = kVolR * imageScale;
        g.setColour(juce::Colours::white.withAlpha(0.3f));
        g.drawEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f, 2.5f);
    }

    // DATA hold-state ring (Alan's request, 2026-09-08): shown whenever dataHeld is on, not
    // just while actively dragging - "reste enfoncé" (stays engaged) should read as such at rest
    // too, not just mid-gesture. Falls back to the plain "something's happening" ring while the
    // dial is merely being clicked/dragged without being held.
    {
        auto c = refToComponent({kDialCx, kDialCy});
        float r = kDialR * imageScale;
        if (dataHeld)
        {
            g.setColour(juce::Colours::yellow.withAlpha(0.65f));
            g.drawEllipse(c.x - r - 2.0f, c.y - r - 2.0f, (r + 2.0f) * 2.0f, (r + 2.0f) * 2.0f, 3.5f);
        }
        else if (pressedButtonIndex == kHitDataDial)
        {
            g.setColour(juce::Colours::white.withAlpha(0.3f));
            g.drawEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f, 2.5f);
        }
    }

    // Purely cosmetic rotation indicator - see the header's own comment on dialAngleDeg.
    {
        auto c = refToComponent({kDialCx, kDialCy});
        float r = kDialR * imageScale;
        auto angle = juce::degreesToRadians(dialAngleDeg - 90.0f);
        juce::Point<float> tip(c.x + r * 0.8f * std::cos(angle), c.y + r * 0.8f * std::sin(angle));
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawLine(c.x, c.y, tip.x, tip.y, 2.0f);
    }

    // VOLUME's own pointer, same idea as DATA's but reflecting a real value (Master Volume,
    // 0..1) rather than a cosmetic step count - -135deg (MIN) to +135deg (MAX), 0deg (50%)
    // pointing straight up, the usual rotary-pot sweep. Only where the photo actually has a
    // VOLUME knob to draw it on.
    if (variant.hasLcdAndVolume)
    {
        auto c = refToComponent({kVolCx, kVolCy});
        float r = kVolR * imageScale;
        float vol = juce::jlimit(0.0f, 1.0f, processor.getMasterVolume());
        auto angle = juce::degreesToRadians((-135.0f + vol * 270.0f) - 90.0f);
        juce::Point<float> tip(c.x + r * 0.8f * std::cos(angle), c.y + r * 0.8f * std::sin(angle));
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawLine(c.x, c.y, tip.x, tip.y, 2.0f);
    }
}
