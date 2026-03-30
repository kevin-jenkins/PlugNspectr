/*
  ==============================================================================
    PlugNspectrPost  –  PluginEditor.cpp
  ==============================================================================
*/

#include "PluginEditor.h"
#include <cmath>
#include <algorithm>

//==============================================================================
// SpectrumView
//==============================================================================
SpectrumView::SpectrumView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    // Smoothing combo — styled by PnsLookAndFeel
    m_smoothBox.addItem ("Fast",   1);
    m_smoothBox.addItem ("Medium", 2);
    m_smoothBox.addItem ("Slow",   3);
    m_smoothBox.setSelectedId (2, juce::dontSendNotification);
    m_smoothBox.onChange = [this]
    {
        switch (m_smoothBox.getSelectedId())
        {
            case 1:  setSmoothPreset (SmoothPreset::Fast);   break;
            case 3:  setSmoothPreset (SmoothPreset::Slow);   break;
            default: setSmoothPreset (SmoothPreset::Medium); break;
        }
    };
    addAndMakeVisible (m_smoothBox);

    // Avg toggle — styled by PnsLookAndFeel
    m_avgBtn.setToggleable (true);
    m_avgBtn.setToggleState (true, juce::dontSendNotification);
    m_avgBtn.onClick = [this]
    {
        m_showAvg = !m_showAvg;
        m_avgBtn.setToggleState (m_showAvg, juce::dontSendNotification);
    };
    addAndMakeVisible (m_avgBtn);
}

void SpectrumView::resized()
{
    const int W = getWidth();
    constexpr int bh      = PnsTheme::kButtonHeight;
    constexpr int marginR = PnsTheme::kPaddingMid;
    constexpr int marginT = PnsTheme::kPaddingSmall;
    constexpr int gap     = 4;
    constexpr int avgW    = 38;
    constexpr int comboW  = 72;
    m_avgBtn   .setBounds (W - marginR - avgW,                  marginT, avgW,  bh);
    m_smoothBox.setBounds (W - marginR - avgW - gap - comboW,   marginT, comboW, bh);
}

void SpectrumView::setSmoothPreset (SmoothPreset p)
{
    switch (p)
    {
        case SmoothPreset::Fast:   m_decay = 0.60f; break;
        case SmoothPreset::Medium: m_decay = 0.85f; break;
        case SmoothPreset::Slow:   m_decay = 0.95f; break;
    }
}

void SpectrumView::update()
{
    m_proc.getSpectra (m_rawPre, m_rawPost);

    const float attack = 1.0f - m_decay;
    const float avgAtt = 1.0f - kAvgDecay;

    for (int k = 0; k < PlugNspectrPostProcessor::kNumSpecBins; ++k)
    {
        m_pre [k] = m_pre [k] * m_decay + m_rawPre [k] * attack;
        m_post[k] = m_post[k] * m_decay + m_rawPost[k] * attack;

        m_peakPre [k] = juce::jmax (m_pre [k], m_peakPre [k] * kPeakDecay);
        m_peakPost[k] = juce::jmax (m_post[k], m_peakPost[k] * kPeakDecay);

        m_avgPre [k] = m_avgPre [k] * kAvgDecay + m_rawPre [k] * avgAtt;
        m_avgPost[k] = m_avgPost[k] * kAvgDecay + m_rawPost[k] * avgAtt;
    }
}

