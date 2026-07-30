// Capture ring — the audio path feeding the Dynamics waveform, the Oscilloscope
// and the Stereo goniometer. Its index arithmetic (the first/rest wrap split and
// the fall-behind clamp) is exactly the kind of thing that silently half-works,
// and two real bugs have already been fixed here, so it gets direct coverage.
//
// Driven through the public seams only: injectTestCapture() writes, and
// readCaptureSince() drains.
#include "doctest.h"
#include "helpers.h"
#include "PluginProcessor.h"

#include <vector>

using P = PlugNspectrPostProcessor;

namespace
{
constexpr int kCh   = P::kCaptureChannels;
constexpr int kRing = P::kCaptureRingLen;

// Every sample encodes its own absolute position, so a drained chunk can be
// checked for both correct values and correct ordering across a wrap. Channels
// are made distinct so a channel mix-up cannot pass.
//   ch0 = idx,  ch1 = -idx,  ch2 = idx + 0.5
float expected (int ch, int64_t idx)
{
    switch (ch)
    {
        case 0:  return (float) idx;
        case 1:  return -(float) idx;
        default: return (float) idx + 0.5f;
    }
}

// Writes `n` samples starting at absolute position `from`. preN < n leaves Pre
// short, which the ring must zero-fill rather than skew.
void writeChunk (P& proc, int64_t from, int n, int preN)
{
    juce::AudioBuffer<float> pre  (kCh, juce::jmax (1, preN));
    juce::AudioBuffer<float> post (kCh, n);
    pre.clear();
    for (int c = 0; c < kCh; ++c)
    {
        for (int i = 0; i < n;    ++i) post.setSample (c, i, expected (c, from + i));
        for (int i = 0; i < preN; ++i) pre .setSample (c, i, expected (c, from + i));
    }
    // preN == 0 must still look like "no Pre", so hand over an empty buffer.
    if (preN <= 0) pre.setSize (kCh, 0, false, true, false);
    proc.injectTestCapture (pre, post, -12.0f, -12.0f);
}
} // namespace

TEST_CASE ("Capture ring: round-trip preserves samples, order and channels")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, 512);

    writeChunk (proc, 0, 512, 512);

    uint64_t pos = 0;
    juce::AudioBuffer<float> outPre, outPost;
    const int n = proc.readCaptureSince (pos, outPre, outPost);

    REQUIRE (n == 512);
    CHECK (pos == 512);                       // cursor advanced by exactly what we got

    bool ok = true;
    for (int c = 0; c < kCh && ok; ++c)
        for (int i = 0; i < n; ++i)
            if (outPost.getSample (c, i) != expected (c, i)
             || outPre .getSample (c, i) != expected (c, i)) { ok = false; break; }
    CHECK (ok);

    // Nothing new written → nothing drained, cursor unmoved.
    CHECK (proc.readCaptureSince (pos, outPre, outPost) == 0);
    CHECK (pos == 512);
}

TEST_CASE ("Capture ring: data stays contiguous across the wrap boundary")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, 512);

    uint64_t pos = 0;
    juce::AudioBuffer<float> outPre, outPost;

    // kRing is a power of two, so power-of-two chunks would land exactly ON the
    // boundary and never straddle it — the case this test exists for. Start with
    // an odd-sized write to break that alignment.
    constexpr int kStep = 4096;
    int64_t wrote = 0;
    writeChunk (proc, wrote, 1234, 1234);
    wrote += 1234;
    proc.readCaptureSince (pos, outPre, outPost);

    // Walk up to just under the ring end, draining as we go so nothing is lost.
    while (wrote + kStep < (int64_t) kRing)
    {
        writeChunk (proc, wrote, kStep, kStep);
        wrote += kStep;
        proc.readCaptureSince (pos, outPre, outPost);
    }
    REQUIRE (pos == (uint64_t) wrote);
    // The next chunk must genuinely span the boundary, or this proves nothing.
    REQUIRE ((int64_t) kRing - wrote < kStep);
    REQUIRE ((int64_t) kRing - wrote > 0);

    // This chunk straddles the end of the ring: part lands at the tail, the
    // remainder wraps to index 0.
    writeChunk (proc, wrote, kStep, kStep);
    const int n = proc.readCaptureSince (pos, outPre, outPost);

    REQUIRE (n == kStep);
    int firstBad = -1;
    for (int i = 0; i < n; ++i)
        if (outPost.getSample (0, i) != expected (0, wrote + i)) { firstBad = i; break; }
    INFO ("wrote=" << wrote << " ring=" << kRing << " firstBad=" << firstBad);
    CHECK (firstBad == -1);          // no discontinuity where the copy split
}

TEST_CASE ("Capture ring: falling behind keeps the newest and reveals the loss")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, 512);

    // Overrun the ring without draining.
    constexpr int kStep  = 8192;
    const int64_t total  = (int64_t) kRing + 5 * kStep;
    int64_t wrote = 0;
    while (wrote < total) { writeChunk (proc, wrote, kStep, kStep); wrote += kStep; }

    uint64_t pos = 0;
    const uint64_t before = pos;
    juce::AudioBuffer<float> outPre, outPost;
    const int n = proc.readCaptureSince (pos, outPre, outPost);

    // Only a ring's worth survives...
    CHECK (n == kRing);
    // ...but the cursor still lands on the true write position, so the caller can
    // see how much elapsed and account for it (the timebase fix depends on this).
    CHECK (pos == (uint64_t) wrote);
    const uint64_t lost = (pos - before) - (uint64_t) n;
    INFO ("wrote=" << wrote << " n=" << n << " lost=" << lost);
    CHECK (lost == (uint64_t) (wrote - kRing));

    // What survived must be the NEWEST audio, ending at the write position.
    const int64_t firstKept = wrote - kRing;
    CHECK (outPost.getSample (0, 0)     == expected (0, firstKept));
    CHECK (outPost.getSample (0, n - 1) == expected (0, wrote - 1));
}

TEST_CASE ("Capture ring: a short Pre is zero-filled, not skewed against Post")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, 512);

    // Post 500 samples, Pre only 100 — as happens when Pre is absent or lagging.
    writeChunk (proc, 0, 500, 100);

    uint64_t pos = 0;
    juce::AudioBuffer<float> outPre, outPost;
    const int n = proc.readCaptureSince (pos, outPre, outPost);

    REQUIRE (n == 500);                       // ring advances by the POST count
    CHECK (pos == 500);

    // Pre matches for the samples it supplied...
    CHECK (outPre.getSample (0, 0)  == expected (0, 0));
    CHECK (outPre.getSample (0, 99) == expected (0, 99));
    // ...and is silent beyond, so Pre and Post stay aligned in time rather than
    // Pre's samples sliding forward to fill the gap.
    CHECK (outPre.getSample (0, 100) == 0.0f);
    CHECK (outPre.getSample (0, 499) == 0.0f);
    // Post is unaffected throughout.
    CHECK (outPost.getSample (0, 499) == expected (0, 499));
}

TEST_CASE ("Capture ring: a stale cursor past the write position is clamped")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, 512);
    writeChunk (proc, 0, 512, 512);

    // prepareToPlay resets the ring, so a view's saved cursor can outrun it.
    uint64_t pos = 999999;
    juce::AudioBuffer<float> outPre, outPost;
    const int n = proc.readCaptureSince (pos, outPre, outPost);

    CHECK (n == 0);                 // no bogus huge read
    CHECK (pos == 512);             // clamped to the real write position
}
