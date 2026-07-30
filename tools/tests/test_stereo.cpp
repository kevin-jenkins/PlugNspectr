// Stereo image engine — width (Side/Mid, dB) and correlation per band, plus the
// broadband correlation / mono-loss readouts. Driven through injectStereoBlock so
// no IPC or editor is involved. Cases are closed-form: mono, anti-phase,
// uncorrelated, a known Mid/Side ratio, and a band-limited widener.
#include "doctest.h"
#include "helpers.h"
#include "PluginProcessor.h"

#include <cmath>
#include <random>
#include <vector>

using P = PlugNspectrPostProcessor;

namespace
{
constexpr double PI = 3.14159265358979323846;

// Feed the engine the same L/R pair as both Pre and Post for enough blocks that
// the exponential averages settle.
void drive (P& proc, const std::vector<float>& l, const std::vector<float>& r, int blocks = 40)
{
    const int n = (int) l.size();
    for (int b = 0; b < blocks; ++b)
        proc.injectStereoBlock (l.data(), r.data(), l.data(), r.data(), n);
}

// Bin index for a frequency, matching the engine's linear FFT bin spacing.
int binOf (double hz)
{
    const double binW = pnst::kSR / (double) P::kFftSize;
    return juce::jlimit (0, P::kNumSpecBins - 1, (int) std::lround (hz / binW));
}

std::vector<float> sine (double hz, double amp, int n, double phase = 0.0)
{
    std::vector<float> v ((size_t) n);
    const double inc = 2.0 * PI * hz / pnst::kSR;
    for (int i = 0; i < n; ++i) v[(size_t) i] = (float) (amp * std::sin (inc * i + phase));
    return v;
}
} // namespace

TEST_CASE ("Stereo: mono source — correlation +1, width at floor")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, pnst::kBlk);

    const auto l = sine (1000.0, 0.5, pnst::kBlk);
    drive (proc, l, l);                            // R == L

    P::StereoResult r;
    proc.getStereo (r);
    const int b = binOf (1000.0);

    INFO ("corr=" << r.corrPost[(size_t) b] << " width=" << r.widthPost[(size_t) b]
                  << " bbCorr=" << r.bbCorrPost << " monoLoss=" << r.monoLossPost);
    CHECK (r.corrPost[(size_t) b] > 0.99f);        // perfectly correlated
    CHECK (r.widthPost[(size_t) b] == P::kStereoFloorDb);   // no side energy at all
    CHECK (r.bbCorrPost > 0.99f);
    CHECK (std::abs (r.monoLossPost) < 0.2f);      // mono-safe: nothing lost summing
}

TEST_CASE ("Stereo: anti-phase — correlation -1 and mono collapse")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, pnst::kBlk);

    const auto l = sine (1000.0, 0.5, pnst::kBlk);
    std::vector<float> r_ (l.size());
    for (size_t i = 0; i < l.size(); ++i) r_[i] = -l[i];   // R == -L
    drive (proc, l, r_);

    P::StereoResult r;
    proc.getStereo (r);
    const int b = binOf (1000.0);

    INFO ("corr=" << r.corrPost[(size_t) b] << " bbCorr=" << r.bbCorrPost
                  << " monoLoss=" << r.monoLossPost);
    CHECK (r.corrPost[(size_t) b] < -0.99f);
    CHECK (r.bbCorrPost < -0.99f);
    CHECK (r.monoLossPost < -30.0f);               // sums to (near) silence in mono
}