void SpectrumView::paint (juce::Graphics& g)
{
    constexpr float kML = 44.0f, kMR = 12.0f, kMT = 10.0f, kMB = 24.0f;
    constexpr float kMinFreq = 20.0f, kMaxFreq = 20000.0f;
    constexpr float kMinDb   = -90.0f, kMaxDb   = +12.0f;

    const float W = (float) getWidth(),  H = (float) getHeight();
    const float px = kML,  py = kMT;
    const float pw = W - kML - kMR,  ph = H - kMT - kMB;

    const float kLogMin = std::log10 (kMinFreq);
    const float kLogMax = std::log10 (kMaxFreq);

    auto freqToX = [&] (float f) -> float {
        f = juce::jlimit (kMinFreq, kMaxFreq, f);
        return px + pw * (std::log10 (f) - kLogMin) / (kLogMax - kLogMin);
    };
    auto dbToY = [&] (float db) -> float {
        db = juce::jlimit (kMinDb, kMaxDb, db);
        return py + ph * (1.0f - (db - kMinDb) / (kMaxDb - kMinDb));
    };
    auto magToDb = [] (float mag) -> float {
        return 20.0f * std::log10 (juce::jmax (mag, 1.0e-6f));
    };

    // ── Background ────────────────────────────────────────────────────────
    g.fillAll (PnsTheme::kBgDark);
    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (px, py, pw, ph);

    // ── Grid ──────────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());

    const float freqMarkers[] = { 30, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (float freq : freqMarkers)
    {
        const float x = freqToX (freq);
        g.setColour (PnsTheme::kGridLine);
        g.drawVerticalLine (juce::roundToInt (x), py, py + ph);

        juce::String lbl = (freq >= 1000.0f)
            ? juce::String (juce::roundToInt (freq / 1000.0f)) + "k"
            : juce::String (juce::roundToInt (freq));
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (lbl, juce::roundToInt (x) - 14, juce::roundToInt (py + ph) + 3,
                    28, 14, juce::Justification::centred);
    }

    const float dbMarkers[] = { +12, +6, 0, -6, -12, -24, -36, -48, -60, -72, -90 };
    for (float db : dbMarkers)
    {
        const float y = dbToY (db);
        g.setColour (db == 0.0f ? PnsTheme::kZeroLine : PnsTheme::kGridLine);
        g.drawHorizontalLine (juce::roundToInt (y), px, px + pw);

        juce::String lbl = (db > 0) ? ("+" + juce::String (juce::roundToInt (db)))
                                     : (db == 0 ? "0" : juce::String (juce::roundToInt (db)));
        g.setColour (db == 0.0f ? PnsTheme::kTextSecondary : PnsTheme::kGridLabel);
        g.drawText (lbl, 2, juce::roundToInt (y) - 7,
                    juce::roundToInt (kML) - 5, 14, juce::Justification::centredRight);
    }

    // ── Curves + peak hold ────────────────────────────────────────────────
    const double sr = m_proc.getSampleRate();
    if (sr <= 0.0) return;

    const float binW = (float) (sr / PlugNspectrPostProcessor::kFftSize);

    const juce::Colour kPeakPost = PnsTheme::kColorPost.brighter (0.4f);
    const juce::Colour kPeakPre  = PnsTheme::kColorPreAvg;

    auto drawCurveAndPeaks = [&] (
        const std::array<float, PlugNspectrPostProcessor::kNumSpecBins>& spec,
        const std::array<float, PlugNspectrPostProcessor::kNumSpecBins>& peaks,
        juce::Colour curveColour,
        juce::Colour peakColour)
    {
        juce::Path curvePath, peakPath;
        bool curveStarted = false, peakStarted = false;

        for (int k = 1; k < PlugNspectrPostProcessor::kNumSpecBins; ++k)
        {
            const float freq = (float) k * binW;
            if (freq < kMinFreq) continue;
            if (freq > kMaxFreq) break;

            const float x = freqToX (freq);

            {
                const float db = magToDb (spec[k]);
                const float y  = dbToY (juce::jlimit (kMinDb, kMaxDb, db));
                if (!curveStarted) { curvePath.startNewSubPath (x, y); curveStarted = true; }
                else               { curvePath.lineTo (x, y); }
            }

            {
                const float db = magToDb (peaks[k]);
                if (db > kMinDb + 1.0f)
                {
                    const float y = dbToY (juce::jlimit (kMinDb, kMaxDb, db));
                    if (!peakStarted) { peakPath.startNewSubPath (x, y); peakStarted = true; }
                    else              { peakPath.lineTo (x, y); }
                }
                else if (peakStarted)
                {
                    peakPath.startNewSubPath (x, dbToY (kMinDb));
                    peakStarted = false;
                }
            }
        }

        if (curveStarted)
        {
            g.setColour (curveColour);
            g.strokePath (curvePath, juce::PathStrokeType (1.5f));
        }
        if (peakStarted)
        {
            g.setColour (peakColour);
            g.strokePath (peakPath, juce::PathStrokeType (1.0f));
        }
    };

    drawCurveAndPeaks (m_post, m_peakPost, PnsTheme::kColorPost, kPeakPost);
    drawCurveAndPeaks (m_pre,  m_peakPre,  PnsTheme::kColorPre,  kPeakPre);

    // ── Average EQ curves (5-bin smoothed rolling average) ────────────────
    if (m_showAvg)
    {
        constexpr int kSmoothHalf = 2;

        std::vector<juce::Point<float>> postPts, prePts;
        postPts.reserve (PlugNspectrPostProcessor::kNumSpecBins);
        prePts .reserve (PlugNspectrPostProcessor::kNumSpecBins);

        auto collectAvg = [&] (
            const std::array<float, PlugNspectrPostProcessor::kNumSpecBins>& avgBuf,
            std::vector<juce::Point<float>>& pts)
        {
            for (int k = 1; k < PlugNspectrPostProcessor::kNumSpecBins; ++k)
            {
                const float freq = (float) k * binW;
                if (freq < kMinFreq) continue;
                if (freq > kMaxFreq) break;
                float sum = 0.0f;
                int   cnt = 0;
                for (int j = k - kSmoothHalf; j <= k + kSmoothHalf; ++j)
                    if (j >= 1 && j < PlugNspectrPostProcessor::kNumSpecBins)
                        { sum += avgBuf[j]; ++cnt; }
                const float smoothed = (cnt > 0) ? sum / (float) cnt : avgBuf[k];
                const float db = magToDb (smoothed);
                pts.push_back ({ freqToX (freq),
                                 dbToY (juce::jlimit (kMinDb, kMaxDb, db)) });
            }
        };

        collectAvg (m_avgPost, postPts);
        collectAvg (m_avgPre,  prePts);

        // Fill region between post and pre averages (amber 60% opacity)
        if (!postPts.empty() && !prePts.empty())
        {
            juce::Path fillPath;
            fillPath.startNewSubPath (postPts.front());
            for (size_t i = 1; i < postPts.size(); ++i)
                fillPath.lineTo (postPts[i]);
            fillPath.lineTo (prePts.back());
            for (int i = (int) prePts.size() - 2; i >= 0; --i)
                fillPath.lineTo (prePts[(size_t) i]);
            fillPath.closeSubPath();
            g.setColour (PnsTheme::kColorPostAvg.withAlpha ((uint8_t) 0x99));
            g.fillPath (fillPath);
        }

        auto drawAvgLine = [&] (const std::vector<juce::Point<float>>& pts,
                                juce::Colour colour)
        {
            if (pts.empty()) return;
            juce::Path avgPath;
            avgPath.startNewSubPath (pts.front());
            for (size_t i = 1; i < pts.size(); ++i)
                avgPath.lineTo (pts[i]);
            g.setColour (colour);
            g.strokePath (avgPath, juce::PathStrokeType (2.0f));
        };

        drawAvgLine (prePts,  PnsTheme::kColorPreAvg);
        drawAvgLine (postPts, PnsTheme::kColorPostAvg);
    }

    // ── Legend — below the controls row (controls bottom = 6+22=28, legend top = 36) ──
    g.setFont (PnsTheme::fontLabel());
    constexpr float sw = 8.0f, sh = 8.0f, rowH = 13.0f;
    {
        // Panel geometry: right-aligned to same margin as the buttons (kPaddingMid from right)
        constexpr float kCtrlBottom = (float) (PnsTheme::kPaddingSmall + PnsTheme::kButtonHeight);  // 28
        const int numRows  = m_showAvg ? 4 : 2;
        const float panelW = 88.0f;
        const float panelH = (float) numRows * rowH + (float) (PnsTheme::kPaddingSmall * 2);
        const float panelR = (float) getWidth() - (float) PnsTheme::kPaddingMid;
        const float panelT = kCtrlBottom + 8.0f;
        PnsTheme::drawFrostedPanel (g, { panelR - panelW, panelT, panelW, panelH });
    }
    const float panelR = (float) getWidth() - (float) PnsTheme::kPaddingMid;
    const float panelT = (float) (PnsTheme::kPaddingSmall + PnsTheme::kButtonHeight) + 8.0f;
    const float lx = panelR - 88.0f + (float) PnsTheme::kPaddingSmall;
    const float ly = panelT          + (float) PnsTheme::kPaddingSmall;

    auto drawLegendRow = [&] (float row, juce::Colour swatch, const char* label)
    {
        const float ry = ly + row * rowH;
        g.setColour (swatch);
        g.fillRect (lx, ry, sw, sh);
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText (label,
                    juce::roundToInt (lx + sw + 4), juce::roundToInt (ry - 1),
                    60, 11, juce::Justification::centredLeft);
    };

    drawLegendRow (0.0f, PnsTheme::kColorPre,     "Pre");
    drawLegendRow (1.0f, PnsTheme::kColorPost,    "Post");
    if (m_showAvg)
    {
        drawLegendRow (2.0f, PnsTheme::kColorPreAvg,  "Pre Avg");
        drawLegendRow (3.0f, PnsTheme::kColorPostAvg, "Post Avg");
    }

    // ── Border ────────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);
}

//==============================================================================
// DynamicsView
//==============================================================================
DynamicsView::DynamicsView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    auto styleZoom = [&] (juce::TextButton& btn, bool active)
    {
        btn.setToggleable (true);
        btn.setToggleState (active, juce::dontSendNotification);
    };

    styleZoom (m_zoom6s,  false);
    styleZoom (m_zoom12s, true);   // default: 12s

    auto setZoom = [this] (int seconds)
    {
        m_zoomSeconds = seconds;
        m_zoom6s .setToggleState (seconds ==  6, juce::dontSendNotification);
        m_zoom12s.setToggleState (seconds == 12, juce::dontSendNotification);
    };

    m_zoom6s .onClick = [this, setZoom] { setZoom ( 6); };
    m_zoom12s.onClick = [this, setZoom] { setZoom (12); };

    addAndMakeVisible (m_zoom6s);
    addAndMakeVisible (m_zoom12s);

    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void DynamicsView::resized()
{
    const int W  = getWidth();
    constexpr int bw      = 28;
    constexpr int bh      = PnsTheme::kButtonHeight;
    constexpr int gap     = 2;
    constexpr int marginR = PnsTheme::kPaddingSmall;
    constexpr int marginT = PnsTheme::kPaddingSmall;
    m_zoom12s.setBounds (W - marginR - bw,           marginT, bw, bh);
    m_zoom6s .setBounds (W - marginR - bw * 2 - gap, marginT, bw, bh);
}

void DynamicsView::update()
{
    // ── Waveform sample buffer — gated to 30fps so scroll speed is constant
    if (++m_waveTickCounter % 2 == 0)
    {
        const auto cap = m_proc.getCapture();

        if (cap.captureCount != m_lastCaptureCount)
        {
            m_lastCaptureCount = cap.captureCount;

            const int n = juce::jmin (cap.post.getNumSamples(), kSampleBufLen);
            const float* postData = (cap.post.getNumChannels() > 0)
                                        ? cap.post.getReadPointer (0) : nullptr;
            const float* preData  = (cap.pre.getNumChannels() > 0)
                                        ? cap.pre.getReadPointer (0)  : nullptr;
            for (int i = 0; i < n; ++i)
            {
                m_postSamples[m_sampleWritePos] = postData ? postData[i] : 0.0f;
                m_preSamples [m_sampleWritePos] = preData  ? preData[i]  : 0.0f;
                m_sampleWritePos = (m_sampleWritePos + 1) % kSampleBufLen;
            }
            m_samplesStored = juce::jmin (m_samplesStored + n, kSampleBufLen);
        }
    }

    // ── Peak-based GR + rolling average + smoothed RMS volume ────────────
    {
        const auto cap2 = m_proc.getCapture();
        const int  n    = cap2.post.getNumSamples();

        float currentGr = 0.0f;

        if (n > 0
            && cap2.pre .getNumChannels() > 0
            && cap2.post.getNumChannels() > 0)
        {
            const float* prePtr  = cap2.pre .getReadPointer (0);
            const float* postPtr = cap2.post.getReadPointer (0);

            float prePeak = 0.0f, postPeak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                prePeak  = juce::jmax (prePeak,  std::abs (prePtr[i]));
                postPeak = juce::jmax (postPeak, std::abs (postPtr[i]));
            }

            if (prePeak > 1.0e-6f)
                currentGr = 20.0f * std::log10 (juce::jmax (postPeak, 1.0e-6f))
                          - 20.0f * std::log10 (prePeak);
        }

        const auto rms = m_proc.getRms();
        m_preConnected = rms.preValid;

        if (rms.preValid)
        {
            // Instantaneous hold: keep last non-zero reading
            if (currentGr < -0.1f)
                m_instantGr = currentGr;

            // Push current GR into 30s rolling average buffer
            m_avgGrBuf[m_avgGrPos] = currentGr;
            m_avgGrPos  = (m_avgGrPos + 1) % kAvgGrLen;
            m_avgGrFill = juce::jmin (m_avgGrFill + 1, kAvgGrLen);

            // Compute average of non-zero entries
            float sum = 0.0f;
            int   cnt = 0;
            for (int i = 0; i < m_avgGrFill; ++i)
                if (m_avgGrBuf[i] < -0.1f) { sum += m_avgGrBuf[i]; ++cnt; }
            m_avgGr = (cnt > 0) ? sum / (float) cnt : 0.0f;

            // Smooth RMS volume (0.9 decay)
            constexpr float kSmooth = 0.9f;
            m_smoothPreDb  = m_smoothPreDb  * kSmooth + rms.preDb  * (1.0f - kSmooth);
            m_smoothPostDb = m_smoothPostDb * kSmooth + rms.postDb * (1.0f - kSmooth);
        }

        m_gr[m_grPos] = rms.preValid
            ? juce::jlimit (-24.0f, 0.0f, currentGr)
            : 0.0f;
        m_grPos = (m_grPos + 1) % kGrLen;
    }
}

