/*
  ==============================================================================
    PlugNspectrPost  –  PluginEditor.h

    Two-tab editor:
      Tab 0 "Spectrum"  — dual FFT spectrum analyzer (PRE blue, POST orange,
                          Avg cyan) with EMA smoothing + peak hold
      Tab 1 "Dynamics"  — waveform comparison (top) + GR scrolling meter (bottom)
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PlugNspectrTheme.h"
#include "PluginProcessor.h"
#include "PnsAnalysis.h"
// (logo is embedded in PnsLogoData.h — no Projucer BinaryData dependency)

//==============================================================================
// ViewOptions — a live view's settings, folded behind one pill at the view's
// top right ("Smoothing: Medium · Avg: on"). Clicking it opens a panel of
// labelled rows of choices, each with a one-line plain explanation, plus an
// optional "Start over" action. Keeps the graph clear of small buttons while
// the current settings stay readable at a glance.
//==============================================================================
class ViewOptions : public juce::Component
{
public:
    ViewOptions()
    {
        m_pill.onClick = [this] { setOpen (! m_open); };
        addAndMakeVisible (m_pill);
        m_resetBtn.setButtonText ("Start over");
        m_resetBtn.onClick = [this] { if (m_onReset) m_onReset(); setOpen (false); };
        addChildComponent (m_resetBtn);
    }

    // Adds a row; onPick runs with the chosen index. Returns the row index.
    int addRow (const juce::String& title, const juce::String& hint, const juce::StringArray& choices,
                int selected, std::function<void (int)> onPick)
    {
        auto& row = m_rows.emplace_back();
        row.title = title; row.hint = hint; row.choices = choices; row.selected = selected;
        row.onPick = std::move (onPick);
        const int rowIdx = (int) m_rows.size() - 1;
        for (int i = 0; i < choices.size(); ++i)
        {
            auto* b = m_segs.add (new juce::TextButton (choices[i]));
            b->setClickingTogglesState (false);
            b->onClick = [this, rowIdx, i] { pick (rowIdx, i); };
            addChildComponent (b);
            row.buttons.push_back (b);
        }
        refresh();
        return rowIdx;
    }
    void setResetAction (const juce::String& hint, std::function<void()> fn)
    {
        m_resetHint = hint; m_onReset = std::move (fn);
        refresh();
    }

    // Size the component itself: the pill when closed, pill + panel when open.
    // `anchorRight` / `anchorTop` are in the parent's coordinates.
    void place (int anchorRight, int anchorTop)
    {
        m_anchorRight = anchorRight; m_anchorTop = anchorTop;
        const int pw = pillWidth();
        const int w  = m_open ? juce::jmax (pw, kPanelW) : pw;
        int h = kPillH;
        if (m_open)
        {
            h += 8 + kPanelPad;
            for (size_t i = 0; i < m_rows.size(); ++i) h += kRowH;
            if (m_onReset) h += 46;
            h += kPanelPad - 10;
        }
        setBounds (anchorRight - w, anchorTop, w, h);
    }

    void resized() override
    {
        const int w = getWidth();
        m_pill.setBounds (w - pillWidth(), 0, pillWidth(), kPillH);
        int y = kPillH + 8 + kPanelPad;
        for (auto& row : m_rows)
        {
            const int segY = y + 18, n = (int) row.buttons.size();
            const int segW = (w - 2 * kPanelPad) / juce::jmax (1, n);
            for (int i = 0; i < n; ++i)
            {
                row.buttons[(size_t) i]->setBounds (kPanelPad + i * segW, segY, segW, 26);
                row.buttons[(size_t) i]->setVisible (m_open);
            }
            y += kRowH;
        }
        m_resetBtn.setBounds (kPanelPad, y + 4, w - 2 * kPanelPad, 26);
        m_resetBtn.setVisible (m_open && m_onReset != nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        if (! m_open) return;
        const auto panel = panelBounds().toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (panel.translated (0.0f, 4.0f), 12.0f);
        g.setColour (PnsTheme::kBgPanel.withAlpha (0.97f));
        g.fillRoundedRectangle (panel, 12.0f);
        g.setColour (PnsTheme::kBorderActive);
        g.drawRoundedRectangle (panel.reduced (0.5f), 12.0f, 1.0f);

        int y = kPillH + 8 + kPanelPad;
        for (auto& row : m_rows)
        {
            g.setFont (PnsTheme::fontSized (10.5f, true));
            g.setColour (PnsTheme::kTextSecondary);
            g.drawText (row.title.toUpperCase(), kPanelPad, y, getWidth() - 2 * kPanelPad, 14,
                        juce::Justification::centredLeft);
            g.setFont (PnsTheme::fontLabel());
            g.setColour (PnsTheme::kGridLabel);
            g.drawText (row.hint, kPanelPad, y + 48, getWidth() - 2 * kPanelPad, 14,
                        juce::Justification::centredLeft);
            y += kRowH;
        }
        if (m_onReset)
        {
            g.setFont (PnsTheme::fontLabel());
            g.setColour (PnsTheme::kGridLabel);
            g.drawText (m_resetHint, kPanelPad, y + 32, getWidth() - 2 * kPanelPad, 14,
                        juce::Justification::centredLeft);
        }
    }

    ~ViewOptions() override { juce::Desktop::getInstance().removeGlobalMouseListener (this); }

    // Only the pill and the panel itself take clicks — not the strip beside a
    // narrow pill or the gap above the panel, which belong to the graph.
    bool hitTest (int x, int y) override
    {
        return m_pill.getBounds().contains (x, y) || (m_open && panelBounds().contains (x, y));
    }

    // While open this is a global mouse listener: a press anywhere outside the
    // pill and panel closes it, like a menu.
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! m_open || e.eventComponent == this || isParentOf (e.eventComponent)) return;
        setOpen (false);
    }

    void setOpen (bool o)
    {
        if (o == m_open) return;
        m_open = o;
        if (o) juce::Desktop::getInstance().addGlobalMouseListener (this);
        else   juce::Desktop::getInstance().removeGlobalMouseListener (this);
        place (m_anchorRight, m_anchorTop);
        resized();
        repaint();
    }

private:
    struct Row
    {
        juce::String title, hint;
        juce::StringArray choices;
        int selected = 0;
        std::function<void (int)> onPick;
        std::vector<juce::TextButton*> buttons;
    };

    static constexpr int kPillH = 26, kPanelW = 290, kPanelPad = 14, kRowH = 70;

    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withTrimmedTop (kPillH + 8); }

    void pick (int rowIdx, int i)
    {
        auto& row = m_rows[(size_t) rowIdx];
        row.selected = i;
        if (row.onPick) row.onPick (i);
        refresh();
    }
    void refresh()
    {
        juce::StringArray parts;
        for (auto& row : m_rows)
        {
            parts.add (row.title + ": " + row.choices[row.selected]);
            for (size_t i = 0; i < row.buttons.size(); ++i)
                row.buttons[i]->setToggleState ((int) i == row.selected, juce::dontSendNotification);
        }
        m_pill.setButtonText ((parts.isEmpty() ? juce::String ("Options")
                                                 : parts.joinIntoString (juce::String::fromUTF8 ("  \xc2\xb7  "))));
        if (getParentComponent() != nullptr) { place (m_anchorRight, m_anchorTop); resized(); }
    }
    int pillWidth() const
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (PnsTheme::fontLabel(), m_pill.getButtonText(), 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + 34;
    }

    juce::TextButton m_pill, m_resetBtn;
    juce::OwnedArray<juce::TextButton> m_segs;
    std::vector<Row> m_rows;
    juce::String m_resetHint;
    std::function<void()> m_onReset;
    bool m_open = false;
    int  m_anchorRight = 0, m_anchorTop = 0;
};

//==============================================================================
// Spectrum analyzer view
//==============================================================================
class SpectrumView : public juce::Component
{
public:
    explicit SpectrumView (PlugNspectrPostProcessor& p);
    void paint   (juce::Graphics& g) override;
    void resized ()                   override;
    void update  ();
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner
    void reset   ();   // clear rolling averages, peak-hold, and fluctuation markers

    enum class SmoothPreset { Fast, Medium, Slow };
    void setSmoothPreset (SmoothPreset p);

private:
    PlugNspectrPostProcessor& m_proc;

    // Raw spectra from the processor
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_rawPre  {};
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_rawPost {};

    // EMA-smoothed display buffers
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_pre  {};
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_post {};

    // Peak-hold buffers
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_peakPre  {};
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_peakPost {};

    static constexpr float kPeakDecay = 0.995f;

    float m_decay = 0.85f;

    // Long-term rolling averages — PRE and POST (slow EMA, ~300-frame time constant)
    // Approximated as decay = (300-1)/300 ≈ 0.9967 per frame.
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_avgPre  {};
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_avgPost {};
    static constexpr float kAvgDecay = 299.0f / 300.0f;

    bool m_showAvg = true;

    ViewOptions      m_options;   // smoothing, long-term average, start over

    // Interactive hairline
    float m_mouseX     = -1.0f;   // x in component coords, -1 = not in plot
    bool  m_mouseLocked = false;   // true after click — hairline stays until next click
    bool  m_mouseInPlot = false;

    void mouseMove  (const juce::MouseEvent&) override;
    void mouseExit  (const juce::MouseEvent&) override;
    void mouseDown  (const juce::MouseEvent&) override;

    // ── Fluctuation markers ────────────────────────────────────────────────
    static constexpr int   kMarkerUpdateFrames = 150;           // 5 s update cadence at 30 fps
    static constexpr int   kWarmupFrames       = 90;            // 3 s warmup — nothing shown until elapsed
    static constexpr int   kSilenceFrames      = 90;            // 3 s silence triggers full warmup reset
    static constexpr int   kNumPeakMarkers     = 5;
    static constexpr float kMarkerFadeInRate   = 1.0f / 15.0f;  // 0.5 s fade-in at 30 fps
    static constexpr float kMarkerFadeOutRate  = 1.0f / 30.0f;  // 1 s fade-out at 30 fps
    static constexpr float kLerpRate           = 0.05f;         // ~2 s exponential lerp to new position
    static constexpr float kScoreHysteresis    = 1.20f;         // new bin needs 20% higher score to displace
    // Screen-space spacing: 80 px general; 40 px below 100 Hz (log scale compresses low end)
    static constexpr float kMinPixelSpacing    = 80.0f;
    static constexpr float kMinPixelSpacingLo  = 40.0f;         // used when freq < 100 Hz

    int m_markerN  = 0;  // frame counter for 5-second recalc cadence
    int m_warmupN  = 0;  // non-silent frames accumulated; gates display until kWarmupFrames
    int m_silenceN = 0;  // consecutive silent frames; triggers reset at kSilenceFrames

    struct PeakMarkerState
    {
        int   bin;         // target bin (from selection)
        float displayBin;  // current rendered position (lerps toward bin each frame)
        float alpha;
        bool  fadingIn;
        float score;       // deviation score at bin — used for 20% hysteresis
        bool  isBoost;     // true when Post has signal but Pre dropped out (plugin adding content)
    };
    std::vector<PeakMarkerState>       m_peakMarkers;
    std::array<int,   kNumPeakMarkers> m_topBins   { -1,-1,-1,-1,-1 };
    std::array<float, kNumPeakMarkers> m_topScores {  0.f, 0.f, 0.f, 0.f, 0.f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumView)
};

//==============================================================================
// Dynamics view — waveform comparison (top) + GR scrolling meter (bottom)
//==============================================================================

//==============================================================================
class DynamicsView : public juce::Component
{
public:
    explicit DynamicsView (PlugNspectrPostProcessor& p);
    void paint   (juce::Graphics& g) override;
    void resized ()                   override;
    void update  ();
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner
    void reset   ();   // clear all accumulated readings and history (keeps the zoom)
    void resetReadings();   // clear the cut readings only (average, peak hold, now); keeps the waveform
    void openOptionsForTest() { m_options.setOpen (true); }
    // Post-vs-Pre latency, measured once by the editor's LagTracker. Until it
    // has settled, gain reduction isn't recorded: unaligned transients would
    // read as cuts.
    void setLagSamples (int lag, bool settled)
    {
        m_lagSamples = juce::jlimit (0, kSampleBufLen / 2, lag);
        m_lagSettled = settled;
    }

private:
    //──────────────────────────────────────────────────────────────────────────
    // Waveform sample ring buffer — raw audio from each captured block.
    // 576 000 samples ≈ 12 s at 48 kHz; drawn as per-pixel min/max columns.
    //──────────────────────────────────────────────────────────────────────────
    static constexpr int kSampleBufLen = 576000;

    std::array<float, kSampleBufLen> m_preSamples  {};
    std::array<float, kSampleBufLen> m_postSamples {};
    int                              m_sampleWritePos = 0;
    int                              m_samplesStored  = 0;
    juce::int64                      m_totalSamplesWritten = 0;  // monotonic; anchors absolute-aligned bins

    // How far Post lags Pre, in samples (set by the editor's LagTracker). Pre
    // is read back by this much before comparing levels; without it every
    // transient reads as a cut.
    // Auto-restart: when the cut changes for good (a plugin switched off or
    // on, a big setting change), the 30 s average and peak hold would take a
    // minute or more to forget the old state. A fast average (~0.5 s) that
    // stays > 1.5 dB away from the 30 s one for 1.5 s restarts the readings.
    float  m_fastGr          = 0.0f;
    double m_shiftSinceMs    = 0.0;   // when the fast/slow disagreement began; 0 = agreeing
    double m_restartNoteEnd  = 0.0;   // show "readings restarted" until this time

    int  m_lagSamples = 0;
    bool m_lagSettled = false;
    std::vector<float> m_alignPre, m_alignPost;   // delay-aligned copy of the newest audio

    // Zoom: visible window in seconds (6 / 12)
    int m_zoomSeconds = 12;

    // Vertical scale — calibrated once from the first ~2s of audio, then locked
    // so the song's natural loud/soft dynamics stay visible. Re-arms after ~1s
    // of silence (transport stop) so the next playback recalibrates.
    // 1.0 = no zoom (true amplitude).
    //
    // The initial value (before calibration locks) assumes the signal opens at
    // ~-12 dBFS RMS — a safe floor for typical program material — so the view
    // starts at a sensible zoom instead of all the way out. Sized so a -12 dB
    // RMS level sits at ~half the display height (0.5 / 10^(-12/20) ≈ 2.0),
    // leaving headroom for peaks. Calibration refines it within ~2s.
    static constexpr float kInitialWaveScale = 2.0f;
    float m_waveScale        = kInitialWaveScale;
    bool  m_waveScaleLocked  = false;
    int   m_waveCalibSamples = 0;      // audio samples accumulated this calibration
    float m_waveCalibPeak    = 0.0f;   // running peak during calibration
    int   m_waveSilenceCount = 0;      // consecutive silent update() ticks (re-arm)

    // Per-column waveform envelope (amplitude, not pixels). Built once in
    // update() and rendered by drawWaveform(), so the buffer is scanned once
    // per frame and the calibration peak comes from the same pass.
    static constexpr int kWaveCols = 120;
    std::array<float, kWaveCols> m_wavePreTop  {};
    std::array<float, kWaveCols> m_wavePreBot  {};
    std::array<float, kWaveCols> m_wavePostTop {};
    std::array<float, kWaveCols> m_wavePostBot {};
    int   m_waveColsValid  = 0;        // number of columns with valid data (0 = none yet)
    float m_waveScrollFrac = 0.0f;     // 0..1 fill of the rightmost (newest) bin — sub-column scroll offset

    ViewOptions      m_options;   // history length, start over

    // GR readout — instantaneous + 30s rolling average + peak hold
    float  m_instantGr        = 0.0f;
    float  m_avgGr            = 0.0f;   // calculated rolling average
    float  m_grPeakHold       = 0.0f;  // most-negative GR seen, decays slowly
    double m_grFlashEnd       = 0.0;    // ms timestamp; 0 = not flashing
    bool   m_mouseOverReadout = false;
    juce::Rectangle<int> m_chipBounds;   // readout chips, laid out in drawWaveform

    // 30-second GR average buffer — 900 slots at 30fps
    // One entry per update that carried audio, with how many samples it
    // covered, so the average spans 30 s of audio whatever the host's block
    // size or the UI rate. 4096 entries covers 30 s down to ~7 ms blocks.
    static constexpr int kAvgGrLen = 4096;
    std::array<float, kAvgGrLen> m_avgGrBuf {};
    std::array<int,   kAvgGrLen> m_avgGrDur {};   // samples each entry covers
    int                          m_avgGrPos  = 0;
    int                          m_avgGrFill = 0;   // number of valid entries

    // Volume (RMS) display — smoothed with 0.9 decay
    float m_smoothPreDb  = -90.0f;
    float m_smoothPostDb = -90.0f;

    //──────────────────────────────────────────────────────────────────────────
    // GR scrolling history (circular buffer, 3 s @ 30 fps)
    //──────────────────────────────────────────────────────────────────────────
    // 3 s at 30 points per second of AUDIO (not per UI tick, which varies with
    // the host block size), so the lane's "-3s" axis is true at any buffer.
    static constexpr int kGrLen = 90, kGrRateHz = 30;
    int   m_grClock  = 0;         // samples since the last history point
    float m_grDeepest = 0.0f;     // deepest cut since the last history point

    std::array<float, kGrLen> m_gr {};
    int                       m_grPos = 0;

    //──────────────────────────────────────────────────────────────────────────
    PlugNspectrPostProcessor& m_proc;
    // Capture-ring drain cursor + scratch chunk (see readCaptureSince).
    uint64_t                  m_capReadPos = 0;
    juce::AudioBuffer<float>  m_capPre, m_capPost;
    bool                      m_preConnected     = false;

    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove        (const juce::MouseEvent&) override;
    void mouseExit        (const juce::MouseEvent&) override;

    void drawWaveform (juce::Graphics&, juce::Rectangle<float>);
    void drawGrMeter  (juce::Graphics&, juce::Rectangle<float>);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicsView)
};

//==============================================================================
// Oscilloscope view — triggered zero-crossing time-domain display
//==============================================================================
class OscilloscopeView : public juce::Component
{
public:
    explicit OscilloscopeView (PlugNspectrPostProcessor& p);
    void paint   (juce::Graphics& g) override;
    void resized ()                   override;
    void update  ();
    // Post-vs-Pre latency (measured by the Dynamics view): Pre is drawn this
    // many samples earlier so both traces show the same moment of audio.
    void setLagSamples (int lag) { m_lag = juce::jlimit (0, kRingLen / 2, lag); }
    void setToneActive (bool on) { m_toneOn = on; }   // steers the headline's advice
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner

private:
    // Ring buffer: ~1s at 48kHz, enough for trigger search + capture
    static constexpr int kRingLen    = 48000;
    // Max display samples: 100ms at up to 192kHz
    static constexpr int kMaxCapture = 20000;

    std::array<float, kRingLen>    m_ringPre    {};
    std::array<float, kRingLen>    m_ringPost   {};
    int      m_ringWrite  = 0;
    int      m_ringAvail  = 0;
    uint64_t m_totalWritten = 0;
    uint64_t m_searchFrom   = 0;
    uint64_t m_capReadPos = 0;
    juce::AudioBuffer<float> m_capPre, m_capPost;

    // Display buffer — written by update(), read by paint()
    std::array<float, kMaxCapture> m_displayPre  {};
    std::array<float, kMaxCapture> m_displayPost {};
    int  m_displayCount = 0;
    bool m_hasDisplay   = false;

    // Time window control
    int m_windowMs = 50;

    // How often a new snapshot replaces the picture. Real program material is
    // not periodic, so redrawing every frame shows a different snippet 60 times
    // a second; a few per second is readable, and Hold freezes it to study.
    enum class Refresh { Live, Slow, Hold };
    Refresh m_refresh = Refresh::Slow;
    double  m_lastCaptureMs = 0.0;
    int     m_lag = 0;
    bool    m_toneOn = false;

    // Level matching: Post is drawn scaled to Pre's loudness so only the shape
    // difference is left to see. Off shows the real levels.
    bool  m_matchLevels = true;

    // Shape of the current snapshot, behind the headline's verdict (PnsAnalysis.h).
    pns::analysis::Shape m_shapePre, m_shapePost;
    void setWindowMs (int ms);

    ViewOptions      m_options;   // zoom, level matching, update rate

    float ringGet (const std::array<float, kRingLen>& buf, uint64_t idx) const;

    PlugNspectrPostProcessor& m_proc;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscilloscopeView)
};

//==============================================================================
// Stereo view — what the plugin does to the stereo image. Two stacked lanes
// (width = Side/Mid in dB, and correlation) comparing Pre against Post, plus a
// 45°-rotated goniometer and the broadband readouts. Width says how much side
// energy there is; correlation says whether it is real decorrelation or a phase
// trick that collapses in mono.
//==============================================================================
class StereoView : public juce::Component
{
public:
    explicit StereoView (PlugNspectrPostProcessor& p);
    void paint   (juce::Graphics& g) override;
    void resized ()                   override;
    void update  ();
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner
    void reset   ();   // clear the averages and the goniometer trail

    void setCursorForTest (int x) { m_cursorX = (float) x; m_cursorLocked = true; }

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int kBins = PlugNspectrPostProcessor::kNumSpecBins;
    static constexpr float kLoHz = 20.0f, kHiHz = 20000.0f;

    // Goniometer trail — a ring of rotated L/R points (X = (L-R)/√2, Y = (L+R)/√2,
    // so mono draws a vertical line, the convention engineers expect).
    // ~21 ms at 48 kHz. Short on purpose: as a continuous trace this is a
    // Lissajous, and a long trail turns dense material into a solid scribble.
    // Classic goniometers show a comparable window.
    static constexpr int kGonioPoints = 1024;
    std::array<float, kGonioPoints> m_gxPost {}, m_gyPost {};
    std::array<float, kGonioPoints> m_gxPre  {}, m_gyPre  {};
    int      m_gonioPos  = 0;
    int      m_gonioFill = 0;
    uint64_t m_capReadPos = 0;
    juce::AudioBuffer<float> m_capPre, m_capPost;
    float    m_gonioScale = 2.0f;   // smoothed auto-gain so quiet material still reads
    float    m_gonioPeak  = 0.0f;   // decaying peak envelope the gain tracks

    float m_cursorX = -1.0f;
    bool  m_cursorLocked = false;

    // Display copies, smoothed over a fractional-octave window. The raw per-bin
    // estimates are far too jagged to compare by eye: the FFT is linearly spaced,
    // so an octave up at 10 kHz spans hundreds of bins and single-bin variance
    // swamps the shape. Smoothing is what makes the Pre/Post difference legible.
    static constexpr double kSmoothOctaves = 1.0 / 6.0;
    std::array<float, kBins> m_dWidthPre {}, m_dWidthPost {};
    std::array<float, kBins> m_dCorrPre  {}, m_dCorrPost  {};
    std::array<double, kBins + 1> m_cumV {}, m_cumN {};   // prefix sums, O(bins)
    void smoothOctave (const std::array<float, kBins>& in, std::array<float, kBins>& out);

    float freqToX (double hz, juce::Rectangle<float> plot) const;
    // Everything that differs between the two lanes, so drawLane stays one
    // function and the call sites read as a description of each lane.
    struct LaneSpec
    {
        const char* title;
        float       vMin, vMax;
        const char* unit;
        int         steps;         // gridline intervals (chosen so labels are round)
        bool        fillDelta;     // amber Pre->Post difference fill
        const char* loTag;         // plain-language label at the bottom of the scale
        const char* hiTag;         // ...and at the top
        bool        cautionAbove;  // shade the notable half: above 0 (width) or below it (corr)
        const char* cautionTag;    // what that shaded region means
        bool        legend;        // draw the Pre/Post key (once, on the upper lane)
    };
    void  drawLane (juce::Graphics& g, juce::Rectangle<float> area, const LaneSpec& spec,
                    const std::array<float, kBins>& pre,
                    const std::array<float, kBins>& post) const;
    void  drawGonio    (juce::Graphics& g, juce::Rectangle<float> area) const;
    void  drawReadouts (juce::Graphics& g, juce::Rectangle<float> area) const;

    PlugNspectrPostProcessor& m_proc;
    PlugNspectrPostProcessor::StereoResult m_st;
    ViewOptions m_options;   // start over

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoView)
};

//==============================================================================
// Harmonics view — THD analysis using an internal test tone
//==============================================================================
class HarmonicsView : public juce::Component
{
public:
    explicit HarmonicsView (PlugNspectrPostProcessor& p);

    void paint   (juce::Graphics& g) override;
    void resized ()                   override {}
    void update  ();
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner

    // Called by the editor when the user changes the dock's tone controls
    void setToneFreq   (double freq);
    void setToneActive (bool active);

private:
    // ── Test-tone state (set by the editor's dock) ─────────────────────────────
    bool   m_toneActive     = false;
    bool   m_harmonicsPaused = false;   // true after tone deactivated — freezes last frame
    double m_toneFreq       = 1000.0;

    // ── Harmonic analysis ─────────────────────────────────────────────────
    static constexpr int kNumH = 8;   // H1 .. H8

    // Smoothed magnitudes in linear scale (not dB) — H1=index 0
    std::array<float, kNumH> m_harmPre  {};
    std::array<float, kNumH> m_harmPost {};

    float m_thdPre  = 0.0f;
    float m_thdPost = 0.0f;

    // Local copies of the FFT spectra for painting
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_specPre  {};
    std::array<float, PlugNspectrPostProcessor::kNumSpecBins> m_specPost {};

    // Hover state for harmonic dot tooltips
    int m_hoveredHarmonic = -1;   // 0-based index into H2-H8 (stored as H index 1-7)

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    // ── Drawing helpers ───────────────────────────────────────────────────
    void drawSpectrumArea (juce::Graphics&, juce::Rectangle<float> area);
    void drawReadouts     (juce::Graphics&, juce::Rectangle<float> area);

    PlugNspectrPostProcessor& m_proc;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmonicsView)
};

//==============================================================================
// Linear view — transfer-function magnitude / phase / group-delay measurement
//==============================================================================
class LinearView : public juce::Component
{
public:
    explicit LinearView (PlugNspectrPostProcessor& p);
    void paint   (juce::Graphics& g) override;
    void resized ()                   override;
    void update  ();
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner

    bool isMeasureActive() const { return m_measureActive; }
    std::function<void()> onMeasureChanged;   // editor wires this to writeCmdBlock()

    // Snapshot the current curves as a dimmed reference overlay (A/B compare).
    void freezeForTest();
    void setMeasureForTest (bool on) { m_measureActive = on;
                                       m_measureBtn.setToggleState (on, juce::dontSendNotification); }
    bool disarm() { if (! m_measureActive) return false;
                    m_measureActive = false;
                    m_measureBtn.setToggleState (false, juce::dontSendNotification);
                    repaint(); return true; }
    void setCursorForTest (int x) { m_cursorX = (float) x; m_cursorLocked = true; }

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int kBins = PlugNspectrPostProcessor::kMeasBins;
    float m_cursorX = -1.0f;     // hairline x in view coords (-1 = hidden)
    bool  m_cursorLocked = false;

    void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, const char* title,
                    const std::array<float, kBins>& vals, float vMin, float vMax,
                    const juce::String& unit, juce::Colour curve,
                    const std::array<float, kBins>* frozen) const;
    float freqToX (double f, juce::Rectangle<float> r) const;
    void doFreeze();

    PlugNspectrPostProcessor& m_proc;

    PlugNspectrPostProcessor::MeasResult m_meas;
    std::array<float, kBins> m_phaseDeg {};   // wrapped, degrees (display)
    std::array<float, kBins> m_groupMs  {};   // group delay, ms (from unwrapped phase)
    float m_gdLo = -2.0f, m_gdHi = 10.0f;     // auto-scaled group-delay range
    int   m_latSamples = 0;                    // measured bulk latency (samples)
    float m_latMs      = 0.0f;                  // …in ms (compensated out of phase)

    // Frozen reference (A/B compare)
    bool m_hasFrozen = false;
    std::array<float, kBins> m_frozenMag {};
    std::array<float, kBins> m_frozenPhase {};
    std::array<float, kBins> m_frozenGroup {};

    juce::TextButton m_measureBtn { "Measure" };
    juce::TextButton m_freezeBtn  { "Freeze"  };
    bool m_measureActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LinearView)
};

//==============================================================================
// Transfer view — measured dynamics transfer curve (output level vs input level)
//==============================================================================
class TransferView : public juce::Component
{
public:
    explicit TransferView (PlugNspectrPostProcessor& p);
    void paint   (juce::Graphics& g) override;
    void resized ()                   override;
    void update  ();
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner

    bool isMeasureActive() const { return m_measureActive; }
    std::function<void()> onMeasureChanged;
    void freezeForTest();
    void setCursorForTest (int x) { m_cursorX = (float) x; m_cursorLocked = true; }
    bool disarm() { if (! m_measureActive) return false;
                    m_measureActive = false;
                    m_measureBtn.setToggleState (false, juce::dontSendNotification);
                    repaint(); return true; }

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int kBins = PlugNspectrPostProcessor::kDynBins;
    void doFreeze();
    float m_cursorX = -1.0f;
    bool  m_cursorLocked = false;

    PlugNspectrPostProcessor& m_proc;
    PlugNspectrPostProcessor::DynResult m_dyn;

    bool m_hasFrozen = false;
    std::array<float, kBins> m_frozenOut {};
    std::array<bool,  kBins> m_frozenValid {};

    juce::TextButton m_measureBtn { "Measure" };
    juce::TextButton m_freezeBtn  { "Freeze"  };
    bool m_measureActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransferView)
};

//==============================================================================
// Envelope view — measured attack/release (gain reduction vs time)
//==============================================================================
class EnvelopeView : public juce::Component
{
public:
    explicit EnvelopeView (PlugNspectrPostProcessor& p);
    void paint   (juce::Graphics& g) override;
    void resized ()                   override;
    void update  ();
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner

    bool isMeasureActive() const { return m_measureActive; }
    std::function<void()> onMeasureChanged;
    void setCursorForTest (int x) { m_cursorX = (float) x; m_cursorLocked = true; }
    bool disarm() { if (! m_measureActive) return false;
                    m_measureActive = false;
                    m_measureBtn.setToggleState (false, juce::dontSendNotification);
                    repaint(); return true; }

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int kBins = PlugNspectrPostProcessor::kEnvBins;
    float m_cursorX = -1.0f;
    bool  m_cursorLocked = false;

    PlugNspectrPostProcessor& m_proc;
    PlugNspectrPostProcessor::EnvResult m_env;
    float m_grHi = 6.0f;   // auto-scaled GR range top

    juce::TextButton m_measureBtn { "Measure" };
    bool m_measureActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeView)
};

//==============================================================================
// THD-sweep view — total harmonic distortion vs frequency
//==============================================================================
class ThdSweepView : public juce::Component
{
public:
    explicit ThdSweepView (PlugNspectrPostProcessor& p);
    void paint   (juce::Graphics& g) override;
    void resized ()                   override;
    void update  ();
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner

    bool isMeasureActive() const { return m_measureActive; }
    std::function<void()> onMeasureChanged;
    void freezeForTest();
    void setCursorForTest (int x) { m_cursorX = (float) x; m_cursorLocked = true; }
    bool disarm() { if (! m_measureActive) return false;
                    m_measureActive = false;
                    m_measureBtn.setToggleState (false, juce::dontSendNotification);
                    repaint(); return true; }

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int kBins = PlugNspectrPostProcessor::kThdBins;
    void doFreeze();
    float m_cursorX = -1.0f;
    bool  m_cursorLocked = false;

    PlugNspectrPostProcessor& m_proc;
    PlugNspectrPostProcessor::ThdResult m_thd;
    float m_thdHi = 1.0f;   // auto-scaled %THD range top

    bool m_hasFrozen = false;
    std::array<float, kBins> m_frozenThd {};
    std::array<bool,  kBins> m_frozenValid {};

    juce::TextButton m_measureBtn { "Measure" };
    juce::TextButton m_freezeBtn  { "Freeze"  };
    bool m_measureActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThdSweepView)
};

//==============================================================================
// InfoButton — a small circled "i" that opens the About modal.
//==============================================================================
class InfoButton : public juce::Button
{
public:
    InfoButton() : juce::Button ("About") { setTooltip ("About PlugNspectr"); }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto b = getLocalBounds().toFloat();
        const float d = juce::jmin (b.getWidth(), b.getHeight()) - 2.0f;
        const auto circ = juce::Rectangle<float> (d, d).withCentre (b.getCentre());
        const float cx = circ.getCentreX(), cy = circ.getCentreY();

        g.setColour ((over || down) ? PnsTheme::kAccentPrimary : PnsTheme::kTextPrimary);
        g.drawEllipse (circ, 1.5f);

        // Lowercase "i" as a dot + stem (filled shapes) so it stays crisp + bold here.
        const float dotD  = juce::jmax (2.2f, d * 0.20f);
        const float stemW = juce::jmax (2.0f, d * 0.18f);
        g.fillEllipse (cx - dotD * 0.5f, cy - d * 0.29f, dotD, dotD);                  // dot
        g.fillRoundedRectangle (cx - stemW * 0.5f, cy - d * 0.07f, stemW, d * 0.34f,   // stem
                                stemW * 0.5f);
    }
};

//==============================================================================
// AboutOverlay — dark scrim + centred card (Softube-style). Content is iterated
// on later; for now it shows the logo, name, version/format/OS, placeholder
// action buttons, and a copyright line. Click the scrim or the X to dismiss.
//==============================================================================
class AboutOverlay : public juce::Component
{
public:
    AboutOverlay()
    {
        addAndMakeVisible (m_close);
        m_close.setButtonText ("X");
        m_close.onClick = [this] { setVisible (false); };

        m_manual.setButtonText ("Open Manual");
        m_manual.onClick = []
        {
            juce::URL ("https://github.com/kevin-jenkins/PlugNspectr/blob/main/README.md")
                .launchInDefaultBrowser();
        };
        addAndMakeVisible (m_manual);
    }

    void setLogo (const juce::Image& img) { m_logo = img; }
    void setSystemInfo (juce::String version, juce::String format, juce::String os)
    { m_version = std::move (version); m_format = std::move (format); m_os = std::move (os); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.72f));   // scrim

        const auto card = cardBounds();
        g.setColour (PnsTheme::kBgPanel);
        g.fillRoundedRectangle (card.toFloat(), 10.0f);
        g.setColour (PnsTheme::kBorderSubtle);
        g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 10.0f, 1.0f);

        const auto c = card.reduced (28, 22);

        if (m_logo.isValid())
        {
            const float lw = 150.0f, lh = lw * (float) m_logo.getHeight() / (float) m_logo.getWidth();
            g.drawImage (m_logo, (float) c.getCentreX() - lw * 0.5f, (float) c.getY(), lw, lh,
                         0, 0, m_logo.getWidth(), m_logo.getHeight());
        }

        g.setColour (PnsTheme::kTextPrimary);
        g.setFont (juce::Font (juce::FontOptions().withHeight (26.0f)).boldened());
        g.drawText ("PlugNspectr", c.getX(), c.getY() + 66, c.getWidth(), 32, juce::Justification::centred);

        struct Row { const char* k; const juce::String& v; };
        const Row rows[] = { { "Version", m_version }, { "Format", m_format }, { "OS", m_os } };
        int y = c.getY() + 118;
        g.setFont (PnsTheme::fontPrimary());
        for (const auto& r : rows)
        {
            g.setColour (PnsTheme::kTextSecondary);
            g.drawText (r.k, c.getX(), y, c.getWidth() / 2 - 8, 20, juce::Justification::centredRight);
            g.setColour (PnsTheme::kTextPrimary);
            g.drawText (r.v, c.getX() + c.getWidth() / 2 + 8, y, c.getWidth() / 2, 20, juce::Justification::centredLeft);
            y += 26;
        }

        g.setColour (PnsTheme::kTextSecondary);
        g.setFont (PnsTheme::fontLabel());
        g.drawText (juce::CharPointer_UTF8 ("\xc2\xa9 2026 Biltroy Audio. All rights reserved."),
                    c.getX(), card.getBottom() - 34, c.getWidth(), 16, juce::Justification::centred);
    }

    void resized() override
    {
        const auto card = cardBounds();
        m_close.setBounds (card.getRight() - 30, card.getY() + 8, 22, 22);

        constexpr int bw = 132, bh = 30;
        m_manual.setBounds (card.getCentreX() - bw / 2, card.getBottom() - 70, bw, bh);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! cardBounds().contains (e.getPosition())) setVisible (false);   // click outside → close
    }

private:
    juce::Rectangle<int> cardBounds() const
    { return juce::Rectangle<int> (380, 430).withCentre (getLocalBounds().getCentre()); }

    juce::Image      m_logo;
    juce::String     m_version { "1.0.0" }, m_format { "VST3" }, m_os { "Windows" };
    juce::TextButton m_close, m_manual;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutOverlay)
};

//==============================================================================
// LagTracker — how far Post lags Pre: the latency of everything between the two
// plugins, which a host keeps even when those plugins are bypassed. Owned by the
// editor and measured once from the audio, so every view that compares Pre with
// Post (Dynamics, Waveform) lines them up the same way, whichever tab is open.
//==============================================================================
class LagTracker
{
public:
    explicit LagTracker (PlugNspectrPostProcessor& p) : m_proc (p) {}

    // Drain the capture and, every ~0.5 s of audio, re-estimate. Keeps the last
    // confident answer when the audio gives none (silence, a steady tone).
    void update()
    {
        const double sr = m_proc.getSampleRate();
        if (sr <= 0.0) return;
        const int need = pns::analysis::requiredLagSamples (sr);
        if (sr != m_sr)
        {
            m_sr = sr;
            m_pre.assign ((size_t) need, 0.0f);
            m_post.assign ((size_t) need, 0.0f);
            m_filled = m_sinceEstimate = 0;
            m_locked = false; m_heardUnlocked = 0;
        }

        const int n = m_proc.readCaptureSince (m_capReadPos, m_capPre, m_capPost);
        if (n <= 0) return;

        // Newest-last history: slide left, append the newest k samples.
        const int k = juce::jmin (n, need);
        auto append = [&] (std::vector<float>& hist, const juce::AudioBuffer<float>& cap)
        {
            std::copy (hist.begin() + k, hist.end(), hist.begin());
            const float* src = cap.getReadPointer (0) + (n - k);
            std::copy (src, src + k, hist.end() - k);
        };
        append (m_pre,  m_capPre);
        append (m_post, m_capPost);
        m_filled = juce::jmin (need, m_filled + n);

        m_sinceEstimate += n;
        if (! m_locked) m_heardUnlocked += n;
        if (m_filled >= need && m_sinceEstimate >= (int) (sr * 0.5))
        {
            m_sinceEstimate = 0;
            const int lag = pns::analysis::estimateLag (m_pre.data(), m_post.data(), need, sr);
            if (lag >= 0) { m_lag = lag; m_locked = true; }
        }
    }

    int lagSamples() const { return m_lag; }

    // True once a confident estimate exists, or after 4 s of audio without
    // one: steady material (a held tone, a drone) gives no answer, and for it
    // alignment doesn't matter.
    bool settled() const { return m_locked || m_heardUnlocked >= (juce::int64) (4.0 * m_sr); }

private:
    PlugNspectrPostProcessor& m_proc;
    uint64_t                  m_capReadPos = 0;
    juce::AudioBuffer<float>  m_capPre, m_capPost;
    std::vector<float>        m_pre, m_post;
    double m_sr = 0.0;
    int    m_filled = 0, m_sinceEstimate = 0, m_lag = 0;
    bool   m_locked = false;
    juce::int64 m_heardUnlocked = 0;
};

//==============================================================================
// DockTab — one tab in the bottom dock: a line icon over a short label, on a
// rounded highlight when selected. Icons are drawn on a 24-unit grid so they
// scale with the button.
//==============================================================================
class DockTab : public juce::Button
{
public:
    enum Icon { Tone, Dynamics, Waveform, Stereo, Harmonics, EqCurve, Compression, AttackRelease, Distortion };

    DockTab (const juce::String& label, Icon icon) : juce::Button (label), m_icon (icon) {}

    void setActive (bool a) { if (m_active != a) { m_active = a; repaint(); } }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto b = getLocalBounds().toFloat().reduced (2.0f, 4.0f);
        if (m_active || over || down)
        {
            g.setColour (m_active ? PnsTheme::kBgWidget : PnsTheme::kBgWidget.withAlpha (0.5f));
            g.fillRoundedRectangle (b, 10.0f);
        }
        const auto col = m_active ? PnsTheme::kTextPrimary
                       : over     ? PnsTheme::kTextPrimary.withAlpha (0.85f)
                                  : PnsTheme::kTextSecondary;

        constexpr float iconSz = 22.0f;
        const auto ib = juce::Rectangle<float> (iconSz, iconSz)
                            .withCentre ({ b.getCentreX(), b.getY() + 8.0f + iconSz * 0.5f });
        g.setColour (m_active ? PnsTheme::kAccentPrimary : col);
        g.strokePath (iconPath().createPathWithRoundedCorners (1.0f),
                      juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                      juce::AffineTransform::scale (iconSz / 24.0f).translated (ib.getX(), ib.getY()));

        g.setColour (col);
        g.setFont (PnsTheme::fontLabel());
        g.drawText (getButtonText(), b.withTrimmedTop (iconSz + 12.0f).withHeight (16.0f),
                    juce::Justification::centred, false);
    }

private:
    juce::Path iconPath() const
    {
        juce::Path p;
        auto poly = [&p] (std::initializer_list<float> xy)
        {
            auto it = xy.begin();
            p.startNewSubPath (it[0], it[1]);
            for (size_t i = 2; i + 1 < xy.size(); i += 2) p.lineTo (it[i], it[i + 1]);
        };
        switch (m_icon)
        {
            case Tone:          poly ({ 3,17, 7,11, 11,14, 15,6, 19,11, 21,9 }); break;
            case Dynamics:      poly ({ 3,12, 6,12, 8,6, 11,18, 14,9, 16,12, 21,12 }); break;
            case Waveform:      p.startNewSubPath (3, 12);
                                p.cubicTo (5, 4, 8, 4, 10, 12);
                                p.cubicTo (12, 20, 15, 20, 17, 12);
                                p.cubicTo (18, 8, 20, 8, 21, 10); break;
            case Stereo:        p.addEllipse (4, 7, 10, 10); p.addEllipse (10, 7, 10, 10); break;
            case Harmonics:     poly ({ 5,20, 5,6 }); poly ({ 10,20, 10,12 }); poly ({ 15,20, 15,9 }); poly ({ 20,20, 20,15 }); break;
            case EqCurve:       p.startNewSubPath (3, 15);
                                p.cubicTo (8, 15, 8, 8, 12, 8);
                                p.cubicTo (16, 8, 16, 15, 21, 15); break;
            case Compression:   poly ({ 4,20, 12,12, 20,9 }); poly ({ 4,20, 20,4 }); break;
            case AttackRelease: poly ({ 3,18, 7,18, 9,6, 15,6, 19,18, 21,18 }); break;
            case Distortion:    poly ({ 3,17, 21,17 }); p.startNewSubPath (3, 17); p.quadraticTo (15, 16, 21, 5); break;
        }
        return p;
    }

    Icon m_icon;
    bool m_active = false;
};

//==============================================================================
// Main editor — headline block on top, the active view in the middle, and the
// tab dock (LISTEN / TEST) with the tone controls along the bottom.
//==============================================================================
class PlugNspectrPostEditor : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    // Tab order, as laid out left-to-right in the tab bar. Live tabs first, then
    // the test-signal tabs on the amber tray (see kFirstMeasTab). Named so the
    // gating/visibility/layout logic never hardcodes an index — inserting a tab
    // used to mean chasing the same integers through eight unrelated places.
    enum Tab
    {
        TabSpectrum = 0, TabDynamics, TabOscilloscope, TabStereo,
        TabHarmonics, TabLinear, TabTransfer, TabEnvelope, TabThd,
        TabCount
    };
    static constexpr int kFirstMeasTab = TabHarmonics;   // first tab in the dock's TEST group

    explicit PlugNspectrPostEditor (PlugNspectrPostProcessor& p);
    ~PlugNspectrPostEditor() override;

    void paint   (juce::Graphics& g) override;
    void resized ()                   override;

    // Test seams for the offline render harness.
    void selectTabForTest     (int index) { switchTab (index); }
    void freezeLinearForTest()            { m_linearView.freezeForTest(); }
    void armMeasureForTest()              { m_linearView.setMeasureForTest (true); writeCmdBlock(); repaint(); }
    bool isStimulusActiveForTest() const  { return isStimulusActive(); }
    void setLinearCursorForTest (int x)   { m_linearView.setCursorForTest (x);   m_linearView.repaint(); }
    void setTransferCursorForTest (int x) { m_transferView.setCursorForTest (x); m_transferView.repaint(); }
    void setEnvelopeCursorForTest (int x) { m_envelopeView.setCursorForTest (x); m_envelopeView.repaint(); }
    void setThdCursorForTest (int x)      { m_thdView.setCursorForTest (x);      m_thdView.repaint(); }
    void freezeTransferForTest()          { m_transferView.freezeForTest(); }
    void freezeThdForTest()               { m_thdView.freezeForTest(); }
    void showAboutForTest()               { m_about.setVisible (true); m_about.toFront (true); }
    void openDynamicsOptionsForTest()     { m_dynView.openOptionsForTest(); }
    // The views that react to the test tone (Harmonics draws only while it's on),
    // without sending a command to a Pre that isn't there.
    void setToneActiveForTest (bool on)   { m_toneActive = on; m_toneBtn.setToggleState (on, juce::dontSendNotification);
                                            m_harmView.setToneActive (on); repaint(); }

private:
    void timerCallback      () override;
    void paintOverChildren  (juce::Graphics& g) override;
    void switchTab          (int index);

    PlugNspectrPostProcessor& audioProcessor;

    // Dock tabs, indexed by Tab. LISTEN tabs first, then TEST (kFirstMeasTab..).
    juce::OwnedArray<DockTab> m_tabs;
    int              m_dockDividerX = 0;        // between the LISTEN and TEST groups
    int              m_logoX        = 0;        // header logo, left of the L+R / Side selector
    juce::Rectangle<int> m_freqText, m_levelText;   // tone readouts beside their knobs
    int              m_activeTab   = 0;
    int              m_tickCounter = 0;

    // "No Pre" overlay animation state
    float m_overlayAlpha  = 0.0f;   // current rendered alpha (0=hidden, 1=fully visible)
    float m_overlayTarget = 0.0f;   // desired alpha (animated toward each tick)
    float m_pulsePhase    = 0.0f;   // 0–2π, for the pulsing search dot

    // ── Dock — test tone controls ────────────────────────────────
    bool          m_toneActive = false;
    double        m_toneFreq   = 1000.0;
    double        m_toneLevel  = -6.0;   // test-tone level, dBFS

    juce::Slider     m_toneFreqSlider;
    juce::Slider     m_toneLevelSlider;
    juce::TextButton m_toneBtn { "Test Tone" };

    // Header channel selector — L+R (combined) / Side (L-R)/2, all analysis.
    juce::TextButton m_chLR { "L+R" }, m_chSide { "Side" };

    // True while any stimulus (test tone, noise, ramp, step, sweep) is being
    // emitted — drives the amber "measuring" border + banner.
    bool isStimulusActive() const;

    // Turn off every active stimulus (tone + all measure toggles). Returns true
    // if anything was actually disarmed. Used by the transport-stop auto-defeat.
    bool disarmAllStimuli();
    bool m_wasTransportPlaying = true;   // edge-detect playing -> stopped

    void writeCmdBlock  ();

    // Plain-language layer: each tab's question (banner headline + tooltip) and
    // the active tab's one-line verdict, refreshed with the views.
    static const char* tabQuestion (int tab);
    PnsTheme::Summary  currentSummary() const;
    PnsTheme::Summary  m_summary;
    static bool toneFreqOn  (int tab);
    static bool toneLevelOn (int tab);

    PnsLookAndFeel    m_laf;
    juce::Image       m_pnsLogo;
    LagTracker        m_lagTracker;   // Post-vs-Pre latency, shared by Dynamics + Waveform
    SpectrumView      m_specView;
    DynamicsView      m_dynView;
    OscilloscopeView  m_oscView;
    StereoView        m_stereoView;
    HarmonicsView     m_harmView;
    LinearView        m_linearView;
    TransferView      m_transferView;
    EnvelopeView      m_envelopeView;
    ThdSweepView      m_thdView;

    InfoButton        m_infoBtn;     // top-right "i" — opens the About modal
    AboutOverlay      m_about;       // dark scrim + card

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlugNspectrPostEditor)
};
