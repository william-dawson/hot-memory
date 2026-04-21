---
description: Build/run/profile the synthetic MPI benchmark in /workspace. TRIGGER when: user asks to build, run, compile, test, profile bench.c, stream_kernel, or compute_kernel. Invoke BEFORE starting any work.
---

# Skill: my-code

## What this code is

A synthetic MPI benchmark with two kernels that deliberately sit at
opposite ends of the memory-vs-compute spectrum. Runs in a few seconds
on any hardware.

The stream arrays are distributed across MPI ranks (strong scaling), so
each rank's working set shrinks as you add more processes. The compute
kernel is per-rank (weak scaling) — each rank works on its own 2 MB array.

## Source layout

```
/workspace/
  bench.c       — entry point and both kernels; everything is in one file
  Makefile      — build targets: `bench` (plain) and `profile` (with profiling)
```

Key functions:

- `main()` — MPI init, allocation, distributes work, calls both kernels
- `stream_kernel(a, b, c, n)` — writes `c[i] = a[i] + b[i]` over the local
  portion of 16 M doubles total; three arrays split across ranks; with 4 ranks
  each rank touches ~8 MB per array (~24 MB total) — memory-bandwidth-bound
- `compute_kernel(x, n, iters)` — 4 M doubles per rank, 100 multiply-add
  iterations per element — compute-bound

## Build command

```bash
cd /workspace
make bench
```

### Extending the build

This is a C project built with `mpicc` via a simple Makefile.

- **To add extra C compiler flags** (e.g. `-DSOME_FLAG`): `make CFLAGS+="-DSOME_FLAG" bench`
- **To add extra linker flags** (e.g. `-lsomelib`): `make LDFLAGS+="-lsomelib" bench`
- **To add a header include path**: `make CFLAGS+="-I/path/to/headers" bench`
- **To include a new C header**: add `#include "header.h"` to `bench.c`; if it's
  on a system path (`/usr/local/include`) no `-I` flag is needed.

The Makefile variables `CFLAGS` and `LDFLAGS` accept appended values.

## Run command

```bash
mpirun -np 4 ./bench
```

Successful run prints one line to stdout and exits 0:
```
checksum: <some floating-point number>
```

## Expected output / correctness check

Exit code 0 and a line matching `checksum:` on stdout. The exact checksum
value depends on the number of ranks.

## Notes for the profiler

- **This is a C code** using MPI and OpenMP.
- Both kernels are called once per run (no time-stepping loop).
- `stream_kernel` touches three large arrays — memory-bandwidth-bound.
  Hot MB scales with rank count (~96 MB at -np 4, ~48 MB at -np 8).
- `compute_kernel` has a ~32 MB working set regardless of rank count —
  compute-bound.
- There is an `MPI_Allreduce` after each kernel for synchronisation.
- Arrays are pre-initialised before the kernel runs, so all pages are
  already mapped.
