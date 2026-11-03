#include "StereoView.h"
#include "ViewCommon.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <cstring>

//==============================================================================
// StereoView
//==============================================================================
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

void StereoView::consumeCapture (const CaptureChunk& cap)
{
    if (cap.n <= 0) return;
    // Keep at most a ring's worth (the processor's ring is the limit anyway).
    constexpr int kMaxPending = PlugNspectrPostProcessor::kCaptureRingLen;
    const int n = juce::jmin (cap.n, kMaxPending);
    if (m_pendingN + n > kMaxPending)
    {
        const int drop = m_pendingN + n - kMaxPending;   // oldest first
        for (int ch = 0; ch < 4; ++ch)
            std::memmove (m_pending.getWritePointer (ch), m_pending.getReadPointer (ch) + drop,
                          (size_t) (m_pendingN - drop) * sizeof (float));
        m_pendingN -= drop;
    }
    if (m_pending.getNumSamples() < m_pendingN + n)
        m_pending.setSize (4, juce::jmax (m_pendingN + n, m_pending.getNumSamples() * 2), true, false, true);

    using P = PlugNspectrPostProcessor;
    const int src = cap.n - n;
    m_pending.copyFrom (0, m_pendingN, cap.post, P::kCapL, src, n);
    m_pending.copyFrom (1, m_pendingN, cap.post, P::kCapR, src, n);
    m_pending.copyFrom (2, m_pendingN, cap.pre,  P::kCapL, src, n);
    m_pending.copyFrom (3, m_pendingN, cap.pre,  P::kCapR, src, n);
    m_pendingN += n;
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
        const int n = m_pendingN;
        m_pendingN = 0;
        if (n > 0)
        {
            const float* pl = m_pending.getReadPointer (0);
            const float* pr = m_pending.getReadPointer (1);
            const float* ql = m_pending.getReadPointer (2);
            const float* qr = m_pending.getReadPointer (3);

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
    if (haveBoth && m_cursor.x >= 0.0f)
    {
        const float cx = juce::jlimit (plot.getX(), plot.getRight(), m_cursor.x);
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
                            PnsTheme::kColorPost, rows, m_cursor.locked);
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
    if (m_cursor.move (e.position, true)) repaint();
}

void StereoView::mouseExit (const juce::MouseEvent&)
{
    if (m_cursor.exit()) repaint();
}

void StereoView::mouseDown (const juce::MouseEvent& e)
{
    if (m_cursor.down (e.position, true)) repaint();
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
