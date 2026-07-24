#!/bin/sh
set -eu

PLUGIN_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$PLUGIN_ROOT/../.." && pwd)
SIF=${HOTMEMORY_SIF:-"$REPO_ROOT/hotmemory.sif"}
RUNTIME=${HOTMEMORY_RUNTIME:-singularity}

if ! command -v "$RUNTIME" >/dev/null 2>&1; then
    echo "Hot Memory: runtime '$RUNTIME' was not found; install Singularity/Apptainer or set HOTMEMORY_RUNTIME." >&2
    exit 1
fi

if [ ! -f "$SIF" ]; then
    echo "Hot Memory: SIF not found at '$SIF'; set HOTMEMORY_SIF or build hotmemory.sif first." >&2
    exit 1
fi

# The MCP process runs inside the image so its commands resolve to the bundled
# PAPI/perf/WSS toolchain. Mount the caller's project at the same path expected
# by the existing server workflow.
PROJECT_DIR=$(pwd)

exec "$RUNTIME" exec --fakeroot --pwd /workspace \
    --bind "$PROJECT_DIR:/workspace" \
    "$SIF" node /usr/local/lib/wss-mcp/server.js "$@"
