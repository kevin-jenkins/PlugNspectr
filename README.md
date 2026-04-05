# PlugNspectr
### by Biltroy Audio

> A two-plugin VST3 signal chain analyzer for Windows and DAWs that support VST3 plugins.  
> Insert Pre and Post around any plugin to see exactly what it's doing to your audio.

<img width="1509" height="677" alt="Screenshot 2026-03-30 001506" src="https://github.com/user-attachments/assets/da8dd478-2d7e-4785-841a-58ddf7aa0320" />

---

## Table of Contents

- [Overview](#overview)
- [Requirements](#requirements)
- [Installation](#installation)
- [How It Works](#how-it-works)
- [Setting Up the Signal Chain](#setting-up-the-signal-chain)
- [Analysis Tabs](#analysis-tabs)
  - [Spectrum](#spectrum-tab)
    - [Frequency Variance Markers](#frequency-variance-markers)
  - [Dynamics](#dynamics-tab)
  - [Oscilloscope](#oscilloscope-tab)
  - [Harmonics](#harmonics-tab)
- [Bottom Bar Controls](#bottom-bar-controls)
- [Tips and Best Practices](#tips-and-best-practices)
- [Building from Source](#building-from-source)
- [Architecture](#architecture)
- [Roadmap](#roadmap)

---

## Overview

PlugNspectr is a **before/after signal analyzer** that reveals exactly how any plugin in your signal chain is affecting your audio. Unlike standalone analyzers, PlugNspectr works directly inside your DAW in real time — no test exports, no switching windows.

It consists of two companion VST3 plugins:

| Plugin | Role |
|---|---|
| **PlugNspectr Pre** | Insert before the plugin you want to analyze. Captures the unprocessed signal. |
| **PlugNspectr Post** | Insert after the plugin you want to analyze. Hosts the analysis UI and compares both signals. |

Both plugins communicate via Windows shared memory, passing audio data between them in real time with minimal latency impact.

---

## Requirements

- **OS:** Windows 10 or later (64-bit)
- **DAW:** Any VST3-compatible host (developed and tested on Cubase 15 Pro)
- **Format:** VST3 (64-bit only)
- **Sample Rates:** 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz

---

## Installation

1. Copy both `.vst3` folders to your VST3 plugin directory:
```
C:\Program Files\Common Files\VST3\
```

Your directory should contain:
```
VST3\
  PlugNspectrPre.vst3\
    Contents\
      x86_64-win\
        PlugNspectrPre.dll
  PlugNspectrPost.vst3\
    Contents\
      x86_64-win\
        PlugNspectrPost.dll
```

2. Open your DAW and perform a full plugin rescan.

3. Both plugins will appear under **Biltroy Audio** in your plugin manager.

---

## How It Works

PlugNspectr uses **Windows Named Shared Memory** (`BiltroyPlugNspectrShared`) to pass audio buffer data between the Pre and Post plugins in real time.

- **PlugNspectrPre** captures each audio block in its `processBlock()` callback and writes it to shared memory along with a heartbeat timestamp.
- **PlugNspectrPost** reads the Pre signal from shared memory each block alongside its own input (the Post-processed signal).
- The Post plugin performs all FFT analysis, dynamics measurement, and visualization using both signals simultaneously.
- If no Pre plugin heartbeat is detected within 500ms, the Post plugin displays a setup guidance overlay.

> **Important:** Both plugins must be on the **same audio track or bus**. They cannot communicate across different tracks.

---

## Setting Up the Signal Chain

The correct insert order is:

```
Audio Signal
     ↓
[ PlugNspectr Pre ]        ← Insert here first
     ↓
[ Plugin Under Analysis ]  ← The plugin you want to inspect
     ↓
[ PlugNspectr Post ]       ← Insert here last
     ↓
Audio Output
```

### Step by Step in Cubase

1. Open the **Insert FX** chain for your track or bus
2. Add **PlugNspectr Pre** as insert slot 1
3. Add the **plugin you want to analyze** as insert slot 2
4. Add **PlugNspectr Post** as insert slot 3
5. Open **PlugNspectr Post** — the UI will activate automatically when audio plays

<img width="163" height="418" alt="Screenshot 2026-03-29 221243" src="https://github.com/user-attachments/assets/0360b39d-8661-48d5-8fb4-18b862d39d9f" />

> **Tip:** PlugNspectr works on any track type — audio tracks, instrument tracks, group buses, and the master bus.

---

## Analysis Tabs

PlugNspectr Post contains four analysis tabs accessible via the tab bar at the top of the UI.

---

### Spectrum Tab

<img width="805" height="603" alt="Screenshot 2026-03-30 001159" src="https://github.com/user-attachments/assets/19587bfc-f711-4de4-9d7a-180da01f151e" />

Real-time FFT frequency analysis showing how the plugin under analysis is shaping the frequency content of your audio.

**What you see:**
- **Pre signal** (lavender, dimmed) — the unprocessed frequency spectrum
- **Post signal** (bright teal, glowing) — the processed frequency spectrum
- **Pre Average** (light lavender line) — 10-second rolling average of the Pre spectrum
- **Post Average** (amber line, glowing) — 10-second rolling average of the Post spectrum
- **Average fill** — amber fill between the two average lines showing the cumulative EQ difference

**Controls (top right):**
| Control | Function |
|---|---|
| **Medium dropdown** | Smoothing speed — Fast / Medium / Slow |
| **Avg button** | Toggle average lines on/off |

**Interactive frequency inspection:**
- **Hover** over the spectrum to see a vertical hairline and floating tooltip showing the exact frequency, Pre dB, and Post dB at that point
- **Click** to lock the tooltip in place
- **Click again** to unlock

**Reading the display:**
- Where the Post curve sits **above** the Pre curve → the plugin is boosting that frequency range
- Where the Post curve sits **below** the Pre curve → the plugin is cutting that frequency range
- The average fill shows the **cumulative EQ character** over time — useful for seeing subtle coloration from compressors, tape plugins, and saturators

**Axis:**
- X: 20 Hz – 20 kHz (logarithmic)
- Y: -90 dB to +12 dB (0 dB reference line highlighted)

---

#### Frequency Variance Markers

After 3 seconds of audio, the Spectrum tab automatically places up to **5 floating markers** on the display identifying the frequency regions where the Pre and Post *average* curves diverge most. These highlight where the plugin is making its most significant EQ changes over time.

**What you see:**
- **Amber pill labels** — show the exact frequency (e.g. `2.4 kHz` or `340 Hz`) of each flagged region
- **Vertical stems with dots** — the dot rests directly on the Post average curve at that frequency; the stem extends upward to the pill
- **Boost markers** — pills marked with `↑` indicate a frequency where the plugin is generating content that wasn't present in the Pre signal (Post has signal, Pre is below the noise floor)
- **Two-row stagger** — when markers would overlap, the lower-priority one drops to a second row to keep labels readable

**How markers are selected:**

The five marker slots each serve a distinct purpose:

| Slot | Region | Purpose |
|---|---|---|
| **1** | 20 – 200 Hz | Best low-frequency variance — always reserved for the low end |
| **2** | Full range | Boost detection — fires when the plugin adds content at a frequency below the Pre noise floor |
| **3 – 5** | 200 Hz – 20 kHz | The three highest-variance bins in the mid/high range, spaced at least 80 px apart |

**Behavior:**
- Markers fade in over ~0.5 seconds and fade out over ~1 second when they change
- A new candidate frequency needs to show **20% more variance** than the current marker to displace it — this prevents rapid jitter on broad, flat EQ curves
- Bins where both Pre and Post are below -85 dB are excluded (treated as noise)
- Markers slide smoothly to a new frequency over ~2 seconds rather than jumping instantly

**Reading the markers:**
- **Multiple markers clustered in the same region** → the plugin has a broad EQ shelf or resonance centered there
- **A single isolated marker in the highs with no others** → a narrow high-frequency cut or boost (e.g. an air shelf, de-esser band)
- **A boost marker `↑`** → the plugin is synthesizing or heavily boosting a frequency that was essentially absent in the input — common with resonant filters, exciters, or heavy compression pumping artifacts
- **No markers appearing** → the plugin is spectrally transparent on average — averages are very close across all frequencies

> Markers are based on the **rolling 10-second averages**, not the live spectrum. They reflect the plugin's cumulative EQ character, not momentary peaks. Let the audio run for 10+ seconds for the most accurate marker positions.

---

### Dynamics Tab

<img width="803" height="601" alt="Screenshot 2026-03-30 001300" src="https://github.com/user-attachments/assets/97befaf4-ca1c-42e9-be73-d8d0d3279b2d" />

Reveals how the plugin is affecting the dynamic range and overall level of your audio in real time.

#### Waveform Comparison (top section)

A scrolling time-domain waveform showing both signals overlaid:

- **Pre waveform** (soft cream/sand, 35% opacity) — the original unprocessed waveform in the background
- **Post waveform** (bright teal, 85% opacity) — the processed waveform in the foreground
- Where the Post waveform is **smaller** than Pre → compression or limiting is occurring
- The **teal fill** between waveforms highlights the compressed region

**Time window controls (top right):**
| Button | Window |
|---|---|
| **6s** | 6-second scrolling window |
| **12s** | 12-second scrolling window (default) |

#### Readouts (top right panel)

| Readout | Description |
|---|---|
| **Avg GR** | Average gain reduction over the last 30 seconds (amber) |
| **Now** | Instantaneous gain reduction — holds last non-zero value (teal) |
| **In** | RMS input level in dB |
| **Out** | RMS output level in dB |
| **Δ** | Net level difference (orange = attenuation, teal = gain, grey = unity) |

> **Double-click** on either the Avg GR or Now label/value to reset all readings.

#### Gain Reduction (bottom section)

A scrolling gain reduction history graph:

- **Orange gradient fill** shows gain reduction over time
- **Peak hold line** marks the maximum gain reduction seen
- Y-axis: 0 to -24 dB in 3 dB increments
- X-axis: last 3 seconds of history

**Reading the display:**
- A flat line at 0 dB → no gain reduction (plugin is transparent dynamically)
- Dips below 0 dB → the plugin is reducing gain (compression, limiting)
- Deep consistent dips → heavy compression
- Occasional brief dips → peak limiting

---

### Oscilloscope Tab

<img width="805" height="601" alt="Screenshot 2026-03-30 001358" src="https://github.com/user-attachments/assets/653e5eba-001c-421f-95d8-9150aaf1b87b" />

A zero-crossing triggered oscilloscope showing the time-domain waveform of both signals. Useful for detecting phase shifts, transient shaping, clipping, and saturation character.

**What you see:**
- **Pre signal** (lavender, 40% opacity, 1px) — receding background reference
- **Post signal** (bright teal, glowing, 2px) — the processed waveform

**How triggering works:**
The oscilloscope locks to a **rising zero-crossing** in the Post signal above a minimum threshold. This keeps the waveform stable and non-swimming — similar to a hardware oscilloscope in auto-trigger mode.

**Time window controls (top right):**
| Button | Window |
|---|---|
| **10ms** | Tight zoom — see individual wave cycles and transient shape |
| **50ms** | Medium zoom (default) — good for seeing attack/release behavior |
| **100ms** | Wide zoom — see slower dynamic events |

**Sample rate display:**
Current sample rate shown in the top right (e.g. "44.1 kHz").

**What to look for:**
- **Pre and Post lines perfectly overlaid** → plugin is transparent (no time domain effect)
- **Post line smaller amplitude** → gain reduction or limiting
- **Post line shifted left or right** → plugin is introducing latency or phase shift
- **Post line shows different shape** → harmonic coloration or saturation changing the waveform character
- **Flat tops on Post** → the plugin is clipping your signal

---

### Harmonics Tab

<img width="805" height="597" alt="Screenshot 2026-03-30 001448" src="https://github.com/user-attachments/assets/1631c824-01e5-455b-a2d3-c97c82f76d57" />


Analyzes harmonic distortion introduced by the plugin under analysis using a pure test tone. Identifies the character and intensity of even and odd harmonics from H2 through H8.

> **⚠ Important:** Activating the test tone **replaces your audio output** with a sine wave. Always deactivate before resuming normal playback.

#### Controls (top left)

| Control | Function |
|---|---|
| **FREQ knob** | Sets the test tone frequency (100 Hz – 8000 Hz, default 1000 Hz) |
| **Test Tone button** | Activates/deactivates the sine wave test tone |

#### Harmonic Spectrum Display

When the test tone is active the display shows:

- **Pre spectrum** (lavender, ghosted) — the input sine wave, should show only the fundamental
- **Post spectrum** (bright teal, glowing) — what comes out of the plugin — any additional frequencies are harmonics added by the plugin
- **Harmonic markers** — vertical dotted lines at H1 through H8 positions
- **Diamond indicators** — colored diamonds at each harmonic peak, graduating from cyan (H2) to pink (H8)

#### Readouts (top right panel)

| Readout | Description |
|---|---|
| **THD Pre %** | Total Harmonic Distortion of the input signal (should be near 0%) |
| **THD Post %** | Total Harmonic Distortion of the output — how much distortion the plugin adds |
| **H2 – H8** | Individual harmonic levels for Pre and Post in dB |

Values below -60 dB show as `---` (below measurement floor).

**Reading the harmonics:**
- **Even harmonics (H2, H4, H6, H8)** → warm, musical character — typical of tube and tape saturation
- **Odd harmonics (H3, H5, H7)** → harsher, more aggressive — typical of transistor and digital clipping
- **THD Post near 0%** → plugin is clean and transparent
- **THD Post 1-5%** → subtle harmonic coloration
- **THD Post > 10%** → significant saturation or distortion character

**Choosing the test frequency:**
- **1000 Hz (default)** → industry standard for THD testing, all harmonics fall within 20 kHz
- **Lower frequencies** → gives more headroom for higher harmonics before they exceed 20 kHz
- **Higher frequencies** → upper harmonics may exceed 20 kHz and won't be measured

---

## Bottom Bar Controls

The bottom bar provides global level trim controls:

| Control | Range | Default | Function |
|---|---|---|---|
| **IN trim** | -24 dB to +24 dB | 0.0 dB | Scales the Pre signal level for analysis purposes only — does not affect audio passing through |
| **OUT trim** | -24 dB to +24 dB | 0.0 dB | Scales the actual Post audio output level |

> **Double-click** either slider to reset to 0.0 dB.

Values display in grey at 0 dB and switch to teal when non-zero so you always know when a trim is active.

---

## Tips and Best Practices

### General
- Always insert Pre **before** the plugin under analysis and Post **after** — the order matters
- Both plugins must be on the **same track or bus**
- Audio must be **playing** for the analysis to update — the plugins only process audio in real time

### Spectrum Tab
- Use **Slow** smoothing when analyzing subtle EQ coloration from compressors or tape plugins — Fast smoothing can mask gentle curves
- The **average lines** are most useful for revealing the cumulative character of a plugin over a full mix — let audio play for 10+ seconds before reading them
- **Click to lock** the frequency tooltip when you want to note a specific frequency value without it jumping around
- **Frequency variance markers** appear after ~3 seconds and update continuously — the marker positions tell you *where* the plugin is working hardest; use them as a starting point for further EQ investigation

### Dynamics Tab
- The **30-second Avg GR** is the most musically meaningful compression reading — it tells you how hard the compressor is working on average across a full musical phrase
- Use the **12s waveform window** for compressors and the **6s window** for limiters and faster transient processors
- The **Δ readout** quickly tells you if a plugin is unity gain — if it shows anything other than 0.0 dB the plugin is changing your overall level

### Oscilloscope Tab
- Use **10ms** to inspect how a plugin affects individual transients — kick drums, snare hits, pick attacks
- Use **50ms** to see attack and release curves on compressors
- Use **100ms** to see how a limiter handles a longer passage

### Harmonics Tab
- Always start with the **default 1kHz test tone** for consistent, comparable readings
- A **THD Post below 0.1%** means the plugin is essentially linear — no meaningful harmonic coloration
- Compare plugins by noting their THD% and whether their harmonics are predominantly even or odd — this tells you their fundamental sonic character
- **Lower the test tone frequency** if you want to inspect higher harmonics on plugins that generate many overtones

---

## Building from Source

### Prerequisites

- [JUCE](https://juce.com) (latest version)
- Visual Studio Community 2022 or later with C++ Desktop Development workload
- Windows 10 SDK

### Build Steps

1. Clone the repository:
```bash
git clone https://github.com/YOUR_USERNAME/PlugNspectr.git
cd PlugNspectr
```

2. Open **Projucer** and open each `.jucer` file:
   - `PlugNspectrPre/PlugNspectrPre.jucer`
   - `PlugNspectrPost/PlugNspectrPost.jucer`

3. In Projucer click **Save Project** to regenerate build files and BinaryData.

4. Open each `.sln` file in Visual Studio:
   - `PlugNspectrPre/Builds/VisualStudio2026/PlugNspectrPre.sln`
   - `PlugNspectrPost/Builds/VisualStudio2026/PlugNspectrPost.sln`

5. Set configuration to **Release x64** and build both solutions.

6. Copy the built `.vst3` folders to:
```
C:\Program Files\Common Files\VST3\
```

### Project Structure

```
PlugNspectr/
├── PlugNspectrPre/
│   ├── Source/
│   │   ├── PluginProcessor.cpp     # Audio processing + shared memory write
│   │   ├── PluginProcessor.h
│   │   └── SharedMemoryBlock.h     # Shared memory struct definition
│   └── PlugNspectrPre.jucer
│
├── PlugNspectrPost/
│   ├── Source/
│   │   ├── PluginProcessor.cpp     # Audio processing + shared memory read
│   │   ├── PluginProcessor.h
│   │   ├── PluginEditor.cpp        # All UI and visualization
│   │   ├── PluginEditor.h
│   │   ├── PlugNspectrTheme.h      # Design system — colors, fonts, utilities
│   │   └── SharedMemoryBlock.h     # Shared memory struct definition
│   ├── Resources/
│   │   └── BiltroyAudioLogo.png    # Logo asset
│   └── PlugNspectrPost.jucer
│
└── .gitignore
```

### Key Technical Details

| Detail | Value |
|---|---|
| Shared memory name | `BiltroyPlugNspectrShared` |
| Pre plugin VST3 ID | `BNSP` |
| Post plugin VST3 ID | `BNSQ` |
| Manufacturer code | `Bilt` |
| FFT size | 2048 points (order 11) |
| FFT windowing | Hann window |
| Spectrum smoothing | Exponential moving average (0.6 / 0.85 / 0.95) |
| Target frame rate | 60 fps |
| Heartbeat timeout | 500ms |

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    DAW Audio Thread                  │
│                                                      │
│  ┌──────────────┐    ┌──────────────┐               │
│  │PlugNspectrPre│    │Your Plugin   │               │
│  │processBlock()│───▶│processBlock()│               │
│  └──────┬───────┘    └──────────────┘               │
│         │ write pre signal          ┌──────────────┐ │
│         ▼                          │PlugNspectrPost│ │
│  ┌──────────────────────────┐      │processBlock() │ │
│  │  Windows Named Shared    │─────▶│               │ │
│  │  Memory                  │      └──────┬────────┘ │
│  │  BiltroyPlugNspectrShared│             │          │
│  └──────────────────────────┘             │ UI thread│
└─────────────────────────────────────────┐ │          │
                                          │ ▼          │
                                   ┌──────────────┐    │
                                   │  PluginEditor │    │
                                   │  60fps Timer  │    │
                                   │               │    │
                                   │  • Spectrum   │    │
                                   │  • Dynamics   │    │
                                   │  • Oscilloscope│   │
                                   │  • Harmonics  │    │
                                   └───────────────┘    │
```

---

## Roadmap

### v1.1 — Planned
- Performance tab — CPU and latency measurement
- Latency detection via impulse response
- Harmonics live mode with improved noise floor rejection
- Resizable window with saved dimensions

### v2.0 — Future
- Mac / AU support
- Mid/Side analysis mode
- Stereo field comparison (Lissajous display)
- Preset saving for reference comparisons
- Plugin latency compensation display

---

## License

PlugNspectr is free software: you can redistribute it and/or modify
it under the terms of the **GNU General Public License v3.0** as
published by the Free Software Foundation.

See [LICENSE](LICENSE) for the full license text.

© 2026 Biltroy Audio

---

*PlugNspectr is developed by Biltroy Audio using JUCE and C++.*  
*Built with ❤️ for mixing / mastering engineers and music producers who want to hear what their plugins are really doing.*
