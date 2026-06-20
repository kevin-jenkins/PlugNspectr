/*
  ==============================================================================
    PlugNspectrPost  –  PluginEditor.cpp
  ==============================================================================
*/

#include "PluginEditor.h"
#include <cmath>
#include <algorithm>
#include <set>

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
    setInterceptsMouseClicks (true, false);
}

void SpectrumView::mouseMove (const juce::MouseEvent& e)
{
    constexpr float kML = 44.0f, kMR = 12.0f, kMT = 10.0f, kMB = 24.0f;
    const float px = kML, py = kMT;
    const float pw = (float) getWidth()  - kML - kMR;
    const float ph = (float) getHeight() - kMT - kMB;
    const float mx = (float) e.x;
    const float my = (float) e.y;

    if (!m_mouseLocked)
    {
        const bool inPlot = mx >= px && mx <= px + pw && my >= py && my <= py + ph;
        if (inPlot != m_mouseInPlot || (inPlot && mx != m_mouseX))
        {
            m_mouseInPlot = inPlot;
            m_mouseX      = mx;
            repaint();
        }
    }
}

void SpectrumView::mouseExit (const juce::MouseEvent&)
{
    if (!m_mouseLocked && m_mouseInPlot)
    {
        m_mouseInPlot = false;
        repaint();
    }
}

