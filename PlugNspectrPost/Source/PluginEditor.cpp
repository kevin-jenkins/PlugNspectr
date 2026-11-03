/*
  ==============================================================================
    PlugNspectrPost  –  PluginEditor.cpp
  ==============================================================================
*/

#include "PluginEditor.h"
#include "PnsLogoData.h"
#include <cmath>
#include <algorithm>
#include <set>

//==============================================================================
PlugNspectrPostEditor::PlugNspectrPostEditor (PlugNspectrPostProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      m_lagTracker (p),
      m_specView (p),
      m_dynView  (p),
      m_oscView  (p),
      m_stereoView (p),
      m_harmView (p),
      m_linearView (p),
      m_transferView (p),
      m_envelopeView (p),
      m_thdView (p)
{
    setLookAndFeel (&m_laf);

    m_pnsLogo = juce::ImageCache::getFromMemory (kPnsLogoPng, kPnsLogoPngSize);

    {
        struct Def { const char* label; DockTab::Icon icon; };
        const Def defs[TabCount] = {
            { "Tone",       DockTab::Tone        }, { "Dynamics",   DockTab::Dynamics      },
            { "Waveform",   DockTab::Waveform    }, { "Stereo",     DockTab::Stereo        },
            { "Harmonics",  DockTab::Harmonics   }, { "EQ curve",   DockTab::EqCurve       },
            { "Compression",DockTab::Compression }, { "Attack/Rel", DockTab::AttackRelease },
            { "Distortion", DockTab::Distortion  } };
        for (int i = 0; i < TabCount; ++i)
        {
            auto* t = m_tabs.add (new DockTab (defs[i].label, defs[i].icon));
            t->setTooltip (tabQuestion (i));
            t->onClick = [this, i] { switchTab (i); };
            addAndMakeVisible (t);
        }
    }

    addAndMakeVisible (m_specView);
    addAndMakeVisible (m_dynView);
    addAndMakeVisible (m_oscView);
    addAndMakeVisible (m_stereoView);
    addAndMakeVisible (m_harmView);
    addAndMakeVisible (m_linearView);
    addAndMakeVisible (m_transferView);
    addAndMakeVisible (m_envelopeView);
    addAndMakeVisible (m_thdView);

    // Measure toggles re-publish the cmd block (stimulus on/off).
    m_linearView.onMeasureChanged   = [this] { writeCmdBlock(); repaint(); };
    m_transferView.onMeasureChanged = [this] { writeCmdBlock(); repaint(); };
    m_envelopeView.onMeasureChanged = [this] { writeCmdBlock(); repaint(); };
    m_thdView.onMeasureChanged      = [this] { writeCmdBlock(); repaint(); };

    // ── Dock — test-tone frequency knob ────────────────────────────────────
    m_toneFreqSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    m_toneFreqSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    m_toneFreqSlider.setRange (100.0, 8000.0, 1.0);
    m_toneFreqSlider.setSkewFactorFromMidPoint (1000.0);
    m_toneFreqSlider.setValue (1000.0, juce::dontSendNotification);
    m_toneFreqSlider.onValueChange = [this]
    {
        m_toneFreq = m_toneFreqSlider.getValue();
        writeCmdBlock();
        m_harmView.setToneFreq (m_toneFreq);
        repaint();   // update the tone readout
    };
    addAndMakeVisible (m_toneFreqSlider);

    // ── Dock — test-tone level ───────────────────────────────────
    m_toneLevelSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    m_toneLevelSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    m_toneLevelSlider.setRange (-48.0, 0.0, 0.5);
    m_toneLevelSlider.setValue (m_toneLevel, juce::dontSendNotification);
    m_toneLevelSlider.onValueChange = [this]
    {
        m_toneLevel = m_toneLevelSlider.getValue();
        writeCmdBlock();
        repaint();
    };
    addAndMakeVisible (m_toneLevelSlider);

    // ── Dock — test-tone button ──────────────────────────────────
    m_toneBtn.setToggleable (true);
    m_toneBtn.getProperties().set ("armed", true);
    m_toneBtn.setToggleState (false, juce::dontSendNotification);
    m_toneBtn.onClick = [this]
    {
        m_toneActive = !m_toneActive;
        m_toneBtn.setToggleState (m_toneActive, juce::dontSendNotification);
        writeCmdBlock();
        m_harmView.setToneActive (m_toneActive);
        repaint();   // show/hide warning label
    };
    addAndMakeVisible (m_toneBtn);

    // ── Header — L+R / Side segmented channel selector (all analysis) ──────
    {
        juce::TextButton* segs[2] = { &m_chLR, &m_chSide };
        const char* tip[2] = { "L+R \xe2\x80\x94 both channels combined, (L+R)/2",
                               "Side \xe2\x80\x94 stereo difference, (L-R)/2" };
        for (int i = 0; i < 2; ++i)
        {
            segs[i]->setClickingTogglesState (true);
            segs[i]->setRadioGroupId (7001);
            segs[i]->setTooltip (tip[i]);
            segs[i]->setColour (juce::TextButton::buttonOnColourId,  PnsTheme::kAccentPrimary.withAlpha (0.22f));
            segs[i]->setColour (juce::TextButton::textColourOnId,    PnsTheme::kAccentPrimary);
            segs[i]->setColour (juce::TextButton::textColourOffId,   PnsTheme::kTextSecondary);
            segs[i]->onClick = [this, i] { audioProcessor.setChannelMode (i); };
            addAndMakeVisible (segs[i]);
        }
        segs[juce::jlimit (0, 1, audioProcessor.getChannelMode())]->setToggleState (true, juce::dontSendNotification);
    }

    // ── About modal + the info icon that opens it ──────────────────────────
    m_infoBtn.onClick = [this] { m_about.setVisible (true); m_about.toFront (true); };
    addAndMakeVisible (m_infoBtn);

    m_about.setLogo (m_pnsLogo);
#ifdef JucePlugin_VersionString
    m_about.setSystemInfo (JucePlugin_VersionString,   // from the .jucer / CMake project version
#else
    m_about.setSystemInfo ("dev",                       // tools builds (render harness) carry no version
#endif
                           juce::AudioProcessor::getWrapperTypeDescription (audioProcessor.wrapperType),
                           juce::SystemStats::getOperatingSystemName());
    addChildComponent (m_about);   // hidden until the info icon is clicked

    switchTab (0);

    setSize (1280, 800);
    setResizable (true, true);
    setResizeLimits (960, 600, 2560, 1600);   // 960 still fits the nine dock tabs + tone controls
    startTimerHz (60);
}

PlugNspectrPostEditor::~PlugNspectrPostEditor()
{
    // Stop any stimulus so Pre goes quiet immediately on editor close.
    audioProcessor.postCommand (pns::PNS_CmdBlock{});
    // Drop the stereo gate too — otherwise closing the window while the Stereo
    // tab is showing leaves four FFTs per frame running with nothing reading them.
    audioProcessor.setStereoActive (false);
    // And the EQ-curve accumulation gate, or with Measure on it keeps
    // averaging programme material into the held curve after the window closes.
    audioProcessor.setLinearMeasuring (false);
    setLookAndFeel (nullptr);
    stopTimer();
}

//──────────────────────────────────────────────────────────────────────────────
void PlugNspectrPostEditor::writeCmdBlock()
{
    pns::PNS_CmdBlock c;
    c.testToneFrequency = m_toneFreq;
    c.testToneLevelDb   = m_toneLevel;
    c.testToneActive    = m_toneActive ? 1u : 0u;
    // Noise stimulus only while the Linear tab is showing AND Measure is on.
    c.measureActive     = (m_activeTab == TabLinear && m_linearView.isMeasureActive()) ? 1u : 0u;
    // Dynamics stimulus: level ramp on the Transfer tab, level step on the
    // Envelope tab, THD sweep on the Distortion tab — only while showing + Measure on.
    c.dynMeasureMode    = (m_activeTab == TabTransfer && m_transferView.isMeasureActive()) ? 1u
                        : (m_activeTab == TabEnvelope && m_envelopeView.isMeasureActive()) ? 2u
                        : (m_activeTab == TabThd      && m_thdView.isMeasureActive())      ? 3u
                                                                                          : 0u;
    audioProcessor.postCommand (c);

    // Keep the Linear accumulation gate in lock-step with its noise stimulus so
    // pausing/leaving the tab/stopping transport all hold the curve.
    audioProcessor.setLinearMeasuring (c.measureActive != 0);
}

bool PlugNspectrPostEditor::isStimulusActive() const
{
    if (m_toneActive) return true;
    if (m_activeTab == TabLinear   && m_linearView  .isMeasureActive()) return true;
    if (m_activeTab == TabTransfer && m_transferView.isMeasureActive()) return true;
    if (m_activeTab == TabEnvelope && m_envelopeView.isMeasureActive()) return true;
    if (m_activeTab == TabThd      && m_thdView     .isMeasureActive()) return true;
    return false;
}

bool PlugNspectrPostEditor::disarmAllStimuli()
{
    bool changed = false;
    if (m_toneActive)
    {
        m_toneActive = false;
        m_toneBtn.setToggleState (false, juce::dontSendNotification);
        m_harmView.setToneActive (false);
        changed = true;
    }
    changed |= m_linearView  .disarm();
    changed |= m_transferView.disarm();
    changed |= m_envelopeView.disarm();
    changed |= m_thdView     .disarm();
    if (changed) { writeCmdBlock(); repaint(); }
    return changed;
}

//==============================================================================
void PlugNspectrPostEditor::timerCallback()
{
    // ── Transport-stop auto-defeat ──────────────────────────────────────────
    // A measurement stimulus replaces your audio; when the host transport goes
    // from playing to stopped, auto-disarm it so a tone can't keep running.
    const bool playing = audioProcessor.isTransportPlaying();
    if (m_wasTransportPlaying && ! playing)
        disarmAllStimuli();
    m_wasTransportPlaying = playing;

    // ── Overlay animation ──────────────────────────────────────────────────
    // Check Pre active every 200ms (every 12 ticks at 60fps)
    if (m_tickCounter % 12 == 0)
    {
        m_overlayTarget = audioProcessor.isPreActive() ? 0.0f : 1.0f;

        // Setup problems that make every reading unreliable.
        const juce::String warning =
            audioProcessor.isSecondPreActive()
                ? "Two PlugNspectr Pre plugins are active. Only the first one is used: remove the other."
          : audioProcessor.isHostBlockTooLarge()
                ? "The host buffer is over " + juce::String (pns::kMaxSamples)
                  + " samples, so part of each block isn't analysed. Use a smaller buffer."
                : juce::String();
        if (warning != m_setupWarning)
        {
            m_setupWarning = warning;
            repaint (0, 0, getWidth(), PnsTheme::kHeroHeight);
        }
    }

    // Smooth fade: slower fade-out (300ms) when Pre connects, faster fade-in
    const float diff      = m_overlayTarget - m_overlayAlpha;
    const float fadeStep  = (diff > 0.0f) ? 0.08f : 0.055f;
    if (std::abs (diff) > 0.004f)
    {
        m_overlayAlpha = juce::jlimit (0.0f, 1.0f, m_overlayAlpha + fadeStep * (diff > 0 ? 1.0f : -1.0f));
        repaint();
    }

    // Pulse phase for the searching dot (~1 cycle per second at 60fps)
    m_pulsePhase += juce::MathConstants<float>::twoPi / 60.0f;
    if (m_pulsePhase > juce::MathConstants<float>::twoPi)
        m_pulsePhase -= juce::MathConstants<float>::twoPi;

    // ── Views ──────────────────────────────────────────────────────────────
    // One drain of the capture ring per tick, handed to every reader of it.
    {
        const uint64_t before = m_capReadPos;
        m_cap.n       = audioProcessor.readCaptureSince (m_capReadPos, m_cap.pre, m_cap.post);
        m_cap.elapsed = m_capReadPos - before;
    }
    m_oscView.consumeCapture (m_cap);
    m_stereoView.consumeCapture (m_cap);

    m_lagTracker.update (m_cap);
    m_dynView.setLagSamples (m_lagTracker.lagSamples(), m_lagTracker.settled());
    audioProcessor.setAlignLag (m_lagTracker.locked() ? m_lagTracker.lagSamples() : 0);
    if (const auto note = delayNote(); note != m_delayNote)
    {
        m_delayNote = note;
        repaint (0, 0, getWidth(), PnsTheme::kHeroHeight);
    }
    m_dynView.update (m_cap);
    if (m_activeTab == TabDynamics) m_dynView.repaint();

    if (++m_tickCounter % 2 == 0)
    {
        m_specView.update();
        if (m_activeTab == TabSpectrum) m_specView.repaint();

        m_oscView.setLagSamples (m_lagTracker.lagSamples());
        m_oscView.setToneActive (m_toneActive);
        m_oscView.update();
        if (m_activeTab == TabOscilloscope) m_oscView.repaint();

        m_stereoView.update();
        if (m_activeTab == TabStereo) m_stereoView.repaint();

        m_harmView.update();
        if (m_activeTab == TabHarmonics) m_harmView.repaint();

        m_linearView.update();
        if (m_activeTab == TabLinear) m_linearView.repaint();

        m_transferView.update();
        if (m_activeTab == TabTransfer) m_transferView.repaint();

        m_envelopeView.update();
        if (m_activeTab == TabEnvelope) m_envelopeView.repaint();

        m_thdView.update();
        if (m_activeTab == TabThd) m_thdView.repaint();

        const auto sm = currentSummary();
        if (sm != m_summary)
        {
            m_summary = sm;
            repaint (0, 0, getWidth(), PnsTheme::kHeroHeight);
        }
    }
}

//==============================================================================
void PlugNspectrPostEditor::paintOverChildren (juce::Graphics& g)
{
    // The About modal sits above everything — don't draw the measuring border or
    // the no-Pre overlay over it.
    if (m_about.isVisible()) return;

    // Amber "measuring" border around the whole window whenever a test signal
    // is being emitted — an unmissable cue that the audio is being replaced.
    if (isStimulusActive())
    {
        g.setColour (PnsTheme::kWarn);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 6.0f, 3.0f);
    }

    if (m_overlayAlpha < 0.005f) return;

    const float alpha = m_overlayAlpha;
    const int W = getWidth(), H = getHeight();
    const int cY = PnsTheme::kHeroHeight;
    const int cH = H - cY - PnsTheme::kDockHeight;

    // ── Dim overlay over content area ──────────────────────────────────────
    g.setColour (PnsTheme::kBgDark.withAlpha (0.92f * alpha));
    g.fillRect (0, cY, W, cH);

    // ── Content box ────────────────────────────────────────────────────────
    constexpr float boxW = 430.0f;
    constexpr float boxH = 270.0f;
    const float boxX = (float) (W - (int) boxW) * 0.5f;
    const float boxY = (float) cY + ((float) cH - boxH) * 0.5f;

    PnsTheme::drawFrostedPanel (g, { boxX, boxY, boxW, boxH });

    float cy = boxY + 16.0f;

    // ── "No connection" icon (circle with diagonal slash) ──────────────────
    constexpr float iconDia = 44.0f, iconPad = 9.0f;
    const float icX = boxX + (boxW - iconDia) * 0.5f;
    g.setColour (PnsTheme::kColorGainRed.withAlpha (alpha));
    g.drawEllipse (icX, cy, iconDia, iconDia, 2.5f);
    g.drawLine (icX + iconPad,          cy + iconDia - iconPad,
                icX + iconDia - iconPad, cy + iconPad, 2.5f);
    cy += iconDia + 10.0f;

    // ── Title ──────────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontTitle());
    g.setColour (PnsTheme::kTextPrimary.withAlpha (alpha));
    g.drawText ("No Pre Plugin Detected",
                (int) boxX, (int) cy, (int) boxW, 14, juce::Justification::centred);
    cy += 18.0f;

    // ── Subtitle ───────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary.withAlpha (alpha));
    g.drawText ("PlugNspectr requires both Pre and Post plugins to be active.",
                (int) boxX, (int) cy, (int) boxW, 11, juce::Justification::centred);
    cy += 15.0f;

    // ── Divider ────────────────────────────────────────────────────────────
    g.setColour (PnsTheme::kBorderSubtle.withAlpha (alpha));
    {
        const float divX = boxX + (boxW - 200.0f) * 0.5f;
        g.drawHorizontalLine (juce::roundToInt (cy), divX, divX + 200.0f);
    }
    cy += 9.0f;

    // ── Setup instructions ─────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary.withAlpha (alpha));
    g.drawText ("To begin analysis, set up your signal chain:",
                (int) boxX, (int) cy, (int) boxW, 11, juce::Justification::centred);
    cy += 16.0f;

    // ── Signal chain diagram ───────────────────────────────────────────────
    constexpr float bH    = 22.0f;
    constexpr float bW1   = 96.0f;     // "PlugNspectr Pre"
    constexpr float bW2   = 108.0f;    // "Plugin to Analyze"
    constexpr float bW3   = 100.0f;    // "PlugNspectr Post"
    constexpr float arrW  = 20.0f;
    const float chainW    = bW1 + arrW + bW2 + arrW + bW3;
    const float chainX    = boxX + (boxW - chainW) * 0.5f;

    auto drawBlock = [&] (float bx, float bw, juce::Colour border, const char* lbl)
    {
        g.setColour (PnsTheme::kBgWidget.withAlpha (alpha));
        g.fillRoundedRectangle (bx, cy, bw, bH, 3.0f);
        g.setColour (border.withAlpha (alpha));
        g.drawRoundedRectangle (bx + 0.5f, cy + 0.5f, bw - 1.0f, bH - 1.0f, 3.0f, 1.0f);
        g.setFont (PnsTheme::fontLabel());
        g.setColour (PnsTheme::kTextPrimary.withAlpha (alpha * 0.85f));
        g.drawText (lbl, (int) bx, (int) cy, (int) bw, (int) bH, juce::Justification::centred);
    };

    auto drawArrow = [&] (float ax)
    {
        const float ay = cy + bH * 0.5f;
        g.setColour (PnsTheme::kTextSecondary.withAlpha (alpha));
        g.drawLine (ax + 1.0f, ay, ax + arrW - 6.0f, ay, 1.0f);
        juce::Path head;
        const float hx = ax + arrW - 6.0f;
        head.addTriangle (hx, ay - 3.5f, hx + 6.0f, ay, hx, ay + 3.5f);
        g.fillPath (head);
    };

    drawBlock (chainX,                              bW1, PnsTheme::kColorPre,     "PlugNspectr Pre");
    drawArrow (chainX + bW1);
    drawBlock (chainX + bW1 + arrW,                 bW2, PnsTheme::kBorderActive, "Plugin to Analyze");
    drawArrow (chainX + bW1 + arrW + bW2);
    drawBlock (chainX + bW1 + arrW + bW2 + arrW,    bW3, PnsTheme::kColorPost,    "PlugNspectr Post");
    cy += bH + 10.0f;

    // ── Note ───────────────────────────────────────────────────────────────
    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kGridLabel.withAlpha (alpha));
    g.drawText ("Both plugins must be on the same audio track and",
                (int) boxX + 10, (int) cy, (int) boxW - 20, 11, juce::Justification::centred);
    cy += 12.0f;
    g.drawText ("PlugNspectr Pre must come before the plugin you want to analyze.",
                (int) boxX + 10, (int) cy, (int) boxW - 20, 11, juce::Justification::centred);
    cy += 16.0f;

    // ── Pulsing search dot + status text ───────────────────────────────────
    const float pulse  = 0.65f + 0.35f * std::sin (m_pulsePhase);
    const float dotR   = 4.0f * pulse;
    constexpr float statusContentW = 172.0f;
    const float dotX   = boxX + (boxW - statusContentW) * 0.5f;
    const float dotCY  = cy + 5.5f;

    g.setColour (PnsTheme::kColorGainRed.withAlpha (alpha * pulse));
    g.fillEllipse (dotX, dotCY - dotR, dotR * 2.0f, dotR * 2.0f);

    g.setFont (PnsTheme::fontLabel());
    g.setColour (PnsTheme::kTextSecondary.withAlpha (alpha));
    g.drawText ("Searching for PlugNspectr Pre...",
                (int) (dotX + 11.0f), (int) (dotCY - 5.5f),
                (int) (statusContentW - 11.0f), 11,
                juce::Justification::centredLeft);
}

