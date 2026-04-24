---
description: HPC hot-memory profiler for GPU memory planning. Measures per-kernel working set sizes in MPI C/C++/Fortran codes. Use when the user asks about profiling, hot memory, working sets, GPU memory, or says "get started".
when_to_use: User asks to profile code, measure memory, find hotspots, plan GPU porting, or just wants to know what this tool does.
---

# Hot-Memory Profiler

Welcome! This container is an HPC profiling tool. It measures the **hot
working set** of each kernel in your code — the actual bytes touched, not
just total allocation. This is the key input for GPU memory planning.

**Critical policy:** perform this analysis as **MPI-only**. Ignore OpenMP.
If the target code has OpenMP support, disable it for the profiled run or
force `OMP_NUM_THREADS=1` and interpret the results as MPI-only.

## What this tool does

Given your MPI code, it answers:
1. **Where is time going?** (sampling via `perf`)
2. **How much memory does each kernel actually touch?** (via `/proc/clear_refs` + PAPI)
3. **Will it fit on a GPU?** (comparing per-kernel hot sets against device memory)

## How to get started

1. If you don't have a `/my-code` skill yet, ask me to generate one:
   *"Generate a skill file for this project."*
2. Then ask: *"Measure the hot memory of the key kernels."*
3. I'll handle the rest — build, instrument, run, and report.

---

## Interaction protocol

This workflow must be **interactive**. Do not silently run through all phases.

Before each major step:
- explain **what you are about to do**
- explain **why that step matters**
- list **what steps will remain afterwards**
- ask for permission before proceeding

After each completed step:
- briefly report what you found
- explain how that affects the plan
- ask for permission for the next major step

Use this cadence for:
- baseline build and run
- capability check
- baseline peak-memory measurement
- perf hotspot profiling
- source instrumentation
- profiled rebuild
- profiled execution

For source edits, be explicit about which files you plan to modify and which
functions or call sites you plan to instrument before asking permission.

Example phrasing:
```text
Next I want to run the capability check. This will tell us whether this
machine can measure hot bytes, perf samples, and FLOPs. After that, the
remaining steps would be baseline peak-memory measurement, hotspot discovery,
instrumentation, rebuild, and the profiled run. Do you want me to proceed?
```

Do not ask permission for every shell command inside a step. Ask once per
major step, then complete that step fully.

---

## General rule: log everything

Always redirect profiling output to files so the user can review it later.
Name logs descriptively: `phase1_peak_memory.log`, `phase2_perf.log`,
`phase3_hotset_run1.log`, etc. Keep the full log — the user may want to
inspect it for warnings, timing data, or unexpected output.

---

## Container requirements

- gcc / g++ / gfortran, OpenMPI (`mpicc`, `mpirun`)
- PAPI (`libpapi-dev`, `papi-tools`), `perf` (`linux-tools-generic`)
- Runtime flags: `--privileged` (needed for `clear_refs` and `perf_event_open`)
- Host may need: `sysctl kernel.perf_event_paranoid=-1`
- OpenMPI rank binding must be disabled for profiling in this container:
  use `OMPI_MCA_hwloc_base_binding_policy=none` or `mpirun --bind-to none`

---

## Prerequisite: Confirm baseline build and run

Before profiling anything, read the `/my-code` skill, build the code, and
run it. Verify it exits cleanly with the expected output. Do not proceed
until the baseline build and run are confirmed working.

If the code normally uses OpenMP, disable it before proceeding. Do not try
to interpret mixed MPI+OpenMP runs.

If the project already has a build system (`Makefile`, `CMakeLists.txt`,
configure script, documented build script, etc.), use that build system.
Do not improvise a handwritten `mpicc`/`gcc` compile line unless there is no
project build system at all. If a profiling target already exists, use it.

Before doing this prerequisite step, explain that you are validating the
unmodified baseline first, state that the remaining steps are capability
check, baseline memory, hotspot discovery, instrumentation, rebuild, and
profiled execution, and ask permission.

---

## Workflow

### Step 0: `wss_capability_check` — what can this machine measure?

Call `wss_capability_check` (optionally with `extra_codes` for additional
PMU event codes to probe). **Always run first.**

Before running it, tell the user you are checking machine capabilities.
State that the remaining steps after this are baseline memory measurement,
hotspot discovery, instrumentation, rebuild, and profiled execution. Ask
permission before proceeding.

What to do with the result:
- Show the raw capability result to the user before paraphrasing it.
- Treat the WSS runtime probe embedded in `wss_capability_check` as the
  authoritative source of truth for hot-byte, FLOP, and accessed-byte
  capabilities. It exercises the same WSS instrumentation path used by real
  profiled programs.
