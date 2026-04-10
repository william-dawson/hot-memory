---
description: Build/run/profile the code in this workspace. TRIGGER when: user asks to build, run, compile, test, profile, or modify code; or mentions any filename or function from the source layout. Invoke BEFORE starting any work.
---

# Skill: my-code

<!--
  INSTRUCTIONS FOR THE USER:
  Copy this file to your code's skills directory as SKILL.md and fill in
  every section below. Claude reads this alongside wss-profiler/SKILL.md
  to build, run, and profile your code without needing to guess.

  Remove these comment blocks when you're done.
-->

## What this code is

<!-- One or two sentences: what the code does, its domain, scale. -->
<!-- Example: "A 3-D finite-difference CFD solver for incompressible flow.
              Runs 500 timesteps on a 256³ grid. Single-node, 4 MPI ranks,
              OpenMP within each rank." -->

TODO

## Source layout

<!-- List the files that contain the hot kernels, roughly. -->
<!-- Example:
src/main.c          — entry point, MPI init, time loop
src/stencil.c       — stencil_apply(): main compute kernel
src/fft.c           — fft_forward(), fft_inverse()
src/boundary.c      — boundary_exchange(): MPI halo exchange
-->

TODO

## Build command

<!-- The exact command to build from scratch inside the container. -->
<!-- If the build accepts extra flags, say so — Claude will inject
     -DPROFILE_WSS and -lpapi here when profiling. -->
<!-- Example:
make clean && make EXTRA_CFLAGS="..." EXTRA_LDFLAGS="..."
-->

TODO

## Run command

<!-- The exact command to run a representative (not too long) test case. -->
<!-- Example:
mpirun -np 4 ./solver test/small.cfg
-->

TODO

## Expected output / correctness check

<!-- How to verify the run succeeded. A checksum, a line in stdout, etc. -->
<!-- Example: "Prints 'Converged at step 312' and exits 0." -->

TODO

## Notes for the profiler

<!--
  Optional. Anything that will help Claude instrument the right things.
  - Which functions are the inner-loop kernels?
  - Are kernels called inside a time loop? (If so, Claude will ask whether
    to instrument one iteration or all of them.)
  - Any arrays that you already know are large or important?
  - Compiler flags that must always be present (e.g. -DUSE_DOUBLE)?
-->

TODO