//==============================================================================
// Each tab answers one question: shown above the headline and as its tooltip.
const char* PlugNspectrPostEditor::tabQuestion (int tab)
{
    switch (tab)
    {
        case TabSpectrum:     return "What does it do to the tone?";
        case TabDynamics:     return "How much does it turn the level down?";
        case TabOscilloscope: return "Does it change the shape of the wave?";
        case TabStereo:       return "Does it make the sound wider or narrower?";
        case TabHarmonics:    return "What does it add to a pure tone?";
        case TabLinear:       return "Which frequencies does it change?";
        case TabTransfer:     return "How hard does it squash loud sounds?";
        case TabEnvelope:     return "How fast does it grab and let go?";
        case TabThd:          return "Is it clean at every pitch?";
        default:              return "";
    }
}

// Shown on the tabs that compare Pre and Post sample by sample, so you can
// see the plugin chain's delay and trust (or doubt) the alignment. The EQ
// curve reports its own measured delay in its readout instead.
juce::String PlugNspectrPostEditor::delayNote() const
{
    const bool usesAlignment = m_activeTab == TabDynamics || m_activeTab == TabOscilloscope
                            || m_activeTab == TabTransfer || m_activeTab == TabEnvelope;
    const double sr = audioProcessor.getSampleRate();
    if (! usesAlignment || ! audioProcessor.isPreActive() || sr <= 0.0) return {};

    if (m_lagTracker.locked())
    {
        const int lag = m_lagTracker.lagSamples();
        if (lag == 0) return {};
        return "lined up for " + juce::String (lag) + " samples ("
             + juce::String (1000.0 * lag / sr, 1) + " ms) of plugin delay";
    }
    return m_lagTracker.settled() ? "plugin delay not measurable on this sound, assuming none"
                                  : "measuring plugin delay...";
}

