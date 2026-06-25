// Pre-side stimulus generators. Drives the Pre processor through the
// setTestCommand() seam (no shared memory) and asserts the generated
// output buffer for each stimulus mode.
#include "doctest.h"
#include "helpers.h"
#include "PluginProcessor.h"   // PlugNspectrPre

#include <cmath>

using Pre = PlugNspectrPreProcessor;

namespace
{
float peakAbs (const juce::AudioBuffer<float>& b)
{
    float p = 0.0f;
    for (int i = 0; i < b.getNumSamples(); ++i) p = juce::jmax (p, std::abs (b.getSample (0, i)));
    return p;
}
float rms (const juce::AudioBuffer<float>& b)
{
    double s = 0.0;
    for (int i = 0; i < b.getNumSamples(); ++i) { const float x = b.getSample (0, i); s += (double) x * x; }
    return (float) std::sqrt (s / juce::jmax (1, b.getNumSamples()));
}
int zeroCrossings (const juce::AudioBuffer<float>& b)
{
    int c = 0;
    for (int i = 1; i < b.getNumSamples(); ++i)
        if ((b.getSample (0, i - 1) < 0.0f) != (b.getSample (0, i) < 0.0f)) ++c;
    return c;
}

// getSampleRate() is only valid after the rate details are set (the host does
// this before prepareToPlay); replicate that here so the generators see 48 kHz.
void preparePre (Pre& pre)
{
    pre.setRateAndBufferSizeDetails (pnst::kSR, pnst::kBlk);
    pre.prepareToPlay (pnst::kSR, pnst::kBlk);
}

// One processBlock pass into a fresh 1-channel buffer.
void pump (Pre& pre, juce::AudioBuffer<float>& buf)
{
    juce::MidiBuffer midi;
    buf.clear();
    pre.processBlock (buf, midi);
}
} // namespace

TEST_CASE ("Pre: sine test tone — level and frequency")
{
    Pre pre;
    preparePre (pre);
    pns::PNS_CmdBlock cmd {};
    cmd.testToneActive = 1; cmd.testToneFrequency = 1000.0; cmd.testToneLevelDb = -6.0;
    pre.setTestCommand (cmd);

    juce::AudioBuffer<float> buf (1, pnst::kBlk);
    pump (pre, buf);

    const float expAmp = std::pow (10.0f, -6.0f / 20.0f);   // 0.5012 peak
    INFO ("peak=" << peakAbs (buf) << " zc=" << zeroCrossings (buf));
    CHECK (std::abs (peakAbs (buf) - expAmp) < 0.02f);
    // 1 kHz over 0.1 s = 100 cycles → ~200 zero crossings.
    CHECK (zeroCrossings (buf) >= 196);
    CHECK (zeroCrossings (buf) <= 204);
}

TEST_CASE ("Pre: white-noise stimulus — level and bound")
{
    Pre pre;
    preparePre (pre);
    pns::PNS_CmdBlock cmd {};
    cmd.measureActive = 1;
    pre.setTestCommand (cmd);

    juce::AudioBuffer<float> buf (1, pnst::kBlk);
    pump (pre, buf);

    // Amplitude 0.25 uniform → RMS = 0.25/sqrt(3) ≈ 0.144; peak strictly < 0.25.
    INFO ("rms=" << rms (buf) << " peak=" << peakAbs (buf));
    CHECK (std::abs (rms (buf) - 0.144f) < 0.02f);
    CHECK (peakAbs (buf) <= 0.25f);
    CHECK (peakAbs (buf) > 0.15f);
}

TEST_CASE ("Pre: level ramp — sweeps quiet to loud over the cycle")
{
    Pre pre;
    preparePre (pre);
    pns::PNS_CmdBlock cmd {};
    cmd.dynMeasureMode = 1;
    pre.setTestCommand (cmd);

    juce::AudioBuffer<float> buf (1, pnst::kBlk);
    float first = 0.0f, mid = 0.0f, last = 0.0f;
    for (int b = 0; b < 60; ++b)                 // 60 * 0.1 s = 6 s = one full ramp
    {
        pump (pre, buf);
        const float pk = peakAbs (buf);
        if (b == 0)  first = pk;
        if (b == 30) mid   = pk;
        if (b == 59) last  = pk;
    }
    INFO ("first=" << first << " mid=" << mid << " last=" << last);
    CHECK (first < 0.02f);    // starts near -60 dBFS
    CHECK (last  > 0.5f);     // ends near 0 dBFS
    CHECK (mid   > first);
    CHECK (last  > mid);
}

TEST_CASE ("Pre: level step — high then low across the 1 s cycle")
{
    Pre pre;
    preparePre (pre);
    pns::PNS_CmdBlock cmd {};
    cmd.dynMeasureMode = 2;
    pre.setTestCommand (cmd);

    juce::AudioBuffer<float> buf (1, pnst::kBlk);
    float highPk = 0.0f, lowPk = 0.0f;
    for (int b = 0; b < 10; ++b)                 // 10 * 0.1 s = one 1 s cycle
    {
        pump (pre, buf);
        if (b == 2) highPk = peakAbs (buf);      // first half → -10 dBFS
        if (b == 7) lowPk  = peakAbs (buf);      // second half → -40 dBFS
    }
    INFO ("high=" << highPk << " low=" << lowPk);
    CHECK (std::abs (highPk - std::pow (10.0f, -10.0f / 20.0f)) < 0.03f);   // 0.316
    CHECK (std::abs (lowPk  - std::pow (10.0f, -40.0f / 20.0f)) < 0.01f);   // 0.010
    CHECK (highPk > lowPk * 10.0f);
}

TEST_CASE ("Pre: THD log sweep — frequency ascends across steps")
{
    Pre pre;
    preparePre (pre);
    pns::PNS_CmdBlock cmd {};
    cmd.dynMeasureMode = 3;
    pre.setTestCommand (cmd);

    juce::AudioBuffer<float> buf (1, pnst::kBlk);
    pump (pre, buf);
    const int zcStart = zeroCrossings (buf);     // step 0 → ~50 Hz (low)

    for (int b = 0; b < 120; ++b) pump (pre, buf);
    const int zcLater = zeroCrossings (buf);     // a much higher step

    INFO ("zcStart=" << zcStart << " zcLater=" << zcLater);
    CHECK (zcStart < 20);                        // ~50 Hz → ~10 crossings in 0.1 s
    CHECK (zcLater > zcStart * 5);               // ascended to a far higher frequency
}
