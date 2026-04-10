# WSS Profiler: Plan

## What this is

A skill for Claude Code that profiles C/C++ MPI/OpenMP HPC codes to answer two questions:

1. **Where is time going?** (Phase 1 — discovery via `perf`)
2. **For a given kernel, how many unique bytes are hot, and how many FLOPs does it execute?** (Phase 2 — instrumentation via `/proc/clear_refs` + PAPI)

The result is a per-kernel report: hot MB, GFLOP, FLOP/byte ratio, and a plain-English assessment.

---

## Why this matters: the GPU porting problem

The motivating use case is GPU memory planning. When porting an HPC code to GPU, the first question is: "Will my data fit in device memory?" The naive answer — total allocation on the CPU side — is almost always too conservative, because no single kernel touches all of it.

Consider a code that allocates 12 GB across several arrays. Kernel A touches arrays X and Y (4 GB). Kernel B touches arrays Y and Z (3 GB). The total allocation is 12 GB, but the largest hot set of any single kernel is 4 GB. That fits on an 8 GB GPU with room to spare.

**The key insight:** GPU memory planning is not about total allocation. It is about the maximum hot working set across all kernels that must execute on the device, plus whatever needs to remain resident between kernel launches.

With per-kernel hot set data, the user (or Claude) can reason about:

- **Will it fit?** If max(hot set) < device memory, the entire working set of the heaviest kernel fits. No swapping needed.
- **What to keep resident:** Data that is hot in consecutive kernels (e.g., array Y is hot in both A and B above) should stay on the device. Data that is cold between kernels can be evicted or never transferred.
- **Swap budget:** If max(hot set) > device memory, the user needs to tile or swap. The hot set tells them exactly how much: `max(hot set) - device memory` is the minimum that must be streamed in/out per kernel invocation.
- **Overlap potential:** If kernel A's hot set and kernel B's hot set partially overlap (shared arrays), the transfer cost of transitioning from A to B is `hot(B) - (hot(A) ∩ hot(B))`, not `hot(B)`. The per-kernel measurements bound this.

This is why the methodology must live inside the skill — not as passive documentation, but as reasoning context. When the user asks "will this fit on an A100?" or "what's my swap strategy?", the LLM needs to understand what hot bytes mean, why they're smaller than total allocation, and how to compose per-kernel measurements into a memory plan.

---

## Methodology

### Phase 1: Discovery

Sample-based profiling via Linux `perf`. No code modification. `perf record` attaches to rank 0 of the MPI job and collects instruction pointer samples at a configurable frequency. `perf report` attributes samples to functions. The output is a ranked list of functions by % of total samples.

**Why perf and not gprof?**
- No recompile needed. Works with any binary that has symbols (`-g` or not stripped).
- Sampling-based — constant overhead regardless of call frequency.
- Works with MPI and OpenMP out of the box.
- gprof requires `-pg`, which changes codegen and can distort results for short/inlined functions.

**Limitations:**
- Sampling is statistical. Functions that consume <1% of time may not appear.
- Inlined functions are attributed to their caller unless built with `-g`.
- perf sees wall-clock time, not CPU time. If a rank is waiting in MPI, that shows up as MPI library time, not user kernel time. This is useful information but can surprise users.

### Phase 2: Working set + FLOPs

Two independent mechanisms combined:

#### Hot bytes via `/proc/self/clear_refs` + `/proc/self/smaps`

Linux tracks a "Referenced" bit per page (4KB). Writing `1` to `/proc/self/clear_refs` clears all Referenced bits for the process. After the kernel runs, reading `/proc/self/smaps` and summing the `Referenced` fields gives the number of unique pages touched.

**What this measures:** The number of distinct 4KB pages accessed by any load or store during the kernel, on the profiling rank's process.

**What this does NOT measure:**
- Sub-page granularity. If a kernel touches 1 byte on a page, it counts as 4096 bytes.
- Per-thread breakdown. smaps is process-wide. With OpenMP, all threads' accesses are merged.
- Cache-line granularity. Two accesses 64 bytes apart on the same page are one page, not two cache lines.
- Remote memory. On NUMA systems, this counts pages touched, not where they live.

**Overhead:** Near zero. Two `/proc` reads, no runtime instrumentation.

**Permissions:** Writing to `clear_refs` requires `CAP_SYS_RESOURCE` or root. The container runs privileged.

#### FLOPs via PAPI hardware counters

