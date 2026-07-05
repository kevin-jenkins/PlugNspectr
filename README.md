# PlugNspectr
### by Biltroy Audio

[![Latest release](https://img.shields.io/github/v/release/kevin-jenkins/PlugNspectr?sort=semver)](https://github.com/kevin-jenkins/PlugNspectr/releases/latest)
[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%20%C2%B7%20macOS-lightgrey)
![Format](https://img.shields.io/badge/format-VST3%20%C2%B7%20AU-9cf)

> A two-plugin signal-chain analyzer for Windows (VST3) and macOS (VST3 + AU).
> Insert Pre and Post around any plugin — or a whole chain — to see exactly what it's doing to your audio.

<img src="docs/screenshots/spectrum-hero.png" width="900" alt="PlugNspectr Post — Spectrum tab analyzing live audio in Cubase" />

---

## Table of Contents

- [Overview](#overview)
- [Requirements](#requirements)
- [Installation](#installation)
- [How It Works](#how-it-works)
- [Setting Up the Signal Chain](#setting-up-the-signal-chain)
- [The UI at a Glance](#the-ui-at-a-glance)
  - [Analysis Channel](#analysis-channel)
  - [Measurement Safety](#measurement-safety)
  - [Cursor Readouts & Curve Freeze](#cursor-readouts--curve-freeze)
- [Live Analysis Tabs](#live-analysis-tabs)
  - [Spectrum](#spectrum-tab)
  - [Dynamics](#dynamics-tab)
  - [Oscilloscope](#oscilloscope-tab)
- [Measurement Tabs](#measurement-tabs)
  - [Harmonics](#harmonics-tab)
  - [Freq Response](#freq-response-tab)
  - [Compression](#compression-tab)
  - [Attack / Release](#attack--release-tab)
  - [Distortion](#distortion-tab)
- [Bottom Bar Controls](#bottom-bar-controls)
- [Tips and Best Practices](#tips-and-best-practices)
- [Building from Source](#building-from-source)
- [Architecture](#architecture)
- [Roadmap](#roadmap)

---

## Overview

PlugNspectr is a **before/after signal analyzer** that reveals exactly how any plugin — or any span of your chain — is affecting your audio. Unlike standalone analyzers, it works directly inside your DAW in real time: no test exports, no switching windows.

It consists of two companion VST3 plugins:

| Plugin | Role |
|---|---|
| **PlugNspectr Pre** | Insert before the plugin(s) you want to analyze. Captures the unprocessed reference signal and generates test stimuli on request. |
| **PlugNspectr Post** | Insert after. Hosts the analysis UI and compares the reference (Pre) signal against the processed (Post) signal. |

Both communicate via lock-free named shared memory (Windows file mapping / POSIX `shm`), passing audio between them in real time with minimal latency impact.

Two complementary kinds of analysis:

- **Live analysis** — driven by your actual program material as it plays (Spectrum, Dynamics, Oscilloscope).
- **Measurement** — Post commands Pre to inject a controlled stimulus (sine, noise, level ramp/step, swept tone) and measures the plugin's exact response: frequency/phase, compression transfer, attack/release, and distortion. Because the stimulus runs through the *real* chain in your DAW, you measure the plugin exactly as it's configured in your session.

---

## Requirements

- **OS:** Windows 10 or later (64-bit), or macOS 11+ (Apple Silicon & Intel — universal binary)
- **DAW:** Any VST3 host (developed and tested on Cubase 15 Pro); on macOS also any AU host (e.g. Logic Pro)
- **Format:** VST3 (Windows & macOS) · Audio Unit (macOS)
- **Sample Rates:** 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz
- **Window:** Resizable, with sensible minimum/maximum bounds

> **macOS status:** the build is universal and passes [pluginval](https://github.com/Tracktion/pluginval) in CI, but the Mac binaries are **not yet code-signed/notarized** and haven't been confirmed in a Mac DAW end-to-end. Treat macOS as beta for now.

---

## Installation

> **Download:** grab the latest build from the [**Releases**](https://github.com/kevin-jenkins/PlugNspectr/releases/latest) page — `PlugNspectr-1.0.0-Windows-x64.zip` contains both plugins. (Or [build from source](#building-from-source).)

1. Unzip and copy **both** plugins to your plugin folder:

   **Windows** — copy the two `.vst3` folders to:
   ```
   C:\Program Files\Common Files\VST3\
   ```

   **macOS** — copy the `.vst3` bundles to `~/Library/Audio/Plug-Ins/VST3/` and/or the
   `.component` (AU) bundles to `~/Library/Audio/Plug-Ins/Components/`:
   ```
   ~/Library/Audio/Plug-Ins/VST3/PlugNspectrPre.vst3
   ~/Library/Audio/Plug-Ins/VST3/PlugNspectrPost.vst3
   ~/Library/Audio/Plug-Ins/Components/PlugNspectrPre.component
   ~/Library/Audio/Plug-Ins/Components/PlugNspectrPost.component
   ```
   > Since the Mac builds aren't notarized yet, macOS may quarantine them — clear it with
   > `xattr -dr com.apple.quarantine <bundle>` if your DAW won't load them.

2. Open your DAW and perform a full plugin rescan.

3. Both plugins will appear under **Biltroy Audio** in your plugin manager.

> **Both must be on the same track/bus** to communicate (see below). You can install both formats on macOS; use whichever your DAW prefers (AU for Logic, VST3 for Cubase/Reaper, etc.).

---

## How It Works

PlugNspectr uses a single **named shared-memory segment** (`PlugNspectrIPC`) to pass audio between the Pre and Post plugins in real time, with a small command channel inside it so Post can ask Pre to generate test stimuli. The segment is portable — Windows file mapping or POSIX `shm` — and all synchronization is **lock-free** (a seqlock-guarded payload plus atomic heartbeats), so the audio thread never blocks.

- **PlugNspectrPre** captures each audio block in its `processBlock()` and writes it to shared memory with a heartbeat timestamp. When a measurement is armed, Pre replaces the signal with the requested stimulus so it passes through the plugin under analysis.
- **PlugNspectrPost** reads the Pre signal each block alongside its own input (the processed signal), and performs all FFT analysis, dynamics measurement, and visualization using both.
- If no Pre heartbeat is detected within 500 ms, Post shows a setup-guidance overlay.

> **Important:** Both plugins must be on the **same audio track or bus** — they cannot communicate across different tracks.

---

## Setting Up the Signal Chain

The correct insert order is:

```
Audio Signal
     ↓
[ PlugNspectr Pre ]        ← Insert here first
     ↓
[ Plugin Under Analysis ]  ← The plugin (or chain) you want to inspect
     ↓
[ PlugNspectr Post ]       ← Insert here last
     ↓
Audio Output
```

### Step by Step in Cubase

1. Open the **Insert FX** chain for your track or bus
2. Add **PlugNspectr Pre** as insert slot 1
3. Add the **plugin you want to analyze** as insert slot 2 (or several plugins — Pre/Post can wrap a whole span)
4. Add **PlugNspectr Post** as insert slot 3
5. Open **PlugNspectr Post** — the UI activates automatically when audio plays

<img width="163" height="418" alt="Insert chain in Cubase" src="https://github.com/user-attachments/assets/0360b39d-8661-48d5-8fb4-18b862d39d9f" />

> **Tip:** PlugNspectr works on any track type — audio tracks, instrument tracks, group buses, and the master bus.

### Adding Both Plugins at Once (FX Chain Preset)

A plugin can't insert its companion automatically — the DAW controls the insert chain. To add Pre and Post together in one step, save them as a **Cubase FX Chain Preset**:

1. Set up the chain once — **Pre** → plugin under analysis → **Post**
2. In the **Inserts** rack header, open the preset menu and choose **Save FX Chain Preset…**
3. Name it, e.g. `PlugNspectr Pre+Post`
4. On any future track, load that preset to drop both inserts at once

> **Tip:** Save a version with only **Pre → Post** (no middle plugin) — loading it gives you the empty analyzer pair, ready for you to drop a plugin into the slot between them.

---

## The UI at a Glance

PlugNspectr Post has **eight tabs**, grouped by how they get their signal:

- **Live analysis** (program material): **Spectrum · Dynamics · Oscilloscope**
- **Measurement** (inject a test signal): **Harmonics · Freq Response · Compression · Attack / Release · Distortion**

The measurement tabs sit on a recessed **amber tray** in the tab bar to set them apart — they're the ones that temporarily replace your audio with a test signal.

The top-right **ⓘ** icon opens an **About** box (version, plugin format, OS, and a link back to this manual).

### Analysis Channel

The header has a two-way **`L+R · Side`** selector that picks which signal *all* analysis runs on:

| Mode | Signal | What it is |
|---|---|---|
| **L+R** (default) | (L + R) / 2 | both channels combined — the whole signal (a.k.a. Mid / Mono) |
| **Side** | (L − R) / 2 | the stereo difference — what lives only in the sides |

This is **analysis-only** — your audio always passes through untouched in full stereo. Switch to **Side** to see exactly what a plugin does to the stereo image (a widener's added top-end, M/S EQ on the sides, etc.) on *any* tab — spectrum, compression curve, distortion, and so on.

### Measurement Safety

The measurement tabs replace your audio with a test signal — dangerous on a master bus. PlugNspectr makes this impossible to miss:

- An **amber border** frames the whole window and an **amber banner** appears below the tab bar whenever any stimulus is active.
- The **Measure** / **Test Tone** buttons turn solid amber while armed.
- The stimulus **auto-stops** when you **leave the tab** or **stop DAW playback** — so a tone can't keep blasting after you stop the transport.

Throughout, the color language is consistent: **teal = your current selection**, **amber = test signal touching your audio**.

### Cursor Readouts & Curve Freeze

- **Cursor readout:** hover any measurement plot for a hairline, a dot on the curve, and a value chip reading the exact value at that point (frequency + magnitude/phase/group-delay, input→output level, time→gain-reduction, or frequency→THD%). **Click to lock** it; click again or move off to release.
- **Freeze (A/B):** the Freq Response, Compression, and Distortion tabs have a **Freeze** button that snapshots the current curve as a dimmed reference overlaid under the live measurement — drop-in plugin A, freeze, swap to plugin B, and compare directly.

---

## Live Analysis Tabs

### Spectrum Tab

<img src="docs/screenshots/spectrum.png" width="820" alt="Spectrum tab" />

Real-time FFT frequency analysis showing how the plugin is shaping your audio's frequency content.

**What you see:**
- **Pre signal** (lavender, dimmed) — the unprocessed spectrum
- **Post signal** (bright teal, glowing) — the processed spectrum
- **Pre / Post Averages** — 10-second rolling averages, with an amber fill between them showing the cumulative EQ difference

**Controls (top right):** a **Reset** icon (circular arrow), smoothing speed (**Fast / Medium / Slow**), and an **Avg** toggle for the average lines. **Reset** clears the rolling averages and the fluctuation markers so the averaged Pre↔Post comparison reflects only post-tweak audio — the "start over after a plugin change" companion to the same control on the Dynamics tab.

**Interactive inspection:** hover for a hairline + tooltip (frequency, Pre dB, Post dB); click to lock, click again to unlock.

**Frequency Variance Markers:** after ~3 seconds, up to **5 floating markers** highlight where the Pre and Post *average* curves diverge most — i.e. where the plugin is making its biggest EQ changes over time. Clustered markers indicate a broad shelf/resonance; a `↑` boost marker indicates a frequency synthesized or heavily boosted from near-silence. Markers are based on the rolling 10-second averages, so let audio run 10+ seconds for accurate positions.

**Axis:** X = 20 Hz–20 kHz (log), Y = −90 dB to +12 dB.

---

### Dynamics Tab

<img src="docs/screenshots/dynamics.png" width="820" alt="Dynamics tab" />

Reveals how the plugin is affecting dynamic range and level in real time.

**Waveform comparison (top):** Pre (soft sand, background) vs Post (bright teal, foreground) overlaid; where Post is smaller than Pre, compression/limiting is occurring, highlighted by the teal fill. Time window: **6s** or **12s** (default).

**Reset (circular-arrow icon, top right):** clears all readings and history — waveform, Avg GR, the 30 s rolling average, GR history, and the In/Out levels — so measurement starts over after you tweak the plugin chain. The auto-zoom is kept so the view doesn't jump.

**Readouts (right panel):**
| Readout | Description |
|---|---|
| **Avg GR** | Average gain reduction over the last 30 s (amber) |
| **Now** | Instantaneous gain reduction — holds last non-zero value (teal) |
| **In / Out** | RMS input / output level (dB) |
| **Δ** | Net level difference (orange = attenuation, teal = gain, grey = unity) |

> **Double-click** the readout panel (Avg GR / Now) for the same full reset as the **Reset** button.

**Gain Reduction (bottom):** a scrolling GR history with an orange gradient fill and a peak-hold line. Y = 0 to −24 dB; X = last 3 seconds. Flat at 0 = dynamically transparent; deep consistent dips = heavy compression; brief dips = peak limiting.

---

### Oscilloscope Tab

<img src="docs/screenshots/oscilloscope.png" width="820" alt="Oscilloscope tab" />

A zero-crossing-triggered oscilloscope of both signals — useful for phase shifts, transient shaping, clipping, and saturation character.

- **Pre** (lavender, receding) vs **Post** (bright teal) overlaid; triggering locks to a rising zero-crossing in the Post signal (auto-trigger style, non-swimming).
- Time windows: **10ms** (individual cycles / transients), **50ms** (default, attack/release), **100ms** (slower events).
- Look for: lines overlaid = transparent; smaller Post = gain reduction; shifted Post = latency/phase; different shape = harmonic coloration; flat tops = clipping.

---

## Measurement Tabs

> All measurement tabs inject a test signal through the plugin under analysis. The amber border/banner and auto-stop ([Measurement Safety](#measurement-safety)) are active whenever they're running.

### Harmonics Tab

<img src="docs/screenshots/harmonics.png" width="820" alt="Harmonics tab — harmonic spectrum with the measuring-safety banner active" />

Harmonic-distortion analysis using a pure sine test tone. Identifies the character and intensity of even and odd harmonics H2–H8.

- **FREQ** (footer) sets the tone frequency; **LEVEL** (footer) sets its level; **Test Tone** arms it.
- **Pre** spectrum shows the clean fundamental; **Post** shows the plugin's output — any added frequencies are harmonics. Vertical markers sit at H1–H8 with colored diamonds at each peak.
- **Readouts:** THD Pre %, THD Post %, and individual H2–H8 levels (Pre/Post). Values below −60 dB show as `---`.
- **Even harmonics** (H2/H4/H6/H8) → warm, tube/tape character; **odd** (H3/H5/H7) → harsher, transistor/digital character. THD Post near 0% = clean; 1–5% = subtle coloration; >10% = significant saturation.

---

### Freq Response Tab

<img src="docs/screenshots/freq-response.png" width="820" alt="Freq Response tab — magnitude, phase, group delay" />

The full **linear transfer function** of the plugin, measured with a white-noise stimulus (cross-spectrum H1 = Pre→Post). Three stacked plots:

- **Magnitude (dB)** — the exact frequency response (boosts/cuts), beyond what the live Spectrum averaging can show.
- **Phase (deg)** — phase response, with bulk latency de-rotated out.
- **Group Delay (ms)** — frequency-dependent delay.

A **Latency** readout reports the measured Pre→Post delay in samples / ms. Hover for the cursor readout (frequency + magnitude/phase/group-delay at the hairline); **Freeze** to A/B against another plugin or setting.

---

### Compression Tab

<img src="docs/screenshots/compression.png" width="820" alt="Compression tab — static transfer curve" />

The **static transfer curve** (output level vs input level), measured by Pre sweeping a tone's level from −60 to 0 dBFS while Post bins output against input. Reveals threshold, ratio, and knee at a glance:

- A straight 1:1 diagonal = no compression; a bend = the knee; a shallower slope above it = the ratio; downward expansion/gating shows as a steeper region below the knee.
- Cursor readout shows **input → output + gain reduction** at the hairline. **Freeze** for A/B comparison.

---

### Attack / Release Tab

<img src="docs/screenshots/attack-release.png" width="820" alt="Attack / Release tab — gain reduction vs time" />

The measured **time response** — gain reduction vs time — captured with a level-step stimulus (Pre alternates a loud/quiet 1-second cycle) and synchronously averaged. The curve shows the **attack** ramp when the level jumps up and the **release** decay when it drops, so you can read a compressor's real attack/release behavior. Cursor readout shows **time → gain reduction** at the hairline.

---

### Distortion Tab

<img src="docs/screenshots/distortion.png" width="820" alt="Distortion tab — THD vs frequency" />

**THD vs frequency** — total harmonic distortion measured as Pre sweeps a stepped tone across the spectrum (50 Hz → 5 kHz, log). Shows how a plugin's distortion changes with frequency (e.g. more harmonic generation in the lows from tape/tube emulations), plotted log-log. Cursor readout shows **frequency → THD %**; **Freeze** for A/B comparison.

---

## Bottom Bar Controls

The footer hosts the test-tone controls and global level trim:

| Control | Function |
|---|---|
| **FREQ** | Test-tone frequency (100 Hz – 8000 Hz, default 1000 Hz) |
| **LEVEL** | Test-tone level (dBFS) |
| **Test Tone** | Arms / disarms the sine test tone (turns amber while active) |
| **IN trim** | −24…+24 dB — scales the Pre signal for analysis only (does not affect audio) |
| **OUT trim** | −24…+24 dB — scales the actual Post audio output |

> **Double-click** a trim slider to reset to 0.0 dB. Trims display grey at 0 dB and teal when active.

> **FREQ / LEVEL / Test Tone** only apply where they're used — they're active on the live tabs and **Harmonics** (with **LEVEL** also on **Distortion**), and dim out on the other measurement tabs, which are driven entirely by their **Measure** button.

---

## Tips and Best Practices

### General
- Insert Pre **before** the plugin and Post **after** — order matters; both on the **same track/bus**.
- **Live** tabs need audio playing. **Measurement** tabs generate their own stimulus, but the transport must be running (audio has to flow through the chain) — and they auto-stop when you leave the tab or stop playback.
- Use the **L+R / Side** selector to check the whole signal vs the stereo sides independently (works on every tab).
- Save the Pre/Post pair as an **FX Chain Preset** to add both inserts at once.

### Live tabs
- **Spectrum:** use **Slow** smoothing for subtle EQ coloration; let audio run 10+ seconds before reading the averages and variance markers.
- **Dynamics:** the **30-second Avg GR** is the most musically meaningful compression reading; use **12s** for compressors and **6s** for limiters; the **Δ** readout flags non-unity gain.
- **Oscilloscope:** **10ms** for individual transients, **50ms** for compressor attack/release, **100ms** for longer passages.

### Measurement tabs
- **Freq Response** is the most accurate way to see a plugin's exact EQ and phase — far more precise than live spectrum averaging.
- Use **Freeze** to A/B two plugins (or two settings) on Freq Response, Compression, and Distortion.
- **Harmonics:** start with the default 1 kHz tone for comparable readings; lower the frequency to inspect higher harmonics on plugins that generate many overtones.

---

## Building from Source

### Prerequisites
- [JUCE](https://juce.com) 8.x (the CMake build takes `-DJUCE_DIR=<path>`)
- **Windows:** Visual Studio 2022/2026 (C++ Desktop Development) + Windows 10/11 SDK
- **macOS:** Xcode 15+ (command-line tools) + CMake 3.22+

### Cross-platform build (CMake) — recommended

The top-level `CMakeLists.txt` builds **both plugins** for the current OS — **VST3** everywhere, **AU** additionally on macOS, as a **universal** (arm64 + x86_64) binary on Mac. The editor is BinaryData-free, so no Projucer step is needed:

```bash
cmake -B build -DJUCE_DIR="/path/to/JUCE"
cmake --build build --config Release
# -> build/PlugNspectr{Pre,Post}_artefacts/Release/{VST3,AU}/...
```

A manually-triggered GitHub Actions workflow (`.github/workflows/macos.yml`) builds the universal Mac VST3/AU and validates them with [pluginval](https://github.com/Tracktion/pluginval); run it from the Actions tab or `gh workflow run "macOS build"`.

### Windows (Projucer / Visual Studio) — alternative

The `.jucer` projects are kept for the existing local Windows workflow:

1. Open each `.jucer` in **Projucer** and click **Save Project** once (generates the git-ignored `JuceLibraryCode/` + `BinaryData`).
2. Open each `Builds/VisualStudio2026/*.sln`, build **Release | x64**, then copy the `.vst3` folders to `C:\Program Files\Common Files\VST3\`.

> Re-saving the `.jucer` regenerates the build files — avoid it unless necessary; the CMake build above is the simpler cross-platform path.

### Development tooling (CMake)

`tools/render-harness/` builds, via CMake against your JUCE install, two extra targets that never touch the Projucer projects:

- **`RenderHarness`** — a headless console app that drives the real Post editor with synthetic audio and dumps PNG frames (used to generate this README's screenshots and to eyeball UI changes without a DAW). Needs the plugin's `BinaryData` generated once first.
- **`PnsPostStandalone`** — the full Post UI as a launchable `.exe` with its own audio device, for instant manual iteration.

```bash
cmake -S tools/render-harness -B tools/render-harness/build -DJUCE_DIR="C:/Program Files/JUCE"
cmake --build tools/render-harness/build --config Release --target RenderHarness
```

### Tests

`tools/tests/` holds [doctest](https://github.com/doctest/doctest) unit suites that validate the measurement DSP against closed-form references — **editor-free**, so they build straight from the processor sources with no Projucer/BinaryData needed:

```powershell
powershell -File tools/tests/run-tests.ps1
```

A tracked **pre-push hook** runs the suites before every push; enable it once per clone with `powershell -File tools/tests/install-hooks.ps1`. See [tools/tests/README.md](tools/tests/README.md) for details.

### Key technical details

| Detail | Value |
|---|---|
| Shared segment | `PlugNspectrIPC` — one segment carrying audio (Pre→Post) + commands (Post→Pre) |
| IPC sync | Lock-free seqlocks + atomic heartbeats (wait-free audio thread); Win32 file mapping / POSIX `shm` |
| Pre / Post plugin IDs | `BNSP` / `BNSQ` (manufacturer `Bilt`) |
| Spectrum FFT | 2048-point, Hann window (order 11) |
| Measurement FFT | 4096-point (magnitude / phase / group-delay, THD) |
| Latency measurement | PHAT cross-correlation of the Pre→Post impulse |
| Spectrum smoothing | Exponential moving average (0.6 / 0.85 / 0.95) |
| Target frame rate | 60 fps |
| Heartbeat timeout | 500 ms |

---

## Architecture

```
┌──────────────────────── DAW Audio Thread ────────────────────────┐
│                                                                   │
│  ┌──────────────┐    ┌──────────────┐                             │
│  │PlugNspectrPre│───▶│ Your Plugin  │───▶ (to) PlugNspectrPost    │
│  │processBlock()│    │processBlock()│                             │
│  └──────┬───────┘    └──────────────┘         ┌──────────────┐    │
│         │ write Pre signal / read commands     │PlugNspectrPost│   │
│         ▼                                       │processBlock() │   │
│  ┌──────────────────────────┐  audio  ─────────└──────┬────────┘   │
│  │  Shared segment           │─────────────────────────┘            │
│  │  "PlugNspectrIPC"         │◀── commands (Post → Pre: stimulus)   │
│  │  (Win mmap / POSIX shm)   │     lock-free seqlocks + heartbeats   │
│  └──────────────────────────┘                                      │
└───────────────────────────────────────────────┬───────────────────┘
                                                 │ UI thread (60 fps)
                                          ┌──────▼────────────────┐
                                          │   PluginEditor         │
                                          │  Live: Spectrum,       │
                                          │        Dynamics, Scope │
                                          │  Measure: Harmonics,   │
                                          │   Freq Response,       │
                                          │   Compression,         │
                                          │   Attack/Release,      │
                                          │   Distortion           │
                                          └────────────────────────┘
```

---

## Roadmap

### Shipped
- Linear measurement — magnitude / phase / group delay + latency detection
- Static compression transfer curve
- Measured attack / release envelope
- THD-vs-frequency distortion sweep
- Curve **Freeze** (A/B) on Freq Response, Compression, Distortion
- **L+R / Side** channel analysis (whole signal + stereo difference)
- Measurement-safety system (amber border/banner, armed buttons, auto-stop on tab change or transport stop)
- Cursor readouts on all measurement plots
- About box (info icon → version / format / OS / manual)
- Resizable window
- Headless render harness + standalone build + unit-test suites
- **Cross-platform: macOS (VST3 + AU, universal) via portable lock-free IPC** — building + pluginval-validated in CI (beta; see macOS status above)
- First release — **v1.0.0** (Windows x64 VST3)

### Planned
- IMD (two-tone intermodulation) and THD+N
- Selectable FFT size; optional oversampling / aliasing view
- Linear-vs-minimum-phase readout
- Stereo field comparison (Lissajous display)
- Performance tab — CPU and latency overview
- macOS: code-signing / notarization + real-DAW (Logic/Cubase) confirmation

---

## License

PlugNspectr is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License v3.0** as published by the Free Software Foundation.

See [LICENSE](LICENSE) for the full license text.

© 2026 Biltroy Audio

---

*PlugNspectr is developed by Biltroy Audio using JUCE and C++.*
*Built with ❤️ for mixing / mastering engineers and producers who want to measure and see what their plugins are actually doing.*
