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

#include "UploadDiagnostics.h"
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include <thread>
#include <vector>

TEST(CpuDiagnosticsTest, NestedCpuIsExclusiveAndErrnoIsPreserved)
{
    const auto parentCpu = uploaddiag::counters[uploaddiag::Read].cpuNs.load();
    const auto parentExclusive = uploaddiag::counters[uploaddiag::Read].exclusiveCpuNs.load();
    const auto childCpu = uploaddiag::counters[uploaddiag::Blob].cpuNs.load();
    errno = EAGAIN;
    {
        uploaddiag::RoleScope role(uploaddiag::Foreground);
        uploaddiag::Scope parent(uploaddiag::Read, 123);
        {
            uploaddiag::Scope child(uploaddiag::Blob, 123);
            volatile uint64_t value = 1;
            for (unsigned i = 0; i < 100000; ++i) value = value * 1664525 + 1013904223;
        }
        parent.Stop();
        parent.Stop();
    }
    EXPECT_EQ(errno, EAGAIN);
    EXPECT_EQ(uploaddiag::currentScope, nullptr);
    EXPECT_EQ(uploaddiag::currentRole, uploaddiag::Unknown);
    EXPECT_EQ(uploaddiag::counters[uploaddiag::Read].cpuNs.load() - parentCpu,
        uploaddiag::counters[uploaddiag::Read].exclusiveCpuNs.load() - parentExclusive
        + uploaddiag::counters[uploaddiag::Blob].cpuNs.load() - childCpu);
    EXPECT_EQ(uploaddiag::clockErrors.load(), 0U);
}

TEST(CpuDiagnosticsTest, CopyPreservesOwnedBytesAndAttributesTheRole)
{
    std::array<unsigned char, 128> source, destination;
    source.fill(37);
    const auto before = uploaddiag::roleCounters[uploaddiag::Replay][uploaddiag::PartCopy].calls.load();
    {
        uploaddiag::RoleScope role(uploaddiag::Replay);
        uploaddiag::Copy(destination.data(), source.data(), source.size(), uploaddiag::PartCopy);
    }
    source.fill(0);
    for (const auto value : destination) EXPECT_EQ(value, 37);
    EXPECT_EQ(uploaddiag::roleCounters[uploaddiag::Replay][uploaddiag::PartCopy].calls.load() - before,
        uploaddiag::Enabled() ? 1U : 0U);
}

TEST(CpuDiagnosticsTest, ExceptionRestoresScopeAndRole)
{
    EXPECT_THROW(([] {
        uploaddiag::RoleScope role(uploaddiag::Replay);
        uploaddiag::Scope scope(uploaddiag::Blob, 17);
        throw std::runtime_error("diagnostic propagation test");
    }()), std::runtime_error);
    EXPECT_EQ(uploaddiag::currentScope, nullptr);
    EXPECT_EQ(uploaddiag::currentRole, uploaddiag::Unknown);
    EXPECT_EQ(uploaddiag::counters[uploaddiag::Blob].active.load(), 0U);
}

TEST(CpuDiagnosticsTest, ConcurrentScopesPublishDistinctCompleteRoles)
{
    const auto before = readpressure::next.load();
    const auto calls = uploaddiag::counters[uploaddiag::Read].calls.load();
    std::vector<std::thread> threads;
    for (unsigned index = 0; index < 8; ++index)
        threads.emplace_back([index] {
            uploaddiag::RoleScope role(index % 2 ? uploaddiag::Replay : uploaddiag::Uploader);
            for (unsigned call = 0; call < 100; ++call)
            { uploaddiag::Scope scope(uploaddiag::Read, 4096); }
        });
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(uploaddiag::counters[uploaddiag::Read].calls.load() - calls,
        uploaddiag::Enabled() ? 800U : 0U);
    const bool traced = uploaddiag::Enabled() && readpressure::Enabled();
    ASSERT_EQ(readpressure::next.load() - before, traced ? 800U : 0U);
    if (!traced) return;
    unsigned replay = 0, uploader = 0;
    for (auto index = before; index < before + 800; ++index)
    {
        const auto& event = readpressure::events[index];
        ASSERT_TRUE(event.ready.load());
        ASSERT_LE(event.exclusiveCpuNs, event.cpuNs);
        ASSERT_LE(event.startNs, event.endNs);
        if (std::strcmp(event.role, "replay") == 0) ++replay;
        if (std::strcmp(event.role, "uploader") == 0) ++uploader;
    }
    EXPECT_EQ(replay, 400U);
    EXPECT_EQ(uploader, 400U);
    EXPECT_EQ(uploaddiag::clockErrors.load(), 0U);
}

