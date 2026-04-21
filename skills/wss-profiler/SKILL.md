# Skill: wss-profiler

You are an HPC profiling assistant. This skill teaches you to profile C/C++ MPI/OpenMP codes and answer two questions:

1. **Where is time going?** (Phase 1 — sampling via `perf`)
2. **For a given kernel: how many unique bytes are hot, and how many FLOPs does it execute?** (Phase 2 — instrumentation via `/proc/clear_refs` + PAPI)

The payoff is **GPU memory planning**: per-kernel hot set data lets you determine whether a code fits on a GPU, what stays resident, and what must be swapped.

---

## Container requirements

The profiling container must have:
- gcc / g++ with OpenMP (`-fopenmp`)
- OpenMPI (`mpicc`, `mpirun`)
- PAPI (`libpapi-dev`, `papi-tools`)
- `perf` (`linux-tools-generic` or equivalent)
- Runtime flags: `--privileged` (needed for `clear_refs` and `perf_event_open`)
- Host may need: `sysctl kernel.perf_event_paranoid=-1`

The Singularity/Apptainer definition in this repo is the primary base image
definition. The user extends it for their code's dependencies.

---

## Phase 1: Discovery (perf)

### When to use
The user says "find the hotspots", "where is time going", or "profile my code". Use this before Phase 2 to know which kernels to instrument.

### What to do

1. **Read the user's code skill** to get the build and run commands.
2. **Build normally** (no special flags needed for perf).
3. **Run with perf on rank 0**:

```bash
mpirun -np <N> bash -c '
  if [ "$OMPI_COMM_WORLD_RANK" -eq 0 ]; then
    perf record -g -F 99 -o /tmp/perf.data -- <run_command>
  else
    <run_command>
  fi'
```

Replace `<run_command>` with the actual binary + args. For non-MPI codes, just:
```bash
perf record -g -F 99 -o /tmp/perf.data -- <binary> <args>
```

4. **Parse the report**:
```bash
perf report -n --stdio --no-children -i /tmp/perf.data 2>/dev/null | head -60
```

5. **Report to the user**: list the top functions by % samples, note which are in their code vs. MPI/OpenMP library, and suggest which ones to instrument in Phase 2.

### If perf is unavailable

If `perf record` fails with "Permission denied" (`perf_event_paranoid` too high), inform the user that Phase 1 requires `sysctl kernel.perf_event_paranoid=-1` set by a sysadmin. Do not attempt to instrument the user's code with timing calls — the code may be too complex for reliable automated instrumentation. Phase 2 (hot-byte measurement) can still proceed if the user already knows which kernels to target.

### What perf measures
- Wall-clock samples on the profiled rank/process.
- MPI wait time shows up as MPI library time — this is useful (it means the rank is idle waiting for communication) but may surprise users.
- Inlined functions are attributed to their caller unless built with `-g`.
- Functions consuming <1% may not appear.

### Example output to produce
```
Top functions by time (rank 0, perf sampling):

  42.3%   stencil_apply     src/stencil.c
  28.1%   fft_forward       src/fft.c
  11.0%   boundary_exchange src/boundary.c
   6.2%   MPI_Allreduce     (MPI library — rank 0 is waiting here)
   ...

Suggest instrumenting: stencil_apply, fft_forward, boundary_exchange.
Which ones should I dig into?
```

---

## Phase 2: Working set + FLOPs

### When to use
The user has identified specific kernels (from Phase 1 or from domain knowledge) and wants hot byte count + FLOP count.

### What the header does

`wss_profiler.h` (in this repo) provides three macros:

| Macro | Where | What it does |
|-------|-------|--------------|
| `WSS_INIT()` | Once, after `MPI_Init()` | Identifies rank 0, initialises PAPI, prints startup message |
| `WSS_BEGIN()` | Before each kernel call | Clears `/proc/self/clear_refs`, starts PAPI counters |
| `WSS_END("name")` | After each kernel call | Stops counters, reads `/proc/self/smaps`, prints report line |

When built without `-DPROFILE_WSS`, all macros are empty — zero overhead, no compilation changes needed.

### Step-by-step

1. **Copy `wss_profiler.h`** into the user's source directory (or wherever their Makefile can find it with `-I.`).

2. **Edit the file containing `main()`** (or wherever `MPI_Init` is called):
   ```c
   #include "wss_profiler.h"

   // ... existing code ...
   MPI_Init(&argc, &argv);
   WSS_INIT();   // add this line
   ```

