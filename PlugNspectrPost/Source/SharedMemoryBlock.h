/*
  ==============================================================================
    SharedMemoryBlock.h
    PlugNspectr cross-platform inter-plugin IPC.

    One named shared segment ("PlugNspectrIPC") carries both directions:
      • audio payload   Pre  -> Post  (seqlock-guarded, single writer/reader)
      • command slot    Post -> Pre   (seqlock-guarded, single writer/reader)
      • two heartbeats  (atomic ms timestamps) for liveness detection

    All synchronisation is lock-free std::atomics living *inside* the mapped
    region — the only platform-specific code is map/unmap (SharedRegion), so the
    Transport logic is byte-for-byte identical on Windows and macOS.

    This header is duplicated verbatim in PlugNspectrPre/ and PlugNspectrPost/ and
    kept identical by the ABI unit test — both plugins must agree on the layout.
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <JuceHeader.h>

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
 #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <sys/mman.h>
 #include <sys/stat.h>
 #include <fcntl.h>
 #include <unistd.h>
#endif

namespace pns
{

inline constexpr int      kMaxChannels  = 2;
inline constexpr int      kMaxSamples   = 4096;
inline constexpr uint32_t kMagic        = 0x504E5332u;   // 'PNS2' — bump if layout changes
inline constexpr char     kSegmentName[] = "PlugNspectrIPC";   // bare; wrapper adds platform prefix

// Command slot — written by Post (UI), read by Pre. Level-based: Pre reacts to
// the current snapshot every block (no edge/requestId needed for continuous stimuli).
struct PNS_CmdBlock
{
    uint32_t testToneActive    = 0;       // non-zero -> Pre generates a sine test tone
    double   testToneFrequency = 1000.0;  // Hz
    uint32_t measureActive     = 0;       // non-zero -> white-noise measurement stimulus
    uint32_t dynMeasureMode    = 0;       // 0 off / 1 level-ramp / 2 level-step / 3 THD-sweep
    double   testToneLevelDb   = -6.0;    // dBFS
};

// Audio payload — written by Pre, read by Post.
struct AudioPayload
{
    int32_t  numChannels   = 0;
    int32_t  numSamples    = 0;
    double   sampleRate    = 0.0;
    uint32_t dynEnvPos     = 0;           // envelope cycle position / THD fundamental Hz
    uint32_t dynModeActive = 0;           // Pre's active stimulus mode (gates Post engines)
    float    preData[kMaxChannels][kMaxSamples] = {};
};

// The full mapped segment. Atomics are 4-byte, lock-free, and live in the shared
// region; payloads are plain POD copied under the seqlocks.
struct alignas (64) SharedHeader
{
    std::atomic<uint32_t> magic;          // 0 until the creator stamps it last
    std::atomic<uint32_t> preHeartbeat;   // ms (Pre writes each block)
    std::atomic<uint32_t> postHeartbeat;  // ms (Post writes each block)
    std::atomic<uint32_t> audioSeq;       // seqlock counter for `audio`
    std::atomic<uint32_t> cmdSeq;         // seqlock counter for `cmd`
    AudioPayload          audio;
    PNS_CmdBlock          cmd;
};

static_assert (std::atomic<uint32_t>::is_always_lock_free, "atomics must be lock-free in shared memory");
static_assert (std::is_trivially_copyable<AudioPayload>::value, "AudioPayload must be POD");
static_assert (std::is_trivially_copyable<PNS_CmdBlock>::value, "PNS_CmdBlock must be POD");
static_assert (std::is_standard_layout<SharedHeader>::value,    "SharedHeader must be standard-layout");

//==============================================================================
// SharedRegion — the ONLY platform-specific code: create-or-open + map/unmap.
class SharedRegion
{
public:
    SharedRegion() = default;
    ~SharedRegion() { close(); }
    SharedRegion (const SharedRegion&) = delete;
    SharedRegion& operator= (const SharedRegion&) = delete;

    // Create the named segment, or open it if another process already did.
    bool open (size_t bytes)
    {
        if (m_ptr != nullptr) return true;
        m_bytes = bytes;

       #if JUCE_WINDOWS
        const juce::String name = "Local\\" + juce::String (kSegmentName);
        HANDLE h = CreateFileMappingA (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       0, (DWORD) bytes, name.toRawUTF8());
        if (h == nullptr) return false;
        m_created = (GetLastError() != ERROR_ALREADY_EXISTS);
        void* p = MapViewOfFile (h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
        if (p == nullptr) { CloseHandle (h); return false; }
        m_handle = h;
        m_ptr = p;
        return true;
       #else
        m_name = "/" + juce::String (kSegmentName);
        const char* cname = m_name.toRawUTF8();
        int fd = ::shm_open (cname, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0)
        {
            m_created = true;
            if (::ftruncate (fd, (off_t) bytes) != 0)
            { ::close (fd); ::shm_unlink (cname); return false; }
        }
        else
        {
            fd = ::shm_open (cname, O_RDWR, 0600);
            if (fd < 0) return false;
            m_created = false;
        }
        void* p = ::mmap (nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED)
        { ::close (fd); if (m_created) ::shm_unlink (cname); return false; }
        m_fd = fd;
        m_ptr = p;
        return true;
       #endif
    }

    void close()
    {
        if (m_ptr == nullptr) return;
       #if JUCE_WINDOWS
        UnmapViewOfFile (m_ptr);
        if (m_handle != nullptr) CloseHandle (m_handle);
        m_handle = nullptr;
       #else
        ::munmap (m_ptr, m_bytes);
        if (m_fd >= 0) ::close (m_fd);
        if (m_created) ::shm_unlink (m_name.toRawUTF8());   // only the creator unlinks
        m_fd = -1;
       #endif
        m_ptr = nullptr;
        m_bytes = 0;
        m_created = false;
    }

    void* data()    const { return m_ptr; }
    bool  created() const { return m_created; }

private:
    void*  m_ptr     = nullptr;
    size_t m_bytes   = 0;
    bool   m_created = false;
   #if JUCE_WINDOWS
    HANDLE m_handle  = nullptr;
   #else
    int          m_fd = -1;
    juce::String m_name;
   #endif
};

//==============================================================================
// Transport — platform-neutral seqlock IPC over a SharedRegion.
class Transport
{
public:
    // Call once off the audio thread (prepareToPlay). Safe in either plugin, any
    // load order: first caller creates + stamps magic last; the other spins on it.
    bool open()
    {
        if (m_ok) return true;
        if (! m_region.open (sizeof (SharedHeader))) return false;
        m_hdr = reinterpret_cast<SharedHeader*> (m_region.data());
        auto* h = hdr();
        if (m_region.created())
        {
            std::memset (h, 0, sizeof (SharedHeader));
            h->magic.store (kMagic, std::memory_order_release);     // publish last
        }
        else
        {
            for (int i = 0; i < 200 && h->magic.load (std::memory_order_acquire) != kMagic; ++i)
                juce::Thread::sleep (1);                             // bounded, off audio thread
        }
        m_ok = (h->magic.load (std::memory_order_acquire) == kMagic);
        return m_ok;
    }

    void close() { m_region.close(); m_hdr = nullptr; m_ok = false; }
    bool isOpen() const { return m_ok; }

    // Test hook — drive the seqlocks over caller-owned memory (no OS mapping),
    // so the concurrency unit test never touches the real named segment.
    void openForTest (SharedHeader* h)
    {
        m_hdr = h;
        std::memset (h, 0, sizeof (SharedHeader));
        h->magic.store (kMagic, std::memory_order_release);
        m_ok = true;
    }

    // ---- audio payload: Pre writes, Post reads ----
    void writeBlock (const float* const* chans, int numCh, int numSmp,
                     double sr, uint32_t envPos, uint32_t mode)
    {
        if (! m_ok) return;
        auto* h = hdr();
        const uint32_t s = h->audioSeq.load (std::memory_order_relaxed);
        h->audioSeq.store (s + 1, std::memory_order_relaxed);       // begin (odd)
        std::atomic_thread_fence (std::memory_order_release);
        auto& a = h->audio;
        a.numChannels   = juce::jmin (numCh, kMaxChannels);
        a.numSamples    = juce::jmin (numSmp, kMaxSamples);
        a.sampleRate    = sr;
        a.dynEnvPos     = envPos;
        a.dynModeActive = mode;
        for (int ch = 0; ch < a.numChannels; ++ch)
            std::memcpy (a.preData[ch], chans[ch], (size_t) a.numSamples * sizeof (float));
        std::atomic_thread_fence (std::memory_order_release);
        h->audioSeq.store (s + 2, std::memory_order_relaxed);       // end (even)
    }

    bool readBlock (AudioPayload& out)
    {
        if (! m_ok) return false;
        auto* h = hdr();
        for (int tries = 0; tries < 8; ++tries)
        {
            const uint32_t s1 = h->audioSeq.load (std::memory_order_acquire);
            if (s1 & 1u) continue;                                  // writer mid-update
            std::atomic_thread_fence (std::memory_order_acquire);
            std::memcpy (&out, &h->audio, sizeof (AudioPayload));
            std::atomic_thread_fence (std::memory_order_acquire);
            if (h->audioSeq.load (std::memory_order_acquire) == s1) return true;
        }
        return false;
    }

    // ---- command slot: Post writes, Pre reads ----
    void writeCommand (const PNS_CmdBlock& c)
    {
        if (! m_ok) return;
        auto* h = hdr();
        const uint32_t s = h->cmdSeq.load (std::memory_order_relaxed);
        h->cmdSeq.store (s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        h->cmd = c;
        std::atomic_thread_fence (std::memory_order_release);
        h->cmdSeq.store (s + 2, std::memory_order_relaxed);
    }

    bool readCommand (PNS_CmdBlock& out)
    {
        if (! m_ok) return false;
        auto* h = hdr();
        for (int tries = 0; tries < 8; ++tries)
        {
            const uint32_t s1 = h->cmdSeq.load (std::memory_order_acquire);
            if (s1 & 1u) continue;
            std::atomic_thread_fence (std::memory_order_acquire);
            const PNS_CmdBlock tmp = h->cmd;
            std::atomic_thread_fence (std::memory_order_acquire);
            if (h->cmdSeq.load (std::memory_order_acquire) == s1) { out = tmp; return true; }
        }
        return false;
    }

    // ---- heartbeats / liveness ----
    void preHeartbeat()  { if (m_ok) hdr()->preHeartbeat .store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed); }
    void postHeartbeat() { if (m_ok) hdr()->postHeartbeat.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed); }

    bool isPreAlive  (uint32_t timeoutMs) const { return m_ok && age (hdr()->preHeartbeat)  < timeoutMs; }
    bool isPostAlive (uint32_t timeoutMs) const { return m_ok && age (hdr()->postHeartbeat) < timeoutMs; }

    // Exposed for the seqlock unit test.
    SharedHeader* header() const { return hdr(); }

private:
    SharedHeader* hdr() const { return m_hdr; }
    static uint32_t age (const std::atomic<uint32_t>& hb)
    { return juce::Time::getMillisecondCounter() - hb.load (std::memory_order_relaxed); }

    SharedRegion  m_region;
    SharedHeader* m_hdr = nullptr;
    bool m_ok = false;
};

} // namespace pns
