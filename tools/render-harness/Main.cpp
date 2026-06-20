// ───────────────────────────────────────────────────────────────────────────
// PlugNspectrPost headless render harness
//
// Instantiates the real plugin editor, feeds it synthetic audio through the
// processor's test seam, ticks the editor's 60fps timer, and writes a sequence
// of PNG frames — all without a DAW or a live PlugNspectrPre.
//
// Usage:  PnsRenderHarness [scenario] [numFrames] [grDb]
//   scenario: "dynamics" (default) — quiet mix-bus-level signal with swelling
//             dynamics and a fixed gain reduction, to exercise the waveform + GR.
//   grDb:     simulated gain reduction in dB (default 1.4). e.g. 0.5 to check the
//             detector resolves small GR; 0 for a transparent (no-GR) pass.
// Output:  ./frames/frame_000.png …  (in the current working directory)
// ───────────────────────────────────────────────────────────────────────────
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <iostream>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 4800;   // 0.1s per injected frame

// Synthetic "mix bus" block: a ~-42 dBFS tone whose level swells over time, with
// the post signal attenuated by postGain to stand in for a fixed gain reduction.
void makeFrame (juce::AudioBuffer<float>& pre, juce::AudioBuffer<float>& post,
                double& phase, int frameIdx, float postGain, float& preDb, float& postDb)
{
    pre .setSize (1, kBlock, false, false, true);
    post.setSize (1, kBlock, false, false, true);

    const float env  = 0.30f * (1.0f + 0.5f * std::sin (frameIdx * 0.12f));   // ≈ -12 dBFS RMS, swelling
    const float freq = 220.0f;

    double preSq = 0.0, postSq = 0.0;
    for (int i = 0; i < kBlock; ++i)
    {
        const float s  = env * std::sin ((float) phase);
        phase += juce::MathConstants<double>::twoPi * freq / kSampleRate;
        const float ps = s;
        const float qs = s * postGain;   // post attenuated → simulated GR

        pre .setSample (0, i, ps);
        post.setSample (0, i, qs);
        preSq  += (double) ps * ps;
        postSq += (double) qs * qs;
    }

    preDb  = 20.0f * std::log10 (juce::jmax ((float) std::sqrt (preSq  / kBlock), 1.0e-6f));
    postDb = 20.0f * std::log10 (juce::jmax ((float) std::sqrt (postSq / kBlock), 1.0e-6f));
}

