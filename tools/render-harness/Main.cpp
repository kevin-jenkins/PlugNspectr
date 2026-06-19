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

    std::unique_ptr<juce::AudioProcessorEditor> editorBase (proc.createEditor());
    auto* editor = dynamic_cast<PlugNspectrPostEditor*> (editorBase.get());
    if (editor == nullptr) { std::cerr << "Failed to create editor\n"; return 1; }

    // "linear-render" renders the Linear tab fed a known one-pole low-pass.
    const bool linearRender = (argc > 1 && juce::String (argv[1]) == "linear-render");

    editor->setSize (1000, 640);
    editor->selectTabForTest (linearRender ? 4 : 1);

    juce::PNGImageFormat png;
    double phase = 0.0;
    int written = 0;
    juce::Random rng;
    const float lpA = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 1000.0 / kSampleRate));
    float lpY = 0.0f;
    constexpr int kDelay = 64;                 // bulk latency to exercise compensation
    std::array<float, kDelay> dl {}; int dp = 0;

    for (int f = 0; f < numFrames; ++f)
    {
        if (linearRender)
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