TEST_CASE ("Stereo: uncorrelated noise — correlation ~0, width ~0 dB")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, pnst::kBlk);

    std::mt19937 rngL (1234), rngR (9876);
    std::uniform_real_distribution<float> d (-0.3f, 0.3f);
    std::vector<float> l ((size_t) pnst::kBlk), r_ ((size_t) pnst::kBlk);

    // Fresh independent noise every block so the averages see many realisations.
    for (int b = 0; b < 200; ++b)
    {
        for (int i = 0; i < pnst::kBlk; ++i) { l[(size_t) i] = d (rngL); r_[(size_t) i] = d (rngR); }
        proc.injectStereoBlock (l.data(), r_.data(), l.data(), r_.data(), pnst::kBlk);
    }

    P::StereoResult r;
    proc.getStereo (r);

    // Average the mid-band bins — a single bin of a noise estimate is noisy.
    double wSum = 0.0, cSum = 0.0; int cnt = 0;
    for (int b = binOf (500.0); b <= binOf (5000.0); ++b)
    { wSum += r.widthPost[(size_t) b]; cSum += r.corrPost[(size_t) b]; ++cnt; }
    const double wAvg = wSum / cnt, cAvg = cSum / cnt;

    INFO ("width avg=" << wAvg << " corr avg=" << cAvg << " bbCorr=" << r.bbCorrPost);
    CHECK (std::abs (cAvg) < 0.15);       // decorrelated
    CHECK (std::abs (wAvg) < 2.0);        // equal mid and side energy → ~0 dB
    CHECK (std::abs (r.bbCorrPost) < 0.15);
}

TEST_CASE ("Stereo: known Mid/Side ratio reads the expected width")
{
    // L = m + s, R = m - s  =>  Mid = m, Side = s, width = 20·log10(s/m).
    // s/m = 0.5 → -6.02 dB.
    P proc;
    proc.prepareToPlay (pnst::kSR, pnst::kBlk);

    const double m = 0.4, s = 0.2;
    const auto mid  = sine (1000.0, m, pnst::kBlk);
    // Side at a different frequency would land in another bin, so use the same
    // tone with a 90° offset: still Mid/Side separable per bin by power.
    const auto side = sine (1000.0, s, pnst::kBlk, PI * 0.5);

    std::vector<float> l ((size_t) pnst::kBlk), r_ ((size_t) pnst::kBlk);
    for (int i = 0; i < pnst::kBlk; ++i)
    {
        l [(size_t) i] = mid[(size_t) i] + side[(size_t) i];
        r_[(size_t) i] = mid[(size_t) i] - side[(size_t) i];
    }
    drive (proc, l, r_);

    P::StereoResult r;
    proc.getStereo (r);
    const int b = binOf (1000.0);
    const float expected = (float) (20.0 * std::log10 (s / m));   // -6.02 dB

    INFO ("width=" << r.widthPost[(size_t) b] << " expected=" << expected);
    CHECK (std::abs (r.widthPost[(size_t) b] - expected) < 1.0f);
}

TEST_CASE ("Stereo: band-limited widener — width rises only above the corner")
{
    // Side energy only on the high tone: narrow at 200 Hz, wide at 5 kHz.
    P proc;
    proc.prepareToPlay (pnst::kSR, pnst::kBlk);

    const auto lowMid  = sine (200.0,  0.4, pnst::kBlk);          // mono content
    const auto highMid = sine (5000.0, 0.3, pnst::kBlk);
    const auto highSide = sine (5000.0, 0.3, pnst::kBlk, PI * 0.5);   // equal side → ~0 dB

    std::vector<float> l ((size_t) pnst::kBlk), r_ ((size_t) pnst::kBlk);
    for (int i = 0; i < pnst::kBlk; ++i)
    {
        const float lo = lowMid[(size_t) i];
        l [(size_t) i] = lo + highMid[(size_t) i] + highSide[(size_t) i];
        r_[(size_t) i] = lo + highMid[(size_t) i] - highSide[(size_t) i];
    }
    drive (proc, l, r_);

    P::StereoResult r;
    proc.getStereo (r);
    const float wLow  = r.widthPost[(size_t) binOf (200.0)];
    const float wHigh = r.widthPost[(size_t) binOf (5000.0)];

    INFO ("width @200=" << wLow << " @5k=" << wHigh);
    CHECK (wLow  < -20.0f);               // low band is mono
    CHECK (wHigh > -3.0f);                // high band is wide
    CHECK (wHigh > wLow + 15.0f);         // and clearly separated
}