//==============================================================================
// Plain-language summaries — what each tab found, in one sentence, for the
// headline above the view. Thresholds are "would a listener notice": ~1 dB for
// level, 0.5 dB of gain reduction, 0.01% THD. Below them the plugin is reported
// as leaving that property alone rather than quoting noise.
//==============================================================================
PnsTheme::Summary PlugNspectrPostEditor::currentSummary() const
{
    switch (m_activeTab)
    {
        case TabSpectrum:     return m_specView    .summary();
        case TabDynamics:     return m_dynView     .summary();
        case TabOscilloscope: return m_oscView     .summary();
        case TabStereo:       return m_stereoView  .summary();
        case TabHarmonics:    return m_harmView    .summary();
        case TabLinear:       return m_linearView  .summary();
        case TabTransfer:     return m_transferView.summary();
        case TabEnvelope:     return m_envelopeView.summary();
        case TabThd:          return m_thdView     .summary();
        default:              return {};
    }
}

// Tone-control relevance. FREQ + Test Tone belong to the single-tone tabs (live tabs
// + Harmonics); LEVEL also drives the Distortion sweep. Stereo is excluded even
// though it's live: every Pre generator is mono, so a tone reads as zero side.
bool PlugNspectrPostEditor::toneFreqOn (int tab)
{
    return (tab <= TabOscilloscope || tab == TabHarmonics);
}
bool PlugNspectrPostEditor::toneLevelOn (int tab)
{
    return toneFreqOn (tab) || tab == TabThd;
}

