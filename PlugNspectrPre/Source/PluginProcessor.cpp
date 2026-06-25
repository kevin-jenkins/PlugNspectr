/*
  ==============================================================================
    PlugNspectrPre  –  PluginProcessor.cpp
  ==============================================================================
*/

#include "PluginProcessor.h"
#ifndef PNS_HEADLESS_TESTS          // editor-free in the unit-test target
#include "PluginEditor.h"
#endif

#include <cstring>   // std::memcpy
#include <cmath>

//==============================================================================
PlugNspectrPreProcessor::PlugNspectrPreProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor (BusesProperties()
                     .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
#endif
{
}

PlugNspectrPreProcessor::~PlugNspectrPreProcessor()
{
    m_ipc.close();
}

//==============================================================================
void PlugNspectrPreProcessor::prepareToPlay (double /*sampleRate*/, int /*samplesPerBlock*/)
{
    m_ipc.open();          // create-or-open the IPC segment, off the audio thread
    m_tonePhase = 0.0;
}

void PlugNspectrPreProcessor::releaseResources()
{
    m_ipc.close();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool PlugNspectrPreProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Accept mono or stereo; input layout must match output layout.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainOutputChannelSet() == layouts.getMainInputChannelSet();
}
#endif

//==============================================================================
void PlugNspectrPreProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    // Snapshot the current command (torn-free via the seqlock); tests inject directly.
    if (! m_testCmd)
        m_ipc.readCommand (m_cmd);

    // ── Test-tone mode ────────────────────────────────────────────────────────
    const bool toneActive    = (m_cmd.testToneActive != 0);
    const bool measureActive = (m_cmd.measureActive  != 0);
    const bool dynRamp       = (m_cmd.dynMeasureMode == 1);
    const bool dynStep       = (m_cmd.dynMeasureMode == 2);
    const bool thdSweep      = (m_cmd.dynMeasureMode == 3);

    if (thdSweep && ! toneActive)
    {
        // STEPPED log sweep 50 Hz → 5 kHz: hold each of 100 frequencies for
        // ~0.15 s (long enough for a clean measurement FFT frame), so Post gets
        // an accurate THD per frequency. The held fundamental is published (Hz).
        const double sr      = getSampleRate();
        const double loHz = 50.0, hiHz = 5000.0;
        constexpr int kSteps = 100;
        const double holdLen = 0.15 * sr;
        const double sweepLen = holdLen * kSteps;
        double levelDb = m_cmd.testToneLevelDb;
        if (levelDb > -0.5 || levelDb < -90.0) levelDb = -6.0;
        const float  amp  = (float) std::pow (10.0, levelDb / 20.0);
        const int    numCh = buffer.getNumChannels(), numSmp = buffer.getNumSamples();

        const int    step0 = (int) (m_dynRampPhase / holdLen) % kSteps;
        const double fHz   = loHz * std::pow (hiHz / loHz, (double) step0 / (kSteps - 1));
        m_dynEnvBlockStart = (uint32_t) fHz;                       // held fundamental Hz
        const double phaseInc = (2.0 * juce::MathConstants<double>::pi * fHz) / sr;

        float* ch0 = buffer.getWritePointer (0);
        for (int i = 0; i < numSmp; ++i)
        {
            m_tonePhase += phaseInc;
            if (m_tonePhase > juce::MathConstants<double>::twoPi) m_tonePhase -= juce::MathConstants<double>::twoPi;
            ch0[i] = amp * (float) std::sin (m_tonePhase);

            m_dynRampPhase += 1.0;
            if (m_dynRampPhase >= sweepLen) m_dynRampPhase = 0.0;
        }
        for (int ch = 1; ch < numCh; ++ch)
            std::memcpy (buffer.getWritePointer (ch), ch0,
                         static_cast<size_t> (numSmp) * sizeof (float));
    }
    else if (dynStep && ! toneActive)
    {
        // Level-stepped 1 kHz sine for attack/release measurement: HIGH (-10 dBFS)
        // for the first half-second of a 1 s cycle (step up → attack), LOW
        // (-40 dBFS) for the second half (step down → release). The cycle position
        // is published so Post can synchronously average GR vs time.
        const double sr       = getSampleRate();
        const double phaseInc = (2.0 * juce::MathConstants<double>::pi * 1000.0) / sr;
        const double period   = 1.0 * sr;
        const double half     = period * 0.5;
        const int    numCh    = buffer.getNumChannels();
        const int    numSmp   = buffer.getNumSamples();

        m_dynEnvBlockStart = (uint32_t) m_dynRampPhase;   // reuse ramp counter as cycle pos

        float* ch0 = buffer.getWritePointer (0);
        for (int i = 0; i < numSmp; ++i)
        {
            m_tonePhase += phaseInc;
            if (m_tonePhase > juce::MathConstants<double>::twoPi) m_tonePhase -= juce::MathConstants<double>::twoPi;

            const double levelDb = (m_dynRampPhase < half) ? -10.0 : -40.0;
            const double amp     = std::pow (10.0, levelDb / 20.0);
            ch0[i] = (float) (amp * std::sin (m_tonePhase));

            m_dynRampPhase += 1.0;
            if (m_dynRampPhase >= period) m_dynRampPhase = 0.0;
        }
        for (int ch = 1; ch < numCh; ++ch)
            std::memcpy (buffer.getWritePointer (ch), ch0,
                         static_cast<size_t> (numSmp) * sizeof (float));
    }
    else if (dynRamp && ! toneActive)
    {
        // Level-ramped 1 kHz sine: amplitude sweeps -60 → 0 dBFS over 6 s and
        // repeats. Slow enough for a compressor to settle, so Post can plot the
        // static transfer curve (output level vs input level).
        const double sr        = getSampleRate();
        const double phaseInc  = (2.0 * juce::MathConstants<double>::pi * 1000.0) / sr;
        const double rampLen   = 6.0 * sr;     // samples for a full sweep
        const int    numCh     = buffer.getNumChannels();
        const int    numSmp    = buffer.getNumSamples();

        float* ch0 = buffer.getWritePointer (0);
        for (int i = 0; i < numSmp; ++i)
        {
            m_tonePhase += phaseInc;
            if (m_tonePhase > juce::MathConstants<double>::twoPi) m_tonePhase -= juce::MathConstants<double>::twoPi;

            const double tt      = m_dynRampPhase / rampLen;        // 0..1
            const double levelDb  = -60.0 + 60.0 * tt;
            const double amp      = std::pow (10.0, levelDb / 20.0);
            ch0[i] = (float) (amp * std::sin (m_tonePhase));

            m_dynRampPhase += 1.0;
            if (m_dynRampPhase >= rampLen) m_dynRampPhase = 0.0;
        }
        for (int ch = 1; ch < numCh; ++ch)
            std::memcpy (buffer.getWritePointer (ch), ch0,
                         static_cast<size_t> (numSmp) * sizeof (float));
    }
    else if (measureActive && ! toneActive)
    {
        // White-noise stimulus for the Linear (magnitude/phase/group-delay)
        // measurement. Generated here so it passes through the plugin under
        // analysis before Post measures the transfer function.
        constexpr float kNoiseAmp = 0.25f;   // ~-12 dBFS
        const int numCh  = buffer.getNumChannels();
        const int numSmp = buffer.getNumSamples();

        float* ch0 = buffer.getWritePointer (0);
        for (int i = 0; i < numSmp; ++i)
            ch0[i] = kNoiseAmp * (m_noiseRng.nextFloat() * 2.0f - 1.0f);
        for (int ch = 1; ch < numCh; ++ch)
            std::memcpy (buffer.getWritePointer (ch), ch0,
                         static_cast<size_t> (numSmp) * sizeof (float));
    }
    else if (toneActive)
    {
        const double sr        = getSampleRate();
        const double frequency = m_cmd.testToneFrequency;
        const double freq      = juce::jlimit (20.0, 20000.0, frequency);
        const double phaseInc  = (2.0 * juce::MathConstants<double>::pi * freq) / sr;
        // Tone level from the command block (dBFS), clamped; default -6 dBFS.
        double levelDb = m_cmd.testToneLevelDb;
        if (levelDb > -0.5 || levelDb < -90.0) levelDb = -6.0;   // 0/unset → default
        const float kAmp = (float) std::pow (10.0, levelDb / 20.0);

        const int numCh  = buffer.getNumChannels();
        const int numSmp = buffer.getNumSamples();

        // Generate sine into channel 0, then copy to remaining channels.
        float* ch0 = buffer.getWritePointer (0);
        for (int i = 0; i < numSmp; ++i)
        {
            m_tonePhase += phaseInc;
            if (m_tonePhase > juce::MathConstants<double>::twoPi) m_tonePhase -= juce::MathConstants<double>::twoPi;
            ch0[i] = kAmp * (float) std::sin (m_tonePhase);
        }
        for (int ch = 1; ch < numCh; ++ch)
            std::memcpy (buffer.getWritePointer (ch), ch0,
                         static_cast<size_t> (numSmp) * sizeof (float));
    }
    else
    {
        m_tonePhase = 0.0;
    }

    // ── Publish the captured block + heartbeat to the IPC segment ─────────────
    // (skipped under test injection so the unit tests never touch the real segment)
    if (! m_testCmd)
    {
        m_ipc.writeBlock (buffer.getArrayOfReadPointers(),
                          buffer.getNumChannels(), buffer.getNumSamples(),
                          getSampleRate(), m_dynEnvBlockStart, m_cmd.dynMeasureMode);
        m_ipc.preHeartbeat();
    }
    // When test tone is active the buffer now contains the sine wave,
    // which passes through to the plugin-under-analysis as intended.
}

//==============================================================================
juce::AudioProcessorEditor* PlugNspectrPreProcessor::createEditor()
{
#ifndef PNS_HEADLESS_TESTS
    return new PlugNspectrPreEditor (*this);
#else
    return nullptr;                 // unit-test target links no editor
#endif
}

//==============================================================================
// Entry point called by the host to create the plugin instance.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PlugNspectrPreProcessor();
}
