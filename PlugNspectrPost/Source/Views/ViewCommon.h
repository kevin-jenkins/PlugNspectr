#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"

//==============================================================================
// One drain of the processor's capture ring, taken by the editor once per UI
// tick and handed to every reader of raw audio (LagTracker, Dynamics,
// Waveform, Stereo) instead of each draining the ring itself.
struct CaptureChunk
{
    juce::AudioBuffer<float> pre, post;   // kCaptureChannels x n
    int      n       = 0;                 // samples in pre / post
    uint64_t elapsed = 0;                 // audio since the last drain: n plus any lost to a stall
};

//==============================================================================
// The hover cursor of the measurement views: it follows the mouse over the
// view's plot, a click there locks or unlocks it, and leaving the view hides
// it unless locked. Each call returns true if the view should repaint.
struct HoverCursor
{
    float x      = -1.0f;   // hairline x in view coords (-1 = hidden)
    bool  locked = false;

    bool move (juce::Point<float> p, bool inPlot)
    {
        if (locked) return false;
        x = inPlot ? p.x : -1.0f;
        return true;
    }
    bool exit()
    {
        if (locked) return false;
        x = -1.0f;
        return true;
    }
    bool down (juce::Point<float> p, bool inPlot)
    {
        if (! inPlot) return false;
        x = p.x;
        locked = ! locked;
        return true;
    }
    void lockAt (int px) { x = (float) px; locked = true; }
};

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
