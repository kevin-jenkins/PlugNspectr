/*
  ==============================================================================
    PlugNspectrPost  –  PluginProcessor.cpp
  ==============================================================================
*/

#include "PluginProcessor.h"
#ifndef PNS_HEADLESS_TESTS          // editor-free in the unit-test target
#include "PluginEditor.h"
#endif

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
    m_ipc.close();
}

//==============================================================================
void PlugNspectrPostProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    m_ipc.open();          // create-or-open the IPC segment, off the audio thread
    m_measSampleRate = sampleRate;

    const int safeSamples = juce::jmax (samplesPerBlock, pns::kMaxSamples);

    {
        juce::ScopedLock sl (m_captureLock);
        // 3 channels: 0 = derived analysis signal (waveform / oscilloscope),
        // 1 = raw L, 2 = raw R (goniometer on the Stereo tab).
        m_capture.pre .setSize (kCaptureChannels, safeSamples, false, true, false);
        m_capture.post.setSize (kCaptureChannels, safeSamples, false, true, false);
    }

    resetMeasurement();
    resetDynamics();
    resetEnvelope();
    {   // full wipe on (re)prepare — no ghost carried across a sample-rate change
        juce::ScopedLock sl (m_thdLock);
        m_thdPct.fill (0.0f); m_thdValid.fill (0); m_thdFresh.fill (0); m_thdPos = 0;
    }
    resetStereo();
    m_stPrePos = m_stPostPos = 0;
}