//──────────────────────────────────────────────────────────────────────────────
// Hit-test covers the frosted GR panel (Avg GR + Now rows).
static juce::Rectangle<int> getReadoutBounds (int compW)
{
    constexpr int kBtnBottom = PnsTheme::kPaddingSmall + PnsTheme::kButtonHeight;   // 28
    constexpr int kPanelW    = 132;
    constexpr int kGrRowsH   = 42;   // covers Avg GR + Now inside panel
    const int panelTop   = kBtnBottom + 8;                       // 36
    const int panelRight = compW - PnsTheme::kPaddingMid;
    const int panelLeft  = panelRight - kPanelW;
    return { panelLeft, panelTop, kPanelW, kGrRowsH };
}

void DynamicsView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (getReadoutBounds (getWidth()).contains (e.getPosition()))
    {
        m_instantGr  = 0.0f;
        m_avgGr      = 0.0f;
        m_avgGrFill  = 0;
        m_avgGrPos   = 0;
        m_avgGrBuf.fill (0.0f);
        m_grFlashEnd = juce::Time::getMillisecondCounterHiRes() + 200.0;
        repaint();
    }
}

void DynamicsView::mouseMove (const juce::MouseEvent& e)
{
    const bool over = getReadoutBounds (getWidth()).contains (e.getPosition());
    if (over != m_mouseOverReadout)
    {
        m_mouseOverReadout = over;
        repaint();
    }
}

void DynamicsView::mouseExit (const juce::MouseEvent&)
{
    if (m_mouseOverReadout)
    {
        m_mouseOverReadout = false;
        repaint();
    }
}

