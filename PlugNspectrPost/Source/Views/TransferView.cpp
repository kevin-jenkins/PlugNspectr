#include "TransferView.h"
#include "ViewCommon.h"

#include <algorithm>
#include <cmath>
#include <set>

static juce::Rectangle<float> transferPlot (juce::Component& c)
{
    auto a = c.getLocalBounds().toFloat().reduced (8.0f);
    a.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);
    const float side = juce::jmin (a.getWidth(), a.getHeight());
    const juce::Rectangle<float> r (a.getX() + (a.getWidth() - side) * 0.5f, a.getY(), side, side);
    return { r.getX() + 40.0f, r.getY() + 16.0f, r.getWidth() - 52.0f, r.getHeight() - 36.0f };
}

//==============================================================================
// TransferView — measured dynamics transfer curve (output vs input level)
//==============================================================================
TransferView::TransferView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    m_measureBtn.setClickingTogglesState (true);
    m_measureBtn.getProperties().set ("armed", true);
    m_measureBtn.onClick = [this]
    {
        m_measureActive = m_measureBtn.getToggleState();
        if (m_measureActive) m_proc.resetDynamics();   // start fresh; pausing keeps the curve
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

void TransferView::doFreeze()
{
    m_frozenOut   = m_dyn.outDb;
    m_frozenValid = m_dyn.valid;
    m_hasFrozen   = true;
}

void TransferView::freezeForTest() { doFreeze(); repaint(); }

void TransferView::mouseMove (const juce::MouseEvent& e)
{
    if (m_cursorLocked) return;
    m_cursorX = transferPlot (*this).contains (e.position) ? e.position.x : -1.0f;
    repaint();
}
void TransferView::mouseExit (const juce::MouseEvent&)
{
    if (! m_cursorLocked) { m_cursorX = -1.0f; repaint(); }
}
void TransferView::mouseDown (const juce::MouseEvent& e)
{
    if (transferPlot (*this).contains (e.position))
    { m_cursorX = e.position.x; m_cursorLocked = ! m_cursorLocked; repaint(); }
}

void TransferView::resized()
{
    m_measureBtn.setBounds (getWidth() - 90, PnsTheme::kPaddingSmall,
                            78, PnsTheme::kButtonHeight);
    m_freezeBtn .setBounds (getWidth() - 90 - 84, PnsTheme::kPaddingSmall,
                            78, PnsTheme::kButtonHeight);
}

void TransferView::update()
{
    m_proc.getDynamics (m_dyn);
}

void TransferView::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);

    auto area = getLocalBounds().toFloat().reduced (8.0f);
    area.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);

    // Square plot, centred.
    const float side = juce::jmin (area.getWidth(), area.getHeight());
    const juce::Rectangle<float> r (area.getX() + (area.getWidth() - side) * 0.5f,
                                    area.getY(), side, side);

    constexpr float kML = 40.0f, kMR = 12.0f, kMT = 16.0f, kMB = 20.0f;
    const float px = r.getX() + kML, py = r.getY() + kMT;
    const float pw = r.getWidth() - kML - kMR, ph = r.getHeight() - kMT - kMB;
    if (pw <= 0 || ph <= 0) return;

    constexpr float lo = -60.0f, hi = 0.0f;
    auto X = [&] (float inDb)  { return px + pw * (inDb  - lo) / (hi - lo); };
    auto Y = [&] (float outDb) { return py + ph * (hi - outDb) / (hi - lo); };

    PnsTheme::drawPanelTitle (g, r.withHeight (13.0f), "OUTPUT LEVEL vs INPUT LEVEL", "transfer curve, dB");

    PnsTheme::drawPlotCard (g, { px, py, pw, ph });

    for (int v = -60; v <= 0; v += 10)
    {
        const float gx = X ((float) v), gy = Y ((float) v);
        g.setColour (PnsTheme::kGridLine);
        g.drawVerticalLine   (juce::roundToInt (gx), py, py + ph);
        g.drawHorizontalLine (juce::roundToInt (gy), px, px + pw);
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (juce::String (v), (int) r.getX(), juce::roundToInt (gy) - 6,
                    (int) kML - 4, 12, juce::Justification::centredRight);
        g.drawText (juce::String (v), juce::roundToInt (gx) - 14, juce::roundToInt (py + ph) + 2,
                    28, 12, juce::Justification::centred);
    }

    // Unity (no-change) reference diagonal.
    g.setColour (PnsTheme::kZeroLine);
    g.drawLine (X (lo), Y (lo), X (hi), Y (hi), 1.0f);

    auto buildPath = [&] (const std::array<float, kBins>& out,
                          const std::array<bool, kBins>& valid, int& count) -> juce::Path
    {
        juce::Path path;
        bool started = false;
        count = 0;
        for (int b = 0; b < kBins; ++b)
        {
            if (! valid[(size_t) b]) continue;
            const float inDb  = PlugNspectrPostProcessor::kDynMinDb
                              + (float) b * PlugNspectrPostProcessor::kDynBinW;
            const float x = X (inDb), y = Y (juce::jlimit (lo, hi, out[(size_t) b]));
            if (! started) { path.startNewSubPath (x, y); started = true; }
            else             path.lineTo (x, y);
            ++count;
        }
        return path;
    };

    // Frozen reference (dimmed grey) under the live curve.
    if (m_hasFrozen)
    {
        int fc = 0;
        const juce::Path fp = buildPath (m_frozenOut, m_frozenValid, fc);
        if (fc > 1) { g.setColour (PnsTheme::kTextSecondary.withAlpha (0.5f));
                      g.strokePath (fp, juce::PathStrokeType (1.0f)); }
    }

    int nValid = 0;
    const juce::Path path = buildPath (m_dyn.outDb, m_dyn.valid, nValid);
    if (nValid > 1)
        PnsTheme::drawGlowLine (g, path, PnsTheme::kColorPost, 1.5f);

    // Cursor readout — input level → output level / gain reduction.
    if (nValid > 1 && m_cursorX >= 0.0f)
    {
        const float cx = juce::jlimit (px, px + pw, m_cursorX);
        const float inDb = lo + (cx - px) / pw * (hi - lo);
        const int b = juce::jlimit (0, kBins - 1,
                                    (int) std::lround ((inDb - PlugNspectrPostProcessor::kDynMinDb)
                                                       / PlugNspectrPostProcessor::kDynBinW));
        if (m_dyn.valid[(size_t) b])
        {
            const float outDb = juce::jlimit (lo, hi, m_dyn.outDb[(size_t) b]);
            juce::StringArray rows;
            rows.add (juce::String (inDb, 1) + " dB in");
            rows.add (juce::String (outDb, 1) + " dB out");
            rows.add ("GR " + juce::String (inDb - outDb, 1) + " dB");
            drawCursorChip (g, { px, py, pw, ph }, cx, Y (outDb), PnsTheme::kColorPost, rows, m_cursorLocked);
        }
    }

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);

    if (nValid < 2)
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Press Measure (top right) - Pre ramps a tone up in level to map how hard your plugin compresses",
                    getLocalBounds().reduced (20), juce::Justification::centred);
    }
}

