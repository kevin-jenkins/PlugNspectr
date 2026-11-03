#include "LinearView.h"

#include <algorithm>
#include <cmath>
#include <set>

//==============================================================================
// PlugNspectrPostEditor
//==============================================================================
//==============================================================================
// LinearView — magnitude / phase / group-delay of the measured transfer function
//==============================================================================
LinearView::LinearView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    m_measureBtn.setClickingTogglesState (true);
    m_measureBtn.getProperties().set ("armed", true);
    m_measureBtn.onClick = [this]
    {
        m_measureActive = m_measureBtn.getToggleState();
        if (m_measureActive) m_proc.resetMeasurement();   // start fresh; pausing keeps the curve
        if (onMeasureChanged) onMeasureChanged();
        repaint();
    };
    addAndMakeVisible (m_measureBtn);

    // Freeze: snapshot the current curves as a dimmed A/B reference (click again
    // to clear). Lets you compare two plugin settings / two plugins.
    m_freezeBtn.onClick = [this]
    {
        if (m_hasFrozen) m_hasFrozen = false;
        else             doFreeze();
        m_freezeBtn.setToggleState (m_hasFrozen, juce::dontSendNotification);
        m_freezeBtn.setButtonText (m_hasFrozen ? "Frozen" : "Freeze");
        repaint();
    };
    addAndMakeVisible (m_freezeBtn);
}

void LinearView::doFreeze()
{
    m_frozenMag   = m_meas.magDb;
    m_frozenPhase = m_phaseDeg;
    m_frozenGroup = m_groupMs;
    m_hasFrozen   = (m_meas.frames > 0);
}

void LinearView::freezeForTest() { doFreeze(); repaint(); }

void LinearView::resized()
{
    m_measureBtn.setBounds (getWidth() - 90, PnsTheme::kPaddingSmall,
                            78, PnsTheme::kButtonHeight);
    m_freezeBtn .setBounds (getWidth() - 90 - 84, PnsTheme::kPaddingSmall,
                            78, PnsTheme::kButtonHeight);
}

float LinearView::freqToX (double f, juce::Rectangle<float> r) const
{
    constexpr double lo = 20.0, hi = 20000.0;
    const double t = std::log10 (juce::jlimit (lo, hi, f) / lo) / std::log10 (hi / lo);
    return r.getX() + (float) t * r.getWidth();
}

void LinearView::update()
{
    m_proc.getMeasurement (m_meas);
    const double sr = m_meas.sampleRate;
    if (sr <= 0.0) return;
    const int    N  = PlugNspectrPostProcessor::kMeasFftSize;
    const double pi = juce::MathConstants<double>::pi;

    // Latency compensation: a pure Pre→Post delay of L samples is a linear phase
    // ramp (−2π·k·L/N). De-rotate it so the graphs show the plugin's *intrinsic*
    // phase / group delay, with the bulk delay reported separately.
    // The processor already removed alignSamples by delaying Pre, so L is
    // only what is left; the plugin's delay is the sum.
    const int    L     = m_meas.latencySamples;
    m_latSamples       = L + m_meas.alignSamples;
    m_latMs            = (float) (1000.0 * m_latSamples / sr);
    auto wrap = [&] (double a) { while (a >  pi) a -= 2.0 * pi;
                                 while (a < -pi) a += 2.0 * pi; return a; };

    std::array<double, kBins> cph {};   // intrinsic phase, wrapped
    for (int k = 0; k < kBins; ++k)
        cph[k] = wrap (m_meas.phase[k] + 2.0 * pi * (double) k * L / N);

    for (int k = 0; k < kBins; ++k)
        m_phaseDeg[k] = (float) (cph[k] * 180.0 / pi);

    // Unwrap the compensated phase, then central-difference → group delay (ms).
    std::array<double, kBins> uw {};
    double off = 0.0, prev = cph[0];
    uw[0] = cph[0];
    for (int k = 1; k < kBins; ++k)
    {
        const double d = cph[k] - prev;
        if      (d >  pi) off -= 2.0 * pi;
        else if (d < -pi) off += 2.0 * pi;
        prev  = cph[k];
        uw[k] = cph[k] + off;
    }

    const double df = sr / N;
    float gdLo = 1.0e9f, gdHi = -1.0e9f;
    for (int k = 0; k < kBins; ++k)
    {
        const int k0 = juce::jmax (k - 1, 0), k1 = juce::jmin (k + 1, kBins - 1);
        const double slope = (uw[k1] - uw[k0]) / (2.0 * pi * df * (k1 - k0));
        m_groupMs[k] = (float) (-slope * 1000.0);

        const double f = (double) k * df;
        if (f >= 40.0 && f <= 16000.0 && m_meas.coh[k] > 0.5f)
        { gdLo = juce::jmin (gdLo, m_groupMs[k]); gdHi = juce::jmax (gdHi, m_groupMs[k]); }
    }
    if (gdHi < gdLo) { gdLo = -2.0f; gdHi = 10.0f; }
    const float pad = juce::jmax (1.0f, (gdHi - gdLo) * 0.15f);
    m_gdLo = gdLo - pad; m_gdHi = gdHi + pad;
}

