# AGENTS.md

## What this repository is

A deliverable template for an HPC performance-modeling engagement. It ships a Singularity/Apptainer container that bundles:

- **Profiling tools** (`perf`, PAPI, the `wss_profiler.h` C header)
- **Claude Code** (pre-configured with skills as slash commands)
- **Skill files** — the key abstraction: markdown documents that teach Claude how to do a task or how to work with a specific codebase

The end-user clones this repo, builds the SIF, mounts their own code + a filled-in code skill, and then types `claude` inside the container. Claude reads both skills and runs a two-phase profiling workflow to answer: *"Where is time going?"* and *"How much GPU memory does each hot kernel actually need?"*

---

## Repository layout

```
hotmemory.def               Singularity/Apptainer build definition (primary delivery)
wss_profiler.h              C header baked into the image; users copy this into their src

skills/
  wss-profiler/SKILL.md     Profiler skill — baked into the image and exposed as /wss-profiler slash command
  code-template/SKILL.md    Blank template users copy to write their code skill

example/
  bench.c                   Synthetic MPI+OpenMP benchmark (two deliberately contrasting kernels)
  Makefile                  Builds bench; `make profile` enables WSS macros
  my-code/SKILL.md          Fully filled-in code skill for the example (the reference for what a good skill looks like)

README.md                   User-facing quickstart
AGENTS.md                   Developer reference for agents and contributors
```

---

## The skills system — the central abstraction

Skills are markdown files placed in `~/.claude/commands/` so Claude Code can invoke them as slash commands (e.g. `/wss-profiler`, `/my-code`). The container's `%runscript` / `entrypoint.sh` copies them there automatically.

**Two skills are always in play:**

1. **`/wss-profiler`** (`skills/wss-profiler/SKILL.md`) — knows *how* to profile; does NOT know the user's code
2. **`/my-code`** (user-supplied, mounted at `/skills/my-code/SKILL.md`) — knows *the user's code*; knows nothing about profiling

Claude is the glue. It reads both and synthesises. Neither skill references the other.

**The code-template skill** (`skills/code-template/SKILL.md`) is the blank form users fill in. Study `example/my-code/SKILL.md` for a fully correct example — it is the canonical reference for what Claude expects to find.

---

## Container build and test

**Build (Linux amd64 or aarch64 — must build on the target machine):**
```bash
singularity build --fakeroot hotmemory.sif hotmemory.def
# or with apptainer:
apptainer build --fakeroot hotmemory.sif hotmemory.def
# omit --fakeroot if you have root
```

**Test with the built-in example:**
```bash
export SINGULARITYENV_ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY

singularity run --fakeroot \
  --bind "$(pwd)/example":/workspace \
  --bind "$(pwd)/example/my-code":/skills/my-code \
  ./hotmemory.sif bash
```

Inside the container, verify the toolchain:
```bash
claude --version
perf stat echo ok
cd /workspace && make profile && mpirun --allow-run-as-root -np 2 ./bench 2>&1 | grep '\[WSS\]'
```

Expected WSS output (sanity check values):
- `stream_kernel` → ~768 MB hot, near-zero FLOP/byte
- `compute_kernel` → ~2 MB hot, ~128 FLOP/byte

**Publishing a release:**
```bash
gh release create v1.0.0 --title "v1.0.0" --notes "..."
```
CI (`singularity.yml`) builds and attaches `hotmemory.sif` automatically. On ordinary pushes to `main`, the SIF is only a 90-day workflow artifact, not a release download.

---

## Platform constraints — critical

- **Linux only (amd64 or aarch64).** `perf` and PAPI hardware counters require a real Linux kernel. Docker Desktop for Mac runs in a VM that does not expose CPU performance counters. All testing must happen on a Linux host or HPC node.
- **Build the SIF on the target architecture.** The CI workflow produces an amd64 SIF. For ARM (e.g. NVIDIA Grace / Neoverse V2), build directly on the Grace node: `singularity build --fakeroot hotmemory.sif hotmemory.def`.
- **PAPI on ARM (Neoverse V2).** Run `papi_avail | grep -E 'PAPI_DP_OPS|PAPI_SP_OPS|PAPI_FP_OPS'` inside the container after building. If the standard presets are unavailable the header falls back gracefully (FLOPs reported as 0 with a message). See the PAPI fallback chain note in the header section below.
- **Privileged runtime required.** Writing to `/proc/self/clear_refs` (WSS measurement) needs `CAP_SYS_RESOURCE`; `perf_event_open` may need `kernel.perf_event_paranoid=-1` set on the host. Use `--fakeroot` with Singularity.
- The `bench.c` clangd diagnostics (MPI not found) are expected on Mac/without MPI headers — ignore them; the code is correct and builds inside the container.

