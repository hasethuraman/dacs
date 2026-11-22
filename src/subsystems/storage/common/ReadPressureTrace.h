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
#include <fcntl.h>
#include <mutex>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace readpressure
{
inline constexpr uint64_t Capacity = 262144;
struct QueueCounter
{
    uint64_t value = 0;
    int error = 0;
};

inline QueueCounter ReadRunQueue(int descriptor)
{
    char buffer[128]{};
    const auto length = pread(descriptor, buffer, sizeof(buffer) - 1, 0);
    if (length < 0) return {0, errno};
    unsigned long long execution = 0, queue = 0, slices = 0;
    if (std::sscanf(buffer, "%llu %llu %llu", &execution, &queue, &slices) != 3)
        return {0, EIO};
    return {queue, 0};
}

struct Stamp
{
    QueueCounter queue;
    uint64_t minorFaults = 0, majorFaults = 0;
    int faultError = 0;
};

inline bool PressureEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("DACS_READ_PRESSURE_TRACE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

inline Stamp Snapshot()
{
    if (!PressureEnabled()) return {{0, ENODATA}, 0, 0, ENODATA};
    const int savedErrno = errno;
    Stamp result;
    const int descriptor = open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
    result.queue = descriptor < 0 ? QueueCounter{0, errno} : ReadRunQueue(descriptor);
    if (descriptor >= 0) close(descriptor);
    rusage usage{};
    if (getrusage(RUSAGE_THREAD, &usage) != 0)
        result.faultError = errno;
    else
    {
        result.minorFaults = static_cast<uint64_t>(usage.ru_minflt);
        result.majorFaults = static_cast<uint64_t>(usage.ru_majflt);
    }
    errno = savedErrno;
    return result;
}

enum class DumpState : uint8_t { Pending, Selected, Emitted };
struct Event
{
    const char* stage = nullptr;
    const char* role = "unknown";
    uint64_t tid = 0, bytes = 0, startNs = 0, endNs = 0, cpuNs = 0;
    uint64_t exclusiveCpuNs = 0;
    uint64_t pendingStart = 0, pendingEnd = 0, queueNs = UINT64_MAX;
    uint64_t minorFaults = UINT64_MAX, majorFaults = UINT64_MAX;
    Stamp before;
    int queueError = 0, faultError = 0;
    std::atomic<bool> ready{false};
    DumpState dumpState = DumpState::Pending;
};

inline std::array<Event, Capacity> events;
inline std::atomic<uint64_t> next{0}, dropped{0};
inline std::mutex dumpMutex;
inline uint64_t dumpedIndex = 0;

inline bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("DACS_THREAD_CPU_TRACE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled || PressureEnabled();
}

inline uint64_t Reserve()
{
    const auto index = next.fetch_add(1, std::memory_order_relaxed);
    if (index >= Capacity)
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return Capacity;
    }
    return index;
}

inline uint64_t Begin(const char* stage, uint64_t bytes, uint64_t start, uint64_t pending,
    const char* role = "unknown")
{
    if (!Enabled()) return Capacity;
    const auto index = Reserve();
    if (index == Capacity) return index;
    const int savedErrno = errno;
    auto& event = events[index];
    event.stage = stage;
    event.role = role;
    event.bytes = bytes;
    event.startNs = start;
    event.pendingStart = pending;
    event.before = Snapshot();
    static thread_local const auto tid = static_cast<uint64_t>(syscall(SYS_gettid));
    event.tid = tid;
    errno = savedErrno;
    return index;
}

inline void Finish(uint64_t index, const Stamp& after, uint64_t end, uint64_t cpu, uint64_t pending,
    uint64_t exclusiveCpu = 0)
{
    auto& event = events[index];
    event.endNs = end;
    event.cpuNs = cpu;
    event.exclusiveCpuNs = exclusiveCpu;
    event.pendingEnd = pending;
    event.queueError = event.before.queue.error ? event.before.queue.error : after.queue.error;
    event.faultError = event.before.faultError ? event.before.faultError : after.faultError;
    if (!event.queueError)
    {
        if (after.queue.value < event.before.queue.value) event.queueError = EOVERFLOW;
        else event.queueNs = after.queue.value - event.before.queue.value;
    }
    if (!event.faultError)
    {
        if (after.minorFaults < event.before.minorFaults || after.majorFaults < event.before.majorFaults)
            event.faultError = EOVERFLOW;
        else
        {
            event.minorFaults = after.minorFaults - event.before.minorFaults;
            event.majorFaults = after.majorFaults - event.before.majorFaults;
        }
    }
    event.ready.store(true, std::memory_order_release);
}

inline void Dump(bool final = true)
{
    if (!Enabled()) return;
    std::lock_guard<std::mutex> lock(dumpMutex);
    const auto count = std::min(next.load(), Capacity);
    uint64_t completed = 0, incomplete = 0;
    for (uint64_t index = dumpedIndex; index < count; ++index)
        if (events[index].dumpState == DumpState::Pending)
        {
            if (events[index].ready.load(std::memory_order_acquire))
            { events[index].dumpState = DumpState::Selected; ++completed; }
            else ++incomplete;
        }
    if (completed == 0 && !final) return;
    std::fprintf(stderr, "DACS_READ_TRACE_BEGIN version=4 capacity=%llu events=%llu dropped=%llu first_index=%llu total_reserved=%llu final=%d\n",
        (unsigned long long)Capacity, (unsigned long long)completed,
        (unsigned long long)dropped.load(), (unsigned long long)dumpedIndex,
        (unsigned long long)count, final ? 1 : 0);
    for (uint64_t index = dumpedIndex; index < count; ++index)
    {
        auto& event = events[index];
        if (event.dumpState != DumpState::Selected) continue;
        std::fprintf(stderr,
            "DACS_READ_TRACE stage=%s tid=%llu bytes=%llu start_ns=%llu end_ns=%llu cpu_ns=%llu runqueue_ns=%llu pending_start=%llu pending_end=%llu minor_faults=%llu major_faults=%llu runqueue_error=%d fault_error=%d role=%s exclusive_cpu_ns=%llu pid=%ld\n",
            event.stage, (unsigned long long)event.tid, (unsigned long long)event.bytes,
            (unsigned long long)event.startNs, (unsigned long long)event.endNs,
            (unsigned long long)event.cpuNs, (unsigned long long)event.queueNs,
            (unsigned long long)event.pendingStart, (unsigned long long)event.pendingEnd,
            (unsigned long long)event.minorFaults, (unsigned long long)event.majorFaults,
            event.queueError, event.faultError, event.role,
            (unsigned long long)event.exclusiveCpuNs, static_cast<long>(getpid()));
        event.dumpState = DumpState::Emitted;
    }
    while (dumpedIndex < count && events[dumpedIndex].dumpState == DumpState::Emitted) ++dumpedIndex;
    std::fprintf(stderr, "DACS_READ_TRACE_END events=%llu incomplete=%llu dropped=%llu next_index=%llu total_reserved=%llu final=%d\n",
        (unsigned long long)completed, (unsigned long long)(final ? incomplete : 0),
        (unsigned long long)dropped.load(), (unsigned long long)dumpedIndex,
        (unsigned long long)count, final ? 1 : 0);
}
}
