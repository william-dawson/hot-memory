#!/bin/bash
set -e

DEST="${1:-/workspace/qws}"

if [ -d "$DEST" ]; then
    echo "QWS already exists at $DEST"
    exit 0
fi

echo "Fetching QWS..."
git clone --depth 1 https://github.com/RIKEN-LQCD/qws.git "$DEST"

echo "Building QWS..."
cd "$DEST"
make -j "$(grep -c processor /proc/cpuinfo)" fugaku_benchmark= omp=1 compiler=openmpi-gnu arch=thunderx2 rdma= mpi=1 powerapi=

echo ""
echo "QWS built successfully at $DEST"
echo "Run with: cd $DEST && mpirun -np 1 ./main 32 6 4 3 1 1 1 1 -1 -1 6 50"