void SpectrumView::mouseDown (const juce::MouseEvent& e)
{
    constexpr float kML = 44.0f, kMR = 12.0f, kMT = 10.0f, kMB = 24.0f;
    const float px = kML, py = kMT;
    const float pw = (float) getWidth()  - kML - kMR;
    const float ph = (float) getHeight() - kMT - kMB;
    const float mx = (float) e.x;
    const float my = (float) e.y;

    if (mx >= px && mx <= px + pw && my >= py && my <= py + ph)
    {
        m_mouseLocked = !m_mouseLocked;
        m_mouseX      = mx;
        m_mouseInPlot = true;
        repaint();
    }
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

    // ── Silence detection and warmup gating ──────────────────────────────
    {
        float maxMag = 0.0f;
        for (int k = 1; k < PlugNspectrPostProcessor::kNumSpecBins; ++k)
            maxMag = juce::jmax (maxMag, m_rawPost[k]);
        const bool isSilent = (maxMag < 1.0e-5f);   // below ~-100 dB

        if (isSilent)
        {
            ++m_silenceN;
            if (m_silenceN >= kSilenceFrames)
            {
                // 3 s of continuous silence — full reset
                m_warmupN  = 0;
                m_markerN  = 0;
                m_silenceN = 0;
                m_peakMarkers.clear();
                m_topBins.fill (-1);
                m_topScores.fill (0.0f);
            }
        }
        else
        {
            m_silenceN = 0;
            if (m_warmupN < kWarmupFrames)
            {
                ++m_warmupN;
                if (m_warmupN == kWarmupFrames)
                    m_markerN = kMarkerUpdateFrames;  // trigger first calc immediately after warmup
            }
        }
    }

    // ── Variance markers: score = abs(postAvg_dB − preAvg_dB) per bin ──────
    // Measures the plugin's actual EQ/compression effect at each frequency.
    // Gated behind 3 s warmup; recalculates every 5 seconds.
    if (m_warmupN >= kWarmupFrames)
    {
        ++m_markerN;
        if (m_markerN >= kMarkerUpdateFrames)
        {
            m_markerN = 0;

            auto magToDbF = [] (float mag) -> float {
                return 20.0f * std::log10 (juce::jmax (mag, 1.0e-6f));
            };

            const double sr   = m_proc.getSampleRate();
            const float  binW = (sr > 0.0) ? (float)(sr / PlugNspectrPostProcessor::kFftSize) : 1.0f;

            constexpr float kDispML      = 44.0f, kDispMR = 12.0f;
            constexpr float kDispMinFreq = 20.0f, kDispMaxFreq = 20000.0f;
            const float kLogMin = std::log10 (kDispMinFreq);
            const float kLogMax = std::log10 (kDispMaxFreq);
            const float dispW   = (float) getWidth() - kDispML - kDispMR;
            auto binToX = [&] (int k2) -> float {
                const float f = juce::jlimit (kDispMinFreq, kDispMaxFreq, (float) k2 * binW);
                return kDispML + dispW * (std::log10 (f) - kLogMin) / (kLogMax - kLogMin);
            };

            // Build ranked list: score = |postAvg_dB − preAvg_dB|, both clamped to
            // kNoiseFloorDb so a noise-floor value never inflates the difference
            // unrealistically. Bins where BOTH signals are below floor are skipped
            // outright (true silence). A bin where only one is below floor is still
            // valid — it means the plugin is adding or removing content there.
            constexpr float kNoiseFloorDb = -85.0f;
            using VB = std::pair<float, int>;
            std::vector<VB> ranked;
            ranked.reserve (PlugNspectrPostProcessor::kNumSpecBins);
            for (int k = 1; k < PlugNspectrPostProcessor::kNumSpecBins; ++k)
            {
                const float freq = (float) k * binW;
                if (freq < kDispMinFreq || freq > kDispMaxFreq) continue;
                const float rawPostDb = magToDbF (m_avgPost[k]);
                const float rawPreDb  = magToDbF (m_avgPre [k]);
                if (rawPostDb < kNoiseFloorDb && rawPreDb < kNoiseFloorDb) continue;
                // Clamp each to floor so a -110 dB value doesn't create a huge gap
                const float postDb = juce::jmax (rawPostDb, kNoiseFloorDb);
                const float preDb  = juce::jmax (rawPreDb,  kNoiseFloorDb);
                ranked.push_back ({ std::abs (postDb - preDb), k });
            }
            std::sort (ranked.begin(), ranked.end(),
                       [] (const VB& a, const VB& b) { return a.first > b.first; });

            // Debug: log top 10 candidates before spacing enforcement
            {
                int logged = 0;
                for (auto& [sc, k] : ranked)
                {
                    if (logged >= 10) break;
                    DBG ("Candidate bin=" + juce::String (k)
                         + " freq=" + juce::String ((float) k * binW, 1)
                         + " variance=" + juce::String (sc, 2)
                         + " screenX=" + juce::String (binToX (k), 1));
                    ++logged;
                }
            }

            // ── Slot 0: reserved low-frequency marker (20 – 200 Hz) ────────
            // Independently selected; not subject to cross-region spacing. Min 2 dB.
            constexpr float kLowFreqCeil  = 200.0f;
            constexpr float kLowFreqMinDb = 2.0f;

            int   lowFreqBin   = -1;
            float lowFreqScore = 0.0f;
            for (auto& [sc, k] : ranked)
            {
                if (sc < kLowFreqMinDb) break;
                if ((float) k * binW <= kLowFreqCeil)
                {
                    lowFreqBin   = k;
                    lowFreqScore = sc;
                    break;
                }
            }

            // ── Slot 1: reserved Pre-dropout marker ─────────────────────────
            // Fires when the plugin is boosting/adding content at a frequency:
            // Post has meaningful signal but Pre has dropped below noise floor.
            // Pre < -80 dB AND Post > -75 dB AND variance >= 8 dB.
            constexpr float kBoostPreMax  = -80.0f;   // Pre must be below this
            constexpr float kBoostPostMin = -75.0f;   // Post must be above this
            constexpr float kBoostMinDb   =  8.0f;

            int   boostBin   = -1;
            float boostScore = 0.0f;
            for (auto& [sc, k] : ranked)
            {
                if (sc < kBoostMinDb) break;
                const float rawPostDb = magToDbF (m_avgPost[k]);
                const float rawPreDb  = magToDbF (m_avgPre [k]);
                if (rawPreDb < kBoostPreMax && rawPostDb > kBoostPostMin)
                {
                    boostBin   = k;
                    boostScore = sc;
                    break;
                }
            }
            const float boostX = (boostBin >= 0) ? binToX (boostBin) : -1.0e9f;

            // ── Slots 2–4: greedy high-frequency selection (200 Hz – 20 kHz) ──
            // Spacing checked against each other and against slot 1 (Pre-dropout).
            constexpr int kHighFreqSlots = kNumPeakMarkers - 2;   // 3

            std::array<int,   kHighFreqSlots> hfTop    { -1,-1,-1 };
            std::array<float, kHighFreqSlots> hfScores {  0, 0, 0 };
            std::array<float, kHighFreqSlots> hfX      {  0, 0, 0 };
            int hfPicked = 0;

            for (auto& [sc, k] : ranked)
            {
                if (hfPicked >= kHighFreqSlots) break;
                const float freq = (float) k * binW;
                if (freq < kLowFreqCeil || freq > kDispMaxFreq) continue;
                const float x = binToX (k);
                bool tooClose = (std::abs (x - boostX) < kMinPixelSpacing);
                if (!tooClose)
                    for (int i = 0; i < hfPicked; ++i)
                        if (std::abs (x - hfX[i]) < kMinPixelSpacing) { tooClose = true; break; }
                if (!tooClose)
                {
                    hfTop[hfPicked]    = k;
                    hfScores[hfPicked] = sc;
                    hfX[hfPicked]      = x;
                    ++hfPicked;
                }
            }

            // ── Assemble final 5-slot array ─────────────────────────────────
            // Slot 0 = low-freq reserved; Slot 1 = Pre-dropout; Slots 2-4 = greedy
            std::array<int,   kNumPeakMarkers> newTop    { -1,-1,-1,-1,-1 };
            std::array<float, kNumPeakMarkers> newScores {  0, 0, 0, 0, 0 };
            std::array<float, kNumPeakMarkers> newX      {  0, 0, 0, 0, 0 };

            newTop[0]    = lowFreqBin;
            newScores[0] = lowFreqScore;
            newX[0]      = (lowFreqBin >= 0) ? binToX (lowFreqBin) : 0.0f;

            newTop[1]    = boostBin;
            newScores[1] = boostScore;
            newX[1]      = (boostBin >= 0) ? binToX (boostBin) : 0.0f;

            for (int i = 0; i < kHighFreqSlots; ++i)
            {
                newTop   [2 + i] = hfTop[i];
                newScores[2 + i] = hfScores[i];
                newX     [2 + i] = hfX[i];
            }

            // ── Per-slot 20% score hysteresis ─────────────────────────────
            // Each slot competes against its own previously confirmed bin only.
            for (int i = 0; i < kNumPeakMarkers; ++i)
            {
                if (newTop[i] < 0 || m_topBins[i] < 0) continue;
                if (newTop[i] == m_topBins[i]) continue;
                const float opDb  = juce::jmax (magToDbF (m_avgPost[m_topBins[i]]), kNoiseFloorDb);
                const float orDb  = juce::jmax (magToDbF (m_avgPre  [m_topBins[i]]), kNoiseFloorDb);
                const float oldSc = std::abs (opDb - orDb);
                if (newScores[i] < oldSc * kScoreHysteresis)
                {
                    newTop   [i] = m_topBins[i];
                    newScores[i] = oldSc;
                    newX     [i] = binToX (m_topBins[i]);
                }
            }

            // ── Post-hysteresis deduplication ─────────────────────────────
            // After all slots (including hysteresis) are finalised, discard the
            // lower-scoring slot in any pair that land within 80 px of each other.
            // This handles the case where a reserved slot and a greedy slot (or two
            // greedy slots after hysteresis) independently settle on the same region.
            for (int i = 0; i < kNumPeakMarkers; ++i)
            {
                if (newTop[i] < 0) continue;
                for (int j = i + 1; j < kNumPeakMarkers; ++j)
                {
                    if (newTop[j] < 0) continue;
                    if (std::abs (newX[i] - newX[j]) < kMinPixelSpacing)
                    {
                        if (newScores[i] >= newScores[j])
                            newTop[j] = -1;
                        else
                        {
                            newTop[i] = -1;
                            break;   // slot i gone — stop checking it
                        }
                    }
                }
            }

            // Apply update: fade out removed markers, fade in new ones
            std::set<int> newSet (newTop.begin(), newTop.end());
            for (auto& mk : m_peakMarkers)
                if (mk.fadingIn && newSet.find (mk.bin) == newSet.end())
                    mk.fadingIn = false;

            std::set<int> alreadyShown;
            for (auto& mk : m_peakMarkers)
                alreadyShown.insert (mk.bin);

            for (int i = 0; i < kNumPeakMarkers; ++i)
            {
                const int  b       = newTop[i];
                const bool isBoost = (i == 1 && b >= 0);
                if (b < 0) continue;
                if (alreadyShown.find (b) == alreadyShown.end())
                    m_peakMarkers.push_back ({ b, (float) b, 0.0f, true, newScores[i], isBoost });
                else
                    for (auto& mk : m_peakMarkers)
                        if (mk.bin == b) { mk.score = newScores[i]; mk.isBoost = isBoost; break; }
            }

            m_topBins   = newTop;
            m_topScores = newScores;
        }

        // Lerp displayBin toward target bin each frame (~2 s exponential approach)
        for (auto& mk : m_peakMarkers)
            mk.displayBin += ((float) mk.bin - mk.displayBin) * kLerpRate;
    }

    // Animate alphas — fast fade-in (0.5 s), slower fade-out (1 s)
    for (auto& mk : m_peakMarkers)
        mk.alpha = juce::jlimit (0.0f, 1.0f,
                                 mk.alpha + (mk.fadingIn ? kMarkerFadeInRate : -kMarkerFadeOutRate));
    m_peakMarkers.erase (
        std::remove_if (m_peakMarkers.begin(), m_peakMarkers.end(),
                        [] (const PeakMarkerState& mk) { return !mk.fadingIn && mk.alpha <= 0.0f; }),
        m_peakMarkers.end());
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
    auto xToFreq = [&] (float x) -> float {
        const float t = (x - px) / pw;
        return std::pow (10.0f, kLogMin + t * (kLogMax - kLogMin));
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
    // Pure #0F0F0F plot area
    g.setColour (juce::Colour (0xff0f0f0f));
    g.fillRect (px, py, pw, ph);

    // ── Dotted grid ───────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());

    const float freqMarkers[] = { 30, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (float freq : freqMarkers)
    {
        const float x = freqToX (freq);
        g.setColour (PnsTheme::kGridLine);
        PnsTheme::drawDottedVLine (g, x, py, py + ph);

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
        if (db == 0.0f)
        {
            // 0 dB: slightly brighter solid line
            g.setColour (PnsTheme::kZeroLine);
            g.drawHorizontalLine (juce::roundToInt (y), px, px + pw);
        }
        else
        {
            g.setColour (PnsTheme::kGridLine);
            PnsTheme::drawDottedHLine (g, y, px, px + pw);
        }

        juce::String lbl = (db > 0) ? ("+" + juce::String (juce::roundToInt (db)))
                                     : (db == 0 ? "0" : juce::String (juce::roundToInt (db)));
        g.setColour (db == 0.0f ? PnsTheme::kTextSecondary : PnsTheme::kGridLabel);
        g.drawText (lbl, 2, juce::roundToInt (y) - 7,
                    juce::roundToInt (kML) - 5, 14, juce::Justification::centredRight);
    }

    // ── Spectra ───────────────────────────────────────────────────────────
    const double sr = m_proc.getSampleRate();
    if (sr <= 0.0) return;

    const float binW = (float) (sr / PlugNspectrPostProcessor::kFftSize);

    // Build paths for PRE and POST
    juce::Path prePath, postPath;
    bool preStarted = false, postStarted = false;

    for (int k = 1; k < PlugNspectrPostProcessor::kNumSpecBins; ++k)
    {
        const float freq = (float) k * binW;
        if (freq < kMinFreq) continue;
        if (freq > kMaxFreq) break;

        const float x = freqToX (freq);

        {
            const float db = juce::jlimit (kMinDb, kMaxDb, magToDb (m_post[k]));
            const float y  = dbToY (db);
            if (!postStarted) { postPath.startNewSubPath (x, y); postStarted = true; }
            else               postPath.lineTo (x, y);
        }
        {
            const float db = juce::jlimit (kMinDb, kMaxDb, magToDb (m_pre[k]));
            const float y  = dbToY (db);
            if (!preStarted) { prePath.startNewSubPath (x, y); preStarted = true; }
            else              prePath.lineTo (x, y);
        }
    }

    // Clip all curve drawing to the plot area so glow strokes don't bleed
    // into the frequency-label strip below or the margins around the plot.
    g.saveState();
    g.reduceClipRegion (juce::Rectangle<float> (px, py, pw, ph).toNearestInt());

    // POST: subtle glow fill (10%) then glow line
    if (postStarted)
    {
        PnsTheme::drawGlowFill (g, postPath, PnsTheme::kColorPost,
                                px + pw, py + ph, 0.10f);
        PnsTheme::drawGlowLine (g, postPath, PnsTheme::kColorPost, 2.0f);
    }

    // PRE: thin ghosted line + very subtle fill (5%)
    if (preStarted)
    {
        PnsTheme::drawGlowFill (g, prePath, PnsTheme::kColorPre,
                                px + pw, py + ph, 0.05f);
        g.setColour (PnsTheme::kColorPre.withAlpha (PnsTheme::kColorPreAlpha));
        g.strokePath (prePath, juce::PathStrokeType (1.0f));
    }

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

        // Fill region between post and pre averages (amber 15% opacity)
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
            g.setColour (PnsTheme::kColorPostAvg.withAlpha (0.08f));
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
            g.setColour (colour.withAlpha (0.75f));
            g.strokePath (avgPath, juce::PathStrokeType (1.5f));
        };

        drawAvgLine (prePts,  PnsTheme::kColorPreAvg);
        drawAvgLine (postPts, PnsTheme::kColorPostAvg);
    }

    g.restoreState();   // end plot-area clip

    // ── Fluctuation peak markers — only after warmup ─────────────────────
    if (m_warmupN >= kWarmupFrames && !m_peakMarkers.empty())
    {
        constexpr float labelH    = 16.0f;
        constexpr float stemLen   = 28.0f;
        constexpr float dotR      = 3.0f;
        constexpr int   kSmH      = 2;   // must match kSmoothHalf in collectAvg above
        const juce::Colour kPillBg   = PnsTheme::kColorPostAvg;
        const juce::Colour kPillText = PnsTheme::kBgPanel;
        const juce::Font   pillFont  = PnsTheme::fontLabel();

        struct LayoutMk { float x, dotY, alpha, pillW; juce::String label; };
        std::vector<LayoutMk> layout;
        layout.reserve (m_peakMarkers.size());

        for (auto& mk : m_peakMarkers)
        {
            if (mk.displayBin < 1.0f) continue;
            const int dispBin = juce::roundToInt (mk.displayBin);
            if (dispBin < 1 || dispBin >= PlugNspectrPostProcessor::kNumSpecBins) continue;
            const float displayFreq = mk.displayBin * binW;
            if (displayFreq < kMinFreq || displayFreq > kMaxFreq) continue;

            const float x = freqToX (displayFreq);

            // Dot Y: identical 5-bin smoothing as the Post avg line (collectAvg above)
            const int cBin = juce::jlimit (1, PlugNspectrPostProcessor::kNumSpecBins - 1, dispBin);
            float sum = 0.0f;  int cnt = 0;
            for (int j = cBin - kSmH; j <= cBin + kSmH; ++j)
                if (j >= 1 && j < PlugNspectrPostProcessor::kNumSpecBins)
                    { sum += m_avgPost[j]; ++cnt; }
            const float smoothed = (cnt > 0) ? sum / (float) cnt : m_avgPost[cBin];
            const float dotY = dbToY (juce::jlimit (kMinDb, kMaxDb, magToDb (smoothed)));

            juce::String lbl = (displayFreq >= 1000.0f)
                ? juce::String (displayFreq / 1000.0f, 1) + " kHz"
                : juce::String (juce::roundToInt (displayFreq)) + " Hz";
            // Boost markers get a "↑" indicator and a slightly wider pill
            if (mk.isBoost)
                lbl += juce::String (juce::CharPointer_UTF8 (" \xe2\x86\x91"));
            const float markerPillW = mk.isBoost ? 63.0f : 52.0f;
            layout.push_back ({ x, dotY, mk.alpha, markerPillW, lbl });
        }

        std::sort (layout.begin(), layout.end(),
                   [] (const LayoutMk& a, const LayoutMk& b) { return a.x < b.x; });

        // Two-row stagger: row 0 = normal stem, row 1 = stem extended by 16px.
        // If a label would conflict in both rows, the marker is dropped entirely.
        constexpr float kRow1Extra = 16.0f;
        float row0Right = -1e9f, row1Right = -1e9f;   // rightmost right-edge of placed labels

        for (auto& mk : layout)
        {
            const float pw2    = mk.pillW;
            const float lLeft  = mk.x - pw2 * 0.5f;
            const float lRight = mk.x + pw2 * 0.5f;

            int row = -1;
            if (lLeft >= row0Right + 3.0f)      { row = 0; row0Right = lRight; }
            else if (lLeft >= row1Right + 3.0f) { row = 1; row1Right = lRight; }
            if (row < 0) continue;   // no valid row — drop

            const float alpha    = mk.alpha;
            const float cx       = mk.x;
            const float dotY     = mk.dotY;
            const float extraLen = (row == 1) ? kRow1Extra : 0.0f;
            const float labelTop = dotY - stemLen - extraLen - labelH;

            // Stem — straight vertical from dot to label bottom
            g.setColour (kPillBg.withAlpha (0.6f * alpha));
            g.drawLine (cx, dotY - dotR, cx, labelTop + labelH, 1.0f);

            // Dot — soft outer glow + bright core
            g.setColour (kPillBg.withAlpha (0.30f * alpha));
            g.fillEllipse (cx - dotR * 2.0f, dotY - dotR * 2.0f, dotR * 4.0f, dotR * 4.0f);
            g.setColour (kPillBg.withAlpha (alpha));
            g.fillEllipse (cx - dotR, dotY - dotR, dotR * 2.0f, dotR * 2.0f);

            // Pill label
            const juce::Rectangle<float> pill (cx - pw2 * 0.5f, labelTop, pw2, labelH);
            g.setColour (kPillBg.withAlpha (alpha));
            g.fillRoundedRectangle (pill, 3.0f);
            g.setFont (pillFont);
            g.setColour (kPillText.withAlpha (alpha));
            g.drawText (mk.label, pill.toNearestInt(), juce::Justification::centred);
        }
    }

    // ── Legend — drawn before hairline so tooltip renders on top ─────────
    g.setFont (PnsTheme::fontLabel());
    constexpr float sw = 8.0f, sh = 8.0f, rowH = 13.0f;
    constexpr float kLegendW = 88.0f;
    constexpr float kCtrlBottom = (float) (PnsTheme::kPaddingSmall + PnsTheme::kButtonHeight);
    const int   legendRows  = m_showAvg ? 4 : 2;
    const float legendH     = (float) legendRows * rowH + (float) (PnsTheme::kPaddingSmall * 2);
    const float legendPanelR = (float) getWidth() - (float) PnsTheme::kPaddingMid;
    const float legendPanelT = kCtrlBottom + 8.0f;
    const float legendPanelL = legendPanelR - kLegendW;
    const juce::Rectangle<float> legendRect { legendPanelL, legendPanelT, kLegendW, legendH };
    PnsTheme::drawFrostedPanel (g, legendRect);

    {
        const float lx = legendPanelL + (float) PnsTheme::kPaddingSmall;
        const float ly = legendPanelT + (float) PnsTheme::kPaddingSmall;
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
    }

    // ── Border ────────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);

    // ── Interactive frequency hairline — drawn last so it's always on top ─
    const bool showHair = (m_mouseInPlot || m_mouseLocked)
                          && m_mouseX >= px && m_mouseX <= px + pw;
    if (showHair)
    {
        const float hx = m_mouseX;

        // Hairline — teal glow
        {
            juce::Path hairLine;
            hairLine.startNewSubPath (hx, py);
            hairLine.lineTo          (hx, py + ph);
            g.setColour (PnsTheme::kColorPost.withAlpha (0.18f));
            g.strokePath (hairLine, juce::PathStrokeType (5.0f));
            g.setColour (PnsTheme::kColorPost.withAlpha (0.55f));
            g.strokePath (hairLine, juce::PathStrokeType (1.0f));
        }

        // Find magnitude at hairline for PRE and POST
        const float hFreq = xToFreq (hx);
        const int   hBin  = juce::jlimit (1,
                                PlugNspectrPostProcessor::kNumSpecBins - 1,
                                juce::roundToInt (hFreq / binW));

        const float preDb  = magToDb (m_avgPre [hBin]);
        const float postDb = magToDb (m_avgPost[hBin]);

        // Intersection dots on each curve
        auto drawDot = [&] (float dotDb, juce::Colour colour)
        {
            const float cy = dbToY (juce::jlimit (kMinDb, kMaxDb, dotDb));
            g.setColour (colour.withAlpha (0.30f));
            g.fillEllipse (hx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
            g.setColour (colour);
            g.fillEllipse (hx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
        };
        drawDot (postDb, PnsTheme::kColorPost);
        drawDot (preDb,  PnsTheme::kColorPre);

        // Floating tooltip — freq + PRE + POST dB + delta pill
        juce::String freqStr;
        if (hFreq >= 1000.0f)
            freqStr = juce::String (hFreq / 1000.0f, 1) + " kHz";
        else
            freqStr = juce::String (juce::roundToInt (hFreq)) + " Hz";

        constexpr float kValidFloor = -90.0f;
        const bool postValid = (postDb > kValidFloor);
        const bool preValid  = (preDb  > kValidFloor);

        auto fmtDb = [] (float db, bool valid) -> juce::String {
            if (!valid) return "---";
            return (db >= 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
        };

        const juce::Font ttFont  = PnsTheme::fontLabel();
        constexpr float ttPad    = 6.0f;
        constexpr float lineH    = 12.0f;
        constexpr float lineGap  = 13.0f;
        constexpr float pillPadX = 8.0f;
        constexpr float pillPadY = 2.0f;
        const float pillH        = lineH + pillPadY * 2.0f;

        auto strW = [&] (const juce::String& s) -> float {
            juce::GlyphArrangement ga;
            ga.addLineOfText (ttFont, s, 0.0f, 0.0f);
            return ga.getBoundingBox (0, -1, true).getWidth();
        };

        // Determine pill case:
        //   Case 1 — both valid: show delta
        //   Case 2 — post valid, pre "---": plugin adding content (↑)
        //   Case 3 — pre valid, post "---": plugin removing content (↓)
        //   Case 4 — both "---": no pill
        juce::String deltaStr;
        juce::Colour pillColour;
        float pillW = 0.0f;
        bool  showPill = false;

        if (postValid && preValid)
        {
            // Case 1 — normal delta
            const float deltaDb = postDb - preDb;
            if (std::abs (deltaDb) > 0.1f)
            {
                showPill   = true;
                pillColour = (deltaDb > 0.0f) ? PnsTheme::kColorPost
                                               : PnsTheme::kColorGainRed;
                deltaStr   = (deltaDb > 0.0f ? "+" : "")
                             + juce::String (deltaDb, 1) + " dB";
                pillW      = strW (deltaStr) + pillPadX * 2.0f;
            }
        }
        else if (postValid && !preValid)
        {
            // Case 2 — post valid, pre silent: plugin adding content
            showPill   = true;
            pillColour = PnsTheme::kColorPost;
            deltaStr   = juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x91"))
                         + " +" + juce::String (std::abs (postDb - kValidFloor), 1) + " dB";
            pillW      = strW (deltaStr) + pillPadX * 2.0f;
        }
        else if (!postValid && preValid)
        {
            // Case 3 — pre valid, post silent: plugin removing content
            showPill   = true;
            pillColour = PnsTheme::kColorGainRed;
            deltaStr   = juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x93"))
                         + " -" + juce::String (std::abs (preDb - kValidFloor), 1) + " dB";
            pillW      = strW (deltaStr) + pillPadX * 2.0f;
        }

        const juce::String postLine = "Post: " + fmtDb (postDb, postValid);
        const juce::String preLine  = "Pre:  " + fmtDb (preDb,  preValid);
        const float freqLineW  = strW (freqStr);
        const float postLineW  = strW (postLine) + (showPill ? 6.0f + pillW : 0.0f);
        const float preLineW   = strW (preLine);
        const float contentW   = juce::jmax (freqLineW, postLineW, preLineW);
        const float ttW        = contentW + ttPad * 2.0f;
        const float ttH        = ttPad + lineH + lineGap + lineH + ttPad;

        // Position tooltip: try right of hairline, then left, then below legend
        const float tty0 = py + 6.0f;
        auto tooltipOverlapsLegend = [&] (float tx) {
            return juce::Rectangle<float> (tx, tty0, ttW, ttH)
                       .intersects (legendRect);
        };
        float ttx = hx + 8.0f;
        if (ttx + ttW > px + pw || tooltipOverlapsLegend (ttx))
            ttx = hx - ttW - 8.0f;
        float tty = tty0;
        if (tooltipOverlapsLegend (ttx))
            tty = legendRect.getBottom() + 4.0f;

        PnsTheme::drawFrostedPanel (g, { ttx, tty, ttW, ttH });

        g.setFont (ttFont);

        // Frequency label
        g.setColour (PnsTheme::kTextPrimary);
        g.drawText (freqStr,
                    juce::roundToInt (ttx + ttPad), juce::roundToInt (tty + ttPad),
                    juce::roundToInt (contentW), juce::roundToInt (lineH),
                    juce::Justification::centredLeft);

        // Post dB value
        const float postLineY = tty + ttPad + lineGap;
        g.setColour (PnsTheme::kColorPost);
        g.drawText (postLine,
                    juce::roundToInt (ttx + ttPad), juce::roundToInt (postLineY),
                    juce::roundToInt (contentW), juce::roundToInt (lineH),
                    juce::Justification::centredLeft);

        // Delta pill — right of Post value, vertically centred on Post line
        if (showPill)
        {
            const float postTextW = strW (postLine);
            const float pillX     = ttx + ttPad + postTextW + 6.0f;
            const float pillY     = postLineY + (lineH - pillH) * 0.5f;
            const juce::Rectangle<float> pill (pillX, pillY, pillW, pillH);
            const float pillRadius = pillH * 0.5f;

            g.setColour (pillColour.withAlpha (0.20f));
            g.fillRoundedRectangle (pill, pillRadius);
            g.setColour (pillColour.withAlpha (0.70f));
            g.drawRoundedRectangle (pill, pillRadius, 1.0f);
            g.setColour (pillColour);
            g.drawText (deltaStr, pill.toNearestInt(), juce::Justification::centred);
        }

        // Pre dB value
        g.setColour (PnsTheme::kColorPre.withAlpha (0.9f));
        g.drawText (preLine,
                    juce::roundToInt (ttx + ttPad), juce::roundToInt (postLineY + lineGap),
                    juce::roundToInt (contentW), juce::roundToInt (lineH),
                    juce::Justification::centredLeft);
    }
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
            m_totalSamplesWritten += n;
        }
    }

    // ── Waveform envelope + fixed-at-start vertical scale ─────────────────
    // Columns are anchored to ABSOLUTE sample positions: bin k always covers the
    // same audio [k*binSamples, (k+1)*binSamples), so a given chunk keeps a stable
    // height instead of "breathing" as the window scrolls. Smooth scroll comes
    // from the sub-bin offset (m_waveScrollFrac), not from re-binning each frame.
    //
    // Gated to when the Dynamics tab is visible: this scan is ~displaySamples
    // (up to 576k) per frame, pure waste on the other tabs. Calibration simply
    // begins when the tab is opened — the sample ring buffer keeps filling above.
    if (isVisible())
    {
        const double sr = m_proc.getSampleRate();
        const int displaySamples = (sr > 0.0)
            ? juce::jmin ((int) (sr * (double) m_zoomSeconds), kSampleBufLen)
            : kSampleBufLen;
        const int        binSamples = juce::jmax (1, displaySamples / kWaveCols);
        const juce::int64 total      = m_totalSamplesWritten;
        const juce::int64 oldestAvail = total - juce::jmin ((juce::int64) m_samplesStored,
                                                            (juce::int64) kSampleBufLen);
        const juce::int64 curBin     = total / binSamples;   // rightmost (partial) bin

        m_waveScrollFrac = (float) ((double) (total % binSamples) / (double) binSamples);

        float winPeak = 0.0f;
        bool  anyData = false;

        for (int col = 0; col < kWaveCols; ++col)
        {
            // Newest bin (curBin) sits at the right; older bins to the left.
            const juce::int64 binIndex = curBin - (juce::int64) (kWaveCols - 1) + col;

            float preMin = 0.0f, preMax = 0.0f, postMin = 0.0f, postMax = 0.0f;
            if (binIndex >= 0)
            {
                const juce::int64 a0 = juce::jmax (binIndex * (juce::int64) binSamples, oldestAvail);
                const juce::int64 a1 = juce::jmin ((binIndex + 1) * (juce::int64) binSamples, total);
                // Contiguous range (≤ binSamples < kSampleBufLen): one modulo to
                // seed the ring index, then increment-with-wrap in the hot loop.
                int idx = (int) (a0 % kSampleBufLen);
                for (juce::int64 a = a0; a < a1; ++a)
                {
                    const float ps  = m_preSamples [idx];
                    const float qs  = m_postSamples[idx];
                    if (ps < preMin)  preMin  = ps;
                    if (ps > preMax)  preMax  = ps;
                    if (qs < postMin) postMin = qs;
                    if (qs > postMax) postMax = qs;
                    if (++idx == kSampleBufLen) idx = 0;
                }
                if (a1 > a0) anyData = true;
            }

            winPeak = juce::jmax (winPeak, preMax, -preMin);
            winPeak = juce::jmax (winPeak, postMax, -postMin);

            m_wavePreTop [col] = preMax;  m_wavePreBot [col] = preMin;
            m_wavePostTop[col] = postMax; m_wavePostBot[col] = postMin;
        }
        if (anyData) m_waveColsValid = kWaveCols;

        // Fixed-at-start vertical scale: calibrate from the first ~2s of real
        // audio, lock, and re-arm after ~1s of silence so the next play recals.
        constexpr float kSilence = 1.0e-3f;     // ~-60 dBFS
        const bool hasSignal = winPeak > kSilence;

        if (! m_waveScaleLocked)
        {
            if (hasSignal)
            {
                m_waveCalibPeak     = juce::jmax (m_waveCalibPeak, winPeak);
                m_waveCalibSamples += (int) (sr / 60.0);   // ~one tick of audio

                if (m_waveCalibSamples >= (int) (sr * 2.0) && m_waveCalibPeak > kSilence)
                {
                    // 0.65 → calibration peak fills ~65% of half-height (headroom)
                    m_waveScale       = juce::jlimit (1.0f, 64.0f, 0.65f / m_waveCalibPeak);
                    m_waveScaleLocked = true;
                }
            }
        }
        else
        {
            if (hasSignal)
                m_waveSilenceCount = 0;
            else if (++m_waveSilenceCount >= 60)   // ~1s silent → re-arm
            {
                m_waveScaleLocked  = false;
                m_waveCalibPeak    = 0.0f;
                m_waveCalibSamples = 0;
                m_waveSilenceCount = 0;
            }
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

            if (prePeak > 1.0e-3f)   // ~-60 dBFS silence gate — ignore noise floor
                currentGr = 20.0f * std::log10 (juce::jmax (postPeak, 1.0e-6f))
                          - 20.0f * std::log10 (prePeak);
        }

        const auto rms = m_proc.getRms();
        m_preConnected = rms.preValid;

        // Reject the peak-timing-skew false positive (a transparent plugin showed
        // GR while In/Out RMS were identical, i.e. RMS delta ≈ 0). Keep the gate
        // small — 0.2 dB — so a genuine 0.5 dB gain reduction (RMS delta ≈ 0.5)
        // still registers; the original false case had a 0.0 dB delta, so it's
        // still caught.
        if (rms.preValid && std::abs (rms.postDb - rms.preDb) < 0.2f)
            currentGr = 0.0f;

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

        // Peak hold: track most-negative GR, decay at 0.998 per frame (~3s hold)
        if (rms.preValid && currentGr < m_grPeakHold)
            m_grPeakHold = currentGr;
        else
            m_grPeakHold = juce::jmin (0.0f, m_grPeakHold * 0.998f);
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

    const float scale = m_waveScale;   // smoothed auto-zoom, computed in update()
    auto ampToY = [&] (float a) -> float {
        return midY - juce::jlimit (-1.0f, 1.0f, a * scale) * ph * 0.5f;
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

    // ── Clip lines ── only meaningful when not auto-zoomed (edges = ±1.0) ──
    if (scale < 1.05f)
    {
        g.setColour (PnsTheme::kClipLine);
        g.drawHorizontalLine (juce::roundToInt (ampToY ( 1.0f)), px, px + pw);
        g.drawHorizontalLine (juce::roundToInt (ampToY (-1.0f)), px, px + pw);
    }

    // ── Center line ───────────────────────────────────────────────────────
    g.setColour (PnsTheme::kZeroLine);
    g.drawHorizontalLine (juce::roundToInt (midY), px, px + pw);

    // ── Y axis labels ─────────────────────────────────────────────────────
    // The top/bottom edges represent amplitude 1/scale — show it in dBFS so the
    // current auto-zoom level is legible (0 dB when not zoomed).
    const float edgeDb = 20.0f * std::log10 (juce::jmax (1.0f / scale, 1.0e-6f));
    const juce::String edgeLbl = juce::String (juce::roundToInt (edgeDb)) + " dB";
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kGridLabel);
    g.drawText (edgeLbl, juce::roundToInt (area.getX()), juce::roundToInt (py) - 6,
                juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);
    g.drawText ("0",  juce::roundToInt (area.getX()), juce::roundToInt (midY) - 6,
                juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);
    g.drawText (edgeLbl, juce::roundToInt (area.getX()), juce::roundToInt (py + ph) - 6,
                juce::roundToInt (kML) - 4, 12, juce::Justification::centredRight);

    // ── Waveform paths — render the per-column envelope built in update() ──
    {
        if (m_waveColsValid > 0)
        {
            constexpr int kCols = kWaveCols;
            const float   colW  = pw / (float) kCols;

            // Map the per-column amplitude envelope through the locked scale.
            std::array<float, kCols> preTopY, preBotY, postTopY, postBotY;
            for (int col = 0; col < kCols; ++col)
            {
                preTopY [col] = ampToY (m_wavePreTop [col]);
                preBotY [col] = ampToY (m_wavePreBot [col]);
                postTopY[col] = ampToY (m_wavePostTop[col]);
                postBotY[col] = ampToY (m_wavePostBot[col]);
            }

            // Sub-column scroll: shift left by how far the newest bin has filled,
            // so the waveform glides continuously instead of jumping per column.
            auto cx = [&] (int col) -> float {
                return px + ((float) col + 1.5f - m_waveScrollFrac) * colW;
            };

            // Top-edge-only open path for glow stroke
            auto buildTopPath = [&] (const std::array<float, kCols>& topY) -> juce::Path
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
                return p;
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

            // Clip to the plot so the partial (sub-column-shifted) edges don't
            // spill past the right border.
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (juce::Rectangle<float> (px, py, pw, ph).toNearestInt());

            // PRE: lavender at 35% opacity — ghosted behind POST
            g.setColour (PnsTheme::kColorPre.withAlpha ((uint8_t) 0x59));
            g.fillPath (buildPath (preTopY, preBotY));

            // POST: teal at 85% opacity + glow outline on top edge
            g.setColour (PnsTheme::kColorPost.withAlpha ((uint8_t) 0xD9));
            g.fillPath (buildPath (postTopY, postBotY));
            PnsTheme::drawGlowLine (g, buildTopPath (postTopY), PnsTheme::kColorPost, 1.5f);

            // Compression difference fill: subtle teal at 15% — aligned to the
            // same sub-column scroll offset as the paths.
            g.setColour (PnsTheme::kColorPost.withAlpha ((uint8_t) 0x26));
            for (int col = 0; col < kCols; ++col)
            {
                const float x = px + ((float) col + 1.0f - m_waveScrollFrac) * colW;
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

    // Filled GR history — vertical gradient (85% at top → 35% at bottom)
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
    {
        juce::ColourGradient grad (PnsTheme::kColorGainRed.withAlpha (0.85f), px, py,
                                   PnsTheme::kColorGainRed.withAlpha (0.35f), px, py + ph,
                                   false);
        g.setGradientFill (grad);
        g.fillPath (fillPath);
    }

    // Outline curve — glow treatment
    juce::Path curvePath;
    for (int i = 0; i < kGrLen; ++i)
    {
        const int   idx = (m_grPos + i) % kGrLen;
        const float x   = px + pw * (float) i / (float) (kGrLen - 1);
        const float y   = grToY (m_gr[idx]);
        if (i == 0) curvePath.startNewSubPath (x, y);
        else        curvePath.lineTo (x, y);
    }
    PnsTheme::drawGlowLine (g, curvePath, PnsTheme::kColorGainRed, 1.5f);

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
    g.setColour (juce::Colour (0, 0, 0));    // pure black plot area
    g.fillRect (px, py, pw, ph);

    // ── Title ─────────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("OSCILLOSCOPE",
                (int) px, 0, (int) pw, (int) kMT, juce::Justification::centred);

    // ── Grid — solid for clip lines, dotted for sub-divisions ─────────────
    g.setColour (PnsTheme::kClipLine);
    g.drawHorizontalLine (juce::roundToInt (ampToY ( 1.0f)), px, px + pw);
    g.drawHorizontalLine (juce::roundToInt (ampToY (-1.0f)), px, px + pw);

    g.setColour (PnsTheme::kGridLine);
    PnsTheme::drawDottedHLine (g, ampToY ( 0.5f), px, px + pw);
    PnsTheme::drawDottedHLine (g, ampToY (-0.5f), px, px + pw);

    g.setColour (PnsTheme::kZeroLine);
    g.drawHorizontalLine (juce::roundToInt (midY), px, px + pw);

    g.setColour (PnsTheme::kGridLine);
    for (int q = 1; q <= 3; ++q)
        PnsTheme::drawDottedVLine (g, px + pw * (float) q / 4.0f, py, py + ph);

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

        // PRE — ghosted lavender at 40%, no glow, thin
        g.setColour (PnsTheme::kColorPre.withAlpha (0.40f));
        g.strokePath (buildPath (m_displayPre), juce::PathStrokeType (1.0f));

        // POST — teal with glow (on top)
        PnsTheme::drawGlowLine (g, buildPath (m_displayPost), PnsTheme::kColorPost, 1.5f);
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
        juce::String srStr = "SR: --";
        if (sr > 0.0)
        {
            if (sr >= 1000.0)
            {
                const double srKhz = sr / 1000.0;
                // Show one decimal only when needed (e.g. 44.1, 88.2; suppress for 48, 96)
                const bool needDecimal = (std::fmod (srKhz, 1.0) > 0.05);
                srStr = "SR: " + juce::String (srKhz, needDecimal ? 1 : 0) + " kHz";
            }
            else
            {
                srStr = "SR: " + juce::String ((int) sr) + " Hz";
            }
        }
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

    // Show "Activate" prompt only when tone has never been run (no frozen data)
    if (!m_toneActive && !m_harmonicsPaused)
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Activate test tone to begin analysis",
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
        m_proc.resetMeasurement();
        if (onMeasureChanged) onMeasureChanged();
        repaint();
    };
    addAndMakeVisible (m_measureBtn);

    // Freeze: snapshot the current curves as a dimmed A/B reference (toggle off
    // to clear). Lets you compare two plugin settings / two plugins.
    m_freezeBtn.onClick = [this]
    {
        if (m_hasFrozen) m_hasFrozen = false;
        else             doFreeze();
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
    const int    L     = m_meas.latencySamples;
    m_latSamples       = L;
    m_latMs            = (float) (1000.0 * L / sr);
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

    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (px, py, pw, ph);

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText (title, (int) r.getX(), (int) r.getY(), (int) r.getWidth(), 13,
                juce::Justification::centred);

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

    drawPanel (g, magR, "MAGNITUDE (dB)",   m_meas.magDb, -48.0f,  12.0f, "dB",  PnsTheme::kColorPost,
               m_hasFrozen ? &m_frozenMag   : nullptr);
    drawPanel (g, phR,  "PHASE (deg)",      m_phaseDeg,  -180.0f, 180.0f, "deg", PnsTheme::kColorPostAvg,
               m_hasFrozen ? &m_frozenPhase : nullptr);
    drawPanel (g, gdR,  "GROUP DELAY (ms)", m_groupMs,    m_gdLo,  m_gdHi, "ms",  PnsTheme::kColorGainRed,
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
            { phR,  m_phaseDeg[k],  -180.0f, 180.0f, PnsTheme::kColorPostAvg },
            { gdR,  m_groupMs[k],    m_gdLo,  m_gdHi, PnsTheme::kColorGainRed },
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
        g.setColour (juce::Colour (10, 10, 10));
        g.fillRect (bx, by, boxW, boxH);
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
        g.drawText ("Enable Measure to inject a noise stimulus and measure magnitude / phase / group delay",
                    getLocalBounds().reduced (20), juce::Justification::centred);
    }
}

//==============================================================================
// Shared single-plot cursor: hairline + dot + readout chip.
static void drawCursorChip (juce::Graphics& g, juce::Rectangle<float> plot, float cx,
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
    g.setColour (juce::Colour (10, 10, 10));
    g.fillRect (bx, by, boxW, boxH);
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

static juce::Rectangle<float> transferPlot (juce::Component& c)
{
    auto a = c.getLocalBounds().toFloat().reduced (8.0f);
    a.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);
    const float side = juce::jmin (a.getWidth(), a.getHeight());
    const juce::Rectangle<float> r (a.getX() + (a.getWidth() - side) * 0.5f, a.getY(), side, side);
    return { r.getX() + 40.0f, r.getY() + 16.0f, r.getWidth() - 52.0f, r.getHeight() - 36.0f };
}
static juce::Rectangle<float> envPlot (juce::Component& c)
{
    auto a = c.getLocalBounds().toFloat().reduced (8.0f);
    a.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);
    return { a.getX() + 42.0f, a.getY() + 16.0f, a.getWidth() - 54.0f, a.getHeight() - 36.0f };
}
static juce::Rectangle<float> thdPlot (juce::Component& c)
{
    auto a = c.getLocalBounds().toFloat().reduced (8.0f);
    a.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);
    return { a.getX() + 46.0f, a.getY() + 16.0f, a.getWidth() - 58.0f, a.getHeight() - 36.0f };
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
        m_proc.resetDynamics();
        if (onMeasureChanged) onMeasureChanged();
        repaint();
    };
    addAndMakeVisible (m_measureBtn);

    m_freezeBtn.onClick = [this]
    {
        if (m_hasFrozen) m_hasFrozen = false; else doFreeze();
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

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("TRANSFER CURVE: output vs input (dB)",
                (int) r.getX(), (int) r.getY(), (int) r.getWidth(), 13,
                juce::Justification::centred);

    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (px, py, pw, ph);

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
        g.drawText ("Enable Measure to sweep a level ramp and plot the transfer curve",
                    getLocalBounds().reduced (20), juce::Justification::centred);
    }
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
        m_proc.resetEnvelope();
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

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("ATTACK / RELEASE: gain reduction vs time (ms)",
                (int) area.getX(), (int) area.getY(), (int) area.getWidth(), 13,
                juce::Justification::centred);

    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (px, py, pw, ph);

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
    g.setColour (PnsTheme::kColorPostAvg.withAlpha (0.6f));
    g.drawVerticalLine (juce::roundToInt (X (0.0f)),   py, py + ph);
    g.drawVerticalLine (juce::roundToInt (X (500.0f)), py, py + ph);
    g.setColour (PnsTheme::kColorPostAvg);
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
        PnsTheme::drawGlowLine (g, path, PnsTheme::kColorGainRed, 1.5f);

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
                            PnsTheme::kColorGainRed, rows, m_cursorLocked);
        }
    }

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);

    if (nValid < 2)
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Enable Measure to send a level-step stimulus and plot attack / release",
                    getLocalBounds().reduced (20), juce::Justification::centred);
    }
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
        m_proc.resetThdSweep();
        if (onMeasureChanged) onMeasureChanged();
        repaint();
    };
    addAndMakeVisible (m_measureBtn);

    m_freezeBtn.onClick = [this]
    {
        if (m_hasFrozen) m_hasFrozen = false; else doFreeze();
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
    if (m_cursorLocked) return;
    m_cursorX = thdPlot (*this).contains (e.position) ? e.position.x : -1.0f;
    repaint();
}
void ThdSweepView::mouseExit (const juce::MouseEvent&)
{
    if (! m_cursorLocked) { m_cursorX = -1.0f; repaint(); }
}
void ThdSweepView::mouseDown (const juce::MouseEvent& e)
{
    if (thdPlot (*this).contains (e.position))
    { m_cursorX = e.position.x; m_cursorLocked = ! m_cursorLocked; repaint(); }
}

