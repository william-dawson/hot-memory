# wss-profiler base image
#
# Provides the full profiling toolchain for C/C++ MPI/OpenMP HPC codes:
#   - gcc/g++ with OpenMP
#   - OpenMPI (mpicc, mpirun)
#   - PAPI (papi_avail, libpapi-dev)
#   - perf (linux-tools-generic)
#
# Runtime flags required:
#   docker run --privileged ...
#   (needed for /proc/self/clear_refs and perf_event_open)
#
# Host may also need:
#   sysctl kernel.perf_event_paranoid=-1
#
# ── Skill layout inside the container ─────────────────────────────────────
#
#   /skills/
#     wss-profiler/SKILL.md   ← baked into the image (this repo)
#     my-code/SKILL.md        ← mounted at runtime by the user
#
# ── Typical run command ───────────────────────────────────────────────────
#
#   docker run --privileged \
#     -v /path/to/your/code:/workspace \
#     -v /path/to/your/code-skill:/skills/my-code \
#     -it wss-profiler:latest bash
#
# ── Extending the image for your code's dependencies ─────────────────────
#
#   FROM wss-profiler:latest
#   RUN apt-get update && apt-get install -y libfftw3-dev libhdf5-dev ...

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    # compilers
    gcc \
    g++ \
    gfortran \
    make \
    # MPI
    openmpi-bin \
    libopenmpi-dev \
    # PAPI hardware counters
    libpapi-dev \
    papi-tools \
    # perf (kernel version agnostic meta-package)
    linux-tools-generic \
    linux-tools-common \
    # useful utilities
    curl \
    git \
    strace \
    procps \
    && rm -rf /var/lib/apt/lists/*

# ── Claude Code ───────────────────────────────────────────────────────────
RUN curl -fsSL https://claude.ai/install.sh | bash

# perf inside a container typically needs perf_event_paranoid=-1 on the host.
# linux-tools-generic installs a kernel-versioned binary; create a stable
# symlink so scripts can call /usr/local/bin/perf reliably.
RUN PERF_BIN=$(find /usr/lib/linux-tools -name perf -type f 2>/dev/null | head -1); \
    if [ -n "$PERF_BIN" ]; then \
        ln -sf "$PERF_BIN" /usr/local/bin/perf; \
    fi

# ── Install the wss-profiler skill ────────────────────────────────────────
# The wss-profiler skill is baked into the image so Claude Code can read it
# without the user having to mount it. The user's code skill is mounted at
# runtime at /skills/my-code (see run command above).
COPY skills/wss-profiler/SKILL.md /skills/wss-profiler/SKILL.md
COPY wss_profiler.h                /skills/wss-profiler/wss_profiler.h

# /workspace is where the user's code lives (mounted at runtime).
WORKDIR /workspace
