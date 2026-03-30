/*
  ==============================================================================
    PlugNspectrPost  –  PluginProcessor.cpp
  ==============================================================================
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstring>
#include <cmath>

//==============================================================================
PlugNspectrPostProcessor::PlugNspectrPostProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor (BusesProperties()
                     .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
#endif
{
}

PlugNspectrPostProcessor::~PlugNspectrPostProcessor()
{
    closeSharedMemory();
}

//==============================================================================
void PlugNspectrPostProcessor::openSharedMemory()
{
    if (m_hMapFile != nullptr)
        return;

    // Post needs write access to update postLastHeartbeat in the shared block.
    m_hMapFile = OpenFileMappingA (FILE_MAP_WRITE, FALSE, kPNS_SharedMemName);

    if (m_hMapFile == nullptr || m_hMapFile == INVALID_HANDLE_VALUE)
    {
        m_hMapFile = nullptr;
        return;
    }

    m_pShared = static_cast<PNS_SharedBlock*> (
        MapViewOfFile (m_hMapFile, FILE_MAP_WRITE, 0, 0, kPNS_SharedMemBytes));

    if (m_pShared == nullptr)
    {
        CloseHandle (m_hMapFile);
        m_hMapFile = nullptr;
    }
}

void PlugNspectrPostProcessor::closeSharedMemory()
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
void PlugNspectrPostProcessor::prepareToPlay (double /*sampleRate*/, int samplesPerBlock)
{
    openSharedMemory();

    const int safeSamples = juce::jmax (samplesPerBlock, kPNS_MaxSamplesPerBlock);

    juce::ScopedLock sl (m_captureLock);
    m_capture.pre .setSize (kPNS_MaxChannels, safeSamples, false, true, false);
    m_capture.post.setSize (kPNS_MaxChannels, safeSamples, false, true, false);
}

void PlugNspectrPostProcessor::releaseResources()
{
    closeSharedMemory();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool PlugNspectrPostProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainOutputChannelSet() == layouts.getMainInputChannelSet();
}
#endif

//==============================================================================
void PlugNspectrPostProcessor::pushSamplesToAccum (
    const float* src, int count,
    std::array<float, kFftSize>& accum,
    int& pos,
    std::array<float, kNumSpecBins>& outSpectrum)
{
    for (int i = 0; i < count; ++i)
    {
        accum[pos++] = src[i];

        if (pos >= kFftSize)
        {
            pos = 0;

            std::copy (accum.begin(), accum.end(), m_fftWorkBuf.begin());
            std::fill (m_fftWorkBuf.begin() + kFftSize, m_fftWorkBuf.end(), 0.0f);

            m_window.multiplyWithWindowingTable (m_fftWorkBuf.data(), kFftSize);
            m_fft.performRealOnlyForwardTransform (m_fftWorkBuf.data(), true);

            juce::ScopedLock sl (m_specLock);
            for (int k = 0; k < kNumSpecBins; ++k)
            {
                const float re = m_fftWorkBuf[k * 2];
                const float im = m_fftWorkBuf[k * 2 + 1];
                outSpectrum[k] = std::sqrt (re * re + im * im) / (float)(kFftSize / 2);
            }
        }
    }
}

//==============================================================================
void PlugNspectrPostProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    if (m_pShared == nullptr)
        openSharedMemory();

    const int numCh  = juce::jmin (buffer.getNumChannels(), kPNS_MaxChannels);
    const int numSmp = juce::jmin (buffer.getNumSamples(),  kPNS_MaxSamplesPerBlock);

    // ── Capture pre/post audio blocks ─────────────────────────────────────
    {
        juce::ScopedLock sl (m_captureLock);

        if (m_pShared != nullptr && m_pShared->magic == kPNS_Magic)
        {
            const int preCh  = juce::jmin ((int) m_pShared->numChannels, kPNS_MaxChannels);
            const int preSmp = juce::jmin ((int) m_pShared->numSamples,  kPNS_MaxSamplesPerBlock);

            for (int ch = 0; ch < preCh; ++ch)
                std::memcpy (m_capture.pre.getWritePointer (ch),
                             m_pShared->preData[ch],
                             static_cast<size_t> (preSmp) * sizeof (float));
        }

        for (int ch = 0; ch < numCh; ++ch)
            std::memcpy (m_capture.post.getWritePointer (ch),
                         buffer.getReadPointer (ch),
                         static_cast<size_t> (numSmp) * sizeof (float));

        ++m_capture.captureCount;
    }

    // ── FFT accumulation ──────────────────────────────────────────────────
    pushSamplesToAccum (buffer.getReadPointer (0), numSmp,
                        m_postAccum, m_postAccumPos, m_postSpectrum);

    if (m_pShared != nullptr && m_pShared->magic == kPNS_Magic)
    {
        const int preSmp = juce::jmin ((int) m_pShared->numSamples,
                                       kPNS_MaxSamplesPerBlock);
        pushSamplesToAccum (m_pShared->preData[0], preSmp,
                            m_preAccum, m_preAccumPos, m_preSpectrum);
    }

    // ── RMS for dynamics display ───────────────────────────────────────────
    {
        // Post: channel 0
        float postSumSq = 0.0f;
        const float* postPtr = buffer.getReadPointer (0);
        for (int i = 0; i < numSmp; ++i)
            postSumSq += postPtr[i] * postPtr[i];
        const float postRms = std::sqrt (postSumSq / (float) numSmp);
        const float postDb  = 20.0f * std::log10 (juce::jmax (postRms, 1.0e-6f));

        float preDb    = -90.0f;
        bool  preValid = false;

        if (m_pShared != nullptr && m_pShared->magic == kPNS_Magic)
        {
            const int preSmp = juce::jmin ((int) m_pShared->numSamples,
                                           kPNS_MaxSamplesPerBlock);
            if (preSmp > 0)
            {
                float preSumSq = 0.0f;
                const float* prePtr = m_pShared->preData[0];
                for (int i = 0; i < preSmp; ++i)
                    preSumSq += prePtr[i] * prePtr[i];
                const float preRms = std::sqrt (preSumSq / (float) preSmp);
                preDb   = 20.0f * std::log10 (juce::jmax (preRms, 1.0e-6f));
                preValid = true;
            }
        }

        juce::ScopedLock sl (m_rmsLock);
        m_lastRms = { preDb, postDb, preValid };
    }

    // ── Post heartbeat — lets Pre editor detect that Post is running ──────
    if (m_pShared != nullptr)
        m_pShared->postLastHeartbeat = juce::Time::getMillisecondCounter();

}

//==============================================================================
PlugNspectrPostProcessor::CaptureBufs PlugNspectrPostProcessor::getCapture() const
{
    juce::ScopedLock sl (m_captureLock);
    return m_capture;
}

void PlugNspectrPostProcessor::getSpectra (
    std::array<float, kNumSpecBins>& outPre,
    std::array<float, kNumSpecBins>& outPost) const
{
    juce::ScopedLock sl (m_specLock);
    outPre  = m_preSpectrum;
    outPost = m_postSpectrum;
}

PlugNspectrPostProcessor::RmsPair PlugNspectrPostProcessor::getRms() const
{
    juce::ScopedLock sl (m_rmsLock);
    return m_lastRms;
}

//==============================================================================
juce::AudioProcessorEditor* PlugNspectrPostProcessor::createEditor()
{
    return new PlugNspectrPostEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PlugNspectrPostProcessor();
}
