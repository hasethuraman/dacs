// Copyright (c) Microsoft Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <sys/resource.h>
#include <time.h>
#include "ReadPressureTrace.h"

namespace uploaddiag
{
enum Role { Unknown, Foreground, Replay, Uploader, RoleCount };
inline constexpr std::array<const char*, RoleCount> roleNames = {
    "unknown", "foreground", "replay", "uploader"};
inline thread_local Role currentRole = Unknown;
class RoleScope
{
public:
    explicit RoleScope(Role role) : m_previous(currentRole) { currentRole = role; }
    ~RoleScope() { currentRole = m_previous; }
    RoleScope(const RoleScope&) = delete;
    RoleScope& operator=(const RoleScope&) = delete;
private:
    Role m_previous;
};
inline std::atomic<uint64_t> clockErrors{0};
enum Stage
{
    Read, Probe, Blob, Upload, Wrapper, UploadIdleWait, ReplayWait, Count
};

inline constexpr std::array<const char*, Count> stageNames = {
    "read", "probe", "blob", "upload", "wrapper", "upload_idle_wait", "replay_wait"};

inline bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("DACS_UPLOAD_DIAGNOSTICS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

inline uint64_t Now(clockid_t clock = CLOCK_MONOTONIC)
{
    timespec value{};
    const int savedErrno = errno;
    if (clock_gettime(clock, &value) != 0)
    {
        const int error = errno;
        clockErrors.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "DACS_CPU_ERROR operation=clock_gettime clock=%d error=%d\n",
            static_cast<int>(clock), error);
        errno = savedErrno;
        return 0;
    }
    errno = savedErrno;
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL +
        static_cast<uint64_t>(value.tv_nsec);
}

struct alignas(64) Counter
{
    std::atomic<uint64_t> calls{0}, bytes{0}, wallNs{0}, cpuNs{0}, maxNs{0};
    std::atomic<uint64_t> exclusiveCpuNs{0};
    std::atomic<uint64_t> slow10ms{0}, active{0}, peakActive{0};
    std::atomic<uint64_t> firstNs{UINT64_MAX}, lastNs{0}, maxGapNs{0};
};

inline std::array<Counter, Count> counters;
inline std::array<std::array<Counter, Count>, RoleCount> roleCounters;
inline std::atomic<uint64_t> pendingBytes{0}, peakPendingBytes{0}, failures{0};
inline std::atomic<uint64_t> inFlightBytes{0}, completedReads{0}, memoryNext{0};
inline constexpr uint64_t MemoryCapacity = 1024;
struct MemorySample
{
    uint64_t monotonicNs = 0, reads = 0, pending = 0, inFlight = 0;
    uint64_t rssBytes = 0, peakRssBytes = 0;
    int rssError = 0, peakError = 0;
    std::atomic<bool> ready{false};
};
inline std::array<MemorySample, MemoryCapacity> memorySamples;
inline std::mutex memoryDumpMutex;
inline uint64_t memoryDumpIndex = 0;

inline void SampleMemory(uint64_t reads)
{
    if (!Enabled() || !readpressure::Enabled()) return;
    const int savedErrno = errno;
    const auto index = memoryNext.fetch_add(1, std::memory_order_relaxed);
    if (index >= MemoryCapacity) { errno = savedErrno; return; }
    auto& sample = memorySamples[index];
    sample.reads = reads;
    const int descriptor = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) sample.rssError = errno;
    else
    {
        char buffer[128]{};
        const auto length = read(descriptor, buffer, sizeof(buffer) - 1);
        unsigned long long size = 0, resident = 0;
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (length < 0) sample.rssError = errno;
        else if (pageSize <= 0 || std::sscanf(buffer, "%llu %llu", &size, &resident) != 2)
            sample.rssError = EIO;
        else sample.rssBytes = resident * static_cast<uint64_t>(pageSize);
        close(descriptor);
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) sample.peakError = errno;
    else sample.peakRssBytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024;
    sample.pending = pendingBytes.load();
    sample.inFlight = inFlightBytes.load();
    sample.monotonicNs = Now();
    sample.ready.store(true, std::memory_order_release);
    errno = savedErrno;
}

