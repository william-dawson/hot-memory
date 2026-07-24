# Hot Memory

Hot Memory profiles MPI applications to measure:

- per-kernel hot working sets;
- memory traffic and FLOPs, when hardware counters are available;
- likely GPU memory requirements.

It supports C, C++, and Fortran codes. The analysis is MPI-only.

## Requirements

- Linux on the target machine (x86_64 or aarch64)
- Singularity or Apptainer
- Claude Code with an active subscription
- An MPI application and a short build/run description

`perf`, PAPI, and hot-page measurement depend on host permissions. For the
full workflow, an administrator may need to set:

```bash
sysctl kernel.perf_event_paranoid=0
```

Hot-page measurement also requires `--fakeroot`, root, or equivalent
permissions. OpenMP should be disabled; otherwise use `OMP_NUM_THREADS=1` and
interpret the results as MPI-only.

## Quickstart

Clone and enter the repository:

```bash
git clone https://github.com/william-dawson/hot-memory.git
cd hot-memory
```

Build the image, or let `hotmemory.sh` download a release image when one is
available:

```bash
singularity build --fakeroot hotmemory.sif hotmemory.def
```

Launch Hot Memory with your code and its skill file:

```bash
./hotmemory.sh /path/to/code /path/to/code-skill
```

Inside the container, run:

```bash
claude
```

On the first run, complete Claude Code's normal subscription login if needed.

Then ask Claude to find hotspots or measure specific kernels. If you do not
have a code skill yet, mount the code directory as both arguments and ask
Claude to generate one:

```bash
./hotmemory.sh /path/to/code /path/to/code
```

## Built-in example

The benchmark is already instrumented:

```bash
./hotmemory.sh ./examples/bench ./examples/bench/my-code
```

Inside Claude, ask:

```text
Build with profiling and measure both kernels.
```

At four MPI ranks, `stream_kernel` should touch about 96 MB and
`compute_kernel` about 32 MB, subject to system noise and counter availability.

## Plugin mode

`plugins/hotmemory/` contains a Claude Code/Codex plugin that registers the
profiling skill and MCP server. The server still runs inside the SIF.

By default, the plugin looks for `hotmemory.sif` at the repository root. To
use another image or Apptainer:

```bash
export HOTMEMORY_SIF=/path/to/hotmemory.sif
export HOTMEMORY_RUNTIME=apptainer
```

The wrapper mounts `~/.claude` into the container so your normal Claude Code
subscription login persists between runs. Set `CLAUDE_CONFIG_DIR` to use a
different Claude configuration directory.

## What the profiler measures

Before each kernel, it clears Linux page-reference bits. Afterward, it reads
`/proc/self/smaps` and reports the pages referenced by that kernel. Results are
rounded to 4 KiB pages and include a small amount of process overhead.

The MCP server provides capability checks, baseline RSS measurement, `perf`
hotspot profiling, and structured WSS results. It does not modify source code;
Claude decides where to instrument and how to rebuild the application.

For implementation details and contributor guidance, see [AGENTS.md](AGENTS.md).
