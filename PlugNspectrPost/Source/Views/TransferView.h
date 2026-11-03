#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"
#include "../PluginProcessor.h"
#include "../PnsAnalysis.h"
#include "ViewOptions.h"
#include "ViewCommon.h"

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
    void setCursorForTest (int x) { m_cursor.lockAt (x); }
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
    HoverCursor m_cursor;

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
