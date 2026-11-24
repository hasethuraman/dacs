# Cold-cache CPU diagnostics

This branch adds opt-in diagnostics on top of v0.1.0. It does not suppress
uploads, change upload worker defaults, change requested read sizes, or add a
foreground shutdown/population barrier.

Enable `DACS_UPLOAD_DIAGNOSTICS=1` for stage counters and
`DACS_THREAD_CPU_TRACE=1` for bounded Read-scope CPU/timestamp records and
100-read memory samples. Optional `DACS_READ_PRESSURE_TRACE=1` additionally
collects procfs scheduler wait and fault deltas; it is not needed for CPU timing.
All diagnostics default to disabled. Keep OpenMP settings unchanged.

Full `blob_read` latency and CPU need only `DACS_UPLOAD_DIAGNOSTICS=1`.
Every 100 completed positive-byte foreground wrapper calls, pooled across
threads within one process, produce one native aggregate. Existing periodic
and shutdown hooks emit buffered `DACS_BLOB_READ_SAMPLE` records; shutdown
also emits the actual final partial batch and `DACS_BLOB_READ_SAMPLE_END`.
No per-call wrapper trace or eBPF wrapper state capture is needed.

Samples retain calls, bytes, sequence, earliest start/latest end, summed
wall/CPU nanoseconds, `mean_wall_ms`, `mean_cpu_ms`, and `cpu_percent`.
Means divide native totals by actual calls. CPU percentage divides summed
thread CPU by the batch span, times 100; idle gaps remain in the span and
concurrent CPU can exceed 100%. Graphs plot native means/CPU directly, without
regrouping. The lifetime buffer holds 4096 samples; reject explicit errors,
dropped calls, missing samples or incomplete final drain.

SDK reads (`Blob`), `Probe`, and `Upload` also aggregate natively with only
`DACS_UPLOAD_DIAGNOSTICS=1`. Each stage has independent 100-call batches per
process/role, pooling threads and retaining zero-byte probes. Existing logging
hooks emit `DACS_STAGE_LATENCY_SAMPLE` and final-drain declarations. Shutdown
flushes each role's actual partial count. Each stage holds 4096 lifetime samples;
loss, unfinished scopes, clock errors and counter mismatches reject a capture.
Stage CPU counters remain unchanged. Detailed per-call stage traces now require
`DACS_STAGE_LATENCY_TRACE=1` together with CPU/pressure tracing; they are off by
default to avoid per-call buffering, procfs pressure sampling and trace logs.

Graphs plot native means without regrouping. Primary curves include only
batches wholly inside streaming. Boundary-crossing batches remain exported and
are explicitly marked excluded; their pre-end subset is unknown, not zero.
The deferred-upload appendix retains complete native batches, including those
crossing streaming end. Native batch means remain exact for their actual calls;
aggregation does not preserve exact individual completion order or per-second
completion rates.

Retained stages are `Read`, `Probe`, `Blob`, `Upload`, `Wrapper`,
`UploadIdleWait`, and `ReplayWait`. The waits support native upload/replay
activity CPU, not separate latency panels. Open/SI_pread/close, allocation/copy,
wire/probe substages, queue timers, and Server stages have no diagnostic timers.
Their actual storage, allocation, copy, checksum, network and upload operations
are preserved.

Each scope measures `CLOCK_THREAD_CPUTIME_ID` and monotonic wall time. Inclusive
CPU contains nested scopes; exclusive CPU subtracts their inclusive CPU.
Foreground, replay, uploader, and unknown roles are reported separately.
Passive upload queue latency has zero attributed CPU. Condition-variable waits
measure caller CPU and elapsed time, not CPU spent while blocked.

Numeric trace ranges are buffered during operations and emitted from existing
StreamingClient statistics calls and shutdown, not formatted per read or upload.
Version-4 batches carry `first_index`, `next_index`,
and `total_reserved`; an unfinished earlier operation delays later records.
Final snapshots identify incomplete and dropped records. Reject measurements
with diagnostic errors, dropped records, or an incomplete final trace.

Boundary clocks, counters, and optional memory/pressure sampling still cost
CPU; zero-overhead instrumentation is not claimed. There are no new observer
threads, no per-operation log flushes, and no clocks inside polling iterations.
Compare enabled and disabled sanity runs before interpreting small differences.
Do not add overlapping
inclusive DACS, RunAI clone, and OpenMP work measurements together.

Process memory is sampled at initialization, every 100 positive-byte foreground
wrapper completions, and shutdown. Replay reads do not advance that cadence.
Memory samples are emitted while the process is alive.

Initialize profiling independently in each process. Forking after native client
or profiling initialization is not supported; inherited counters and worker
state are not a fresh child-process measurement.

`CpuDiagnosticsTest.*` covers nested exclusive accounting, errno preservation,
role restoration, concurrent event completion, native 100/100/50-call samples,
thread pooling, capacity loss and incremental emission. Validate diagnostics
off, aggregate-only, CPU-trace and pressure modes. The dedicated
`BlobReadScopeSamplesUseNativeClocks` test qualifies actual sample logs in a
fresh process; synthetic aggregation tests are not measurement evidence.
Existing StreamingClient and wrapper tests must also pass.
The reusable builder lives in
`v2-test-utils/scripts/vllm-gpu-testing/cold-cpu-diagnostics/build-dacs.sh`:

```sh
bash build-dacs.sh /absolute/source /new/protected/evidence \
  'CpuDiagnosticsTest.*:StreamingClientTest.*:BlobSiWrapperTest.*'
```

The builder only compiles and tests; it does not install or publish anything.
It pins the diagnostic runtimes to the v0.1.0 wheel's OpenSSL 3.5.9 and curl
8.23.0, records native compiler and
library provenance, and excludes unused Azure SDK AMQP/Rust projects from the
build. Output may contain source code and must remain private for private inputs.
