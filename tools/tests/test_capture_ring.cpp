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
    juce::AudioBuffer<float> pre  (kCh, juce::jmax (0, preN));   // 0 == "no Pre"
    juce::AudioBuffer<float> post (kCh, n);
    pre.clear();
    for (int c = 0; c < kCh; ++c)
    {
        for (int i = 0; i < n;    ++i) post.setSample (c, i, expected (c, from + i));
        for (int i = 0; i < preN; ++i) pre .setSample (c, i, expected (c, from + i));
    }
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

    // Check every channel of BOTH buffers. readCaptureSince splits the copy into
    // four hand-offset copyFrom calls (post/pre x head/wrapped), and the write
    // side's put() passes a different `valid` for Pre than Post — so verifying
    // only post[0] would let a wrong offset in any of the other three pass.
    int badCh = -1, badIdx = -1;
    bool badWasPre = false;
    for (int c = 0; c < kCh && badCh < 0; ++c)
        for (int i = 0; i < n; ++i)
        {
            const float want = expected (c, wrote + i);
            if (outPost.getSample (c, i) != want)
            { badCh = c; badIdx = i; badWasPre = false; break; }
            if (outPre .getSample (c, i) != want)
            { badCh = c; badIdx = i; badWasPre = true;  break; }
        }
    INFO ("wrote=" << wrote << " ring=" << kRing << " badCh=" << badCh
                   << " badIdx=" << badIdx << " inPre=" << badWasPre);
    CHECK (badCh == -1);             // no discontinuity where the copy split
}

TEST_CASE ("Capture ring: falling behind keeps the newest and reveals the loss")
{
    P proc;
    proc.prepareToPlay (pnst::kSR, 512);

    constexpr int kStep = 8192;
    uint64_t pos = 0;
    juce::AudioBuffer<float> outPre, outPost;
    int64_t wrote = 0;

    // Drain normally for a while first. The real scenario is a view that has been
    // keeping up and then blocks — not one opening onto an already-full ring — so
    // the stall must begin from a mid-stream cursor, which also makes the
    // (newPos - oldPos) - n arithmetic below a real test rather than x - 0.
    for (int i = 0; i < 3; ++i)
    {
        writeChunk (proc, wrote, kStep, kStep);
        wrote += kStep;
        proc.readCaptureSince (pos, outPre, outPost);
    }
    const uint64_t before = pos;
    REQUIRE (before == (uint64_t) wrote);
    REQUIRE (before > 0);                     // genuinely mid-stream

    // Now stall: overrun the ring without draining.
    const int64_t stallEnd = wrote + (int64_t) kRing + 5 * kStep;
    while (wrote < stallEnd) { writeChunk (proc, wrote, kStep, kStep); wrote += kStep; }

    const int n = proc.readCaptureSince (pos, outPre, outPost);

    // Only a ring's worth survives...
    CHECK (n == kRing);
    // ...but the cursor still lands on the true write position, so the caller can
    // see how much elapsed and account for it (the timebase fix depends on this).
    CHECK (pos == (uint64_t) wrote);
    // Elapsed since the cursor last moved, minus what survived. Note this is
    // relative to `before`, not to zero — the whole point of starting mid-stream.
    const uint64_t elapsed = pos - before;
    const uint64_t lost    = elapsed - (uint64_t) n;
    INFO ("before=" << before << " wrote=" << wrote << " elapsed=" << elapsed
                    << " n=" << n << " lost=" << lost);
    CHECK (elapsed == (uint64_t) wrote - before);
    CHECK (lost    == elapsed - (uint64_t) kRing);

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
