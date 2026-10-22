#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Theme.h"

// Vertical drag reported to the owner, in pixels from the mouse-down position (screen space, so
// it stays correct while the window itself is being resized under the cursor).
struct VerticalDragCallbacks
{
    std::function<void()> onDragStart;
    std::function<void(int dy)> onDrag;
    std::function<void()> onDragEnd;
};

// A thin, full-width bar with two roles, like the D-110 editor's bands: a click collapses/
// expands the drawer below it (currently VirtualKeyboard or the sequencer), a drag (more than a
// few pixels) resizes the pane directly above it instead - the owner decides what that means.
class CollapseHandle final : public juce::Component, public VerticalDragCallbacks
{
public:
    void paint(juce::Graphics &g) override
    {
        auto bounds = getLocalBounds();
        g.setColour(jivtheme::handleBg());
        g.fillRect(bounds);
        g.setColour(jivtheme::line(0.15f));
        g.drawRect(bounds, 1);

        g.setColour(jivtheme::handleText());
        g.setFont(juce::FontOptions(12.0f));
        // U+25BC/25B2 (filled down/up triangle) rather than an image asset - a single glyph
        // is enough to show which way clicking will go.
        const juce::String arrow = expanded ? juce::String(juce::CharPointer_UTF8("\xe2\x96\xbc"))
                                             : juce::String(juce::CharPointer_UTF8("\xe2\x96\xb2"));
        g.drawText(arrow + " " + label, bounds, juce::Justification::centred);
    }

    void mouseEnter(const juce::MouseEvent &) override
    {
        setMouseCursor(onDrag ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown(const juce::MouseEvent &e) override
    {
        dragging = false;
        if (onDragStart && !e.mods.isPopupMenu()) onDragStart();
    }

    void mouseDrag(const juce::MouseEvent &e) override
    {
        if (!onDrag || e.mods.isPopupMenu()) return;
        const int dy = e.getScreenPosition().y - e.getMouseDownScreenPosition().y;
        if (!dragging)
        {
            if (std::abs(dy) < 4) return;
            dragging = true;
        }
        onDrag(dy);
    }

    void mouseUp(const juce::MouseEvent &e) override
    {
        if (dragging)
        {
            dragging = false;
            if (onDragEnd) onDragEnd();
            return;
        }
        if (e.mouseWasDraggedSinceMouseDown()) return;
        // Right-click opens the app-wide context menu (see VirtualJVEditor::showAppContextMenu())
        // instead of collapsing the drawer - only when the owner wired it up.
        if (e.mods.isPopupMenu() && onRightClick) { onRightClick(); return; }
        if (onClick) onClick();
    }

    void setLabel(const juce::String &l) { label = l; repaint(); }
    void setExpanded(bool e) { expanded = e; repaint(); }

    std::function<void()> onClick;
    std::function<void()> onRightClick;

private:
    juce::String label { "Keyboard" };
    bool expanded = true;
    bool dragging = false;
};

// A short grip under the last drawer: drag-only (nothing to fold below it), resizes that drawer.
class ResizeGrip final : public juce::Component, public VerticalDragCallbacks
{
public:
    ResizeGrip() { setMouseCursor(juce::MouseCursor::UpDownResizeCursor); }

    void paint(juce::Graphics &g) override
    {
        auto bounds = getLocalBounds();
        g.setColour(jivtheme::handleBg());
        g.fillRect(bounds);
        g.setColour(jivtheme::handleText().withAlpha(0.7f));
        const auto c = bounds.getCentre();
        for (int i = -1; i <= 1; ++i)
            g.fillRect(c.x + i * 14 - 4, c.y - 1, 8, 2);
    }

    void mouseDown(const juce::MouseEvent &) override
    {
        if (onDragStart) onDragStart();
    }

    void mouseDrag(const juce::MouseEvent &e) override
    {
        if (onDrag) onDrag(e.getScreenPosition().y - e.getMouseDownScreenPosition().y);
    }

    void mouseUp(const juce::MouseEvent &) override
    {
        if (onDragEnd) onDragEnd();
    }
};
