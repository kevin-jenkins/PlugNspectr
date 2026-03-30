/*
  ==============================================================================
    SharedMemoryBlock.h
    PlugNspectr inter-plugin shared memory definition.

    Named mapping: "BiltroyPlugNspectrShared"
      PlugNspectrPre  – creator / writer  (writes preData each processBlock)
      PlugNspectrPost – reader            (reads  preData each processBlock)

    Layout is kept as plain POD so it can live directly in mapped memory
    without any construction or vtable overhead.
  ==============================================================================
*/

#pragma once

#include <cstdint>

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr int      kPNS_MaxChannels       = 2;
static constexpr int      kPNS_MaxSamplesPerBlock = 4096;
static constexpr uint32_t kPNS_Magic             = 0xB11750BBu;
static constexpr char     kPNS_SharedMemName[]   = "BiltroyPlugNspectrShared";

// ── Shared block layout ───────────────────────────────────────────────────────

#pragma pack(push, 1)
struct PNS_SharedBlock
{
    uint32_t          magic;          // kPNS_Magic once Pre has initialised the block
    int32_t           numChannels;    // number of channels stored in preData (1 or 2)
    int32_t           numSamples;     // valid sample count per channel this block
    double            sampleRate;     // sample rate Pre is running at
    volatile uint32_t writeCount;     // incremented by Pre after every block write;
                                      // Post can compare to detect a new block
    float             preData[kPNS_MaxChannels][kPNS_MaxSamplesPerBlock];
};
#pragma pack(pop)

// Convenience: total byte size to pass to CreateFileMapping / MapViewOfFile
static constexpr uint32_t kPNS_SharedMemBytes = static_cast<uint32_t>(sizeof(PNS_SharedBlock));

// ── Command block — written by Post, read by Pre ──────────────────────────────
static constexpr char kPNS_CmdMemName[] = "BiltroyPlugNspectrCmd";

#pragma pack(push, 1)
struct PNS_CmdBlock
{
    uint32_t testToneActive;    // non-zero → generate a sine test tone
    double   testToneFrequency; // Hz (100 – 8000)
};
#pragma pack(pop)

static constexpr uint32_t kPNS_CmdMemBytes = static_cast<uint32_t>(sizeof(PNS_CmdBlock));