PAPI reads CPU performance counters for floating-point operations. We count `PAPI_DP_OPS` (double-precision) and `PAPI_SP_OPS` (single-precision) across the kernel.

**What this measures:** Retired floating-point operations on the core running the profiled thread.

**What this does NOT measure:**
- OpenMP threads other than the main thread (PAPI counters are per-thread; our current implementation only instruments the main thread).
- Vectorization details. 4 DP ops in one AVX-256 instruction count as 4 ops, which is correct for FLOP count but doesn't tell you about vector utilization.
- Integer or comparison operations.

**Overhead:** Reading two counters — negligible.

**Counter availability:** Varies by CPU microarchitecture. `PAPI_DP_OPS` may not exist on all chips. Fallback is `PAPI_FP_OPS` (all precisions combined) or native event names. The skill should run `papi_avail` and adapt.

#### The combined metric: FLOP/byte

```
FLOP/byte = (PAPI_DP_OPS + PAPI_SP_OPS) / (Referenced_KB * 1024)
```

**Interpretation:**
- This is FLOP per byte of *working set*, not per byte *transferred through cache*. It is NOT arithmetic intensity in the roofline model sense.
- It answers: "For every byte of data this kernel cares about, how much compute does it do?"
- Low FLOP/byte (< 1): kernel sweeps data with little reuse. Likely memory-bandwidth-bound.
- High FLOP/byte (> 10): kernel does a lot of work per byte of data it touches. Likely compute-bound.
- This metric complements roofline. Roofline tells you if you're hitting the bandwidth ceiling. FLOP/byte-of-WSS tells you how much data your kernel actually needs — which is the starting point for deciding whether to block, tile, or compress.

**The user should understand:** this is a ratio of two independently measured quantities at different granularities (exact FLOP count vs. 4KB-granular byte count). For kernels with small working sets (a few pages), the byte count has significant rounding. For large working sets (MB+), the 4KB granularity error is negligible.

---

## The container

The container is the entire controlled environment. It provides:

- Ubuntu 24.04 base
- gcc / g++ with OpenMP support
- OpenMPI (mpicc, mpirun)
- PAPI (libpapi-dev, papi-tools)
- perf (linux-tools-generic)
- Any other dependencies the user's code needs (specified in Dockerfile or installed at build time)

The user's code and their code skill are mounted in at runtime. The profiler skill's assets (header, scripts) are also mounted or copied in.

```
Container
├── /usr/bin/mpicc, mpirun, perf, papi_avail   (from image)
├── /workspace/                                  (user's code, mounted)
│   ├── src/
│   ├── Makefile
│   ├── test/
│   └── wss_profiler.h                          (copied in by Claude)
└── /skills/                                     (skills, mounted)
    ├── wss-profiler/SKILL.md
    └── my-code/SKILL.md
```

**Runtime flags:**
- `--privileged` (or `--cap-add SYS_ADMIN SYS_PTRACE`, `--security-opt seccomp=unconfined`) for clear_refs and perf_event_open.
- `--pid=host` if perf needs host PID namespace (usually not needed when profiling inside the container).
- Host may need `sysctl kernel.perf_event_paranoid=-1` if perf counters are restricted.

The Dockerfile should be extended by the user if their code has additional dependencies (FFTW, HDF5, etc.). The profiler skill's Dockerfile is a base that they build on top of.

---

## Skills

### Skill 1: `wss-profiler` (this project)

Knows:
- How to run perf record/report and parse the output.
- How to instrument code with WSS_BEGIN/WSS_END macros.
- How to interpret the results.
- The container requirements and permissions.
- The caveats and limitations.

Does NOT know:
- How to build or run the user's code.
- The user's source layout, compiler flags, or run command.
- What the user's kernels do or what performance is expected.

### Skill 2: User's code skill (written by the user)

Knows:
- Source layout (which files, which functions).
- Build command and how to inject extra flags.
- Run command and test cases.
- Any domain-specific context (grid sizes, expected behavior).

Does NOT know:
- Anything about profiling.

### Claude is the glue

Claude reads both skills and synthesizes. It never needs both skills to reference each other. The interaction is:

1. User says "find hotspots" → Claude reads both skills.
2. From the code skill: how to build and run.
3. From the profiler skill: wrap the run with perf, parse the output.
4. User says "measure hot memory of function X" → Claude reads the profiler skill.
5. From the profiler skill: copy header, wrap function X, add compiler/linker flags.
6. From the code skill: the actual build/run command with those flags injected.

---

## Walkthrough example

