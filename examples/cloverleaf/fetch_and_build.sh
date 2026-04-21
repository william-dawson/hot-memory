#!/bin/bash
set -e

DEST="${1:-/workspace/CloverLeaf}"

if [ -d "$DEST" ]; then
    echo "CloverLeaf already exists at $DEST"
    exit 0
fi

echo "Fetching CloverLeaf reference version..."
git clone --depth 1 https://github.com/UK-MAC/CloverLeaf_ref.git "$DEST"

echo "Building CloverLeaf..."
cd "$DEST"
make COMPILER=GNU

echo ""
echo "CloverLeaf built successfully at $DEST"
echo "Run with: cd $DEST && mpirun -np 4 ./clover_leaf"
