# hot-memory

This repository is a model of what an outsourced AI-agent deliverable should look like. The idea: when you hire someone to build a tool, they should ship it in a container that drops you straight into a vibe coding environment. Everything you need — the tool, its dependencies, and the skill files that teach your AI agent how to use it — is right there. You type `claude` and start working.

The specific tool here is an HPC performance profiler. Given an MPI/OpenMP C/C++/Fortran code, it answers two questions:

1. **Where is time going?** (sampling via `perf`)
2. **For a given kernel: how many unique bytes are hot, and how many FLOPs does it execute?** (instrumentation via `/proc/clear_refs` + PAPI)

The motivation is GPU memory planning. Before porting a code to a small-memory GPU (like a consumer RTX), you need to know not just total allocation but the *hot working set* of each kernel. Total allocation is always a pessimistic overestimate — no single kernel touches all of it. The hot set tells you what actually needs to fit on the device, what can stay resident between kernels, and what must be swapped.

---

## How the delivery model works

The container is the delivery vehicle. Inside it:

```
/skills/
  wss-profiler/       ← baked into the image by the vendor
    SKILL.md              profiling methodology, interpretation, GPU memory reasoning
    wss_profiler.h        C header installed at /usr/local/include
  my-code/            ← mounted by you at runtime
    SKILL.md              your code's build/run commands and kernel layout
```

**Skill files** are the key abstraction. They're markdown documents that teach Claude Code how to do something. One skill describes the profiling methodology (shipped by the vendor). The other describes your specific code (written by you from a template). Neither references the other — Claude reads both and synthesises.

This means the vendor doesn't need to know your code, and you don't need to understand the profiling methodology. You write a short description of your code, mount it into the container, and Claude does the rest.

---

## Requirements

- Linux host, `amd64` or `aarch64` (e.g. NVIDIA Grace / Neoverse V2)
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

---

## Quickstart

**1. Build the container**

```bash
git clone https://github.com/william-dawson/hot-memory.git
cd hot-memory
singularity build --fakeroot hotmemory.sif hotmemory.def
```

**2. Set your credentials**

```bash
export SINGULARITYENV_AWS_BEARER_TOKEN_BEDROCK=<your-bearer-token>
export SINGULARITYENV_OPENAI_API_KEY=<your-openai-api-key>
```

**3. Write your code skill**

```bash
cp -r skills/code-template my-code-skill
$EDITOR my-code-skill/SKILL.md
```

Fill in your source layout, build command, run command, and which functions are the hot kernels. See `example/my-code/SKILL.md` for a complete example.

**4. Run**

```bash
./hotmemory.sh /path/to/your/code /path/to/my-code-skill
```

**5. Start Claude Code**

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

The `example/` directory contains a synthetic MPI benchmark with two kernels deliberately at opposite ends of the spectrum — `stream_kernel` is memory-bound (~768 MB hot, near-zero FLOP/byte) and `compute_kernel` is compute-bound (~2 MB hot, ~128 FLOP/byte).

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

Inside the container:

```bash
bash fetch_and_build.sh
claude
```

This is where the methodology shines: total allocation may be large, but individual kernels touch only subsets of the data. The per-kernel hot set measurements tell you exactly what a GPU needs.
