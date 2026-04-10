/*
 * example/bench.c — Synthetic benchmark that exercises wss_profiler.h.
 *
 * Simulates two kernels:
 *   - stream_kernel: touches a large array with minimal compute
 *                    (expected: high hot MB, low FLOP/byte)
 *   - compute_kernel: operates on a small array with repeated arithmetic
 *                    (expected: low hot MB, high FLOP/byte)
 *
 * Build (inside the profiler container):
 *   mpicc -O2 -fopenmp -DPROFILE_WSS -I.. bench.c -o bench -lpapi
 *
 * Build without profiling (macros compile away):
 *   mpicc -O2 -fopenmp bench.c -o bench
 *
 * Run:
 *   mpirun -np 2 ./bench
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <omp.h>
#include "wss_profiler.h"

#define STREAM_N   (32 * 1024 * 1024)   /* 32M doubles = 256 MB */
#define COMPUTE_N  (256 * 1024)          /* 256K doubles = 2 MB */
#define COMPUTE_ITERS 1000               /* repeat inner loop for FLOP density */

/* Memory-bandwidth-bound: sweeps STREAM_N doubles once. */
static void stream_kernel(double *a, double *b, double *c, long n)
{
    #pragma omp parallel for schedule(static)
    for (long i = 0; i < n; i++)
        c[i] = a[i] + b[i];
}

/*
 * Compute-bound: small array, many arithmetic ops per element.
 * Each element does COMPUTE_ITERS multiplications (no memory traffic after
 * the first touch).
 */
static void compute_kernel(double *x, long n, int iters)
{
    #pragma omp parallel for schedule(static)
    for (long i = 0; i < n; i++) {
        double v = x[i];
        for (int k = 0; k < iters; k++)
            v = v * 1.0000001 + 1e-10;
        x[i] = v;
    }
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    WSS_INIT();

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    /* Allocate arrays */
    double *a = malloc(STREAM_N * sizeof(double));
    double *b = malloc(STREAM_N * sizeof(double));
    double *c = malloc(STREAM_N * sizeof(double));
    double *x = malloc(COMPUTE_N * sizeof(double));

    /* Initialise (touch pages so they're mapped) */
    for (long i = 0; i < STREAM_N; i++) { a[i] = 1.0; b[i] = 2.0; c[i] = 0.0; }
    for (long i = 0; i < COMPUTE_N; i++) x[i] = (double)i;

    /* ── Phase 2 instrumentation ── */

    WSS_BEGIN();
    stream_kernel(a, b, c, STREAM_N);
    WSS_END("stream_kernel");

    WSS_BEGIN();
    compute_kernel(x, COMPUTE_N, COMPUTE_ITERS);
    WSS_END("compute_kernel");

    /* ── Expected output (rank 0, approximate) ──────────────────────────
     *
     * [WSS] stream_kernel                ~768.0 MB hot   ~0.000 GFLOP   ~0.00 FLOP/byte
     *   (3 arrays × 256 MB = 768 MB; addition is 1 FLOP/3 reads ≈ tiny)
     *
     * [WSS] compute_kernel                 ~2.0 MB hot   ~0.256 GFLOP  ~128.0 FLOP/byte
     *   (256 K doubles × 1000 iters ≈ 256 M FLOPs; 2 MB working set)
     *
     * Note: FLOP counts depend on PAPI availability and OpenMP thread
     * counting (main thread only). Hot MB includes 4 KB page rounding
     * plus a few MB of smaps noise (stack, libs).
     * ─────────────────────────────────────────────────────────────────── */

    if (rank == 0) {
        /* Prevent optimiser from discarding the results */
        double sum = 0;
        for (long i = 0; i < 8; i++) sum += c[i] + x[i];
        printf("checksum: %g\n", sum);
    }

    free(a); free(b); free(c); free(x);
    MPI_Finalize();
    return 0;
}
