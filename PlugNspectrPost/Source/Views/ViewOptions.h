#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"
#include "../PluginProcessor.h"
#include "../PnsAnalysis.h"

//==============================================================================
// ViewOptions — a live view's settings, folded behind one pill at the view's
// top right ("Smoothing: Medium · Avg: on"). Clicking it opens a panel of
// labelled rows of choices, each with a one-line plain explanation, plus an
// optional "Start over" action. Keeps the graph clear of small buttons while
// the current settings stay readable at a glance.
//==============================================================================
class ViewOptions : public juce::Component
{
public:
    ViewOptions()
    {
        m_pill.onClick = [this] { setOpen (! m_open); };
        addAndMakeVisible (m_pill);
        m_resetBtn.setButtonText ("Start over");
        m_resetBtn.onClick = [this] { if (m_onReset) m_onReset(); setOpen (false); };
        addChildComponent (m_resetBtn);
    }

    // Adds a row; onPick runs with the chosen index. Returns the row index.
    int addRow (const juce::String& title, const juce::String& hint, const juce::StringArray& choices,
                int selected, std::function<void (int)> onPick)
    {
        auto& row = m_rows.emplace_back();
        row.title = title; row.hint = hint; row.choices = choices; row.selected = selected;
        row.onPick = std::move (onPick);
        const int rowIdx = (int) m_rows.size() - 1;
        for (int i = 0; i < choices.size(); ++i)
        {
            auto* b = m_segs.add (new juce::TextButton (choices[i]));
            b->setClickingTogglesState (false);
            b->onClick = [this, rowIdx, i] { pick (rowIdx, i); };
            addChildComponent (b);
            row.buttons.push_back (b);
        }
        refresh();
        return rowIdx;
    }
    void setResetAction (const juce::String& hint, std::function<void()> fn)
    {
        m_resetHint = hint; m_onReset = std::move (fn);
        refresh();
    }

    // Size the component itself: the pill when closed, pill + panel when open.
    // `anchorRight` / `anchorTop` are in the parent's coordinates.
    void place (int anchorRight, int anchorTop)
    {
        m_anchorRight = anchorRight; m_anchorTop = anchorTop;
        const int pw = pillWidth();
        const int w  = m_open ? juce::jmax (pw, kPanelW) : pw;
        int h = kPillH;
        if (m_open)
        {
            h += 8 + kPanelPad;
            for (size_t i = 0; i < m_rows.size(); ++i) h += kRowH;
            if (m_onReset) h += 46;
            h += kPanelPad - 10;
        }
        setBounds (anchorRight - w, anchorTop, w, h);
    }

    void resized() override
    {
        const int w = getWidth();
        m_pill.setBounds (w - pillWidth(), 0, pillWidth(), kPillH);
        int y = kPillH + 8 + kPanelPad;
        for (auto& row : m_rows)
        {
            const int segY = y + 18, n = (int) row.buttons.size();
            const int segW = (w - 2 * kPanelPad) / juce::jmax (1, n);
            for (int i = 0; i < n; ++i)
            {
                row.buttons[(size_t) i]->setBounds (kPanelPad + i * segW, segY, segW, 26);
                row.buttons[(size_t) i]->setVisible (m_open);
            }
            y += kRowH;
        }
        m_resetBtn.setBounds (kPanelPad, y + 4, w - 2 * kPanelPad, 26);
        m_resetBtn.setVisible (m_open && m_onReset != nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        if (! m_open) return;
        const auto panel = panelBounds().toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (panel.translated (0.0f, 4.0f), 12.0f);
        g.setColour (PnsTheme::kBgPanel.withAlpha (0.97f));
        g.fillRoundedRectangle (panel, 12.0f);
        g.setColour (PnsTheme::kBorderActive);
        g.drawRoundedRectangle (panel.reduced (0.5f), 12.0f, 1.0f);

        int y = kPillH + 8 + kPanelPad;
        for (auto& row : m_rows)
        {
            g.setFont (PnsTheme::fontSized (10.5f, true));
            g.setColour (PnsTheme::kTextSecondary);
            g.drawText (row.title.toUpperCase(), kPanelPad, y, getWidth() - 2 * kPanelPad, 14,
                        juce::Justification::centredLeft);
            g.setFont (PnsTheme::fontLabel());
            g.setColour (PnsTheme::kGridLabel);
            g.drawText (row.hint, kPanelPad, y + 48, getWidth() - 2 * kPanelPad, 14,
                        juce::Justification::centredLeft);
            y += kRowH;
        }
        if (m_onReset)
        {
            g.setFont (PnsTheme::fontLabel());
            g.setColour (PnsTheme::kGridLabel);
            g.drawText (m_resetHint, kPanelPad, y + 32, getWidth() - 2 * kPanelPad, 14,
                        juce::Justification::centredLeft);
        }
    }

    ~ViewOptions() override { juce::Desktop::getInstance().removeGlobalMouseListener (this); }

    // Only the pill and the panel itself take clicks — not the strip beside a
    // narrow pill or the gap above the panel, which belong to the graph.
    bool hitTest (int x, int y) override
    {
        return m_pill.getBounds().contains (x, y) || (m_open && panelBounds().contains (x, y));
    }

    // While open this is a global mouse listener: a press anywhere outside the
    // pill and panel closes it, like a menu.
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! m_open || e.eventComponent == this || isParentOf (e.eventComponent)) return;
        setOpen (false);
    }

    void setOpen (bool o)
    {
        if (o == m_open) return;
        m_open = o;
        if (o) juce::Desktop::getInstance().addGlobalMouseListener (this);
        else   juce::Desktop::getInstance().removeGlobalMouseListener (this);
        place (m_anchorRight, m_anchorTop);
        resized();
        repaint();
    }

private:
    struct Row
    {
        juce::String title, hint;
        juce::StringArray choices;
        int selected = 0;
        std::function<void (int)> onPick;
        std::vector<juce::TextButton*> buttons;
    };

    static constexpr int kPillH = 26, kPanelW = 290, kPanelPad = 14, kRowH = 70;

    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withTrimmedTop (kPillH + 8); }

    void pick (int rowIdx, int i)
    {
        auto& row = m_rows[(size_t) rowIdx];
        row.selected = i;
        if (row.onPick) row.onPick (i);
        refresh();
    }
    void refresh()
    {
        juce::StringArray parts;
        for (auto& row : m_rows)
        {
            parts.add (row.title + ": " + row.choices[row.selected]);
            for (size_t i = 0; i < row.buttons.size(); ++i)
                row.buttons[i]->setToggleState ((int) i == row.selected, juce::dontSendNotification);
        }
        m_pill.setButtonText ((parts.isEmpty() ? juce::String ("Options")
                                                 : parts.joinIntoString (juce::String::fromUTF8 ("  \xc2\xb7  "))));
        if (getParentComponent() != nullptr) { place (m_anchorRight, m_anchorTop); resized(); }
    }
    int pillWidth() const
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (PnsTheme::fontLabel(), m_pill.getButtonText(), 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + 34;
    }

    juce::TextButton m_pill, m_resetBtn;
    juce::OwnedArray<juce::TextButton> m_segs;
    std::vector<Row> m_rows;
    juce::String m_resetHint;
    std::function<void()> m_onReset;
    bool m_open = false;
    int  m_anchorRight = 0, m_anchorTop = 0;
};
