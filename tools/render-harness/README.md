# PlugNspectrPost — UI test tooling

A standalone CMake build that compiles the PlugNspectrPost UI/processor sources
into two dev tools, **without touching the Projucer/VST3 project**:

| Target | What it is |
|---|---|
| **RenderHarness** (`PnsRenderHarness.exe`) | Headless harness — instantiates the real editor, injects synthetic audio through a test seam, ticks the 60 fps timer, and writes PNG frames. No DAW, no display needed. |
| **PnsPostStandalone** (`PlugNspectrPost.exe`) | The full UI as a launchable standalone app with its own audio device, for instant manual iteration. |

Both build from the same `PlugNspectrPost/Source/*.cpp` that ships in the VST3, so
what you see here matches the plugin.

## Prerequisites

- JUCE at `C:/Program Files/JUCE` (override with `-DJUCE_DIR=...`).
- Visual Studio 2026 (or adjust the generator).
- **Build the VST3 once first** so the Projucer-generated
  `PlugNspectrPost/JuceLibraryCode/BinaryData.{cpp,h}` (the logo) exist — they're
  `.gitignore`d, so a fresh clone must generate them via a normal plugin build.

## Configure & build

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

# Configure
& $cmake -S tools/render-harness -B tools/render-harness/build -G "Visual Studio 18 2026" -A x64

# Build everything (or pass --target RenderHarness / PnsPostStandalone_Standalone)
& $cmake --build tools/render-harness/build --config Release
```

## Run the harness

```powershell
cd tools/render-harness/out      # any working dir; frames are written to ./frames
& ..\build\RenderHarness_artefacts\Release\RenderHarness.exe dynamics 90
```

Writes `frames/frame_000.png …` — a sequence of the Dynamics tab being fed a
~-42 dBFS swelling signal with light compression. Inspect them, diff consecutive
frames to check scroll/jitter, or wire into CI as a visual-regression check.

## How it works

- `PlugNspectrPostProcessor::injectTestCapture(pre, post, preDb, postDb)` — a
  test-only seam that pushes synthetic pre/post audio + RMS into the same path
  the editor reads (the capture ring, drained via `readCaptureSince()`, plus
  `getRms()`), bypassing shared memory. Channel 0 is the derived analysis signal;
  channels 1/2 are raw L/R for the Stereo goniometer.
- `setTestPreActive(true)` forces `isPreActive()` so the "No Pre signal" overlay
  stays off.
- `PlugNspectrPostEditor::selectTabForTest(index)` selects a tab programmatically.

These seams are compiled into the normal plugin too but are inert unless called.

## Extending

- New scenarios: add cases in `Main.cpp`'s `makeFrame` (e.g. silence→signal to
  exercise calibrate/re-arm, or a hot signal to check the 1× no-zoom path).
- Other tabs: `selectTabForTest(0|2|3)` and feed `getSpectra` (would need a
  similar inject seam for the FFT path).
