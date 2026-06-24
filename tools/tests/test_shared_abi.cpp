// Shared-memory ABI guard. The Pre and Post plugins are separate processes that
// map the same struct; these checks pin its layout and the magic, and verify the
// two copies of the header stay byte-identical.
#include "doctest.h"
#include "SharedMemoryBlock.h"

#include <cstddef>
#include <type_traits>
#include <juce_core/juce_core.h>

// Layout must never silently change — both processes depend on the exact bytes.
static_assert (kPNS_Magic == 0xB11750BBu, "shared-memory magic changed");
static_assert (sizeof (PNS_SharedBlock) == 32808, "PNS_SharedBlock size changed");
static_assert (sizeof (PNS_CmdBlock)    == 28,    "PNS_CmdBlock size changed");
static_assert (offsetof (PNS_SharedBlock, preData) == 40, "preData offset changed");
static_assert (std::is_standard_layout<PNS_SharedBlock>::value, "PNS_SharedBlock must be POD");
static_assert (std::is_trivially_copyable<PNS_SharedBlock>::value, "PNS_SharedBlock must be POD");
static_assert (std::is_standard_layout<PNS_CmdBlock>::value, "PNS_CmdBlock must be POD");
static_assert (std::is_trivially_copyable<PNS_CmdBlock>::value, "PNS_CmdBlock must be POD");

TEST_CASE ("Shared ABI: layout and magic are pinned")
{
    CHECK (kPNS_Magic == 0xB11750BBu);
    CHECK (sizeof (PNS_SharedBlock) == 32808u);
    CHECK (sizeof (PNS_CmdBlock) == 28u);
    CHECK (kPNS_MaxChannels == 2);
    CHECK (kPNS_MaxSamplesPerBlock == 4096);
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