PnsTheme::Summary TransferView::summary() const
{
    using P = PlugNspectrPostProcessor;
    int first = -1, last = -1;
    for (int b = 0; b < kBins; ++b)
        if (m_dyn.valid[(size_t) b]) { if (first < 0) first = b; last = b; }
    if (first < 0 || last - first < 2)
        return { "Press Measure  -  Pre ramps a tone up in level to map how hard your plugin compresses." };

    auto inDb = [] (int b) { return P::kDynMinDb + (float) b * P::kDynBinW; };
    // Gain reduction relative to the quietest point, so make-up gain doesn't read as compression.
    const float offset = inDb (first) - m_dyn.outDb[(size_t) first];
    auto grAt = [&] (int b) { return (inDb (b) - m_dyn.outDb[(size_t) b]) - offset; };

    int knee = -1;
    for (int b = first; b <= last; ++b)
        if (m_dyn.valid[(size_t) b] && grAt (b) > 1.0f) { knee = juce::jmax (first, b - 1); break; }
    if (knee < 0)
        return { "No compression across the measured range  -  " + juce::String (juce::roundToInt (inDb (first)))
                 + " to " + juce::String (juce::roundToInt (inDb (last))) + " dB", false, "Ratio", "1.0", ":1" };

    const float dIn  = inDb (last) - inDb (knee);
    const float dOut = m_dyn.outDb[(size_t) last] - m_dyn.outDb[(size_t) knee];
    const juce::String ratio = dOut <= 0.1f ? juce::String ("limiting (output stops rising)")
                                            : "ratio about " + juce::String (dIn / dOut, 1) + ":1";
    return { "Starts compressing near " + juce::String (juce::roundToInt (inDb (knee))) + " dB  -  " + ratio,
             false, "Ratio", dOut <= 0.1f ? juce::String ("Limit") : juce::String (dIn / dOut, 1),
             dOut <= 0.1f ? "" : ":1" };
}
