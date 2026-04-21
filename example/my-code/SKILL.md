---
description: Build/run/profile the synthetic MPI+OpenMP benchmark in /workspace. TRIGGER when: user asks to build, run, compile, test, profile bench.c, stream_kernel, or compute_kernel. Invoke BEFORE starting any work.
---

# Skill: my-code

## What this code is

A synthetic MPI+OpenMP benchmark with two kernels that deliberately sit at
opposite ends of the memory-vs-compute spectrum. Used to validate the
wss-profiler skill end-to-end. Runs in a few seconds on any hardware.

2 MPI ranks, OpenMP within each rank (thread count follows `OMP_NUM_THREADS`
or defaults to the number of cores).

## Source layout

```
/workspace/
  bench.c       — entry point and both kernels; everything is in one file
  Makefile      — build targets: `bench` (plain) and `profile` (with WSS macros)
```

Key functions:

- `main()` — MPI init, allocation, calls both kernels, prints checksum
- `stream_kernel(a, b, c, n)` — writes `c[i] = a[i] + b[i]` over 32 M doubles;
  three arrays of 256 MB each (768 MB total working set); one FLOP per three
  reads — memory-bandwidth-bound
- `compute_kernel(x, n, iters)` — 256 K doubles, 1000 multiply-add iterations
  per element; ~256 M FLOPs on a 2 MB working set — compute-bound

## Build command

Standard build (no profiling):
```bash
cd /workspace
make bench
```

With WSS profiling macros enabled (Claude injects this when instrumenting):
```bash
cd /workspace
make profile
```

The `profile` Makefile target automatically adds `-DPROFILE_WSS` and `-lpapi`.
No manual flag injection is needed.

## Run command

```bash
mpirun --allow-run-as-root -np 2 ./bench
```

`--allow-run-as-root` is required when running as root inside the profiling
container. Outside a container, drop that flag.

Successful run prints one line to stdout and exits 0:
```
checksum: <some floating-point number>
```

WSS output (when profiling) goes to stderr.

## Expected output / correctness check

Exit code 0 and a line matching `checksum:` on stdout. The exact checksum
value is deterministic for a given build.

## Notes for the profiler

- Both kernels are called once per run (no time-stepping loop). Instrument
  each call site directly — no need to ask about loop vs. single-iteration.
- The WSS macros are already present in bench.c and compiled away unless
  `-DPROFILE_WSS` is set. Claude does not need to add them; just rebuild
  with `make profile` to activate them.
- `stream_kernel` is expected to show ~768 MB hot and near-zero FLOP/byte.
- `compute_kernel` is expected to show ~2 MB hot and ~128 FLOP/byte.
  These make a useful sanity check that the profiler is working correctly.
- Arrays `a`, `b`, `c` are pre-initialised before the kernel runs, so all
  pages are already mapped — the hot-byte count reflects only the kernel's
  true working set, not first-touch overhead.
