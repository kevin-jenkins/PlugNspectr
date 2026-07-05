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
// (logo is embedded in PnsLogoData.h — no Projucer BinaryData dependency)

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

    // Controls
    juce::ComboBox   m_smoothBox;
    juce::TextButton m_avgBtn { "Avg" };

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
class DynamicsView : public juce::Component
{
public:
    explicit DynamicsView (PlugNspectrPostProcessor& p);
    void paint   (juce::Graphics& g) override;
    void resized ()                   override;
    void update  ();
    void reset   ();   // clear all accumulated readings and history (keeps the zoom)

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

    juce::TextButton m_zoom6s  { "6s"  };
    juce::TextButton m_zoom12s { "12s" };
    juce::TextButton m_reset   { "Reset" };

    // GR readout — instantaneous + 30s rolling average + peak hold
    float  m_instantGr        = 0.0f;
    float  m_avgGr            = 0.0f;   // calculated rolling average
    float  m_grPeakHold       = 0.0f;  // most-negative GR seen, decays slowly
    double m_grFlashEnd       = 0.0;    // ms timestamp; 0 = not flashing
    bool   m_mouseOverReadout = false;

    // 30-second GR average buffer — 900 slots at 30fps
    static constexpr int kAvgGrLen = 900;
    std::array<float, kAvgGrLen> m_avgGrBuf {};
    int                          m_avgGrPos  = 0;
    int                          m_avgGrFill = 0;   // number of valid entries

    // Volume (RMS) display — smoothed with 0.9 decay
    float m_smoothPreDb  = -90.0f;
    float m_smoothPostDb = -90.0f;

    //──────────────────────────────────────────────────────────────────────────
    // GR scrolling history (circular buffer, 3 s @ 30 fps)
    //──────────────────────────────────────────────────────────────────────────
    static constexpr int kGrLen = 90;

    std::array<float, kGrLen> m_gr {};
    int                       m_grPos = 0;

    //──────────────────────────────────────────────────────────────────────────
    PlugNspectrPostProcessor& m_proc;
    uint32_t                  m_lastCaptureCount = 0;
    bool                      m_preConnected     = false;
    int                       m_waveTickCounter  = 0;  // gates waveform ingestion to 30fps

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
    uint32_t m_lastCaptureCount = 0;

    // Display buffer — written by update(), read by paint()
    std::array<float, kMaxCapture> m_displayPre  {};
    std::array<float, kMaxCapture> m_displayPost {};
    int  m_displayCount = 0;
    bool m_hasDisplay   = false;

    // Time window control
    int m_windowMs = 50;

    juce::TextButton m_btn10ms  { "10ms"  };
    juce::TextButton m_btn50ms  { "50ms"  };
    juce::TextButton m_btn100ms { "100ms" };

    float ringGet (const std::array<float, kRingLen>& buf, uint64_t idx) const;

    PlugNspectrPostProcessor& m_proc;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscilloscopeView)
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

    // Called by the editor footer when the user changes the tone controls
    void setToneFreq   (double freq);
    void setToneActive (bool active);

private:
    // ── Test-tone state (set by editor footer) ─────────────────────────────
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
// Main editor — dark tab bar + eight views
//==============================================================================
class PlugNspectrPostEditor : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
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

private:
    void timerCallback      () override;
    void paintOverChildren  (juce::Graphics& g) override;
    void switchTab          (int index);

    PlugNspectrPostProcessor& audioProcessor;

    juce::TextButton m_tabSpectrum     { "Spectrum"        };
    juce::TextButton m_tabDynamics     { "Dynamics"        };
    juce::TextButton m_tabOscilloscope { "Oscilloscope"    };
    juce::TextButton m_tabHarmonics    { "Harmonics"       };
    juce::TextButton m_tabLinear       { "Freq Response"   };
    juce::TextButton m_tabTransfer     { "Compression"     };
    juce::TextButton m_tabEnvelope     { "Attack / Release"};
    juce::TextButton m_tabThd          { "Distortion"      };

    // Test-signal tabs (indices 3..7) sit on a recessed amber tray; the live
    // tabs (0..2) sit plain to the left. Tray bounds computed in resized().
    juce::Rectangle<int> m_trayBounds;
    int              m_activeTab   = 0;
    int              m_tickCounter = 0;

    // "No Pre" overlay animation state
    float m_overlayAlpha  = 0.0f;   // current rendered alpha (0=hidden, 1=fully visible)
    float m_overlayTarget = 0.0f;   // desired alpha (animated toward each tick)
    float m_pulsePhase    = 0.0f;   // 0–2π, for the pulsing search dot

    // ── Global footer — test tone controls ────────────────────────────────
    bool          m_toneActive = false;
    double        m_toneFreq   = 1000.0;
    double        m_toneLevel  = -6.0;   // test-tone level, dBFS

    juce::Slider     m_footerFreqSlider;
    juce::Slider     m_footerLevelSlider;
    juce::TextButton m_footerToneBtn { "Test Tone" };

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

    PnsLookAndFeel    m_laf;
    juce::Image       m_pnsLogo;
    SpectrumView      m_specView;
    DynamicsView      m_dynView;
    OscilloscopeView  m_oscView;
    HarmonicsView     m_harmView;
    LinearView        m_linearView;
    TransferView      m_transferView;
    EnvelopeView      m_envelopeView;
    ThdSweepView      m_thdView;

    InfoButton        m_infoBtn;     // top-right "i" — opens the About modal
    AboutOverlay      m_about;       // dark scrim + card

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlugNspectrPostEditor)
};
