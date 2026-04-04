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
#include "../JuceLibraryCode/BinaryData.h"

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

    // Zoom: visible window in seconds (6 / 12)
    int m_zoomSeconds = 12;

    juce::TextButton m_zoom6s  { "6s"  };
    juce::TextButton m_zoom12s { "12s" };

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
// Main editor — dark tab bar + four views
//==============================================================================
class PlugNspectrPostEditor : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit PlugNspectrPostEditor (PlugNspectrPostProcessor& p);
    ~PlugNspectrPostEditor() override;

    void paint   (juce::Graphics& g) override;
    void resized ()                   override;

private:
    void timerCallback      () override;
    void paintOverChildren  (juce::Graphics& g) override;
    void switchTab          (int index);

    PlugNspectrPostProcessor& audioProcessor;

    juce::TextButton m_tabSpectrum     { "Spectrum"     };
    juce::TextButton m_tabDynamics     { "Dynamics"     };
    juce::TextButton m_tabOscilloscope { "Oscilloscope" };
    juce::TextButton m_tabHarmonics    { "Harmonics"    };
    int              m_activeTab   = 0;
    int              m_tickCounter = 0;

    // "No Pre" overlay animation state
    float m_overlayAlpha  = 0.0f;   // current rendered alpha (0=hidden, 1=fully visible)
    float m_overlayTarget = 0.0f;   // desired alpha (animated toward each tick)
    float m_pulsePhase    = 0.0f;   // 0–2π, for the pulsing search dot

    // ── Global footer — test tone controls ────────────────────────────────
    HANDLE        m_hCmdFile   = nullptr;
    PNS_CmdBlock* m_pCmd       = nullptr;
    bool          m_toneActive = false;
    double        m_toneFreq   = 1000.0;

    juce::Slider     m_footerFreqSlider;
    juce::TextButton m_footerToneBtn { "Test Tone" };

    void openCmdMemory  ();
    void closeCmdMemory ();
    void writeCmdBlock  ();

    PnsLookAndFeel    m_laf;
    juce::Image       m_biltroyLogo;
    SpectrumView      m_specView;
    DynamicsView      m_dynView;
    OscilloscopeView  m_oscView;
    HarmonicsView     m_harmView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlugNspectrPostEditor)
};
