# wss-profiler base image
#
# Target platform: linux/amd64 (HPC nodes).
# Requires a real Linux host — perf and PAPI hardware counters do not work
# inside Docker Desktop for Mac (VM kernel, no hardware counter access).
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
#     -e ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY \
#     -v /path/to/your/code:/workspace \
#     -v /path/to/your/code-skill:/skills/my-code \
#     -it wddawson/hotmemory:latest bash
#
# ── Extending the image for your code's dependencies ─────────────────────
#
#   FROM wddawson/hotmemory:latest
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
    # perf: generic installs the wrapper; the versioned pkg has the actual binary
    linux-tools-generic \
    linux-tools-common \
    # useful utilities
    curl \
    git \
    strace \
    procps \
    && PERF_BIN=$(find /usr/lib/linux-tools-* -name perf -type f | head -1) \
    && ln -sf "$PERF_BIN" /usr/local/bin/perf \
    && rm -rf /var/lib/apt/lists/*

# ── Claude Code ───────────────────────────────────────────────────────────
RUN curl -fsSL https://claude.ai/install.sh | bash
ENV PATH="/root/.local/bin:${PATH}"

# Configure Claude Code to read the API key from the ANTHROPIC_API_KEY
# environment variable passed in at runtime (docker run -e ANTHROPIC_API_KEY=...).
# The key is never baked into the image.
RUN mkdir -p /root/.claude && \
    printf '{\n  "apiKeyHelper": "echo $ANTHROPIC_API_KEY"\n}\n' \
    > /root/.claude/settings.json

# ── Install the wss-profiler skill ────────────────────────────────────────
COPY skills/wss-profiler/SKILL.md /skills/wss-profiler/SKILL.md
COPY wss_profiler.h /skills/wss-profiler/wss_profiler.h
COPY wss_profiler.h /usr/local/include/wss_profiler.h

# ── Claude Code slash commands ─────────────────────────────────────────────
RUN mkdir -p /root/.claude/commands && \
    cp /skills/wss-profiler/SKILL.md /root/.claude/commands/wss-profiler.md

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

WORKDIR /workspace
ENTRYPOINT ["/entrypoint.sh"]
CMD ["bash"]