void ThdSweepView::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);

    auto area = getLocalBounds().toFloat().reduced (8.0f);
    area.removeFromTop ((float) PnsTheme::kButtonHeight + 4.0f);

    constexpr float kML = 46.0f, kMR = 12.0f, kMT = 16.0f, kMB = 20.0f;
    const float px = area.getX() + kML, py = area.getY() + kMT;
    const float pw = area.getWidth() - kML - kMR, ph = area.getHeight() - kMT - kMB;
    if (pw <= 0 || ph <= 0) return;

    const float lo = PlugNspectrPostProcessor::kThdLoHz, hi = PlugNspectrPostProcessor::kThdHiHz;
    auto X = [&] (double f) { return px + pw * (float) (std::log (juce::jlimit ((double) lo, (double) hi, f) / lo)
                                                        / std::log ((double) hi / lo)); };
    // Log-% Y axis: 0.001% (bottom) .. 100% (top), 5 decades.
    constexpr float yHi = 2.0f, yLo = -3.0f;
    auto Y = [&] (float pct) { const float lv = juce::jlimit (yLo, yHi, std::log10 (juce::jmax (pct, 1.0e-4f)));
                               return py + ph * (yHi - lv) / (yHi - yLo); };

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("THD vs FREQUENCY (%)", (int) area.getX(), (int) area.getY(),
                (int) area.getWidth(), 13, juce::Justification::centred);

    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (px, py, pw, ph);

    const float decades[] = { 100.0f, 10.0f, 1.0f, 0.1f, 0.01f, 0.001f };
    for (float d : decades)
    {
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
                          const std::array<bool, kBins>& valid, int& count) -> juce::Path
    {
        juce::Path path;
        bool started = false;
        count = 0;
        for (int b = 0; b < kBins; ++b)
        {
            if (! valid[(size_t) b]) continue;
            const double f = lo * std::pow ((double) hi / lo, (double) b / (kBins - 1));
            const float x = X (f), y = Y (thd[(size_t) b]);
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
        const juce::Path fp = buildPath (m_frozenThd, m_frozenValid, fc);
        if (fc > 1) { g.setColour (PnsTheme::kTextSecondary.withAlpha (0.5f));
                      g.strokePath (fp, juce::PathStrokeType (1.0f)); }
    }

    int nValid = 0;
    const juce::Path path = buildPath (m_thd.thdPct, m_thd.valid, nValid);
    if (nValid > 1)
        PnsTheme::drawGlowLine (g, path, PnsTheme::kColorPostAvg, 1.5f);

    // Cursor readout — frequency → THD %.
    if (nValid > 1 && m_cursorX >= 0.0f)
    {
        const float cx = juce::jlimit (px, px + pw, m_cursorX);
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
            drawCursorChip (g, { px, py, pw, ph }, cx, Y (pct), PnsTheme::kColorPostAvg, rows, m_cursorLocked);
        }
    }

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (px, py, pw, ph, 1.0f);

    if (nValid < 2)
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Enable Measure to sweep a tone across frequency and plot THD",
                    getLocalBounds().reduced (20), juce::Justification::centred);
    }
}