//──────────────────────────────────────────────────────────────────────────────
void DynamicsView::drawWaveform (juce::Graphics& g, juce::Rectangle<float> area)
{
    constexpr float kML = 42.0f, kMR = 12.0f, kMT = 20.0f, kMB = 18.0f;

    const float px = area.getX() + kML,  py = area.getY() + kMT;
    const float pw = area.getWidth()  - kML - kMR;
    const float ph = area.getHeight() - kMT - kMB;
    if (pw <= 0 || ph <= 0) return;

    const float midY = py + ph * 0.5f;
    auto ampToY = [&] (float a) -> float {
        return midY - juce::jlimit (-1.0f, 1.0f, a) * ph * 0.5f;
    };

    // ── Title ─────────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("WAVEFORM COMPARISON",
                (int) area.getX(), (int) area.getY(),
                (int) area.getWidth(), 16, juce::Justification::centred);

    // ── Background ────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (px, py, pw, ph);

    // ── Clip lines ────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kClipLine);
    g.drawHorizontalLine (juce::roundToInt (ampToY ( 1.0f)), px, px + pw);
    g.drawHorizontalLine (juce::roundToInt (ampToY (-1.0f)), px, px + pw);

    // ── Center line ───────────────────────────────────────────────────────
    g.setColour (PnsTheme::kZeroLine);
    g.drawHorizontalLine (juce::roundToInt (midY), px, px + pw);

    // ── Y axis labels ─────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kGridLabel);
    g.drawText ("+1", juce::roundToInt (area.getX()), juce::roundToInt (ampToY ( 1.0f)) - 6,
                juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);
    g.drawText ("0",  juce::roundToInt (area.getX()), juce::roundToInt (midY) - 6,
                juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);
    g.drawText ("-1", juce::roundToInt (area.getX()), juce::roundToInt (ampToY (-1.0f)) - 6,
                juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);

    // ── Smooth waveform paths — 120 columns, quadratic midpoint smoothing ──
    {
        const double sr = m_proc.getSampleRate();
        const int displaySamples = (sr > 0.0)
            ? juce::jmin ((int) (sr * (double) m_zoomSeconds), kSampleBufLen)
            : kSampleBufLen;
        const int available = juce::jmin (m_samplesStored, displaySamples);

        if (available > 0)
        {
            constexpr int kCols = 120;
            const float   colW  = pw / (float) kCols;
            const int     base  = (m_sampleWritePos - available + kSampleBufLen) % kSampleBufLen;

            std::array<float, kCols> preTopY, preBotY, postTopY, postBotY;

            for (int col = 0; col < kCols; ++col)
            {
                const int s0 = (int) ((float)  col      / (float) kCols * (float) available);
                const int s1 = juce::jmax (s0 + 1,
                               (int) ((float) (col + 1) / (float) kCols * (float) available));

                float preMin = 0.0f, preMax = 0.0f;
                float postMin = 0.0f, postMax = 0.0f;

                for (int s = s0; s < s1; ++s)
                {
                    const int   idx = (base + s) % kSampleBufLen;
                    const float ps  = m_preSamples [idx];
                    const float qs  = m_postSamples[idx];
                    if (ps < preMin)  preMin  = ps;
                    if (ps > preMax)  preMax  = ps;
                    if (qs < postMin) postMin = qs;
                    if (qs > postMax) postMax = qs;
                }

                preTopY [col] = ampToY (preMax);
                preBotY [col] = ampToY (preMin);
                postTopY[col] = ampToY (postMax);
                postBotY[col] = ampToY (postMin);
            }

            auto cx = [&] (int col) -> float {
                return px + (col + 0.5f) * colW;
            };

            auto buildPath = [&] (const std::array<float, kCols>& topY,
                                  const std::array<float, kCols>& botY) -> juce::Path
            {
                juce::Path p;
                p.startNewSubPath (px, topY[0]);
                p.lineTo (cx (0), topY[0]);
                for (int i = 1; i < kCols; ++i)
                {
                    const float mx = (cx (i - 1) + cx (i)) * 0.5f;
                    const float my = (topY[i - 1] + topY[i]) * 0.5f;
                    p.quadraticTo (cx (i - 1), topY[i - 1], mx, my);
                }
                p.lineTo (cx (kCols - 1), topY[kCols - 1]);
                p.lineTo (px + pw, topY[kCols - 1]);
                p.lineTo (px + pw, botY[kCols - 1]);
                p.lineTo (cx (kCols - 1), botY[kCols - 1]);
                for (int i = kCols - 2; i >= 0; --i)
                {
                    const float mx = (cx (i) + cx (i + 1)) * 0.5f;
                    const float my = (botY[i] + botY[i + 1]) * 0.5f;
                    p.quadraticTo (cx (i + 1), botY[i + 1], mx, my);
                }
                p.lineTo (cx (0), botY[0]);
                p.lineTo (px, botY[0]);
                p.closeSubPath();
                return p;
            };

            // PRE: lavender at 50% opacity
            g.setColour (PnsTheme::kColorPre.withAlpha ((uint8_t) 0x80));
            g.fillPath (buildPath (preTopY, preBotY));

            // POST: teal at 90% opacity
            g.setColour (PnsTheme::kColorPost.withAlpha ((uint8_t) 0xe6));
            g.fillPath (buildPath (postTopY, postBotY));

            // Compression difference fill: subtle teal at 15%
            g.setColour (PnsTheme::kColorPost.withAlpha ((uint8_t) 0x26));
            for (int col = 0; col < kCols; ++col)
            {
                const float x = px + (float) col * colW;
                if (postTopY[col] > preTopY[col])
                    g.fillRect (x, preTopY[col], colW, postTopY[col] - preTopY[col]);
                if (preBotY[col] > postBotY[col])
                    g.fillRect (x, postBotY[col], colW, preBotY[col] - postBotY[col]);
            }
        }
    }

    // ── Readouts: Avg GR / Now / VOLUME ──────────────────────────────────
    {
        auto grStr = [] (float v) -> juce::String {
            return (v <= -0.1f) ? (juce::String (v, 1) + " dB") : "0.0 dB";
        };
        auto dbStr = [] (float v) -> juce::String {
            if (v <= -89.0f) return "---";
            return juce::String (v, 1) + " dB";
        };

        const bool flashing = (juce::Time::getMillisecondCounterHiRes() < m_grFlashEnd);

        // Panel geometry — top sits 8px below the zoom buttons (bottom edge at 28px)
        constexpr int kBtnBottom = PnsTheme::kPaddingSmall + PnsTheme::kButtonHeight;  // 28
        constexpr int kPanelW    = 132;   // 120 content + 6px padding each side
        constexpr int kPanelH    = 123;   // fits all rows with padding
        const int     panelTop   = kBtnBottom + 8;
        const int     panelRight = juce::roundToInt (area.getRight()) - PnsTheme::kPaddingMid;
        const int     panelLeft  = panelRight - kPanelW;

        PnsTheme::drawFrostedPanel (g, juce::Rectangle<float> (
            (float) panelLeft, (float) panelTop,
            (float) kPanelW,   (float) kPanelH));

        const int lx = panelLeft  + PnsTheme::kPaddingSmall;
        const int rx = panelRight - PnsTheme::kPaddingSmall;
        constexpr int lw = 46;
        constexpr int vw = 70;
        const int vx = rx - vw;

        constexpr int rowH = 14;
        const int row0 = panelTop + PnsTheme::kPaddingSmall;   // 42
        const int row1 = row0 + 17;

        // ── Avg GR ────────────────────────────────────────────────────────
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Avg GR", lx, row0, lw, 12, juce::Justification::centredLeft);

        g.setFont (PnsTheme::fontReadout());
        g.setColour (flashing ? juce::Colours::white : PnsTheme::kColorPostAvg);
        g.drawText (grStr (m_avgGr), vx, row0 - 1, vw, rowH, juce::Justification::centredRight);

        // ── Now ───────────────────────────────────────────────────────────
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Now", lx, row1, lw, 12, juce::Justification::centredLeft);

        g.setFont (PnsTheme::fontReadout());
        g.setColour (flashing ? juce::Colours::white : PnsTheme::kTextAccent);
        g.drawText (grStr (m_instantGr), vx, row1 - 1, vw, rowH, juce::Justification::centredRight);

        // ── Hover tooltip ─────────────────────────────────────────────────
        if (m_mouseOverReadout)
        {
            g.setFont (PnsTheme::fontLabel());
            g.setColour (PnsTheme::kGridLabel);
            g.drawText ("Double-click to reset",
                        lx, row1 + 14, rx - lx, 11, juce::Justification::centredRight);
        }

        // ── Divider ───────────────────────────────────────────────────────
        const int divY = row1 + 28;
        g.setColour (PnsTheme::kBorderSubtle);
        g.drawHorizontalLine (divY, (float) lx, (float) rx);

        // ── VOLUME section ────────────────────────────────────────────────
        const int vrow0    = divY + 5;
        const int dataRow0 = vrow0 + 13;
        const int dataRow1 = dataRow0 + 17;
        const int dataRow2 = dataRow1 + 17;

        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("VOLUME", lx, vrow0, 50, 11, juce::Justification::centredLeft);

        // In
        g.drawText ("In", lx, dataRow0, lw, 12, juce::Justification::centredLeft);
        g.setFont (PnsTheme::fontReadout());
        g.setColour (PnsTheme::kTextPrimary);
        g.drawText (dbStr (m_smoothPreDb), vx, dataRow0 - 1, vw, rowH, juce::Justification::centredRight);

        // Out
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Out", lx, dataRow1, lw, 12, juce::Justification::centredLeft);
        g.setFont (PnsTheme::fontReadout());
        g.setColour (PnsTheme::kTextPrimary);
        g.drawText (dbStr (m_smoothPostDb), vx, dataRow1 - 1, vw, rowH, juce::Justification::centredRight);

        // Δ — colour depends on sign
        const float delta = m_smoothPostDb - m_smoothPreDb;
        const juce::Colour deltaCol = (delta < -0.1f) ? PnsTheme::kColorGainRed
                                    : (delta >  0.1f) ? PnsTheme::kAccentPrimary
                                                      : PnsTheme::kTextSecondary;
        juce::String deltaStr = (delta >= 0.0f ? "+" : "") + juce::String (delta, 1) + " dB";
        if (m_smoothPreDb <= -89.0f) deltaStr = "---";

        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText (juce::String::fromUTF8 ("\xce\x94"), lx, dataRow2, lw, 12, juce::Justification::centredLeft);
        g.setFont (PnsTheme::fontReadout());
        g.setColour (deltaCol);
        g.drawText (deltaStr, vx, dataRow2 - 1, vw, rowH, juce::Justification::centredRight);
    }

    // ── Border ────────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);
}

