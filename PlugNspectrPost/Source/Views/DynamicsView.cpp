#include "DynamicsView.h"

#include <algorithm>
#include <cmath>
#include <set>

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
        const bool  audible   = measured.audible && m_lagSettled;
        const float currentGr = measured.meanDb;
        const float deepestGr = audible ? measured.minDb : 0.0f;

        const auto rms = m_proc.getRms();
        m_preConnected = rms.preValid;

        if (rms.preValid)
        {
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

        // History lane + peak hold on an audio-time clock: one point per
        // 1/30 s of audio holding the deepest cut in it, so the lane spans
        // exactly 3 s and the hold decays at the same rate at any buffer size.
        m_grDeepest = juce::jmin (m_grDeepest, rms.preValid ? deepestGr : 0.0f);
        m_grClock  += capN;
        const int perPoint = juce::jmax (1, (int) (juce::jmax (1.0, sr) / kGrRateHz));
        // One update can cover several points (big host blocks); they all
        // hold this update's measurement — it's the only one there is for them.
        const float point = juce::jlimit (-24.0f, 0.0f, m_grDeepest);
        bool emitted = false;
        while (m_grClock >= perPoint)
        {
            m_grClock -= perPoint;
            emitted = true;
            m_gr[(size_t) m_grPos] = point;
            m_grPos = (m_grPos + 1) % kGrLen;

            // Peak hold: deepest cut seen, decaying 0.998 per point (~3 s hold).
            if (point < m_grPeakHold) m_grPeakHold = point;
            else                            m_grPeakHold = juce::jmin (0.0f, m_grPeakHold * 0.998f);

            // "Cut now": the deepest cut in the last half second, so it falls
            // back to 0 when compression stops (it used to hold forever).
            float recent = 0.0f;
            for (int k = 1; k <= kGrRateHz / 2; ++k)
                recent = juce::jmin (recent, m_gr[(size_t) ((m_grPos - k + kGrLen) % kGrLen)]);
            m_instantGr = recent;
        }
        if (emitted) m_grDeepest = 0.0f;
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
    m_grClock    = 0;
    m_grDeepest  = 0.0f;
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
    m_grClock   = 0;
    m_grDeepest = 0.0f;

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
