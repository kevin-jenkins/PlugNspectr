// Shared constants + tiny numeric helpers for the PlugNspectr unit tests.
#pragma once
#include <cmath>

namespace pnst
{
constexpr double kSR  = 48000.0;   // sample rate used across the suite
constexpr int    kBlk = 4800;      // 0.1 s injection block (matches the harness)

// FFT bin index for a frequency, given an FFT size.
inline int binAt (double f, int fftSize)
{
    return (int) std::lround (f * (double) fftSize / kSR);
}

inline double radToDeg (double r) { return r * 180.0 / 3.14159265358979323846; }
} // namespace pnst
