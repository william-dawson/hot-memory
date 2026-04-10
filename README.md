# hot-memory

A Claude Code skill that profiles C/C++ MPI/OpenMP HPC codes to answer two questions:

1. **Where is time going?** — sampling via `perf`
2. **For a given kernel: how many unique bytes are hot, and how many FLOPs does it execute?** — instrumentation via `/proc/clear_refs` + PAPI

The payoff is **GPU memory planning**: per-kernel hot set data lets you determine whether a code fits on a target GPU, what stays resident between kernels, and what must be explicitly swapped.

---

## Quickstart

### 1. Pull the image

```bash
docker pull williamdawson/hot-memory:latest
```

### 2. Write your code skill

Copy the template and fill it in:

```bash
cp -r skills/code-template my-code-skill
$EDITOR my-code-skill/SKILL.md
```

The template asks for your source layout, build command, run command, and any notes about which functions are the hot kernels.

### 3. Run the container

```bash
docker run --privileged \
  -v /path/to/your/code:/workspace \
  -v /path/to/my-code-skill:/skills/my-code \
  -it williamdawson/hot-memory:latest bash
```

> **Host requirement (if perf returns "Permission denied"):**
> ```bash
> sudo sysctl kernel.perf_event_paranoid=-1
> ```

### 4. Start Claude Code inside the container

```bash
claude
```

Then ask:

- *"Find the hotspots in my code."* → Claude reads both skills, runs `perf`, reports top functions by % wall-clock time.
- *"Measure the working set of stencil_apply."* → Claude copies the header, adds macros, rebuilds, runs, reports hot MB + FLOP/byte.
- *"Will this fit on an A100?"* → Claude uses the measured hot sets to reason about GPU memory.

---

## How it works

Two Claude Code skills work together. Claude reads both and synthesizes — neither skill needs to know about the other.

```
/skills/
  wss-profiler/       ← baked into the image
    SKILL.md              teaches Claude how to profile and interpret results
    wss_profiler.h        C header Claude copies into the user's source tree
  my-code/            ← mounted by you at runtime
    SKILL.md              teaches Claude how to build and run your specific code
```

### `wss-profiler` skill

- **Phase 1 — Discovery**: `perf record`/`perf report` to rank functions by % of wall-clock time. No recompile needed.
- **Phase 2 — Instrumentation**: `WSS_BEGIN()` / `WSS_END()` macros around individual kernel calls. Measures:
  - **Hot bytes**: unique 4 KB pages touched, via `/proc/self/clear_refs` + `/proc/self/smaps`
  - **FLOPs**: `PAPI_DP_OPS` + `PAPI_SP_OPS` (falls back to `PAPI_FP_OPS` on unsupported microarchitectures)
  - **FLOP/byte**: a proxy for memory vs. compute pressure on this kernel's working set
- **GPU memory planning**: interprets per-kernel hot sets to answer "will it fit?", "what stays resident?", and "what's the swap cost per timestep?"

### `my-code` skill

A short `SKILL.md` you write once per codebase. Template at [`skills/code-template/SKILL.md`](skills/code-template/SKILL.md).

---

## The header

`wss_profiler.h` is baked into the image at `/skills/wss-profiler/wss_profiler.h`. Claude copies it into your source tree when needed.

```c
#include "wss_profiler.h"

MPI_Init(&argc, &argv);
WSS_INIT();                 // once, after MPI_Init

WSS_BEGIN();
stencil_apply(grid, nx, ny, nz);
WSS_END("stencil_apply");
```

Build flags when profiling:
```
-DPROFILE_WSS -I/skills/wss-profiler -lpapi
```

Without `-DPROFILE_WSS`, all macros compile away to nothing.

Output (stderr, rank 0 only):
```
[WSS] Profiling active on rank 0
[WSS] stencil_apply                   512.0 MB hot     0.480 GFLOP     0.98 FLOP/byte
```

---

## FLOP/byte interpretation

| FLOP/byte | Meaning |
|-----------|---------|
| < 1 | Sweeps data with little reuse — memory-bandwidth-bound |
| 1–5 | Borderline — depends on hardware balance |
| > 10 | Compute-heavy relative to working set — compute-bound |

This is FLOP per byte of *working set*, not per byte of *bandwidth transferred*. It is not arithmetic intensity in the roofline sense — see [`plan.md`](plan.md) for the distinction.

---

## GPU memory planning

Once per-kernel hot sets are measured, Claude can answer:

- **Will it fit?** `max(hot set)` is the realistic lower bound for device memory needed. Total allocation is the worst-case upper bound and is almost always too conservative.
- **What stays resident?** Arrays hot in consecutive kernels should stay on device. Arrays cold between kernels can be evicted.
- **Swap cost per timestep?** Bounded by `sum(hot MB)` (worst case) and `max(hot MB)` (best case). Given execution order, Claude computes the exact number.

---

## Caveats

- **4 KB page granularity.** Hot bytes are rounded up to the page. Small working sets carry significant rounding error; large ones (MB+) do not.
- **Main thread FLOPs only.** PAPI counters are per-thread; OpenMP worker threads are not counted.
- **smaps noise.** Stack, code, and library pages add a few MB to every measurement — negligible for large kernels.
- **Rank 0 only.** For load-imbalanced codes the hottest rank may not be rank 0.
- **PAPI availability varies.** Run `papi_avail` inside the container to see what your CPU exposes.

---

## Example

[`example/bench.c`](example/bench.c) is a synthetic MPI+OpenMP benchmark with two contrasting kernels — one memory-bound, one compute-bound — that exercises the full profiling flow.
