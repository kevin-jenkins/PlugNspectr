// Editor-side signal measurements (PnsAnalysis.h): Pre->Post latency, delay-
// aligned gain reduction, and waveform-shape classification. These drive the
// Dynamics and Waveform headlines, so each is checked against a case with a
// known answer.
#include "doctest.h"
#include "PnsAnalysis.h"

#include <cmath>
#include <random>
#include <vector>

namespace an = pns::analysis;

namespace
{
constexpr double kSR = 48000.0;
const double     kPi = 3.14159265358979323846;

// Drum-like programme: a decaying noise hit every 250 ms over a quiet bed.
std::vector<float> hits (int n, unsigned seed = 7)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) (i % 12000) / (float) kSR;
        x[(size_t) i] = 0.6f * std::exp (-t * 30.0f) * u (rng) + 0.02f * u (rng);
    }
    return x;
}

// y[i] = gain * x[i - delay] (zeros before the start).
std::vector<float> delayed (const std::vector<float>& x, int delay, float gain = 1.0f)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t i = (size_t) delay; i < x.size(); ++i) y[i] = gain * x[i - (size_t) delay];
    return y;
}

std::vector<float> sine (int n, double hz, float amp)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i) x[(size_t) i] = amp * (float) std::sin (2.0 * kPi * hz * i / kSR);
    return x;
}
}

TEST_CASE ("Analysis: latency of a delayed copy is found to the sample")
{
    const int n = an::requiredLagSamples (kSR) + 4800;
    const auto pre = hits (n);
    for (int d : { 0, 64, 2048, 9000 })
    {
        const auto post = delayed (pre, d);
        CHECK (an::estimateLag (pre.data(), post.data(), n, kSR) == d);
    }
}

TEST_CASE ("Analysis: latency survives a polarity flip and a level change")
{
    const int n = an::requiredLagSamples (kSR) + 4800;
    const auto pre  = hits (n, 11);
    const auto post = delayed (pre, 2048, -0.5f);
    CHECK (an::estimateLag (pre.data(), post.data(), n, kSR) == 2048);
}

TEST_CASE ("Analysis: no confident latency from silence, a steady tone, or too little audio")
{
    const int n = an::requiredLagSamples (kSR) + 4800;
    std::vector<float> silent ((size_t) n, 0.0f);
    CHECK (an::estimateLag (silent.data(), silent.data(), n, kSR) == -1);

    const auto tone = sine (n, 1000.0, 0.5f);   // flat envelope: nothing to align
    CHECK (an::estimateLag (tone.data(), tone.data(), n, kSR) == -1);

    const auto pre = hits (n);
    CHECK (an::estimateLag (pre.data(), pre.data(), n / 2, kSR) == -1);
}

TEST_CASE ("Analysis: gain reduction of aligned audio")
{
    const int n = 4800;
    const auto pre = hits (n);

    SUBCASE ("identical signals read 0")
    {
        const auto gr = an::gainReduction (pre.data(), pre.data(), n, 1440);
        CHECK (gr.audible);
        CHECK (gr.meanDb == 0.0f);
        CHECK (gr.minDb  == 0.0f);
    }
    SUBCASE ("half the level reads about -6 dB everywhere")
    {
        const auto post = delayed (pre, 0, 0.5f);
        const auto gr   = an::gainReduction (pre.data(), post.data(), n, 1440);
        CHECK (gr.meanDb == doctest::Approx (-6.02).epsilon (0.01));
        CHECK (gr.minDb  == doctest::Approx (-6.02).epsilon (0.01));
    }
    SUBCASE ("a brief cut anywhere in the stretch shows as the deepest window")
    {
        auto post = pre;
        for (int i = 0; i < 1440; ++i) post[(size_t) i] *= 0.25f;   // oldest window only, -12 dB
        const auto gr = an::gainReduction (pre.data(), post.data(), n, 1440);
        CHECK (gr.minDb == doctest::Approx (-12.04).epsilon (0.01));
        CHECK (gr.meanDb > gr.minDb);
    }
    SUBCASE ("silence is not audible")
    {
        std::vector<float> quiet ((size_t) n, 1.0e-5f);
        CHECK_FALSE (an::gainReduction (quiet.data(), quiet.data(), n, 1440).audible);
    }
}

TEST_CASE ("Analysis: waveform shape changes are named correctly")
{
    const int n = 2400;   // 50 ms
    const auto x = sine (n, 220.0, 0.7f);
    auto through = [&] (auto f)
    {
        std::vector<float> y ((size_t) n);
        for (int i = 0; i < n; ++i) y[(size_t) i] = f (x[(size_t) i]);
        return an::classifyShape (an::measureShape (x.data(), n), an::measureShape (y.data(), n));
    };

    CHECK (through ([] (float v) { return v; })        == an::ShapeChange::Unchanged);
    CHECK (through ([] (float v) { return 0.5f * v; }) == an::ShapeChange::Unchanged);   // quieter, same shape
    for (float drive : { 1.5f, 2.5f })
        CHECK (through ([drive] (float v) { return std::tanh (drive * v); }) == an::ShapeChange::Rounded);
    for (float ceiling : { 0.6f, 0.5f, 0.35f })
        CHECK (through ([ceiling] (float v) { return std::clamp (v, -ceiling, ceiling); }) == an::ShapeChange::HardClip);
    CHECK (through ([] (float v) { return v * v * v; }) == an::ShapeChange::Sharper);   // expander-like
}
