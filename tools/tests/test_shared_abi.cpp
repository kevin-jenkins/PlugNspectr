// Shared-memory ABI guard. Pre and Post are separate processes mapping the same
// SharedHeader; these checks pin the invariants the layout depends on and verify
// the two copies of the header stay byte-identical.
#include "doctest.h"
#include "SharedMemoryBlock.h"

#include <cstddef>
#include <type_traits>
#include <juce_core/juce_core.h>

// Cross-process sharing requires lock-free atomics and POD payloads; the magic
// is version-stamped so a stale segment from an older build is rejected.
static_assert (pns::kMagic == 0x504E5332u, "shared-memory magic/version changed");
static_assert (std::atomic<uint32_t>::is_always_lock_free, "atomics must be lock-free in shared memory");
static_assert (std::is_trivially_copyable<pns::AudioPayload>::value, "AudioPayload must be POD");
static_assert (std::is_trivially_copyable<pns::PNS_CmdBlock>::value, "PNS_CmdBlock must be POD");
static_assert (std::is_standard_layout<pns::SharedHeader>::value,    "SharedHeader must be standard-layout");
static_assert (pns::kMaxChannels == 2,    "channel count changed");
static_assert (pns::kMaxSamples  == 4096, "block size changed");

TEST_CASE ("Shared ABI: layout invariants are pinned")
{
    CHECK (pns::kMagic == 0x504E5332u);
    CHECK (std::atomic<uint32_t>::is_always_lock_free);
    // The audio buffer dominates the segment, which holds both payloads + atomics.
    const size_t audioBytes = (size_t) pns::kMaxChannels * pns::kMaxSamples * sizeof (float);
    CHECK (sizeof (pns::AudioPayload) >= audioBytes);
    CHECK (sizeof (pns::SharedHeader) >= sizeof (pns::AudioPayload) + sizeof (pns::PNS_CmdBlock));
}

TEST_CASE ("Shared ABI: Pre and Post copies of SharedMemoryBlock.h are identical")
{
    const juce::File root (PNS_REPO_ROOT);
    const juce::File post = root.getChildFile ("PlugNspectrPost/Source/SharedMemoryBlock.h");
    const juce::File pre  = root.getChildFile ("PlugNspectrPre/Source/SharedMemoryBlock.h");

    REQUIRE (post.existsAsFile());
    REQUIRE (pre.existsAsFile());
    CHECK (post.loadFileAsString() == pre.loadFileAsString());
}
