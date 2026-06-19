// ───────────────────────────────────────────────────────────────────────────
// PlugNspectrPost headless render harness
//
// Instantiates the real plugin editor, feeds it synthetic audio through the
// processor's test seam, ticks the editor's 60fps timer, and writes a sequence
// of PNG frames — all without a DAW or a live PlugNspectrPre.
//
// Usage:  PnsRenderHarness [scenario] [numFrames]
//   scenario: "dynamics" (default) — quiet mix-bus-level signal with swelling
//             dynamics and light compression, to exercise the waveform + GR.
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
// the post signal lightly attenuated to stand in for compression.
void makeFrame (juce::AudioBuffer<float>& pre, juce::AudioBuffer<float>& post,
                double& phase, int frameIdx, float& preDb, float& postDb)
{
    pre .setSize (1, kBlock, false, false, true);
    post.setSize (1, kBlock, false, false, true);

    const float env  = 0.008f * (1.0f + 0.8f * std::sin (frameIdx * 0.12f));  // ≈ -42 dB, swelling
    const float freq = 220.0f;

    double preSq = 0.0, postSq = 0.0;
    for (int i = 0; i < kBlock; ++i)
    {
        const float s  = env * std::sin ((float) phase);
        phase += juce::MathConstants<double>::twoPi * freq / kSampleRate;
        const float ps = s;
        const float qs = s * 0.85f;   // post a touch quieter → visible GR

        pre .setSample (0, i, ps);
        post.setSample (0, i, qs);
        preSq  += (double) ps * ps;
        postSq += (double) qs * qs;
    }

    preDb  = 20.0f * std::log10 (juce::jmax ((float) std::sqrt (preSq  / kBlock), 1.0e-6f));
    postDb = 20.0f * std::log10 (juce::jmax ((float) std::sqrt (postSq / kBlock), 1.0e-6f));
}
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const int numFrames = (argc > 2) ? juce::String (argv[2]).getIntValue() : 90;

    juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile ("frames");
    outDir.createDirectory();

    PlugNspectrPostProcessor proc;
    proc.prepareToPlay (kSampleRate, kBlock);
    proc.setTestPreActive (true);

    std::unique_ptr<juce::AudioProcessorEditor> editorBase (proc.createEditor());
    auto* editor = dynamic_cast<PlugNspectrPostEditor*> (editorBase.get());
    if (editor == nullptr) { std::cerr << "Failed to create editor\n"; return 1; }

    editor->setSize (1000, 640);
    editor->selectTabForTest (1);   // Dynamics tab

    juce::PNGImageFormat png;
    double phase = 0.0;
    int written = 0;

    for (int f = 0; f < numFrames; ++f)
    {
        juce::AudioBuffer<float> pre, post;
        float preDb = -90.0f, postDb = -90.0f;
        makeFrame (pre, post, phase, f, preDb, postDb);
        proc.injectTestCapture (pre, post, preDb, postDb);

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
