/*
  ==============================================================================
    PlugNspectrPost  –  PluginEditor.cpp
  ==============================================================================
*/

#include "PluginEditor.h"
#include "PnsLogoData.h"
#include <cmath>
#include <algorithm>
#include <set>

//==============================================================================
// SpectrumView
//==============================================================================
SpectrumView::SpectrumView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    m_options.addRow ("Smoothing", "How quickly the curves follow the music.",
                      { "Fast", "Medium", "Slow" }, 1,
                      [this] (int i) { setSmoothPreset (i == 0 ? SmoothPreset::Fast
                                                      : i == 1 ? SmoothPreset::Medium : SmoothPreset::Slow); });
    m_options.addRow ("Long-term average", "A steady line of the overall tone.",
                      { "Off", "On" }, m_showAvg ? 1 : 0,
                      [this] (int i) { m_showAvg = (i == 1); repaint(); });
    m_options.setResetAction ("Clears the averages and markers.", [this] { reset(); });
    addAndMakeVisible (m_options);

    setInterceptsMouseClicks (true, false);
}

// Clear the accumulated long-term state so the averaged Pre/Post comparison and
// the fluctuation markers reflect only post-tweak audio. Seeds the display /
// peak-hold / averages from the current spectrum so it reads correctly at once
// rather than climbing from silence.
void SpectrumView::reset()
{
    m_pre      = m_rawPre;   m_post      = m_rawPost;
    m_peakPre  = m_rawPre;   m_peakPost  = m_rawPost;
    m_avgPre   = m_rawPre;   m_avgPost   = m_rawPost;

    // Re-arm the fluctuation markers' warmup (mirrors the silence-reset path).
    m_warmupN  = 0;
    m_markerN  = 0;
    m_silenceN = 0;
    m_peakMarkers.clear();
    m_topBins.fill (-1);
    m_topScores.fill (0.0f);
    repaint();
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
    m_options.place (getWidth() - PnsTheme::kPaddingMid, PnsTheme::kPaddingSmall);
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
    PnsTheme::drawPlotCard (g, { px, py, pw, ph });

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
    m_options.addRow ("History", "How many seconds of audio the graphs show.",
                      { "6 s", "12 s" }, m_zoomSeconds == 6 ? 0 : 1,
                      [this] (int i) { m_zoomSeconds = (i == 0 ? 6 : 12); repaint(); });
    m_options.setResetAction ("Clears every reading and the history.", [this] { reset(); });
    addAndMakeVisible (m_options);

    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void DynamicsView::resized()
{
    m_options.place (getWidth() - PnsTheme::kPaddingMid, PnsTheme::kPaddingSmall);
}

void DynamicsView::update()
{
    // Drain the capture ring once per tick; the waveform and the GR/level block
    // below both consume this same chunk. This used to be gated to 30 fps "so
    // scroll speed is constant", which was compensating for the old snapshot API
    // dropping blocks — with a ring the scroll rate follows the sample count and
    // is correct at any buffer size, so the gate is gone.
    const uint64_t capBefore = m_capReadPos;
    const int      capN       = m_proc.readCaptureSince (m_capReadPos, m_capPre, m_capPost);
    // If the UI stalled longer than the ring holds, some audio is gone. Advance
    // the absolute clock by the elapsed amount, not just what survived, or the
    // waveform stays permanently time-compressed after the stall.
    const uint64_t capElapsed = m_capReadPos - capBefore;

    // ── Waveform sample buffer ────────────────────────────────────────────
    {
        // Every reader indexes the ring by ABSOLUTE sample (a % kSampleBufLen),
        // so the writer must too. If the UI stalled long enough for audio to be
        // lost, blank that stretch at its true position rather than packing the
        // survivors in early — otherwise the write position and the absolute
        // clock drift apart for good and every later read mixes eras.
        if (capN > 0)
        {
            const juce::int64 lost = juce::jmax ((juce::int64) 0, (juce::int64) capElapsed - capN);
            for (juce::int64 g = 0, ng = juce::jmin (lost, (juce::int64) kSampleBufLen); g < ng; ++g)
            {
                const auto idx = (size_t) ((m_totalSamplesWritten + lost - ng + g) % kSampleBufLen);
                m_preSamples[idx] = m_postSamples[idx] = 0.0f;
            }
            m_totalSamplesWritten += lost;

            const float* postData = m_capPost.getReadPointer (0);
            const float* preData  = m_capPre .getReadPointer (0);
            const int skip = juce::jmax (0, capN - kSampleBufLen);   // only the newest ring-full survives
            for (int i = skip; i < capN; ++i)
            {
                const auto idx = (size_t) ((m_totalSamplesWritten + i) % kSampleBufLen);
                m_postSamples[idx] = postData ? postData[i] : 0.0f;
                m_preSamples [idx] = preData  ? preData[i]  : 0.0f;
            }
            m_totalSamplesWritten += capN;
            m_sampleWritePos = (int) (m_totalSamplesWritten % kSampleBufLen);
            m_samplesStored  = (int) juce::jmin ((juce::int64) kSampleBufLen,
                                                 (juce::int64) m_samplesStored + lost + capN);
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
                int idx    = (int) (a0 % kSampleBufLen);
                int preIdx = (int) (((a0 - m_lagSamples) % kSampleBufLen + kSampleBufLen) % kSampleBufLen);
                for (juce::int64 a = a0; a < a1; ++a)
                {
                    const float ps  = m_preSamples [preIdx];
                    const float qs  = m_postSamples[idx];
                    if (++preIdx == kSampleBufLen) preIdx = 0;
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
                // Count the audio actually ingested. This used to add sr/60 per
                // tick, i.e. assume the timer really runs at 60 Hz — the very
                // assumption the capture ring exists to stop relying on. Under
                // load that made the window shorter than the 2 s it claims.
                const bool firstAccum = (m_waveCalibSamples == 0);
                m_waveCalibPeak     = juce::jmax (m_waveCalibPeak, winPeak);
                m_waveCalibSamples += capN;

                // Follow the running peak for the whole window rather than sitting
                // on a fixed guess and snapping at the end. The first tick with
                // signal jumps straight to the right ballpark, so the view is
                // usable immediately; after that the scale eases toward the target
                // (~90 ms). Because the peak is a running maximum, the target only
                // ever decreases, so this is a smooth monotonic zoom-out with no
                // hunting — then it locks, which keeps quiet passages from zooming
                // back in.
                // 0.65 → calibration peak fills ~65% of half-height (headroom)
                const float target = juce::jlimit (1.0f, 64.0f, 0.65f / m_waveCalibPeak);
                m_waveScale = firstAccum ? target
                                         : m_waveScale + (target - m_waveScale) * 0.18f;

                if (m_waveCalibSamples >= (int) (sr * 2.0) && m_waveCalibPeak > kSilence)
                {
                    m_waveScale       = target;
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

    // ── Delay-aligned GR + rolling average + smoothed RMS volume ─────────
    // Only when the ring actually produced audio. The UI ticks at 60 Hz but at
    // larger buffer sizes blocks arrive less often, so some ticks legitimately
    // drain nothing — treating those as "GR = 0" injected spurious zeros into the
    // history and the rolling average (a visible sawtooth).
    if (capN > 0)
    {
        const double sr = m_proc.getSampleRate();

        // Gain reduction over everything drained this update (at least one
        // ~30 ms window), Pre read back by the measured lag so both hold the
        // same audio. Mean feeds the averages; the deepest window feeds "cut
        // now", the lane and the peak hold, so a brief grab isn't missed.
        const int W   = juce::jmax (64, juce::roundToInt (juce::jmax (1.0, sr) * 0.03));
        const int len = juce::jmin (juce::jmax (capN, W), kSampleBufLen - m_lagSamples - 1);
        pns::analysis::GainReduction measured;
        if (len > 0 && (juce::int64) m_samplesStored >= (juce::int64) len + m_lagSamples)
        {
            m_alignPre .resize ((size_t) len);
            m_alignPost.resize ((size_t) len);
            const juce::int64 start = m_totalSamplesWritten - len;
            for (int i = 0; i < len; ++i)
            {
                m_alignPost[(size_t) i] = m_postSamples[(size_t) ((start + i) % kSampleBufLen)];
                m_alignPre [(size_t) i] = m_preSamples [(size_t) ((start + i - m_lagSamples) % kSampleBufLen)];
            }
            measured = pns::analysis::gainReduction (m_alignPre.data(), m_alignPost.data(), len, W);
        }
        const bool  audible   = measured.audible;
        const float currentGr = measured.meanDb;
        const float deepestGr = measured.minDb;

        const auto rms = m_proc.getRms();
        m_preConnected = rms.preValid;

        if (rms.preValid)
        {
            // Instantaneous hold: keep last non-zero reading
            if (deepestGr < -0.1f)
                m_instantGr = deepestGr;

            // 30 s rolling average over the time audio was playing — moments
            // with no cut count as 0. (Averaging only the non-zero moments made
            // a few stray dips read as a steady cut.) Silence stores +1, skipped.
            m_avgGrBuf[m_avgGrPos] = audible ? currentGr : 1.0f;
            m_avgGrDur[m_avgGrPos] = capN;
            m_avgGrPos  = (m_avgGrPos + 1) % kAvgGrLen;
            m_avgGrFill = juce::jmin (m_avgGrFill + 1, kAvgGrLen);

            // Newest first, weighted by the audio each entry covers, back to
            // 30 s of audio (silence counts toward the 30 s but not the mean).
            const double span = 30.0 * juce::jmax (1.0, sr);
            double sum = 0.0, heard = 0.0, covered = 0.0;
            for (int k = 0; k < m_avgGrFill && covered < span; ++k)
            {
                const int    i = (m_avgGrPos - 1 - k + kAvgGrLen) % kAvgGrLen;
                const double d = (double) m_avgGrDur[(size_t) i];
                covered += d;
                if (m_avgGrBuf[(size_t) i] <= 0.0f) { sum += m_avgGrBuf[(size_t) i] * d; heard += d; }
            }
            m_avgGr = heard > 0.0 ? (float) (sum / heard) : 0.0f;

            // Auto-restart on a lasting change (see m_fastGr). Time-based, since
            // ticks with audio arrive at the host's block rate, not 60 Hz.
            // The bar scales with the cut: a compressor riding 6 dB on average
            // swings a few dB between phrases, which must not count as a change
            // of plugin. 3 s of sustained disagreement, and at least 2 s of
            // audio in the average first, so it never fires on a warm-up.
            if (audible)
            {
                const double nowMs = juce::Time::getMillisecondCounterHiRes();
                const float  k     = (float) juce::jlimit (0.0, 1.0, (double) capN / (0.5 * sr));
                m_fastGr += k * (currentGr - m_fastGr);
                const float bar = juce::jmax (1.5f, 0.4f * std::abs (m_avgGr));
                if (heard > 2.0 * sr && std::abs (m_fastGr - m_avgGr) > bar)
                {
                    if (m_shiftSinceMs == 0.0) m_shiftSinceMs = nowMs;
                    else if (nowMs - m_shiftSinceMs > 3000.0)
                    {
                        resetReadings();
                        m_restartNoteEnd = nowMs + 4000.0;
                    }
                }
                else
                    m_shiftSinceMs = 0.0;
            }
            else
                m_shiftSinceMs = 0.0;   // a pause breaks the run; it must be sustained while playing

            // Smooth RMS volume (0.9 decay)
            constexpr float kSmooth = 0.9f;
            m_smoothPreDb  = m_smoothPreDb  * kSmooth + rms.preDb  * (1.0f - kSmooth);
            m_smoothPostDb = m_smoothPostDb * kSmooth + rms.postDb * (1.0f - kSmooth);
        }

        m_gr[m_grPos] = rms.preValid
            ? juce::jlimit (-24.0f, 0.0f, deepestGr)
            : 0.0f;
        m_grPos = (m_grPos + 1) % kGrLen;

        // Peak hold: track most-negative GR, decay at 0.998 per frame (~3s hold)
        if (rms.preValid && deepestGr < m_grPeakHold)
            m_grPeakHold = deepestGr;
        else
            m_grPeakHold = juce::jmin (0.0f, m_grPeakHold * 0.998f);
    }
}

void DynamicsView::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-clicking the readout chips is a shortcut for Start over.
    if (m_chipBounds.contains (e.getPosition()))
        reset();
}

// Clear just the cut readings — used by the auto-restart when the plugin's
// behaviour changes for good. The waveform history stays: it's still true.
void DynamicsView::resetReadings()
{
    m_instantGr  = 0.0f;
    m_avgGr      = 0.0f;
    m_grPeakHold = 0.0f;
    m_avgGrBuf.fill (0.0f);
    m_avgGrDur.fill (0);
    m_avgGrPos   = 0;
    m_avgGrFill  = 0;
    m_gr.fill (0.0f);
    m_grPos      = 0;
    m_shiftSinceMs = 0.0;
    // m_fastGr is kept: it already reflects the new state.
}

// Clear every accumulated reading and history so measurement starts over after a
// tweak to the plugin chain. The auto-zoom is intentionally left untouched — it
// already tracks the signal, and resetting it would just re-calibrate/flicker.
void DynamicsView::reset()
{
    // Waveform history (ring buffer + absolute anchor + derived columns).
    m_preSamples.fill (0.0f);
    m_postSamples.fill (0.0f);
    m_sampleWritePos      = 0;
    m_samplesStored       = 0;
    m_totalSamplesWritten = 0;
    m_wavePreTop.fill (0.0f);  m_wavePreBot.fill (0.0f);
    m_wavePostTop.fill (0.0f); m_wavePostBot.fill (0.0f);
    m_waveColsValid  = 0;
    m_waveScrollFrac = 0.0f;

    // GR readouts + the 30 s rolling Avg GR buffer.
    m_instantGr  = 0.0f;
    m_avgGr      = 0.0f;
    m_grPeakHold = 0.0f;
    m_avgGrBuf.fill (0.0f);
    m_avgGrDur.fill (0);
    m_avgGrPos   = 0;
    m_avgGrFill  = 0;
    m_fastGr       = 0.0f;
    m_shiftSinceMs = 0.0;

    // Smoothed In/Out levels and the scrolling GR history plot.
    m_smoothPreDb  = -90.0f;
    m_smoothPostDb = -90.0f;
    m_gr.fill (0.0f);
    m_grPos = 0;

    // Brief flash for feedback (same as the old double-click).
    m_grFlashEnd = juce::Time::getMillisecondCounterHiRes() + 200.0;
    repaint();
}

void DynamicsView::mouseMove (const juce::MouseEvent& e)
{
    const bool over = m_chipBounds.contains (e.getPosition());
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
    PnsTheme::drawPanelTitle (g, area.withHeight (16.0f), "LEVEL OVER TIME", "pre vs post waveform");

    // ── Background ────────────────────────────────────────────────────────
    PnsTheme::drawPlotCard (g, { px, py, pw, ph });

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

    // ── Readout chips along the bottom of the card ───────────────────────
    //    The average cut is the headline number above the view, so the chips
    //    carry what it doesn't: the cut right now, and the loudness in / out.
    {
        auto dbStr = [] (float v) -> juce::String
        {
            return v <= -89.0f ? juce::String ("---") : juce::String (v, 1) + " dB";
        };
        const bool  flashing = (juce::Time::getMillisecondCounterHiRes() < m_grFlashEnd);
        const float delta    = m_smoothPostDb - m_smoothPreDb;
        juce::String deltaStr = m_smoothPreDb <= -89.0f ? juce::String ("---")
                              : (delta >= 0.0f ? "+" : "") + juce::String (delta, 1) + " dB";

        struct Chip { const char* label; juce::String value; juce::Colour col; };
        const Chip chips[] = {
            { "Cut now",  m_instantGr <= -0.1f ? juce::String (-m_instantGr, 1) + " dB" : juce::String ("0.0 dB"),
              PnsTheme::kColorGainRed },
            { "Loudness in",  dbStr (m_smoothPreDb),  PnsTheme::kTextPrimary },
            { "Loudness out", dbStr (m_smoothPostDb), PnsTheme::kTextPrimary },
            { "Change",   deltaStr, std::abs (delta) > 0.1f ? PnsTheme::kAccentPrimary : PnsTheme::kTextSecondary },
        };

        constexpr float chipW = 116.0f, chipH = 44.0f, chipGap = 8.0f;
        float cx = px + 10.0f;
        const float cy = py + ph - chipH - 10.0f;
        for (const auto& c : chips)
        {
            const juce::Rectangle<float> r (cx, cy, chipW, chipH);
            g.setColour (PnsTheme::kBgDark.withAlpha (0.78f));
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (PnsTheme::kBorderSubtle);
            g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
            g.setFont (PnsTheme::fontLabel());
            g.setColour (PnsTheme::kTextSecondary);
            g.drawText (c.label, r.reduced (10.0f, 6.0f).withHeight (13.0f), juce::Justification::centredLeft);
            g.setFont (PnsTheme::fontSized (16.0f, true));
            g.setColour (flashing ? juce::Colours::white : c.col);
            g.drawText (c.value, r.reduced (10.0f, 6.0f).withTrimmedTop (14.0f), juce::Justification::centredLeft);
            cx += chipW + chipGap;
        }
        m_chipBounds = juce::Rectangle<float> (px + 10.0f, cy, cx - chipGap - (px + 10.0f), chipH).toNearestInt();

        if (juce::Time::getMillisecondCounterHiRes() < m_restartNoteEnd)
        {
            g.setFont (PnsTheme::fontLabel());
            g.setColour (PnsTheme::kAccentPrimary);
            g.drawText ("Readings restarted - the plugin's behaviour changed",
                        juce::roundToInt (cx + 4.0f), juce::roundToInt (cy), 320,
                        juce::roundToInt (chipH), juce::Justification::centredLeft);
        }
        else if (m_mouseOverReadout)
        {
            g.setFont (PnsTheme::fontLabel());
            g.setColour (PnsTheme::kGridLabel);
            g.drawText ("Double-click to start over", juce::roundToInt (cx + 4.0f), juce::roundToInt (cy),
                        160, juce::roundToInt (chipH), juce::Justification::centredLeft);
        }
    }
}

//──────────────────────────────────────────────────────────────────────────────
void DynamicsView::drawGrMeter (juce::Graphics& g, juce::Rectangle<float> area)
{
    constexpr float kML = 42.0f, kMR = 12.0f, kMT = 20.0f, kMB = 20.0f;
    // Scale to the deepest cut in view (history + peak hold) with headroom, on
    // round steps so the axis doesn't twitch: 1.4 dB of cut fills a 3 dB lane
    // instead of being a sliver at the top of a fixed 24 dB one.
    float deepest = -m_grPeakHold;
    for (float v : m_gr) deepest = juce::jmax (deepest, -v);
    const float kMinGr = deepest * 1.2f <= 3.0f ? -3.0f : deepest * 1.2f <= 6.0f ? -6.0f
                       : deepest * 1.2f <= 12.0f ? -12.0f : -24.0f;
    constexpr float kMaxGr = 0.0f;

    const float px = area.getX() + kML,  py = area.getY() + kMT;
    const float pw = area.getWidth()  - kML - kMR;
    const float ph = area.getHeight() - kMT - kMB;
    if (pw <= 0 || ph <= 0) return;

    auto grToY = [&] (float gr) -> float {
        return py + ph * (-juce::jlimit (kMinGr, kMaxGr, gr)) / (-kMinGr);
    };

    PnsTheme::drawPanelTitle (g, area.withHeight (16.0f), "HOW MUCH IT TURNS DOWN", "gain reduction, dB");

    PnsTheme::drawPlotCard (g, { px, py, pw, ph });

    const int divs = kMinGr >= -6.0f ? 3 : 4;   // whole-dB steps: 1, 2, 3 or 6 dB
    for (int i = 0; i <= divs; ++i)
    {
        const float gr = kMinGr * (float) i / (float) divs;
        const float gy = grToY (gr);
        g.setColour (gr == 0.0f ? PnsTheme::kZeroLine : PnsTheme::kGridLine);
        g.drawHorizontalLine (juce::roundToInt (gy), px, px + pw);

        const juce::String lbl (juce::roundToInt (gr));
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
        juce::ColourGradient grad (PnsTheme::kColorGainRed.withAlpha (0.30f), px, py,
                                   PnsTheme::kColorGainRed.withAlpha (0.10f), px, py + ph,
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
    m_options.addRow ("Zoom", "How much time fits across the screen.",
                      { "10 ms", "50 ms", "100 ms" }, m_windowMs == 10 ? 0 : (m_windowMs == 50 ? 1 : 2),
                      [this] (int i) { setWindowMs (i == 0 ? 10 : i == 1 ? 50 : 100); });
    m_options.addRow ("Levels", "Matched scales Post to Pre's loudness, so only shape differences show.",
                      { "Matched", "As played" }, 0,
                      [this] (int i) { m_matchLevels = (i == 0); repaint(); });
    m_options.addRow ("Update", "How often a new snapshot is drawn. Hold freezes it.",
                      { "4x a second", "Live", "Hold" }, 0,
                      [this] (int i) { m_refresh = i == 0 ? Refresh::Slow : i == 1 ? Refresh::Live : Refresh::Hold;
                                       m_lastCaptureMs = 0.0; repaint(); });
    addAndMakeVisible (m_options);
}

void OscilloscopeView::resized()
{
    m_options.place (getWidth() - PnsTheme::kPaddingMid, PnsTheme::kPaddingSmall);
}

//──────────────────────────────────────────────────────────────────────────────
void OscilloscopeView::setWindowMs (int ms)
{
    m_windowMs   = ms;
    m_hasDisplay = false;            // a new window needs a fresh snapshot
    m_searchFrom = m_totalWritten;
}

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
        // Drain the ring: every sample, so the 10/50/100 ms windows mean what they
        // say regardless of the host buffer size.
        const int n = m_proc.readCaptureSince (m_capReadPos, m_capPre, m_capPost);
        if (n > 0)
        {
            const float* prePtr  = m_capPre .getReadPointer (0);
            const float* postPtr = m_capPost.getReadPointer (0);

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

    // Throttle: keep the current picture until it is time for a new one.
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (m_refresh == Refresh::Hold && m_hasDisplay) return;
    if (m_refresh == Refresh::Slow && m_hasDisplay && nowMs - m_lastCaptureMs < 250.0) return;

    const int captureTarget = juce::jmin ((int) (sr * m_windowMs / 1000.0), kMaxCapture);
    if (captureTarget <= 1) return;

    if (m_totalWritten < (uint64_t) captureTarget) return;
    const uint64_t searchEnd = m_totalWritten - (uint64_t) captureTarget;

    // Leave room behind the trigger for Pre, which is read m_lag samples earlier.
    const uint64_t oldest      = m_totalWritten - (uint64_t) m_ringAvail;
    // When throttled, skip ahead to recent audio rather than replaying the backlog.
    const uint64_t recent      = searchEnd > (uint64_t) captureTarget * 4 ? searchEnd - (uint64_t) captureTarget * 4 : 0;
    const uint64_t searchStart = std::max ({ m_searchFrom, oldest + 1 + (uint64_t) m_lag,
                                               m_refresh == Refresh::Live ? (uint64_t) 0 : recent });

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
                m_displayPre [s] = ringGet (m_ringPre,  i - (uint64_t) m_lag + (uint64_t) s);
                m_displayPost[s] = ringGet (m_ringPost, i + (uint64_t) s);
            }
            m_displayCount = captureTarget;
            m_hasDisplay   = true;
            m_searchFrom   = i + (uint64_t) captureTarget;
            m_lastCaptureMs = nowMs;
            m_shapePre  = pns::analysis::measureShape (m_displayPre .data(), m_displayCount);
            m_shapePost = pns::analysis::measureShape (m_displayPost.data(), m_displayCount);
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

    // Post's display gain: matched to Pre's loudness (RMS), or 1 = as played.
    // Bounded to ±24 dB so a near-silent output isn't blown up into noise.
    const float postGain = (m_matchLevels && m_shapePost.rms > 1.0e-6f && m_shapePre.rms > 1.0e-6f)
                         ? juce::jlimit (1.0f / 16.0f, 16.0f, m_shapePre.rms / m_shapePost.rms) : 1.0f;
    // Vertical range: the smallest of 1 / 0.5 / 0.25 / 0.125 that fits both
    // traces, so quiet material still fills the height.
    const float tallest = juce::jmax (m_shapePre.peak, m_shapePost.peak * postGain);
    float range = 1.0f;
    while (range > 0.125f && tallest > 0.0f && tallest < range * 0.45f) range *= 0.5f;

    const float midY = py + ph * 0.5f;
    auto ampToY = [&] (float a) -> float {
        return midY - juce::jlimit (-1.0f, 1.0f, a / range) * ph * 0.5f;
    };

    // ── Background ────────────────────────────────────────────────────────
    g.fillAll (PnsTheme::kBgDark);
    PnsTheme::drawPlotCard (g, { px, py, pw, ph });

    // ── Title ─────────────────────────────────────────────────────────────
    PnsTheme::drawPanelTitle (g, { px, 0.0f, pw, kMT }, "WAVEFORM CLOSE-UP", "oscilloscope");

    // ── Grid — solid for clip lines, dotted for sub-divisions ─────────────
    // 0 dBFS lines only when the full scale is showing — then they mean clipping.
    g.setColour (range >= 1.0f ? PnsTheme::kClipLine : PnsTheme::kGridLine);
    g.drawHorizontalLine (juce::roundToInt (ampToY ( range)), px, px + pw);
    g.drawHorizontalLine (juce::roundToInt (ampToY (-range)), px, px + pw);

    g.setColour (PnsTheme::kGridLine);
    PnsTheme::drawDottedHLine (g, ampToY ( range * 0.5f), px, px + pw);
    PnsTheme::drawDottedHLine (g, ampToY (-range * 0.5f), px, px + pw);

    g.setColour (PnsTheme::kZeroLine);
    g.drawHorizontalLine (juce::roundToInt (midY), px, px + pw);

    g.setColour (PnsTheme::kGridLine);
    for (int q = 1; q <= 3; ++q)
        PnsTheme::drawDottedVLine (g, px + pw * (float) q / 4.0f, py, py + ph);

    // ── Y axis labels ─────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kGridLabel);
    {
        auto lbl = [] (float v)
        {
            const juce::String t = juce::String (std::abs (v), 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
            return (v > 0.0f ? "+" : v < 0.0f ? "-" : "") + (t.startsWith ("0.") ? t.substring (1) : t);
        };
        for (float v : { range, range * 0.5f, 0.0f, -range * 0.5f, -range })
            g.drawText (lbl (v), 0, juce::roundToInt (ampToY (v)) - 6, juce::roundToInt (kML) - 4, 12,
                        juce::Justification::centredRight);
    }

    // ── Waveforms ─────────────────────────────────────────────────────────
    if (m_hasDisplay && m_displayCount > 1)
    {
        const int drawPts = juce::jmin (m_displayCount, juce::jmax (2, (int) pw));
        const float step  = (float) m_displayCount / (float) drawPts;

        auto buildPath = [&] (const std::array<float, kMaxCapture>& buf, float gain) -> juce::Path
        {
            juce::Path p;
            for (int i = 0; i < drawPts; ++i)
            {
                const float fIdx = i * step;
                const int   lo   = (int) fIdx;
                const int   hi   = juce::jmin (lo + 1, m_displayCount - 1);
                const float t    = fIdx - (float) lo;
                const float val  = (buf[lo] + t * (buf[hi] - buf[lo])) * gain;
                const float x    = px + pw * (float) i / (float) (drawPts - 1);
                const float y    = ampToY (val);
                if (i == 0) p.startNewSubPath (x, y);
                else        p.lineTo (x, y);
            }
            return p;
        };

        // Pre — the reference, grey and a little heavier than before so it
        // reads under Post; Post on top, level-matched unless "As played".
        g.setColour (PnsTheme::kColorPre.withAlpha (0.75f));
        g.strokePath (buildPath (m_displayPre, 1.0f), juce::PathStrokeType (1.4f));
        PnsTheme::drawGlowLine (g, buildPath (m_displayPost, postGain), PnsTheme::kColorPost, 1.6f);
    }
    else
    {
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kGridLine);
        g.drawText ("Waiting for trigger...",
                    juce::roundToInt (px), juce::roundToInt (midY - 8),
                    juce::roundToInt (pw), 16, juce::Justification::centred);
    }

    // ── Key ───────────────────────────────────────────────────────────────
    {
        const juce::String postLabel = m_matchLevels && std::abs (postGain - 1.0f) > 0.01f
            ? "Out of your plugin (level-matched)" : "Out of your plugin";
        const float kx = px + 10.0f, ky = py + 8.0f;
        PnsTheme::drawFrostedPanel (g, { px + 4.0f, py + 4.0f, 228.0f, 36.0f });
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kColorPre);
        g.fillRoundedRectangle (kx, ky + 4.0f, 14.0f, 3.0f, 1.5f);
        g.setColour (PnsTheme::kTextPrimary);
        g.drawText ("Into your plugin", juce::roundToInt (kx + 20.0f), juce::roundToInt (ky), 190, 12,
                    juce::Justification::centredLeft);
        g.setColour (PnsTheme::kColorPost);
        g.fillRoundedRectangle (kx, ky + 18.0f, 14.0f, 3.0f, 1.5f);
        g.setColour (PnsTheme::kTextPrimary);
        g.drawText (postLabel, juce::roundToInt (kx + 20.0f), juce::roundToInt (ky + 14.0f), 200, 12,
                    juce::Justification::centredLeft);
    }

}

//==============================================================================
// StereoView
//==============================================================================
// Shared cursor chip helper, defined further down with the other measurement views.
static void drawCursorChip (juce::Graphics& g, juce::Rectangle<float> plot, float cx,
                            float cy, juce::Colour accent,
                            const juce::StringArray& rows, bool locked);

StereoView::StereoView (PlugNspectrPostProcessor& p) : m_proc (p)
{
    m_options.setResetAction ("Clears the averages and the stereo-field trail.", [this] { reset(); });
    addAndMakeVisible (m_options);
    setInterceptsMouseClicks (true, false);
}

void StereoView::resized()
{
    m_options.place (getWidth() - PnsTheme::kPaddingMid, PnsTheme::kPaddingSmall);
}

void StereoView::reset()
{
    m_proc.resetStereo();
    m_gxPost.fill (0.0f); m_gyPost.fill (0.0f);
    m_gxPre .fill (0.0f); m_gyPre .fill (0.0f);
    m_gonioPos = m_gonioFill = 0;
    repaint();
}

// Average a per-bin curve over a fractional-octave window, skipping bins with no
// energy. Prefix sums keep it O(bins) rather than O(bins x window) — at 10 kHz
// the window is over a hundred bins wide, so the naive form would be ~60k
// operations per curve per frame, x4 curves at 30 fps.
void StereoView::smoothOctave (const std::array<float, kBins>& in,
                               std::array<float, kBins>& out)
{
    m_cumV[0] = 0.0;
    m_cumN[0] = 0.0;
    for (int k = 0; k < kBins; ++k)
    {
        const bool v = m_st.valid[(size_t) k];
        m_cumV[(size_t) k + 1] = m_cumV[(size_t) k] + (v ? (double) in[(size_t) k] : 0.0);
        m_cumN[(size_t) k + 1] = m_cumN[(size_t) k] + (v ? 1.0 : 0.0);
    }

    // Window is a constant *ratio* of the centre frequency, so it is narrow in
    // the lows (few bins per octave) and wide in the highs — which is exactly
    // where the noise is.
    const double halfRatio = std::pow (2.0, kSmoothOctaves * 0.5);
    for (int k = 0; k < kBins; ++k)
    {
        const int lo = juce::jlimit (0, kBins, (int) std::floor (k / halfRatio));
        const int hi = juce::jlimit (0, kBins, (int) std::ceil  (k * halfRatio) + 1);
        const double n = m_cumN[(size_t) hi] - m_cumN[(size_t) lo];
        out[(size_t) k] = (n > 0.0)
                        ? (float) ((m_cumV[(size_t) hi] - m_cumV[(size_t) lo]) / n)
                        : in[(size_t) k];
    }
}

void StereoView::update()
{
    m_proc.getStereo (m_st);

    smoothOctave (m_st.widthPre,  m_dWidthPre);
    smoothOctave (m_st.widthPost, m_dWidthPost);
    smoothOctave (m_st.corrPre,   m_dCorrPre);
    smoothOctave (m_st.corrPost,  m_dCorrPost);

    // Goniometer trail — pull raw L/R (capture channels 1/2) and rotate 45° so a
    // mono signal draws a vertical line.
    {
        const int n = m_proc.readCaptureSince (m_capReadPos, m_capPre, m_capPost);
        if (n > 0)
        {
            const float* pl = m_capPost.getReadPointer (PlugNspectrPostProcessor::kCapL);
            const float* pr = m_capPost.getReadPointer (PlugNspectrPostProcessor::kCapR);
            const float* ql = m_capPre .getReadPointer (PlugNspectrPostProcessor::kCapL);
            const float* qr = m_capPre .getReadPointer (PlugNspectrPostProcessor::kCapR);

            constexpr float kR = 0.70710678f;   // 1/√2
            float peak = 0.0f;
            // Decimate: a few hundred points per block is plenty for the cloud.
            const int step = juce::jmax (1, n / 512);
            for (int i = 0; i < n; i += step)
            {
                const float l = pl[i], r = pr[i];
                m_gxPost[(size_t) m_gonioPos] = (l - r) * kR;
                m_gyPost[(size_t) m_gonioPos] = (l + r) * kR;
                if (ql != nullptr)
                {
                    m_gxPre[(size_t) m_gonioPos] = (ql[i] - qr[i]) * kR;
                    m_gyPre[(size_t) m_gonioPos] = (ql[i] + qr[i]) * kR;
                }
                peak = juce::jmax (peak, std::abs (l), std::abs (r));
                m_gonioPos = (m_gonioPos + 1) % kGonioPoints;
                m_gonioFill = juce::jmin (m_gonioFill + 1, kGonioPoints);
            }

            // Smoothed auto-gain so quiet material still fills the square.
            if (peak > 1.0e-4f)
            {
                // Meter ballistics: hold a peak envelope that captures a
                // transient instantly and decays slowly (~3 s), then track it.
                // Scaling off the raw per-chunk peak made the scale oscillate —
                // the trace overflowed the square on an onset and then collapsed
                // to a dot moments later, because the instantaneous peak swings
                // wildly between chunks. A decaying envelope is smooth, so the
                // scale can follow it quickly without jitter and recovers on its
                // own instead of staying stuck small.
                m_gonioPeak = juce::jmax (peak, m_gonioPeak * 0.995f);
                const float target = juce::jlimit (0.5f, 12.0f,
                                                   0.9f / juce::jmax (m_gonioPeak, 1.0e-4f));
                m_gonioScale += (target - m_gonioScale) * 0.20f;
            }
        }
    }
}

float StereoView::freqToX (double hz, juce::Rectangle<float> plot) const
{
    const double t = std::log (juce::jlimit ((double) kLoHz, (double) kHiHz, hz) / kLoHz)
                   / std::log ((double) kHiHz / kLoHz);
    return plot.getX() + plot.getWidth() * (float) t;
}

// One stacked lane: log-frequency X, linear value Y, Pre dim under Post bright.
void StereoView::drawLane (juce::Graphics& g, juce::Rectangle<float> area, const LaneSpec& spec,
                           const std::array<float, kBins>& pre,
                           const std::array<float, kBins>& post) const
{
    const float vMin = spec.vMin, vMax = spec.vMax;
    const juce::String unit (spec.unit);
    const int  steps     = spec.steps;
    const bool fillDelta = spec.fillDelta;

    constexpr float kML = 42.0f, kMR = 10.0f, kMT = 16.0f, kMB = 16.0f;
    const juce::Rectangle<float> plot { area.getX() + kML, area.getY() + kMT,
                                        area.getWidth() - kML - kMR,
                                        area.getHeight() - kMT - kMB };
    if (plot.getWidth() <= 0.0f || plot.getHeight() <= 0.0f) return;

    {   // "PLAIN|technical" — plain name first, engineering term after it
        const juce::String t (spec.title);
        PnsTheme::drawPanelTitle (g, area.withHeight (13.0f), t.upToFirstOccurrenceOf ("|", false, false),
                                  t.fromFirstOccurrenceOf ("|", false, false));
    }

    // Pre/Post key. The readout panel colour-codes the two, but the graphs
    // themselves never said which trace was which.
    if (spec.legend)
    {
        const int ly = (int) area.getY();
        int lx = (int) plot.getRight() - 92;
        auto key = [&] (juce::Colour c, const char* label, int w)
        {
            g.setColour (c);
            g.fillRect (lx, ly + 6, 11, 2);
            g.setColour (PnsTheme::kTextSecondary);
            g.drawText (label, lx + 15, ly, w, 13, juce::Justification::centredLeft);
            lx += 15 + w + 6;
        };
        key (PnsTheme::kColorPre.withAlpha (0.85f), "Pre",  22);
        key (PnsTheme::kColorPost,                  "Post", 28);
    }

    PnsTheme::drawPlotCard (g, plot);

    // 2 px inset so a curve sitting exactly at vMin/vMax (a mono signal reads
    // correlation 1.0 and width at the floor) is drawn just inside the frame
    // instead of vanishing underneath it.
    constexpr float kInset = 2.0f;
    auto valToY = [&] (float v)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (v - vMin) / (vMax - vMin));
        return (plot.getBottom() - kInset) - t * (plot.getHeight() - 2.0f * kInset);
    };

    // The half of the scale that means "look at this" — side-dominant for width,
    // anti-phase for correlation. Drawn as a tinted region rather than left as a
    // gridline crossing, so the risk is something you see instead of compute.
    const bool hasZero = (vMin < 0.0f && 0.0f < vMax);
    if (hasZero)
    {
        const float y0 = valToY (0.0f);
        const auto zone = spec.cautionAbove
            ? juce::Rectangle<float> (plot.getX(), plot.getY(), plot.getWidth(), y0 - plot.getY())
            : juce::Rectangle<float> (plot.getX(), y0, plot.getWidth(), plot.getBottom() - y0);
        g.setColour (PnsTheme::kTextSecondary.withAlpha (0.05f));
        g.fillRect (zone);
    }

    // Horizontal gridlines + labels; `steps` chosen per lane so values are round.
    for (int i = 0; i <= steps; ++i)
    {
        const float v  = vMin + (vMax - vMin) * (float) i / (float) steps;
        const float gy = valToY (v);
        g.setColour (PnsTheme::kGridLine);
        g.drawHorizontalLine (juce::roundToInt (gy), plot.getX(), plot.getRight());
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (juce::String (v, (std::abs (vMax - vMin) <= 2.5f) ? 1 : 0) + unit,
                    (int) area.getX(), juce::roundToInt (gy) - 6, (int) kML - 5, 12,
                    juce::Justification::centredRight);
    }
    // Zero is the threshold that changes the meaning on both lanes, so it must not
    // look like every other gridline.
    if (hasZero)
    {
        g.setColour (PnsTheme::kBorderActive);
        g.drawHorizontalLine (juce::roundToInt (valToY (0.0f)), plot.getX(), plot.getRight());
    }

    // Plain-language anchors, so the scale does not need the manual to decode.
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary.withAlpha (0.55f));
    g.drawText (spec.hiTag, (int) plot.getX() + 6, (int) plot.getY() + 2,
                140, 12, juce::Justification::centredLeft);
    g.drawText (spec.loTag, (int) plot.getX() + 6, (int) plot.getBottom() - 14,
                140, 12, juce::Justification::centredLeft);
    if (spec.cautionTag != nullptr && hasZero)
    {
        const float y0 = valToY (0.0f);
        g.setColour (PnsTheme::kTextSecondary.withAlpha (0.75f));
        constexpr int kTagW = 170;   // wide enough for the longest tag, unclipped
        g.drawText (spec.cautionTag, (int) plot.getRight() - (kTagW + 6),
                    spec.cautionAbove ? (int) plot.getY() + 2 : (int) y0 + 3,
                    kTagW, 12, juce::Justification::centredRight);
    }

    // Frequency gridlines.
    for (double f : { 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0 })
    {
        const float gx = freqToX (f, plot);
        g.setColour (PnsTheme::kGridLine);
        g.drawVerticalLine (juce::roundToInt (gx), plot.getY(), plot.getBottom());
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String ((int) f),
                    juce::roundToInt (gx) - 14, (int) plot.getBottom() + 2, 28, 11,
                    juce::Justification::centred);
    }

    const double sr  = m_proc.getSampleRate() > 0.0 ? m_proc.getSampleRate() : 48000.0;
    const double binW = sr / (double) PlugNspectrPostProcessor::kFftSize;

    // Collect the visible points once — both the curves and the delta polygon
    // are built from the same screen coordinates.
    std::vector<juce::Point<float>> ptsPre, ptsPost;
    ptsPre.reserve ((size_t) kBins);
    ptsPost.reserve ((size_t) kBins);
    for (int k = 1; k < kBins; ++k)
    {
        if (! m_st.valid[(size_t) k]) continue;
        const double f = k * binW;
        if (f < kLoHz || f > kHiHz) continue;
        const float x = freqToX (f, plot);
        ptsPre .push_back ({ x, valToY (pre [(size_t) k]) });
        ptsPost.push_back ({ x, valToY (post[(size_t) k]) });
    }

    auto toPath = [] (const std::vector<juce::Point<float>>& pts)
    {
        juce::Path p;
        if (pts.size() > 1)
        {
            p.startNewSubPath (pts.front());
            for (size_t i = 1; i < pts.size(); ++i) p.lineTo (pts[i]);
        }
        return p;
    };

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (plot.toNearestInt());

    const juce::Path pPre  = toPath (ptsPre);
    const juce::Path pPost = toPath (ptsPost);
    const bool haveBoth = ptsPost.size() > 1;

    // Amber fill between the curves — the change the plugin made, same visual
    // language as the Spectrum tab's Pre/Post average difference.
    if (fillDelta && haveBoth)
    {
        juce::Path band;
        band.startNewSubPath (ptsPost.front());
        for (size_t i = 1; i < ptsPost.size(); ++i) band.lineTo (ptsPost[i]);
        for (size_t i = ptsPre.size(); i-- > 0; )   band.lineTo (ptsPre[i]);
        band.closeSubPath();
        g.setColour (PnsTheme::kColorPost.withAlpha (0.10f));   // the change, in the Post colour
        g.fillPath (band);
    }

    if (ptsPre.size() > 1)
    {
        g.setColour (PnsTheme::kColorPre.withAlpha (PnsTheme::kColorPreAlpha));
        g.strokePath (pPre, juce::PathStrokeType (1.0f));
    }
    if (haveBoth)
        PnsTheme::drawGlowLine (g, pPost, PnsTheme::kColorPost, 1.5f);

    // Cursor readout — frequency → value for both signals.
    if (haveBoth && m_cursorX >= 0.0f)
    {
        const float cx = juce::jlimit (plot.getX(), plot.getRight(), m_cursorX);
        const double t = (double) (cx - plot.getX()) / plot.getWidth();
        const double f = (double) kLoHz * std::pow ((double) kHiHz / kLoHz, t);
        const int k = juce::jlimit (1, kBins - 1, (int) std::lround (f / binW));
        if (m_st.valid[(size_t) k])
        {
            juce::StringArray rows;
            rows.add (f >= 1000.0 ? juce::String (f / 1000.0, 2) + " kHz"
                                  : juce::String ((int) f) + " Hz");
            rows.add ("Post " + juce::String (post[(size_t) k], 2) + unit);
            rows.add ("Pre  " + juce::String (pre [(size_t) k], 2) + unit);
            drawCursorChip (g, plot, cx, valToY (post[(size_t) k]),
                            PnsTheme::kColorPost, rows, m_cursorLocked);
        }
    }

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (plot, 1.0f);
}