//──────────────────────────────────────────────────────────────────────────────
void DynamicsView::drawGrMeter (juce::Graphics& g, juce::Rectangle<float> area)
{
    constexpr float kML = 42.0f, kMR = 12.0f, kMT = 20.0f, kMB = 20.0f;
    constexpr float kMinGr = -24.0f, kMaxGr = 0.0f;

    const float px = area.getX() + kML,  py = area.getY() + kMT;
    const float pw = area.getWidth()  - kML - kMR;
    const float ph = area.getHeight() - kMT - kMB;
    if (pw <= 0 || ph <= 0) return;

    auto grToY = [&] (float gr) -> float {
        return py + ph * (-juce::jlimit (kMinGr, kMaxGr, gr)) / (-kMinGr);
    };

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("GAIN REDUCTION",
                (int) area.getX(), (int) area.getY(),
                (int) area.getWidth(), 16, juce::Justification::centred);

    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (px, py, pw, ph);

    const float grLines[] = { 0.0f, -3.0f, -6.0f, -9.0f, -12.0f, -15.0f, -18.0f, -21.0f, -24.0f };
    for (float gr : grLines)
    {
        const float gy = grToY (gr);
        g.setColour (gr == 0.0f ? PnsTheme::kZeroLine : PnsTheme::kGridLine);
        g.drawHorizontalLine (juce::roundToInt (gy), px, px + pw);

        juce::String lbl = (gr == 0.0f) ? "0" : juce::String (juce::roundToInt (gr));
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (lbl, juce::roundToInt (area.getX()),
                    juce::roundToInt (gy) - 6,
                    juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);
    }

    g.setColour (PnsTheme::kGridLabel);
    g.drawText ("-3s", juce::roundToInt (px), juce::roundToInt (py + ph) + 2,
                28, 12, juce::Justification::centredLeft);
    g.drawText ("0",   juce::roundToInt (px + pw) - 14, juce::roundToInt (py + ph) + 2,
                28, 12, juce::Justification::centredRight);

    // Filled GR history
    const float zeroY = grToY (0.0f);
    juce::Path fillPath;
    fillPath.startNewSubPath (px, zeroY);
    for (int i = 0; i < kGrLen; ++i)
    {
        const int   idx = (m_grPos + i) % kGrLen;
        const float x   = px + pw * (float) i / (float) (kGrLen - 1);
        fillPath.lineTo (x, grToY (m_gr[idx]));
    }
    fillPath.lineTo (px + pw, zeroY);
    fillPath.closeSubPath();
    g.setColour (PnsTheme::kColorGainRed.withAlpha ((uint8_t) 0x88));
    g.fillPath (fillPath);

    // Outline curve
    juce::Path curvePath;
    for (int i = 0; i < kGrLen; ++i)
    {
        const int   idx = (m_grPos + i) % kGrLen;
        const float x   = px + pw * (float) i / (float) (kGrLen - 1);
        const float y   = grToY (m_gr[idx]);
        if (i == 0) curvePath.startNewSubPath (x, y);
        else        curvePath.lineTo (x, y);
    }
    g.setColour (PnsTheme::kColorGainRed);
    g.strokePath (curvePath, juce::PathStrokeType (1.5f));

    // "No Pre signal" overlay
    if (!m_preConnected)
    {
        g.setColour (PnsTheme::kBgDark.withAlpha ((uint8_t) 0xaa));
        g.fillRect (px, py, pw, ph);
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("No Pre signal — insert PlugNspectrPre before this plugin",
                    juce::roundToInt (px), juce::roundToInt (py + ph * 0.5f - 8),
                    juce::roundToInt (pw), 16, juce::Justification::centred);
    }

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);
}

//──────────────────────────────────────────────────────────────────────────────
void DynamicsView::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);

    const float H      = (float) getHeight();
    const float W      = (float) getWidth();
    const float splitY = H * 0.55f;
    const float gap    = 6.0f;

    drawWaveform (g, { 0.0f, 0.0f, W, splitY });
    drawGrMeter  (g, { 0.0f, splitY + gap, W, H - splitY - gap });
}

//==============================================================================
// OscilloscopeView
//==============================================================================
OscilloscopeView::OscilloscopeView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    auto styleBtn = [&] (juce::TextButton& btn, bool active)
    {
        btn.setToggleable (true);
        btn.setToggleState (active, juce::dontSendNotification);
    };

    styleBtn (m_btn10ms,  false);
    styleBtn (m_btn50ms,  true);   // default: 50ms
    styleBtn (m_btn100ms, false);

    auto setWindow = [this] (int ms)
    {
        m_windowMs    = ms;
        m_hasDisplay  = false;
        m_searchFrom  = m_totalWritten;
        m_btn10ms .setToggleState (ms ==  10, juce::dontSendNotification);
        m_btn50ms .setToggleState (ms ==  50, juce::dontSendNotification);
        m_btn100ms.setToggleState (ms == 100, juce::dontSendNotification);
    };

    m_btn10ms .onClick = [this, setWindow] { setWindow ( 10); };
    m_btn50ms .onClick = [this, setWindow] { setWindow ( 50); };
    m_btn100ms.onClick = [this, setWindow] { setWindow (100); };

    addAndMakeVisible (m_btn10ms);
    addAndMakeVisible (m_btn50ms);
    addAndMakeVisible (m_btn100ms);
}

void OscilloscopeView::resized()
{
    const int W  = getWidth();
    constexpr int bw      = 36;
    constexpr int bh      = PnsTheme::kButtonHeight;
    constexpr int gap     = 2;
    constexpr int marginR = PnsTheme::kPaddingSmall;
    constexpr int marginT = PnsTheme::kPaddingSmall;
    m_btn100ms.setBounds (W - marginR - bw,               marginT, bw, bh);
    m_btn50ms .setBounds (W - marginR - bw * 2 - gap,     marginT, bw, bh);
    m_btn10ms .setBounds (W - marginR - bw * 3 - gap * 2, marginT, bw, bh);
}

//──────────────────────────────────────────────────────────────────────────────
float OscilloscopeView::ringGet (const std::array<float, kRingLen>& buf,
                                 uint64_t idx) const
{
    const uint64_t oldest = m_totalWritten - (uint64_t) m_ringAvail;
    if (idx < oldest || idx >= m_totalWritten) return 0.0f;
    const int offset = (int) (idx - oldest);
    const int pos    = ((m_ringWrite - m_ringAvail + offset) % kRingLen + kRingLen) % kRingLen;
    return buf[pos];
}

