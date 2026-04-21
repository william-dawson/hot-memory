# AGENTS.md

## What this repository is

A deliverable template for an HPC performance-modeling engagement. It ships a Singularity/Apptainer container that bundles:

- **Profiling tools** (`perf`, PAPI, the `wss_profiler` C/Fortran library)
- **Claude Code** (pre-configured with skills as slash commands)
- **Skill files** — the key abstraction: markdown documents that teach Claude how to do a task or how to work with a specific codebase

The end-user clones this repo, builds the SIF, mounts their own code + a filled-in code skill, and then types `claude` inside the container. Claude reads both skills and runs a two-phase profiling workflow to answer: *"Where is time going?"* and *"How much GPU memory does each hot kernel actually need?"*

---

## Repository layout

```
hotmemory.def               Singularity/Apptainer build definition (primary delivery)
hotmemory.sh                Wrapper script for running the container

wss_profiler/
  wss_profiler.h            C header — macros for WSS + FLOP measurement
  wss_profiler_f.c          Fortran-callable C wrappers
  wss_profiler_mod.f90      Fortran module interface

skills/
  wss-profiler/SKILL.md     Profiler skill — baked into the image and exposed as /wss-profiler slash command
  code-template/SKILL.md    Blank template users copy to write their code skill

example/
  bench.c                   Synthetic MPI benchmark (two deliberately contrasting kernels)
  Makefile                  Builds bench; `make profile` enables WSS macros
  my-code/SKILL.md          Fully filled-in code skill for the example

cloverleaf/
  fetch_and_build.sh        Clones and builds CloverLeaf reference version
  my-code/SKILL.md          Code skill for CloverLeaf

README.md                   User-facing quickstart
AGENTS.md                   Developer reference for agents and contributors
```

---

## The skills system — the central abstraction

Skills are markdown files placed in `~/.claude/commands/` so Claude Code can invoke them as slash commands (e.g. `/wss-profiler`, `/my-code`). The container's `/etc/bash.bashrc` copies them there on every interactive bash session.

**Two skills are always in play:**

1. **`/wss-profiler`** (`skills/wss-profiler/SKILL.md`) — knows *how* to profile; does NOT know the user's code
2. **`/my-code`** (user-supplied, mounted at `/skills/my-code/SKILL.md`) — knows *the user's code*; knows nothing about profiling

Claude is the glue. It reads both and synthesises. Neither skill references the other.

**Strict isolation rule:** The code skill must contain ONLY knowledge about the user's code — source layout, build commands, run commands, and domain-specific notes. It must NOT reference the profiler, WSS macros, PAPI, `-DPROFILE_WSS`, or any instrumentation details. Similarly, the profiler skill must NOT reference any specific user code. Claude synthesises the two at runtime. If you find profiler-specific instructions leaking into a code skill (or vice versa), that is a bug — remove them.

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
export SINGULARITYENV_AWS_BEARER_TOKEN_BEDROCK=<your-bearer-token>
export SINGULARITYENV_OPENAI_API_KEY=<your-openai-api-key>

