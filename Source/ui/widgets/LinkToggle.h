#pragma once

#include <JuceHeader.h>

// Toggle drawn as a padlock (closed = on) followed by a short caption.
class LinkToggle final : public juce::ToggleButton
{
public:
    explicit LinkToggle(const juce::String &caption) : juce::ToggleButton(caption) {}

    void paintButton(juce::Graphics &g, bool highlighted, bool /*down*/) override
    {
        const bool on = getToggleState();
        const auto col = on ? juce::Colour(0xffffb347) : juce::Colours::lightgrey.withAlpha(highlighted ? 0.95f : 0.65f);
        const auto b = getLocalBounds().toFloat();
        const float s = juce::jmin(b.getHeight() - 6.0f, 18.0f);
        const float x = b.getX() + 3.0f, y = b.getCentreY() - s * 0.5f;

        // body
        const juce::Rectangle<float> body(x, y + s * 0.42f, s * 0.8f, s * 0.58f);
        g.setColour(col);
        g.fillRoundedRectangle(body, 2.0f);
        // shackle: closed when on, lifted and open on the right when off
        juce::Path sh;
        const float r = s * 0.2f, cx = body.getCentreX();
        if (on)
        {
            sh.startNewSubPath(cx - r, body.getY());
            sh.lineTo(cx - r, body.getY() - r);
            sh.addArc(cx - r, body.getY() - 2 * r, 2 * r, 2 * r, -juce::MathConstants<float>::pi, 0.0f, false);
            sh.lineTo(cx + r, body.getY());
        }
        else
        {
            sh.startNewSubPath(cx - r, body.getY());
            sh.lineTo(cx - r, body.getY() - r - 2.0f);
            sh.addArc(cx - r, body.getY() - 2 * r - 2.0f, 2 * r, 2 * r, -juce::MathConstants<float>::pi, 0.0f, false);
            sh.lineTo(cx + r, body.getY() - 4.0f);
        }
        g.strokePath(sh, juce::PathStrokeType(1.8f));
        g.setColour(juce::Colour(0xff1d2327));
        g.fillEllipse(body.getCentreX() - 1.4f, body.getCentreY() - 2.0f, 2.8f, 2.8f);

        g.setColour(col);
        g.setFont(juce::FontOptions(14.0f));
        g.drawText(getButtonText(), getLocalBounds().withTrimmedLeft((int)(s * 0.8f + 10.0f)), juce::Justification::centredLeft, false);
    }
};