- Tell the user exactly where the truth came from: cite `capability_truth_source`,
  `fp_source`, `fp_events`, and `fp_events_provenance` when discussing FP
  capability on machines without PAPI support.
- Read `summary.unavailable` and report each item to the user before proceeding.
- On `aarch64`, the tool should automatically retry with `["0x74","0x75"]`
  when PAPI FP events are unavailable. If `fp_events` is still empty and
  `perf_stat_ok` is true, then try again with additional architecture-specific
  extra codes.
- If a direct runtime probe works but an MPI-launched run reports zero FLOPs,
  check OpenMPI CPU binding first. In this container the default binding can
  pin rank 0 in a way that makes raw perf fallback counters read zero. Retry
  with `mpirun --bind-to none ...` or ensure
  `OMPI_MCA_hwloc_base_binding_policy=none` is set.
- Distinguish two different memory metrics:
  - `accessed_mb`: byte-based load/store estimate, only available when PAPI
    load/store events are available
  - PMU `mem_access` fallback: total memory-access event count, useful for
    traffic intensity and `FLOP/access` even when `accessed_mb` is unavailable
- If `clear_refs_ok` is false, stop and tell the user to re-run with `--privileged`.
- The tool stores `fp_events` internally; `wss_run_profiled` picks them up automatically.
- If ad-hoc shell experiments disagree with the capability result, rerun the
  capability check and show the raw result again. Do not replace the
  capability model with speculative prose.

Example report to give the user:
```
Capability check:
  ✓ Hot-byte measurement (/proc/clear_refs)
  ✓ perf sampling (hotspot discovery)
  ✗ PAPI FP counters (libpfm4 doesn't know this CPU)
  ✓ perf_event_open fallback: WSS_PERF_FP_EVENTS=0x74,0x75
  ✗ PAPI load/store counters (byte-based accessed MB unavailable)
  ✓ PMU mem_access fallback (total memory-access events + FLOP/access)
```

### Step 1: `wss_measure_baseline` — peak RSS upper bound

Call `wss_measure_baseline` with `binary_command` (and `mpirun_prefix` for
MPI codes, e.g. `"mpirun -np 4"`).

Before running it, tell the user you are measuring peak RSS as an upper bound
for later hot-set comparisons. State that the remaining steps after this are
hotspot discovery, instrumentation, rebuild, and profiled execution. Ask
permission before proceeding.

- Record `peak_rss_mb`. This is the upper bound for all hot-set comparisons.
- Report to user: "Peak memory for rank 0: X MB. Per-kernel hot sets will be
  fractions of this."

### Step 2: `wss_perf_profile` — identify which kernels to instrument

Call `wss_perf_profile` with the same `mpirun_prefix` and `binary_command`.

Before running it, tell the user you are sampling the code to decide which
functions are worth instrumenting. State that the remaining steps after this
are instrumentation, rebuild, and profiled execution. Ask permission before
proceeding.

- Read `raw_report`. Identify the top functions by sample %. Note which are
  in user code vs. MPI/library code.
- Treat too-few-samples as a profiling setup problem, not a code conclusion.
  If the report is dominated by MPI/runtime frames or has very few samples,
  rerun with a longer-lived workload or more iterations before deciding that
  the user kernels are absent.
- The intended perf configuration is cycle sampling with frequent samples and
  DWARF call stacks. If you need to reason about the raw command, expect
  `perf record -e cycles -F 99 --call-graph=dwarf`.
- Tell the user which functions appear hottest and ask which to instrument,
  or make a recommendation based on the data.
- If `exit_code` is non-zero and `perf_record_output` mentions "Permission denied",
  inform the user that `perf_event_paranoid` must be ≤ 1 on the host. Phase 3
  (hot-byte measurement) can still proceed if the user already knows which
  kernels to target.

### Step 3: Instrument, rebuild, then `wss_run_profiled`

This step requires agent judgment. The MCP tool runs the binary; **you** decide
how and where to instrument it.

Treat this as three separate permission gates:

1. **Instrumentation plan approval**
   - Explain which files you plan to edit.
   - Explain which functions or call sites you plan to wrap with `WSS_BEGIN()` / `WSS_END()`.
   - Explain why that granularity is appropriate.
   - State that the remaining steps after instrumentation are rebuild and profiled execution.
   - Ask permission before editing files.