void StereoView::drawGonio (juce::Graphics& g, juce::Rectangle<float> area) const
{
    PnsTheme::drawPanelTitle (g, area.withHeight (13.0f), "STEREO FIELD", "goniometer");

    // Square plot, centred.
    const float d = juce::jmin (area.getWidth(), area.getHeight() - 16.0f);
    const juce::Rectangle<float> sq =
        juce::Rectangle<float> (d, d).withCentre ({ area.getCentreX(),
                                                    area.getY() + 16.0f + (area.getHeight() - 16.0f) * 0.5f });
    PnsTheme::drawPlotCard (g, sq);

    const float cx = sq.getCentreX(), cy = sq.getCentreY(), half = d * 0.5f;

    // Guides: vertical = mono, diagonals = hard L / hard R.
    g.setColour (PnsTheme::kGridLine);
    g.drawVerticalLine (juce::roundToInt (cx), sq.getY(), sq.getBottom());
    g.drawHorizontalLine (juce::roundToInt (cy), sq.getX(), sq.getRight());
    g.drawLine (sq.getX(), sq.getY(), sq.getRight(), sq.getBottom(), 0.5f);
    g.drawLine (sq.getX(), sq.getBottom(), sq.getRight(), sq.getY(), 0.5f);
    g.setColour (PnsTheme::kGridLabel);
    g.drawText ("M", (int) cx - 8, (int) sq.getY() + 2, 16, 11, juce::Justification::centred);
    g.drawText ("L", (int) sq.getX() + 2, (int) cy - 6, 12, 11, juce::Justification::centredLeft);
    g.drawText ("R", (int) sq.getRight() - 14, (int) cy - 6, 12, 11, juce::Justification::centredRight);

    if (m_gonioFill < 2) { g.setColour (PnsTheme::kBorderSubtle); g.drawRect (sq, 1.0f); return; }

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (sq.toNearestInt());

    // Continuous Lissajous trace rather than a scatter of dots: the shape of the
    // trajectory is what tells Pre and Post apart, and two line traces read far
    // more clearly overlaid than two interleaved point clouds.
    //
    // This MUST walk the ring in time order, not array order. Once the buffer has
    // filled, the oldest sample sits at m_gonioPos — iterating 0..fill would join
    // the newest point to the oldest and draw a bogus segment straight across the
    // plot at the wrap. (Harmless for dots, which is why the old code got away
    // with it.) One path stroked once is also far cheaper than per-sample fills.
    auto plotTrail = [&] (const std::array<float, kGonioPoints>& xs,
                          const std::array<float, kGonioPoints>& ys,
                          juce::Colour c, float thickness)
    {
        const int count = m_gonioFill;
        if (count < 2) return;
        const int start = (count == kGonioPoints) ? m_gonioPos : 0;   // oldest sample

        juce::Path p;
        for (int k = 0; k < count; ++k)
        {
            const int i = (start + k) % kGonioPoints;
            // Clamp generously, NOT to the square's edge. At +/-1 an overshooting
            // sample lands exactly on the border, so consecutive ones draw a
            // segment along it and the trace appears to smear off the graph.
            // Overshoot instead and let the clip region cut it, so a loud
            // transient leaves the square cleanly.
            const float px = cx + juce::jlimit (-3.0f, 3.0f, xs[(size_t) i] * m_gonioScale) * half;
            const float py = cy - juce::jlimit (-3.0f, 3.0f, ys[(size_t) i] * m_gonioScale) * half;
            if (k == 0) p.startNewSubPath (px, py);
            else        p.lineTo (px, py);
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    };

    // Post underneath, Pre ON TOP. Everywhere else in the plugin Pre is the
    // receding background, but here that fails: Pre is usually the narrower
    // signal, so its trace sits geometrically *inside* Post's and gets buried —
    // a mono Pre is a thin vertical line hidden within a widened Post blob. No
    // amount of alpha fixes occlusion. Drawing the reference over the result
    // keeps it readable while Post still reads as the dominant shape, since it
    // is the wider and heavier stroke.
    plotTrail (m_gxPost, m_gyPost, PnsTheme::kColorPost.withAlpha (0.80f), 1.3f);
    plotTrail (m_gxPre,  m_gyPre,  PnsTheme::kColorPre .withAlpha (0.85f), 1.0f);

    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRect (sq, 1.0f);
}

void StereoView::drawReadouts (juce::Graphics& g, juce::Rectangle<float> area) const
{
    g.setColour (PnsTheme::kBgWidget.withAlpha (0.85f));
    g.fillRoundedRectangle (area, (float) PnsTheme::kCornerRadius);
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawRoundedRectangle (area, (float) PnsTheme::kCornerRadius, 1.0f);

    const int lx = (int) area.getX() + 10;
    const int lw = (int) area.getWidth() - 20;
    int y = (int) area.getY() + 8;

    auto row = [&] (const juce::String& label, const juce::String& pre,
                    const juce::String& post, juce::Colour postCol)
    {
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText (label, lx, y, lw / 3, 14, juce::Justification::centredLeft);
        g.drawText (pre,  lx + lw / 3,     y, lw / 3, 14, juce::Justification::centredRight);
        g.setColour (postCol);
        g.drawText (post, lx + lw * 2 / 3, y, lw / 3, 14, juce::Justification::centredRight);
        y += 17;
    };

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary);
    g.drawText ("PRE",  lx + lw / 3,     y, lw / 3, 12, juce::Justification::centredRight);
    g.setColour (PnsTheme::kAccentPrimary);
    g.drawText ("POST", lx + lw * 2 / 3, y, lw / 3, 12, juce::Justification::centredRight);
    y += 16;

    auto dbStr = [] (float v) { return (v <= PlugNspectrPostProcessor::kStereoFloorDb + 0.01f)
                                       ? juce::String ("mono") : juce::String (v, 1) + " dB"; };

    row ("Width", dbStr (m_st.bbWidthPre), dbStr (m_st.bbWidthPost), PnsTheme::kAccentPrimary);
    row ("Phase", juce::String (m_st.bbCorrPre, 2), juce::String (m_st.bbCorrPost, 2),
         m_st.bbCorrPost < 0.0f ? PnsTheme::kWarn : PnsTheme::kAccentPrimary);
    // Mono-sum loss: how much level disappears when summed. Amber once it bites.
    row ("In mono", juce::String (m_st.monoLossPre, 1) + " dB",
                  juce::String (m_st.monoLossPost, 1) + " dB",
         m_st.monoLossPost < -3.0f ? PnsTheme::kWarn : PnsTheme::kAccentPrimary);
}

