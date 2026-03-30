/*
  ==============================================================================
    PlugNspectrPost  –  PluginProcessor.h
  ==============================================================================
*/

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <JuceHeader.h>
#include "SharedMemoryBlock.h"

class PlugNspectrPostProcessor  : public juce::AudioProcessor
{
public:
    //==========================================================================
    // FFT parameters (public so the editor can size its arrays)
    static constexpr int kFftOrder    = 11;
    static constexpr int kFftSize     = 1 << kFftOrder;   // 2048
    static constexpr int kNumSpecBins = kFftSize / 2 + 1; // 1025

    //==========================================================================
    // Thread-safe snapshot of the most recent pre + post audio block.
    struct CaptureBufs
    {
        juce::AudioBuffer<float> pre;
        juce::AudioBuffer<float> post;
        uint32_t                 captureCount = 0;
    };

    // Per-block RMS levels, published for the dynamics display.
    struct RmsPair
    {
        float preDb   = -90.0f;
        float postDb  = -90.0f;
        bool  preValid = false;  // false when PlugNspectrPre is not connected
    };

    //==========================================================================
    PlugNspectrPostProcessor();
    ~PlugNspectrPostProcessor() override;

    //==========================================================================
    void prepareToPlay   (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    bool                        hasEditor()    const override { return true; }
    juce::AudioProcessorEditor* createEditor()       override;

    //==========================================================================
    const juce::String getName() const override { return JucePlugin_Name; }

    bool   acceptsMidi()           const override { return false; }
    bool   producesMidi()          const override { return false; }
    bool   isMidiEffect()          const override { return false; }
    double getTailLengthSeconds()  const override { return 0.0;   }

    int                getNumPrograms()                              override { return 1;  }
    int                getCurrentProgram()                           override { return 0;  }
    void               setCurrentProgram (int)                       override {}
    const juce::String getProgramName    (int)                       override { return {}; }
    void               changeProgramName (int, const juce::String&)  override {}

    void getStateInformation (juce::MemoryBlock&)    override {}
    void setStateInformation (const void*, int)      override {}

    //==========================================================================
    // Editor API — all called from the message thread at ~30 fps.

    // Returns a copy of the current capture buffers (pre + post audio blocks).
    CaptureBufs getCapture() const;

    // Fills outPre / outPost with the latest FFT magnitude spectra (linear scale).
    void getSpectra (std::array<float, kNumSpecBins>& outPre,
                     std::array<float, kNumSpecBins>& outPost) const;

    // Returns the most recent per-block RMS levels for the dynamics display.
    RmsPair getRms() const;

    // Returns true if PlugNspectrPre has set a heartbeat within the last 500ms.
    bool isPreActive() const
    {
        if (m_pShared == nullptr || m_pShared->magic != kPNS_Magic) return false;
        const uint32_t age = juce::Time::getMillisecondCounter()
                           - m_pShared->preLastHeartbeat;
        return age < 500u;
    }


private:
    //==========================================================================
    HANDLE m_hMapFile = nullptr;
    PNS_SharedBlock* m_pShared = nullptr;

    mutable juce::CriticalSection m_captureLock;
    CaptureBufs                   m_capture;

    void openSharedMemory();
    void closeSharedMemory();

    //==========================================================================
    // FFT
    juce::dsp::FFT                       m_fft    { kFftOrder };
    juce::dsp::WindowingFunction<float>  m_window { (size_t) kFftSize,
                                         juce::dsp::WindowingFunction<float>::hann };

    std::array<float, kFftSize>          m_postAccum {};
    std::array<float, kFftSize>          m_preAccum  {};
    int                                  m_postAccumPos = 0;
    int                                  m_preAccumPos  = 0;
    std::array<float, 2 * kFftSize>      m_fftWorkBuf {};

    std::array<float, kNumSpecBins>      m_postSpectrum {};
    std::array<float, kNumSpecBins>      m_preSpectrum  {};
    mutable juce::CriticalSection        m_specLock;

    void pushSamplesToAccum (const float* src, int count,
                             std::array<float, kFftSize>& accum,
                             int& pos,
                             std::array<float, kNumSpecBins>& outSpectrum);

    //==========================================================================
    // RMS (dynamics display)
    RmsPair                       m_lastRms;
    mutable juce::CriticalSection m_rmsLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlugNspectrPostProcessor)
};
