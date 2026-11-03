#include "EnvelopeView.h"
#include "ViewCommon.h"

#include <algorithm>
#include <cmath>
#include <set>

static juce::Rectangle<float> envPlot (juce::Component& c)
{
    auto a = c.getLocalBounds().toFloat().reduced (8.0f);
    a.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);
    return { a.getX() + 42.0f, a.getY() + 16.0f, a.getWidth() - 54.0f, a.getHeight() - 36.0f };
}

//==============================================================================
// EnvelopeView — measured attack/release (gain reduction vs time)
//==============================================================================
EnvelopeView::EnvelopeView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    m_measureBtn.setClickingTogglesState (true);
    m_measureBtn.getProperties().set ("armed", true);
    m_measureBtn.onClick = [this]
    {
        m_measureActive = m_measureBtn.getToggleState();
        if (m_measureActive) m_proc.resetEnvelope();   // start fresh; pausing keeps the curve
        if (onMeasureChanged) onMeasureChanged();
        repaint();
    };
    addAndMakeVisible (m_measureBtn);
}

void EnvelopeView::resized()
{
    m_measureBtn.setBounds (getWidth() - 90, PnsTheme::kPaddingSmall,
                            78, PnsTheme::kButtonHeight);
}

void EnvelopeView::update()
{
    m_proc.getEnvelope (m_env);
    float hi = 0.0f;
    for (int b = 0; b < kBins; ++b)
        if (m_env.valid[(size_t) b]) hi = juce::jmax (hi, m_env.grDb[(size_t) b]);
    m_grHi = juce::jmax (6.0f, hi * 1.2f);
}

void EnvelopeView::mouseMove (const juce::MouseEvent& e)
{
    if (m_cursorLocked) return;
    m_cursorX = envPlot (*this).contains (e.position) ? e.position.x : -1.0f;
    repaint();
}
void EnvelopeView::mouseExit (const juce::MouseEvent&)
{
    if (! m_cursorLocked) { m_cursorX = -1.0f; repaint(); }
}
void EnvelopeView::mouseDown (const juce::MouseEvent& e)
{
    if (envPlot (*this).contains (e.position))
    { m_cursorX = e.position.x; m_cursorLocked = ! m_cursorLocked; repaint(); }
}

