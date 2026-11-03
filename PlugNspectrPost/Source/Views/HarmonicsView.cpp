#include "HarmonicsView.h"

#include <algorithm>
#include <cmath>
#include <set>

//==============================================================================
// HarmonicsView
//==============================================================================
HarmonicsView::HarmonicsView (PlugNspectrPostProcessor& p) : m_proc (p)
{
}

//──────────────────────────────────────────────────────────────────────────────
void HarmonicsView::setToneFreq (double freq)
{
    m_toneFreq = freq;
    repaint();
}

void HarmonicsView::setToneActive (bool active)
{
    if (active)
    {
        // Reactivating — clear freeze and start fresh
        m_toneActive      = true;
        m_harmonicsPaused = false;
        m_harmPre .fill (0.0f);
        m_harmPost.fill (0.0f);
        m_thdPre  = 0.0f;
        m_thdPost = 0.0f;
    }
    else
    {
        // Deactivating — freeze the last captured frame
        m_toneActive      = false;
        m_harmonicsPaused = true;
    }
    repaint();
}

void HarmonicsView::mouseMove (const juce::MouseEvent& e)
{
    (void) e;
    // Harmonic dot hit-testing is done in paint(); just trigger repaint on move.
    repaint();
}

void HarmonicsView::mouseExit (const juce::MouseEvent&)
{
    if (m_hoveredHarmonic != -1) { m_hoveredHarmonic = -1; repaint(); }
}

//──────────────────────────────────────────────────────────────────────────────

//──────────────────────────────────────────────────────────────────────────────
void HarmonicsView::update()
{
    if (m_harmonicsPaused) return;

    m_proc.getSpectra (m_specPre, m_specPost);

    const double sr = m_proc.getSampleRate();
    if (sr <= 0.0) return;

    const float binW     = (float) (sr / PlugNspectrPostProcessor::kFftSize);
    const float fund     = (float) m_toneFreq;
    constexpr float kDecay  = 0.9f;
    constexpr float kAttack = 1.0f - kDecay;

    for (int h = 0; h < kNumH; ++h)  // h=0 → H1, h=1 → H2, …
    {
        const float targetFreq   = fund * (float) (h + 1);
        const int   targetBin    = juce::roundToInt (targetFreq / binW);
        // Peak search, ±3 bins at most but kept under half the harmonic
        // spacing so a harmonic's window never reaches the fundamental's
        // main lobe (a fixed ±3 did at low tones / high sample rates — at
        // 96 kHz H2's window held the fundamental's own bin).
        const int   searchRadius = juce::jlimit (0, 3, (int) (fund / binW / 2.0f) - 1);
        const int   lo = juce::jmax (1, targetBin - searchRadius);
        const int   hi = juce::jmin (PlugNspectrPostProcessor::kNumSpecBins - 1,
                                     targetBin + searchRadius);

        float peakPre = 0.0f, peakPost = 0.0f;
        for (int k = lo; k <= hi; ++k)
        {
            peakPre  = juce::jmax (peakPre,  m_specPre [k]);
            peakPost = juce::jmax (peakPost, m_specPost[k]);
        }

        m_harmPre [h] = m_harmPre [h] * kDecay + peakPre  * kAttack;
        m_harmPost[h] = m_harmPost[h] * kDecay + peakPost * kAttack;
    }

    // THD: sqrt(H2²+…+H8²) / H1  (linear amplitudes)
    auto calcThd = [this] (const std::array<float, kNumH>& harm) -> float {
        if (harm[0] < 1.0e-9f) return 0.0f;
        float sumSq = 0.0f;
        for (int h = 1; h < kNumH; ++h) sumSq += harm[h] * harm[h];
        return 100.0f * std::sqrt (sumSq) / harm[0];
    };

    m_thdPre  = calcThd (m_harmPre);
    m_thdPost = calcThd (m_harmPost);
}

