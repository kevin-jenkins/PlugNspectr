// Attack/release envelope: drive a one-pole compressor (T=-20, 2:1, attack 10 ms,
// release 100 ms) with the level step and assert the measured GR-vs-time curve.
#include "doctest.h"
#include "helpers.h"
#include "PluginProcessor.h"

#include <vector>

using P = PlugNspectrPostProcessor;

TEST_CASE ("Envelope: attack 10 ms / release 100 ms GR-vs-time")
{
    constexpr int blk = pnst::kBlk;
    const double  PI  = 3.14159265358979323846;
    const double  T = -20.0, R = 2.0;
    const double  atkCoef = 1.0 - std::exp (-1.0 / (0.010 * pnst::kSR));
    const double  relCoef = 1.0 - std::exp (-1.0 / (0.100 * pnst::kSR));
    const uint32_t period = (uint32_t) pnst::kSR, half = period / 2;

    P proc;
    proc.prepareToPlay (pnst::kSR, blk);
    proc.resetEnvelope();

    std::vector<float> pre ((size_t) blk), post ((size_t) blk);
    double phase = 0.0, gr = 0.0;
    const double pinc = 2.0 * PI * 1000.0 / pnst::kSR;
    uint32_t pos = 0;

    for (int b = 0; b < 240; ++b)                       // ~20 cycles
    {
        const uint32_t blockStart = pos;
        for (int i = 0; i < blk; ++i)
        {
            const double levelDb = (pos < half) ? -10.0 : -40.0;
            const double amp     = std::pow (10.0, levelDb / 20.0) * std::sqrt (2.0);
            const double target  = (levelDb > T) ? (levelDb - T) * (1.0 - 1.0 / R) : 0.0;
            gr += (target - gr) * (target > gr ? atkCoef : relCoef);
            phase += pinc; if (phase > 2.0 * PI) phase -= 2.0 * PI;
            const float s = (float) (amp * std::sin (phase));
            pre[(size_t) i]  = s;
            post[(size_t) i] = (float) (s * std::pow (10.0, -gr / 20.0));
            if (++pos >= period) pos = 0;
        }
        proc.injectEnvelopeBlock (pre.data(), post.data(), blk, blockStart);
    }

    P::EnvResult e;
    proc.getEnvelope (e);
    auto at = [&] (double ms)
    {
        const int b = juce::jlimit (0, P::kEnvBins - 1, (int) (ms / 1000.0 * P::kEnvBins));
        REQUIRE (e.valid[(size_t) b]);
        return e.grDb[(size_t) b];
    };

    INFO ("GR@400=" << at (400) << " GR@900=" << at (900)
                    << " GR@10=" << at (10) << " GR@600=" << at (600));
    // Steady-state high level: GR = (-10 - -20)*(1 - 1/2) = 5 dB.
    CHECK (std::abs (at (400) - 5.0f) < 0.6f);
    // Fully released in the low half.
    CHECK (at (900) < 0.8f);
    // One attack time-constant in (~63% of 5 = 3.16) — fast attacks read a touch
    // low through the 4 ms bins, so allow a wide band.
    CHECK (at (10) > 2.2f);
    CHECK (at (10) < 4.2f);
    // One release time-constant past the step down (~37% of 5 = 1.84).
    CHECK (at (600) > 1.0f);
    CHECK (at (600) < 2.8f);
    // Attack must be faster than release: more GR at +10 ms than at +100 ms into release.
    CHECK (at (10) > at (600));
}
