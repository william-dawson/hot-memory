# hot-memory

An AI-agent workflow for HPC performance modelling. Given an MPI/OpenMP C/C++ code, it answers two questions:

1. **Where is time going?** (sampling via `perf`)
2. **For a given kernel: how many unique bytes are hot, and how many FLOPs does it execute?** (instrumentation via `/proc/clear_refs` + PAPI)

The motivation is GPU memory planning: before porting a code to a small-memory GPU (e.g. a consumer RTX), you need to know not just total allocation but the *hot working set* of each kernel. That tells you what actually needs to fit on the device, what can stay resident between kernels, and what must be swapped.

---

## Requirements

- Linux host, `amd64` or `aarch64` (e.g. NVIDIA Grace / Neoverse V2)
- Singularity or Apptainer
- Amazon Bedrock access (for Claude Code)

The profiling tools require a real Linux kernel — Docker Desktop for Mac
will not work. The workflow degrades gracefully depending on what the
host permits:

| Feature | Requires | Without it |
|---------|----------|------------|
| Hot-byte measurement (`/proc/clear_refs`) | `--fakeroot` or root | Not available |
| Hotspot discovery (`perf record`) | `perf_event_paranoid` ≤ 1 | Claude falls back to `MPI_Wtime()` instrumentation |
| FLOP counting (PAPI) | `perf_event_paranoid` ≤ 1 | FLOPs reported as 0; hot-byte measurement still works |

For the full workflow, ask your sysadmin to set on the compute nodes:
```bash
sysctl kernel.perf_event_paranoid=-1
```

---

## Quickstart

**1. Build the container**

```bash
git clone https://github.com/william-dawson/hot-memory.git
cd hot-memory
singularity build --fakeroot hotmemory.sif hotmemory.def
# or: apptainer build --fakeroot hotmemory.sif hotmemory.def
# (omit --fakeroot if you have root)
```

If a GitHub Release has been published with `hotmemory.sif` attached you can download that instead.

**2. Write your code skill**

```bash
cp -r skills/code-template my-code-skill
$EDITOR my-code-skill/SKILL.md
```

Fill in your source layout, build command, run command, and which functions are the hot kernels. See `example/my-code/SKILL.md` for a complete example.

**3. Run**

```bash
export SINGULARITYENV_AWS_BEARER_TOKEN_BEDROCK=<your-bearer-token>
export SINGULARITYENV_OPENAI_API_KEY=<your-openai-api-key>

singularity run --fakeroot --pwd /workspace \
                --bind /path/to/your/code:/workspace \
                --bind /path/to/my-code-skill:/skills/my-code \
                ./hotmemory.sif bash
```

**4. Start Claude Code**

```bash
claude
```

Ask:

- *"Find the hotspots in my code."* → Claude runs `perf`, reports top functions by % wall-clock time.
- *"Measure the working set of stencil_apply."* → Claude instruments the kernel, rebuilds, runs, reports hot MB + FLOP/byte.
- *"Will this fit on an RTX 5070?"* → Claude uses the measured hot sets to reason about GPU memory requirements.

> **Note:** If `perf` returns "Permission denied", the workflow will still
> work — Claude will use timing-based instrumentation instead for hotspot
> discovery, and hot-byte measurement works regardless. FLOP counts will
> be reported as 0. For the full workflow including `perf` and FLOP counts,
> ask your sysadmin to run: `sysctl kernel.perf_event_paranoid=-1`

---

## How it works

The container bundles profiling tools and skill files. Skills are markdown documents that teach Claude how to do something — one describes the profiling methodology, the other describes the user's specific code. Claude reads both and synthesises.

```
/skills/
  wss-profiler/       ← baked into the image
    SKILL.md              profiling methodology, interpretation, GPU memory reasoning
    wss_profiler.h        C header installed at /usr/local/include
  my-code/            ← mounted by you at runtime
    SKILL.md              your code's build/run commands and kernel layout
```

Neither skill references the other. Claude is the glue.

---

## Try it with the built-in example

The `example/` directory contains a synthetic MPI+OpenMP benchmark with two kernels deliberately at opposite ends of the spectrum — `stream_kernel` is memory-bound (~768 MB hot, near-zero FLOP/byte) and `compute_kernel` is compute-bound (~2 MB hot, ~128 FLOP/byte). Use it to verify the whole stack works before profiling your own code.

```bash
export SINGULARITYENV_AWS_BEARER_TOKEN_BEDROCK=<your-bearer-token>
export SINGULARITYENV_OPENAI_API_KEY=<your-openai-api-key>

singularity run --fakeroot --pwd /workspace \
                --bind "$(pwd)/example":/workspace \
                --bind "$(pwd)/example/my-code":/skills/my-code \
                ./hotmemory.sif bash
```

Inside the container:

```bash
claude
```

Try asking:
- *"Find the hotspots in this code."*
- *"Measure the working set of both kernels."*
- *"Would this fit on a GPU with 4 GB of memory?"*
