# Hot Memory plugin

This plugin exposes the repository's WSS profiling MCP server and workflow skill
to Claude Code and Codex. The profiler runtime remains in the Hot Memory SIF;
the plugin is the installable agent-facing layer.

This repository-local development version expects the checkout to contain
`wss_mcp/server.js`. A future standalone release should package the MCP server
and pin the native runtime/SIF separately.

Set `HOTMEMORY_SIF` when the image is not `./hotmemory.sif`, or set
`HOTMEMORY_RUNTIME=apptainer` when using Apptainer. The MCP server is launched
inside the image so PAPI, perf, MPI, and the WSS probe are available.