### Setup

The user has a CFD solver. Their code skill says:

```
Source: src/main.c, src/stencil.c, src/fft.c, src/boundary.c
Build:  make EXTRA_CFLAGS="..." EXTRA_LDFLAGS="..."
Run:    mpirun -np 4 ./solver test/small.cfg
```

They start Claude Code in the profiler container with their code mounted.

### "Find me the hotspots"

Claude:
1. Reads the code skill → `make && mpirun -np 4 ./solver test/small.cfg`
2. Reads the profiler skill → Phase 1: perf on rank 0.
3. Builds normally: `make`
4. Runs:
```bash
mpirun -np 4 bash -c '
  if [ $OMPI_COMM_WORLD_RANK -eq 0 ]; then
    perf record -g -o perf.data -- ./solver test/small.cfg
  else
    ./solver test/small.cfg
  fi'
```
5. Parses: `perf report -n --stdio --no-children -i perf.data`
6. Reports to user:

```
Top functions by time (rank 0):

  42.3%   stencil_apply     (src/stencil.c)
  28.1%   fft_forward       (src/fft.c)
  11.0%   boundary_exchange (src/boundary.c)
   6.2%   MPI_Allreduce     (library)
   ...

Suggest instrumenting: stencil_apply, fft_forward, boundary_exchange.
Which ones should I dig into?
```

### "Do stencil_apply"

Claude:
1. Reads the profiler skill → Phase 2 recipe.
2. Copies `wss_profiler.h` into `src/`.
3. Edits `src/main.c`:
   - Adds `#include "wss_profiler.h"` at the top.
   - Adds `WSS_INIT();` after `MPI_Init()`.
   - Finds the call to `stencil_apply()` and wraps it:
     ```c
     WSS_BEGIN();
     stencil_apply(grid, nx, ny, nz);
     WSS_END("stencil_apply");
     ```
4. Rebuilds: `make EXTRA_CFLAGS="-DPROFILE_WSS -I." EXTRA_LDFLAGS="-lpapi"`
5. Runs: `mpirun -np 4 ./solver test/small.cfg`
6. Captures stderr from rank 0:
   ```
   [WSS] Profiling active on rank 0
   [WSS] stencil_apply                  512.0 MB hot       0.480 GFLOP       0.98 FLOP/byte
   ```
7. Reports to user:

```
stencil_apply on rank 0 (test/small.cfg):

  Hot working set:    512 MB
  Total allocated:   ~768 MB (estimated from smaps total)
  FLOPs:              0.48 GFLOP
  FLOP/byte (WSS):    0.98

Assessment: stencil_apply touches 67% of the total allocation. At ~1 FLOP
per byte of working set, this kernel does very little compute per byte it
cares about. It is almost certainly memory-bandwidth-bound.

Caveats:
- Hot bytes measured at 4KB page granularity. True byte count may be lower.
- FLOP count is main thread only. OpenMP threads are not counted by PAPI
  in this configuration.
- FLOP/byte here is per byte of working set, not per byte transferred.
  A roofline analysis (FLOP per byte of bandwidth) would give a different
  number.

Want me to instrument fft_forward next?
```

### "Do fft_forward too"

Claude adds a second WSS_BEGIN/WSS_END block, rebuilds, reruns, and reports both:

```
| Kernel         | % Time | Hot MB | GFLOP | FLOP/byte | Assessment       |
|----------------|--------|--------|-------|-----------|------------------|
| stencil_apply  |  42.3% |    512 |  0.48 |      0.98 | memory-bound     |
| fft_forward    |  28.1% |    128 |  0.19 |      1.57 | borderline       |
```

### Cleanup

Claude reverts the instrumentation: removes the WSS macros, removes the header, rebuilds clean. Or leaves it guarded behind `-DPROFILE_WSS` so it compiles away to nothing by default.

---

## GPU memory modeling

This is the payoff. Once the user has per-kernel hot set measurements, they (or Claude) can model GPU memory feasibility.

### The data we have

After profiling N kernels, we have a table:

| Kernel | Hot MB | Arrays touched (if known) |
|--------|--------|---------------------------|
| stencil_apply | 512 | grid_old, grid_new, coeffs |
| fft_forward | 128 | fft_buf |
| boundary_exchange | 16 | halo_buf |

### Question 1: "Will it fit on device X?"

