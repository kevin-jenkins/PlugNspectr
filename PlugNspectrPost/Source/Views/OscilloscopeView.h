#pragma once

#include <JuceHeader.h>
#include "../PlugNspectrTheme.h"
#include "../PluginProcessor.h"
#include "../PnsAnalysis.h"
#include "ViewOptions.h"

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
    void setLagSamples (int lag) { m_lag = juce::jlimit (0, kRingLen - kMaxCapture - 1, lag); }
    void setToneActive (bool on) { m_toneOn = on; }   // steers the headline's advice
    PnsTheme::Summary summary() const;   // one-line plain-language verdict for the banner

private:
    // Ring buffer: room for the longest capture plus LagTracker's maximum
    // 250 ms of latency even at 192 kHz (48k + 20k samples), with margin.
    static constexpr int kRingLen    = 96000;
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