2. **Rebuild approval**
   - Explain the exact existing build target or build command you will use.
   - Prefer the project's own build system or profiling target if one exists
     (for example `make profile`).
   - If profiling support is missing, modify the build system or build target
     so it adds `-DPROFILE_WSS -lwss_profiler -lpapi` correctly.
   - Do not bypass the build system with a one-off handwritten compiler
     invocation unless the project has no build system at all.
   - Explain that profiling requires `-DPROFILE_WSS -lwss_profiler -lpapi`.
   - State that the only remaining step after rebuild is the profiled execution.
   - Ask permission before rebuilding.

3. **Profiled run approval**
   - Explain exactly which profiled binary command you will run.
   - State that the remaining work after the run is result interpretation and GPU-memory planning.
   - Ask permission before running it.

1. **Add instrumentation** (see "Instrumentation strategy" below).
2. **Rebuild** using the project's build system if it exists. Prefer an
   existing profiling target. If profiling flags are missing, update the
   build system or build target to add `-DPROFILE_WSS -lwss_profiler -lpapi`
   correctly. Only fall back to a manual compiler invocation when the
   project has no build system at all.
3. Call `wss_run_profiled` with `mpirun_prefix` and `binary_command`.
   - `fp_events` from Step 0 are injected automatically.
4. Parse `measurements[]` for results. Check `errors[]` for `[WSS] ERROR` lines.
5. Present the results table (see "How to interpret results" below).

---

## Instrumentation strategy

`wss_profiler.h` is at `/usr/local/include` in the container. Include it with
`#include "wss_profiler.h"` (no `-I` flag needed). Link with `-lwss_profiler -lpapi`.

### Macros

| Macro | Where | What it does |
|-------|-------|--------------|
| `WSS_INIT()` | Once, after `MPI_Init()` | Identifies rank 0, initialises PAPI |
| `WSS_BEGIN()` | Before each kernel call | Clears `/proc/self/clear_refs`, starts counters |
| `WSS_END("name")` | After each kernel call | Stops counters, reads smaps, prints `[WSS]` line |

When built without `-DPROFILE_WSS`, all macros compile away to nothing.

### Granularity (the most important decision)

- **Too coarse** (wrap an entire solver): hot set ≈ peak allocation, not useful.
- **Too fine** (individual vector ops): noise dominates.
- **Goal**: find the level where distinct computational phases have meaningfully
  different working sets.

Process:
1. Read the source to understand the main loop structure and data access patterns.
2. Start coarse (one solver iteration). If hot set ≈ peak, go deeper.
3. Look for phase boundaries: before/after matvec, preconditioner, halo exchange.
4. For iterative solvers (CG, BiCGStab), the interesting level is usually
   individual operations within one iteration: matvec, preconditioner, dot products,
   vector updates.
5. Name measurements descriptively: `"matvec"`, `"preconditioner"`, not `"step1"`.

### NEVER nest WSS_BEGIN/WSS_END calls

The profiler uses a single global PAPI eventset and a single `clear_refs` state.
Nesting corrupts both. To measure at multiple levels of a hierarchy, do **separate runs**:
- Run 1: BEGIN/END around the whole solver
- Run 2: BEGIN/END around each individual operation inside the solver

### Example — BiCGStab kernel

```c
// main.c — init only
#include "wss_profiler.h"
MPI_Init(&argc, &argv);
WSS_INIT();

// solver.c — measurements
#include "wss_profiler.h"
WSS_BEGIN();
matvec(A, p, Ap);
WSS_END("matvec");

WSS_BEGIN();
precondition(M, r, z);
WSS_END("preconditioner");

WSS_BEGIN();
dot_product(r, z, &rz);
WSS_END("dot_product");

WSS_BEGIN();
axpy(alpha, p, x);
WSS_END("vector_update");
```

### Fortran codes

Use `use wss_profiler_mod` and call `wss_init()`, `wss_begin()`,
`wss_end_named("name")`. Link with `-lwss_profiler -lpapi` and pass
`-I/usr/local/include` so mpif90 finds `wss_profiler_mod.mod` (Fortran
compilers do NOT search system include paths for `.mod` files by default).

### OpenMP policy

Do not analyse OpenMP execution with this workflow. Hardware counters (PAPI
and the perf fallback) only instrument the main thread, while the hot-byte
measurement (smaps) is process-wide. That mismatch makes mixed MPI+OpenMP
runs hard to interpret consistently.

If a codebase supports OpenMP:
- build with OpenMP disabled when practical
- otherwise run with `OMP_NUM_THREADS=1`
- describe the run and the results as MPI-only

---

## How to interpret results

The `measurements[]` array from `wss_run_profiled` contains per-kernel objects:

| Field | Meaning |
|-------|---------|
| `hot_mb` | Unique pages touched — the GPU memory this kernel needs |
| `accessed_mb` | Total bytes loaded+stored (0 if PAPI load/store unavailable) |
| `gflop` | Floating-point operations (0 if no FP counters) |
| `flop_per_byte_hot` | FLOPs per byte of working set |
| `flop_per_byte_acc` | FLOPs per byte of traffic (closest to roofline arithmetic intensity) |

When `accessed_mb > hot_mb`, the kernel revisits data (reuse ratio = accessed/hot).
When they're close, the kernel streams through data once.

### Results table format

Always include Phase 1 peak RSS as context:

```
Peak memory (rank 0): 3072 MB

| Kernel        | % Time | Hot MB | % of Peak | Accessed MB | Reuse | GFLOP | FLOP/B-hot | FLOP/B-acc | Assessment   |
|---------------|--------|--------|-----------|-------------|-------|-------|------------|------------|--------------|
| stencil_apply |  42.3% |    512 |     16.7% |        1024 |  2.0x |  0.48 |       0.98 |       0.47 | memory-bound |
| fft_forward   |  28.1% |    128 |      4.2% |         256 |  2.0x |  0.19 |       1.57 |       0.74 | borderline   |
```

Show unavailable counter columns as "n/a" rather than 0. Hot MB and % of Peak always work.

**FLOP/byte interpretation:**
- < 1: sweeps data with little reuse — almost certainly memory-bandwidth-bound
- 1–5: borderline — depends on hardware balance
- > 10: compute-heavy — likely compute-bound

**Caveats to always state:**
- Hot bytes are at 4 KB page granularity (rounded up). Small working sets have significant rounding error.
- Ignore OpenMP for analysis. Build with OpenMP disabled or run with `OMP_NUM_THREADS=1`.
- smaps includes stack, code, and library pages (~a few MB of noise). Negligible for large working sets.

---

## GPU memory planning

### Key insight

**GPU memory planning is not about total allocation. It is about the maximum hot working set across all kernels, plus what needs to stay resident between them.**

### "Will it fit on GPU X?"

```
max_hot_mb = max(hot_mb across all profiled kernels)
device_mem_mb = <device memory in MB, e.g. 81920 for A100 80GB>

if max_hot_mb < device_mem_mb:
    "The heaviest kernel's working set is [max_hot_mb] MB, which fits in
     [device] memory ([device_mem_mb] MB). Total allocation is [total] GB
     but no single kernel needs all of it."
else:
    "The heaviest kernel ([name]) needs [max_hot_mb] MB, which exceeds
     [device] memory by [max_hot_mb - device_mem_mb] MB. Tiling or
     explicit swapping will be required."
```

### Transfer plan

Reason about execution order. For a time-stepping loop:
- **Data resident across transitions**: arrays hot in consecutive kernels stay on device.
- **Transfer cost of A → B**: `hot(B) - overlap(A,B)`, not `hot(B)`.
- **Minimum resident set**: union of arrays hot in kernels that must remain on device.

### Swap cost per timestep

```
swap_per_step ≤ sum(hot MB for all kernels in one timestep)   [worst case: no reuse]
swap_per_step ≥ max(hot MB)                                   [best case: all fits]
```

### Framing rules

- Never say "total allocation is X GB, so you need X GB of GPU memory." Always clarify hot set < total allocation.
- When you don't have array-level attribution, you can only bound overlap — say so.
- If the user asks about a GPU before Step 3 is done, explain you need measurements first and offer to run Step 3.

---

## Generating a code skill for the target project

When the user asks you to generate a skill file for their project:

1. Explore the source tree: file extensions, language (C, C++, Fortran, mixed).
2. Find the build system: `Makefile`, `CMakeLists.txt`, configure, or scripts.
3. Identify the entry point: `main()` (C/C++) or `program` (Fortran).
4. Try building and running. Note the exact commands that work.
5. Determine build extensibility — how to add `-I`, `-l`, `-D`, `-I` for `.mod` files, etc.
6. Write the SKILL.md following the template at `/skills/code-template/SKILL.md`.
7. Save to `/skills/my-code/SKILL.md`.

The generated skill must NOT reference this profiler, WSS macros, PAPI, or any instrumentation.

---

## What this skill does NOT know

- How to build or run the user's code — that is in the user's code skill.
- What the user's kernels do or what performance is expected.
- Array-level attribution of hot pages (planned future work).
- PAPI counter availability on specific microarchitectures — `wss_capability_check` detects this.
