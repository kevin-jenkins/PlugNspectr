// Seqlock concurrency test. Hammers the Transport's audio seqlock with a
// dedicated writer + reader thread over heap memory (Transport::openForTest, so
// no named segment is touched) and asserts the reader never observes a torn
// snapshot — fields written together must always be read together.
#include "doctest.h"
#include "SharedMemoryBlock.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

TEST_CASE ("Seqlock: concurrent writer/reader never tears")
{
    auto mem = std::make_unique<pns::SharedHeader>();   // heap — not OS shared memory
    pns::Transport tx;
    tx.openForTest (mem.get());

    constexpr int N = 256;
    std::vector<float> chan ((size_t) N);
    const float* chans[1] = { chan.data() };

    std::atomic<bool> stop { false };
    std::atomic<long> torn  { 0 };
    std::atomic<long> reads { 0 };

    // Writer: every block stamps the SAME value v into the sample data, the
    // sample count, the sample rate, and dynEnvPos — so a torn read shows up as
    // those fields disagreeing.
    std::thread writer ([&]
    {
        for (uint32_t v = 1; ! stop.load (std::memory_order_relaxed); ++v)
        {
            for (int i = 0; i < N; ++i) chan[(size_t) i] = (float) v;
            tx.writeBlock (chans, 1, N, (double) v, v, v & 3u);
        }
    });

    pns::AudioPayload out;
    for (int i = 0; i < 300000; ++i)
    {
        if (! tx.readBlock (out)) continue;
        reads.fetch_add (1, std::memory_order_relaxed);
        if (out.numSamples <= 0) continue;
        const float first = out.preData[0][0];
        const float last  = out.preData[0][out.numSamples - 1];
        const float sr    = (float) out.sampleRate;
        if (first != last || first != sr || first != (float) out.dynEnvPos)
            torn.fetch_add (1, std::memory_order_relaxed);   // fields from different writes
    }

    stop.store (true, std::memory_order_relaxed);
    writer.join();

    INFO ("reads=" << reads.load() << " torn=" << torn.load());
    CHECK (reads.load() > 0);
    CHECK (torn.load()  == 0);
}