//──────────────────────────────────────────────────────────────────────────────
void OscilloscopeView::update()
{
    // ── Accumulate samples from processor into ring buffer ────────────────
    {
        const auto cap = m_proc.getCapture();
        if (cap.captureCount != m_lastCaptureCount)
        {
            m_lastCaptureCount = cap.captureCount;
            const int n = cap.post.getNumSamples();
            const float* prePtr  = (cap.pre .getNumChannels() > 0) ? cap.pre .getReadPointer (0) : nullptr;
            const float* postPtr = (cap.post.getNumChannels() > 0) ? cap.post.getReadPointer (0) : nullptr;

            for (int i = 0; i < n; ++i)
            {
                m_ringPre [m_ringWrite] = prePtr  ? prePtr [i] : 0.0f;
                m_ringPost[m_ringWrite] = postPtr ? postPtr[i] : 0.0f;
                m_ringWrite = (m_ringWrite + 1) % kRingLen;
                if (m_ringAvail < kRingLen) ++m_ringAvail;
            }
            m_totalWritten += (uint64_t) n;
        }
    }

    // ── Trigger search ────────────────────────────────────────────────────
    const double sr = m_proc.getSampleRate();
    if (sr <= 0.0 || m_ringAvail == 0) return;

    const int captureTarget = juce::jmin ((int) (sr * m_windowMs / 1000.0), kMaxCapture);
    if (captureTarget <= 1) return;

    if (m_totalWritten < (uint64_t) captureTarget) return;
    const uint64_t searchEnd = m_totalWritten - (uint64_t) captureTarget;

    const uint64_t oldest      = m_totalWritten - (uint64_t) m_ringAvail;
    const uint64_t searchStart = juce::jmax (m_searchFrom, oldest + 1);

    if (searchStart >= searchEnd) return;

    for (uint64_t i = searchStart; i < searchEnd; ++i)
    {
        const float prev = ringGet (m_ringPost, i - 1);
        const float curr = ringGet (m_ringPost, i);

        // Rising zero-crossing above minimum amplitude threshold
        if (prev < 0.0f && curr >= 0.0f && curr > 0.01f)
        {
            for (int s = 0; s < captureTarget; ++s)
            {
                m_displayPre [s] = ringGet (m_ringPre,  i + (uint64_t) s);
                m_displayPost[s] = ringGet (m_ringPost, i + (uint64_t) s);
            }
            m_displayCount = captureTarget;
            m_hasDisplay   = true;
            m_searchFrom   = i + (uint64_t) captureTarget;
            break;
        }
    }
}

//──────────────────────────────────────────────────────────────────────────────
void OscilloscopeView::paint (juce::Graphics& g)
{
    constexpr float kML = 42.0f, kMR = 12.0f, kMT = 28.0f, kMB = 18.0f;

    const float W = (float) getWidth(),  H = (float) getHeight();
    const float px = kML,          py = kMT;
    const float pw = W - kML - kMR, ph = H - kMT - kMB;

    const float midY = py + ph * 0.5f;
    auto ampToY = [&] (float a) -> float {
        return midY - juce::jlimit (-1.0f, 1.0f, a) * ph * 0.5f;
    };

    // ── Background ────────────────────────────────────────────────────────
    g.fillAll (PnsTheme::kBgDark);
    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (px, py, pw, ph);

    // ── Title ─────────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("OSCILLOSCOPE",
                (int) px, 0, (int) pw, (int) kMT, juce::Justification::centred);

    // ── Grid ──────────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kClipLine);
    g.drawHorizontalLine (juce::roundToInt (ampToY ( 1.0f)), px, px + pw);
    g.drawHorizontalLine (juce::roundToInt (ampToY (-1.0f)), px, px + pw);

    g.setColour (PnsTheme::kGridLine);
    g.drawHorizontalLine (juce::roundToInt (ampToY ( 0.5f)), px, px + pw);
    g.drawHorizontalLine (juce::roundToInt (ampToY (-0.5f)), px, px + pw);

    g.setColour (PnsTheme::kZeroLine);
    g.drawHorizontalLine (juce::roundToInt (midY), px, px + pw);

    g.setColour (PnsTheme::kGridLine);
    for (int q = 1; q <= 3; ++q)
        g.drawVerticalLine (juce::roundToInt (px + pw * (float) q / 4.0f), py, py + ph);

    // ── Y axis labels ─────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kGridLabel);
    g.drawText ("+1",  0, juce::roundToInt (ampToY ( 1.0f)) - 6, juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);
    g.drawText ("+.5", 0, juce::roundToInt (ampToY ( 0.5f)) - 6, juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);
    g.drawText ("0",   0, juce::roundToInt (midY)           - 6, juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);
    g.drawText ("-.5", 0, juce::roundToInt (ampToY (-0.5f)) - 6, juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);
    g.drawText ("-1",  0, juce::roundToInt (ampToY (-1.0f)) - 6, juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);

    // ── Waveforms ─────────────────────────────────────────────────────────
    if (m_hasDisplay && m_displayCount > 1)
    {
        const int drawPts = juce::jmin (m_displayCount, juce::jmax (2, (int) pw));
        const float step  = (float) m_displayCount / (float) drawPts;

        auto buildPath = [&] (const std::array<float, kMaxCapture>& buf) -> juce::Path
        {
            juce::Path p;
            for (int i = 0; i < drawPts; ++i)
            {
                const float fIdx = i * step;
                const int   lo   = (int) fIdx;
                const int   hi   = juce::jmin (lo + 1, m_displayCount - 1);
                const float t    = fIdx - (float) lo;
                const float val  = buf[lo] + t * (buf[hi] - buf[lo]);
                const float x    = px + pw * (float) i / (float) (drawPts - 1);
                const float y    = ampToY (val);
                if (i == 0) p.startNewSubPath (x, y);
                else        p.lineTo (x, y);
            }
            return p;
        };

        // PRE — light lavender, 1.5px
        g.setColour (PnsTheme::kColorPreAvg);
        g.strokePath (buildPath (m_displayPre),  juce::PathStrokeType (1.5f));

        // POST — teal, 2px (on top)
        g.setColour (PnsTheme::kColorPost);
        g.strokePath (buildPath (m_displayPost), juce::PathStrokeType (2.0f));
    }
    else
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kGridLine);
        g.drawText ("Waiting for trigger...",
                    juce::roundToInt (px), juce::roundToInt (midY - 8),
                    juce::roundToInt (pw), 16, juce::Justification::centred);
    }

    // ── Legend ────────────────────────────────────────────────────────────
    {
        constexpr float sw = 9.0f, sh = 7.0f, rowH = 12.0f;
        const float lx = px + 6.0f,  ly = py + 6.0f;
        // Frosted panel: 52px wide, 31px tall (sw+gap+text=40 + 12 padding; 2*rowH+sh-rowH+12=31)
        PnsTheme::drawFrostedPanel (g, juce::Rectangle<float> (px, py + 2.0f, 52.0f, 31.0f));
        g.setFont (PnsTheme::fontLabel());

        g.setColour (PnsTheme::kColorPreAvg);
        g.fillRect (lx, ly, sw, sh);
        g.setColour (PnsTheme::kTextPrimary);
        g.drawText ("Pre",  juce::roundToInt (lx + sw + 3), juce::roundToInt (ly - 1), 28, 10, juce::Justification::centredLeft);

        g.setColour (PnsTheme::kColorPost);
        g.fillRect (lx, ly + rowH, sw, sh);
        g.setColour (PnsTheme::kTextPrimary);
        g.drawText ("Post", juce::roundToInt (lx + sw + 3), juce::roundToInt (ly + rowH - 1), 28, 10, juce::Justification::centredLeft);
    }

    // ── SR readout — sits 8px below the time buttons (button bottom = 28px) ─
    {
        const double sr = m_proc.getSampleRate();
        const juce::String srStr = (sr > 0.0)
            ? ("SR: " + juce::String ((int) sr) + " Hz")
            : "SR: --";
        constexpr int kPanelW  = 104;
        constexpr int kPanelH  = 23;
        constexpr int kPanelT  = PnsTheme::kPaddingSmall + PnsTheme::kButtonHeight + 8;   // 36
        const int panelRight = juce::roundToInt (W) - PnsTheme::kPaddingMid;
        PnsTheme::drawFrostedPanel (g, juce::Rectangle<float> (
            (float) (panelRight - kPanelW), (float) kPanelT,
            (float) kPanelW, (float) kPanelH));
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextAccent);
        g.drawText (srStr,
                    panelRight - kPanelW + PnsTheme::kPaddingSmall,
                    kPanelT + PnsTheme::kPaddingSmall,
                    kPanelW - PnsTheme::kPaddingSmall * 2, 11,
                    juce::Justification::centredRight);
    }

    // ── Border ────────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);
}