---

## The wss_profiler.h header

Three macros, all compile to nothing without `-DPROFILE_WSS`:

| Macro | Placement | Effect |
|---|---|---|
| `WSS_INIT()` | Once, after `MPI_Init()` | Rank-detect, PAPI init |
| `WSS_BEGIN()` | Before kernel call | Clear `/proc/self/clear_refs`, start counters |
| `WSS_END("name")` | After kernel call | Stop counters, read smaps, print to stderr |

Build flags for profiling: `-DPROFILE_WSS -lpapi` (wss_profiler.h is at `/usr/local/include` inside the container — no `-I` needed).

The header has a PAPI fallback chain: `PAPI_DP_OPS` + `PAPI_SP_OPS` → `PAPI_FP_OPS` → FLOPs reported as 0 with a message. Always check stderr for `[WSS]` init messages when results look wrong.

---

## Modifying the container

**Adding dependencies for a user's code** — extend the def file, not by editing it:
```singularity
Bootstrap: localimage
From: hotmemory.sif

%post
    apt-get update && apt-get install -y libfftw3-dev libhdf5-dev \
        && rm -rf /var/lib/apt/lists/*
```

**Changing the baked-in skill** — edit `skills/wss-profiler/SKILL.md` then rebuild the SIF. The `%files` section in `hotmemory.def` copies it in.

**The `perf` symlink** — `linux-tools-generic` installs a kernel-version-specific binary under `/usr/lib/linux-tools-*/perf`. The def file resolves this with `ln -sf $(find ... -name perf | head -1) /usr/local/bin/perf`. If perf breaks after a base image update, this is where to look.

---

## Writing / improving skill files

The most common contribution is improving `skills/wss-profiler/SKILL.md`. Key things it must contain for Claude to work correctly:

- The Phase 1 perf command verbatim (including the rank-0 filtering pattern)
- The Phase 2 step-by-step (copy header → edit → build flags → run → grep stderr)
- The FLOP/byte interpretation thresholds (<1 memory-bound, 1–5 borderline, >10 compute-bound)
- The GPU memory planning reasoning (max hot set, not total allocation)
- The caveats table (4 KB granularity, main-thread PAPI only, smaps noise)
- The troubleshooting table

The skill file is the authoritative, actionable description of the methodology. `wss-profiler/SKILL.md` is the source of truth for how profiling works.

---

## Key gotchas

- **Singularity maps `$HOME`, not `/root`.** The `%runscript` exists specifically because Singularity runs as the real user with their `$HOME`, so `/root/.claude/` from the image is inaccessible. The runscript copies settings and skill files into `$HOME/.claude/`. If Claude can't find skills inside the container, this is the first place to debug.
- **`--allow-run-as-root` for mpirun inside containers.** OpenMPI refuses to run as root without this flag. Required in the container (where the user is root). Drop it outside.
- **WSS measures rank 0 only.** The profiler assumes roughly symmetric workload across ranks. For load-imbalanced codes, the measurements may undercount the busiest rank.
- **PAPI only counts the main thread.** OpenMP worker threads are not instrumented. FLOP counts will be low for heavily-threaded kernels.
- **smaps noise floor is a few MB.** For kernels with small working sets (<10 MB), the hot-byte count includes stack, code segment, and library pages. Interpret with caution; for large kernels it's negligible.
- **`wss_profiler.h` is included in `example/bench.c` as `"wss_profiler.h"`** (relative path), but also installed at `/usr/local/include/wss_profiler.h`. The Makefile in the example does not use `-I..`; it relies on the system include path inside the container.