void PlugNspectrPostProcessor::releaseResources()
{
    m_ipc.close();
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
// Attack/release envelope: short-window gain reduction (in dB) binned by the
// Pre stimulus's cycle phase, so repeated step cycles synchronously average.
void PlugNspectrPostProcessor::pushEnvelopeSamples (const float* pre, const float* post,
                                                    int n, uint32_t envPosAtStart)
{
    if (pre == nullptr || post == nullptr) return;
    const double sr = (m_measSampleRate > 0.0) ? m_measSampleRate : getSampleRate();
    if (sr <= 0.0) return;

    const uint32_t period = juce::jmax (1u, (uint32_t) sr);   // 1 s cycle
    constexpr double kDecay = 0.85;
    m_envPos = envPosAtStart % period;

    for (int i = 0; i < n; ++i)
    {
        m_envSumSqPre  += (double) pre [i] * pre [i];
        m_envSumSqPost += (double) post[i] * post[i];
        if (++m_envPos >= period) m_envPos = 0;

        if (++m_envWinCount < kEnvWin) continue;

        const double preRms  = std::sqrt (m_envSumSqPre  / kEnvWin);
        const double postRms = std::sqrt (m_envSumSqPost / kEnvWin);
        m_envSumSqPre = m_envSumSqPost = 0.0;
        m_envWinCount = 0;

        const double inDb  = 20.0 * std::log10 (juce::jmax (preRms,  1.0e-7));
        const double outDb = 20.0 * std::log10 (juce::jmax (postRms, 1.0e-7));
        if (inDb < -70.0) continue;                 // no stimulus present
        const double gr = inDb - outDb;             // gain reduction (dB, +)

        const int b = juce::jlimit (0, kEnvBins - 1,
                                    (int) ((uint64_t) m_envPos * kEnvBins / period));

        juce::ScopedLock sl (m_envLock);
        if (m_envValid[b]) m_envGrDb[b] = (float) (m_envGrDb[b] * kDecay + gr * (1.0 - kDecay));
        else             { m_envGrDb[b] = (float) gr; m_envValid[b] = 1; }
    }
}

void PlugNspectrPostProcessor::getEnvelope (EnvResult& out) const
{
    juce::ScopedLock sl (m_envLock);
    out.sampleRate = (m_measSampleRate > 0.0) ? m_measSampleRate : getSampleRate();
    for (int b = 0; b < kEnvBins; ++b)
    {
        out.grDb[b]  = m_envGrDb[b];
        out.valid[b] = (m_envValid[b] != 0);
    }
}

void PlugNspectrPostProcessor::resetEnvelope()
{
    juce::ScopedLock sl (m_envLock);
    m_envGrDb.fill (0.0f);
    m_envValid.fill (0);
    m_envSumSqPre = m_envSumSqPost = 0.0;
    m_envWinCount = 0;
    m_envPos = 0;
}

void PlugNspectrPostProcessor::injectEnvelopeBlock (const float* pre, const float* post,
                                                    int n, uint32_t envPosAtStart)
{
    pushEnvelopeSamples (pre, post, n, envPosAtStart);
}

//==============================================================================
// THD vs frequency: frame the Post signal, FFT, pick the fundamental + harmonic
// peaks, and bin %THD by the (swept) fundamental's log-frequency.
void PlugNspectrPostProcessor::pushThdSweepSamples (const float* post, int n, double fundamentalHz)
{
    if (post == nullptr) return;
    const double sr = (m_measSampleRate > 0.0) ? m_measSampleRate : getSampleRate();
    if (sr <= 0.0) return;

    // The sweep holds each frequency for ~0.15 s but the FFT frame is shorter, so
    // unless we realign, a frame can straddle a step boundary and mix two tones —
    // smearing the harmonic search and reading garbage (near-zero at high freq,
    // which slams the curve into the grid floor). Drop the partial frame whenever
    // the swept fundamental changes, so every frame holds a single pure tone.
    if (fundamentalHz != m_thdLastFundHz)
    {
        m_thdPos = 0;
        m_thdLastFundHz = fundamentalHz;
    }

    for (int i = 0; i < n; ++i)
    {
        m_thdAccum[m_thdPos++] = post[i];
        if (m_thdPos < kMeasFftSize) continue;
        m_thdPos = 0;

        const double f0 = m_thdLastFundHz;
        if (f0 < (double) kThdLoHz * 0.9 || f0 > (double) kThdHiHz * 1.1) continue;

        std::copy (m_thdAccum.begin(), m_thdAccum.end(), m_thdWork.begin());
        std::fill (m_thdWork.begin() + kMeasFftSize, m_thdWork.end(), 0.0f);
        m_measWindow.multiplyWithWindowingTable (m_thdWork.data(), kMeasFftSize);
        m_measFft.performRealOnlyForwardTransform (m_thdWork.data(), true);

        const double binW = sr / kMeasFftSize;
        // Search radius must stay below half the harmonic spacing (f0 in bins),
        // or at low frequencies H2's window would grab the fundamental.
        const int rad = juce::jlimit (1, 6, (int) (f0 / binW / 2.0) - 1);
        auto peak = [&] (double freq) -> double
        {
            const int b  = (int) std::lround (freq / binW);
            const int lo = juce::jmax (1, b - rad);
            const int hi = juce::jmin (kMeasBins - 1, b + rad);
            double pk = 0.0;
            for (int k = lo; k <= hi; ++k)
            {
                const double re = m_thdWork[k * 2], im = m_thdWork[k * 2 + 1];
                pk = juce::jmax (pk, std::sqrt (re * re + im * im));
            }
            return pk;
        };

        const double h1 = peak (f0);
        if (h1 < 1.0e-6) continue;
        double sumSq = 0.0;
        for (int h = 2; h <= 8; ++h)
        {
            const double fh = f0 * h;
            if (fh > sr * 0.5) break;
            const double hh = peak (fh);
            sumSq += hh * hh;
        }
        const double thd = 100.0 * std::sqrt (sumSq) / h1;

        const double t = std::log (f0 / kThdLoHz) / std::log ((double) kThdHiHz / kThdLoHz);
        const int tb = juce::jlimit (0, kThdBins - 1, (int) std::lround (t * (kThdBins - 1)));

        juce::ScopedLock sl (m_thdLock);
        if (m_thdFresh[tb]) m_thdPct[tb] = (float) (m_thdPct[tb] * 0.8 + thd * 0.2);  // smooth within cycle
        else                m_thdPct[tb] = (float) thd;   // first hit this cycle: overwrite the ghost raw
        m_thdFresh[tb] = 1;
        m_thdValid[tb] = 1;
    }
}

void PlugNspectrPostProcessor::getThdSweep (ThdResult& out) const
{
    juce::ScopedLock sl (m_thdLock);
    out.sampleRate = (m_measSampleRate > 0.0) ? m_measSampleRate : getSampleRate();
    for (int b = 0; b < kThdBins; ++b)
    {
        out.thdPct[b] = m_thdPct[b];
        out.valid[b]  = (m_thdValid[b] != 0);
        out.fresh[b]  = (m_thdFresh[b] != 0);
    }
}

void PlugNspectrPostProcessor::startThdSweepCycle()
{
    // Begin a new sweep cycle: clear only the "fresh" flags so the bright curve
    // rebuilds from scratch, while m_thdPct/m_thdValid stay as the dimmed ghost
    // (the previous sweep) that the new sweep overwrites bin-by-bin, low→high.
    // m_thdPos is owned solely by the audio thread (pushThdSweepSamples) — its
    // realign-on-fundamental-change zeroes it, and the sweep restarts at 50 Hz on
    // re-arm, so the first injected block resets it there. Don't race it from here.
    juce::ScopedLock sl (m_thdLock);
    m_thdFresh.fill (0);
}

void PlugNspectrPostProcessor::injectThdSweepBlock (const float* post, int n, double fundamentalHz)
{
    pushThdSweepSamples (post, n, fundamentalHz);
}

//==============================================================================
void PlugNspectrPostProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    // Track host transport so the editor can auto-stop a measurement stimulus
    // when playback stops. Leave the last value if the host reports no playhead.
    if (auto* ph = getPlayHead())
        if (const auto pos = ph->getPosition())
            m_transportPlaying.store (pos->getIsPlaying());

    const int numCh  = juce::jmin (buffer.getNumChannels(), pns::kMaxChannels);
    const int numSmp = juce::jmin (buffer.getNumSamples(),  pns::kMaxSamples);

    // Pull the latest Pre block (torn-free via the seqlock); valid only if Pre is live.
    const bool preReady = isPreActive() && m_ipc.readBlock (m_preIn);
    const int  preCh    = preReady ? juce::jmin (m_preIn.numChannels, pns::kMaxChannels) : 0;
    const int  preSmp   = preReady ? juce::jmin (m_preIn.numSamples,  pns::kMaxSamples)  : 0;

    // ── Derive the analysis channel (L / R / Mid / Side) once, then feed it
    //    to every downstream measurement and the capture display ───────────
    {
        const int mode = m_channelMode.load();
        auto derive = [mode] (float l, float r) -> float
        {
            return (mode == 1) ? 0.5f * (l - r)     // Side — stereo difference (L-R)/2
                               : 0.5f * (l + r);    // L+R  — both channels combined (L+R)/2 [default]
        };

        const float* post0 = (numCh > 0) ? buffer.getReadPointer (0) : nullptr;
        const float* post1 = (numCh > 1) ? buffer.getReadPointer (1) : post0;
        for (int i = 0; i < numSmp; ++i)
            m_anaPost[i] = (post0 != nullptr) ? derive (post0[i], post1[i]) : 0.0f;

        const float* pre0 = preReady ? m_preIn.preData[0] : nullptr;
        const float* pre1 = (preReady && preCh > 1) ? m_preIn.preData[1] : pre0;
        for (int i = 0; i < preSmp; ++i)
            m_anaPre[i] = (pre0 != nullptr) ? derive (pre0[i], pre1[i]) : 0.0f;

        // ── Stereo image analysis ─────────────────────────────────────────────
        // Reuses the raw L/R pointers above. Costs four extra FFTs per frame, so
        // it only runs while the Stereo tab is showing (set from switchTab).
        m_stereoSource.store (numCh > 1 && preCh > 1);
        if (m_stereoActive.load())
        {
            if (post0 != nullptr)
                pushStereoSamples (post0, post1, numSmp, m_stPostL, m_stPostR,
                                   m_stPostPos, m_stPostAcc, m_stBbPostLive, m_stBbPost);
            if (pre0 != nullptr)
                pushStereoSamples (pre0, pre1, preSmp, m_stPreL, m_stPreR,
                                   m_stPrePos, m_stPreAcc, m_stBbPreLive, m_stBbPre);
        }
    }

    // ── Capture pre/post audio blocks ─────────────────────────────────────
    {
        juce::ScopedLock sl (m_captureLock);

        // Channel 0 of the capture holds the derived analysis signal so the
        // waveform / oscilloscope views follow the L/R/Mid/Side selection.
        // Channels 1/2 carry raw L/R for the Stereo tab's goniometer, which
        // needs the untouched pair rather than the derived mono signal.
        if (preSmp > 0)
            std::memcpy (m_capture.pre.getWritePointer (0), m_anaPre.data(),
                         static_cast<size_t> (preSmp) * sizeof (float));

        std::memcpy (m_capture.post.getWritePointer (0), m_anaPost.data(),
                     static_cast<size_t> (numSmp) * sizeof (float));

        {
            const float* post0 = (numCh > 0) ? buffer.getReadPointer (0) : nullptr;
            const float* post1 = (numCh > 1) ? buffer.getReadPointer (1) : post0;
            if (post0 != nullptr)
            {
                const auto n = static_cast<size_t> (numSmp) * sizeof (float);
                std::memcpy (m_capture.post.getWritePointer (kCapL), post0, n);
                std::memcpy (m_capture.post.getWritePointer (kCapR), post1, n);
            }
            if (preSmp > 0)
            {
                const float* pre0 = m_preIn.preData[0];
                const float* pre1 = (preCh > 1) ? m_preIn.preData[1] : pre0;
                const auto n = static_cast<size_t> (preSmp) * sizeof (float);
                std::memcpy (m_capture.pre.getWritePointer (kCapL), pre0, n);
                std::memcpy (m_capture.pre.getWritePointer (kCapR), pre1, n);
            }
        }

        ++m_capture.captureCount;
    }

    // ── FFT accumulation (on the derived analysis channel) ────────────────
    pushSamplesToAccum (m_anaPost.data(), numSmp,
                        m_postAccum, m_postAccumPos, m_postSpectrum);

    if (preReady)
    {
        pushSamplesToAccum (m_anaPre.data(), preSmp,
                            m_preAccum, m_preAccumPos, m_preSpectrum);

        // ── Linear transfer-function measurement (Pre → Post) ─────────────
        // Only while the noise stimulus is armed, so pausing Measure holds the
        // last curve instead of drifting onto program material.
        const int measN = juce::jmin (preSmp, numSmp);
        if (m_linearMeasuring.load())
            pushMeasurementSamples (m_anaPre.data(), m_anaPost.data(), measN);

        // ── Dynamics engines — gated by Pre's active stimulus mode so each
        //    only runs (and interprets dynEnvPos) when its stimulus is live ──
        const uint32_t dynMode = m_preIn.dynModeActive;
        if (dynMode == 1)
            pushDynamicsSamples (m_anaPre.data(), m_anaPost.data(), measN);
        else if (dynMode == 2)
            pushEnvelopeSamples (m_anaPre.data(), m_anaPost.data(), measN, m_preIn.dynEnvPos);
        else if (dynMode == 3)
            pushThdSweepSamples (m_anaPost.data(), measN, (double) m_preIn.dynEnvPos);
    }

    // ── RMS for dynamics display (on the derived analysis channel) ─────────
    {
        float postSumSq = 0.0f;
        for (int i = 0; i < numSmp; ++i)
            postSumSq += m_anaPost[i] * m_anaPost[i];
        const float postRms = std::sqrt (postSumSq / (float) juce::jmax (numSmp, 1));
        const float postDb  = 20.0f * std::log10 (juce::jmax (postRms, 1.0e-6f));

        float preDb    = -90.0f;
        bool  preValid = false;

        if (preSmp > 0)
        {
            float preSumSq = 0.0f;
            for (int i = 0; i < preSmp; ++i)
                preSumSq += m_anaPre[i] * m_anaPre[i];
            const float preRms = std::sqrt (preSumSq / (float) preSmp);
            preDb   = 20.0f * std::log10 (juce::jmax (preRms, 1.0e-6f));
            preValid = true;
        }

        juce::ScopedLock sl (m_rmsLock);
        m_lastRms = { preDb, postDb, preValid };
    }

    // ── Post heartbeat — lets Pre editor detect that Post is running ──────
    m_ipc.postHeartbeat();
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
// Stereo engine. See StereoResult in the header for the maths; in short we FFT L
// and R, then accumulate |L|², |R|² and Re{L·conj(R)} per bin. Width and
// correlation both derive from those three exactly.
void PlugNspectrPostProcessor::accumulateStereoFrame (
    const std::array<float, kFftSize>& l,
    const std::array<float, kFftSize>& r,
    StereoAccum& acc,
    const StereoBroadband& bbLive, StereoBroadband& bbPub)
{
    auto forward = [this] (const std::array<float, kFftSize>& src,
                           std::array<float, 2 * kFftSize>& work)
    {
        std::copy (src.begin(), src.end(), work.begin());
        std::fill (work.begin() + kFftSize, work.end(), 0.0f);
        m_window.multiplyWithWindowingTable (work.data(), kFftSize);
        m_fft.performRealOnlyForwardTransform (work.data(), true);
    };

    forward (l, m_stWorkA);
    forward (r, m_stWorkB);

    // Exponential forgetting so the display tracks the material instead of
    // averaging the whole session. ~2 s at 2048-sample frames.
    constexpr double kDecay = 0.90;

    juce::ScopedLock sl (m_stereoLock);
    for (int k = 0; k < kNumSpecBins; ++k)
    {
        const double lr_ = m_stWorkA[k * 2], li = m_stWorkA[k * 2 + 1];
        const double rr_ = m_stWorkB[k * 2], ri = m_stWorkB[k * 2 + 1];

        const double sll   = lr_ * lr_ + li * li;
        const double srr   = rr_ * rr_ + ri * ri;
        const double slrRe = lr_ * rr_ + li * ri;      // Re{L·conj(R)}

        acc.sll  [k] = acc.sll  [k] * kDecay + sll   * (1.0 - kDecay);
        acc.srr  [k] = acc.srr  [k] * kDecay + srr   * (1.0 - kDecay);
        acc.slrRe[k] = acc.slrRe[k] * kDecay + slrRe * (1.0 - kDecay);
    }
    ++acc.frames;
    // Publish the broadband triple here so the reader always sees ll/rr/lr from
    // the same moment — correlation is meaningless if they are mixed.
    bbPub = bbLive;
}

void PlugNspectrPostProcessor::pushStereoSamples (
    const float* l, const float* r, int n,
    std::array<float, kFftSize>& accumL,
    std::array<float, kFftSize>& accumR,
    int& pos, StereoAccum& acc,
    StereoBroadband& bbLive, StereoBroadband& bbPub)
{
    if (l == nullptr || r == nullptr) return;

    // Broadband sums, decayed per block, for the correlation / mono-loss readouts.
    // Accumulated privately here; published under the lock on frame completion.
    constexpr double kBbDecay = 0.95;
    StereoBroadband& bb = bbLive;
    double ll = 0.0, rr = 0.0, lr = 0.0;
    for (int i = 0; i < n; ++i)
    {
        ll += (double) l[i] * l[i];
        rr += (double) r[i] * r[i];
        lr += (double) l[i] * r[i];
    }
    const double inv = 1.0 / juce::jmax (1, n);
    bb.ll = bb.ll * kBbDecay + (ll * inv) * (1.0 - kBbDecay);
    bb.rr = bb.rr * kBbDecay + (rr * inv) * (1.0 - kBbDecay);
    bb.lr = bb.lr * kBbDecay + (lr * inv) * (1.0 - kBbDecay);

    for (int i = 0; i < n; ++i)
    {
        accumL[pos] = l[i];
        accumR[pos] = r[i];
        if (++pos >= kFftSize)
        {
            pos = 0;
            accumulateStereoFrame (accumL, accumR, acc, bbLive, bbPub);
        }
    }
}

void PlugNspectrPostProcessor::injectStereoBlock (const float* preL, const float* preR,
                                                  const float* postL, const float* postR,
                                                  int n)
{
    // The caller is explicitly supplying an L/R pair, so this counts as a stereo
    // source — real mono detection stays in processBlock (numCh/preCh).
    m_stereoSource.store (true);
    pushStereoSamples (preL,  preR,  n, m_stPreL,  m_stPreR,  m_stPrePos,
                       m_stPreAcc,  m_stBbPreLive,  m_stBbPre);
    pushStereoSamples (postL, postR, n, m_stPostL, m_stPostR, m_stPostPos,
                       m_stPostAcc, m_stBbPostLive, m_stBbPost);
}

void PlugNspectrPostProcessor::getStereo (StereoResult& out) const
{
    // Snapshot under the lock, then do the logs/sqrts outside it. The audio
    // thread needs this same lock to publish frames, so holding it across ~4k
    // transcendental ops would make the audio thread wait on UI-thread maths.
    StereoAccum     preAcc, postAcc;
    StereoBroadband bbPre, bbPost;
    {
        juce::ScopedLock sl (m_stereoLock);
        preAcc  = m_stPreAcc;
        postAcc = m_stPostAcc;
        bbPre   = m_stBbPre;
        bbPost  = m_stBbPost;
    }

    // A bin is only trustworthy once there is real energy in it; below that the
    // width ratio is noise divided by noise.
    auto fill = [] (const StereoAccum& acc,
                    std::array<float, kNumSpecBins>& width,
                    std::array<float, kNumSpecBins>& corr,
                    std::array<bool,  kNumSpecBins>* valid)
    {
        double peak = 0.0;
        for (int k = 0; k < kNumSpecBins; ++k)
            peak = juce::jmax (peak, acc.sll[k] + acc.srr[k]);
        const double gate = peak * 1.0e-6;   // -60 dB below the loudest bin

        for (int k = 0; k < kNumSpecBins; ++k)
        {
            const double sll = acc.sll[k], srr = acc.srr[k], slr = acc.slrRe[k];
            const double mid  = juce::jmax (0.0, (sll + srr + 2.0 * slr) * 0.25);
            const double side = juce::jmax (0.0, (sll + srr - 2.0 * slr) * 0.25);

            width[k] = (mid > 0.0 && side > 0.0)
                     ? (float) juce::jmax ((double) kStereoFloorDb, 10.0 * std::log10 (side / mid))
                     : kStereoFloorDb;

            const double den = std::sqrt (sll * srr);
            corr[k] = (den > 0.0) ? (float) juce::jlimit (-1.0, 1.0, slr / den) : 1.0f;

            if (valid != nullptr)
                (*valid)[k] = (sll + srr) > gate;
        }
    };

    fill (preAcc,  out.widthPre,  out.corrPre,  &out.valid);
    // Post shares the Pre validity mask: a bin is comparable only where both have
    // energy, and Pre is the reference.
    std::array<bool, kNumSpecBins> ignored {};
    fill (postAcc, out.widthPost, out.corrPost, &ignored);
    for (int k = 0; k < kNumSpecBins; ++k)
        out.valid[k] = out.valid[k] && ignored[k];

    // ── Broadband readouts ────────────────────────────────────────────────────
    auto broadband = [] (const StereoBroadband& bb, float& widthDb, float& corr, float& monoLoss)
    {
        const double den = std::sqrt (bb.ll * bb.rr);
        corr = (den > 0.0) ? (float) juce::jlimit (-1.0, 1.0, bb.lr / den) : 1.0f;

        const double mid  = juce::jmax (0.0, (bb.ll + bb.rr + 2.0 * bb.lr) * 0.25);
        const double side = juce::jmax (0.0, (bb.ll + bb.rr - 2.0 * bb.lr) * 0.25);
        widthDb = (mid > 0.0 && side > 0.0)
                ? (float) juce::jmax ((double) kStereoFloorDb, 10.0 * std::log10 (side / mid))
                : kStereoFloorDb;

        // Level lost when summed to mono: mid RMS against the stereo RMS.
        const double stereoPow = (bb.ll + bb.rr) * 0.5;
        monoLoss = (stereoPow > 0.0 && mid > 0.0)
                 ? (float) juce::jmax (-40.0, 10.0 * std::log10 (mid / stereoPow))
                 : (stereoPow > 0.0 ? -40.0f : 0.0f);
    };

    broadband (bbPre,  out.bbWidthPre,  out.bbCorrPre,  out.monoLossPre);
    broadband (bbPost, out.bbWidthPost, out.bbCorrPost, out.monoLossPost);

    out.stereoSource = m_stereoSource.load();
}

void PlugNspectrPostProcessor::resetStereo()
{
    juce::ScopedLock sl (m_stereoLock);
    m_stPreAcc  = StereoAccum {};
    m_stPostAcc = StereoAccum {};
    m_stBbPre   = StereoBroadband {};
    m_stBbPost  = StereoBroadband {};
    m_stBbPreLive  = StereoBroadband {};
    m_stBbPostLive = StereoBroadband {};
}

//==============================================================================
juce::AudioProcessorEditor* PlugNspectrPostProcessor::createEditor()
{
#ifndef PNS_HEADLESS_TESTS
    return new PlugNspectrPostEditor (*this);
#else
    return nullptr;                 // unit-test target links no editor
#endif
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PlugNspectrPostProcessor();
}