//==============================================================================
// HarmonicsView
//==============================================================================
HarmonicsView::HarmonicsView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    openCmdMemory();

    // Frequency slider — logarithmic rotary
    m_freqSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    m_freqSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    m_freqSlider.setRange (100.0, 8000.0, 1.0);
    m_freqSlider.setSkewFactorFromMidPoint (900.0);
    m_freqSlider.setValue (m_toneFreq, juce::dontSendNotification);
    m_freqSlider.addListener (this);
    addAndMakeVisible (m_freqSlider);

    // Test tone toggle button
    m_toneBtn.setToggleable (true);
    m_toneBtn.setToggleState (false, juce::dontSendNotification);
    m_toneBtn.onClick = [this]
    {
        m_toneActive = !m_toneActive;
        m_toneBtn.setToggleState (m_toneActive, juce::dontSendNotification);
        writeCmdBlock();
    };
    addAndMakeVisible (m_toneBtn);
}

HarmonicsView::~HarmonicsView()
{
    m_freqSlider.removeListener (this);
    // Zero cmd block before closing so Pre knows test tone is off
    if (m_pCmd != nullptr)
    {
        m_pCmd->testToneActive    = 0;
        m_pCmd->testToneFrequency = 1000.0;
    }
    closeCmdMemory();
}

//──────────────────────────────────────────────────────────────────────────────
void HarmonicsView::openCmdMemory()
{
    if (m_pCmd != nullptr) return;

    // Post creates and owns this mapping; Pre opens it read-only.
    HANDLE hMap = CreateFileMappingA (INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0, kPNS_CmdMemBytes,
                                      kPNS_CmdMemName);
    if (hMap == nullptr || hMap == INVALID_HANDLE_VALUE) return;

    m_hCmdFile = hMap;
    m_pCmd = static_cast<PNS_CmdBlock*> (
        MapViewOfFile (m_hCmdFile, FILE_MAP_ALL_ACCESS, 0, 0, kPNS_CmdMemBytes));

    if (m_pCmd == nullptr)
    {
        CloseHandle (m_hCmdFile);
        m_hCmdFile = nullptr;
        return;
    }

    // Initialise to safe defaults
    m_pCmd->testToneActive    = 0;
    m_pCmd->testToneFrequency = m_toneFreq;
}

void HarmonicsView::closeCmdMemory()
{
    if (m_pCmd != nullptr) { UnmapViewOfFile (m_pCmd);  m_pCmd = nullptr; }
    if (m_hCmdFile != nullptr) { CloseHandle (m_hCmdFile); m_hCmdFile = nullptr; }
}

void HarmonicsView::writeCmdBlock()
{
    if (m_pCmd == nullptr) return;
    m_pCmd->testToneFrequency = m_toneFreq;
    m_pCmd->testToneActive    = m_toneActive ? 1u : 0u;
}

//──────────────────────────────────────────────────────────────────────────────
void HarmonicsView::sliderValueChanged (juce::Slider*)
{
    m_toneFreq = m_freqSlider.getValue();
    writeCmdBlock();
    repaint();   // rescale X axis immediately
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
void HarmonicsView::resized()
{
    constexpr int knobSize = 52;
    constexpr int btnW     = 80;
    constexpr int btnH     = PnsTheme::kButtonHeight;
    constexpr int pad      = PnsTheme::kPaddingMid;

    m_freqSlider.setBounds (pad, pad, knobSize, knobSize);
    m_toneBtn   .setBounds (pad + knobSize + 8, pad + (knobSize - btnH) / 2, btnW, btnH);
}

//──────────────────────────────────────────────────────────────────────────────
void HarmonicsView::update()
{
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
        const int   searchRadius = 3;
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

    if (!m_toneActive)
    {
        // Inactive state — full-area message
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Activate test tone to begin analysis",
                    getLocalBounds(), juce::Justification::centred);

        // Still draw controls so user can find and click them
        return;
    }

    // ── Split area: spectrum left, readouts right ─────────────────────────
    constexpr int kReadoutW = 160;
    const float W = (float) getWidth(), H = (float) getHeight();
    const auto specArea    = juce::Rectangle<float> (0, 0, W - kReadoutW, H);
    const auto readoutArea = juce::Rectangle<float> (W - kReadoutW, 0, (float) kReadoutW, H);

    drawSpectrumArea (g, specArea);
    drawReadouts     (g, readoutArea);
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
    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (px, py, pw, ph);

    // Title
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("HARMONIC SPECTRUM",
                (int) area.getX(), (int) area.getY(), (int) area.getWidth(), (int) kMT,
                juce::Justification::centred);

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
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kColorPostAvg, 1.0f/6.0f),
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kColorPostAvg, 2.0f/6.0f),
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kColorPostAvg, 3.0f/6.0f),
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kColorPostAvg, 4.0f/6.0f),
        PnsTheme::kColorPost.interpolatedWith (PnsTheme::kColorPostAvg, 5.0f/6.0f),
        PnsTheme::kColorPostAvg,
        PnsTheme::kColorPostAvg,
    };

    for (int h = 0; h < kNumH; ++h)
    {
        const float freq = fund * (float) (h + 1);
        if (freq < xMin || freq > xMax) continue;
        const float x = freqToX (freq);
        g.setColour (PnsTheme::kBorderSubtle);
        g.drawVerticalLine (juce::roundToInt (x), py, py + ph);
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kGridLabel);
        g.drawText ("H" + juce::String (h + 1),
                    juce::roundToInt (x) - 10, juce::roundToInt (py) - (int) kMT + 2,
                    20, 12, juce::Justification::centred);
        (void) kHarmCols;
    }

    // PRE spectrum — lavender 1px
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
        if (started) { g.setColour (PnsTheme::kColorPre); g.strokePath (p, juce::PathStrokeType (1.0f)); }
    }

    // POST spectrum — teal 2px
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
        if (started) { g.setColour (PnsTheme::kColorPost); g.strokePath (p, juce::PathStrokeType (2.0f)); }
    }

    // Harmonic peak dots on POST (H2-H8) + hover tooltip
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
        const float t     = (float) (h - 1) / 6.0f;
        const juce::Colour col = PnsTheme::kColorPost.interpolatedWith (PnsTheme::kColorPostAvg, t);

        // Diamond
        const float ds = 5.0f;
        juce::Path diamond;
        diamond.addTriangle (x, y - ds, x + ds, y, x, y + ds);
        diamond.addTriangle (x, y - ds, x - ds, y, x, y + ds);
        g.setColour (col);
        g.fillPath (diamond);

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

    // Frequency readout below knob (top-left control area)
    {
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kGridLabel);
        g.drawText ("FREQ", PnsTheme::kPaddingMid, PnsTheme::kPaddingMid - 2, 52, 10,
                    juce::Justification::centred);
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextAccent);
        const juce::String freqStr = (m_toneFreq >= 1000.0)
            ? juce::String (m_toneFreq / 1000.0, 2) + " kHz"
            : juce::String (juce::roundToInt ((float) m_toneFreq)) + " Hz";
        g.drawText (freqStr, PnsTheme::kPaddingMid, PnsTheme::kPaddingMid + 52, 52, 12,
                    juce::Justification::centred);
    }

    // Warning label
    {
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kColorGainRed);
        g.drawText (juce::String::fromUTF8 ("\xe2\x9a\xa0") + " Replaces audio output",
                    PnsTheme::kPaddingMid + 52 + 8 + 80 + 8, PnsTheme::kPaddingMid + (52 - 10) / 2,
                    200, 12, juce::Justification::centredLeft);
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
    g.drawText ("THD", lx, y, 30, 11, juce::Justification::centredLeft);
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
    g.drawText ("HARMONICS (dB)", lx, y, rx - lx, 11, juce::Justification::centredLeft);
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