void StereoView::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (8.0f, 6.0f);

    // Right column: goniometer above the readouts. Left: the two stacked lanes.
    constexpr float kRightW = 250.0f, kGap = 8.0f;
    const float rightW = juce::jmin (kRightW, b.getWidth() * 0.34f);
    auto right = b.removeFromRight (rightW);
    b.removeFromRight (kGap);
    auto left = b;
    // Leave room for the reset button on the first lane's title row.
    left.removeFromTop (2.0f);

    // -40..+10 dB in 5 steps and -1..+1 in 4 give round axis labels.
    const float laneH = (left.getHeight() - kGap) * 0.5f;
    static constexpr LaneSpec kWidthLane {
        "STEREO WIDTH|side vs mid",
        PlugNspectrPostProcessor::kStereoFloorDb, 10.0f, " dB", 5, true,
        // "SIDE" not "WIDE": the axis is the Side/Mid ratio, so the top of the
        // scale is side-dominant. "WIDE" also read as desirable while sitting
        // inside the amber caution zone, which argued against the shading.
        // No caution tag — the top label covers it, and the width curve rises
        // through the corner where one would sit.
        "MONO", "SIDE", true, nullptr, true
    };
    static constexpr LaneSpec kCorrLane {
        "MONO SAFETY|correlation",
        -1.0f, 1.0f, "", 4, false,
        "ANTI-PHASE", "IN PHASE", false, "CANCELS IN MONO", false
    };

    drawLane (g, left.removeFromTop (laneH), kWidthLane, m_dWidthPre, m_dWidthPost);
    left.removeFromTop (kGap);
    drawLane (g, left, kCorrLane, m_dCorrPre, m_dCorrPost);

    // Goniometer gets a square slot so it doesn't float in a tall column.
    auto gonio = right.removeFromTop (juce::jmin (right.getWidth() + 16.0f,
                                                  right.getHeight() - 86.0f));
    drawGonio (g, gonio);
    drawReadouts (g, right.removeFromTop (78.0f));

    // Mono source: the curves are meaningless, so say so plainly.
    if (! m_st.stereoSource)
    {
        g.setColour (PnsTheme::kBgDark.withAlpha (0.72f));
        g.fillRect (getLocalBounds());
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText ("Stereo analysis needs a stereo track",
                    getLocalBounds().reduced (20), juce::Justification::centred);
    }
}

