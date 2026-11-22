# Cold-cache CPU diagnostics

This branch adds opt-in diagnostics on top of v0.1.0. It does not suppress
uploads, change upload worker defaults, change requested read sizes, or add a
foreground shutdown/population barrier.

Enable `DACS_UPLOAD_DIAGNOSTICS=1` for stage counters and
`DACS_READ_PRESSURE_TRACE=1` for bounded per-operation timestamps, scheduler wait
and fault deltas. Both default to disabled. Keep OpenMP settings unchanged.

Each scope measures `CLOCK_THREAD_CPUTIME_ID` and monotonic wall time. Inclusive
CPU contains nested scopes; exclusive CPU subtracts their inclusive CPU.
Foreground, replay, uploader, and unknown roles are reported separately.
Passive upload queue latency has zero attributed CPU. Condition-variable waits
measure caller CPU and elapsed time, not CPU spent while blocked.

Completed trace ranges are emitted during reads and uploads, rather than only
at process destruction. Version-4 batches carry `first_index`, `next_index`,
and `total_reserved`; an unfinished earlier operation delays later records.
Final snapshots identify incomplete and dropped records. Reject measurements
with diagnostic errors, dropped records, or an incomplete final trace.

The `observer` stage measures emission and logging-lock acquisition. It is not
all instrumentation overhead: boundary clocks, counters, scheduler/fault
snapshots, and memory sampling also cost CPU. Compare enabled and disabled
sanity runs before interpreting small differences. Do not add overlapping
inclusive DACS, RunAI clone, and OpenMP work measurements together.

Process memory is sampled at initialization, every 100 positive-byte foreground
wrapper completions, and shutdown. Replay reads do not advance that cadence.
Memory samples are emitted while the process is alive.

Initialize profiling independently in each process. Forking after native client
or profiling initialization is not supported; inherited counters and worker
state are not a fresh child-process measurement.

`CpuDiagnosticsTest.*` covers nested exclusive accounting, errno preservation,
role restoration, owned copy semantics, concurrent event completion, and
incremental emission. Run the native tests with all four combinations of the
two diagnostic flags. Existing StreamingClient and wrapper tests must also pass.
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