//──────────────────────────────────────────────────────────────────────────────
void HarmonicsView::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);

    // Show "Activate" prompt only when tone has never been run (no frozen data)
    if (!m_toneActive && !m_harmonicsPaused)
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Turn on Test Tone (bottom bar) - Pre plays a pure tone through your plugin",
                    getLocalBounds(), juce::Justification::centred);
        return;
    }

    // ── Split area: spectrum left, readouts right ─────────────────────────
    constexpr int kReadoutW = 160;
    const float W = (float) getWidth(), H = (float) getHeight();
    const auto specArea    = juce::Rectangle<float> (0, 0, W - kReadoutW, H);
    const auto readoutArea = juce::Rectangle<float> (W - kReadoutW, 0, (float) kReadoutW, H);

    drawSpectrumArea (g, specArea);
    drawReadouts     (g, readoutArea);

    // ── "PAUSED" badge in top-left of spectrum area ───────────────────────
    if (m_harmonicsPaused)
    {
        constexpr float kML = 44.0f, kMT = 28.0f;
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary.withAlpha (0.65f));
        g.drawText ("PAUSED", (int) kML + 4, (int) kMT + 4, 60, 12,
                    juce::Justification::centredLeft);
    }
}

//──────────────────────────────────────────────────────────────────────────────
void HarmonicsView::drawSpectrumArea (juce::Graphics& g, juce::Rectangle<float> area)
{
    const double sr = m_proc.getSampleRate();
    if (sr <= 0.0) return;

    constexpr float kML = 44.0f, kMR = 8.0f, kMT = 28.0f, kMB = 22.0f;
    const float px = area.getX() + kML,  py = area.getY() + kMT;
    const float pw = area.getWidth()  - kML - kMR;
    const float ph = area.getHeight() - kMT - kMB;
    if (pw <= 0 || ph <= 0) return;

    const float fund     = (float) m_toneFreq;
    const float xMin     = fund * 0.8f;
    const float xMax     = fund * 8.0f * 1.5f;
    const float kLogMin  = std::log10 (xMin);
    const float kLogMax  = std::log10 (xMax);
    constexpr float kMinDb = -60.0f, kMaxDb = 0.0f;
    const float binW     = (float) (sr / PlugNspectrPostProcessor::kFftSize);

    auto freqToX = [&] (float f) -> float {
        f = juce::jlimit (xMin, xMax, f);
        return px + pw * (std::log10 (f) - kLogMin) / (kLogMax - kLogMin);
    };
    auto dbToY = [&] (float db) -> float {
        db = juce::jlimit (kMinDb, kMaxDb, db);
        return py + ph * (1.0f - (db - kMinDb) / (kMaxDb - kMinDb));
    };
    auto magToDb = [] (float m) { return 20.0f * std::log10 (juce::jmax (m, 1.0e-6f)); };

    // Background
    PnsTheme::drawPlotCard (g, { px, py, pw, ph });

    // Title
    PnsTheme::drawPanelTitle (g, area.withHeight (kMT), "WHAT IT ADDS", "harmonic spectrum");

    // dB grid
    g.setFont (PnsTheme::fontLabel());
    for (float db : { 0.0f, -10.0f, -20.0f, -30.0f, -40.0f, -50.0f, -60.0f })
    {
        const float y = dbToY (db);
        g.setColour (db == 0.0f ? PnsTheme::kZeroLine : PnsTheme::kGridLine);
        g.drawHorizontalLine (juce::roundToInt (y), px, px + pw);
        g.setColour (PnsTheme::kGridLabel);
        juce::String lbl = (db == 0.0f) ? "0" : juce::String (juce::roundToInt (db));
        g.drawText (lbl, (int) area.getX(), juce::roundToInt (y) - 7,
                    juce::roundToInt (kML) - 4, 14, juce::Justification::centredRight);
    }

    // Harmonic marker lines and labels (H1-H8)
    const juce::Colour kHarmCols[8] = {
        PnsTheme::kColorPost,
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kTextPrimary, 1.0f/6.0f),
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kTextPrimary, 2.0f/6.0f),
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kTextPrimary, 3.0f/6.0f),
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kTextPrimary, 4.0f/6.0f),
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kTextPrimary, 5.0f/6.0f),
        PnsTheme::kTextPrimary,
        PnsTheme::kTextPrimary,
    };

    for (int h = 0; h < kNumH; ++h)
    {
        const float freq = fund * (float) (h + 1);
        if (freq < xMin || freq > xMax) continue;
        const float x = freqToX (freq);
        g.setColour (PnsTheme::kBorderSubtle);
        PnsTheme::drawDottedVLine (g, x, py, py + ph);
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kGridLabel);
        g.drawText ("H" + juce::String (h + 1),
                    juce::roundToInt (x) - 10, juce::roundToInt (py) - (int) kMT + 2,
                    20, 12, juce::Justification::centred);
        (void) kHarmCols;
    }

    // PRE spectrum — ghosted lavender at 40%, 1px
    {
        juce::Path p;
        bool started = false;
        for (int k = 1; k < PlugNspectrPostProcessor::kNumSpecBins; ++k)
        {
            const float f = (float) k * binW;
            if (f < xMin) continue;
            if (f > xMax) break;
            const float x = freqToX (f);
            const float y = dbToY (juce::jlimit (kMinDb, kMaxDb, magToDb (m_specPre[k])));
            if (!started) { p.startNewSubPath (x, y); started = true; }
            else          { p.lineTo (x, y); }
        }
        if (started)
        {
            g.setColour (PnsTheme::kColorPre.withAlpha (0.40f));
            g.strokePath (p, juce::PathStrokeType (1.0f));
        }
    }

    // POST spectrum — teal with glow
    {
        juce::Path p;
        bool started = false;
        for (int k = 1; k < PlugNspectrPostProcessor::kNumSpecBins; ++k)
        {
            const float f = (float) k * binW;
            if (f < xMin) continue;
            if (f > xMax) break;
            const float x = freqToX (f);
            const float y = dbToY (juce::jlimit (kMinDb, kMaxDb, magToDb (m_specPost[k])));
            if (!started) { p.startNewSubPath (x, y); started = true; }
            else          { p.lineTo (x, y); }
        }
        if (started)
            PnsTheme::drawGlowLine (g, p, PnsTheme::kColorPost, 1.5f);
    }

    // Harmonic peak dots on POST (H2-H8): cyan→pink gradient with glow + hover tooltip
    const juce::Point<float> mousePos = getMouseXYRelative().toFloat();
    m_hoveredHarmonic = -1;
    for (int h = 1; h < kNumH; ++h)   // H2-H8
    {
        const float freq  = fund * (float) (h + 1);
        if (freq < xMin || freq > xMax) continue;
        const float x     = freqToX (freq);
        const float db    = magToDb (m_harmPost[h]);
        if (db < kMinDb + 1.0f) continue;
        const float y     = dbToY (juce::jlimit (kMinDb, kMaxDb, db));
        const float t     = (float) (h - 1) / (float) (kNumH - 2);  // 0=H2, 1=H8
        const juce::Colour col = PnsTheme::kColorHarmLow.interpolatedWith (PnsTheme::kColorHarmHigh, t);

        // Glow halo
        g.setColour (col.withAlpha (0.22f));
        g.fillEllipse (x - 7.0f, y - 7.0f, 14.0f, 14.0f);
        g.setColour (col.withAlpha (0.45f));
        g.fillEllipse (x - 5.0f, y - 5.0f, 10.0f, 10.0f);
        // Core dot
        g.setColour (col);
        g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);

        // Hover detection
        if (mousePos.getDistanceFrom ({ x, y }) < 10.0f)
            m_hoveredHarmonic = h;
    }

    // Tooltip for hovered harmonic
    if (m_hoveredHarmonic >= 1)
    {
        const float freq = fund * (float) (m_hoveredHarmonic + 1);
        const float db   = magToDb (m_harmPost[m_hoveredHarmonic]);
        const juce::String tip = "H" + juce::String (m_hoveredHarmonic + 1)
                               + ": " + juce::String (db, 1) + " dB";
        const float tx = freqToX (freq);
        const float ty = dbToY (juce::jlimit (kMinDb, kMaxDb, db)) - 18.0f;
        g.setColour (PnsTheme::kBgWidget);
        g.fillRoundedRectangle (tx - 2.0f, ty - 2.0f, 70.0f, 15.0f, 3.0f);
        g.setColour (PnsTheme::kBorderSubtle);
        g.drawRoundedRectangle (tx - 2.0f, ty - 2.0f, 70.0f, 15.0f, 3.0f, 1.0f);
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextPrimary);
        g.drawText (tip, (int) tx - 2, (int) ty - 2, 70, 15, juce::Justification::centred);
    }

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);
}