//==============================================================================
void PlugNspectrPostEditor::switchTab (int index)
{
    m_activeTab = index;

    for (int i = 0; i < TabCount; ++i)
        m_tabs[i]->setActive (i == index);

    m_specView    .setVisible (index == TabSpectrum);
    m_dynView     .setVisible (index == TabDynamics);
    m_oscView     .setVisible (index == TabOscilloscope);
    m_stereoView  .setVisible (index == TabStereo);
    m_harmView    .setVisible (index == TabHarmonics);
    m_linearView  .setVisible (index == TabLinear);
    m_transferView.setVisible (index == TabTransfer);
    m_envelopeView.setVisible (index == TabEnvelope);
    m_thdView     .setVisible (index == TabThd);

    // ── Tone-control relevance per tab (see toneFreqOn) ────────────────────────
    const bool freqOn  = toneFreqOn (index);
    const bool levelOn = toneLevelOn (index);
    const bool toneOn  = freqOn;

    // Four extra FFTs — only run them while the Stereo tab is actually showing.
    audioProcessor.setStereoActive (index == TabStereo);
    // The L+R / Side selector is meaningless on Stereo: it uses both inherently.
    const bool chOn = (index != TabStereo);
    m_chLR  .setEnabled (chOn);
    m_chSide.setEnabled (chOn);
    m_chLR  .setAlpha (chOn ? 1.0f : 0.4f);
    m_chSide.setAlpha (chOn ? 1.0f : 0.4f);

    // Entering a tab that doesn't own the tone kills a stray tone so it can't
    // override the measurement stimulus (Pre prioritises the tone otherwise).
    if (! toneOn && m_toneActive)
    {
        m_toneActive = false;
        m_toneBtn.setToggleState (false, juce::dontSendNotification);
        m_harmView.setToneActive (false);
    }

    // Hide rather than grey out: a dimmed control still asks "should I touch this?"
    m_toneFreqSlider .setVisible (freqOn);
    m_toneLevelSlider.setVisible (levelOn);
    m_toneBtn    .setVisible (toneOn);
    m_summary = currentSummary();

    writeCmdBlock();   // start/stop the measurement noise with the tab
    repaint();
}