void EnvelopeView::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);

    auto area = getLocalBounds().toFloat().reduced (8.0f);
    area.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);

    constexpr float kML = 42.0f, kMR = 12.0f, kMT = 16.0f, kMB = 20.0f;
    const float px = area.getX() + kML, py = area.getY() + kMT;
    const float pw = area.getWidth() - kML - kMR, ph = area.getHeight() - kMT - kMB;
    if (pw <= 0 || ph <= 0) return;

    auto X = [&] (float ms) { return px + pw * ms / 1000.0f; };
    auto Y = [&] (float gr) { return py + ph * (m_grHi - gr) / m_grHi; };

    PnsTheme::drawPanelTitle (g, area.withHeight (13.0f), "HOW FAST IT GRABS AND LETS GO",
                              "gain reduction vs time, ms");

    PnsTheme::drawPlotCard (g, { px, py, pw, ph });

    for (int i = 0; i <= 4; ++i)
    {
        const float gr = m_grHi * (float) i / 4.0f;
        const float gy = Y (gr);
        g.setColour (i == 0 ? PnsTheme::kZeroLine : PnsTheme::kGridLine);
        g.drawHorizontalLine (juce::roundToInt (gy), px, px + pw);
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (juce::String (gr, 1), (int) area.getX(), juce::roundToInt (gy) - 6,
                    (int) kML - 4, 12, juce::Justification::centredRight);
    }
    for (int ms = 0; ms <= 1000; ms += 100)
    {
        const float gx = X ((float) ms);
        g.setColour (PnsTheme::kGridLine);
        g.drawVerticalLine (juce::roundToInt (gx), py, py + ph);
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (juce::String (ms), juce::roundToInt (gx) - 16, juce::roundToInt (py + ph) + 2,
                    32, 11, juce::Justification::centred);
    }

    // Step markers: attack onset at 0 ms, release onset at 500 ms.
    g.setColour (PnsTheme::kAccentPrimary.withAlpha (0.6f));
    g.drawVerticalLine (juce::roundToInt (X (0.0f)),   py, py + ph);
    g.drawVerticalLine (juce::roundToInt (X (500.0f)), py, py + ph);
    g.setColour (PnsTheme::kAccentPrimary);
    g.setFont (PnsTheme::fontLabel());
    g.drawText ("attack",  juce::roundToInt (X (4.0f)),   (int) py + 2, 60, 11, juce::Justification::centredLeft);
    g.drawText ("release", juce::roundToInt (X (504.0f)), (int) py + 2, 60, 11, juce::Justification::centredLeft);

    juce::Path path;
    bool started = false;
    int  nValid = 0;
    for (int b = 0; b < kBins; ++b)
    {
        if (! m_env.valid[(size_t) b]) continue;
        const float ms = (float) b / (float) kBins * 1000.0f;
        const float gr = juce::jlimit (0.0f, m_grHi, m_env.grDb[(size_t) b]);
        const float x = X (ms), y = Y (gr);
        if (! started) { path.startNewSubPath (x, y); started = true; }
        else             path.lineTo (x, y);
        ++nValid;
    }
    if (nValid > 1)
        PnsTheme::drawGlowLine (g, path, PnsTheme::kColorPost, 1.5f);

    // Cursor readout — time → gain reduction.
    if (nValid > 1 && m_cursorX >= 0.0f)
    {
        const float cx = juce::jlimit (px, px + pw, m_cursorX);
        const float ms = (cx - px) / pw * 1000.0f;
        const int b = juce::jlimit (0, kBins - 1, (int) (ms / 1000.0f * kBins));
        if (m_env.valid[(size_t) b])
        {
            const float gr = m_env.grDb[(size_t) b];
            juce::StringArray rows;
            rows.add (juce::String (juce::roundToInt (ms)) + " ms");
            rows.add ("GR " + juce::String (gr, 2) + " dB");
            drawCursorChip (g, { px, py, pw, ph }, cx, Y (juce::jlimit (0.0f, m_grHi, gr)),
                            PnsTheme::kColorPost, rows, m_cursorLocked);
        }
    }

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);

    if (nValid < 2)
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Press Measure (top right) - Pre steps the level up and down to time how fast your plugin reacts",
                    getLocalBounds().reduced (20), juce::Justification::centred);
    }
}

PnsTheme::Summary EnvelopeView::summary() const
{
    auto ms = [] (int b) { return (float) b / (float) kBins * 1000.0f; };
    const int half = kBins / 2;
    float peak = 0.0f; int nValid = 0;
    for (int b = 0; b < half; ++b)
        if (m_env.valid[(size_t) b]) { peak = juce::jmax (peak, m_env.grDb[(size_t) b]); ++nValid; }
    if (nValid < 2)
        return { "Press Measure  -  Pre steps the level up and down to time how fast your plugin reacts." };
    if (peak < 0.5f)
        return { "The test step didn't trigger any gain reduction  -  try a lower threshold." };

    int att = 0;                                   // 90% of the full reduction
    for (int b = 0; b < half; ++b)
        if (m_env.valid[(size_t) b] && m_env.grDb[(size_t) b] >= 0.9f * peak) { att = b; break; }
    const float startGr = m_env.valid[(size_t) half] ? m_env.grDb[(size_t) half] : peak;
    int rel = -1;                                  // back down to 10% of where it started
    for (int b = half; b < kBins; ++b)
        if (m_env.valid[(size_t) b] && m_env.grDb[(size_t) b] <= 0.1f * startGr) { rel = b; break; }

    juce::String s = "Grabs in about " + juce::String (juce::roundToInt (ms (att))) + " ms";
    s << ",  lets go in " << (rel < 0 ? juce::String ("more than 500 ms")
                                      : "about " + juce::String (juce::roundToInt (ms (rel) - 500.0f)) + " ms");
    return { s, false, "Attack", juce::String (juce::roundToInt (ms (att))), "ms" };
}
