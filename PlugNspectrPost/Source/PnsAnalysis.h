/*
  ==============================================================================
    PnsAnalysis.h  —  signal measurements behind the editor's plain-language
    readouts, as free functions over plain sample arrays.

    Kept out of the views (and free of JUCE) so the unit tests in tools/tests
    can check them directly: latency between Pre and Post, delay-aligned gain
    reduction, and what a plugin did to the shape of a waveform.
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace pns::analysis
{
    //==========================================================================
    // Latency of Post behind Pre — everything between the two plugins, which a
    // host keeps even when those plugins are bypassed.
    //
    // pre and post hold the same stretch of time (n samples each, newest last).
    // Coarse pass: 1 ms peak envelopes, Post's last 1.5 s slid against Pre over
    // 0..250 ms, best Pearson match accepted if r > 0.7. Envelopes, so it works
    // on any programme material and survives EQ / saturation changing the
    // waveform. Fine pass: raw cross-correlation within one envelope bin of the
    // coarse answer, by magnitude so a polarity-inverting plugin still lines up.
    //
    // Returns the lag in samples, or -1 when the audio gives no confident
    // answer (too short, too steady, or no convincing match).
    //==========================================================================
    static constexpr int kLagWindowMs = 1500, kMaxLagMs = 250;

    inline int requiredLagSamples (double sampleRate)
    {
        const int B = std::max (1, (int) std::lround (sampleRate / 1000.0));
        return (kLagWindowMs + kMaxLagMs + 1) * B;
    }

    inline int estimateLag (const float* pre, const float* post, int n, double sampleRate)
    {
        if (sampleRate <= 0.0 || n < requiredLagSamples (sampleRate)) return -1;
        const int B = std::max (1, (int) std::lround (sampleRate / 1000.0));
        constexpr int N = kLagWindowMs, L = kMaxLagMs;

        // Envelopes over the newest (N + L) bins of Pre and N bins of Post.
        auto envelope = [B] (const float* x, int bins, std::vector<float>& out)
        {
            out.assign ((size_t) bins, 0.0f);
            for (int b = 0; b < bins; ++b)
            {
                float pk = 0.0f;
                for (int i = 0; i < B; ++i) pk = std::max (pk, std::abs (x[b * B + i]));
                out[(size_t) b] = pk;
            }
        };
        std::vector<float> ePre, ePost;
        envelope (pre  + (n - (N + L) * B), N + L, ePre);
        envelope (post + (n -  N      * B), N,     ePost);

        auto meanSd = [] (const float* x, int m, float& mean, float& sd)
        {
            double s = 0.0, s2 = 0.0;
            for (int i = 0; i < m; ++i) { s += x[i]; s2 += (double) x[i] * x[i]; }
            mean = (float) (s / m);
            sd   = (float) std::sqrt (std::max (0.0, s2 / m - (s / m) * (s / m)));
        };
        float pm, ps;
        meanSd (ePost.data(), N, pm, ps);
        if (ps < 1.0e-4f) return -1;   // too steady or quiet to locate anything

        float bestR = 0.0f; int bestBins = -1;
        for (int lag = 0; lag <= L; ++lag)
        {
            const float* x = ePre.data() + (L - lag);   // Pre, `lag` ms earlier than Post
            float xm, xs;
            meanSd (x, N, xm, xs);
            if (xs < 1.0e-4f) continue;
            double c = 0.0;
            for (int i = 0; i < N; ++i) c += (double) (x[i] - xm) * (ePost[(size_t) i] - pm);
            const float r = (float) (c / N) / (xs * ps);
            if (r > bestR) { bestR = r; bestBins = lag; }
        }
        if (bestBins < 0 || bestR <= 0.7f) return -1;

        constexpr int kFine = 4096;
        const int coarse = bestBins * B;
        double bestC = -1.0; int best = coarse;
        for (int lag = std::max (0, coarse - B); lag <= coarse + B; ++lag)
        {
            if (kFine + lag > n) break;
            const float* q = post + (n - kFine);
            const float* p = pre  + (n - kFine - lag);
            double c = 0.0;
            for (int i = 0; i < kFine; ++i) c += (double) p[i] * q[i];
            if (std::abs (c) > bestC) { bestC = std::abs (c); best = lag; }
        }
        return best;
    }

    //==========================================================================
    // Gain reduction of an already delay-aligned Pre / Post stretch, measured as
    // RMS over consecutive windows (so a sub-millisecond residual skew can't
    // turn a transient into a fake dip) that cover the whole stretch.
    //
    // Per window: Pre below -60 dBFS is "not audible" and skipped; otherwise
    // min (0, post - pre) dB with a 0.2 dB dead band so float noise on identical
    // signals reads as 0. The result carries the mean over audible windows (for
    // averages) and the deepest one (for peaks), so a limiter grabbing for a few
    // milliseconds anywhere in the stretch is seen.
    //==========================================================================
    struct GainReduction
    {
        bool  audible = false;
        float meanDb  = 0.0f;   // ≤ 0
        float minDb   = 0.0f;   // ≤ 0, the deepest window
    };

    inline GainReduction gainReduction (const float* pre, const float* post, int n, int window)
    {
        GainReduction out;
        if (n <= 0) return out;
        window = std::max (1, std::min (window, n));
        double sum = 0.0; int count = 0;
        // Windows step back from the newest sample; if they don't divide n, one
        // last window is pinned to the oldest sample (overlapping its neighbour)
        // so every sample is looked at.
        for (int end = n; ; )
        {
            const int start = std::max (0, end - window), stop = start + window;
            double ep = 0.0, eq = 0.0;
            for (int i = start; i < stop; ++i)
            {
                ep += (double) pre[i]  * pre[i];
                eq += (double) post[i] * post[i];
            }
            const double preRms = std::sqrt (ep / window), postRms = std::sqrt (eq / window);
            if (preRms >= 1.0e-3)
            {
                float gr = (float) (20.0 * std::log10 (std::max (postRms, 1.0e-6) / preRms));
                gr = gr > -0.2f ? 0.0f : gr;
                sum += gr; ++count;
                out.minDb = std::min (out.minDb, gr);
            }
            if (start == 0) break;
            end = start;
        }
        if (count > 0) { out.audible = true; out.meanDb = (float) (sum / count); }
        return out;
    }

    //==========================================================================
    // Waveform shape. Crest = peak / RMS in dB: lower means rounder, flatter
    // peaks. "Flat" = share of samples within 0.1% of the peak: ~0 for a sine
    // or soft saturation, 20%+ once a hard clip holds the wave at the ceiling.
    //==========================================================================
    struct Shape
    {
        float peak = 0.0f, rms = 0.0f, crestDb = 0.0f, flat = 0.0f;
    };

    inline Shape measureShape (const float* x, int n)
    {
        Shape s;
        if (n < 2) return s;
        double e = 0.0;
        for (int i = 0; i < n; ++i) { e += (double) x[i] * x[i]; s.peak = std::max (s.peak, std::abs (x[i])); }
        s.rms = (float) std::sqrt (e / n);
        int atPeak = 0;
        for (int i = 0; i < n; ++i) if (std::abs (x[i]) >= 0.999f * s.peak) ++atPeak;
        s.flat    = (float) atPeak / (float) n;
        s.crestDb = s.rms > 1.0e-6f ? 20.0f * std::log10 (s.peak / s.rms) : 0.0f;
        return s;
    }

    enum class ShapeChange { Unchanged, HardClip, Rounded, Sharper };

    // Thresholds tuned on a sine: a hard clip of even 1 dB holds 20%+ of the
    // cycle at the peak, tanh saturation ~3% at moderate drive; tanh rounds the
    // crest by ~0.4 dB already at light drive.
    inline ShapeChange classifyShape (const Shape& pre, const Shape& post)
    {
        const float crestDelta = post.crestDb - pre.crestDb;
        if (post.flat - pre.flat > 0.15f) return ShapeChange::HardClip;
        if (crestDelta < -0.4f)          return ShapeChange::Rounded;
        if (crestDelta >  1.0f)          return ShapeChange::Sharper;
        return ShapeChange::Unchanged;
    }
}
