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
# The install script drops the binary in ~/.local/bin; put it on PATH for
# all subsequent RUN steps and for interactive shells in the container.
ENV PATH="/root/.local/bin:${PATH}"

# Configure Claude Code to read the API key from the ANTHROPIC_API_KEY
# environment variable passed in at runtime (docker run -e ANTHROPIC_API_KEY=...).
# The key is never baked into the image.
RUN mkdir -p /root/.claude && \
    printf '{\n  "apiKeyHelper": "echo $ANTHROPIC_API_KEY"\n}\n' \
    > /root/.claude/settings.json

# perf: linux-tools-generic installs a wrapper that checks the running kernel
# version and refuses to run if it doesn't match (a problem on Docker Desktop
# for Mac, which uses a linuxkit kernel). Symlink past the wrapper directly to
# the versioned binary so perf works on any Linux host.
# On Docker Desktop for Mac perf syscalls still won't work — Phase 1 is
# Linux-only. Phase 2 (WSS + PAPI) works everywhere.
RUN PERF_BIN=$(find /usr/lib/linux-tools -name perf -type f 2>/dev/null | head -1); \
    if [ -n "$PERF_BIN" ]; then \
        ln -sf "$PERF_BIN" /usr/local/bin/perf; \
    else \
        printf '#!/bin/sh\necho "perf: not available (linuxkit kernel — run on a Linux host for Phase 1)" >&2\nexit 1\n' \
        > /usr/local/bin/perf && chmod +x /usr/local/bin/perf; \
    fi

# ── Install the wss-profiler skill ────────────────────────────────────────
# The wss-profiler skill is baked into the image so Claude Code can read it
# without the user having to mount it. The user's code skill is mounted at
# runtime at /skills/my-code (see run command above).
COPY skills/wss-profiler/SKILL.md /skills/wss-profiler/SKILL.md
# Copy header into the skill directory (for Claude to read) and into the
# system include path (so user code can #include "wss_profiler.h" without
# any -I flag).
COPY wss_profiler.h /skills/wss-profiler/wss_profiler.h
COPY wss_profiler.h /usr/local/include/wss_profiler.h

# ── Claude Code slash commands ─────────────────────────────────────────────
# ~/.claude/commands/*.md become /command-name slash commands inside Claude Code.
# wss-profiler is baked in. The user's my-code skill is mounted at runtime and
# copied in by the entrypoint.
RUN mkdir -p /root/.claude/commands && \
    cp /skills/wss-profiler/SKILL.md /root/.claude/commands/wss-profiler.md

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# /workspace is where the user's code lives (mounted at runtime).
WORKDIR /workspace
ENTRYPOINT ["/entrypoint.sh"]
CMD ["bash"]
