---
name: hotmemory
description: Profile MPI applications for per-kernel hot working sets, memory traffic, FLOPs, and GPU-memory planning.
user-invocable: true
---

# Hot Memory profiler

Use the `hotmemory-profiler` MCP tools for Linux MPI applications. The native
tools run inside the Hot Memory Singularity/Apptainer image; this plugin only
provides the agent-facing workflow.

## Workflow

1. Confirm the target's documented baseline build and run command.
2. Run `wss_capability_check` first. Treat its runtime probe as authoritative
   for hot-byte, FLOP, and memory-access support.
3. Run `wss_measure_baseline` with the same MPI command and `working_dir` to
   record rank-0 peak RSS.
4. If `perf_stat_ok` is true, run `wss_perf_profile` to find user-code
   hotspots. Otherwise identify kernels from the source.
5. Ask before editing source. Add `WSS_INIT()` after `MPI_Init()` and wrap
   selected kernels with `WSS_BEGIN()`/`WSS_END("name")`.
6. Rebuild with `-DPROFILE_WSS -lwss_profiler -lpapi`, then run
   `wss_run_profiled`.

## Interpretation

- `hot_mb` is the unique page-backed working set touched by a kernel.
- `accessed_mb` is load/store traffic when PAPI events are available.
- `gflop` is the measured floating-point operation count.
- Report unavailable counters as `n/a`, not as meaningful zeroes.
- State the 4 KiB page granularity and smaps noise caveats.
- Treat results as MPI-only; use `OMP_NUM_THREADS=1` and do not interpret
  worker-thread counters as representative.
- For GPU planning, compare device capacity with the maximum hot set and
  discuss data that remains resident across consecutive kernels.

Never nest WSS measurements. The profiler measures rank 0 and assumes roughly
balanced work across MPI ranks.