void StereoView::mouseMove (const juce::MouseEvent& e)
{
    if (! m_cursorLocked) { m_cursorX = (float) e.x; repaint(); }
}

void StereoView::mouseExit (const juce::MouseEvent&)
{
    if (! m_cursorLocked) { m_cursorX = -1.0f; repaint(); }
}

void StereoView::mouseDown (const juce::MouseEvent& e)
{
    m_cursorLocked = ! m_cursorLocked;
    m_cursorX = (float) e.x;
    repaint();
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
            drawCursorChip (g, { px, py, pw, ph }, cx, Y (pct), PnsTheme::kColorPost, rows, m_cursorLocked);
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

//==============================================================================
PlugNspectrPostEditor::PlugNspectrPostEditor (PlugNspectrPostProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      m_lagTracker (p),
      m_specView (p),
      m_dynView  (p),
      m_oscView  (p),
      m_stereoView (p),
      m_harmView (p),
      m_linearView (p),
      m_transferView (p),
      m_envelopeView (p),
      m_thdView (p)
{
    setLookAndFeel (&m_laf);

    m_pnsLogo = juce::ImageCache::getFromMemory (kPnsLogoPng, kPnsLogoPngSize);

    {
        struct Def { const char* label; DockTab::Icon icon; };
        const Def defs[TabCount] = {
            { "Tone",       DockTab::Tone        }, { "Dynamics",   DockTab::Dynamics      },
            { "Waveform",   DockTab::Waveform    }, { "Stereo",     DockTab::Stereo        },
            { "Harmonics",  DockTab::Harmonics   }, { "EQ curve",   DockTab::EqCurve       },
            { "Compression",DockTab::Compression }, { "Attack/Rel", DockTab::AttackRelease },
            { "Distortion", DockTab::Distortion  } };
        for (int i = 0; i < TabCount; ++i)
        {
            auto* t = m_tabs.add (new DockTab (defs[i].label, defs[i].icon));
            t->setTooltip (tabQuestion (i));
            t->onClick = [this, i] { switchTab (i); };
            addAndMakeVisible (t);
        }
    }

    addAndMakeVisible (m_specView);
    addAndMakeVisible (m_dynView);
    addAndMakeVisible (m_oscView);
    addAndMakeVisible (m_stereoView);
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

    // ── Dock — test-tone frequency knob ────────────────────────────────────
    m_toneFreqSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    m_toneFreqSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    m_toneFreqSlider.setRange (100.0, 8000.0, 1.0);
    m_toneFreqSlider.setSkewFactorFromMidPoint (1000.0);
    m_toneFreqSlider.setValue (1000.0, juce::dontSendNotification);
    m_toneFreqSlider.onValueChange = [this]
    {
        m_toneFreq = m_toneFreqSlider.getValue();
        writeCmdBlock();
        m_harmView.setToneFreq (m_toneFreq);
        repaint();   // update the tone readout
    };
    addAndMakeVisible (m_toneFreqSlider);

    // ── Dock — test-tone level ───────────────────────────────────
    m_toneLevelSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    m_toneLevelSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    m_toneLevelSlider.setRange (-48.0, 0.0, 0.5);
    m_toneLevelSlider.setValue (m_toneLevel, juce::dontSendNotification);
    m_toneLevelSlider.onValueChange = [this]
    {
        m_toneLevel = m_toneLevelSlider.getValue();
        writeCmdBlock();
        repaint();
    };
    addAndMakeVisible (m_toneLevelSlider);

    // ── Dock — test-tone button ──────────────────────────────────
    m_toneBtn.setToggleable (true);
    m_toneBtn.getProperties().set ("armed", true);
    m_toneBtn.setToggleState (false, juce::dontSendNotification);
    m_toneBtn.onClick = [this]
    {
        m_toneActive = !m_toneActive;
        m_toneBtn.setToggleState (m_toneActive, juce::dontSendNotification);
        writeCmdBlock();
        m_harmView.setToneActive (m_toneActive);
        repaint();   // show/hide warning label
    };
    addAndMakeVisible (m_toneBtn);

    // ── Header — L+R / Side segmented channel selector (all analysis) ──────
    {
        juce::TextButton* segs[2] = { &m_chLR, &m_chSide };
        const char* tip[2] = { "L+R \xe2\x80\x94 both channels combined, (L+R)/2",
                               "Side \xe2\x80\x94 stereo difference, (L-R)/2" };
        for (int i = 0; i < 2; ++i)
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
        segs[juce::jlimit (0, 1, audioProcessor.getChannelMode())]->setToggleState (true, juce::dontSendNotification);
    }

    // ── About modal + the info icon that opens it ──────────────────────────
    m_infoBtn.onClick = [this] { m_about.setVisible (true); m_about.toFront (true); };
    addAndMakeVisible (m_infoBtn);

    m_about.setLogo (m_pnsLogo);
    m_about.setSystemInfo ("1.0.3",
                           juce::AudioProcessor::getWrapperTypeDescription (audioProcessor.wrapperType),
                           juce::SystemStats::getOperatingSystemName());
    addChildComponent (m_about);   // hidden until the info icon is clicked

    switchTab (0);

    setSize (1280, 800);
    setResizable (true, true);
    setResizeLimits (960, 600, 2560, 1600);   // 960 still fits the nine dock tabs + tone controls
    startTimerHz (60);
}

PlugNspectrPostEditor::~PlugNspectrPostEditor()
{
    // Stop any stimulus so Pre goes quiet immediately on editor close.
    audioProcessor.postCommand (pns::PNS_CmdBlock{});
    // Drop the stereo gate too — otherwise closing the window while the Stereo
    // tab is showing leaves four FFTs per frame running with nothing reading them.
    audioProcessor.setStereoActive (false);
    setLookAndFeel (nullptr);
    stopTimer();
}

//──────────────────────────────────────────────────────────────────────────────
void PlugNspectrPostEditor::writeCmdBlock()
{
    pns::PNS_CmdBlock c;
    c.testToneFrequency = m_toneFreq;
    c.testToneLevelDb   = m_toneLevel;
    c.testToneActive    = m_toneActive ? 1u : 0u;
    // Noise stimulus only while the Linear tab is showing AND Measure is on.
    c.measureActive     = (m_activeTab == TabLinear && m_linearView.isMeasureActive()) ? 1u : 0u;
    // Dynamics stimulus: level ramp on the Transfer tab, level step on the
    // Envelope tab, THD sweep on the Distortion tab — only while showing + Measure on.
    c.dynMeasureMode    = (m_activeTab == TabTransfer && m_transferView.isMeasureActive()) ? 1u
                        : (m_activeTab == TabEnvelope && m_envelopeView.isMeasureActive()) ? 2u
                        : (m_activeTab == TabThd      && m_thdView.isMeasureActive())      ? 3u
                                                                                          : 0u;
    audioProcessor.postCommand (c);

    // Keep the Linear accumulation gate in lock-step with its noise stimulus so
    // pausing/leaving the tab/stopping transport all hold the curve.
    audioProcessor.setLinearMeasuring (c.measureActive != 0);
}

bool PlugNspectrPostEditor::isStimulusActive() const
{
    if (m_toneActive) return true;
    if (m_activeTab == TabLinear   && m_linearView  .isMeasureActive()) return true;
    if (m_activeTab == TabTransfer && m_transferView.isMeasureActive()) return true;
    if (m_activeTab == TabEnvelope && m_envelopeView.isMeasureActive()) return true;
    if (m_activeTab == TabThd      && m_thdView     .isMeasureActive()) return true;
    return false;
}

bool PlugNspectrPostEditor::disarmAllStimuli()
{
    bool changed = false;
    if (m_toneActive)
    {
        m_toneActive = false;
        m_toneBtn.setToggleState (false, juce::dontSendNotification);
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
    m_lagTracker.update();
    m_dynView.setLagSamples (m_lagTracker.lagSamples());
    m_dynView.update();
    if (m_activeTab == TabDynamics) m_dynView.repaint();

    if (++m_tickCounter % 2 == 0)
    {
        m_specView.update();
        if (m_activeTab == TabSpectrum) m_specView.repaint();

        m_oscView.setLagSamples (m_lagTracker.lagSamples());
        m_oscView.setToneActive (m_toneActive);
        m_oscView.update();
        if (m_activeTab == TabOscilloscope) m_oscView.repaint();

        m_stereoView.update();
        if (m_activeTab == TabStereo) m_stereoView.repaint();

        m_harmView.update();
        if (m_activeTab == TabHarmonics) m_harmView.repaint();

        m_linearView.update();
        if (m_activeTab == TabLinear) m_linearView.repaint();

        m_transferView.update();
        if (m_activeTab == TabTransfer) m_transferView.repaint();

        m_envelopeView.update();
        if (m_activeTab == TabEnvelope) m_envelopeView.repaint();

        m_thdView.update();
        if (m_activeTab == TabThd) m_thdView.repaint();

        const auto sm = currentSummary();
        if (sm != m_summary)
        {
            m_summary = sm;
            repaint (0, 0, getWidth(), PnsTheme::kHeroHeight);
        }
    }
}

//==============================================================================
void PlugNspectrPostEditor::paintOverChildren (juce::Graphics& g)
{
    // The About modal sits above everything — don't draw the measuring border or
    // the no-Pre overlay over it.
    if (m_about.isVisible()) return;

    // Amber "measuring" border around the whole window whenever a test signal
    // is being emitted — an unmissable cue that the audio is being replaced.
    if (isStimulusActive())
    {
        g.setColour (PnsTheme::kWarn);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 6.0f, 3.0f);
    }

    if (m_overlayAlpha < 0.005f) return;

    const float alpha = m_overlayAlpha;
    const int W = getWidth(), H = getHeight();
    const int cY = PnsTheme::kHeroHeight;
    const int cH = H - cY - PnsTheme::kDockHeight;

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

    drawBlock (chainX,                              bW1, PnsTheme::kColorPre,     "PlugNspectr Pre");
    drawArrow (chainX + bW1);
    drawBlock (chainX + bW1 + arrW,                 bW2, PnsTheme::kBorderActive, "Plugin to Analyze");
    drawArrow (chainX + bW1 + arrW + bW2);
    drawBlock (chainX + bW1 + arrW + bW2 + arrW,    bW3, PnsTheme::kColorPost,    "PlugNspectr Post");
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
// Each tab answers one question: shown above the headline and as its tooltip.
const char* PlugNspectrPostEditor::tabQuestion (int tab)
{
    switch (tab)
    {
        case TabSpectrum:     return "What does it do to the tone?";
        case TabDynamics:     return "How much does it turn the level down?";
        case TabOscilloscope: return "Does it change the shape of the wave?";
        case TabStereo:       return "Does it make the sound wider or narrower?";
        case TabHarmonics:    return "What does it add to a pure tone?";
        case TabLinear:       return "Which frequencies does it change?";
        case TabTransfer:     return "How hard does it squash loud sounds?";
        case TabEnvelope:     return "How fast does it grab and let go?";
        case TabThd:          return "Is it clean at every pitch?";
        default:              return "";
    }
}

PnsTheme::Summary PlugNspectrPostEditor::currentSummary() const
{
    switch (m_activeTab)
    {
        case TabSpectrum:     return m_specView    .summary();
        case TabDynamics:     return m_dynView     .summary();
        case TabOscilloscope: return m_oscView     .summary();
        case TabStereo:       return m_stereoView  .summary();
        case TabHarmonics:    return m_harmView    .summary();
        case TabLinear:       return m_linearView  .summary();
        case TabTransfer:     return m_transferView.summary();
        case TabEnvelope:     return m_envelopeView.summary();
        case TabThd:          return m_thdView     .summary();
        default:              return {};
    }
}

// Tone-control relevance. FREQ + Test Tone belong to the single-tone tabs (live tabs
// + Harmonics); LEVEL also drives the Distortion sweep. Stereo is excluded even
// though it's live: every Pre generator is mono, so a tone reads as zero side.
bool PlugNspectrPostEditor::toneFreqOn (int tab)
{
    return (tab <= TabOscilloscope || tab == TabHarmonics);
}
bool PlugNspectrPostEditor::toneLevelOn (int tab)
{
    return toneFreqOn (tab) || tab == TabThd;
}

//==============================================================================
void PlugNspectrPostEditor::switchTab (int index)
{
    m_activeTab = index;

    for (int i = 0; i < TabCount; ++i)
        m_tabs[i]->setActive (i == index);

    m_specView    .setVisible (index == TabSpectrum);
    m_dynView     .setVisible (index == TabDynamics);
    m_oscView     .setVisible (index == TabOscilloscope);
    m_stereoView  .setVisible (index == TabStereo);
    m_harmView    .setVisible (index == TabHarmonics);
    m_linearView  .setVisible (index == TabLinear);
    m_transferView.setVisible (index == TabTransfer);
    m_envelopeView.setVisible (index == TabEnvelope);
    m_thdView     .setVisible (index == TabThd);

    // ── Tone-control relevance per tab (see toneFreqOn) ────────────────────────
    const bool freqOn  = toneFreqOn (index);
    const bool levelOn = toneLevelOn (index);
    const bool toneOn  = freqOn;

    // Four extra FFTs — only run them while the Stereo tab is actually showing.
    audioProcessor.setStereoActive (index == TabStereo);
    // The L+R / Side selector is meaningless on Stereo: it uses both inherently.
    const bool chOn = (index != TabStereo);
    m_chLR  .setEnabled (chOn);
    m_chSide.setEnabled (chOn);
    m_chLR  .setAlpha (chOn ? 1.0f : 0.4f);
    m_chSide.setAlpha (chOn ? 1.0f : 0.4f);

    // Entering a tab that doesn't own the tone kills a stray tone so it can't
    // override the measurement stimulus (Pre prioritises the tone otherwise).
    if (! toneOn && m_toneActive)
    {
        m_toneActive = false;
        m_toneBtn.setToggleState (false, juce::dontSendNotification);
        m_harmView.setToneActive (false);
    }

    // Hide rather than grey out: a dimmed control still asks "should I touch this?"
    m_toneFreqSlider .setVisible (freqOn);
    m_toneLevelSlider.setVisible (levelOn);
    m_toneBtn    .setVisible (toneOn);
    m_summary = currentSummary();

    writeCmdBlock();   // start/stop the measurement noise with the tab
    repaint();
}

//==============================================================================
void PlugNspectrPostEditor::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);
    const int W = getWidth(), H = getHeight();
    constexpr int kPadX = 28;

    auto textW = [] (const juce::Font& f, const juce::String& t)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, t, 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
    };

    // ── Headline block ─────────────────────────────────────────────────────
    //    Question (small, accent) → what it found (large) → the detail; the one
    //    number that answers it sits large at the right. While a test signal is
    //    playing the question line turns amber and says so: it's the one thing
    //    you must not miss.
    {
        // The big number, right-aligned under the header controls.
        int heroLeft = W - kPadX;
        if (m_summary.heroValue.isNotEmpty())
        {
            const auto fv = PnsTheme::fontSized (44.0f, true);
            const auto fu = PnsTheme::fontSized (18.0f);
            const int wu = m_summary.heroUnit.isEmpty() ? 0 : textW (fu, m_summary.heroUnit) + 6;
            const int wv = textW (fv, m_summary.heroValue);
            const int right = W - kPadX;
            heroLeft = right - wu - wv;

            g.setFont (fu);
            g.setColour (PnsTheme::kTextSecondary);
            g.drawText (m_summary.heroUnit, right - wu + 6, 64, wu, 30, juce::Justification::bottomLeft);
            g.setFont (fv);
            g.setColour (PnsTheme::kTextPrimary);
            g.drawText (m_summary.heroValue, heroLeft, 50, wv + 2, 46, juce::Justification::bottomLeft);

            g.setFont (PnsTheme::fontSized (11.0f, true));
            g.setColour (PnsTheme::kTextSecondary);
            g.drawText (m_summary.heroLabel.toUpperCase(), right - 220, 42, 220, 14,
                        juce::Justification::centredRight);
            heroLeft = juce::jmin (heroLeft, right - textW (PnsTheme::fontSized (11.0f, true),
                                                            m_summary.heroLabel.toUpperCase()));
        }
        const int textRight = juce::jmin (heroLeft, m_logoX) - 32;

        // Question line
        const bool measuring = isStimulusActive();
        const bool isTest    = m_activeTab >= kFirstMeasTab;
        g.setFont (PnsTheme::fontSized (12.0f, true));
        if (measuring)
        {
            g.setColour (PnsTheme::kWarn);
            g.drawText (juce::CharPointer_UTF8 ("\xe2\x97\x8f  TEST SIGNAL PLAYING  \xc2\xb7  your audio is replaced until you stop playback or leave this tab"),
                        kPadX, 18, textRight - kPadX, 16, juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour (PnsTheme::kAccentPrimary);
            const juce::String q = juce::String (isTest ? "TEST" : "LISTEN")
                                 + juce::String::fromUTF8 ("  \xc2\xb7  ") + tabQuestion (m_activeTab);
            g.drawText (q, kPadX, 18, textRight - kPadX, 16, juce::Justification::centredLeft, true);
        }

        // Headline + detail: the verdict's first clause is the headline, the
        // rest ("  -  output is 4 dB quieter") is the supporting line.
        const juce::String sep ("  -  ");
        const juce::String head   = m_summary.text.upToFirstOccurrenceOf (sep, false, false);
        const juce::String detail = m_summary.text.fromFirstOccurrenceOf (sep, false, false)
                                                  .replace (sep, juce::String::fromUTF8 ("  \xc2\xb7  "));
        g.setFont (PnsTheme::fontSized (26.0f, true));
        g.setColour (m_summary.warn ? PnsTheme::kWarn : PnsTheme::kTextPrimary);
        g.drawFittedText (head, kPadX, 40, textRight - kPadX, 34, juce::Justification::centredLeft, 1, 0.8f);
        g.setFont (PnsTheme::fontSized (14.0f));
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText (detail, kPadX, 76, textRight - kPadX, 20, juce::Justification::centredLeft, true);
    }

    // Header controls' caption, and the logo at the far left of them.
    if (m_pnsLogo.isValid())
    {
        const float logoH = 20.0f;
        const float logoW = logoH * ((float) m_pnsLogo.getWidth() / (float) m_pnsLogo.getHeight());
        g.setOpacity (0.85f);
        g.drawImage (m_pnsLogo, { (float) m_logoX, 12.0f, logoW, logoH });
        g.setOpacity (1.0f);
    }

    // ── Dock ───────────────────────────────────────────────────────────────
    const int dockY = H - PnsTheme::kDockHeight;
    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (0, dockY, W, PnsTheme::kDockHeight);
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawHorizontalLine (dockY, 0.0f, (float) W);

    // Group captions, set vertically just left of each group.
    auto caption = [&] (const char* text, int rightX)
    {
        g.saveState();
        const float cx = (float) rightX - 9.0f, cy = (float) dockY + PnsTheme::kDockHeight * 0.5f;
        g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, cx, cy));
        g.setFont (PnsTheme::fontSized (9.5f, true));
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (text, juce::Rectangle<float> (60.0f, 12.0f).withCentre ({ cx, cy }),
                    juce::Justification::centred);
        g.restoreState();
    };
    caption ("LISTEN", m_tabs[0]->getX());
    caption ("TEST",   m_tabs[kFirstMeasTab]->getX());
    g.setColour (PnsTheme::kBorderSubtle);
    g.fillRect (m_dockDividerX, dockY + 16, 1, PnsTheme::kDockHeight - 32);

    // Tone readouts beside their knobs (hidden with the knobs on tabs that
    // don't use them — see toneFreqOn / toneLevelOn).
    auto readout = [&] (juce::Rectangle<int> r, const char* label, const juce::String& value)
    {
        g.setFont (PnsTheme::fontSized (10.0f, true));
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText (label, r.removeFromTop (r.getHeight() / 2), juce::Justification::bottomLeft);
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (m_toneActive ? PnsTheme::kTextAccent : PnsTheme::kTextPrimary);
        g.drawText (value, r, juce::Justification::topLeft);
    };
    if (toneFreqOn (m_activeTab))
        readout (m_freqText, "TONE", PnsTheme::fmtHz (m_toneFreq));
    if (toneLevelOn (m_activeTab))
        readout (m_levelText, "LEVEL", juce::String (m_toneLevel, 1) + " dB");
}

