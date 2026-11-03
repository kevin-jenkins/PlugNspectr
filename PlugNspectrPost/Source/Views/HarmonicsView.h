#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"
#include "../PluginProcessor.h"
#include "../PnsAnalysis.h"
#include "ViewOptions.h"

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
