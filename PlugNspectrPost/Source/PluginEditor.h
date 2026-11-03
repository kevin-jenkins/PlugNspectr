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
#include "PnsAnalysis.h"
#include "Views/ViewOptions.h"
#include "Views/SpectrumView.h"
#include "Views/DynamicsView.h"
#include "Views/OscilloscopeView.h"
#include "Views/StereoView.h"
#include "Views/HarmonicsView.h"
#include "Views/LinearView.h"
#include "Views/TransferView.h"
#include "Views/EnvelopeView.h"
#include "Views/ThdSweepView.h"

// (logo is embedded in PnsLogoData.h — no Projucer BinaryData dependency)

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
            g.drawImage (m_logo, { (float) c.getCentreX() - lw * 0.5f, (float) c.getY(), lw, lh });
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
// LagTracker — how far Post lags Pre: the latency of everything between the two
// plugins, which a host keeps even when those plugins are bypassed. Owned by the
// editor and measured once from the audio, so every view that compares Pre with
// Post (Dynamics, Waveform) lines them up the same way, whichever tab is open.
//==============================================================================
class LagTracker
{
public:
    explicit LagTracker (PlugNspectrPostProcessor& p) : m_proc (p) {}

    // Drain the capture and, every ~0.5 s of audio, re-estimate. Keeps the last
    // confident answer when the audio gives none (silence, a steady tone).
    void update (const CaptureChunk& cap)
    {
        const double sr = m_proc.getSampleRate();
        if (sr <= 0.0) return;
        const int need = pns::analysis::requiredLagSamples (sr);
        if (sr != m_sr)
        {
            m_sr = sr;
            m_ringPre.assign ((size_t) need, 0.0f);
            m_ringPost.assign ((size_t) need, 0.0f);
            m_pre.assign ((size_t) need, 0.0f);
            m_post.assign ((size_t) need, 0.0f);
            m_write = m_filled = m_sinceEstimate = 0;
            m_locked = false; m_heardUnlocked = 0;
        }

        const int n = cap.n;
        if (n <= 0) return;

        // Ring history; straightened into m_pre / m_post only when an estimate
        // is due (twice a second) instead of sliding ~84k samples every tick.
        const int k = juce::jmin (n, need);
        const float* srcPre  = cap.pre .getReadPointer (0) + (n - k);
        const float* srcPost = cap.post.getReadPointer (0) + (n - k);
        for (int i = 0; i < k; ++i)
        {
            m_ringPre [(size_t) m_write] = srcPre[i];
            m_ringPost[(size_t) m_write] = srcPost[i];
            if (++m_write == need) m_write = 0;
        }
        m_filled = juce::jmin (need, m_filled + n);

        m_sinceEstimate += n;
        if (! m_locked) m_heardUnlocked += n;
        if (m_filled >= need && m_sinceEstimate >= (int) (sr * 0.5))
        {
            m_sinceEstimate = 0;
            // Oldest sample first: the ring from the write position onwards.
            std::copy (m_ringPre.begin() + m_write,  m_ringPre.end(),  m_pre.begin());
            std::copy (m_ringPre.begin(),            m_ringPre.begin() + m_write,  m_pre.end()  - m_write);
            std::copy (m_ringPost.begin() + m_write, m_ringPost.end(), m_post.begin());
            std::copy (m_ringPost.begin(),           m_ringPost.begin() + m_write, m_post.end() - m_write);
            const int lag = pns::analysis::estimateLag (m_pre.data(), m_post.data(), need, sr);
            if (lag >= 0) { m_lag = lag; m_locked = true; }
        }
    }

    int  lagSamples() const { return m_lag; }
    bool locked()     const { return m_locked; }

    // True once a confident estimate exists, or after 4 s of audio without
    // one: steady material (a held tone, a drone) gives no answer, and for it
    // alignment doesn't matter.
    bool settled() const { return m_locked || m_heardUnlocked >= (juce::int64) (4.0 * m_sr); }

private:
    PlugNspectrPostProcessor& m_proc;
    std::vector<float>        m_ringPre, m_ringPost;   // history ring
    std::vector<float>        m_pre, m_post;           // ring straightened for an estimate
    int                       m_write = 0;
    double m_sr = 0.0;
    int    m_filled = 0, m_sinceEstimate = 0, m_lag = 0;
    bool   m_locked = false;
    juce::int64 m_heardUnlocked = 0;
};