//==============================================================================
PlugNspectrPostEditor::PlugNspectrPostEditor (PlugNspectrPostProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      m_specView (p),
      m_dynView  (p),
      m_oscView  (p),
      m_harmView (p),
      m_linearView (p),
      m_transferView (p),
      m_envelopeView (p),
      m_thdView (p)
{
    setLookAndFeel (&m_laf);

    m_biltroyLogo = juce::ImageCache::getFromMemory (BinaryData::BiltroyAudio_x09mxix09mxix09m_png,
                                                     BinaryData::BiltroyAudio_x09mxix09mxix09m_pngSize);

    for (auto* btn : { &m_tabSpectrum, &m_tabDynamics, &m_tabOscilloscope,
                       &m_tabHarmonics, &m_tabLinear, &m_tabTransfer, &m_tabEnvelope,
                       &m_tabThd })
    {
        btn->getProperties().set ("tabButton", true);
        addAndMakeVisible (btn);
    }

    m_tabSpectrum    .onClick = [this] { switchTab (0); };
    m_tabDynamics    .onClick = [this] { switchTab (1); };
    m_tabOscilloscope.onClick = [this] { switchTab (2); };
    m_tabHarmonics   .onClick = [this] { switchTab (3); };
    m_tabLinear      .onClick = [this] { switchTab (4); };
    m_tabTransfer    .onClick = [this] { switchTab (5); };
    m_tabEnvelope    .onClick = [this] { switchTab (6); };
    m_tabThd         .onClick = [this] { switchTab (7); };

    addAndMakeVisible (m_specView);
    addAndMakeVisible (m_dynView);
    addAndMakeVisible (m_oscView);
    addAndMakeVisible (m_harmView);
    addAndMakeVisible (m_linearView);
    addAndMakeVisible (m_transferView);
    addAndMakeVisible (m_envelopeView);
    addAndMakeVisible (m_thdView);

    // Measure toggles re-publish the cmd block (stimulus on/off).
    m_linearView.onMeasureChanged   = [this] { writeCmdBlock(); repaint(); };
    m_transferView.onMeasureChanged = [this] { writeCmdBlock(); repaint(); };
    m_envelopeView.onMeasureChanged = [this] { writeCmdBlock(); repaint(); };
    m_thdView.onMeasureChanged      = [this] { writeCmdBlock(); repaint(); };

    // ── Global footer — frequency knob ────────────────────────────────────
    m_footerFreqSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    m_footerFreqSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    m_footerFreqSlider.setRange (100.0, 8000.0, 1.0);
    m_footerFreqSlider.setSkewFactorFromMidPoint (1000.0);
    m_footerFreqSlider.setValue (1000.0, juce::dontSendNotification);
    m_footerFreqSlider.onValueChange = [this]
    {
        m_toneFreq = m_footerFreqSlider.getValue();
        writeCmdBlock();
        m_harmView.setToneFreq (m_toneFreq);
        repaint();   // update footer freq label
    };
    addAndMakeVisible (m_footerFreqSlider);

    // ── Global footer — test tone level ───────────────────────────────────
    m_footerLevelSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    m_footerLevelSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    m_footerLevelSlider.setRange (-48.0, 0.0, 0.5);
    m_footerLevelSlider.setValue (m_toneLevel, juce::dontSendNotification);
    m_footerLevelSlider.onValueChange = [this]
    {
        m_toneLevel = m_footerLevelSlider.getValue();
        writeCmdBlock();
        repaint();
    };
    addAndMakeVisible (m_footerLevelSlider);

    // ── Global footer — test tone button ──────────────────────────────────
    m_footerToneBtn.setToggleable (true);
    m_footerToneBtn.getProperties().set ("armed", true);
    m_footerToneBtn.setToggleState (false, juce::dontSendNotification);
    m_footerToneBtn.onClick = [this]
    {
        m_toneActive = !m_toneActive;
        m_footerToneBtn.setToggleState (m_toneActive, juce::dontSendNotification);
        writeCmdBlock();
        m_harmView.setToneActive (m_toneActive);
        repaint();   // show/hide warning label
    };
    addAndMakeVisible (m_footerToneBtn);

    // ── Header — L/R/Mid/Side segmented channel selector (all analysis) ────
    {
        juce::TextButton* segs[4] = { &m_chL, &m_chR, &m_chM, &m_chS };
        const char* tip[4] = { "Left", "Right", "Mid (L+R)/2", "Side (L-R)/2" };
        for (int i = 0; i < 4; ++i)
        {
            segs[i]->setClickingTogglesState (true);
            segs[i]->setRadioGroupId (7001);
            segs[i]->setTooltip (tip[i]);
            segs[i]->setColour (juce::TextButton::buttonOnColourId,  PnsTheme::kAccentPrimary.withAlpha (0.22f));
            segs[i]->setColour (juce::TextButton::textColourOnId,    PnsTheme::kAccentPrimary);
            segs[i]->setColour (juce::TextButton::textColourOffId,   PnsTheme::kTextSecondary);
            segs[i]->onClick = [this, i] { audioProcessor.setChannelMode (i); };
            addAndMakeVisible (segs[i]);
        }
        segs[juce::jlimit (0, 3, audioProcessor.getChannelMode())]->setToggleState (true, juce::dontSendNotification);
    }

    openCmdMemory();

    switchTab (0);

    setSize (900, 576);   // wider for the renamed tabs; +36px footer
    setResizable (true, true);
    setResizeLimits (820, 486, 2000, 1436);   // 820 fits the renamed 8-tab bar
    startTimerHz (60);
}

PlugNspectrPostEditor::~PlugNspectrPostEditor()
{
    // Zero cmd block so Pre stops the test tone immediately on editor close
    if (m_pCmd != nullptr)
    {
        m_pCmd->testToneActive    = 0;
        m_pCmd->testToneFrequency = 1000.0;
        m_pCmd->measureActive     = 0;
        m_pCmd->dynMeasureMode    = 0;
    }
    closeCmdMemory();
    setLookAndFeel (nullptr);
    stopTimer();
}

//──────────────────────────────────────────────────────────────────────────────
void PlugNspectrPostEditor::openCmdMemory()
{
    if (m_pCmd != nullptr) return;

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

    m_pCmd->testToneActive    = 0;
    m_pCmd->testToneFrequency = m_toneFreq;
    m_pCmd->testToneLevelDb   = m_toneLevel;
    m_pCmd->measureActive     = 0;
    m_pCmd->dynMeasureMode    = 0;
}

void PlugNspectrPostEditor::closeCmdMemory()
{
    if (m_pCmd != nullptr) { UnmapViewOfFile (m_pCmd);  m_pCmd = nullptr; }
    if (m_hCmdFile != nullptr) { CloseHandle (m_hCmdFile); m_hCmdFile = nullptr; }
}

void PlugNspectrPostEditor::writeCmdBlock()
{
    if (m_pCmd == nullptr) return;
    m_pCmd->testToneFrequency = m_toneFreq;
    m_pCmd->testToneLevelDb   = m_toneLevel;
    m_pCmd->testToneActive    = m_toneActive ? 1u : 0u;
    // Noise stimulus only while the Linear tab is showing AND Measure is on.
    m_pCmd->measureActive     = (m_activeTab == 4 && m_linearView.isMeasureActive())
                                ? 1u : 0u;
    // Dynamics stimulus: level ramp on the Transfer tab, level step on the
    // Envelope tab — only while that tab is showing AND Measure is on.
    m_pCmd->dynMeasureMode    = (m_activeTab == 5 && m_transferView.isMeasureActive()) ? 1u
                              : (m_activeTab == 6 && m_envelopeView.isMeasureActive()) ? 2u
                              : (m_activeTab == 7 && m_thdView.isMeasureActive())      ? 3u
                                                                                       : 0u;
}

bool PlugNspectrPostEditor::isStimulusActive() const
{
    if (m_toneActive) return true;
    if (m_activeTab == 4 && m_linearView  .isMeasureActive()) return true;
    if (m_activeTab == 5 && m_transferView.isMeasureActive()) return true;
    if (m_activeTab == 6 && m_envelopeView.isMeasureActive()) return true;
    if (m_activeTab == 7 && m_thdView     .isMeasureActive()) return true;
    return false;
}

bool PlugNspectrPostEditor::disarmAllStimuli()
{
    bool changed = false;
    if (m_toneActive)
    {
        m_toneActive = false;
        m_footerToneBtn.setToggleState (false, juce::dontSendNotification);
        m_harmView.setToneActive (false);
        changed = true;
    }
    changed |= m_linearView  .disarm();
    changed |= m_transferView.disarm();
    changed |= m_envelopeView.disarm();
    changed |= m_thdView     .disarm();
    if (changed) { writeCmdBlock(); repaint(); }
    return changed;
}

//==============================================================================
void PlugNspectrPostEditor::timerCallback()
{
    // ── Transport-stop auto-defeat ──────────────────────────────────────────
    // A measurement stimulus replaces your audio; when the host transport goes
    // from playing to stopped, auto-disarm it so a tone can't keep running.
    const bool playing = audioProcessor.isTransportPlaying();
    if (m_wasTransportPlaying && ! playing)
        disarmAllStimuli();
    m_wasTransportPlaying = playing;

    // ── Overlay animation ──────────────────────────────────────────────────
    // Check Pre active every 200ms (every 12 ticks at 60fps)
    if (m_tickCounter % 12 == 0)
        m_overlayTarget = audioProcessor.isPreActive() ? 0.0f : 1.0f;

    // Smooth fade: slower fade-out (300ms) when Pre connects, faster fade-in
    const float diff      = m_overlayTarget - m_overlayAlpha;
    const float fadeStep  = (diff > 0.0f) ? 0.08f : 0.055f;
    if (std::abs (diff) > 0.004f)
    {
        m_overlayAlpha = juce::jlimit (0.0f, 1.0f, m_overlayAlpha + fadeStep * (diff > 0 ? 1.0f : -1.0f));
        repaint();
    }

    // Pulse phase for the searching dot (~1 cycle per second at 60fps)
    m_pulsePhase += juce::MathConstants<float>::twoPi / 60.0f;
    if (m_pulsePhase > juce::MathConstants<float>::twoPi)
        m_pulsePhase -= juce::MathConstants<float>::twoPi;

    // ── Views ──────────────────────────────────────────────────────────────
    m_dynView.update();
    if (m_activeTab == 1) m_dynView.repaint();

    if (++m_tickCounter % 2 == 0)
    {
        m_specView.update();
        if (m_activeTab == 0) m_specView.repaint();

        m_oscView.update();
        if (m_activeTab == 2) m_oscView.repaint();

        m_harmView.update();
        if (m_activeTab == 3) m_harmView.repaint();

        m_linearView.update();
        if (m_activeTab == 4) m_linearView.repaint();

        m_transferView.update();
        if (m_activeTab == 5) m_transferView.repaint();

        m_envelopeView.update();
        if (m_activeTab == 6) m_envelopeView.repaint();

        m_thdView.update();
        if (m_activeTab == 7) m_thdView.repaint();
    }
}

//==============================================================================
void PlugNspectrPostEditor::paintOverChildren (juce::Graphics& g)
{
    // Amber "measuring" border around the whole window whenever a test signal
    // is being emitted — an unmissable cue that the audio is being replaced.
    if (isStimulusActive())
    {
        g.setColour (PnsTheme::kColorPostAvg);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 6.0f, 3.0f);
    }

    if (m_overlayAlpha < 0.005f) return;

    const float alpha = m_overlayAlpha;
    constexpr int hH  = PnsTheme::kHeaderHeight;
    constexpr int tbH = PnsTheme::kTabBarHeight;
    const int W = getWidth(), H = getHeight();
    const int cY = hH + tbH;
    constexpr int kFooterH = 36;
    const int cH = H - cY - kFooterH;

    // ── Dim overlay over content area ──────────────────────────────────────
    g.setColour (PnsTheme::kBgDark.withAlpha (0.92f * alpha));
    g.fillRect (0, cY, W, cH);

    // ── Content box ────────────────────────────────────────────────────────
    constexpr float boxW = 430.0f;
    constexpr float boxH = 270.0f;
    const float boxX = (float) (W - (int) boxW) * 0.5f;
    const float boxY = (float) cY + ((float) cH - boxH) * 0.5f;

    PnsTheme::drawFrostedPanel (g, { boxX, boxY, boxW, boxH });

    float cy = boxY + 16.0f;

    // ── "No connection" icon (circle with diagonal slash) ──────────────────
    constexpr float iconDia = 44.0f, iconPad = 9.0f;
    const float icX = boxX + (boxW - iconDia) * 0.5f;
    g.setColour (PnsTheme::kColorGainRed.withAlpha (alpha));
    g.drawEllipse (icX, cy, iconDia, iconDia, 2.5f);
    g.drawLine (icX + iconPad,          cy + iconDia - iconPad,
                icX + iconDia - iconPad, cy + iconPad, 2.5f);
    cy += iconDia + 10.0f;

    // ── Title ──────────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontTitle());
    g.setColour (PnsTheme::kTextPrimary.withAlpha (alpha));
    g.drawText ("No Pre Plugin Detected",
                (int) boxX, (int) cy, (int) boxW, 14, juce::Justification::centred);
    cy += 18.0f;

    // ── Subtitle ───────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary.withAlpha (alpha));
    g.drawText ("PlugNspectr requires both Pre and Post plugins to be active.",
                (int) boxX, (int) cy, (int) boxW, 11, juce::Justification::centred);
    cy += 15.0f;

    // ── Divider ────────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kBorderSubtle.withAlpha (alpha));
    {
        const float divX = boxX + (boxW - 200.0f) * 0.5f;
        g.drawHorizontalLine (juce::roundToInt (cy), divX, divX + 200.0f);
    }
    cy += 9.0f;

    // ── Setup instructions ─────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary.withAlpha (alpha));
    g.drawText ("To begin analysis, set up your signal chain:",
                (int) boxX, (int) cy, (int) boxW, 11, juce::Justification::centred);
    cy += 16.0f;

    // ── Signal chain diagram ───────────────────────────────────────────────
    constexpr float bH    = 22.0f;
    constexpr float bW1   = 96.0f;     // "PlugNspectr Pre"
    constexpr float bW2   = 108.0f;    // "Plugin to Analyze"
    constexpr float bW3   = 100.0f;    // "PlugNspectr Post"
    constexpr float arrW  = 20.0f;
    const float chainW    = bW1 + arrW + bW2 + arrW + bW3;
    const float chainX    = boxX + (boxW - chainW) * 0.5f;

    auto drawBlock = [&] (float bx, float bw, juce::Colour border, const char* lbl)
    {
        g.setColour (PnsTheme::kBgWidget.withAlpha (alpha));
        g.fillRoundedRectangle (bx, cy, bw, bH, 3.0f);
        g.setColour (border.withAlpha (alpha));
        g.drawRoundedRectangle (bx + 0.5f, cy + 0.5f, bw - 1.0f, bH - 1.0f, 3.0f, 1.0f);
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextPrimary.withAlpha (alpha * 0.85f));
        g.drawText (lbl, (int) bx, (int) cy, (int) bw, (int) bH, juce::Justification::centred);
    };

    auto drawArrow = [&] (float ax)
    {
        const float ay = cy + bH * 0.5f;
        g.setColour (PnsTheme::kTextSecondary.withAlpha (alpha));
        g.drawLine (ax + 1.0f, ay, ax + arrW - 6.0f, ay, 1.0f);
        juce::Path head;
        const float hx = ax + arrW - 6.0f;
        head.addTriangle (hx, ay - 3.5f, hx + 6.0f, ay, hx, ay + 3.5f);
        g.fillPath (head);
    };

    drawBlock (chainX,                              bW1, PnsTheme::kColorPost,    "PlugNspectr Pre");
    drawArrow (chainX + bW1);
    drawBlock (chainX + bW1 + arrW,                 bW2, PnsTheme::kBorderActive, "Plugin to Analyze");
    drawArrow (chainX + bW1 + arrW + bW2);
    drawBlock (chainX + bW1 + arrW + bW2 + arrW,    bW3, PnsTheme::kColorPostAvg, "PlugNspectr Post");
    cy += bH + 10.0f;

    // ── Note ───────────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kGridLabel.withAlpha (alpha));
    g.drawText ("Both plugins must be on the same audio track and",
                (int) boxX + 10, (int) cy, (int) boxW - 20, 11, juce::Justification::centred);
    cy += 12.0f;
    g.drawText ("PlugNspectr Pre must come before the plugin you want to analyze.",
                (int) boxX + 10, (int) cy, (int) boxW - 20, 11, juce::Justification::centred);
    cy += 16.0f;

    // ── Pulsing search dot + status text ───────────────────────────────────
    const float pulse  = 0.65f + 0.35f * std::sin (m_pulsePhase);
    const float dotR   = 4.0f * pulse;
    constexpr float statusContentW = 172.0f;
    const float dotX   = boxX + (boxW - statusContentW) * 0.5f;
    const float dotCY  = cy + 5.5f;

    g.setColour (PnsTheme::kColorGainRed.withAlpha (alpha * pulse));
    g.fillEllipse (dotX, dotCY - dotR, dotR * 2.0f, dotR * 2.0f);

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary.withAlpha (alpha));
    g.drawText ("Searching for PlugNspectr Pre...",
                (int) (dotX + 11.0f), (int) (dotCY - 5.5f),
                (int) (statusContentW - 11.0f), 11,
                juce::Justification::centredLeft);
}