//==============================================================================
void PlugNspectrPostEditor::resized()
{
    const int W = getWidth(), H = getHeight();
    constexpr int kPadX = 28;

    // ── Header controls, top right: logo · L+R/Side · info ─────────────────
    constexpr int infoSz = 18, segW = 46, segH = 24, rowY = 10;
    m_infoBtn.setBounds (W - kPadX + 4 - infoSz, rowY + (segH - infoSz) / 2, infoSz, infoSz);
    const int segRight = m_infoBtn.getX() - 12;
    m_chLR  .setBounds (segRight - segW * 2, rowY, segW, segH);
    m_chSide.setBounds (segRight - segW,     rowY, segW, segH);
    m_logoX = m_pnsLogo.isValid()
                ? m_chLR.getX() - 16 - juce::roundToInt (20.0f * (float) m_pnsLogo.getWidth()
                                                               / (float) m_pnsLogo.getHeight())
                : m_chLR.getX();

    m_about.setBounds (getLocalBounds());

    // ── View area between the headline block and the dock ──────────────────
    // Same side margins as the headline, so graphs line up under the text.
    const auto content = getLocalBounds().withTrimmedTop (PnsTheme::kHeroHeight)
                                         .withTrimmedBottom (PnsTheme::kDockHeight + 14)
                                         .withTrimmedLeft (kPadX - 10).withTrimmedRight (kPadX - 16);
    for (auto* v : std::initializer_list<juce::Component*> { &m_specView, &m_dynView, &m_oscView, &m_stereoView,
                                                             &m_harmView, &m_linearView, &m_transferView,
                                                             &m_envelopeView, &m_thdView })
        v->setBounds (content);

    // ── Dock: tone controls take a fixed block at the right, tabs share the rest
    const int dockY = H - PnsTheme::kDockHeight;
    constexpr int kKnob = 32, kText = 70, kToneBtnW = 104;
    const int toneW = kKnob + 6 + kText + 10 + kKnob + 6 + kText + 14 + kToneBtnW;
    int x = W - 18 - toneW;
    const int knobY = dockY + (PnsTheme::kDockHeight - kKnob) / 2;
    m_toneFreqSlider .setBounds (x, knobY, kKnob, kKnob);            x += kKnob + 6;
    m_freqText = { x, knobY - 2, kText, kKnob + 4 };                    x += kText + 10;
    m_toneLevelSlider.setBounds (x, knobY, kKnob, kKnob);            x += kKnob + 6;
    m_levelText = { x, knobY - 2, kText, kKnob + 4 };                   x += kText + 14;
    m_toneBtn    .setBounds (x, dockY + (PnsTheme::kDockHeight - 32) / 2, kToneBtnW, 32);

    constexpr int kCaptionW = 20, kDividerW = 17;
    const int tabsLeft = 14, tabsRight = W - 18 - toneW - 16;
    const int tabW = juce::jlimit (56, 88, (tabsRight - tabsLeft - 2 * kCaptionW - kDividerW) / TabCount);
    x = tabsLeft + kCaptionW;
    for (int i = 0; i < TabCount; ++i)
    {
        if (i == kFirstMeasTab)
        {
            m_dockDividerX = x + kDividerW / 2 - 1;
            x += kDividerW + kCaptionW;
        }
        m_tabs[i]->setBounds (x, dockY + 2, tabW, PnsTheme::kDockHeight - 4);
        x += tabW;
    }
}

