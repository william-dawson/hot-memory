---
description: Build/run/profile QWS (lattice QCD) in /workspace. TRIGGER when: user asks to build, run, compile, test, profile, or mentions QWS, lattice QCD, BiCGStab, Wilson fermion, or Dirac operator. Invoke BEFORE starting any work.
---

# Skill: my-code

## What this code is

QWS (QCD Wide SIMD) is a lattice quantum chromodynamics simulation library.
It implements Wilson-clover fermion solvers (BiCGStab, CG) with domain
decomposition preconditioning, optimised for wide SIMD architectures.
C/C++ with MPI and OpenMP.

From https://github.com/RIKEN-LQCD/qws.

## Source layout

```
/workspace/qws/
  main.cc                — test program entry point, solver benchmarks
  qws.cc / qws.h        — core library: lattice setup, data structures, init/finalize
  bicgstab.cc            — BiCGStab solver (double precision)
  bicgstab_dd_s.cc       — domain-decomposed BiCGStab (single precision)
  bicgstab_dd_d.cc       — domain-decomposed BiCGStab (double precision)
  bicgstab_dd_mix.cc     — mixed-precision DD BiCGStab (the main production solver)
  bicgstab_precdd_s.cc   — preconditioned DD BiCGStab (single precision)
  static_solver.cc       — Jacobi-method static solver (DD preconditioner inner solve)
  cg.cc                  — Conjugate Gradient solver
  deo_in_d.cc / deo_out_d.cc   — Dirac operator (even-odd, double precision)
  deo_in_s.cc / deo_out_s.cc   — Dirac operator (even-odd, single precision)
  ddd_in_d.cc / ddd_out_d.cc   — domain-decomposed Dirac operator (double)
  ddd_in_s.cc / ddd_out_s.cc   — domain-decomposed Dirac operator (single)
  clover_s.cc            — clover term application (single precision)
  reordering.cc          — data layout reordering for SIMD
  util.cc                — utility functions (norms, printing)
  timing.c / timing.h    — internal timing instrumentation
  qws_xbound_mpi.cc      — MPI boundary exchange
  data/                   — reference output for test cases (CASE0..CASE9)
  check.sh               — script to compare output against reference data
  Makefile               — build system with many architecture/compiler options
```

## Build command

The Makefile uses variables to select compiler, architecture, and features.
For a standard MPI+OpenMP build with GNU compilers:

```bash
cd /workspace/qws
make -j $(nproc) fugaku_benchmark= omp=1 compiler=openmpi-gnu arch=thunderx2 rdma= mpi=1 powerapi=
```

The `arch=` setting controls SIMD vector lengths (`vlend`, `vlens`). Inspect
the Makefile for available options (`thunderx2`, `skylake`, `postk`, etc.)
and choose the one closest to your hardware.

Or use the fetch script:
```bash
bash /workspace/fetch_and_build.sh
```

### Extending the build

The Makefile defines `CFLAGS`, `CXXFLAGS`, `LDFLAGS`, `MYFLAGS`, and
`SYSLIBS` depending on the selected compiler. To extend the build:

- **To add extra C/C++ compiler flags**: append to `CFLAGS` or use
  `CPPFLAGS`, e.g. `make ... CPPFLAGS="-I/path/to/headers -DSOME_FLAG"`
- **To add extra linker flags / libraries**: append to `LDFLAGS` or
  `SYSLIBS`, e.g. `make ... SYSLIBS="-lsomelib"`
- **To include a new C/C++ header**: add `#include "header.h"` to the
  relevant `.cc` file; if it's on a system path no `-I` flag is needed.
  Otherwise pass `-I/path` via `CPPFLAGS`.
- The final link is done by `$(CXX)` (mpicxx for openmpi-gnu).

## Run command

The test program takes 12 command-line arguments:

```
./main lx ly lz lt px py pz pt tol_outer tol_inner maxiter_outer maxiter_inner
```

| Argument | Meaning |
|----------|---------|
| `lx ly lz lt` | Local lattice dimensions (per MPI rank). Working set scales as `lx × ly × lz × lt`. |
| `px py pz pt` | MPI process grid. Total ranks = `px × py × pz × pt`. |
| `tol_outer` | Convergence tolerance for the outer DD BiCGStab solver. Set to `-1` to run for exactly `maxiter_outer` iterations (useful for profiling). |
| `tol_inner` | Convergence tolerance for the inner preconditioner solver. Set to `-1` for fixed iterations. |
| `maxiter_outer` | Maximum iterations for the outer solver (note: the actual count is `maxiter_outer - 1`). |
| `maxiter_inner` | Maximum iterations for the inner preconditioner. |

Single-process test (small lattice):
```bash
mpirun -np 1 ./main 32 6 4 3 1 1 1 1 -1 -1 6 50
```

Multi-process test:
```bash
mpirun -np 2 ./main 32 6 4 3 1 1 1 2 -1 -1 6 50
```

For profiling, use `tol=-1` so the solver runs a fixed number of
iterations regardless of convergence. To increase the working set,
increase the local lattice dimensions (e.g. `64 12 8 6` instead of
`32 6 4 3`).

## Expected output / correctness check

The program prints norm values and solver timing. Reference outputs are
in `data/CASE0` through `data/CASE9`. Verify with:

```bash
./main 32 6 4 3 1 1 1 1 -1 -1 6 50 > output.txt
./check.sh output.txt data/CASE0
```

The check script compares norms. Small floating-point differences across
architectures are acceptable.

## Notes for the profiler

- **This is a C/C++ code** using MPI and OpenMP.
- The main computational kernels are the Dirac operator applications
  (`deo_*`, `ddd_*`) and the clover term (`clover_s`). These are called
  many times per solver iteration inside the BiCGStab loop.
- The solver loop is in `bicgstab_dd_mix.cc`. Each iteration calls
  the domain-decomposed Dirac operator and the preconditioner.
- Setting `tol=-1` runs for a fixed iteration count — useful for
  consistent profiling runs.
- Lattice size controls the working set. `32 6 4 3` is small; increase
  dimensions for realistic memory measurements.