// ── Linear-measurement DSP verification ─────────────────────────────────────
// Feed the processor known Pre/Post pairs through injectMeasurementBlock and
// compare the measured transfer function against closed-form expectations.
void runLinearTest (PlugNspectrPostProcessor& proc)
{
    constexpr double sr  = kSampleRate;
    constexpr int    N   = PlugNspectrPostProcessor::kMeasFftSize;   // 4096
    constexpr int    blk = kBlock;                                   // 4800
    const double pi = juce::MathConstants<double>::pi;
    auto bin = [&] (double f) { return (int) std::lround (f * N / sr); };
    auto deg = [&] (float rad) { return rad * 180.0f / (float) pi; };

    juce::Random rng;
    std::vector<float> pre ((size_t) blk), post ((size_t) blk);
    PlugNspectrPostProcessor::MeasResult r;

    // 1) Flat gain ×0.5 (−6.02 dB), zero phase.
    proc.resetMeasurement();
    for (int b = 0; b < 400; ++b)
    {
        for (int i = 0; i < blk; ++i) { pre[(size_t) i] = rng.nextFloat() * 2.0f - 1.0f;
                                        post[(size_t) i] = pre[(size_t) i] * 0.5f; }
        proc.injectMeasurementBlock (pre.data(), post.data(), blk);
    }
    proc.getMeasurement (r);
    std::cout << "[gain x0.5]  frames=" << r.frames
              << "  mag@1k="   << r.magDb[bin (1000)] << " dB (exp -6.02)"
              << "  phase@1k="  << deg (r.phase[bin (1000)]) << " deg (exp 0)"
              << "  coh@1k="    << r.coh[bin (1000)] << "\n";

    // 2) Pure delay of 10 samples: flat magnitude, linear phase, constant
    //    group delay = 10/sr = 0.2083 ms.
    proc.resetMeasurement();
    {
        constexpr int D = 10;
        std::array<float, D> hist {}; int hp = 0;
        for (int b = 0; b < 400; ++b)
        {
            for (int i = 0; i < blk; ++i)
            {
                const float x = rng.nextFloat() * 2.0f - 1.0f;
                post[(size_t) i] = hist[(size_t) hp];   // x delayed by D
                hist[(size_t) hp] = x;
                hp = (hp + 1) % D;
                pre[(size_t) i] = x;
            }
            proc.injectMeasurementBlock (pre.data(), post.data(), blk);
        }
    }
    proc.getMeasurement (r);
    {
        const int k1 = bin (500), k2 = bin (1500);
        const double f1 = (double) k1 * sr / N, f2 = (double) k2 * sr / N;
        const double gdMs = -(r.phase[k2] - r.phase[k1]) / (2.0 * pi * (f2 - f1)) * 1000.0;
        std::cout << "[delay 10smp] mag@1k=" << r.magDb[bin (1000)] << " dB (exp ~0)"
                  << "  groupDelay=" << gdMs << " ms (exp 0.2083)"
                  << "  latency=" << r.latencySamples << " smp (exp 10)\n";
    }

    // 3) One-pole low-pass at fc=1000 Hz: −3 dB and −45° at fc.
    proc.resetMeasurement();
    {
        const double fc = 1000.0;
        const float  a  = (float) (1.0 - std::exp (-2.0 * pi * fc / sr));
        float y = 0.0f;
        for (int b = 0; b < 400; ++b)
        {
            for (int i = 0; i < blk; ++i)
            {
                const float x = rng.nextFloat() * 2.0f - 1.0f;
                y += a * (x - y);
                pre[(size_t) i] = x; post[(size_t) i] = y;
            }
            proc.injectMeasurementBlock (pre.data(), post.data(), blk);
        }
    }
    proc.getMeasurement (r);
    std::cout << "[1-pole LP fc=1k] mag@1k=" << r.magDb[bin (1000)] << " dB (exp ~-3)"
              << "  phase@1k=" << deg (r.phase[bin (1000)]) << " deg (exp ~-45)"
              << "  mag@4k=" << r.magDb[bin (4000)] << " dB (exp ~-12)\n";
}

// ── Dynamics transfer-curve verification ────────────────────────────────────
// Feed a static compressor (threshold -20 dB, 2:1) at swept input levels and
// check the measured output level per input bin.
void runDynamicsTest (PlugNspectrPostProcessor& proc)
{
    constexpr double sr  = kSampleRate;
    constexpr int    blk = kBlock;
    const double pi = juce::MathConstants<double>::pi;
    auto compOut = [] (double L) { return L <= -20.0 ? L : -20.0 + (L + 20.0) / 2.0; };

    proc.resetDynamics();
    std::vector<float> pre ((size_t) blk), post ((size_t) blk);
    double phase = 0.0;
    const double pinc = 2.0 * pi * 1000.0 / sr;

    for (int step = 0; step < 2400; ++step)
    {
        const double L = -55.0 + (step % 520) * 0.1;        // sweep -55..-3 dB, repeating
        if (L > -2.0) continue;
        const double amp  = std::pow (10.0, L / 20.0) * std::sqrt (2.0);   // sine peak for RMS=L
        const double gain = std::pow (10.0, (compOut (L) - L) / 20.0);
        for (int i = 0; i < blk; ++i)
        {
            phase += pinc; if (phase > 2.0 * pi) phase -= 2.0 * pi;
            const float s = (float) (amp * std::sin (phase));
            pre[(size_t) i]  = s;
            post[(size_t) i] = (float) (s * gain);
        }
        proc.injectDynamicsBlock (pre.data(), post.data(), blk);
    }

    PlugNspectrPostProcessor::DynResult d;
    proc.getDynamics (d);
    auto at = [&] (double L)
    {
        const int b = (int) std::lround ((L - PlugNspectrPostProcessor::kDynMinDb)
                                         / PlugNspectrPostProcessor::kDynBinW);
        return d.valid[(size_t) b] ? d.outDb[(size_t) b] : -999.0f;
    };
    std::cout << "[comp -20dB 2:1]  out@-40=" << at (-40) << " (exp -40)"
              << "  out@-20=" << at (-20) << " (exp -20)"
              << "  out@-10=" << at (-10) << " (exp -15)"
              << "  out@-4="  << at (-4)  << " (exp -12)\n";
}

