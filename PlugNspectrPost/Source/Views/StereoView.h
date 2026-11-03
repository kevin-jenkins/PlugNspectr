#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"
#include "../PluginProcessor.h"
#include "../PnsAnalysis.h"
#include "ViewOptions.h"

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