// Hairline cursor: hover to read values; click to lock/unlock.
static bool linearCursorInPlot (juce::Component& c, juce::Point<float> p, float& outX)
{
    auto a = c.getLocalBounds().toFloat().reduced (8.0f);
    a.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);
    const float px = a.getX() + 46.0f, pw = a.getWidth() - 46.0f - 12.0f;
    outX = p.x;
    return (p.x >= px && p.x <= px + pw && p.y >= a.getY());
}

void LinearView::mouseMove (const juce::MouseEvent& e)
{
    if (m_cursorLocked) return;
    float x; m_cursorX = linearCursorInPlot (*this, e.position, x) ? x : -1.0f;
    repaint();
}

void LinearView::mouseExit (const juce::MouseEvent&)
{
    if (! m_cursorLocked) { m_cursorX = -1.0f; repaint(); }
}

void LinearView::mouseDown (const juce::MouseEvent& e)
{
    float x;
    if (linearCursorInPlot (*this, e.position, x))
    {
        m_cursorX = x;
        m_cursorLocked = ! m_cursorLocked;
        repaint();
    }
}

void LinearView::drawPanel (juce::Graphics& g, juce::Rectangle<float> r, const char* title,
                            const std::array<float, kBins>& vals, float vMin, float vMax,
                            const juce::String& /*unit*/, juce::Colour curve,
                            const std::array<float, kBins>* frozen) const
{
    constexpr float kML = 46.0f, kMR = 12.0f, kMT = 15.0f, kMB = 14.0f;
    const float px = r.getX() + kML, py = r.getY() + kMT;
    const float pw = r.getWidth() - kML - kMR, ph = r.getHeight() - kMT - kMB;
    if (pw <= 0 || ph <= 0) return;

    PnsTheme::drawPlotCard (g, { px, py, pw, ph });

    {   // "PLAIN|technical" — plain name first, engineering term after it
        const juce::String t (title);
        PnsTheme::drawPanelTitle (g, r.withHeight (13.0f), t.upToFirstOccurrenceOf ("|", false, false),
                                  t.fromFirstOccurrenceOf ("|", false, false));
    }

    auto valToY = [&] (float v) { return py + ph * (vMax - v) / (vMax - vMin); };

    const juce::Rectangle<float> plot (px, py, pw, ph);
    const int dec = (std::abs (vMax - vMin) <= 8.0f) ? 1 : 0;
    for (int i = 0; i <= 4; ++i)
    {
        const float v  = vMax - (vMax - vMin) * (float) i / 4.0f;
        const float gy = valToY (v);
        g.setColour (std::abs (v) < 1.0e-3f ? PnsTheme::kZeroLine : PnsTheme::kGridLine);
        g.drawHorizontalLine (juce::roundToInt (gy), px, px + pw);
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (juce::String (v, dec), (int) r.getX(), (int) gy - 6,
                    (int) kML - 4, 12, juce::Justification::centredRight);
    }

    const double vfreqs[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (double f : vfreqs)
    {
        const float gx = freqToX (f, plot);
        g.setColour (PnsTheme::kGridLine);
        g.drawVerticalLine (juce::roundToInt (gx), py, py + ph);
        g.setColour (PnsTheme::kGridLabel);
        const juce::String lbl = (f >= 1000.0) ? juce::String (f / 1000.0, 0) + "k"
                                               : juce::String ((int) f);
        g.drawText (lbl, juce::roundToInt (gx) - 14, juce::roundToInt (py + ph) + 1,
                    28, 11, juce::Justification::centred);
    }

    const double sr = (m_meas.sampleRate > 0.0) ? m_meas.sampleRate
                                                 : (double) PlugNspectrPostProcessor::kMeasFftSize;
    const int    N  = PlugNspectrPostProcessor::kMeasFftSize;
    auto buildPath = [&] (const std::array<float, kBins>& v) -> juce::Path
    {
        juce::Path path;
        bool started = false;
        for (int k = 1; k < kBins; ++k)
        {
            const double f = (double) k * sr / N;
            if (f < 20.0 || f > 20000.0) continue;
            const float x = freqToX (f, plot);
            const float y = juce::jlimit (py, py + ph, valToY (juce::jlimit (vMin, vMax, v[k])));
            if (! started) { path.startNewSubPath (x, y); started = true; }
            else             path.lineTo (x, y);
        }
        return path;
    };

    // Frozen reference first (dimmed grey), then the live curve over it.
    if (frozen != nullptr)
    {
        g.setColour (PnsTheme::kTextSecondary.withAlpha (0.5f));
        g.strokePath (buildPath (*frozen), juce::PathStrokeType (1.0f));
    }

    if (m_meas.frames > 0)
        PnsTheme::drawGlowLine (g, buildPath (vals), curve, 1.5f);

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);
}

