# Developer notes

How to build, test, and publish the `wddawson/hotmemory` Docker image.

**Platform: `linux/amd64` only.** perf and PAPI hardware counters require a
real Linux host. Docker Desktop for Mac does not expose CPU performance
counters from its VM kernel, so profiling does not work there. Use a Linux
machine or HPC node for all testing.

---

## Prerequisites

- Docker (tested with 24+)
- A Linux host (amd64) for testing
- A Docker Hub account with push access to `wddawson/hotmemory`
- `docker login` already run
- `ANTHROPIC_API_KEY` set in your host shell

---

## Local development workflow

```bash
cd hot-memory
docker buildx build --platform linux/amd64 --load \
  -t wddawson/hotmemory:dev .
```

Test with the built-in example:

```bash
docker run --privileged \
  -e ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY \
  -v "$(pwd)/example":/workspace \
  -v "$(pwd)/example/my-code":/skills/my-code \
  -it wddawson/hotmemory:dev bash
```

Inside the container:

```bash
claude --version                          # Claude Code is available
perf stat echo ok                         # perf works
cd /workspace && make profile && make run  # two [WSS] lines with hot MB + FLOPs
```

---

## Tag and push

```bash
docker buildx build --platform linux/amd64 \
  -t wddawson/hotmemory:latest \
  -t wddawson/hotmemory:$(git rev-parse --short HEAD) \
  --push .
```

---

## Extending the image

Users whose code needs additional libraries write a small Dockerfile on top:

```dockerfile
FROM wddawson/hotmemory:latest
RUN apt-get update && apt-get install -y libfftw3-dev libhdf5-dev \
    && rm -rf /var/lib/apt/lists/*
```

The profiler skill, header, and Claude Code are already present.

---

## Repo layout

```
.
├── Dockerfile                      image definition
├── entrypoint.sh                   copies my-code skill into Claude commands/
├── wss_profiler.h                  C profiling header (also COPYd into image)
├── skills/
│   ├── wss-profiler/
│   │   └── SKILL.md                profiler skill (COPYd into image)
│   └── code-template/
│       └── SKILL.md                template for users to write their code skill
├── example/
│   ├── bench.c                     synthetic MPI+OpenMP benchmark
│   ├── Makefile
│   └── my-code/SKILL.md            filled-in code skill for the example
├── plan.md                         design doc and methodology
├── README.md                       user-facing docs
└── DEVELOPING.md                   this file
```

---

## perf inside containers

`perf` requires `--privileged` and the host kernel must allow perf events:

```bash
sudo sysctl kernel.perf_event_paranoid=-1
```

The entrypoint symlinks the installed linux-tools binary past the
kernel-version wrapper so perf runs correctly on any Linux amd64 host.