inline void DumpMemory(bool final = true)
{
    std::lock_guard<std::mutex> lock(memoryDumpMutex);
    const auto count = memoryNext.load();
    const auto end = std::min(count, MemoryCapacity);
    const auto first = memoryDumpIndex;
    while (memoryDumpIndex < end)
    {
        const auto& sample = memorySamples[memoryDumpIndex];
        if (!sample.ready.load(std::memory_order_acquire)) break;
        std::fprintf(stderr,
            "DACS_READ_MEMORY monotonic_ns=%llu completed_reads=%llu pending_bytes=%llu in_flight_bytes=%llu rss_bytes=%llu peak_rss_bytes=%llu rss_error=%d peak_error=%d\n",
            (unsigned long long)sample.monotonicNs, (unsigned long long)sample.reads,
            (unsigned long long)sample.pending, (unsigned long long)sample.inFlight,
            (unsigned long long)sample.rssBytes, (unsigned long long)sample.peakRssBytes,
            sample.rssError, sample.peakError);
        ++memoryDumpIndex;
    }
    if (memoryDumpIndex == first && !final) return;
    std::fprintf(stderr, "DACS_READ_MEMORY_END samples=%llu incomplete=%llu dropped=%llu\n",
        (unsigned long long)memoryDumpIndex,
        (unsigned long long)(final ? end - memoryDumpIndex : 0),
        (unsigned long long)(count > MemoryCapacity ? count - MemoryCapacity : 0));
}

