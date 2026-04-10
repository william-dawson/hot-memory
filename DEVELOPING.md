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

## Publishing (CI handles this)

**Docker image** — built and pushed to Docker Hub automatically on every
push to `main` via `.github/workflows/docker.yml`. You need two secrets
set in the GitHub repo:

| Secret | Value |
|--------|-------|
| `DOCKERHUB_USERNAME` | `wddawson` |
| `DOCKERHUB_TOKEN` | Docker Hub access token (not your password) |

**Singularity SIF** — built and attached to a GitHub Release automatically
via `.github/workflows/singularity.yml`. To publish a release:

```bash
gh release create v1.0.0 --title "v1.0.0" --notes "Release notes here"
```

CI will build `hotmemory.sif` from the Docker Hub image and attach it.
HPC users can then download it directly:

```bash
wget https://github.com/william-dawson/hot-memory/releases/latest/download/hotmemory.sif
```

**Manual push** (if you need to bypass CI):
```bash
docker buildx build --platform linux/amd64 \
  -t wddawson/hotmemory:latest --push .
```

---

## Singularity / Apptainer (HPC deployment)

Docker is typically unavailable on HPC clusters. Use the provided
`hotmemory.def` to build a Singularity/Apptainer image from the Docker Hub
image.

### Get the SIF

Download the pre-built SIF from the latest GitHub Release (no Singularity
install needed on your local machine):

```bash
wget https://github.com/william-dawson/hot-memory/releases/latest/download/hotmemory.sif
```

Or build it yourself from the def file:

```bash
singularity build hotmemory.sif hotmemory.def
# or
apptainer build hotmemory.sif hotmemory.def
```

### Run

```bash
export SINGULARITYENV_ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY

singularity exec \
  --bind /path/to/your/code:/workspace \
  --bind /path/to/your/code-skill:/skills/my-code \
  hotmemory.sif bash
```

The `%runscript` copies the baked-in skills and `settings.json` into the
real user's `~/.claude/` on first run (Singularity maps the host `$HOME`
into the container, so `/root/.claude/` from the image is not the user's
home).

### Privileges

`/proc/self/clear_refs` (WSS measurement) and `perf_event_open` require
elevated privileges. Ask your sysadmin to set on the compute nodes:

```bash
sysctl kernel.perf_event_paranoid=-1
```

If you have fakeroot access:

```bash
singularity exec --fakeroot --bind ... hotmemory.sif bash
```

### Extending for your code's dependencies

```singularity
Bootstrap: docker
From: wddawson/hotmemory:latest

%post
    apt-get update && apt-get install -y libfftw3-dev libhdf5-dev \
        && rm -rf /var/lib/apt/lists/*
```

---

## Extending the image (Docker)

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
├── Dockerfile                      Docker image definition
├── hotmemory.def                   Singularity/Apptainer definition file
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
