// Dynamics transfer curve: feed a known static compressor (threshold -20, 2:1)
// at swept input levels and assert the measured output level per input bin.
#include "doctest.h"
#include "helpers.h"
#include "PluginProcessor.h"

#include <vector>

using P = PlugNspectrPostProcessor;

TEST_CASE ("Dynamics: 2:1 compressor (T=-20) transfer points")
{
    constexpr int blk = pnst::kBlk;
    const double  PI  = 3.14159265358979323846;
    auto compOut = [] (double L) { return L <= -20.0 ? L : -20.0 + (L + 20.0) / 2.0; };

    P proc;
    proc.prepareToPlay (pnst::kSR, blk);
    proc.resetDynamics();

    std::vector<float> pre ((size_t) blk), post ((size_t) blk);
    double phase = 0.0;
    const double pinc = 2.0 * PI * 1000.0 / pnst::kSR;

    for (int step = 0; step < 2400; ++step)
    {
        const double L = -55.0 + (step % 520) * 0.1;     // sweep -55..-3 dB, repeating
        if (L > -2.0) continue;
        const double amp  = std::pow (10.0, L / 20.0) * std::sqrt (2.0);  // sine peak for RMS=L
        const double gain = std::pow (10.0, (compOut (L) - L) / 20.0);
        for (int i = 0; i < blk; ++i)
        {
            phase += pinc; if (phase > 2.0 * PI) phase -= 2.0 * PI;
            const float s = (float) (amp * std::sin (phase));
            pre[(size_t) i]  = s;
            post[(size_t) i] = (float) (s * gain);
        }
        proc.injectDynamicsBlock (pre.data(), post.data(), blk);
    }

    P::DynResult d;
    proc.getDynamics (d);
    auto at = [&] (double L)
    {
        const int b = (int) std::lround ((L - P::kDynMinDb) / P::kDynBinW);
        REQUIRE (b >= 0);
        REQUIRE (b < P::kDynBins);
        REQUIRE (d.valid[(size_t) b]);
        return d.outDb[(size_t) b];
    };

    INFO ("out@-40=" << at (-40) << " out@-20=" << at (-20)
                     << " out@-10=" << at (-10) << " out@-4=" << at (-4));
    CHECK (std::abs (at (-40) - (-40.0f)) < 0.7f);   // below knee → 1:1
    CHECK (std::abs (at (-20) - (-20.0f)) < 0.7f);   // at knee
    CHECK (std::abs (at (-10) - (-15.0f)) < 0.7f);   // 10 dB over → 5 dB out → -15
    CHECK (std::abs (at (-4)  - (-12.0f)) < 0.7f);   // 16 dB over → 8 dB out → -12
}
