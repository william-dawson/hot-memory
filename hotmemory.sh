#!/bin/bash
#
# hotmemory — wrapper for running the hot-memory profiling container.
#
# Usage:
#   ./hotmemory.sh <code-dir> [skill-dir] [extra singularity args...]
#   ./hotmemory.sh --build          Build the SIF from hotmemory.def
#
# Examples:
#   ./hotmemory.sh ./examples/bench ./examples/bench/my-code
#   ./hotmemory.sh ./examples/cloverleaf ./examples/cloverleaf/my-code
#   ./hotmemory.sh /path/to/my/project /path/to/my/skill
#   ./hotmemory.sh --build
#
# Environment variables (set before running):
#   SINGULARITYENV_AWS_BEARER_TOKEN_BEDROCK  — required
#   SINGULARITYENV_OPENAI_API_KEY            — required
#   HOTMEMORY_SIF                            — path to SIF (default: ./hotmemory.sif)

set -e

SIF="${HOTMEMORY_SIF:-./hotmemory.sif}"

if [ "${1:-}" = "--build" ]; then
    echo "Building $SIF from hotmemory.def..."
    singularity build --fakeroot "$SIF" hotmemory.def
    exit 0
fi

CODE_DIR="${1:?Usage: $0 <code-dir> [skill-dir]}"
SKILL_DIR="${2:-$CODE_DIR}"
shift; shift 2>/dev/null || true

# Resolve to absolute paths
CODE_DIR="$(cd "$CODE_DIR" && pwd)"
SKILL_DIR="$(cd "$SKILL_DIR" && pwd)"

if [ ! -f "$SIF" ]; then
    ARCH="$(uname -m)"
    case "$ARCH" in
        x86_64)  SIF_NAME="hotmemory-amd64.sif" ;;
        aarch64) SIF_NAME="hotmemory-arm64.sif" ;;
        *)       echo "Error: unsupported architecture $ARCH"; exit 1 ;;
    esac
    echo "SIF not found at $SIF — downloading $SIF_NAME from latest release..."
    wget -q --show-progress \
        "https://github.com/william-dawson/hot-memory/releases/latest/download/$SIF_NAME" \
        -O "$SIF"
fi

if [ -z "$SINGULARITYENV_AWS_BEARER_TOKEN_BEDROCK" ]; then
    echo "Error: SINGULARITYENV_AWS_BEARER_TOKEN_BEDROCK is not set"
    exit 1
fi

if [ -z "$SINGULARITYENV_OPENAI_API_KEY" ]; then
    echo "Error: SINGULARITYENV_OPENAI_API_KEY is not set"
    exit 1
fi

exec singularity run --fakeroot --pwd /workspace \
    --bind "$CODE_DIR":/workspace \
    --bind "$SKILL_DIR":/skills/my-code \
    "$@" \
    "$SIF" bash
