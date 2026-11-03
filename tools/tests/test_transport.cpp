// Transport command + heartbeat coverage — the half of the IPC layer the
// seqlock test doesn't touch. The seqlock test exercises the audio path
// (writeBlock/readBlock); this covers the Post->Pre command channel and the
// Pre/Post liveness logic. All over heap memory via Transport::openForTest, so
// no named OS segment is opened.
#include "doctest.h"
#include "SharedMemoryBlock.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

TEST_CASE ("Transport: command round-trips intact")
{
    auto mem = std::make_unique<pns::SharedHeader>();   // heap — not OS shared memory
    pns::Transport tx;
    tx.openForTest (mem.get());

    pns::PNS_CmdBlock c;
    c.testToneActive    = 1u;
    c.testToneFrequency = 440.0;
    c.measureActive     = 0u;
    c.dynMeasureMode    = 3u;
    c.testToneLevelDb   = -12.5;
    tx.writeCommand (c);

    pns::PNS_CmdBlock out;
    REQUIRE (tx.readCommand (out));
    CHECK (out.testToneActive    == 1u);
    CHECK (out.testToneFrequency == doctest::Approx (440.0));
    CHECK (out.measureActive     == 0u);
    CHECK (out.dynMeasureMode    == 3u);
    CHECK (out.testToneLevelDb   == doctest::Approx (-12.5));
}

TEST_CASE ("Transport: concurrent command writer/reader never tears")
{
    auto mem = std::make_unique<pns::SharedHeader>();
    pns::Transport tx;
    tx.openForTest (mem.get());

    std::atomic<bool> stop  { false };
    std::atomic<long> torn  { 0 };
    std::atomic<long> reads { 0 };

    // Every command stamps the SAME counter v into both double fields, so a
    // torn read (one field from write N, the other from write N+1) shows up as
    // testToneFrequency != testToneLevelDb.
    std::thread writer ([&]
    {
        for (uint32_t v = 1; ! stop.load (std::memory_order_relaxed); ++v)
        {
            pns::PNS_CmdBlock c;
            c.testToneActive    = v & 1u;
            c.testToneFrequency = (double) v;
            c.measureActive     = v & 1u;
            c.dynMeasureMode    = v & 3u;
            c.testToneLevelDb   = (double) v;
            tx.writeCommand (c);
        }
    });

    pns::PNS_CmdBlock out;
    for (int i = 0; i < 300000; ++i)
    {
        if (! tx.readCommand (out)) continue;
        reads.fetch_add (1, std::memory_order_relaxed);
        if (out.testToneFrequency != out.testToneLevelDb)
            torn.fetch_add (1, std::memory_order_relaxed);   // fields from different writes
    }

    stop.store (true, std::memory_order_relaxed);
    writer.join();

    INFO ("reads=" << reads.load() << " torn=" << torn.load());
    CHECK (reads.load() > 0);
    CHECK (torn.load()  == 0);
}

TEST_CASE ("Transport: heartbeats drive Pre/Post liveness independently")
{
    auto mem = std::make_unique<pns::SharedHeader>();
    pns::Transport tx;
    tx.openForTest (mem.get());

    // Fresh segment: heartbeats are 0, i.e. "ages ago" — neither side is alive.
    CHECK_FALSE (tx.isPreAlive  (500));
    CHECK_FALSE (tx.isPostAlive (500));

    // A Pre heartbeat marks Pre alive without affecting Post.
    tx.preHeartbeat();
    CHECK       (tx.isPreAlive  (500));
    CHECK_FALSE (tx.isPostAlive (500));

    // ...and a Post heartbeat marks Post alive.
    tx.postHeartbeat();
    CHECK (tx.isPostAlive (500));

    // The timeout window is real: an aged heartbeat falls outside a tight window
    // but stays inside a generous one.
    juce::Thread::sleep (30);
    CHECK_FALSE (tx.isPreAlive (10));    // ~30 ms old, 10 ms window -> stale
    CHECK       (tx.isPreAlive (500));   // still within 500 ms
}

TEST_CASE ("Transport: block sequence tells a new block from a repeated one")
{
    auto mem = std::make_unique<pns::SharedHeader>();
    pns::Transport tx;
    tx.openForTest (mem.get());

    std::vector<float> chan (300, 0.25f);
    const float* chans[1] = { chan.data() };
    auto out = std::make_unique<pns::AudioPayload>();

    tx.writeBlock (chans, 1, 300, 48000.0, 0u, 0u);
    uint32_t s1 = 0, s2 = 0, s3 = 0;
    REQUIRE (tx.readBlock (*out, &s1));
    CHECK (out->numSamples == 300);
    CHECK (out->preData[0][299] == doctest::Approx (0.25f));

    // Nothing written since: the same block, so the same sequence.
    REQUIRE (tx.readBlock (*out, &s2));
    CHECK (s2 == s1);

    tx.writeBlock (chans, 1, 300, 48000.0, 0u, 0u);
    REQUIRE (tx.readBlock (*out, &s3));
    CHECK (s3 != s1);
}

TEST_CASE ("Transport: a block larger than 4096 samples arrives whole")
{
    auto mem = std::make_unique<pns::SharedHeader>();
    pns::Transport tx;
    tx.openForTest (mem.get());

    constexpr int N = 8192;
    std::vector<float> chan ((size_t) N);
    for (int i = 0; i < N; ++i) chan[(size_t) i] = (float) i;
    const float* chans[1] = { chan.data() };
    tx.writeBlock (chans, 1, N, 48000.0, 0u, 0u);

    auto out = std::make_unique<pns::AudioPayload>();
    REQUIRE (tx.readBlock (*out));
    CHECK (out->numSamples == N);
    CHECK (out->preData[0][N - 1] == (float) (N - 1));
}

TEST_CASE ("Transport: only one Pre owns the segment; a second is turned away")
{
    auto mem = std::make_unique<pns::SharedHeader>();
    pns::Transport tx;
    tx.openForTest (mem.get());

    constexpr uint32_t a = 11u, b = 22u;
    CHECK_FALSE (tx.isPreConflict (1000));

    REQUIRE (tx.claimPre (a, 500));      // free segment: A takes it
    CHECK (tx.claimPre (a, 500));        // and keeps it
    CHECK_FALSE (tx.claimPre (b, 500));  // B is turned away while A is live...
    CHECK (tx.isPreConflict (1000));     // ...and the conflict is visible

    tx.releasePre (b);                   // releasing a claim you don't hold does nothing
    CHECK_FALSE (tx.claimPre (b, 500));

    tx.releasePre (a);                   // A leaves: B takes over at once
    CHECK (tx.claimPre (b, 500));
    CHECK_FALSE (tx.claimPre (a, 500));
}

TEST_CASE ("Transport: a Pre that stops is replaced after the timeout")
{
    auto mem = std::make_unique<pns::SharedHeader>();
    pns::Transport tx;
    tx.openForTest (mem.get());

    REQUIRE (tx.claimPre (11u, 500));
    CHECK_FALSE (tx.claimPre (22u, 20));
    juce::Thread::sleep (40);            // owner's heartbeat goes stale
    CHECK (tx.claimPre (22u, 20));
}