void LinearView::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);

    auto area = getLocalBounds().toFloat().reduced (8.0f);
    area.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);   // room for Measure button

    const float h    = area.getHeight() / 3.0f;
    const auto  magR = area.removeFromTop (h);
    const auto  phR  = area.removeFromTop (h);
    const auto  gdR  = area;

    drawPanel (g, magR, "LEVEL CHANGE|magnitude, dB",   m_meas.magDb, -48.0f,  12.0f, "dB",  PnsTheme::kColorPost,
               m_hasFrozen ? &m_frozenMag   : nullptr);
    drawPanel (g, phR,  "PHASE SHIFT|phase, deg",      m_phaseDeg,  -180.0f, 180.0f, "deg", PnsTheme::kColorPost,
               m_hasFrozen ? &m_frozenPhase : nullptr);
    drawPanel (g, gdR,  "DELAY BY FREQUENCY|group delay, ms", m_groupMs,    m_gdLo,  m_gdHi, "ms",  PnsTheme::kColorPost,
               m_hasFrozen ? &m_frozenGroup : nullptr);

    // ── Cursor readout — hairline across all panels + dot + value chip ─────
    if (m_meas.frames > 0 && m_cursorX >= 0.0f)
    {
        constexpr float kML = 46.0f, kMR = 12.0f, kMT = 15.0f, kMB = 14.0f;
        const float px = magR.getX() + kML, pw = magR.getWidth() - kML - kMR;
        const float cx = juce::jlimit (px, px + pw, m_cursorX);
        const double t  = (double) (cx - px) / pw;
        const double f  = 20.0 * std::pow (1000.0, t);                 // inverse of freqToX
        const double sr = (m_meas.sampleRate > 0.0) ? m_meas.sampleRate
                                                    : (double) PlugNspectrPostProcessor::kMeasFftSize;
        const int N = PlugNspectrPostProcessor::kMeasFftSize;
        const int k = juce::jlimit (1, kBins - 1, (int) std::lround (f * N / sr));

        struct P { juce::Rectangle<float> r; float v, vMin, vMax; juce::Colour col; };
        const P panels[3] = {
            { magR, m_meas.magDb[k], -48.0f,  12.0f, PnsTheme::kColorPost    },
            { phR,  m_phaseDeg[k],  -180.0f, 180.0f, PnsTheme::kColorPost },
            { gdR,  m_groupMs[k],    m_gdLo,  m_gdHi, PnsTheme::kColorPost },
        };
        for (const auto& p : panels)
        {
            const float pyTop = p.r.getY() + kMT, ph2 = p.r.getHeight() - kMT - kMB;
            g.setColour (PnsTheme::kAccentPrimary.withAlpha (0.45f));
            g.drawVerticalLine (juce::roundToInt (cx), pyTop, pyTop + ph2);
            const float y = pyTop + ph2 * (p.vMax - juce::jlimit (p.vMin, p.vMax, p.v)) / (p.vMax - p.vMin);
            g.setColour (p.col);
            g.fillEllipse (cx - 3.0f, y - 3.0f, 6.0f, 6.0f);
        }

        const juce::String fStr = (f >= 1000.0) ? juce::String (f / 1000.0, 2) + " kHz"
                                                : juce::String ((int) f) + " Hz";
        const juce::String rows[4] = {
            fStr,
            "mag  "  + juce::String (panels[0].v, 1) + " dB",
            "phase " + juce::String (juce::roundToInt (panels[1].v)) + " deg",
            "gd   "  + juce::String (panels[2].v, 2) + " ms" };
        constexpr float boxW = 100.0f, boxH = 60.0f;
        float bx = cx + 10.0f;
        if (bx + boxW > magR.getRight()) bx = cx - 10.0f - boxW;
        const float by = magR.getY() + kMT + 4.0f;
        g.setColour (PnsTheme::kBgDark.withAlpha (0.92f));
        g.fillRoundedRectangle (bx, by, boxW, boxH, 6.0f);
        g.setColour (PnsTheme::kAccentPrimary.withAlpha (0.7f));
        g.drawRect (bx, by, boxW, boxH, 1.0f);
        g.setFont (PnsTheme::fontLabel());
        for (int i = 0; i < 4; ++i)
        {
            g.setColour (i == 0 ? PnsTheme::kTextPrimary : PnsTheme::kTextSecondary);
            g.drawText (rows[i], juce::roundToInt (bx) + 6, juce::roundToInt (by) + 4 + i * 13,
                        (int) boxW - 10, 12, juce::Justification::centredLeft);
        }
        if (m_cursorLocked)
        {
            g.setColour (PnsTheme::kAccentPrimary);
            g.fillEllipse (bx + boxW - 12.0f, by + 4.0f, 5.0f, 5.0f);
        }
    }

    // Measured latency readout (de-rotated out of the phase/group-delay above).
    if (m_meas.frames > 0)
    {
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary);
        const juce::String txt = "Latency: " + juce::String (m_latSamples) + " smp  ("
                               + juce::String (m_latMs, 2) + " ms)";
        g.drawText (txt, 8, PnsTheme::kPaddingSmall, getWidth() - 100,
                    PnsTheme::kButtonHeight, juce::Justification::centredLeft);
    }

    if (m_meas.frames == 0)
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Press Measure (top right) - Pre plays noise through your plugin to map its EQ curve",
                    getLocalBounds().reduced (20), juce::Justification::centred);
    }
}