// ── Attack/release envelope verification ────────────────────────────────────
// Drive a compressor (T=-20, 2:1, attack 10 ms, release 100 ms) with the level
// step and check the measured GR-vs-time against the expected one-pole curves.
void runEnvelopeTest (PlugNspectrPostProcessor& proc)
{
    constexpr double sr  = kSampleRate;
    constexpr int    blk = kBlock;
    const double pi = juce::MathConstants<double>::pi;
    const double T = -20.0, R = 2.0;
    const double atkCoef = 1.0 - std::exp (-1.0 / (0.010 * sr));
    const double relCoef = 1.0 - std::exp (-1.0 / (0.100 * sr));
    const uint32_t period = (uint32_t) sr, half = period / 2;

    proc.resetEnvelope();
    std::vector<float> pre ((size_t) blk), post ((size_t) blk);
    double phase = 0.0, gr = 0.0;
    const double pinc = 2.0 * pi * 1000.0 / sr;
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
            phase += pinc; if (phase > 2.0 * pi) phase -= 2.0 * pi;
            const float s = (float) (amp * std::sin (phase));
            pre[(size_t) i]  = s;
            post[(size_t) i] = (float) (s * std::pow (10.0, -gr / 20.0));
            if (++pos >= period) pos = 0;
        }
        proc.injectEnvelopeBlock (pre.data(), post.data(), blk, blockStart);
    }

    PlugNspectrPostProcessor::EnvResult e;
    proc.getEnvelope (e);
    auto at = [&] (double ms)
    {
        const int b = juce::jlimit (0, PlugNspectrPostProcessor::kEnvBins - 1,
                                    (int) (ms / 1000.0 * PlugNspectrPostProcessor::kEnvBins));
        return e.valid[(size_t) b] ? e.grDb[(size_t) b] : -999.0f;
    };
    std::cout << "[comp atk10 rel100]  GR@400ms=" << at (400) << " (exp ~5, steady)"
              << "  GR@10ms=" << at (10) << " (exp ~3.2, 1 atk TC)"
              << "  GR@900ms=" << at (900) << " (exp ~0)"
              << "  GR@600ms=" << at (600) << " (exp ~1.8, 1 rel TC)\n";
}

