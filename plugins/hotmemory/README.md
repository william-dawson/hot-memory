# Hot Memory plugin

This plugin is bundled into the Hot Memory SIF. When the user starts Claude
inside the container, the image activates this skill and registers the MCP
server automatically.

The SIF remains responsible for the native profiler runtime: MPI, PAPI, perf,
and the WSS library. The plugin provides the agent-facing workflow.

The host-side `run_mcp.sh` launcher is only a development convenience. The
normal workflow is to enter the container with `hotmemory.sh` and run `claude`.
