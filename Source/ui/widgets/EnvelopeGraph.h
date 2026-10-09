#pragma once

#include <JuceHeader.h>

#include <array>
#include <functional>

// Read-only sketch of a 4-stage JV-880 envelope (T1/L1 .. T3/L3, then a held sustain, then T4 to the end level).
// The segment widths follow a display scale (time parameters are not linear in seconds on the real unit), so the
// shape is faithful but the horizontal axis carries no units.
class EnvelopeGraph : public juce::Component
{
public:
    struct Stage
    {
        int time = 0;  // 0..127
        int level = 0; // bipolar: -63..63, unipolar: 0..127
    };

    EnvelopeGraph(juce::String titleIn, bool bipolarIn, juce::Colour colourIn)
        : title(std::move(titleIn)), bipolar(bipolarIn), colour(colourIn) {}

    // Called by paint() to read the current values (the slider values), so the graph never holds a stale copy.
    std::function<std::array<Stage, 4>()> source;

    void paint(juce::Graphics &g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff1d2327));
        g.fillRoundedRectangle(area, 3.0f);
        g.setColour(juce::Colour(0xff4a5459));
        g.drawRoundedRectangle(area.reduced(0.5f), 3.0f, 1.0f);

        g.setColour(juce::Colours::lightgrey.withAlpha(0.8f));
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(title, getLocalBounds().reduced(6, 2), juce::Justification::topLeft, false);

        if (!source)
            return;
        const auto st = source();

        const auto plot = area.reduced(8.0f, 5.0f).withTrimmedTop(11.0f);
        const float maxLevel = bipolar ? 63.0f : 127.0f;
        auto yOf = [&](int level)
        {
            const float v = juce::jlimit(-1.0f, 1.0f, level / maxLevel);
            return bipolar ? plot.getCentreY() - v * plot.getHeight() * 0.5f
                           : plot.getBottom() - juce::jmax(0.0f, v) * plot.getHeight();
        };
        auto widthOf = [](int t) { const float x = t / 127.0f; return 2.0f + 30.0f * x * x; };

        const float sustainW = 12.0f;
        const float total = widthOf(st[0].time) + widthOf(st[1].time) + widthOf(st[2].time) + sustainW + widthOf(st[3].time);
        const float k = plot.getWidth() / total;

        g.setColour(juce::Colours::white.withAlpha(0.15f));
        g.drawHorizontalLine((int)yOf(0), plot.getX(), plot.getRight()); // baseline (zero level)

        const bool ampLike = !bipolar && endsAtZero;
        const int endLevel = ampLike ? 0 : st[3].level;

        juce::Path p;
        float x = plot.getX();
        p.startNewSubPath(x, yOf(0));
        for (int i = 0; i < 3; ++i)
        {
            x += widthOf(st[i].time) * k;
            p.lineTo(x, yOf(st[i].level));
        }
        const float sustainEnd = x + sustainW * k;
        p.lineTo(sustainEnd, yOf(st[2].level));
        p.lineTo(sustainEnd + widthOf(st[3].time) * k, yOf(endLevel));

        juce::Path fill(p);
        fill.lineTo(plot.getRight(), yOf(endLevel));
        fill.lineTo(plot.getRight(), yOf(0));
        fill.closeSubPath();
        g.setColour(colour.withAlpha(0.15f));
        g.fillPath(fill);
        g.setColour(colour);
        g.strokePath(p, juce::PathStrokeType(1.6f));

        // release marker: where the key is let go
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        const float dash[] = { 3.0f, 3.0f };
        g.drawDashedLine(juce::Line<float>(sustainEnd, plot.getY(), sustainEnd, plot.getBottom()), dash, 2);
    }

    // Amp has no L4: the release always ends at silence.
    bool endsAtZero = false;

private:
    juce::String title;
    bool bipolar;
    juce::Colour colour;
};
