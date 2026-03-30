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
    const bool toneActive = (m_pCmd != nullptr && m_pCmd->testToneActive != 0);

    if (toneActive)
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