//==============================================================================
// Plain-language summaries — what each tab found, in one sentence, for the
// headline above the view. Thresholds are "would a listener notice": ~1 dB for
// level, 0.5 dB of gain reduction, 0.01% THD. Below them the plugin is reported
// as leaving that property alone rather than quoting noise.
//==============================================================================
PnsTheme::Summary SpectrumView::summary() const
{
    if (m_warmupN < kWarmupFrames)
        return { "Play some audio  -  this compares the tone going in with the tone coming out." };

    const double sr = m_proc.getSampleRate();
    if (sr <= 0.0) return {};
    const double binW = sr / PlugNspectrPostProcessor::kFftSize;

    struct Band { const char* name; double lo, hi; };
    static constexpr Band bands[] = { { "Lows", 20.0, 250.0 }, { "Mids", 250.0, 4000.0 },
                                      { "Highs", 4000.0, 20000.0 } };
    juce::StringArray parts;
    float biggest = 0.0f;
    for (const auto& b : bands)
    {
        double ePre = 0.0, ePost = 0.0;
        for (int k = 1; k < PlugNspectrPostProcessor::kNumSpecBins; ++k)
        {
            const double f = k * binW;
            if (f < b.lo || f >= b.hi) continue;
            ePre  += (double) m_avgPre [(size_t) k] * m_avgPre [(size_t) k];
            ePost += (double) m_avgPost[(size_t) k] * m_avgPost[(size_t) k];
        }
        if (ePre < 1.0e-12 || ePost < 1.0e-12) continue;
        const float d = (float) (10.0 * std::log10 (ePost / ePre));
        if (std::abs (d) >= 1.0f)
            parts.add (juce::String (b.name) + " " + PnsTheme::fmtDb (d));
        if (std::abs (d) > std::abs (biggest)) biggest = d;
    }
    const juce::String hero = (biggest >= 0.0f ? "+" : "") + juce::String (biggest, 1);
    if (parts.isEmpty())
        return { "No real change to the tone  -  every band within 1 dB", false, "Biggest change", hero, "dB" };
    return { "Tone change:  " + parts.joinIntoString (",  "), false, "Biggest change", hero, "dB" };
}

