#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"
#include "../PluginProcessor.h"
#include "../PnsAnalysis.h"
#include "ViewOptions.h"

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
