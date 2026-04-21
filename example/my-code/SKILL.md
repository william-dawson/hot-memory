---
description: Build/run/profile the synthetic MPI+OpenMP benchmark in /workspace. TRIGGER when: user asks to build, run, compile, test, profile bench.c, stream_kernel, or compute_kernel. Invoke BEFORE starting any work.
---

# Skill: my-code

## What this code is

A synthetic MPI+OpenMP benchmark with two kernels that deliberately sit at
opposite ends of the memory-vs-compute spectrum. Used to validate the
wss-profiler skill end-to-end. Runs in a few seconds on any hardware.

The stream arrays are distributed across MPI ranks (strong scaling), so
each rank's working set shrinks as you add more processes. The compute
kernel is per-rank (weak scaling) — each rank works on its own 2 MB array.

## Source layout

```
/workspace/
  bench.c       — entry point and both kernels; everything is in one file
  Makefile      — build targets: `bench` (plain) and `profile` (with WSS macros)
```

Key functions:

- `main()` — MPI init, allocation, distributes work, calls both kernels
- `stream_kernel(a, b, c, n)` — writes `c[i] = a[i] + b[i]` over the local
  portion of 128 M doubles total; three arrays split across ranks; with 4 ranks
  each rank touches ~96 MB per array (~288 MB total) — memory-bandwidth-bound
- `compute_kernel(x, n, iters)` — 256 K doubles per rank, 1000 multiply-add
  iterations per element; ~256 M FLOPs on a 2 MB working set — compute-bound

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
mpirun -np 4 ./bench
```

Successful run prints one line to stdout and exits 0:
```
checksum: <some floating-point number>
```

WSS output (when profiling) goes to stderr.

## Expected output / correctness check

Exit code 0 and a line matching `checksum:` on stdout. The exact checksum
value depends on the number of ranks.

## Notes for the profiler

- Both kernels are called once per run (no time-stepping loop). Instrument
  each call site directly — no need to ask about loop vs. single-iteration.
- The WSS macros are already present in bench.c and compiled away unless
  `-DPROFILE_WSS` is set. Claude does not need to add them; just rebuild
  with `make profile` to activate them.
- `stream_kernel` hot MB scales with rank count: ~768 MB at -np 4, ~384 MB
  at -np 8, etc. (3 arrays × STREAM_N_TOTAL / nprocs × 8 bytes).
- `compute_kernel` is expected to show ~2 MB hot and ~128 FLOP/byte
  regardless of rank count.
- There is an `MPI_Allreduce` after each kernel to add real communication
  overhead — this will show up in perf profiling as MPI library time.
- Arrays are pre-initialised before the kernel runs, so all pages are
  already mapped — the hot-byte count reflects only the kernel's true
  working set, not first-touch overhead.
