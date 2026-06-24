# PlugNspectr unit tests

Deterministic [doctest](https://github.com/doctest/doctest) suites that validate
the measurement DSP against closed-form references — no DAW, no shared memory, no
editor. Two console executables, built from the same plugin sources that ship in
the VST3s:

| Target         | Covers |
|----------------|--------|
| `PnsTests`     | Post engines: Linear (mag/phase/group-delay/latency/coherence), Dynamics transfer, Attack/Release envelope, THD sweep, L/R/Mid/Side derivation + RMS, and the shared-memory ABI guard. |
| `PnsTestsPre`  | Pre stimulus generators: sine tone, white noise, level ramp, level step, THD log sweep. |

Both targets are **editor-free**: they compile only `PluginProcessor.cpp`
(`PNS_HEADLESS_TESTS` stubs out `createEditor`), so they need neither
`PluginEditor.cpp`, the Projucer-generated `BinaryData`, nor JUCE's GUI modules
beyond what `juce_audio_utils` pulls in.

## Run

```powershell
pwsh tools/tests/run-tests.ps1
```

Or manually (the targets live in the render-harness CMake project):

```powershell
cmake -S tools/render-harness -B tools/render-harness/build
cmake --build tools/render-harness/build --config Release --target PnsTests PnsTestsPre
./tools/render-harness/build/PnsTests_artefacts/Release/PnsTests.exe
./tools/render-harness/build/PnsTestsPre_artefacts/Release/PnsTestsPre.exe
```

Each exe returns a non-zero exit code on failure (doctest), so it gates CI.

## Adding tests

Add a `test_*.cpp` under `tools/tests/`, list it in the relevant target in
`tools/render-harness/CMakeLists.txt`, and use the `inject*Block()` / `get*()`
seams (Post) or the `setTestCmdBlock()` seam (Pre). Shared helpers (sample rate,
block size, bin/deg) are in `helpers.h`.

> The legacy `linear` / `dyntest` / `envtest` / `thdtest` modes in the render
> harness (`tools/render-harness/Main.cpp`) print the same numbers but don't
> assert — these doctest cases supersede them for gating.