TEST(CpuDiagnosticsTest, IncrementalDumpRetainsUnfinishedEvents)
{
    if (!readpressure::Enabled()) return;
    const auto index = readpressure::Begin("probe", 111, uploaddiag::Now(), 0);
    ASSERT_LT(index, readpressure::Capacity);
    readpressure::Dump(false);
    EXPECT_LE(readpressure::dumpedIndex, index);
    readpressure::Finish(index, readpressure::Snapshot(), uploaddiag::Now(), 0, 0);
    readpressure::Dump(false);
    EXPECT_EQ(readpressure::dumpedIndex, index + 1);
    readpressure::Dump(false);
    EXPECT_EQ(readpressure::dumpedIndex, index + 1);
}

TEST(CpuDiagnosticsTest, IdleWaitDoesNotBlockCompletedPayloadRecords)
{
    if (!readpressure::Enabled()) return;
    const auto idle = readpressure::Begin("upload_idle_wait", 0, uploaddiag::Now(), 0);
    const auto payload = readpressure::Begin("blob", 4096, uploaddiag::Now(), 0);
    ASSERT_LT(payload, readpressure::Capacity);
    readpressure::Finish(payload, readpressure::Snapshot(), uploaddiag::Now(), 0, 0);
    readpressure::Dump(false);
    EXPECT_EQ(readpressure::events[idle].dumpState, readpressure::DumpState::Pending);
    EXPECT_EQ(readpressure::events[payload].dumpState, readpressure::DumpState::Emitted);
    EXPECT_LE(readpressure::dumpedIndex, idle);
    readpressure::Finish(idle, readpressure::Snapshot(), uploaddiag::Now(), 0, 0);
    readpressure::Dump(false);
    EXPECT_EQ(readpressure::dumpedIndex, payload + 1);
}

TEST(CpuDiagnosticsTest, MemoryCadenceCountsForegroundCallbacksNotReplay)
{
    const auto before = uploaddiag::completedReads.load();
    const auto samples = uploaddiag::memoryNext.load();
    {
        uploaddiag::RoleScope role(uploaddiag::Replay);
        for (unsigned call = 0; call < 100; ++call)
        { uploaddiag::Scope scope(uploaddiag::Read, 4096); }
    }
    EXPECT_EQ(uploaddiag::completedReads.load(), before);
    {
        uploaddiag::RoleScope role(uploaddiag::Foreground);
        for (unsigned call = 0; call < 100; ++call)
        { uploaddiag::Scope scope(uploaddiag::Wrapper, 4096); }
        uploaddiag::Scope empty(uploaddiag::Wrapper, 0);
    }
    const bool sampled = uploaddiag::Enabled() && readpressure::Enabled();
    EXPECT_EQ(uploaddiag::completedReads.load() - before, sampled ? 100U : 0U);
    EXPECT_EQ(uploaddiag::memoryNext.load() - samples, sampled ? 1U : 0U);
}

TEST(CpuDiagnosticsTest, CpuOnlyTraceKeepsPressureCountersExplicitlyUnavailable)
{
    if (!readpressure::Enabled() || readpressure::PressureEnabled()) return;
    const auto index = readpressure::Begin("blob", 4096, uploaddiag::Now(), 0, "foreground");
    ASSERT_LT(index, readpressure::Capacity);
    readpressure::Finish(index, readpressure::Snapshot(), uploaddiag::Now(), 123, 0, 123);
    const auto& event = readpressure::events[index];
    ASSERT_TRUE(event.ready.load());
    EXPECT_EQ(event.cpuNs, 123U);
    EXPECT_EQ(event.exclusiveCpuNs, 123U);
    EXPECT_EQ(event.queueError, ENODATA);
    EXPECT_EQ(event.faultError, ENODATA);
}

TEST(CpuDiagnosticsTest, OperationCompletionBuffersWithoutAutomaticEmission)
{
    const auto before = readpressure::dumpedIndex;
    {
        uploaddiag::RoleScope role(uploaddiag::Foreground);
        uploaddiag::Scope read(uploaddiag::Read, 4096);
    }
    EXPECT_EQ(readpressure::dumpedIndex, before);
    if (!uploaddiag::Enabled() || !readpressure::Enabled()) return;
    EXPECT_GT(readpressure::next.load(), before);
    readpressure::Dump(false);
    EXPECT_EQ(readpressure::dumpedIndex, readpressure::next.load());
}