//==============================================================================
// PlugNspectrPostEditor
//==============================================================================
PlugNspectrPostEditor::PlugNspectrPostEditor (PlugNspectrPostProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      m_specView (p),
      m_dynView  (p),
      m_oscView  (p),
      m_harmView (p)
{
    setLookAndFeel (&m_laf);

    m_biltroyLogo = juce::ImageCache::getFromMemory (BinaryData::BiltroyAudio_x09mxix09mxix09m_png,
                                                     BinaryData::BiltroyAudio_x09mxix09mxix09m_pngSize);

    for (auto* btn : { &m_tabSpectrum, &m_tabDynamics, &m_tabOscilloscope, &m_tabHarmonics })
    {
        btn->getProperties().set ("tabButton", true);
        addAndMakeVisible (btn);
    }

    m_tabSpectrum    .onClick = [this] { switchTab (0); };
    m_tabDynamics    .onClick = [this] { switchTab (1); };
    m_tabOscilloscope.onClick = [this] { switchTab (2); };
    m_tabHarmonics   .onClick = [this] { switchTab (3); };

    addAndMakeVisible (m_specView);
    addAndMakeVisible (m_dynView);
    addAndMakeVisible (m_oscView);
    addAndMakeVisible (m_harmView);

    switchTab (0);

    setSize (800, 540);
    setResizable (true, true);
    setResizeLimits (600, 450, 2000, 1400);
    startTimerHz (60);
}

PlugNspectrPostEditor::~PlugNspectrPostEditor()
{
    setLookAndFeel (nullptr);
    stopTimer();
}

//==============================================================================
void PlugNspectrPostEditor::timerCallback()
{
    // Dynamics data + repaint at full 60 fps.
    m_dynView.update();
    if (m_activeTab == 1) m_dynView.repaint();

    // Spectrum, Oscilloscope, Harmonics at 30 fps.
    if (++m_tickCounter % 2 == 0)
    {
        m_specView.update();
        if (m_activeTab == 0) m_specView.repaint();

        m_oscView.update();
        if (m_activeTab == 2) m_oscView.repaint();

        m_harmView.update();
        if (m_activeTab == 3) m_harmView.repaint();
    }
}

//==============================================================================
void PlugNspectrPostEditor::switchTab (int index)
{
    m_activeTab = index;

    juce::TextButton* tabs[4] = { &m_tabSpectrum, &m_tabDynamics,
                                   &m_tabOscilloscope, &m_tabHarmonics };
    for (int i = 0; i < 4; ++i)
    {
        const bool active = (i == index);
        tabs[i]->setColour (juce::TextButton::textColourOffId,
                            active ? PnsTheme::kTextPrimary : PnsTheme::kTextSecondary);
        tabs[i]->getProperties().set ("tabActive", active);
        tabs[i]->repaint();
    }

    m_specView.setVisible  (index == 0);
    m_dynView .setVisible  (index == 1);
    m_oscView .setVisible  (index == 2);
    m_harmView.setVisible  (index == 3);

    repaint();
}

//==============================================================================
void PlugNspectrPostEditor::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);

    // ── Header bar ────────────────────────────────────────────────────────
    constexpr int hH = PnsTheme::kHeaderHeight;
    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (0, 0, getWidth(), hH);
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawHorizontalLine (hH - 1, 0.0f, (float) getWidth());

    // Logo + plugin name
    constexpr int marginL  = PnsTheme::kPaddingMid;
    constexpr int marginR2 = PnsTheme::kPaddingMid;

    int textStartX = marginL;

    if (m_biltroyLogo.isValid())
    {
        const float logoH = (float) hH - 8.0f;
        const float logoW = logoH * ((float) m_biltroyLogo.getWidth()
                                   / (float) m_biltroyLogo.getHeight());
        g.drawImage (m_biltroyLogo,
                     marginL, 4, (int) logoW, (int) logoH,
                     0, 0, m_biltroyLogo.getWidth(), m_biltroyLogo.getHeight());
        textStartX = marginL + (int) logoW + PnsTheme::kPaddingMid;
    }

    // Kerned title — draw each character with 3px extra spacing
    {
        const juce::Font titleFont = juce::Font (juce::FontOptions()
            .withName (juce::Font::getDefaultSansSerifFontName())
            .withHeight (18.0f));
        g.setFont (titleFont);
        g.setColour (PnsTheme::kTextPrimary);
        const juce::String titleStr = "PlugNspectr";
        constexpr float kExtraKern = 3.0f;
        const float baselineY = (float) hH * 0.5f + titleFont.getAscent() * 0.5f - 1.0f;
        float tx = (float) textStartX;
        for (int i = 0; i < titleStr.length(); ++i)
        {
            const juce::String ch = titleStr.substring (i, i + 1);
            g.drawSingleLineText (ch, juce::roundToInt (tx), juce::roundToInt (baselineY));
            // Use GlyphArrangement to measure char width without deprecated API
            juce::GlyphArrangement ga;
            ga.addLineOfText (titleFont, ch, 0.0f, 0.0f);
            tx += (ga.getNumGlyphs() > 0 ? ga.getBoundingBox (0, 1, true).getWidth() : 8.0f)
                  + kExtraKern;
        }
    }

    // Version — far right with right margin
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("v1.0",
                getWidth() - marginR2 - 40, (hH - 12) / 2, 40, 12,
                juce::Justification::centredRight);

    // ── Tab bar bottom border ─────────────────────────────────────────────
    constexpr int tbBottom = hH + PnsTheme::kTabBarHeight;
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawHorizontalLine (tbBottom - 1, 0.0f, (float) getWidth());
}

//==============================================================================
void PlugNspectrPostEditor::resized()
{
    constexpr int hH     = PnsTheme::kHeaderHeight;
    constexpr int tbH    = PnsTheme::kTabBarHeight;
    constexpr int kTabW  = 90;
    constexpr int kTabWO = 110;   // wider for "Oscilloscope"

    m_tabSpectrum    .setBounds (0,                   hH, kTabW,  tbH);
    m_tabDynamics    .setBounds (kTabW,               hH, kTabW,  tbH);
    m_tabOscilloscope.setBounds (kTabW * 2,           hH, kTabWO, tbH);
    m_tabHarmonics   .setBounds (kTabW * 2 + kTabWO,  hH, kTabW,  tbH);

    const auto content = getLocalBounds().withTrimmedTop (hH + tbH);
    m_specView.setBounds (content);
    m_dynView .setBounds (content);
    m_oscView .setBounds (content);
    m_harmView.setBounds (content);
}