// ── THD-vs-frequency verification ───────────────────────────────────────────
// Sweep a tone with a fixed 5% 2nd harmonic and check the measured THD per freq.
void runThdTest (PlugNspectrPostProcessor& proc)
{
    constexpr double sr  = kSampleRate;
    constexpr int    blk = kBlock;
    const double pi = juce::MathConstants<double>::pi;
    const double a  = 0.05;                          // 2nd-harmonic amplitude → 5% THD

    proc.resetThdSweep();
    std::vector<float> post ((size_t) blk);
    double ph = 0.0;

    for (int pass = 0; pass < 3; ++pass)
        for (int s = 0; s < 100; ++s)
        {
            const double f   = 50.0 * std::pow (100.0, (double) s / 99.0);   // 50..5000 Hz log
            const double inc = 2.0 * pi * f / sr;
            for (int blkN = 0; blkN < 4; ++blkN)        // hold each frequency for 4 blocks
            {
                for (int i = 0; i < blk; ++i)
                {
                    ph += inc; if (ph > 2.0 * pi) ph -= 2.0 * pi;
                    post[(size_t) i] = (float) (std::sin (ph) + a * std::sin (2.0 * ph));
                }
                proc.injectThdSweepBlock (post.data(), blk, f);
            }
        }

    PlugNspectrPostProcessor::ThdResult r;
    proc.getThdSweep (r);
    auto at = [&] (double f)
    {
        const double t = std::log (f / (double) PlugNspectrPostProcessor::kThdLoHz)
                       / std::log ((double) PlugNspectrPostProcessor::kThdHiHz
                                   / PlugNspectrPostProcessor::kThdLoHz);
        const int b = juce::jlimit (0, PlugNspectrPostProcessor::kThdBins - 1,
                                    (int) std::lround (t * (PlugNspectrPostProcessor::kThdBins - 1)));
        return r.valid[(size_t) b] ? r.thdPct[(size_t) b] : -999.0f;
    };
    std::cout << "[THD 5% 2nd harm]  @100=" << at (100) << "  @500=" << at (500)
              << "  @1k=" << at (1000) << "  @3k=" << at (3000) << "  (exp ~5.0%)\n";
}
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const int numFrames = (argc > 2) ? juce::String (argv[2]).getIntValue() : 90;
    // Optional 3rd arg: simulated gain reduction in dB (post attenuation).
    const float grDb     = (argc > 3) ? juce::String (argv[3]).getFloatValue() : 1.4f;
    const float postGain = std::pow (10.0f, -grDb / 20.0f);

    juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile ("frames");
    outDir.createDirectory();

    PlugNspectrPostProcessor proc;
    proc.prepareToPlay (kSampleRate, kBlock);
    proc.setTestPreActive (true);

    // Numeric DSP verification for the Linear module (no rendering).
    if (argc > 1 && juce::String (argv[1]) == "linear")
    {
        runLinearTest (proc);
        return 0;
    }
    if (argc > 1 && juce::String (argv[1]) == "dyntest")
    {
        runDynamicsTest (proc);
        return 0;
    }
    if (argc > 1 && juce::String (argv[1]) == "envtest")
    {
        runEnvelopeTest (proc);
        return 0;
    }
    if (argc > 1 && juce::String (argv[1]) == "thdtest")
    {
        runThdTest (proc);
        return 0;
    }

    std::unique_ptr<juce::AudioProcessorEditor> editorBase (proc.createEditor());
    auto* editor = dynamic_cast<PlugNspectrPostEditor*> (editorBase.get());
    if (editor == nullptr) { std::cerr << "Failed to create editor\n"; return 1; }

    // Render modes feed a known plugin into the matching measurement tab.
    const bool linearRender   = (argc > 1 && juce::String (argv[1]) == "linear-render");
    const bool transferRender = (argc > 1 && juce::String (argv[1]) == "transfer-render");
    const bool envelopeRender = (argc > 1 && juce::String (argv[1]) == "envelope-render");
    const bool thdRender      = (argc > 1 && juce::String (argv[1]) == "thd-render");

    editor->setSize (1000, 640);
    editor->selectTabForTest (linearRender ? 4 : transferRender ? 5 : envelopeRender ? 6
                              : thdRender ? 7 : 1);

    juce::PNGImageFormat png;
    double phase = 0.0;
    int written = 0;
    juce::Random rng;
    const float lpA = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 1000.0 / kSampleRate));
    float lpY = 0.0f;
    constexpr int kDelay = 64;                 // bulk latency to exercise compensation
    std::array<float, kDelay> dl {}; int dp = 0;
    double trPhase = 0.0; int trStep = 0;      // transfer-curve level sweep state
    double enPhase = 0.0, enGr = 0.0; uint32_t enPos = 0;   // envelope step + compressor state
    double thdPh = 0.0; int thdStep = 0;       // THD sweep state (5% 2nd-harmonic plugin)

    for (int f = 0; f < numFrames; ++f)
    {
        if (thdRender)
        {
            std::vector<float> pre ((size_t) kBlock), post ((size_t) kBlock);
            const double fHz = 50.0 * std::pow (100.0, (double) ((thdStep / 3) % 100) / 99.0);
            const double inc = 2.0 * juce::MathConstants<double>::pi * fHz / kSampleRate;
            for (int i = 0; i < kBlock; ++i)
            {
                thdPh += inc; if (thdPh > 2.0 * juce::MathConstants<double>::pi) thdPh -= 2.0 * juce::MathConstants<double>::pi;
                const float s = (float) (0.25 * (std::sin (thdPh) + 0.05 * std::sin (2.0 * thdPh)));
                pre[(size_t) i] = s; post[(size_t) i] = s;
            }
            proc.injectThdSweepBlock (post.data(), kBlock, fHz);
            ++thdStep;
        }
        else if (envelopeRender)
        {
            // Level-step sine → compressor (T=-20, 2:1, attack 10ms, release 100ms).
            std::vector<float> pre ((size_t) kBlock), post ((size_t) kBlock);
            const uint32_t period = (uint32_t) kSampleRate, half = period / 2;
            const double pinc = 2.0 * juce::MathConstants<double>::pi * 1000.0 / kSampleRate;
            const double atkCoef = 1.0 - std::exp (-1.0 / (0.010 * kSampleRate));
            const double relCoef = 1.0 - std::exp (-1.0 / (0.100 * kSampleRate));
            const uint32_t blockStart = enPos;
            for (int i = 0; i < kBlock; ++i)
            {
                const double levelDb = (enPos < half) ? -10.0 : -40.0;
                const double amp     = std::pow (10.0, levelDb / 20.0) * std::sqrt (2.0);
                const double target  = (levelDb > -20.0) ? (levelDb + 20.0) * 0.5 : 0.0;
                enGr += (target - enGr) * (target > enGr ? atkCoef : relCoef);
                enPhase += pinc; if (enPhase > 2.0 * juce::MathConstants<double>::pi) enPhase -= 2.0 * juce::MathConstants<double>::pi;
                const float s = (float) (amp * std::sin (enPhase));
                pre[(size_t) i] = s; post[(size_t) i] = (float) (s * std::pow (10.0, -enGr / 20.0));
                if (++enPos >= period) enPos = 0;
            }
            proc.injectEnvelopeBlock (pre.data(), post.data(), kBlock, blockStart);
        }
        else if (transferRender)
        {
            // Static compressor (-20 dB, 2:1); input level held constant per
            // block and swept -58..-2 dB across frames so every bin fills.
            auto compOut = [] (double Ld) { return Ld <= -20.0 ? Ld : -20.0 + (Ld + 20.0) / 2.0; };
            std::vector<float> pre ((size_t) kBlock), post ((size_t) kBlock);
            const double pinc = 2.0 * juce::MathConstants<double>::pi * 1000.0 / kSampleRate;
            const double Ld   = -58.0 + (trStep % 113) * 0.5;     // step the level each block
            const double amp  = std::pow (10.0, Ld / 20.0) * std::sqrt (2.0);
            const double gain = std::pow (10.0, (compOut (Ld) - Ld) / 20.0);
            ++trStep;
            for (int i = 0; i < kBlock; ++i)
            {
                trPhase += pinc; if (trPhase > 2.0 * juce::MathConstants<double>::pi) trPhase -= 2.0 * juce::MathConstants<double>::pi;
                const float s = (float) (amp * std::sin (trPhase));
                pre[(size_t) i] = s; post[(size_t) i] = (float) (s * gain);
            }
            proc.injectDynamicsBlock (pre.data(), post.data(), kBlock);
        }
        else if (linearRender)
        {
            // White noise → one-pole LP @1kHz → 64-sample delay (a plugin that
            // both colours AND delays), straight into the measurement.
            std::vector<float> pre ((size_t) kBlock), post ((size_t) kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                const float x = rng.nextFloat() * 2.0f - 1.0f;
                lpY += lpA * (x - lpY);
                post[(size_t) i] = dl[(size_t) dp];   // lpY delayed by kDelay
                dl[(size_t) dp] = lpY;
                dp = (dp + 1) % kDelay;
                pre[(size_t) i] = x;
            }
            proc.injectMeasurementBlock (pre.data(), post.data(), kBlock);
        }
        else
        {
            juce::AudioBuffer<float> pre, post;
            float preDb = -90.0f, postDb = -90.0f;
            makeFrame (pre, post, phase, f, postGain, preDb, postDb);
            proc.injectTestCapture (pre, post, preDb, postDb);
        }

        // Let the editor's 60fps timer fire a few times so update() consumes the
        // injected capture and repaints.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        const auto img = editor->createComponentSnapshot (editor->getLocalBounds());

        juce::File outFile = outDir.getChildFile ("frame_" + juce::String (f).paddedLeft ('0', 3) + ".png");
        outFile.deleteFile();
        if (juce::FileOutputStream fos (outFile); fos.openedOk())
        {
            png.writeImageToStream (img, fos);
            ++written;
        }
    }

    editorBase = nullptr;
    std::cout << "Wrote " << written << " frames to " << outDir.getFullPathName() << "\n";
    return 0;
}
