#include "ThdSweepView.h"
#include "ViewCommon.h"

#include <algorithm>
#include <cmath>
#include <set>

static juce::Rectangle<float> thdPlot (juce::Component& c)
{
    auto a = c.getLocalBounds().toFloat().reduced (8.0f);
    a.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);
    return { a.getX() + 46.0f, a.getY() + 16.0f, a.getWidth() - 58.0f, a.getHeight() - 36.0f };
}

//==============================================================================
// ThdSweepView — total harmonic distortion vs frequency (log-log)
//==============================================================================
ThdSweepView::ThdSweepView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    m_measureBtn.setClickingTogglesState (true);
    m_measureBtn.getProperties().set ("armed", true);
    m_measureBtn.onClick = [this]
    {
        m_measureActive = m_measureBtn.getToggleState();
        if (m_measureActive) m_proc.startThdSweepCycle();   // new sweep; old curve stays as a ghost
        if (onMeasureChanged) onMeasureChanged();
        repaint();
    };
    addAndMakeVisible (m_measureBtn);

    m_freezeBtn.onClick = [this]
    {
        if (m_hasFrozen) m_hasFrozen = false; else doFreeze();
        m_freezeBtn.setToggleState (m_hasFrozen, juce::dontSendNotification);
        m_freezeBtn.setButtonText (m_hasFrozen ? "Frozen" : "Freeze");
        repaint();
    };
    addAndMakeVisible (m_freezeBtn);
}

void ThdSweepView::doFreeze()
{
    m_frozenThd   = m_thd.thdPct;
    m_frozenValid = m_thd.valid;
    m_hasFrozen   = true;
}

void ThdSweepView::freezeForTest() { doFreeze(); repaint(); }

void ThdSweepView::resized()
{
    m_measureBtn.setBounds (getWidth() - 90, PnsTheme::kPaddingSmall,
                            78, PnsTheme::kButtonHeight);
    m_freezeBtn .setBounds (getWidth() - 90 - 84, PnsTheme::kPaddingSmall,
                            78, PnsTheme::kButtonHeight);
}

void ThdSweepView::update()
{
    m_proc.getThdSweep (m_thd);
}

void ThdSweepView::mouseMove (const juce::MouseEvent& e)
{
    if (m_cursor.move (e.position, thdPlot (*this).contains (e.position))) repaint();
}

void ThdSweepView::mouseExit (const juce::MouseEvent&)
{
    if (m_cursor.exit()) repaint();
}

void ThdSweepView::mouseDown (const juce::MouseEvent& e)
{
    if (m_cursor.down (e.position, thdPlot (*this).contains (e.position))) repaint();
}

