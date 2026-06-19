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
void PlugNspectrPostProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    openSharedMemory();
    m_measSampleRate = sampleRate;

    const int safeSamples = juce::jmax (samplesPerBlock, kPNS_MaxSamplesPerBlock);

    {
        juce::ScopedLock sl (m_captureLock);
        m_capture.pre .setSize (kPNS_MaxChannels, safeSamples, false, true, false);
        m_capture.post.setSize (kPNS_MaxChannels, safeSamples, false, true, false);
    }

    resetMeasurement();
    resetDynamics();
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
// Linear measurement: frame time-aligned Pre/Post, FFT both, and accumulate the
// cross-spectrum (H1 estimator) with exponential averaging.
void PlugNspectrPostProcessor::pushMeasurementSamples (const float* pre,
                                                       const float* post, int n)
{
    if (pre == nullptr || post == nullptr) return;

    // Exponential forgetting — converges in ~1 s at typical frame rates while
    // still tracking parameter changes on the plugin under analysis.
    constexpr double kDecay = 0.9;

    for (int i = 0; i < n; ++i)
    {
        m_measPreAccum [m_measPos] = pre [i];
        m_measPostAccum[m_measPos] = post[i];
        ++m_measPos;

        if (m_measPos < kMeasFftSize) continue;
        m_measPos = 0;

        std::copy (m_measPreAccum.begin(),  m_measPreAccum.end(),  m_measPreFft.begin());
        std::copy (m_measPostAccum.begin(), m_measPostAccum.end(), m_measPostFft.begin());
        std::fill (m_measPreFft.begin()  + kMeasFftSize, m_measPreFft.end(),  0.0f);
        std::fill (m_measPostFft.begin() + kMeasFftSize, m_measPostFft.end(), 0.0f);

        m_measWindow.multiplyWithWindowingTable (m_measPreFft.data(),  kMeasFftSize);
        m_measWindow.multiplyWithWindowingTable (m_measPostFft.data(), kMeasFftSize);
        m_measFft.performRealOnlyForwardTransform (m_measPreFft.data(),  true);
        m_measFft.performRealOnlyForwardTransform (m_measPostFft.data(), true);

        juce::ScopedLock sl (m_measLock);
        for (int k = 0; k < kMeasBins; ++k)
        {
            const double pr = m_measPreFft [k * 2],     pi = m_measPreFft [k * 2 + 1];
            const double qr = m_measPostFft[k * 2],     qi = m_measPostFft[k * 2 + 1];

            const double sxx = pr * pr + pi * pi;
            const double syy = qr * qr + qi * qi;
            const double sxyRe = pr * qr + pi * qi;   // Re{conj(P)·Q}
            const double sxyIm = pr * qi - pi * qr;   // Im{conj(P)·Q}

            m_Sxx  [k] = m_Sxx  [k] * kDecay + sxx   * (1.0 - kDecay);
            m_Syy  [k] = m_Syy  [k] * kDecay + syy   * (1.0 - kDecay);
            m_SxyRe[k] = m_SxyRe[k] * kDecay + sxyRe * (1.0 - kDecay);
            m_SxyIm[k] = m_SxyIm[k] * kDecay + sxyIm * (1.0 - kDecay);
        }
        m_measFrames = juce::jmin (m_measFrames + 1, 1 << 20);

        // Re-estimate bulk latency periodically (cheap; reuses m_measFft).
        if (m_measFrames % 8 == 0) computeLatency();
    }
}

void PlugNspectrPostProcessor::computeLatency()
{
    // PHAT (phase-transform) weighting → unit-magnitude cross-spectrum, whose
    // inverse FFT is a sharp correlation peak at the bulk delay.
    for (int k = 0; k <= kMeasFftSize / 2; ++k)
    {
        const double mag = std::sqrt (m_SxyRe[k] * m_SxyRe[k] + m_SxyIm[k] * m_SxyIm[k]);
        if (mag > 1.0e-12)
        {
            m_measIfft[k * 2]     = (float) (m_SxyRe[k] / mag);
            m_measIfft[k * 2 + 1] = (float) (m_SxyIm[k] / mag);
        }
        else { m_measIfft[k * 2] = 0.0f; m_measIfft[k * 2 + 1] = 0.0f; }
    }

    m_measFft.performRealOnlyInverseTransform (m_measIfft.data());

    int   peak = 0;
    float best = -1.0f;
    for (int n = 0; n < kMeasFftSize; ++n)
    {
        const float v = std::abs (m_measIfft[n]);
        if (v > best) { best = v; peak = n; }
    }
    // Indices past N/2 are negative lags; Post lagging Pre (causal) is positive.
    m_latencySamples = (peak <= kMeasFftSize / 2) ? peak : peak - kMeasFftSize;
}

