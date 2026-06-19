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

    // Linear-measurement FFT (transfer function H = Post/Pre). Larger than the
    // spectrum FFT for finer low-frequency resolution of phase / group delay.
    static constexpr int kMeasFftOrder = 12;
    static constexpr int kMeasFftSize  = 1 << kMeasFftOrder;  // 4096
    static constexpr int kMeasBins     = kMeasFftSize / 2 + 1; // 2049

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

    //==========================================================================
    // Linear measurement — transfer function of the plugin(s) under analysis,
    // estimated as H1 = Sxy / Sxx (cross-spectrum of Pre→Post). Drives the
    // magnitude / phase / group-delay display on the Linear tab.
    struct MeasResult
    {
        std::array<float, kMeasBins> magDb {};   // 20·log10|H|
        std::array<float, kMeasBins> phase {};   // arg(H), wrapped radians
        std::array<float, kMeasBins> coh   {};   // coherence 0..1 (estimate trust)
        int    frames         = 0;               // averaged frames (0 = no data yet)
        int    latencySamples = 0;               // measured bulk Pre→Post delay
        double sampleRate     = 0.0;
    };
    void getMeasurement (MeasResult& out) const;
    void resetMeasurement ();

    // Test seam — feed a known Pre/Post pair straight into the measurement
    // accumulator (used by the offline render harness to validate the DSP).
    void injectMeasurementBlock (const float* pre, const float* post, int n);

    // Returns true if PlugNspectrPre has set a heartbeat within the last 500ms.
    bool isPreActive() const
    {
        if (m_testPreActive) return true;   // forced on by the offline render harness
        if (m_pShared == nullptr || m_pShared->magic != kPNS_Magic) return false;
        const uint32_t age = juce::Time::getMillisecondCounter()
                           - m_pShared->preLastHeartbeat;
        return age < 500u;
    }

    //==========================================================================
    // Test seam — lets the offline render harness (tools/render-harness) drive
    // the editor with synthetic audio, no DAW or live PlugNspectrPre needed.
    // Unused by the normal plugin runtime.
    void injectTestCapture (const juce::AudioBuffer<float>& pre,
                            const juce::AudioBuffer<float>& post,
                            float preDb, float postDb);
    void setTestPreActive (bool active) { m_testPreActive = active; }


private:
    //==========================================================================
    HANDLE m_hMapFile = nullptr;
    PNS_SharedBlock* m_pShared = nullptr;

    mutable juce::CriticalSection m_captureLock;
    CaptureBufs                   m_capture;
    bool                          m_testPreActive = false;   // forced by render harness

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

    //==========================================================================
    // Linear measurement engine — time-aligned Pre/Post frames → cross-spectrum.
    juce::dsp::FFT                       m_measFft    { kMeasFftOrder };
    juce::dsp::WindowingFunction<float>  m_measWindow { (size_t) kMeasFftSize,
                                         juce::dsp::WindowingFunction<float>::hann };

    std::array<float, kMeasFftSize>      m_measPreAccum  {};
    std::array<float, kMeasFftSize>      m_measPostAccum {};
    int                                  m_measPos = 0;
    std::array<float, 2 * kMeasFftSize>  m_measPreFft  {};
    std::array<float, 2 * kMeasFftSize>  m_measPostFft {};

    // Exponentially-averaged cross/auto spectra (double for numeric headroom).
    std::array<double, kMeasBins>        m_Sxx   {};   // |Pre|²
    std::array<double, kMeasBins>        m_Syy   {};   // |Post|²
    std::array<double, kMeasBins>        m_SxyRe {};   // Re{conj(Pre)·Post}
    std::array<double, kMeasBins>        m_SxyIm {};   // Im{conj(Pre)·Post}
    int                                  m_measFrames = 0;
    double                               m_measSampleRate = 0.0;
    mutable juce::CriticalSection        m_measLock;

    // Bulk Pre→Post latency (samples), estimated from the cross-spectrum so the
    // Linear view can de-rotate the pure-delay phase ramp.
    std::array<float, 2 * kMeasFftSize>  m_measIfft {};
    int                                  m_latencySamples = 0;

    // Pushes a block of time-aligned Pre/Post samples through the framed FFT,
    // accumulating the cross-spectrum once per full frame.
    void pushMeasurementSamples (const float* pre, const float* post, int n);

    // PHAT-weighted cross-spectrum → impulse response; its peak is the bulk
    // delay. Called under m_measLock from pushMeasurementSamples.
    void computeLatency();

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