//──────────────────────────────────────────────────────────────────────────────
void HarmonicsView::drawReadouts (juce::Graphics& g, juce::Rectangle<float> area)
{
    auto magToDb = [] (float m) { return 20.0f * std::log10 (juce::jmax (m, 1.0e-6f)); };
    auto harmDbStr = [&] (const std::array<float, kNumH>& harm, int h) -> juce::String {
        const float db = magToDb (harm[h]);
        return (db < -59.0f) ? "---" : juce::String (juce::roundToInt (db));
    };

    const int lx = (int) area.getX() + PnsTheme::kPaddingSmall;
    const int rx = (int) area.getRight() - PnsTheme::kPaddingMid;
    int y = (int) area.getY() + PnsTheme::kPaddingLarge + 16;
    constexpr int rowH = 15;

    // Frosted panel — covers all readout content (176px: THD+Harmonics rows)
    {
        constexpr int kPanelH = 182;   // measured span + padding
        PnsTheme::drawFrostedPanel (g, juce::Rectangle<float> (
            (float) lx, (float) (y - PnsTheme::kPaddingSmall),
            (float) (rx - lx), (float) kPanelH));
    }

    // ── THD section ───────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("DISTORTION (THD)", lx, y, rx - lx, 11, juce::Justification::centredLeft);
    y += 14;

    auto thdStr = [] (float v) -> juce::String {
        return juce::String (v, 2) + " %";
    };

    g.drawText ("Pre",  lx, y, 30, 12, juce::Justification::centredLeft);
    g.setFont (PnsTheme::fontReadout());
    g.setColour (PnsTheme::kTextPrimary);
    g.drawText (thdStr (m_thdPre), lx + 28, y - 1, rx - lx - 28, 14, juce::Justification::centredRight);
    y += rowH;

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("Post", lx, y, 30, 12, juce::Justification::centredLeft);
    g.setFont (PnsTheme::fontReadout());
    g.setColour (PnsTheme::kColorPost);
    g.drawText (thdStr (m_thdPost), lx + 28, y - 1, rx - lx - 28, 14, juce::Justification::centredRight);
    y += rowH + 4;

    // Divider
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawHorizontalLine (y, (float) lx, (float) rx);
    y += 6;

    // ── HARMONICS (dB) ────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("EACH HARMONIC (dB)", lx, y, rx - lx, 11, juce::Justification::centredLeft);
    y += 14;

    for (int h = 1; h < kNumH; ++h)   // H2-H8
    {
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("H" + juce::String (h + 1) + ":", lx, y, 20, 12, juce::Justification::centredLeft);

        // PRE value
        g.setColour (PnsTheme::kColorPreAvg);
        g.drawText (harmDbStr (m_harmPre, h),  lx + 22, y, 30, 12, juce::Justification::centredLeft);

        // Separator
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("/", lx + 54, y, 8, 12, juce::Justification::centred);

        // POST value
        g.setColour (PnsTheme::kColorPost);
        g.drawText (harmDbStr (m_harmPost, h), lx + 64, y, 30, 12, juce::Justification::centredLeft);

        y += rowH;
    }
}

PnsTheme::Summary HarmonicsView::summary() const
{
    if (! m_toneActive && ! m_harmonicsPaused)
        return { "Turn on Test Tone  -  Pre plays a pure tone through your plugin." };

    const float added = m_thdPost - m_thdPre;
    const juce::String hero = PnsTheme::fmtPct (m_thdPost).dropLastCharacters (1);
    if (m_thdPost < 0.01f || added < 0.005f)
        return { "Clean  -  no measurable harmonics added", false, "Distortion", hero, "%" };

    int dom = 1;                                 // loudest of H2..H8 at the output
    for (int h = 2; h < kNumH; ++h)
        if (m_harmPost[(size_t) h] > m_harmPost[(size_t) dom]) dom = h;
    const int n = dom + 1;
    const juce::String which = n == 2 ? juce::String ("2nd") : n == 3 ? juce::String ("3rd")
                                                             : juce::String (n) + "th";
    const juce::String flavour = (n % 2 == 0) ? "even - smooth, warm" : "odd - edgier, brighter";
    return { "Adds mostly the " + which + " harmonic  -  " + flavour, false, "Distortion", hero, "%" };
}
