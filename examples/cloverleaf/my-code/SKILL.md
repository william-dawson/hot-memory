---
description: Build/run/profile CloverLeaf in /workspace. TRIGGER when: user asks to build, run, compile, test, profile, or mentions any CloverLeaf kernel, file, or function. Invoke BEFORE starting any work.
---

# Skill: my-code

## What this code is

CloverLeaf is a Lagrangian-Eulerian hydrodynamics mini-app from the UK
Mini-App Consortium. It solves the compressible Euler equations on a 2D
Cartesian grid using an explicit, second-order method. Each timestep
executes a fixed sequence of distinct computational kernels with different
memory access patterns.

MPI-only (no OpenMP). The reference Fortran+C version from
https://github.com/UK-MAC/CloverLeaf_ref.

## Source layout

```
/workspace/CloverLeaf/
  clover_leaf.f90          — main program entry point
  hydro.f90                — main timestep loop, calls each kernel in sequence
  kernels/
    accelerate_kernel.f90  — updates velocity from pressure gradients
    advec_cell_kernel.f90  — cell-centred advection
    advec_mom_kernel.f90   — momentum advection
    PdV_kernel.f90         — pressure-volume work (energy update)
    flux_calc_kernel.f90   — computes fluxes from velocities
    viscosity_kernel.f90   — artificial viscosity
    ideal_gas_kernel.f90   — equation of state (pressure from energy/density)
    reset_field_kernel.f90 — copies new fields to old fields
    revert_kernel.f90      — restores fields for predictor-corrector
    calc_dt_kernel.f90     — timestep calculation
    field_summary_kernel.f90 — diagnostic reductions
    update_halo_kernel.f90 — MPI halo exchange
    *_kernel_c.c           — C implementations of each kernel (called from Fortran)
  clover.in                — input deck (grid size, end time, test problem)
  InputDecks/              — alternative input decks for different test problems
```

The kernels called per timestep (in execution order) are:
1. `ideal_gas` — equation of state
2. `viscosity` — artificial viscosity
3. `calc_dt` — timestep control
4. `accelerate` — velocity update
5. `PdV` — energy update (called twice: predictor + corrector)
6. `flux_calc` — flux computation
7. `advec_cell` — cell advection
8. `advec_mom` — momentum advection
9. `reset_field` — swap new/old fields

## Build command

```bash
cd /workspace/CloverLeaf
make COMPILER=GNU
```

Or use the fetch script which clones and builds:
```bash
bash /workspace/fetch_and_build.sh
```

### Extending the build

This is a mixed Fortran+C project. The Makefile compiles all Fortran
files in a single `mpif90` command and all C files in a single `mpicc`
command. The final link is done by `mpif90`.

- **To add extra Fortran compiler flags**: `make COMPILER=GNU OPTIONS="-some-flag"`
- **To add extra C compiler flags**: `make COMPILER=GNU C_OPTIONS="-some-flag"`
- **To link an additional library**: add `-l` flags to the `mpif90` link
  command in the Makefile. The link step is in the `clover_leaf:` target —
  append before the `-o clover_leaf` at the end.
- **To use a Fortran module from an external library**: `mpif90` does not
  search system include paths for `.mod` files by default. You must pass
  `-I/path/to/mod/files` via `OPTIONS` so the compiler can find them.
  Then add `use module_name` to the relevant `.f90` source file and link
  the library in the link step.
- **To include a new C header**: add `#include "header.h"` to the relevant
  `kernels/*_c.c` file; if it's on a system path no `-I` flag is needed.

## Run command

```bash
cd /workspace/CloverLeaf
mpirun -np 4 ./clover_leaf
```

The input deck `clover.in` controls grid size and number of timesteps.
The default runs test problem 1 (Sod shock tube) for a small number of
steps. For a more interesting profiling run, use a larger problem:

```bash
cp InputDecks/clover_bm16.in clover.in
mpirun -np 4 ./clover_leaf
```

## Expected output / correctness check

Successful runs print timestep progress to stdout and produce `clover.out`
with detailed diagnostics. Each timestep line shows:

```
 Step  <N>  time  <t>  control  <field>  timestep  <dt>
```

The final line of `clover.out` contains a field summary with volume, mass,
density, pressure, and kinetic energy. These values should match the
reference output for the chosen test problem (small floating-point
differences are acceptable).

Exit code 0 indicates success.

## Notes for the profiler

- **This is a Fortran code** with C kernel implementations.
- **MPI-only, no OpenMP.** There are no worker threads.
- The main timestep loop is in `hydro.f90`. Each kernel is called every
  timestep in a fixed order.
- `PdV`, `ideal_gas`, and `viscosity` all touch the energy/pressure/density
  fields. `advec_cell` and `advec_mom` touch the velocity and flux fields.
  The interesting profiling question is which kernels share arrays and
  which have disjoint working sets.
- Grid size controls the working set. The default `clover.in` is small.
  Use `clover_bm16.in` (or larger) for realistic memory measurements.
- The code creates output files (`clover.out`, `clover.visit`) in the
  working directory. These are small and harmless.
