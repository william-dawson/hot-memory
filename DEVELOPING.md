# Developer notes

How to build, test, and publish the `williamdawson/hot-memory` Docker image.

---

## Prerequisites

- Docker (tested with 24+)
- A Docker Hub account with push access to `williamdawson/hot-memory`
- `docker login` already run

---

## Build the image

```bash
git clone git@github.com:william-dawson/hot-memory.git
cd hot-memory
docker build -t williamdawson/hot-memory:latest .
```

The build COPYs `skills/wss-profiler/SKILL.md` and `wss_profiler.h` into
the image at `/skills/wss-profiler/`. Everything else (user code, user code
skill) is mounted at runtime.

---

## Smoke-test locally

Run the synthetic benchmark inside the freshly built image:

```bash
docker run --privileged \
  -v "$(pwd)/example":/workspace \
  williamdawson/hot-memory:latest \
  bash -c '
    cd /workspace
    mpicc -O2 -fopenmp -DPROFILE_WSS -I/skills/wss-profiler bench.c \
          -o bench -lpapi
    mpirun --allow-run-as-root -np 2 ./bench 2>&1
  '
```

Expected output includes two `[WSS]` lines: `stream_kernel` with hundreds of
MB and near-zero FLOP/byte, and `compute_kernel` with ~2 MB and high
FLOP/byte.

If PAPI counters are unavailable (some CI hosts restrict `perf_event_open`),
the FLOPs column will be 0 but the hot-MB measurement should still work.

---

## Tag and push

```bash
# Optionally tag a versioned release alongside latest
docker tag williamdawson/hot-memory:latest williamdawson/hot-memory:$(git rev-parse --short HEAD)

docker push williamdawson/hot-memory:latest
docker push williamdawson/hot-memory:$(git rev-parse --short HEAD)
```

---

## Extending the image

If a user's code needs additional libraries (FFTW, HDF5, NetCDF, …), they
write a small Dockerfile that starts FROM the published image:

```dockerfile
FROM williamdawson/hot-memory:latest
RUN apt-get update && apt-get install -y libfftw3-dev libhdf5-dev \
    && rm -rf /var/lib/apt/lists/*
```

They build and run that derived image; the profiler skill and header are
already present at `/skills/wss-profiler/`.

---

## Repo layout

```
.
├── Dockerfile                      image definition
├── wss_profiler.h                  C profiling header (also COPYd into image)
├── skills/
│   ├── wss-profiler/
│   │   └── SKILL.md                profiler skill (COPYd into image)
│   └── code-template/
│       └── SKILL.md                template for users to write their code skill
├── example/
│   ├── bench.c                     synthetic MPI+OpenMP benchmark
│   └── Makefile
├── plan.md                         design doc and methodology
├── README.md                       user-facing docs
└── DEVELOPING.md                   this file
```

---

## perf inside containers

`perf` requires either `--privileged` or the combination of
`--cap-add SYS_ADMIN --cap-add SYS_PTRACE --security-opt seccomp=unconfined`.
The host kernel must also allow perf events:

```bash
sudo sysctl kernel.perf_event_paranoid=-1
```

The Dockerfile installs `linux-tools-generic` and creates a stable symlink at
`/usr/local/bin/perf` pointing to the kernel-versioned binary inside the
image. If the container's kernel version differs significantly from the host's,
perf may still work (it uses the host kernel's perf subsystem via syscalls)
but the symlink target inside the image may not match. In that case, bind-mount
the host perf binary:

```bash
docker run --privileged \
  -v $(which perf):/usr/local/bin/perf \
  ...
```
