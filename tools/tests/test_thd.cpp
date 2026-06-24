// THD-vs-frequency sweep: inject a tone with a fixed 5% 2nd harmonic across the
// log sweep and assert the measured THD per frequency.
#include "doctest.h"
#include "helpers.h"
#include "PluginProcessor.h"

#include <vector>

using P = PlugNspectrPostProcessor;

TEST_CASE ("THD sweep: 5% 2nd harmonic across frequency")
{
    constexpr int blk = pnst::kBlk;
    const double  PI  = 3.14159265358979323846;
    const double  a   = 0.05;                         // 2nd-harmonic amplitude → 5% THD

    P proc;
    proc.prepareToPlay (pnst::kSR, blk);
    proc.resetThdSweep();

    std::vector<float> post ((size_t) blk);
    double ph = 0.0;

    for (int pass = 0; pass < 3; ++pass)
        for (int s = 0; s < 100; ++s)
        {
            const double f   = 50.0 * std::pow (100.0, (double) s / 99.0);   // 50..5000 Hz log
            const double inc = 2.0 * PI * f / pnst::kSR;
            for (int blkN = 0; blkN < 4; ++blkN)      // hold each frequency for 4 blocks
            {
                for (int i = 0; i < blk; ++i)
                {
                    ph += inc; if (ph > 2.0 * PI) ph -= 2.0 * PI;
                    post[(size_t) i] = (float) (std::sin (ph) + a * std::sin (2.0 * ph));
                }
                proc.injectThdSweepBlock (post.data(), blk, f);
            }
        }

    P::ThdResult r;
    proc.getThdSweep (r);
    auto at = [&] (double f)
    {
        const double t = std::log (f / (double) P::kThdLoHz)
                       / std::log ((double) P::kThdHiHz / (double) P::kThdLoHz);
        const int b = juce::jlimit (0, P::kThdBins - 1,
                                    (int) std::lround (t * (P::kThdBins - 1)));
        REQUIRE (r.valid[(size_t) b]);
        return r.thdPct[(size_t) b];
    };

    INFO ("THD @100=" << at (100) << " @500=" << at (500)
                      << " @1k=" << at (1000) << " @3k=" << at (3000));
    // Mid-band is accurate; band edges are looser (short frame / harmonic crowding).
    CHECK (std::abs (at (500)  - 5.0f) < 1.0f);
    CHECK (std::abs (at (1000) - 5.0f) < 1.0f);
    CHECK (std::abs (at (3000) - 5.0f) < 1.0f);
    CHECK (std::abs (at (100)  - 5.0f) < 2.5f);
}