./hotmemory.sh ./example ./example/my-code
```

Inside the container, verify the toolchain:
```bash
claude --version
perf stat echo ok
cd /workspace && make profile && mpirun -np 2 ./bench 2>&1 | grep '\[WSS\]'
```

Expected WSS output (sanity check values with -np 4):
- `stream_kernel` → ~768 MB hot, near-zero FLOP/byte
- `compute_kernel` → ~2 MB hot (FLOP/byte depends on PAPI availability)

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
- **Privileged runtime required.** Writing to `/proc/self/clear_refs` (WSS measurement) needs `CAP_SYS_RESOURCE`; `perf_event_open` needs `kernel.perf_event_paranoid` ≤ 0 set on the host. Use `--fakeroot` with Singularity.
- **Soft degradation.** The workflow works without `perf_event_paranoid` — hot-byte measurement via `/proc/clear_refs` needs only `--fakeroot`. `perf` and PAPI FLOP counts require the sysctl change. Without it, Phase 1 is unavailable and FLOPs report as 0.
- The `bench.c` clangd diagnostics (MPI not found) are expected on Mac/without MPI headers — ignore them; the code is correct and builds inside the container.

---

## The wss_profiler library

Located in `wss_profiler/`. Three files:

| File | Language | Purpose |
|------|----------|---------|
| `wss_profiler.h` | C | Macros: `WSS_INIT()`, `WSS_BEGIN()`, `WSS_END("name")` |
| `wss_profiler_f.c` | C | Fortran-callable wrappers (handles string length, trailing spaces) |
| `wss_profiler_mod.f90` | Fortran | Module: `wss_init()`, `wss_begin()`, `wss_end_named("name")` |

All compile to nothing without `-DPROFILE_WSS`. Build flags for profiling: `-DPROFILE_WSS -lpapi`. All files are installed at `/usr/local/include` inside the container.

The header has a PAPI fallback chain: `PAPI_DP_OPS` + `PAPI_SP_OPS` → `PAPI_FP_OPS` → FLOPs reported as 0 with a message. Always check stderr for `[WSS]` init messages when results look wrong.

**PAPI only counts the main thread.** OpenMP worker thread FLOPs are NOT counted. The hot-byte measurement (smaps) IS process-wide and includes all threads. For OpenMP codes, rely on hot bytes and treat FLOP counts as lower bounds.

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
- The Phase 2 step-by-step for both C and Fortran codes
- The FLOP/byte interpretation thresholds (<1 memory-bound, 1–5 borderline, >10 compute-bound)
- The GPU memory planning reasoning (max hot set, not total allocation)
- The caveats (4 KB granularity, main-thread PAPI only, smaps noise)
- The troubleshooting table

The skill file is the authoritative, actionable description of the methodology. `wss-profiler/SKILL.md` is the source of truth for how profiling works.

---

## Key gotchas

- **WSS measures rank 0 only.** The profiler assumes roughly symmetric workload across ranks. For load-imbalanced codes, the measurements may undercount the busiest rank.
- **PAPI only counts the main thread.** OpenMP worker threads are not instrumented. FLOP counts will be low for heavily-threaded kernels. Hot-byte measurement is process-wide and accurate.
- **smaps noise floor is a few MB.** For kernels with small working sets (<10 MB), the hot-byte count includes stack, code segment, and library pages. Interpret with caution; for large kernels it's negligible.

---

## Singularity sandbox fallback

When FUSE mounting fails (common without `user_allow_other` in `/etc/fuse.conf`), Singularity falls back to extracting the SIF to a temporary sandbox. In this mode, **both `%runscript` and `%environment` are skipped**. All container setup must therefore live in `/etc/bash.bashrc` (baked in via `%post`) so it runs for any interactive bash session. The `--pwd /workspace` flag is used instead of `cd /workspace` in scripts, because it's the only reliable way to set the initial directory across all Singularity execution modes.

---

## OpenMPI inside fakeroot containers

OpenMPI's `hwloc` topology detection sees only 1 slot inside a Singularity `--fakeroot` namespace, even when `nproc` and `/proc/cpuinfo` show the correct number of cores (e.g. 20). This causes `mpirun -np 2` to fail with "not enough slots". The fix is `OMPI_MCA_rmaps_base_oversubscribe=1` set in `/etc/bash.bashrc`, which tells OpenMPI to skip its broken slot detection. This is not true oversubscription — the cores are available, OpenMPI just can't see them through the user namespace.

Additionally, `--fakeroot` makes the user appear as root, so OpenMPI requires `OMPI_ALLOW_RUN_AS_ROOT=1` and `OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1`. Both are set in `/etc/bash.bashrc`.

---

## Claude Code setup in the container

**Installation approach**: Claude Code is installed via `npm install -g @anthropic-ai/claude-code` into `/usr/bin/claude`, not via the official install script. The install script tries to symlink into `$HOME/.local/share/claude/`, which during `--fakeroot` builds resolves to the host user's home on the host filesystem — the binary ends up outside the SIF and is inaccessible at runtime.

**HOME isolation**: `/etc/bash.bashrc` sets `HOME=/tmp/claude-home` and `PATH` to system paths only. This prevents Claude from picking up the host user's broken symlinks, stale `~/.claude/settings.json`, or interfering dotfiles. The bashrc then populates `/tmp/claude-home/.claude/commands/` with the baked-in skills.

**Why not `--no-home`?** Using `--no-home` flag causes Claude Code to hang on startup — likely a pseudo-terminal issue inside the Singularity sandbox. Setting `HOME=/tmp/claude-home` in `/etc/bash.bashrc` is cleaner and avoids this.

**Bedrock auth**: The container is configured to use Amazon Bedrock via Claude Code's native Bedrock mode (`CLAUDE_CODE_USE_BEDROCK=1`). The user supplies `AWS_BEARER_TOKEN_BEDROCK` and `OPENAI_API_KEY` at runtime as `SINGULARITYENV_*` variables, which Singularity converts to regular env vars inside the container. The Bedrock config variables (`CLAUDE_CODE_USE_BEDROCK`, `OPENAI_BASE_URL`, `AWS_REGION`) are set in `/etc/bash.bashrc`.

**If modifying Claude Code setup**: 
- Do NOT use the official install script; it will break the SIF.
- Ensure `PATH` in `/etc/bash.bashrc` comes before any system paths that might have old Claude binaries.
- Test with `which claude`, `claude --version`, and a simple claude prompt to verify terminal interaction works.
- If Claude hangs on startup, it's likely `/proc` or `/dev` access — check that no restrictive container flags like `--contain` are in use.

---