void PlugNspectrPostProcessor::getMeasurement (MeasResult& out) const
{
    juce::ScopedLock sl (m_measLock);
    out.frames         = m_measFrames;
    out.latencySamples = m_latencySamples;
    out.sampleRate     = (m_measSampleRate > 0.0) ? m_measSampleRate : getSampleRate();

    for (int k = 0; k < kMeasBins; ++k)
    {
        const double sxx = m_Sxx[k];
        if (sxx > 1.0e-20)
        {
            const double hre = m_SxyRe[k] / sxx;          // H = Sxy / Sxx
            const double him = m_SxyIm[k] / sxx;
            const double mag = std::sqrt (hre * hre + him * him);
            out.magDb[k] = 20.0f * std::log10 ((float) juce::jmax (mag, 1.0e-9));
            out.phase[k] = std::atan2 ((float) him, (float) hre);

            const double sxy2 = m_SxyRe[k] * m_SxyRe[k] + m_SxyIm[k] * m_SxyIm[k];
            const double den  = sxx * m_Syy[k];
            out.coh[k] = (den > 1.0e-20) ? (float) juce::jlimit (0.0, 1.0, sxy2 / den) : 0.0f;
        }
        else
        {
            out.magDb[k] = -120.0f;
            out.phase[k] = 0.0f;
            out.coh  [k] = 0.0f;
        }
    }
}

void PlugNspectrPostProcessor::resetMeasurement()
{
    juce::ScopedLock sl (m_measLock);
    m_Sxx.fill (0.0); m_Syy.fill (0.0); m_SxyRe.fill (0.0); m_SxyIm.fill (0.0);
    m_measFrames     = 0;
    m_measPos        = 0;
    m_latencySamples = 0;
}

void PlugNspectrPostProcessor::injectMeasurementBlock (const float* pre, const float* post, int n)
{
    pushMeasurementSamples (pre, post, n);
}

//==============================================================================
// Dynamics transfer curve: windowed RMS of Pre (input) and Post (output), with
// each window's output level binned against its input level.
void PlugNspectrPostProcessor::pushDynamicsSamples (const float* pre, const float* post, int n)
{
    if (pre == nullptr || post == nullptr) return;
    constexpr double kDecay = 0.9;

    for (int i = 0; i < n; ++i)
    {
        m_dynSumSqPre  += (double) pre [i] * pre [i];
        m_dynSumSqPost += (double) post[i] * post[i];

        if (++m_dynWinCount < kDynWin) continue;

        const double preRms  = std::sqrt (m_dynSumSqPre  / kDynWin);
        const double postRms = std::sqrt (m_dynSumSqPost / kDynWin);
        m_dynSumSqPre = m_dynSumSqPost = 0.0;
        m_dynWinCount = 0;

        const double inDb  = 20.0 * std::log10 (juce::jmax (preRms,  1.0e-7));
        const double outDb = 20.0 * std::log10 (juce::jmax (postRms, 1.0e-7));
        if (inDb < kDynMinDb || inDb > 0.0) continue;

        const int b = juce::jlimit (0, kDynBins - 1,
                                    (int) std::lround ((inDb - kDynMinDb) / kDynBinW));

        juce::ScopedLock sl (m_dynLock);
        if (m_dynValid[b]) m_dynOutDb[b] = (float) (m_dynOutDb[b] * kDecay + outDb * (1.0 - kDecay));
        else             { m_dynOutDb[b] = (float) outDb; m_dynValid[b] = 1; }
    }
}

void PlugNspectrPostProcessor::getDynamics (DynResult& out) const
{
    juce::ScopedLock sl (m_dynLock);
    out.sampleRate = (m_measSampleRate > 0.0) ? m_measSampleRate : getSampleRate();
    for (int b = 0; b < kDynBins; ++b)
    {
        out.outDb[b] = m_dynOutDb[b];
        out.valid[b] = (m_dynValid[b] != 0);
    }
}

void PlugNspectrPostProcessor::resetDynamics()
{
    juce::ScopedLock sl (m_dynLock);
    m_dynOutDb.fill (0.0f);
    m_dynValid.fill (0);
    m_dynSumSqPre = m_dynSumSqPost = 0.0;
    m_dynWinCount = 0;
}

void PlugNspectrPostProcessor::injectDynamicsBlock (const float* pre, const float* post, int n)
{
    pushDynamicsSamples (pre, post, n);
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

        // ── Linear transfer-function measurement (Pre → Post) ─────────────
        const int measN = juce::jmin (preSmp, numSmp);
        pushMeasurementSamples (m_pShared->preData[0],
                                buffer.getReadPointer (0), measN);

        // ── Dynamics transfer-curve measurement (input vs output level) ───
        pushDynamicsSamples (m_pShared->preData[0],
                             buffer.getReadPointer (0), measN);
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

void PlugNspectrPostProcessor::injectTestCapture (const juce::AudioBuffer<float>& pre,
                                                  const juce::AudioBuffer<float>& post,
                                                  float preDb, float postDb)
{
    {
        juce::ScopedLock sl (m_captureLock);
        m_capture.pre  = pre;
        m_capture.post = post;
        ++m_capture.captureCount;
    }
    {
        juce::ScopedLock sl (m_rmsLock);
        m_lastRms = { preDb, postDb, true };
    }
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