PnsTheme::Summary DynamicsView::summary() const
{
    if (m_smoothPreDb <= -89.0f)
        return { "Play some audio  -  this shows how much the plugin turns the level down." };

    juce::String s;
    // GR is measured as "how much quieter Post is than Pre", so a compressor
    // with matching make-up gain looks like no cut. Say what was measured.
    if (m_avgGr > -0.5f && m_grPeakHold > -1.0f)
        s = "No drop in level measured  -  make-up gain can hide compression; the Compression test shows it";
    else
        s = "Turning down about " + juce::String (-m_avgGr, 1) + " dB on average, up to "
          + juce::String (-m_grPeakHold, 1) + " dB on peaks";

    const float delta = m_smoothPostDb - m_smoothPreDb;
    if (std::abs (delta) >= 0.5f)
        s << "  -  output is " << juce::String (std::abs (delta), 1) << " dB "
          << (delta > 0.0f ? "louder" : "quieter");
    return { s, false, "Average cut", juce::String (juce::jmax (0.0f, -m_avgGr), 1), "dB" };
}

PnsTheme::Summary OscilloscopeView::summary() const
{
    if (! m_hasDisplay)
        return { "Play some audio  -  or turn on Test Tone for the clearest picture" };

    const juce::String outPk (juce::Decibels::gainToDecibels (m_shapePost.peak, -90.0f), 1);
    // With music, a snapshot is a few random milliseconds; a steady tone makes
    // any change to the shape unmistakable, so say so until the tone is on.
    const juce::String advice = m_toneOn ? juce::String()
        : juce::String ("  -  for a clear picture, turn on Test Tone (bottom right)");

    if (m_shapePost.peak >= 0.999f)
        return { "Hitting 0 dBFS  -  the tops are being cut off (clipping)" + advice, true,
                 "Output peak", outPk, "dBFS" };

    using pns::analysis::ShapeChange;
    const float levelDb = (m_shapePre.rms > 1.0e-6f && m_shapePost.rms > 1.0e-6f)
                        ? 20.0f * std::log10 (m_shapePost.rms / m_shapePre.rms) : 0.0f;
    juce::String head;
    switch (pns::analysis::classifyShape (m_shapePre, m_shapePost))
    {
        case ShapeChange::HardClip: head = "Flat tops  -  hard clipping squares off the peaks"; break;
        case ShapeChange::Rounded:  head = "Rounds off the peaks  -  soft clipping or saturation"; break;
        case ShapeChange::Sharper:  head = "Sharper peaks than the input  -  adds edges or transients"; break;
        case ShapeChange::Unchanged:
            head = std::abs (levelDb) >= 0.5f
                 ? "Shape unchanged  -  just " + juce::String (std::abs (levelDb), 1) + " dB "
                   + (levelDb < 0.0f ? "quieter" : "louder")
                 : juce::String ("Shape unchanged");
            break;
    }

    return { head + advice, false, "Output peak", outPk, "dBFS" };
}

