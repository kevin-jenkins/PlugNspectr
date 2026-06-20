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

    // Dynamics transfer-curve measurement (output level vs input level), binned
    // over a -60..0 dBFS input range at 0.5 dB resolution.
    static constexpr float kDynMinDb = -60.0f;
    static constexpr float kDynBinW  = 0.5f;
    static constexpr int   kDynBins  = 121;

    // Attack/release envelope: gain reduction vs time, synchronously averaged
    // over a 1 s level-step cycle (250 bins → 4 ms resolution).
    static constexpr int   kEnvBins  = 250;

    // THD-vs-frequency sweep: %THD measured per log-frequency bin (50 Hz..5 kHz).
    static constexpr int   kThdBins = 120;
    static constexpr float kThdLoHz = 50.0f;
    static constexpr float kThdHiHz = 5000.0f;

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

    // Channel mode for all analysis: 0=Left, 1=Right, 2=Mid (L+R)/2, 3=Side (L-R)/2.
    void setChannelMode (int m) { m_channelMode.store (juce::jlimit (0, 3, m)); }
    int  getChannelMode () const { return m_channelMode.load(); }

    // Test seam — feed a known Pre/Post pair straight into the measurement
    // accumulator (used by the offline render harness to validate the DSP).
    void injectMeasurementBlock (const float* pre, const float* post, int n);

    //==========================================================================
    // Dynamics transfer curve — output level vs input level, measured by Pre
    // sweeping a tone's level and Post binning Post-RMS against Pre-RMS.
    struct DynResult
    {
        std::array<float, kDynBins> outDb {};   // measured output dB per input bin
        std::array<bool,  kDynBins> valid {};
        double sampleRate = 0.0;
    };
    void getDynamics (DynResult& out) const;
    void resetDynamics ();
    void injectDynamicsBlock (const float* pre, const float* post, int n);

    //==========================================================================
    // Attack/release envelope — gain reduction (Pre−Post level) vs time, phase-
    // aligned to Pre's level-step cycle and synchronously averaged.
    struct EnvResult
    {
        std::array<float, kEnvBins> grDb  {};   // gain reduction (dB) per time bin
        std::array<bool,  kEnvBins> valid {};
        double sampleRate = 0.0;
    };
    void getEnvelope (EnvResult& out) const;
    void resetEnvelope ();
    void injectEnvelopeBlock (const float* pre, const float* post, int n, uint32_t envPosAtStart);

    //==========================================================================
    // THD vs frequency — total harmonic distortion of the Post signal measured
    // per log-frequency bin as Pre sweeps a tone across the spectrum.
    struct ThdResult
    {
        std::array<float, kThdBins> thdPct {};   // %THD per frequency bin
        std::array<bool,  kThdBins> valid  {};
        double sampleRate = 0.0;
    };
    void getThdSweep (ThdResult& out) const;
    void resetThdSweep ();
    void injectThdSweepBlock (const float* post, int n, double fundamentalHz);

    // Returns true if PlugNspectrPre has set a heartbeat within the last 500ms.
    bool isPreActive() const
    {
        if (m_testPreActive) return true;   // forced on by the offline render harness
        if (m_pShared == nullptr || m_pShared->magic != kPNS_Magic) return false;
        const uint32_t age = juce::Time::getMillisecondCounter()
                           - m_pShared->preLastHeartbeat;
        return age < 500u;
    }

    // Host transport state — used by the editor to auto-stop a measurement
    // stimulus when playback stops (the test signal replaces your audio, so it
    // shouldn't keep running once you stop the transport). Defaults to "playing"
    // so hosts that don't report a playhead never trigger a false auto-stop.
    bool isTransportPlaying() const { return m_transportPlaying.load(); }

    //==========================================================================
    // Test seam — lets the offline render harness (tools/render-harness) drive
    // the editor with synthetic audio, no DAW or live PlugNspectrPre needed.
    // Unused by the normal plugin runtime.
    void injectTestCapture (const juce::AudioBuffer<float>& pre,
                            const juce::AudioBuffer<float>& post,
                            float preDb, float postDb);
    void setTestPreActive (bool active) { m_testPreActive = active; }
    void setTestTransportPlaying (bool p) { m_transportPlaying.store (p); }


private:
    //==========================================================================
    HANDLE m_hMapFile = nullptr;
    PNS_SharedBlock* m_pShared = nullptr;

    mutable juce::CriticalSection m_captureLock;
    CaptureBufs                   m_capture;
    bool                          m_testPreActive = false;   // forced by render harness
    std::atomic<bool>             m_transportPlaying { true };

    // L/R/Mid/Side channel selection + derived per-block analysis signals.
    std::atomic<int>                            m_channelMode { 0 };
    std::array<float, kPNS_MaxSamplesPerBlock>  m_anaPre  {};
    std::array<float, kPNS_MaxSamplesPerBlock>  m_anaPost {};

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

    //==========================================================================
    // Dynamics transfer-curve engine — windowed RMS of Pre/Post, binned by input.
    static constexpr int                 kDynWin = 2048;
    double                               m_dynSumSqPre  = 0.0;
    double                               m_dynSumSqPost = 0.0;
    int                                  m_dynWinCount  = 0;
    std::array<float,   kDynBins>        m_dynOutDb {};
    std::array<uint8_t, kDynBins>        m_dynValid {};
    mutable juce::CriticalSection        m_dynLock;

    void pushDynamicsSamples (const float* pre, const float* post, int n);

    //==========================================================================
    // Attack/release envelope engine — short-window GR binned by cycle phase.
    static constexpr int                 kEnvWin = 128;   // level-follower window
    double                               m_envSumSqPre  = 0.0;
    double                               m_envSumSqPost = 0.0;
    int                                  m_envWinCount  = 0;
    uint32_t                             m_envPos = 0;     // current cycle position (samples)
    std::array<float,   kEnvBins>        m_envGrDb {};
    std::array<uint8_t, kEnvBins>        m_envValid {};
    mutable juce::CriticalSection        m_envLock;

    void pushEnvelopeSamples (const float* pre, const float* post, int n, uint32_t envPosAtStart);

    //==========================================================================
    // THD-vs-frequency engine — frame the Post signal, FFT, pick harmonic peaks.
    std::array<float, kMeasFftSize>      m_thdAccum {};
    int                                  m_thdPos = 0;
    std::array<float, 2 * kMeasFftSize>  m_thdWork {};
    double                               m_thdLastFundHz = 0.0;
    std::array<float,   kThdBins>        m_thdPct {};
    std::array<uint8_t, kThdBins>        m_thdValid {};
    mutable juce::CriticalSection        m_thdLock;

    void pushThdSweepSamples (const float* post, int n, double fundamentalHz);

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