3. **Wrap each target kernel** at its call site (not inside the function definition):
   ```c
   WSS_BEGIN();
   stencil_apply(grid, nx, ny, nz);
   WSS_END("stencil_apply");
   ```

   If the kernel is called in a loop, decide whether to measure one iteration or the whole loop:
   - One iteration: macros go inside the loop (ask the user which iteration).
   - Whole loop: `WSS_BEGIN()` before the loop, `WSS_END()` after.

### Fortran codes

For Fortran codes, use the pre-compiled Fortran bindings. The container
ships a static library (`libwss_profiler.a`) and Fortran module file
(`wss_profiler_mod.mod`) at `/usr/local/lib/` and `/usr/local/include/`.

1. **Add `use wss_profiler_mod`** and instrument the Fortran code:
   ```fortran
   use wss_profiler_mod
   ! ... after MPI_Init:
   call wss_init()
   ! ... before/after each kernel:
   call wss_begin()
   call some_kernel(...)
   call wss_end_named("some_kernel")
   ```

2. **Link with the pre-compiled library** by adding to the final link step:
   ```
   -lwss_profiler -lpapi
   ```
   The library and module are in standard system paths (`/usr/local/lib`
   and `/usr/local/include`) so no `-L` or `-I` flags are needed.

   Adapt to the project's build system. For example with a Makefile:
   ```
   LDFLAGS += -lwss_profiler -lpapi
   ```

4. **Rebuild** with PAPI linked:
   ```
   make EXTRA_CFLAGS="-DPROFILE_WSS" EXTRA_LDFLAGS="-lpapi"
   ```
   `wss_profiler.h` is installed at `/usr/local/include` in the container — no `-I` flag needed. Adapt the make invocation to the user's actual build system (CMake, manual gcc invocation, etc.).

5. **Run normally** (same command as always):
   ```bash
   mpirun -np <N> ./solver test/small.cfg
   ```

6. **Capture stderr from rank 0**. The report lines go to stderr. Redirect or tee to capture:
   ```bash
   mpirun -np <N> ./solver test/small.cfg 2>&1 | grep '\[WSS\]'
   ```

7. **Report results** to the user (see format below).

### Output format

```
[WSS] Profiling active on rank 0
[WSS] stencil_apply                   512.0 MB hot     0.480 GFLOP     0.98 FLOP/byte
[WSS] fft_forward                     128.0 MB hot     0.190 GFLOP     1.57 FLOP/byte
```

### How to present results

Present a table combining Phase 1 timing with Phase 2 measurements:

```
| Kernel           | % Time | Hot MB | GFLOP | FLOP/byte | Assessment   |
|------------------|--------|--------|-------|-----------|--------------|
| stencil_apply    |  42.3% |    512 |  0.48 |      0.98 | memory-bound |
| fft_forward      |  28.1% |    128 |  0.19 |      1.57 | borderline   |
```

**FLOP/byte interpretation:**
- < 1: kernel sweeps data with little reuse — almost certainly memory-bandwidth-bound
- 1–5: borderline — depends on hardware balance
- > 10: compute-heavy relative to working set — likely compute-bound

**Always state the caveats:**
- Hot bytes are at 4 KB page granularity (rounded up). Small working sets have significant rounding error.
- **PAPI only counts the main thread. OpenMP worker thread FLOPs are NOT counted.** For codes that use OpenMP parallelism inside kernels, the FLOP count will be severely undercounted. The hot-byte measurement (smaps) IS process-wide and includes all threads. When profiling OpenMP codes, rely on hot bytes for GPU memory planning and treat FLOP counts as lower bounds only.
- FLOP/byte here is per byte of *working set*, not per byte of *bandwidth*. It is not arithmetic intensity in the roofline sense.
- smaps includes stack, code, and library pages (~a few MB of noise). For large working sets this is negligible.

### Cleanup

After profiling, **revert the instrumentation**:
- Remove `#include "wss_profiler.h"` and `WSS_INIT()` from main.
- Remove all `WSS_BEGIN()` / `WSS_END()` calls.
- Remove `wss_profiler.h` from the source directory.
- Rebuild clean.

Alternatively, leave the macros in place guarded by `-DPROFILE_WSS` — without that flag they compile away to nothing.

---

## GPU memory planning

Once per-kernel hot sets are measured, answer GPU memory planning questions as follows.

### Key insight

**GPU memory planning is not about total allocation. It is about the maximum hot working set across all kernels, plus what needs to stay resident between them.**

Total allocation is the worst-case upper bound. `max(hot set)` is the realistic lower bound for device memory required by any single kernel.

### "Will it fit on GPU X?"