//==============================================================================
void PlugNspectrPostEditor::switchTab (int index)
{
    m_activeTab = index;

    juce::TextButton* tabs[8] = { &m_tabSpectrum, &m_tabDynamics, &m_tabOscilloscope,
                                   &m_tabHarmonics, &m_tabLinear, &m_tabTransfer,
                                   &m_tabEnvelope, &m_tabThd };
    for (int i = 0; i < 8; ++i)
    {
        const bool active = (i == index);
        tabs[i]->setColour (juce::TextButton::textColourOffId,
                            active ? PnsTheme::kTextPrimary : PnsTheme::kTextSecondary);
        tabs[i]->getProperties().set ("tabActive", active);
        tabs[i]->repaint();
    }

    m_specView    .setVisible (index == 0);
    m_dynView     .setVisible (index == 1);
    m_oscView     .setVisible (index == 2);
    m_harmView    .setVisible (index == 3);
    m_linearView  .setVisible (index == 4);
    m_transferView.setVisible (index == 5);
    m_envelopeView.setVisible (index == 6);
    m_thdView     .setVisible (index == 7);

    writeCmdBlock();   // start/stop the measurement noise with the tab
    repaint();
}

//==============================================================================
void PlugNspectrPostEditor::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);

    // ── Header bar ────────────────────────────────────────────────────────
    constexpr int hH = PnsTheme::kHeaderHeight;
    g.setColour (juce::Colour (10, 10, 10));   // #0A0A0A — slightly darker than panel
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

    // "CH" label left of the L/R/M/S segmented selector
    g.drawText ("CH", m_chL.getX() - 26, (hH - 12) / 2, 22, 12, juce::Justification::centredRight);

    // ── Test-signal tray — recessed amber group behind the test-signal tabs ─
    {
        const auto tray = m_trayBounds.toFloat();
        g.setColour (PnsTheme::kColorPostAvg.withAlpha (0.07f));
        g.fillRoundedRectangle (tray, 5.0f);
        g.setColour (PnsTheme::kColorPostAvg.withAlpha (0.55f));
        g.fillRect (tray.getX(), tray.getY(), tray.getWidth(), 2.0f);
        g.setColour (PnsTheme::kColorPostAvg);
        g.fillEllipse (tray.getX() + 7.0f, tray.getCentreY() - 3.0f, 6.0f, 6.0f);
    }

    // ── Tab bar bottom border ─────────────────────────────────────────────
    constexpr int tbBottom = hH + PnsTheme::kTabBarHeight;
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawHorizontalLine (tbBottom - 1, 0.0f, (float) getWidth());

    // ── Measuring banner — reserved strip below the tab bar ────────────────
    {
        constexpr int kBannerH = 20;
        if (isStimulusActive())
        {
            g.setColour (PnsTheme::kColorPostAvg.withAlpha (0.16f));
            g.fillRect (0, tbBottom, getWidth(), kBannerH);
            g.setColour (PnsTheme::kColorPostAvg);
            g.fillRect (0, tbBottom, 4, kBannerH);
            g.fillEllipse (16.0f, (float) tbBottom + (float) kBannerH * 0.5f - 3.0f, 6.0f, 6.0f);
            g.setFont (PnsTheme::fontLabel());
            g.setColour (PnsTheme::kColorPostAvg);
            g.drawText ("Measuring - your audio is replaced by a test signal. Auto-stops when you stop playback or leave this tab.",
                        30, tbBottom, getWidth() - 40, kBannerH, juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (PnsTheme::kBorderSubtle);
            g.drawHorizontalLine (tbBottom + kBannerH - 1, 0.0f, (float) getWidth());
        }
    }

    // ── Footer bar ────────────────────────────────────────────────────────
    constexpr int kFooterH = 36;
    const int H2 = getHeight(), W2 = getWidth();
    const int footerY = H2 - kFooterH;

    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (0, footerY, W2, kFooterH);
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawHorizontalLine (footerY, 0.0f, (float) W2);

    // "FREQ" label — to the right of the knob (knob is 28px wide at kPaddingLarge)
    constexpr int knobX = PnsTheme::kPaddingLarge;
    constexpr int knobW = 28;
    const int freqLabelX = knobX + knobW + 4;
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("FREQ", freqLabelX, footerY, 36, kFooterH, juce::Justification::centredLeft);

    // Frequency value display
    const double freq = m_toneFreq;
    juce::String freqStr;
    if (freq < 1000.0)
        freqStr = juce::String (juce::roundToInt (freq)) + " Hz";
    else
        freqStr = juce::String (freq / 1000.0, 2) + " kHz";

    g.setColour (m_toneActive ? PnsTheme::kTextAccent : PnsTheme::kTextSecondary);
    g.drawText (freqStr, freqLabelX + 36, footerY, 64, kFooterH, juce::Justification::centredLeft);

    // "LEVEL" label + value — knob is positioned in resized() just before this.
    const int lvlLabelX = freqLabelX + 100 + 28 + 6;
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("LEVEL", lvlLabelX, footerY, 40, kFooterH, juce::Justification::centredLeft);
    g.setColour (m_toneActive ? PnsTheme::kTextAccent : PnsTheme::kTextSecondary);
    g.drawText (juce::String (m_toneLevel, 1) + " dB", lvlLabelX + 40, footerY, 64, kFooterH,
                juce::Justification::centredLeft);

    // Warning label — only when test tone is active
    if (m_toneActive)
    {
        // Position warning 8px right of the "Test Tone" button
        // Button center is W2/2, button width is 80, so right edge is W2/2 + 40
        const int warnX = W2 / 2 + 48;
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kColorGainRed);
        g.drawText (juce::CharPointer_UTF8 ("\xe2\x9a\xa0 Replaces audio output"),
                    warnX, footerY, W2 - warnX - PnsTheme::kPaddingMid, kFooterH,
                    juce::Justification::centredLeft);
    }
}

