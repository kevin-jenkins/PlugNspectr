// Linear-measurement engine: drive injectMeasurementBlock with known Pre/Post
// relationships and assert the measured transfer function (H1 = Sxy/Sxx).
#include "doctest.h"
#include "helpers.h"
#include "PluginProcessor.h"

#include <array>
#include <vector>

using P = PlugNspectrPostProcessor;

namespace
{
constexpr int N   = P::kMeasFftSize;   // 4096
constexpr int blk = pnst::kBlk;        // 4800
const double  PI  = 3.14159265358979323846;

// A prepared, Pre-active processor (sample rate captured in prepareToPlay).
struct PreparedPost
{
    P proc;
    PreparedPost() { proc.prepareToPlay (pnst::kSR, blk); proc.setTestPreActive (true); }
};
} // namespace

TEST_CASE ("Linear: flat gain x0.5 -> -6.02 dB, 0 phase, coherence ~1")
{
    PreparedPost p;
    juce::Random rng (1);
    std::vector<float> pre ((size_t) blk), post ((size_t) blk);

    p.proc.resetMeasurement();
    for (int b = 0; b < 400; ++b)
    {
        for (int i = 0; i < blk; ++i)
        {
            pre[(size_t) i]  = rng.nextFloat() * 2.0f - 1.0f;
            post[(size_t) i] = pre[(size_t) i] * 0.5f;
        }
        p.proc.injectMeasurementBlock (pre.data(), post.data(), blk);
    }

    P::MeasResult r;
    p.proc.getMeasurement (r);
    const int k = pnst::binAt (1000.0, N);

    REQUIRE (r.frames > 0);
    INFO ("mag@1k=" << r.magDb[k] << "  phase@1k(deg)=" << pnst::radToDeg (r.phase[k])
                    << "  coh@1k=" << r.coh[k]);
    CHECK (std::abs (r.magDb[k] - (-6.02f)) < 0.3f);
    CHECK (std::abs (pnst::radToDeg (r.phase[k])) < 3.0);
    CHECK (r.coh[k] > 0.98f);
}

TEST_CASE ("Linear: pure 10-sample delay -> flat mag, latency=10, group delay ~0.208 ms")
{
    PreparedPost p;
    juce::Random rng (2);
    std::vector<float> pre ((size_t) blk), post ((size_t) blk);

    p.proc.resetMeasurement();
    constexpr int D = 10;
    std::array<float, D> hist {}; int hp = 0;
    for (int b = 0; b < 400; ++b)
    {
        for (int i = 0; i < blk; ++i)
        {
            const float x = rng.nextFloat() * 2.0f - 1.0f;
            post[(size_t) i]  = hist[(size_t) hp];   // x delayed by D
            hist[(size_t) hp] = x;
            hp = (hp + 1) % D;
            pre[(size_t) i] = x;
        }
        p.proc.injectMeasurementBlock (pre.data(), post.data(), blk);
    }

    P::MeasResult r;
    p.proc.getMeasurement (r);

    const int k1 = pnst::binAt (500.0, N), k2 = pnst::binAt (1500.0, N);
    const double f1 = (double) k1 * pnst::kSR / N, f2 = (double) k2 * pnst::kSR / N;
    const double gdMs = -(r.phase[k2] - r.phase[k1]) / (2.0 * PI * (f2 - f1)) * 1000.0;

    INFO ("mag@1k=" << r.magDb[pnst::binAt (1000.0, N)] << "  latency=" << r.latencySamples
                    << "  groupDelay(ms)=" << gdMs);
    CHECK (std::abs (r.magDb[pnst::binAt (1000.0, N)]) < 0.5f);   // flat magnitude
    CHECK (r.latencySamples == 10);
    CHECK (std::abs (gdMs - 0.2083) < 0.03);
}

TEST_CASE ("Linear: one-pole LP at fc=1k -> -3 dB / -45 deg at fc, ~-12 dB at 4k")
{
    PreparedPost p;
    juce::Random rng (3);
    std::vector<float> pre ((size_t) blk), post ((size_t) blk);

    p.proc.resetMeasurement();
    const double fc = 1000.0;
    const float  a  = (float) (1.0 - std::exp (-2.0 * PI * fc / pnst::kSR));
    float y = 0.0f;
    for (int b = 0; b < 400; ++b)
    {
        for (int i = 0; i < blk; ++i)
        {
            const float x = rng.nextFloat() * 2.0f - 1.0f;
            y += a * (x - y);
            pre[(size_t) i] = x; post[(size_t) i] = y;
        }
        p.proc.injectMeasurementBlock (pre.data(), post.data(), blk);
    }

    P::MeasResult r;
    p.proc.getMeasurement (r);
    const int kc = pnst::binAt (1000.0, N), k4 = pnst::binAt (4000.0, N);

    INFO ("mag@1k=" << r.magDb[kc] << "  phase@1k(deg)=" << pnst::radToDeg (r.phase[kc])
                    << "  mag@4k=" << r.magDb[k4]);
    CHECK (std::abs (r.magDb[kc] - (-3.0f)) < 0.6f);
    CHECK (std::abs (pnst::radToDeg (r.phase[kc]) - (-45.0)) < 6.0);
    CHECK (std::abs (r.magDb[k4] - (-12.0f)) < 1.5f);
}