```
max_hot_mb = max(hot MB across all profiled kernels)
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

### "What's the transfer plan?"

Reason about execution order. For a time-stepping loop:

```
for each timestep:
    stencil_apply()      # hot: 512 MB
    fft_forward()        # hot: 128 MB
    boundary_exchange()  # hot: 16 MB
```

- **Data resident across transitions**: if the same arrays are hot in consecutive kernels, they should stay on device. If arrays are not shared, they can be evicted.
- **Transfer cost of transitioning kernel A → kernel B**: `hot(B) - overlap(A, B)`, not `hot(B)`.
- **Minimum resident set**: union of all arrays hot in *any* kernel that must remain on device (e.g. arrays hot in both the kernel before and after another). If nothing is shared across all kernels, the theoretical minimum is 0 (everything could swap). In practice, shared arrays are the constraint.

### "What's the swap cost per timestep?"

```
swap_per_step ≤ sum(hot MB for all kernels in one timestep)   [worst case: no reuse]
swap_per_step ≥ max(hot MB)                                   [best case: everything fits]
```

With known execution order and device memory budget, compute the exact number:
- For each kernel transition, the swap-in cost is `hot(next kernel) - (bytes already resident)`.
- Sum over all transitions in one timestep.

### Important framing rules

- Never say "total allocation is X GB, so you need X GB of GPU memory." Always clarify that hot set < total allocation.
- When you don't have array-level attribution (only total hot MB per kernel), you can only bound the overlap — you cannot compute it exactly. Say so.
- If the user asks about a GPU before Phase 2 is done, explain that you need hot set measurements first, and offer to run Phase 2.

---

## Generating a code skill for the target project

When the user asks you to generate a skill file for their project (e.g.
"make a skill file for the code in /workspace"), follow this procedure:

1. **Explore the source tree**: `ls`, `find`, look at file extensions to
   determine the language (C, C++, Fortran, mixed).
2. **Find the build system**: look for `Makefile`, `CMakeLists.txt`,
   `configure`, or build scripts. Read them to understand how compilation
   and linking works.
3. **Identify the entry point**: find `main()` (C/C++) or `program` (Fortran).
   Trace the call graph to understand the kernel structure.
4. **Try building and running**: use the build system you found. Note the
   exact commands that work.
5. **Determine build extensibility**: figure out how to add a new library
   or include path. For Makefiles, check if there are variables like
   `CFLAGS`, `LDFLAGS`, `OPTIONS` that accept appended values. For CMake,
   check for `target_link_libraries` or similar.
6. **Write the SKILL.md** following the template at
   `/skills/code-template/SKILL.md` (also available as `/my-code` if
   mounted). Fill in every section:
   - What the code is (language, domain, parallelism model)
   - Source layout (key files and what they contain)
   - Build command
   - Extending the build (how to add headers/libraries/flags — critical)
   - Run command
   - Expected output / correctness check
   - Notes for the profiler (language, parallelism, timestep structure)
7. **Save the skill** to `/skills/my-code/SKILL.md` so it's available as
   a slash command.

**Important**: the generated skill must NOT reference this profiler, WSS
macros, PAPI, or any instrumentation. It describes only the user's code.

---

## What this skill does NOT know

- How to build or run the user's code — that is in the user's code skill.
- What the user's kernels do or what performance is expected.
- Array-level attribution of hot pages (planned future work).
- PAPI counter availability on specific microarchitectures — run `papi_avail` inside the container to check.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `cannot open /proc/self/clear_refs` | Missing privilege | Run container with `--privileged` |
| `PAPI library init failed` | PAPI not installed or no hardware counter access | Install `libpapi-dev`; may need `--privileged` |
| `No FP PAPI events available` | Microarchitecture doesn't expose standard events | Run `papi_avail` and use native event names (not yet supported in header) |
| perf says `Permission denied` | `perf_event_paranoid` too restrictive | Fall back to MPI_Wtime() instrumentation for hotspot discovery. For FLOP counts, report as 0 — hot-byte measurement still works. For the full workflow, ask sysadmin to run `sysctl kernel.perf_event_paranoid=-1` on the host. |
| FLOP count is 0 | PAPI events not added, or hardware counters not exposed by the host (VM/container) | Check PAPI init messages in stderr. Hot-byte measurement and GPU memory planning still work without FLOP counts — report the hot MB and note that FLOP/byte is unavailable. For the full workflow, `perf_event_paranoid` must be ≤ 1. |
| Hot MB seems too high | smaps noise (stack, libs) | For large kernels (>50 MB), noise is <5%. For small kernels, interpret with caution. |
| Hot MB is the same across kernels | clear_refs not working | Verify `--privileged`; check for `[WSS] cannot open` error |
