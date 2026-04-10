# wss-profiler

A Claude Code skill that profiles C/C++ MPI/OpenMP HPC codes to answer two questions:

1. **Where is time going?** — sampling via `perf`
2. **For a given kernel: how many unique bytes are hot, and how many FLOPs does it execute?** — instrumentation via `/proc/clear_refs` + PAPI

The payoff is **GPU memory planning**: per-kernel hot set data lets you determine whether a code fits on a target GPU, what stays resident between kernels, and what must be explicitly swapped.

---

## How it works

Two Claude Code skills work together:

```
/skills/
  wss-profiler/   ← this repo, baked into the container image
    SKILL.md          teaches Claude how to profile (perf, WSS, PAPI, GPU planning)
    wss_profiler.h    C header Claude copies into the user's source tree
  my-code/        ← written by the user, mounted at runtime
    SKILL.md          teaches Claude how to build and run the specific code
```

Claude reads both skills and synthesizes. The profiler skill knows nothing about the user's code; the code skill knows nothing about profiling. Claude is the glue.

---

## Skills

### `wss-profiler` (this repo)

Covers:

- **Phase 1 — Discovery**: `perf record`/`perf report` to rank functions by % of wall-clock time. No recompile needed.
- **Phase 2 — Instrumentation**: `WSS_BEGIN()` / `WSS_END()` macros around individual kernel calls. Measures:
  - **Hot bytes**: unique 4 KB pages touched, via `/proc/self/clear_refs` + `/proc/self/smaps`
  - **FLOPs**: `PAPI_DP_OPS` + `PAPI_SP_OPS` (falls back to `PAPI_FP_OPS` on unsupported microarchitectures)
  - **FLOP/byte**: ratio of the two — a proxy for memory vs. compute pressure
- **GPU memory planning**: interprets per-kernel hot sets to answer "will it fit?", "what stays resident?", and "what's the swap cost per timestep?"

### `my-code` (written by you)

A short SKILL.md that tells Claude:

- Source layout (which files contain the hot kernels)
- Build command (and where to inject extra flags)
- Run command and test case
- Any domain-specific context

A template is at [`skills/code-template/SKILL.md`](skills/code-template/SKILL.md). Copy it, fill it in, and mount the directory at `/skills/my-code` when running the container.

---

## Quickstart

### 1. Build the image

```bash
git clone <this-repo> wss-profiler
cd wss-profiler
docker build -t wss-profiler:latest .
```

### 2. Write your code skill

```bash
cp -r skills/code-template /path/to/your/code-skill
$EDITOR /path/to/your/code-skill/SKILL.md   # fill in the template
```

### 3. Run the container

```bash
docker run --privileged \
  -v /path/to/your/code:/workspace \
  -v /path/to/your/code-skill:/skills/my-code \
  -it wss-profiler:latest bash
```

> **Host requirement**: if `perf` returns "Permission denied", run on the host:
> ```bash
> sudo sysctl kernel.perf_event_paranoid=-1
> ```

### 4. Start Claude Code inside the container

```bash
claude
```

Then ask:

- *"Find the hotspots in my code."* → Claude reads both skills, runs perf, reports top functions.
- *"Measure the working set of stencil_apply."* → Claude copies the header, adds macros, rebuilds, runs, reports hot MB + FLOP/byte.
- *"Will this fit on an A100?"* → Claude uses the measured hot sets to reason about GPU memory.

---

## The header: `wss_profiler.h`

```c
#include "wss_profiler.h"   // header lives in /skills/wss-profiler/ inside container

MPI_Init(&argc, &argv);
WSS_INIT();                 // once, after MPI_Init

// ... later, around a kernel call:
WSS_BEGIN();
stencil_apply(grid, nx, ny, nz);
WSS_END("stencil_apply");
```

Build flags when profiling:
```
-DPROFILE_WSS -I/skills/wss-profiler -lpapi
```

Without `-DPROFILE_WSS`, all macros compile away to nothing — no overhead, no PAPI dependency.

Output (to stderr, rank 0 only):
```
[WSS] Profiling active on rank 0
[WSS] stencil_apply                   512.0 MB hot     0.480 GFLOP     0.98 FLOP/byte
[WSS] fft_forward                     128.0 MB hot     0.190 GFLOP     1.57 FLOP/byte
```

---

## FLOP/byte interpretation

| FLOP/byte | Meaning |
|-----------|---------|
| < 1 | Sweeps data with little reuse — memory-bandwidth-bound |
| 1–5 | Borderline — depends on hardware balance |
| > 10 | Compute-heavy relative to working set — compute-bound |

Note: this is FLOP per byte of *working set*, not per byte of *bandwidth*. It is not arithmetic intensity in the roofline sense.

---

## GPU memory planning

Once per-kernel hot sets are measured:

- **Will it fit?** `max(hot set across all kernels)` is the realistic lower bound for device memory required. Total allocation is the worst-case upper bound and is almost always too conservative.
- **What stays resident?** Data hot in consecutive kernels should stay on device. Data cold between kernels can be evicted.
- **Swap cost per timestep?** Bounded by `sum(hot MB)` (no reuse) above and `max(hot MB)` (everything fits) below. With execution order known, Claude can compute the exact number.

See [`plan.md`](plan.md) for the full methodology and reasoning.

---

## Container layout

```
/skills/
  wss-profiler/
    SKILL.md          ← baked in; Claude reads this
    wss_profiler.h    ← baked in; Claude copies this into the user's source tree
  my-code/            ← mounted at runtime
    SKILL.md          ← user-written; Claude reads this

/workspace/           ← user's code, mounted at runtime
```

---

## Caveats and limitations

- **Hot bytes are 4 KB page granular.** If a kernel touches 1 byte on a page, it counts as 4096 bytes. For large working sets (MB+) this rounding is negligible; for small working sets interpret with caution.
- **FLOP count is main-thread only.** PAPI counters are per-thread. OpenMP worker threads are not counted in the current implementation.
- **smaps includes stack, code, and library pages** — a few MB of noise. Negligible for large kernels.
- **Phase 1 is rank 0 only.** For load-imbalanced codes, the hottest rank may not be rank 0.
- **PAPI counter availability varies by CPU.** Run `papi_avail` inside the container to check what's exposed on your hardware.

---

## Example

[`example/bench.c`](example/bench.c) is a synthetic MPI+OpenMP benchmark with two contrasting kernels:

```bash
cd example
make profile          # builds with -DPROFILE_WSS -lpapi
mpirun -np 2 ./bench
```

Expected output pattern:
```
[WSS] stream_kernel       ~768.0 MB hot    ~0.000 GFLOP    ~0.00 FLOP/byte
[WSS] compute_kernel        ~2.0 MB hot    ~0.256 GFLOP  ~128.00 FLOP/byte
```
