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