PnsTheme::Summary LinearView::summary() const
{
    if (m_meas.frames == 0)
        return { "Press Measure  -  Pre plays noise through your plugin to map its EQ curve." };

    const double sr = m_meas.sampleRate;
    const int    N  = PlugNspectrPostProcessor::kMeasFftSize;
    float hiV = -1.0e9f, loV = 1.0e9f;
    double hiF = 0.0, loF = 0.0;
    for (int k = 1; k < kBins; ++k)
    {
        const double f = (double) k * sr / N;
        if (f < 30.0 || f > 16000.0 || m_meas.coh[(size_t) k] < 0.5f) continue;
        const float v = m_meas.magDb[(size_t) k];
        if (v > hiV) { hiV = v; hiF = f; }
        if (v < loV) { loV = v; loF = f; }
    }
    if (hiV < loV) return { "Measuring... keep playback running." };

    juce::StringArray parts;
    if (hiV >=  0.5f) parts.add ("boosts up to " + juce::String (hiV, 1) + " dB near " + PnsTheme::fmtHz (hiF));
    if (loV <= -0.5f) parts.add ("cuts up to "   + juce::String (-loV, 1) + " dB near " + PnsTheme::fmtHz (loF));
    juce::String s = parts.isEmpty() ? juce::String ("flat  -  within 0.5 dB everywhere")
                                     : parts.joinIntoString (",  ");
    s = s.substring (0, 1).toUpperCase() + s.substring (1);
    if (m_latSamples != 0)
        s << "  -  delays audio by " << m_latSamples << " samples";
    const bool boost = std::abs (hiV) >= std::abs (loV);
    const float v = boost ? hiV : loV;
    return { s, false, boost ? "Biggest boost" : "Biggest cut",
             (v >= 0.0f ? "+" : "") + juce::String (v, 1), "dB" };
}
