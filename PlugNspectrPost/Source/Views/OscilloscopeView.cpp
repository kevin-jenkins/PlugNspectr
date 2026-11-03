#include "OscilloscopeView.h"

#include <algorithm>
#include <cmath>
#include <set>

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
// Every sample goes into the ring, so the 10/50/100 ms windows mean what they
// say regardless of the host buffer size.
void OscilloscopeView::consumeCapture (const CaptureChunk& cap)
{
    const int n = cap.n;
    if (n <= 0) return;
    const float* prePtr  = cap.pre .getReadPointer (0);
    const float* postPtr = cap.post.getReadPointer (0);

    for (int i = 0; i < n; ++i)
    {
        m_ringPre [m_ringWrite] = prePtr  ? prePtr [i] : 0.0f;
        m_ringPost[m_ringWrite] = postPtr ? postPtr[i] : 0.0f;
        m_ringWrite = (m_ringWrite + 1) % kRingLen;
        if (m_ringAvail < kRingLen) ++m_ringAvail;
    }
    m_totalWritten += (uint64_t) n;
}

void OscilloscopeView::update()
{

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

        // A rising zero crossing that leads to a real edge: the signal must
        // climb past ~-54 dBFS before it next goes negative. (Testing only the
        // first sample's step never triggered on low or quiet tones — 100 Hz
        // at -6 dBFS rises ~0.0065 per sample.) Noise around zero still fails.
        bool edge = false;
        if (prev < 0.0f && curr >= 0.0f)
            for (uint64_t k = i; k < juce::jmin (i + 512, m_totalWritten); ++k)
            {
                const float v = ringGet (m_ringPost, k);
                if (v > 0.002f) { edge = true; break; }
                if (v < 0.0f)   break;
            }
        if (edge)
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
