#include "SpectrumView.h"

#include <algorithm>
#include <cmath>
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
                        if (mk.bin == b)
                        {
                            // Back in the top set: stop any fade-out under way,
                            // or a still-current marker vanishes for 5 s.
                            mk.score = newScores[i]; mk.isBoost = isBoost; mk.fadingIn = true;
                            break;
                        }
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
