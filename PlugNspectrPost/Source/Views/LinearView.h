#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"
#include "../PluginProcessor.h"
#include "../PnsAnalysis.h"
#include "ViewOptions.h"
#include "ViewCommon.h"

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
    void setCursorForTest (int x) { m_cursor.lockAt (x); }

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int kBins = PlugNspectrPostProcessor::kMeasBins;
    HoverCursor m_cursor;

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
