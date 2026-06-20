/*
  ==============================================================================
    PlugNspectrPre  –  PluginProcessor.cpp
  ==============================================================================
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"

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
    closeSharedMemory();
}

//==============================================================================
void PlugNspectrPreProcessor::openSharedMemory()
{
    if (m_hMapFile != nullptr)
        return;

    // Create (or open if already exists) the named file-mapping object.
    HANDLE hMap = CreateFileMappingA (
        INVALID_HANDLE_VALUE,          // backed by the paging file
        nullptr,                       // default security
        PAGE_READWRITE,
        0,                             // size high DWORD
        kPNS_SharedMemBytes,           // size low DWORD
        kPNS_SharedMemName);

    if (hMap == nullptr || hMap == INVALID_HANDLE_VALUE)
        return;

    m_hMapFile = hMap;

    m_pShared = static_cast<PNS_SharedBlock*> (
        MapViewOfFile (m_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, kPNS_SharedMemBytes));

    if (m_pShared == nullptr)
    {
        CloseHandle (m_hMapFile);
        m_hMapFile = nullptr;
        return;
    }

    // First creator initialises the block; subsequent openers skip this.
    if (m_pShared->magic != kPNS_Magic)
    {
        ZeroMemory (m_pShared, kPNS_SharedMemBytes);
        m_pShared->magic = kPNS_Magic;
    }
}

void PlugNspectrPreProcessor::closeSharedMemory()
{
    if (m_pShared != nullptr)
    {
        UnmapViewOfFile (m_pShared);
        m_pShared = nullptr;
    }
    if (m_hMapFile != nullptr)
    {
        CloseHandle (m_hMapFile);
        m_hMapFile = nullptr;
    }
}

//==============================================================================
void PlugNspectrPreProcessor::openCmdMemory()
{
    if (m_pCmd != nullptr)
        return;

    // Post creates this mapping; Pre opens it read-only.
    // If Post hasn't run yet the call simply fails — retry next block.
    HANDLE hMap = OpenFileMappingA (FILE_MAP_READ, FALSE, kPNS_CmdMemName);
    if (hMap == nullptr || hMap == INVALID_HANDLE_VALUE)
        return;

    m_hCmdFile = hMap;
    m_pCmd = static_cast<PNS_CmdBlock*> (
        MapViewOfFile (m_hCmdFile, FILE_MAP_READ, 0, 0, kPNS_CmdMemBytes));

    if (m_pCmd == nullptr)
    {
        CloseHandle (m_hCmdFile);
        m_hCmdFile = nullptr;
    }
}

void PlugNspectrPreProcessor::closeCmdMemory()
{
    if (m_pCmd != nullptr)
    {
        UnmapViewOfFile (m_pCmd);
        m_pCmd = nullptr;
    }
    if (m_hCmdFile != nullptr)
    {
        CloseHandle (m_hCmdFile);
        m_hCmdFile = nullptr;
    }
}

//==============================================================================
void PlugNspectrPreProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    openSharedMemory();
    openCmdMemory();

    if (m_pShared != nullptr)
        m_pShared->sampleRate = sampleRate;

    m_tonePhase = 0.0;
}

void PlugNspectrPreProcessor::releaseResources()
{
    closeCmdMemory();
    closeSharedMemory();
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

    // Lazily try to open command memory if Post started after us.
    if (m_pCmd == nullptr)
        openCmdMemory();

    // ── Test-tone mode ────────────────────────────────────────────────────────
    const bool toneActive    = (m_pCmd != nullptr && m_pCmd->testToneActive != 0);
    const bool measureActive = (m_pCmd != nullptr && m_pCmd->measureActive  != 0);
    const bool dynRamp       = (m_pCmd != nullptr && m_pCmd->dynMeasureMode == 1);

    if (dynRamp && ! toneActive)
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
        const double frequency = (m_pCmd != nullptr) ? m_pCmd->testToneFrequency : 1000.0;
        const double freq      = juce::jlimit (20.0, 20000.0, frequency);
        const double phaseInc  = (2.0 * juce::MathConstants<double>::pi * freq) / sr;
        constexpr float kAmp   = 0.5f;   // -6 dBFS

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

    // ── Write capture block to shared memory ──────────────────────────────────
    if (m_pShared != nullptr)
    {
        const int numCh  = juce::jmin (buffer.getNumChannels(), kPNS_MaxChannels);
        const int numSmp = juce::jmin (buffer.getNumSamples(),  kPNS_MaxSamplesPerBlock);

        m_pShared->numChannels = numCh;
        m_pShared->numSamples  = numSmp;

        for (int ch = 0; ch < numCh; ++ch)
        {
            std::memcpy (m_pShared->preData[ch],
                         buffer.getReadPointer (ch),
                         static_cast<size_t> (numSmp) * sizeof (float));
        }

        ++m_pShared->writeCount;
        m_pShared->preLastHeartbeat = juce::Time::getMillisecondCounter();
    }
    // When test tone is active the buffer now contains the sine wave,
    // which passes through to the plugin-under-analysis as intended.
}

//==============================================================================
juce::AudioProcessorEditor* PlugNspectrPreProcessor::createEditor()
{
    return new PlugNspectrPreEditor (*this);
}

//==============================================================================
// Entry point called by the host to create the plugin instance.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PlugNspectrPreProcessor();
}