//==============================================================================
void PlugNspectrPostEditor::resized()
{
    constexpr int hH     = PnsTheme::kHeaderHeight;
    constexpr int tbH    = PnsTheme::kTabBarHeight;
    constexpr int kBannerH = 20;   // reserved strip below the tab bar (measuring banner)

    // Variable-width tabs: live group (left), then test-signal group on the
    // amber tray. Width = label width + padding so the renamed tabs all fit.
    auto tabW = [] (juce::TextButton& b)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (PnsTheme::fontPrimary(), b.getButtonText(), 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + 22;
    };
    int x = 8;
    auto place = [&] (juce::TextButton& b) { const int w = tabW (b); b.setBounds (x, hH, w, tbH); x += w; };

    place (m_tabSpectrum); place (m_tabDynamics); place (m_tabOscilloscope);

    constexpr int trayGap = 12, trayPadL = 18, trayPadR = 10;
    x += trayGap;
    const int trayLeft = x;
    x += trayPadL;
    place (m_tabHarmonics); place (m_tabLinear); place (m_tabTransfer);
    place (m_tabEnvelope);  place (m_tabThd);
    m_trayBounds = { trayLeft, hH, (x + trayPadR) - trayLeft, tbH };

    // Segmented L/R/M/S selector in the header, left of the version text.
    {
        constexpr int segW = 28, segH = 22;
        const int segRight = getWidth() - PnsTheme::kPaddingMid - 40 - 8;
        const int segTop   = (hH - segH) / 2;
        m_chL.setBounds (segRight - segW * 4, segTop, segW, segH);
        m_chR.setBounds (segRight - segW * 3, segTop, segW, segH);
        m_chM.setBounds (segRight - segW * 2, segTop, segW, segH);
        m_chS.setBounds (segRight - segW,     segTop, segW, segH);
    }

    constexpr int kFooterH = 36;
    const int W = getWidth(), H = getHeight();

    const auto content = getLocalBounds().withTrimmedTop (hH + tbH + kBannerH).withTrimmedBottom (kFooterH);
    m_specView  .setBounds (content);
    m_dynView   .setBounds (content);
    m_oscView   .setBounds (content);
    m_harmView    .setBounds (content);
    m_linearView  .setBounds (content);
    m_transferView.setBounds (content);
    m_envelopeView.setBounds (content);
    m_thdView     .setBounds (content);

    // Footer controls
    m_footerFreqSlider.setBounds (PnsTheme::kPaddingLarge, H - kFooterH + 4, 28, 28);
    // Level knob sits after the FREQ knob + label + value (~100px of text).
    const int lvlKnobX = PnsTheme::kPaddingLarge + 28 + 4 + 100;
    m_footerLevelSlider.setBounds (lvlKnobX, H - kFooterH + 4, 28, 28);
    m_footerToneBtn   .setBounds ((W - 80) / 2, H - kFooterH + 7, 80, 22);
}