void ThdSweepView::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);

    auto area = getLocalBounds().toFloat().reduced (8.0f);
    area.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);

    constexpr float kML = 54.0f, kMR = 12.0f, kMT = 16.0f, kMB = 20.0f;
    const float px = area.getX() + kML, py = area.getY() + kMT;
    const float pw = area.getWidth() - kML - kMR, ph = area.getHeight() - kMT - kMB;
    if (pw <= 0 || ph <= 0) return;

    const float lo = PlugNspectrPostProcessor::kThdLoHz, hi = PlugNspectrPostProcessor::kThdHiHz;
    auto X = [&] (double f) { return px + pw * (float) (std::log (juce::jlimit ((double) lo, (double) hi, f) / lo)
                                                        / std::log ((double) hi / lo)); };
    // Log-% Y axis, three decades, topped one decade above the highest reading
    // (live or frozen) so the curve uses the height: 0.01..10% for a typical
    // saturator, 0.001..1% for a clean EQ. Never above 100%.
    float peak = 0.0f;
    for (int b = 0; b < kBins; ++b)
    {
        if (m_thd.valid[(size_t) b])                   peak = juce::jmax (peak, m_thd.thdPct[(size_t) b]);
        if (m_hasFrozen && m_frozenValid[(size_t) b]) peak = juce::jmax (peak, m_frozenThd[(size_t) b]);
    }
    const float yHi = juce::jlimit (0.0f, 2.0f, std::ceil (std::log10 (juce::jmax (peak, 1.0e-3f) * 1.5f)));
    const float yLo = yHi - 3.0f;
    auto Y = [&] (float pct) { const float lv = juce::jlimit (yLo, yHi, std::log10 (juce::jmax (pct, 1.0e-4f)));
                               return py + ph * (yHi - lv) / (yHi - yLo); };

    PnsTheme::drawPanelTitle (g, area.withHeight (13.0f), "DISTORTION BY FREQUENCY", "THD, %");

    PnsTheme::drawPlotCard (g, { px, py, pw, ph });

    for (int e = (int) yLo; e <= (int) yHi; ++e)
    {
        const float d  = std::pow (10.0f, (float) e);
        const float gy = Y (d);
        g.setColour (PnsTheme::kGridLine);
        g.drawHorizontalLine (juce::roundToInt (gy), px, px + pw);
        g.setColour (PnsTheme::kGridLabel);
        const juce::String lbl = (d >= 1.0f) ? juce::String ((int) d) + "%"
                                             : juce::String (d, (d >= 0.1f ? 1 : (d >= 0.01f ? 2 : 3))) + "%";
        g.drawText (lbl, (int) area.getX(), juce::roundToInt (gy) - 6, (int) kML - 4, 12,
                    juce::Justification::centredRight);
    }
    const double vfreqs[] = { 50, 100, 200, 500, 1000, 2000, 5000 };
    for (double f : vfreqs)
    {
        const float gx = X (f);
        g.setColour (PnsTheme::kGridLine);
        g.drawVerticalLine (juce::roundToInt (gx), py, py + ph);
        g.setColour (PnsTheme::kGridLabel);
        const juce::String lbl = (f >= 1000.0) ? juce::String (f / 1000.0, 0) + "k" : juce::String ((int) f);
        g.drawText (lbl, juce::roundToInt (gx) - 14, juce::roundToInt (py + ph) + 2, 28, 11,
                    juce::Justification::centred);
    }

    auto buildPath = [&] (const std::array<float, kBins>& thd,
                          const std::array<bool, kBins>& mask, int& count) -> juce::Path
    {
        juce::Path path;
        bool started = false;
        count = 0;
        for (int b = 0; b < kBins; ++b)
        {
            if (! mask[(size_t) b]) continue;
            const double f = lo * std::pow ((double) hi / lo, (double) b / (kBins - 1));
            const float x = X (f), y = Y (thd[(size_t) b]);
            if (! started) { path.startNewSubPath (x, y); started = true; }
            else             path.lineTo (x, y);
            ++count;
        }
        return path;
    };

    // Ghost: the previous sweep (has a value but not yet refreshed this cycle),
    // drawn dim. The bright sweep overwrites it bin-by-bin, low→high.
    std::array<bool, kBins> ghostMask {}, freshMask {};
    int nValid = 0;
    for (int b = 0; b < kBins; ++b)
    {
        if (m_thd.valid[(size_t) b]) ++nValid;
        ghostMask[(size_t) b] = m_thd.valid[(size_t) b] && ! m_thd.fresh[(size_t) b];
        freshMask[(size_t) b] = m_thd.fresh[(size_t) b];
    }

    // Clip the curves to the plot so floored (sub-0.001%) bins ride the bottom
    // line instead of bleeding their glow below the grid.
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (juce::Rectangle<int> ((int) px, (int) py,
                                                  (int) pw, (int) ph));

        // Frozen reference (dimmed grey) under everything.
        if (m_hasFrozen)
        {
            int fc = 0;
            const juce::Path fp = buildPath (m_frozenThd, m_frozenValid, fc);
            if (fc > 1) { g.setColour (PnsTheme::kTextSecondary.withAlpha (0.5f));
                          g.strokePath (fp, juce::PathStrokeType (1.0f)); }
        }

        int gc = 0;
        const juce::Path ghost = buildPath (m_thd.thdPct, ghostMask, gc);
        if (gc > 1) { g.setColour (PnsTheme::kColorPost.withAlpha (0.25f));
                      g.strokePath (ghost, juce::PathStrokeType (1.0f)); }

        int nFresh = 0;
        const juce::Path path = buildPath (m_thd.thdPct, freshMask, nFresh);
        if (nFresh > 1)
            PnsTheme::drawGlowLine (g, path, PnsTheme::kColorPost, 1.5f);
    }

    // Cursor readout — frequency → THD %.
    if (nValid > 1 && m_cursor.x >= 0.0f)
    {
        const float cx = juce::jlimit (px, px + pw, m_cursor.x);
        const double t = (double) (cx - px) / pw;
        const double f = (double) lo * std::pow ((double) hi / lo, t);
        const int b = juce::jlimit (0, kBins - 1, (int) std::lround (t * (kBins - 1)));
        if (m_thd.valid[(size_t) b])
        {
            const float pct = m_thd.thdPct[(size_t) b];
            const juce::String fStr = (f >= 1000.0) ? juce::String (f / 1000.0, 2) + " kHz"
                                                    : juce::String ((int) f) + " Hz";
            juce::StringArray rows;
            rows.add (fStr);
            rows.add ("THD " + juce::String (pct, pct < 1.0f ? 2 : 1) + " %");
            drawCursorChip (g, { px, py, pw, ph }, cx, Y (pct), PnsTheme::kColorPost, rows, m_cursor.locked);
        }
    }

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);

    if (nValid < 2)
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Press Measure (top right) - Pre sweeps a tone low to high to map distortion at every frequency",
                    getLocalBounds().reduced (20), juce::Justification::centred);
    }
}

PnsTheme::Summary ThdSweepView::summary() const
{
    using P = PlugNspectrPostProcessor;
    double sum = 0.0; int n = 0; float hi = 0.0f; int hiB = 0;
    for (int b = 0; b < kBins; ++b)
    {
        if (! m_thd.valid[(size_t) b]) continue;
        sum += m_thd.thdPct[(size_t) b]; ++n;
        if (m_thd.thdPct[(size_t) b] > hi) { hi = m_thd.thdPct[(size_t) b]; hiB = b; }
    }
    if (n < 2)
        return { "Press Measure  -  Pre sweeps a tone from low to high to map distortion at every frequency." };
    if (hi < 0.01f)
        return { "Clean at every frequency  -  under 0.01% distortion", false, "Worst",
                 PnsTheme::fmtPct (hi).dropLastCharacters (1), "%" };

    const float  avg = (float) (sum / n);
    const double f   = P::kThdLoHz * std::pow ((double) P::kThdHiHz / P::kThdLoHz, (double) hiB / (kBins - 1));
    return { "Most distortion near " + PnsTheme::fmtHz (f) + "  -  averages " + PnsTheme::fmtPct (avg)
             + " across the sweep", false, "Worst", PnsTheme::fmtPct (hi).dropLastCharacters (1), "%" };
}