//==============================================================================
// DockTab — one tab in the bottom dock: a line icon over a short label, on a
// rounded highlight when selected. Icons are drawn on a 24-unit grid so they
// scale with the button.
//==============================================================================
class DockTab : public juce::Button
{
public:
    enum Icon { Tone, Dynamics, Waveform, Stereo, Harmonics, EqCurve, Compression, AttackRelease, Distortion };

    DockTab (const juce::String& label, Icon icon) : juce::Button (label), m_icon (icon) {}

    void setActive (bool a) { if (m_active != a) { m_active = a; repaint(); } }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto b = getLocalBounds().toFloat().reduced (2.0f, 4.0f);
        if (m_active || over || down)
        {
            g.setColour (m_active ? PnsTheme::kBgWidget : PnsTheme::kBgWidget.withAlpha (0.5f));
            g.fillRoundedRectangle (b, 10.0f);
        }
        const auto col = m_active ? PnsTheme::kTextPrimary
                       : over     ? PnsTheme::kTextPrimary.withAlpha (0.85f)
                                  : PnsTheme::kTextSecondary;

        constexpr float iconSz = 22.0f;
        const auto ib = juce::Rectangle<float> (iconSz, iconSz)
                            .withCentre ({ b.getCentreX(), b.getY() + 8.0f + iconSz * 0.5f });
        g.setColour (m_active ? PnsTheme::kAccentPrimary : col);
        g.strokePath (iconPath().createPathWithRoundedCorners (1.0f),
                      juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                      juce::AffineTransform::scale (iconSz / 24.0f).translated (ib.getX(), ib.getY()));

        g.setColour (col);
        g.setFont (PnsTheme::fontLabel());
        g.drawText (getButtonText(), b.withTrimmedTop (iconSz + 12.0f).withHeight (16.0f),
                    juce::Justification::centred, false);
    }

private:
    juce::Path iconPath() const
    {
        juce::Path p;
        auto poly = [&p] (std::initializer_list<float> xy)
        {
            auto it = xy.begin();
            p.startNewSubPath (it[0], it[1]);
            for (size_t i = 2; i + 1 < xy.size(); i += 2) p.lineTo (it[i], it[i + 1]);
        };
        switch (m_icon)
        {
            case Tone:          poly ({ 3,17, 7,11, 11,14, 15,6, 19,11, 21,9 }); break;
            case Dynamics:      poly ({ 3,12, 6,12, 8,6, 11,18, 14,9, 16,12, 21,12 }); break;
            case Waveform:      p.startNewSubPath (3, 12);
                                p.cubicTo (5, 4, 8, 4, 10, 12);
                                p.cubicTo (12, 20, 15, 20, 17, 12);
                                p.cubicTo (18, 8, 20, 8, 21, 10); break;
            case Stereo:        p.addEllipse (4, 7, 10, 10); p.addEllipse (10, 7, 10, 10); break;
            case Harmonics:     poly ({ 5,20, 5,6 }); poly ({ 10,20, 10,12 }); poly ({ 15,20, 15,9 }); poly ({ 20,20, 20,15 }); break;
            case EqCurve:       p.startNewSubPath (3, 15);
                                p.cubicTo (8, 15, 8, 8, 12, 8);
                                p.cubicTo (16, 8, 16, 15, 21, 15); break;
            case Compression:   poly ({ 4,20, 12,12, 20,9 }); poly ({ 4,20, 20,4 }); break;
            case AttackRelease: poly ({ 3,18, 7,18, 9,6, 15,6, 19,18, 21,18 }); break;
            case Distortion:    poly ({ 3,17, 21,17 }); p.startNewSubPath (3, 17); p.quadraticTo (15, 16, 21, 5); break;
        }
        return p;
    }

    Icon m_icon;
    bool m_active = false;
};

