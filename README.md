# hot-memory

The point of this repository is to be kind of an example of what you might ask an outsourcing company to produce in terms of AI agent knowhow for doing performance modelling. We're using a simple test case right here. We assume the user has an MPI/OpenMP HPC code. They are wondering:

1. **Where is time going?**
2. **For a given kernel: how many unique bytes are hot, and how many FLOPs does it execute?**

The idea here is to model what happens if you try to accelerate the code on a small memory GPU (like a consumer RTX). First, you need to know what your main kernels are. But second, you know you can't just dump everything on the GPU side for fear of running out of memory. So for each kernel we figure out its memory use to FLOP ratio. Then we can imagine doing some modelling to say "if the GPU is X fast, I could imagine swapping memory between phases."

---

## Quickstart

Pull the image (requires a Linux amd64 host — perf and PAPI hardware counters do not work on Docker Desktop for Mac)

```bash
docker pull wddawson/hotmemory:latest
```

To me this is critical. There should be a container for all the development to live in. And inside this container is everything you need to work with the AI agent.

Next write your code skill

Copy the template and fill it in:

```bash
cp -r skills/code-template my-code-skill
$EDITOR my-code-skill/SKILL.md
```

The template asks for your source layout, build command, run command, and any notes about which functions are the hot kernels. The user then switches into the mindset of writing a skill file about how to use their code, so that future agents can do their thing.

Now run the container, passing your Anthropic API key from the host environment:

```bash
docker run --privileged \
  -e ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY \
  -v /path/to/your/code:/workspace \
  -v /path/to/my-code-skill:/skills/my-code \
  -it wddawson/hotmemory:latest bash
```

> **Host requirement (if perf returns "Permission denied"):**
> ```bash
> sudo sysctl kernel.perf_event_paranoid=-1
> ```

Then you can start Claude Code inside the container

```bash
claude
```

And ask:

- *"Find the hotspots in my code."* → Claude reads both skills, runs `perf`, reports top functions by % wall-clock time.
- *"Measure the working set of stencil_apply."* → Claude copies the header, adds macros, rebuilds, runs, reports hot MB + FLOP/byte.
- *"Will this fit on an RTX5070?"* → Claude uses the measured hot sets to reason about GPU memory.

---

## How it works

The key point here is that the company should provide inside the container both some software and SKILL files. The SKILL files outline general procedures for preparing data if needed, or using premade tools. Then there is a skill to use whatever custom software they made.

```
/skills/
  wss-profiler/       ← baked into the image
    SKILL.md              teaches Claude how to profile and interpret results
    wss_profiler.h        C header Claude copies into the user's source tree
  my-code/            ← mounted by you at runtime
    SKILL.md              teaches Claude how to build and run your specific code
```

As for how this project works, that information should be in the SKILL. You should hop into the container, and ask Claude code to explain it to you. The SKILLS should describe the methodology, limitations, next steps, etc.

---

## Try it with the built-in example

The `example/` directory contains a synthetic benchmark and a fully filled-in
code skill so you can try the whole flow immediately without writing any code.

```bash
docker run --privileged \
  -e ANTHROPIC_API_KEY=$ANTHROPIC_API_KEY \
  -v "$(pwd)/example":/workspace \
  -v "$(pwd)/example/my-code":/skills/my-code \
  -it wddawson/hotmemory:latest bash
```

Then inside the container:

```bash
claude
```

Try asking:
- *"Find the hotspots in this code."*
- *"Measure the working set of both kernels."*
- *"Would this fit on a GPU with 4 GB of memory?"*

The example has two kernels that sit at opposite ends of the spectrum on
purpose — `stream_kernel` is memory-bound (~768 MB hot, near-zero FLOP/byte)
and `compute_kernel` is compute-bound (~2 MB hot, ~128 FLOP/byte) — so the
profiler output is easy to interpret and serves as a sanity check that
everything is working.
