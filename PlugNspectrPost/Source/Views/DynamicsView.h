#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"
#include "../PluginProcessor.h"
#include "../PnsAnalysis.h"
#include "ViewOptions.h"
#include "ViewCommon.h"

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
    void update  (const CaptureChunk& cap);
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
    bool                      m_preConnected     = false;

    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove        (const juce::MouseEvent&) override;
    void mouseExit        (const juce::MouseEvent&) override;

    void drawWaveform (juce::Graphics&, juce::Rectangle<float>);
    void drawGrMeter  (juce::Graphics&, juce::Rectangle<float>);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicsView)
};
