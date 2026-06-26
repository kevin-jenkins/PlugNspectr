// Channel derivation (L+R combined / Side) feeding the RMS readout. Drives
// processBlock with a known stereo signal and checks getRms().postDb.
#include "doctest.h"
#include "helpers.h"
#include "PluginProcessor.h"

using P = PlugNspectrPostProcessor;

namespace
{
// RMS (dB) of the derived analysis channel for a constant L/R pair, in a given mode.
float postDbFor (P& proc, int mode, float L, float R)
{
    proc.setChannelMode (mode);
    juce::MidiBuffer midi;
    float db = -200.0f;
    for (int n = 0; n < 4; ++n)             // a few blocks; RMS is per-block, no smoothing
    {
        juce::AudioBuffer<float> buf (2, pnst::kBlk);
        for (int i = 0; i < pnst::kBlk; ++i) { buf.setSample (0, i, L); buf.setSample (1, i, R); }
        proc.processBlock (buf, midi);
        db = proc.getRms().postDb;
    }
    return db;
}
} // namespace

TEST_CASE ("Channel: L+R (combined) / Side derivation drives RMS")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, pnst::kBlk);

    const float L = 0.5f, R = 0.1f;         // constant (DC) → RMS == |value|
    auto dbOf = [] (float v) { return 20.0f * std::log10 (v); };

    const float lr   = postDbFor (proc, 0, L, R);   // L+R combined = (L+R)/2
    const float side = postDbFor (proc, 1, L, R);   // Side = (L-R)/2

    INFO ("L+R=" << lr << " Side=" << side);
    CHECK (std::abs (lr   - dbOf (0.5f * (L + R))) < 0.1f);   // (0.3) -10.46
    CHECK (std::abs (side - dbOf (0.5f * (L - R))) < 0.1f);   // (0.2) -13.98
}

// Note: there is intentionally no "preValid == false when not connected" case.
// processBlock opens the *global* named shared memory, so that flag flips true
// whenever a real PlugNspectrPre is live on the machine (e.g. a DAW session
// open) — it isn't hermetic. Pre-connection is integration behavior, not a unit
// test. The derivation check above stays hermetic: postDb comes from the buffer.