//==============================================================================
void PlugNspectrPostEditor::paint (juce::Graphics& g)
{
    g.fillAll (PnsTheme::kBgDark);
    const int W = getWidth(), H = getHeight();
    constexpr int kPadX = 28;

    auto textW = [] (const juce::Font& f, const juce::String& t)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, t, 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
    };

    // ── Headline block ─────────────────────────────────────────────────────
    //    Question (small, accent) → what it found (large) → the detail; the one
    //    number that answers it sits large at the right. While a test signal is
    //    playing the question line turns amber and says so: it's the one thing
    //    you must not miss. A setup problem (two Pre plugins, an oversized host
    //    buffer) takes the same line, after the test signal.
    {
        // The big number, right-aligned under the header controls.
        int heroLeft = W - kPadX;
        if (m_summary.heroValue.isNotEmpty())
        {
            const auto fv = PnsTheme::fontSized (44.0f, true);
            const auto fu = PnsTheme::fontSized (18.0f);
            const int wu = m_summary.heroUnit.isEmpty() ? 0 : textW (fu, m_summary.heroUnit) + 6;
            const int wv = textW (fv, m_summary.heroValue);
            const int right = W - kPadX;
            heroLeft = right - wu - wv;

            g.setFont (fu);
            g.setColour (PnsTheme::kTextSecondary);
            g.drawText (m_summary.heroUnit, right - wu + 6, 64, wu, 30, juce::Justification::bottomLeft);
            g.setFont (fv);
            g.setColour (PnsTheme::kTextPrimary);
            g.drawText (m_summary.heroValue, heroLeft, 50, wv + 2, 46, juce::Justification::bottomLeft);

            g.setFont (PnsTheme::fontSized (11.0f, true));
            g.setColour (PnsTheme::kTextSecondary);
            g.drawText (m_summary.heroLabel.toUpperCase(), right - 220, 42, 220, 14,
                        juce::Justification::centredRight);
            heroLeft = juce::jmin (heroLeft, right - textW (PnsTheme::fontSized (11.0f, true),
                                                            m_summary.heroLabel.toUpperCase()));
        }
        const int textRight = juce::jmin (heroLeft, m_logoX) - 32;

        // Question line
        const bool measuring = isStimulusActive();
        const bool isTest    = m_activeTab >= kFirstMeasTab;
        g.setFont (PnsTheme::fontSized (12.0f, true));
        if (measuring)
        {
            g.setColour (PnsTheme::kWarn);
            g.drawText (juce::CharPointer_UTF8 ("\xe2\x97\x8f  TEST SIGNAL PLAYING  \xc2\xb7  your audio is replaced until you stop playback or leave this tab"),
                        kPadX, 18, textRight - kPadX, 16, juce::Justification::centredLeft, true);
        }
        else if (m_setupWarning.isNotEmpty())
        {
            g.setColour (PnsTheme::kWarn);
            g.drawText (juce::String::fromUTF8 ("\xe2\x97\x8f  CHECK YOUR SETUP  \xc2\xb7  ") + m_setupWarning,
                        kPadX, 18, textRight - kPadX, 16, juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour (PnsTheme::kAccentPrimary);
            const juce::String q = juce::String (isTest ? "TEST" : "LISTEN")
                                 + juce::String::fromUTF8 ("  \xc2\xb7  ") + tabQuestion (m_activeTab);
            g.drawText (q, kPadX, 18, textRight - kPadX, 16, juce::Justification::centredLeft, true);
        }

        // Headline + detail: the verdict's first clause is the headline, the
        // rest ("  -  output is 4 dB quieter") is the supporting line.
        const juce::String sep ("  -  ");
        const juce::String head   = m_summary.text.upToFirstOccurrenceOf (sep, false, false);
        juce::String detail = m_summary.text.fromFirstOccurrenceOf (sep, false, false)
                                            .replace (sep, juce::String::fromUTF8 ("  \xc2\xb7  "));
        if (m_delayNote.isNotEmpty())
            detail = detail.isEmpty() ? m_delayNote
                                      : detail + juce::String::fromUTF8 ("  \xc2\xb7  ") + m_delayNote;
        g.setFont (PnsTheme::fontSized (26.0f, true));
        g.setColour (m_summary.warn ? PnsTheme::kWarn : PnsTheme::kTextPrimary);
        g.drawFittedText (head, kPadX, 40, textRight - kPadX, 34, juce::Justification::centredLeft, 1, 0.8f);
        g.setFont (PnsTheme::fontSized (14.0f));
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText (detail, kPadX, 76, textRight - kPadX, 20, juce::Justification::centredLeft, true);
    }

    // Header controls' caption, and the logo at the far left of them.
    if (m_pnsLogo.isValid())
    {
        const float logoH = 20.0f;
        const float logoW = logoH * ((float) m_pnsLogo.getWidth() / (float) m_pnsLogo.getHeight());
        g.setOpacity (0.85f);
        g.drawImage (m_pnsLogo, { (float) m_logoX, 12.0f, logoW, logoH });
        g.setOpacity (1.0f);
    }

    // ── Dock ───────────────────────────────────────────────────────────────
    const int dockY = H - PnsTheme::kDockHeight;
    g.setColour (PnsTheme::kBgPanel);
    g.fillRect (0, dockY, W, PnsTheme::kDockHeight);
    g.setColour (PnsTheme::kBorderSubtle);
    g.drawHorizontalLine (dockY, 0.0f, (float) W);

    // Group captions, set vertically just left of each group.
    auto caption = [&] (const char* text, int rightX)
    {
        g.saveState();
        const float cx = (float) rightX - 9.0f, cy = (float) dockY + PnsTheme::kDockHeight * 0.5f;
        g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, cx, cy));
        g.setFont (PnsTheme::fontSized (9.5f, true));
        g.setColour (PnsTheme::kGridLabel);
        g.drawText (text, juce::Rectangle<float> (60.0f, 12.0f).withCentre ({ cx, cy }),
                    juce::Justification::centred);
        g.restoreState();
    };
    caption ("LISTEN", m_tabs[0]->getX());
    caption ("TEST",   m_tabs[kFirstMeasTab]->getX());
    g.setColour (PnsTheme::kBorderSubtle);
    g.fillRect (m_dockDividerX, dockY + 16, 1, PnsTheme::kDockHeight - 32);

    // Tone readouts beside their knobs (hidden with the knobs on tabs that
    // don't use them — see toneFreqOn / toneLevelOn).
    auto readout = [&] (juce::Rectangle<int> r, const char* label, const juce::String& value)
    {
        g.setFont (PnsTheme::fontSized (10.0f, true));
        g.setColour (PnsTheme::kTextSecondary);
        g.drawText (label, r.removeFromTop (r.getHeight() / 2), juce::Justification::bottomLeft);
        g.setFont (PnsTheme::fontPrimary());
        g.setColour (m_toneActive ? PnsTheme::kTextAccent : PnsTheme::kTextPrimary);
        g.drawText (value, r, juce::Justification::topLeft);
    };
    if (toneFreqOn (m_activeTab))
        readout (m_freqText, "TONE", PnsTheme::fmtHz (m_toneFreq));
    if (toneLevelOn (m_activeTab))
        readout (m_levelText, "LEVEL", juce::String (m_toneLevel, 1) + " dB");
}

