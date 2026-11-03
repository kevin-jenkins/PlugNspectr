#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"
#include "../PluginProcessor.h"
#include "../PnsAnalysis.h"
#include "ViewOptions.h"

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