inline void Max(std::atomic<uint64_t>& target, uint64_t value)
{
    auto old = target.load(std::memory_order_relaxed);
    while (old < value && !target.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
}

inline void RecordCounter(Counter& counter, uint64_t bytes, uint64_t wallNs,
    uint64_t cpuNs, uint64_t endNs, uint64_t exclusiveCpuNs)
{
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    counter.bytes.fetch_add(bytes, std::memory_order_relaxed);
    counter.wallNs.fetch_add(wallNs, std::memory_order_relaxed);
    counter.cpuNs.fetch_add(cpuNs, std::memory_order_relaxed);
    counter.exclusiveCpuNs.fetch_add(exclusiveCpuNs, std::memory_order_relaxed);
    counter.slow10ms.fetch_add(wallNs >= 10000000, std::memory_order_relaxed);
    Max(counter.maxNs, wallNs);
    if (endNs)
    {
        auto previous = counter.lastNs.load(std::memory_order_relaxed);
        while (previous < endNs &&
            !counter.lastNs.compare_exchange_weak(previous, endNs, std::memory_order_relaxed)) {}
        if (previous != 0 && endNs > previous)
        {
            Max(counter.maxGapNs, endNs - previous);
        }
    }
}

inline void Record(Stage stage, uint64_t bytes, uint64_t wallNs, uint64_t cpuNs = 0,
    uint64_t endNs = 0, uint64_t exclusiveCpuNs = 0, Role role = currentRole)
{
    RecordCounter(counters[stage], bytes, wallNs, cpuNs, endNs, exclusiveCpuNs);
    RecordCounter(roleCounters[role][stage], bytes, wallNs, cpuNs, endNs, exclusiveCpuNs);
}

inline constexpr uint64_t BlobReadBatchSize = 100, BlobReadBatchCapacity = 4096;
struct BlobReadBatch
{
    uint64_t calls = 0, bytes = 0, startNs = UINT64_MAX, endNs = 0;
    uint64_t wallNs = 0, cpuNs = 0;
    double meanWallMs = 0, meanCpuMs = 0, cpuPercent = 0;
    uint64_t exclusiveCpuNs = 0;
};
inline std::array<BlobReadBatch, BlobReadBatchCapacity> blobReadBatches;
inline BlobReadBatch blobReadPending;
inline std::mutex blobReadMutex, blobReadDumpMutex;
inline uint64_t blobReadNext = 0, blobReadDumped = 0, blobReadDropped = 0;

// Called with blobReadMutex held; completed batches retain no per-call records.
inline void CompleteBlobReadBatch()
{
    auto& batch = blobReadPending;
    if (!batch.calls) return;
    batch.meanWallMs = static_cast<double>(batch.wallNs) /
        static_cast<double>(batch.calls) / 1000000.0;
    batch.meanCpuMs = static_cast<double>(batch.cpuNs) /
        static_cast<double>(batch.calls) / 1000000.0;
    batch.cpuPercent = 100.0 * static_cast<double>(batch.cpuNs) /
        static_cast<double>(batch.endNs - batch.startNs);
    if (blobReadNext < BlobReadBatchCapacity) blobReadBatches[blobReadNext++] = batch;
    else blobReadDropped += batch.calls;
    batch = BlobReadBatch{};
}

inline void RecordBlobRead(uint64_t start, uint64_t end, uint64_t bytes, uint64_t cpu)
{
    std::lock_guard<std::mutex> lock(blobReadMutex);
    auto& batch = blobReadPending;
    if (end <= start || cpu > end - start ||
        bytes > UINT64_MAX - batch.bytes || end - start > UINT64_MAX - batch.wallNs ||
        cpu > UINT64_MAX - batch.cpuNs)
    {
        ++clockErrors;
        std::fputs("DACS_CPU_ERROR operation=blob_read_aggregate\n", stderr);
        return;
    }
    ++batch.calls;
    batch.bytes += bytes;
    batch.startNs = std::min(batch.startNs, start);
    batch.endNs = std::max(batch.endNs, end);
    batch.wallNs += end - start;
    batch.cpuNs += cpu;
    if (batch.calls == BlobReadBatchSize) CompleteBlobReadBatch();
}

inline void DumpBlobReadBatches(bool final)
{
    std::lock_guard<std::mutex> dumping(blobReadDumpMutex);
    {
        std::lock_guard<std::mutex> lock(blobReadMutex);
        if (final) CompleteBlobReadBatch();
    }
    while (true)
    {
        BlobReadBatch batch;
        uint64_t sequence;
        {
            std::lock_guard<std::mutex> lock(blobReadMutex);
            if (blobReadDumped == blobReadNext) break;
            sequence = ++blobReadDumped;
            batch = blobReadBatches[sequence - 1];
        }
        std::fprintf(stderr,
            "DACS_BLOB_READ_SAMPLE pid=%ld sequence=%llu calls=%llu bytes=%llu "
            "start_ns=%llu end_ns=%llu wall_ns=%llu cpu_ns=%llu "
            "mean_wall_ms=%.17g mean_cpu_ms=%.17g cpu_percent=%.17g\n",
            static_cast<long>(getpid()), (unsigned long long)sequence,
            (unsigned long long)batch.calls, (unsigned long long)batch.bytes,
            (unsigned long long)batch.startNs, (unsigned long long)batch.endNs,
            (unsigned long long)batch.wallNs, (unsigned long long)batch.cpuNs,
            batch.meanWallMs, batch.meanCpuMs, batch.cpuPercent);
    }
    std::lock_guard<std::mutex> lock(blobReadMutex);
    std::fprintf(stderr,
        "DACS_BLOB_READ_SAMPLE_END pid=%ld samples=%llu pending_calls=%llu "
        "dropped_calls=%llu capacity=%llu final=%d\n",
        static_cast<long>(getpid()), (unsigned long long)blobReadDumped,
        (unsigned long long)blobReadPending.calls, (unsigned long long)blobReadDropped,
        (unsigned long long)BlobReadBatchCapacity, final ? 1 : 0);
}

inline constexpr std::array<Stage, 3> LatencyStages = {Blob, Probe, Upload};
struct StageLatencySample
{
    BlobReadBatch batch;
    Role role = Unknown;
};
struct StageLatencySeries
{
    std::mutex mutex;
    std::array<BlobReadBatch, RoleCount> pending;
    std::array<StageLatencySample, BlobReadBatchCapacity> samples;
    uint64_t next = 0, dumped = 0, dropped = 0;
};
inline std::array<StageLatencySeries, LatencyStages.size()> stageLatencySeries;
inline std::mutex stageLatencyDumpMutex;

inline bool StageLatencyTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("DACS_STAGE_LATENCY_TRACE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// Caller holds the series mutex; roles never share an aggregate.
inline void CompleteStageLatencyBatch(StageLatencySeries& series, Role role)
{
    auto& batch = series.pending[role];
    if (!batch.calls) return;
    batch.meanWallMs = static_cast<double>(batch.wallNs) /
        static_cast<double>(batch.calls) / 1000000.0;
    if (series.next < BlobReadBatchCapacity)
        series.samples[series.next++] = {batch, role};
    else series.dropped += batch.calls;
    batch = BlobReadBatch{};
}

inline void RecordStageLatency(Stage stage, Role role, uint64_t start, uint64_t end,
    uint64_t bytes, uint64_t cpu, uint64_t exclusiveCpu = 0)
{
    const auto found = std::find(LatencyStages.begin(), LatencyStages.end(), stage);
    if (found == LatencyStages.end()) return;
    auto& series = stageLatencySeries[static_cast<size_t>(found - LatencyStages.begin())];
    std::lock_guard<std::mutex> lock(series.mutex);
    auto& batch = series.pending[role];
    if (end <= start || cpu > end - start || exclusiveCpu > cpu ||
        bytes > UINT64_MAX - batch.bytes || end - start > UINT64_MAX - batch.wallNs ||
        cpu > UINT64_MAX - batch.cpuNs || exclusiveCpu > UINT64_MAX - batch.exclusiveCpuNs)
    {
        ++clockErrors;
        std::fputs("DACS_CPU_ERROR operation=stage_latency_aggregate\n", stderr);
        return;
    }
    ++batch.calls;
    batch.bytes += bytes;
    batch.startNs = std::min(batch.startNs, start);
    batch.endNs = std::max(batch.endNs, end);
    batch.wallNs += end - start;
    batch.cpuNs += cpu;
    batch.exclusiveCpuNs += exclusiveCpu;
    if (batch.calls == BlobReadBatchSize) CompleteStageLatencyBatch(series, role);
}

inline void DumpStageLatencies(bool final)
{
    std::lock_guard<std::mutex> dumping(stageLatencyDumpMutex);
    for (size_t index = 0; index < LatencyStages.size(); ++index)
    {
        auto& series = stageLatencySeries[index];
        {
            std::lock_guard<std::mutex> lock(series.mutex);
            if (final)
                for (size_t role = 0; role < RoleCount; ++role)
                    CompleteStageLatencyBatch(series, static_cast<Role>(role));
        }
        while (true)
        {
            StageLatencySample sample;
            uint64_t sequence;
            {
                std::lock_guard<std::mutex> lock(series.mutex);
                if (series.dumped == series.next) break;
                sequence = ++series.dumped;
                sample = series.samples[sequence - 1];
            }
            const auto& batch = sample.batch;
            std::fprintf(stderr,
                "DACS_STAGE_LATENCY_SAMPLE pid=%ld stage=%s role=%s sequence=%llu "
                "calls=%llu bytes=%llu start_ns=%llu end_ns=%llu wall_ns=%llu "
                "cpu_ns=%llu exclusive_cpu_ns=%llu mean_wall_ms=%.17g\n",
                static_cast<long>(getpid()), stageNames[LatencyStages[index]],
                roleNames[sample.role], (unsigned long long)sequence,
                (unsigned long long)batch.calls, (unsigned long long)batch.bytes,
                (unsigned long long)batch.startNs, (unsigned long long)batch.endNs,
                (unsigned long long)batch.wallNs, (unsigned long long)batch.cpuNs,
                (unsigned long long)batch.exclusiveCpuNs,
                batch.meanWallMs);
        }
        std::lock_guard<std::mutex> lock(series.mutex);
        uint64_t pending = 0;
        for (const auto& batch : series.pending) pending += batch.calls;
        std::fprintf(stderr,
            "DACS_STAGE_LATENCY_SAMPLE_END pid=%ld stage=%s samples=%llu "
            "pending_calls=%llu dropped_calls=%llu capacity=%llu active_calls=%llu final=%d\n",
            static_cast<long>(getpid()), stageNames[LatencyStages[index]],
            (unsigned long long)series.dumped, (unsigned long long)pending,
            (unsigned long long)series.dropped,
            (unsigned long long)BlobReadBatchCapacity,
            (unsigned long long)counters[LatencyStages[index]].active.load(), final ? 1 : 0);
    }
}

class Scope;
inline thread_local Scope* currentScope = nullptr;
class Scope
{
public:
    explicit Scope(Stage stage, uint64_t bytes = 0)
        : m_stage(stage), m_bytes(bytes), m_start(Enabled() ? Now() : 0),
          m_cpuStart(m_start ? Now(CLOCK_THREAD_CPUTIME_ID) : 0),
          m_parent(m_start ? currentScope : nullptr), m_role(currentRole)
    {
        if (m_start)
        {
            currentScope = this;
            auto& counter = counters[m_stage];
            uint64_t first = UINT64_MAX;
            counter.firstNs.compare_exchange_strong(first, m_start, std::memory_order_relaxed);
            Max(counter.peakActive, counter.active.fetch_add(1, std::memory_order_relaxed) + 1);
            if (m_stage == Upload) inFlightBytes.fetch_add(m_bytes, std::memory_order_relaxed);
            if (m_stage == Read || (StageLatencyTraceEnabled() &&
                (m_stage == Probe || m_stage == Blob || m_stage == Upload)))
                m_pressure = readpressure::Begin(stageNames[m_stage], m_bytes, m_start,
                    pendingBytes.load(), roleNames[m_role]);
        }
    }
    ~Scope() { Stop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Stop()
    {
        if (m_start)
        {
            const int savedErrno = errno;
            const auto cpu = Now(CLOCK_THREAD_CPUTIME_ID) - m_cpuStart;
            const auto end = Now();
            const auto pressureEnd = m_pressure < readpressure::Capacity ? readpressure::Snapshot() : readpressure::Stamp{};
            if (currentScope != this || m_nestedCpu > cpu)
            {
                clockErrors.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "DACS_CPU_ERROR operation=scope_accounting\n");
            }
            const auto exclusiveCpu = cpu >= m_nestedCpu ? cpu - m_nestedCpu : 0;
            if (m_pressure < readpressure::Capacity)
                readpressure::Finish(m_pressure, pressureEnd, end, cpu, pendingBytes.load(), exclusiveCpu);
            Record(m_stage, m_bytes, end - m_start, cpu, end, exclusiveCpu, m_role);
            RecordStageLatency(m_stage, m_role, m_start, end, m_bytes, cpu, exclusiveCpu);
            if (m_stage == Wrapper && m_role == Foreground && m_bytes > 0)
                RecordBlobRead(m_start, end, m_bytes, cpu);
            if (m_parent) m_parent->m_nestedCpu += cpu;
            currentScope = m_parent;
            counters[m_stage].active.fetch_sub(1, std::memory_order_relaxed);
            if (m_stage == Upload) inFlightBytes.fetch_sub(m_bytes, std::memory_order_relaxed);
            if (m_stage == Wrapper && m_role == Foreground && m_bytes > 0 && readpressure::Enabled())
            {
                const auto reads = completedReads.fetch_add(1, std::memory_order_relaxed) + 1;
                if (reads % 100 == 0) SampleMemory(reads);
            }
            m_start = 0;
            errno = savedErrno;
        }
    }
private:
    Stage m_stage;
    uint64_t m_bytes, m_start, m_cpuStart;
    uint64_t m_pressure = readpressure::Capacity;
    Scope* m_parent;
    Role m_role;
    uint64_t m_nestedCpu = 0;
};

inline void Log(const char* snapshot = "periodic")
{
    if (!Enabled()) return;
    if (std::strcmp(snapshot, "start") == 0) SampleMemory(0);
    const bool final = std::strcmp(snapshot, "shutdown") == 0;
    if (final) SampleMemory(completedReads.load());
    DumpBlobReadBatches(final);
    DumpStageLatencies(final);
    readpressure::Dump(final);
    if (readpressure::Enabled()) DumpMemory(final);
    for (size_t i = 0; i < Count; ++i)
    {
        const auto& c = counters[i];
        std::fprintf(stderr,
            "DACS_UPLOAD_STAGE stage=%s calls=%llu bytes=%llu wall_ns=%llu cpu_ns=%llu max_ns=%llu slow_10ms=%llu active=%llu peak_active=%llu first_ns=%llu last_ns=%llu max_gap_ns=%llu snapshot=%s\n",
            stageNames[i], (unsigned long long)c.calls.load(), (unsigned long long)c.bytes.load(),
            (unsigned long long)c.wallNs.load(), (unsigned long long)c.cpuNs.load(),
            (unsigned long long)c.maxNs.load(), (unsigned long long)c.slow10ms.load(),
            (unsigned long long)c.active.load(), (unsigned long long)c.peakActive.load(),
            (unsigned long long)c.firstNs.load(), (unsigned long long)c.lastNs.load(),
            (unsigned long long)c.maxGapNs.load(), snapshot);
    }
    for (size_t role = 0; role < RoleCount; ++role)
        for (size_t stage = 0; stage < Count; ++stage)
        {
            const auto& c = roleCounters[role][stage];
            if (c.calls.load() == 0) continue;
            std::fprintf(stderr,
                "DACS_CPU_ROLE_STAGE pid=%ld role=%s stage=%s calls=%llu bytes=%llu "
                "wall_ns=%llu cpu_ns=%llu exclusive_cpu_ns=%llu monotonic_ns=%llu snapshot=%s\n",
                static_cast<long>(getpid()), roleNames[role], stageNames[stage],
                (unsigned long long)c.calls.load(), (unsigned long long)c.bytes.load(),
                (unsigned long long)c.wallNs.load(), (unsigned long long)c.cpuNs.load(),
                (unsigned long long)c.exclusiveCpuNs.load(), (unsigned long long)Now(), snapshot);
        }
    std::fprintf(stderr, "DACS_CPU_DIAGNOSTIC_ERRORS count=%llu\n",
        (unsigned long long)clockErrors.load());
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
    {
        clockErrors.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "DACS_CPU_ERROR operation=getrusage error=%d\n", errno);
        return;
    }
    const auto heap = mallinfo2();
    std::fprintf(stderr,
        "DACS_UPLOAD_RESOURCE pending_bytes=%llu peak_pending_bytes=%llu failures=%llu maxrss_kib=%ld minor_faults=%ld major_faults=%ld voluntary_switches=%ld involuntary_switches=%ld user_usec=%ld system_usec=%ld arena_bytes=%llu mmap_count=%llu mmap_bytes=%llu arena_used_bytes=%llu arena_free_bytes=%llu monotonic_ns=%llu snapshot=%s\n",
        (unsigned long long)pendingBytes.load(), (unsigned long long)peakPendingBytes.load(),
        (unsigned long long)failures.load(), usage.ru_maxrss, usage.ru_minflt, usage.ru_majflt,
        usage.ru_nvcsw, usage.ru_nivcsw,
        usage.ru_utime.tv_sec * 1000000L + usage.ru_utime.tv_usec,
        usage.ru_stime.tv_sec * 1000000L + usage.ru_stime.tv_usec,
        (unsigned long long)heap.arena, (unsigned long long)heap.hblks,
        (unsigned long long)heap.hblkhd, (unsigned long long)heap.uordblks,
        (unsigned long long)heap.fordblks, (unsigned long long)Now(), snapshot);
}
struct FinalSnapshot
{
    FinalSnapshot()
    {
        if (Enabled())
        {
            std::fputs("DACS_CPU_RUNTIME_MARKER version=4 thread_cpu=1 exclusive_cpu=1 roles=1\n", stderr);
            SampleMemory(0);
        }
    }
    ~FinalSnapshot() { Log("shutdown"); }
};
inline FinalSnapshot finalSnapshot;
}
