#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"

//==============================================================================
// Shared single-plot cursor: hairline + dot + readout chip.
inline void drawCursorChip (juce::Graphics& g, juce::Rectangle<float> plot, float cx,
                            float dotY, juce::Colour dotCol,
                            const juce::StringArray& rows, bool locked)
{
    cx = juce::jlimit (plot.getX(), plot.getRight(), cx);
    g.setColour (PnsTheme::kAccentPrimary.withAlpha (0.45f));
    g.drawVerticalLine (juce::roundToInt (cx), plot.getY(), plot.getBottom());
    g.setColour (dotCol);
    g.fillEllipse (cx - 3.0f, dotY - 3.0f, 6.0f, 6.0f);

    const float boxW = 104.0f, boxH = 8.0f + (float) rows.size() * 13.0f;
    float bx = cx + 10.0f;
    if (bx + boxW > plot.getRight()) bx = cx - 10.0f - boxW;
    bx = juce::jmax (plot.getX(), bx);
    const float by = plot.getY() + 6.0f;
    g.setColour (PnsTheme::kBgDark.withAlpha (0.92f));
    g.fillRoundedRectangle (bx, by, boxW, boxH, 6.0f);
    g.setColour (PnsTheme::kAccentPrimary.withAlpha (0.7f));
    g.drawRect (bx, by, boxW, boxH, 1.0f);
    g.setFont (PnsTheme::fontLabel());
    for (int i = 0; i < rows.size(); ++i)
    {
        g.setColour (i == 0 ? PnsTheme::kTextPrimary : PnsTheme::kTextSecondary);
        g.drawText (rows[i], juce::roundToInt (bx) + 6, juce::roundToInt (by) + 4 + i * 13,
                    (int) boxW - 10, 12, juce::Justification::centredLeft);
    }
    if (locked) { g.setColour (PnsTheme::kAccentPrimary); g.fillEllipse (bx + boxW - 12.0f, by + 4.0f, 5.0f, 5.0f); }
}