```
max_hot = max(hot MB across all kernels)
device_mem = e.g. 80 GB for A100

if max_hot < device_mem:
    "The heaviest kernel's working set fits. Total allocation is [total] GB
     but the most any single kernel needs is [max_hot] GB."
else:
    "The heaviest kernel ([name]) needs [max_hot] GB, which exceeds device
     memory by [max_hot - device_mem] GB. You will need to tile or use
     managed memory / explicit swapping."
```

### Question 2: "What's the transfer plan?"

The execution order of kernels determines what stays resident. For a time-stepping loop like:

```
for each timestep:
    stencil_apply()     # hot: grid_old, grid_new, coeffs (512 MB)
    fft_forward()       # hot: fft_buf (128 MB)
    boundary_exchange() # hot: halo_buf (16 MB)
```

The reasoning is:
- stencil_apply is the peak: 512 MB must be on device.
- fft_forward uses different arrays. If device memory allows 512 + 128 = 640 MB, keep both resident. If not, grid_old/grid_new can be evicted before fft_forward and reloaded before the next stencil_apply.
- boundary_exchange is small. Its 16 MB can share device memory with anything.
- The minimum resident set across the whole loop is the *intersection* of all hot sets. If no array is hot in every kernel, the minimum is zero (everything could theoretically be swapped). In practice, the array hot in consecutive kernels determines the swap cost.

### Question 3: "What's the swap cost per timestep?"

If we must swap, the cost per timestep is:

```
swap_per_step = sum over kernel transitions of (new_hot - already_resident)
```

This is bounded above by `sum(hot MB for all kernels)` (everything swapped every time) and below by `max(hot MB)` (everything fits, no swaps). The per-kernel hot data lets us compute the exact number given the execution order and device memory budget.

### What the skill needs to enable this

The methodology section of the skill must explain:
- Hot bytes = unique bytes touched, not total allocation.
- Per-kernel hot sets can overlap (shared arrays).
- The LLM should reason about kernel execution order, not just individual kernels.
- Total allocation is the worst-case upper bound. max(hot set) is the realistic lower bound for device memory needed.
- When the user asks "will this fit on GPU", the answer is never just "total allocation is X GB". It is always "the heaviest kernel needs Y GB, and here's the transfer plan."

This reasoning is why the methodology is embedded in the skill, not in a separate doc. The LLM needs it in context to answer GPU porting questions correctly.

---

## Open questions and future work

1. **OpenMP PAPI**: the current header only counts FLOPs on the main thread. Supporting per-thread PAPI requires `PAPI_thread_init()` and counting inside parallel regions. This is doable but adds complexity to the header.

2. **Filtering smaps by address range**: right now the Referenced count includes stack, code, and library pages (a few MB of noise). We could parse smaps entries and only count pages in the heap/mmap range. Worth doing if the user's working set is small enough that a few MB of noise matters.

3. **Multiple ranks**: profiling only rank 0 assumes symmetric workload. For load-imbalanced codes, the user may want to profile the busiest rank. We could add a flag to discover which rank is slowest (via MPI timers) and then target that rank.

4. **Cache-line granularity**: the `/proc` approach is 4KB. For finer granularity (64B cache lines), we'd need `perf mem` sampling or binary instrumentation (Pin/DynamoRIO), both of which are much slower. This could be a Phase 3 for users who need it.

5. **Automated roofline comparison**: with hot bytes and FLOPs in hand, plus a STREAM-style bandwidth measurement, we could automatically place each kernel on a roofline chart. The container could include a STREAM benchmark for this.

6. **Repeated kernel calls**: if a kernel is called 10,000 times in a loop, the user needs to decide: instrument one call (instantaneous working set) or all calls (cumulative). The skill should ask. For the cumulative case, WSS_BEGIN goes before the loop and WSS_END after.

7. **Per-array attribution**: right now we measure total hot pages, not which arrays they belong to. For GPU memory planning, knowing that stencil_apply's 512 MB is split into grid_old (256 MB) + grid_new (256 MB) enables finer swap decisions. We could parse smaps address ranges against malloc return values to attribute pages to specific allocations. This is the highest-value future addition.

8. **Kernel overlap analysis**: automatically detecting which arrays are shared between kernels (hot in both A and B) would let Claude compute transfer plans without the user manually listing array-to-kernel mappings. Requires per-array attribution (item 7).

9. **GPU memory simulator**: given per-kernel hot sets and a device memory budget, enumerate swap strategies and estimate PCIe transfer costs. This could be a Phase 3 analysis that runs entirely in the LLM using the Phase 2 data — no additional profiling, just arithmetic.