//==============================================================================
// Main editor — headline block on top, the active view in the middle, and the
// tab dock (LISTEN / TEST) with the tone controls along the bottom.
//==============================================================================
class PlugNspectrPostEditor : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    // Tab order, as laid out left-to-right in the tab bar. Live tabs first, then
    // the test-signal tabs on the amber tray (see kFirstMeasTab). Named so the
    // gating/visibility/layout logic never hardcodes an index — inserting a tab
    // used to mean chasing the same integers through eight unrelated places.
    enum Tab
    {
        TabSpectrum = 0, TabDynamics, TabOscilloscope, TabStereo,
        TabHarmonics, TabLinear, TabTransfer, TabEnvelope, TabThd,
        TabCount
    };
    static constexpr int kFirstMeasTab = TabHarmonics;   // first tab in the dock's TEST group

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
    void openDynamicsOptionsForTest()     { m_dynView.openOptionsForTest(); }
    // The views that react to the test tone (Harmonics draws only while it's on),
    // without sending a command to a Pre that isn't there.
    void setToneActiveForTest (bool on)   { m_toneActive = on; m_toneBtn.setToggleState (on, juce::dontSendNotification);
                                            m_harmView.setToneActive (on); repaint(); }

private:
    void timerCallback      () override;
    void paintOverChildren  (juce::Graphics& g) override;
    void switchTab          (int index);

    PlugNspectrPostProcessor& audioProcessor;

    // Dock tabs, indexed by Tab. LISTEN tabs first, then TEST (kFirstMeasTab..).
    juce::OwnedArray<DockTab> m_tabs;
    int              m_dockDividerX = 0;        // between the LISTEN and TEST groups
    int              m_logoX        = 0;        // header logo, left of the L+R / Side selector
    juce::Rectangle<int> m_freqText, m_levelText;   // tone readouts beside their knobs
    int              m_activeTab   = 0;
    int              m_tickCounter = 0;

    // "No Pre" overlay animation state
    float m_overlayAlpha  = 0.0f;   // current rendered alpha (0=hidden, 1=fully visible)
    float m_overlayTarget = 0.0f;   // desired alpha (animated toward each tick)
    juce::String m_setupWarning;    // a setup problem to fix (amber strip), or empty
    juce::String m_delayNote;       // how Pre and Post are lined up, for the tabs that use it
    juce::String delayNote() const;
    float m_pulsePhase    = 0.0f;   // 0–2π, for the pulsing search dot

    // ── Dock — test tone controls ────────────────────────────────
    bool          m_toneActive = false;
    double        m_toneFreq   = 1000.0;
    double        m_toneLevel  = -6.0;   // test-tone level, dBFS

    juce::Slider     m_toneFreqSlider;
    juce::Slider     m_toneLevelSlider;
    juce::TextButton m_toneBtn { "Test Tone" };

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

    // Plain-language layer: each tab's question (banner headline + tooltip) and
    // the active tab's one-line verdict, refreshed with the views.
    static const char* tabQuestion (int tab);
    PnsTheme::Summary  currentSummary() const;
    PnsTheme::Summary  m_summary;
    static bool toneFreqOn  (int tab);
    static bool toneLevelOn (int tab);

    PnsLookAndFeel    m_laf;
    juce::Image       m_pnsLogo;
    uint64_t          m_capReadPos = 0;
    CaptureChunk      m_cap;          // this tick's capture, shared by every raw-audio reader
    LagTracker        m_lagTracker;   // Post-vs-Pre latency, shared by Dynamics + Waveform
    SpectrumView      m_specView;
    DynamicsView      m_dynView;
    OscilloscopeView  m_oscView;
    StereoView        m_stereoView;
    HarmonicsView     m_harmView;
    LinearView        m_linearView;
    TransferView      m_transferView;
    EnvelopeView      m_envelopeView;
    ThdSweepView      m_thdView;

    InfoButton        m_infoBtn;     // top-right "i" — opens the About modal
    AboutOverlay      m_about;       // dark scrim + card

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlugNspectrPostEditor)
};
