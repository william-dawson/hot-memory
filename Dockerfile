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
# To extend for your code's dependencies, start FROM this image:
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
    strace \
    procps \
    && rm -rf /var/lib/apt/lists/*

# perf inside a container typically needs to use the host perf binary via
# /proc/sys/kernel/perf_event_paranoid=-1 on the host. The linux-tools-generic
# package installs a wrapper; the actual binary path includes the kernel version.
# Create a stable symlink so scripts can call /usr/bin/perf reliably.
RUN PERF_BIN=$(find /usr/lib/linux-tools -name perf -type f 2>/dev/null | head -1); \
    if [ -n "$PERF_BIN" ]; then \
        ln -sf "$PERF_BIN" /usr/local/bin/perf; \
    fi

WORKDIR /workspace

# The user mounts their code at /workspace and the skill at /skills.
# Example run:
#   docker run --privileged \
#     -v $(pwd)/my-code:/workspace \
#     -v $(pwd)/skills:/skills \
#     -it wss-profiler:latest bash
