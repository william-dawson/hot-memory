# Developer notes

How to build, test, and publish the `wddawson/hotmemory` Docker image.

---

## Prerequisites

- Docker (tested with 24+)
- A Docker Hub account with push access to `wddawson/hotmemory`
- `docker login` already run
- `ANTHROPIC_API_KEY` set in your host shell (passed into the container at runtime — never baked into the image)

---

## Local development workflow

Multi-platform `buildx` builds cannot be loaded into the local Docker daemon
(`--push` only). For iterating locally, build for the native platform only
and use `--load`:

```bash
cd hot-memory
docker buildx build --platform linux/arm64 --load \
  -t wddawson/hotmemory:dev .
```

Then test interactively with the built-in example:

```bash
docker run --privileged \
  -e ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY \
  -v "$(pwd)/example":/workspace \
  -v "$(pwd)/example/my-code":/skills/my-code \
  -it wddawson/hotmemory:dev bash
```

Inside the container, verify the two key things:
```bash
claude --version          # should print the Claude Code version
cd /workspace && make profile && make run   # should print two [WSS] lines
```

Once happy, do the full publish (see "Tag and push" below).

---

## Build the image (for publishing)

Build a multi-platform image covering both `linux/amd64` (HPC nodes) and
`linux/arm64` (Apple Silicon). `buildx` handles the cross-compilation and
produces a single manifest that Docker pulls the right variant from.

```bash
git clone git@github.com:william-dawson/hot-memory.git
cd hot-memory
docker buildx build --platform linux/amd64,linux/arm64 \
  -t wddawson/hotmemory:latest .
```

The build COPYs `skills/wss-profiler/SKILL.md` and `wss_profiler.h` into
`/skills/wss-profiler/` and `/usr/local/include/`. Everything else (user
code, user code skill) is mounted at runtime.

---

## Tag and push

```bash
# Build and push in one step (recommended)
docker buildx build --platform linux/amd64,linux/arm64 \
  -t wddawson/hotmemory:latest \
  -t wddawson/hotmemory:$(git rev-parse --short HEAD) \
  --push .
```

Or push an already-built local image:

```bash
docker push wddawson/hotmemory:latest
docker push wddawson/hotmemory:$(git rev-parse --short HEAD)
```

---

## Extending the image

If a user's code needs additional libraries (FFTW, HDF5, NetCDF, …), they
write a small Dockerfile that starts FROM the published image:

```dockerfile
FROM wddawson/hotmemory:latest
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
