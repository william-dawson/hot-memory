# Hot Memory

> [!NOTE]
This repository is a model of what an outsourced AI-agent deliverable should look like. The idea: when you hire someone to build a tool, they should ship it in a container that drops you straight into a vibe coding environment. Everything you need — the tool, its dependencies, and the skill files that teach your AI agent how to use it — is right there. You type `claude` and start working.

The specific tool here is an HPC performance profiler. Given an MPI/OpenMP C/C++/Fortran code, it answers one key research question: for the key kernels ina  program, how many unique bytes are **hot**.

The motivation is GPU memory planning. Before porting a code to a small-memory GPU (like a consumer RTX), you need to know not just total allocation but the *hot working set* of each kernel. Total allocation is always a pessimistic overestimate — no single kernel touches all of it. The hot set tells you what actually needs to fit on the device, what can stay resident between kernels, and what must be swapped.

### How we measure "hot" memory

Before a kernel runs, we clear the Linux "Referenced" bit on every memory page by writing to `/proc/self/clear_refs`. After the kernel finishes, we read `/proc/self/smaps` and sum the `Referenced` fields — that's the number of unique 4 KB pages the kernel actually touched. This is a direct measurement at the OS level: no sampling, no estimation, no compiler instrumentation. The only approximation is 4 KB page granularity (if a kernel touches 1 byte on a page, the whole 4 KB counts).

This gives a per-kernel hot working set in MB, which is the fundamental input for GPU memory planning: if the heaviest kernel's hot set fits in device memory, the whole code can run on the GPU without swapping.

---

## How the delivery model works

The container is the delivery vehicle. Inside it:

```
/skills/
  wss-profiler/                 ← baked into the image
    SKILL.md                      profiling methodology, interpretation, GPU memory reasoning
    wss_profiler/*.h/*.f90/etc    source files
  my-code/                      ← mounted by you at runtime
    SKILL.md                      your code's build/run commands and kernel layout
```

Skill files are the key abstraction. One skill describes the profiling methodology (shipped by us). The other describes your specific code (written by you alone or with the help of AI). Neither references the other — Claude reads both and synthesises.

> [!NOTE]
> These tools bridge the knowledge gap. The developer of the profiling tool writes a skill so an agent knows how to use the tool. You help prepare the skill file related to the code to apply it to. An agent helps with both these tasks.

---

## Requirements

- Linux host
- Singularity or Apptainer
- Amazon Bedrock access (for Claude Code)

The profiling tools require a real Linux kernel — Docker Desktop for Mac will not work. The workflow degrades gracefully depending on what the host permits:

| Feature | Requires | Without it |
|---------|----------|------------|
| Hot-byte measurement (`/proc/clear_refs`) | `--fakeroot` or root | Not available |
| Hotspot discovery (`perf record`) | `perf_event_paranoid` ≤ 0 | Phase 1 unavailable; skip to Phase 2 if you know which kernels to target |
| FLOP counting (PAPI) | `perf_event_paranoid` ≤ 0 | FLOPs reported as 0; hot-byte measurement still works |

For the full workflow, ask your sysadmin to set on the compute nodes:
```bash
sysctl kernel.perf_event_paranoid=0
```

### OpenMP limitation

PAPI hardware counters (FLOPs, load/store counts) only instrument the **main thread**. OpenMP worker thread activity is not counted. The hot-byte measurement (`/proc/self/smaps`) *is* process-wide and includes all threads — so hot MB is accurate for OpenMP codes, but FLOP counts and total bytes accessed will be undercounted. For OpenMP codes, treat FLOP-derived metrics as lower bounds and rely primarily on hot bytes for GPU memory planning.

---

## Quickstart

**1. Get the repo**

```bash
git clone https://github.com/william-dawson/hot-memory.git
cd hot-memory
```

The wrapper script `hotmemory.sh` will automatically download the correct
pre-built container (amd64 or arm64) on first run. To build from source
instead, run `singularity build --fakeroot hotmemory.sif hotmemory.def`.

**2. Set your credentials**

```bash
export SINGULARITYENV_AWS_BEARER_TOKEN_BEDROCK=<your-bearer-token>
export SINGULARITYENV_OPENAI_API_KEY=<your-openai-api-key>
```

**3. Point it at your code**

```bash
./hotmemory.sh /path/to/your/code /path/to/your/code
```

If you already have a code skill, mount it as the second argument. If not, just mount your code directory for both — once inside, ask Claude:

*"Generate a skill file for this project."*

Claude will examine your source tree, figure out the build system, try building, and produce a SKILL.md describing your code. Review it, then you're ready to profile.

Inside the container, start Claude Code:

```bash
claude
```

Ask:

- *"Find the hotspots in my code."*
- *"Measure the working set of stencil_apply."*
- *"Will this fit on an RTX 5070?"*

> **Note:** If `perf` returns "Permission denied", Phase 1 (hotspot discovery)
> is unavailable but Phase 2 (hot-byte measurement) still works if you already
> know which kernels to target. FLOP counts will be reported as 0. For the full
> workflow, ask your sysadmin to run: `sysctl kernel.perf_event_paranoid=0`

---

## Try it with the built-in example

The `example/` directory contains a synthetic MPI benchmark with two kernels that split memory roughly 75/25 — `stream_kernel` is memory-bound (~96 MB hot at 4 ranks) and `compute_kernel` is compute-bound (~32 MB hot). Together they use about 128 MB per rank, but neither touches all of it — exactly the scenario where hot-set profiling is more useful than peak allocation.

```bash
./hotmemory.sh ./example ./example/my-code
```

Inside the container:

```bash
claude
```

Try asking:
- *"Find the hotspots in this code."*
- *"Measure the working set of both kernels."*
- *"Would this fit on a GPU with 4 GB of memory?"*

---

## Try it with CloverLeaf

For a more realistic test, the `cloverleaf/` directory contains a skill and fetch script for [CloverLeaf](https://github.com/UK-MAC/CloverLeaf_ref), a Lagrangian-Eulerian hydrodynamics mini-app with 9 distinct kernels per timestep — each with different memory access patterns.

```bash
./hotmemory.sh ./cloverleaf ./cloverleaf/my-code
```

The skill file includes knowledge of using the fetch script to grab the code automatically. 