PnsTheme::Summary StereoView::summary() const
{
    if (! m_st.stereoSource)
        return { "This track is mono  -  there is no stereo image to compare." };

    const float floor = PlugNspectrPostProcessor::kStereoFloorDb + 0.01f;
    juce::String s, hero;
    const bool preMono = m_st.bbWidthPre <= floor, postMono = m_st.bbWidthPost <= floor;
    if (preMono && postMono)
        s = "Both sides identical (mono)";
    else if (preMono)                 // no side at all going in: a dB difference to the floor means nothing
        s = "Turns a mono signal into stereo";
    else if (postMono)
        s = "Collapses the stereo image to mono";
    else
    {
        const float d = m_st.bbWidthPost - m_st.bbWidthPre;
        hero = (d >= 0.0f ? "+" : "") + juce::String (d, 1);
        s = std::abs (d) < 1.0f ? juce::String ("Stereo width unchanged")
          : juce::String (d > 0.0f ? "Wider by " : "Narrower by ") + juce::String (std::abs (d), 1) + " dB";
    }

    const float monoDrop = m_st.monoLossPre - m_st.monoLossPost;   // extra dB lost when summed
    if (m_st.bbCorrPost < 0.0f || (m_st.monoLossPost < -3.0f && monoDrop > 1.0f))
        return { s + "  -  careful: loses " + juce::String (-m_st.monoLossPost, 1)
                   + " dB when played in mono", true, "Width change", hero, hero.isEmpty() ? "" : "dB" };
    return { s + "  -  safe in mono", false, "Width change", hero, hero.isEmpty() ? "" : "dB" };
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