//==============================================================================
void PlugNspectrPostEditor::resized()
{
    const int W = getWidth(), H = getHeight();
    constexpr int kPadX = 28;

    // ── Header controls, top right: logo · L+R/Side · info ─────────────────
    constexpr int infoSz = 18, segW = 46, segH = 24, rowY = 10;
    m_infoBtn.setBounds (W - kPadX + 4 - infoSz, rowY + (segH - infoSz) / 2, infoSz, infoSz);
    const int segRight = m_infoBtn.getX() - 12;
    m_chLR  .setBounds (segRight - segW * 2, rowY, segW, segH);
    m_chSide.setBounds (segRight - segW,     rowY, segW, segH);
    m_logoX = m_pnsLogo.isValid()
                ? m_chLR.getX() - 16 - juce::roundToInt (20.0f * (float) m_pnsLogo.getWidth()
                                                               / (float) m_pnsLogo.getHeight())
                : m_chLR.getX();

    m_about.setBounds (getLocalBounds());

    // ── View area between the headline block and the dock ──────────────────
    // Same side margins as the headline, so graphs line up under the text.
    const auto content = getLocalBounds().withTrimmedTop (PnsTheme::kHeroHeight)
                                         .withTrimmedBottom (PnsTheme::kDockHeight + 14)
                                         .withTrimmedLeft (kPadX - 10).withTrimmedRight (kPadX - 16);
    for (auto* v : std::initializer_list<juce::Component*> { &m_specView, &m_dynView, &m_oscView, &m_stereoView,
                                                             &m_harmView, &m_linearView, &m_transferView,
                                                             &m_envelopeView, &m_thdView })
        v->setBounds (content);

    // ── Dock: tone controls take a fixed block at the right, tabs share the rest
    const int dockY = H - PnsTheme::kDockHeight;
    constexpr int kKnob = 32, kText = 70, kToneBtnW = 104;
    const int toneW = kKnob + 6 + kText + 10 + kKnob + 6 + kText + 14 + kToneBtnW;
    int x = W - 18 - toneW;
    const int knobY = dockY + (PnsTheme::kDockHeight - kKnob) / 2;
    m_toneFreqSlider .setBounds (x, knobY, kKnob, kKnob);            x += kKnob + 6;
    m_freqText = { x, knobY - 2, kText, kKnob + 4 };                    x += kText + 10;
    m_toneLevelSlider.setBounds (x, knobY, kKnob, kKnob);            x += kKnob + 6;
    m_levelText = { x, knobY - 2, kText, kKnob + 4 };                   x += kText + 14;
    m_toneBtn    .setBounds (x, dockY + (PnsTheme::kDockHeight - 32) / 2, kToneBtnW, 32);

    constexpr int kCaptionW = 20, kDividerW = 17;
    const int tabsLeft = 14, tabsRight = W - 18 - toneW - 16;
    const int tabW = juce::jlimit (56, 88, (tabsRight - tabsLeft - 2 * kCaptionW - kDividerW) / TabCount);
    x = tabsLeft + kCaptionW;
    for (int i = 0; i < TabCount; ++i)
    {
        if (i == kFirstMeasTab)
        {
            m_dockDividerX = x + kDividerW / 2 - 1;
            x += kDividerW + kCaptionW;
        }
        m_tabs[i]->setBounds (x, dockY + 2, tabW, PnsTheme::kDockHeight - 4);
        x += tabW;
    }
}
