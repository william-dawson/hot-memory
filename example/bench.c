/*
 * example/bench.c — Synthetic MPI+OpenMP benchmark for wss_profiler.h.
 *
 * Two kernels at opposite ends of the memory-vs-compute spectrum:
 *   - stream_kernel: touches a large array with minimal compute
 *                    (expected: high hot MB, low FLOP/byte)
 *   - compute_kernel: operates on a small array with repeated arithmetic
 *                    (expected: low hot MB, high FLOP/byte)
 *
 * The problem is distributed across MPI ranks (weak scaling): each rank
 * works on STREAM_N / nprocs elements for stream_kernel and COMPUTE_N
 * for compute_kernel, with an MPI_Allreduce after each kernel to
 * synchronise and add realistic communication overhead.
 *
 * Build (inside the profiler container):
 *   mpicc -O2 -fopenmp -DPROFILE_WSS bench.c -o bench -lpapi
 *
 * Build without profiling (macros compile away):
 *   mpicc -O2 -fopenmp bench.c -o bench
 *
 * Run:
 *   mpirun -np 4 ./bench
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <omp.h>
#include "wss_profiler.h"

#define STREAM_N_TOTAL (16 * 1024 * 1024L)   /* 16M doubles total = 128 MB per array */
#define COMPUTE_N      (4 * 1024 * 1024)     /* 4M doubles per rank = 32 MB */
#define COMPUTE_ITERS  100                    /* fewer iters to keep runtime short */

/* Memory-bandwidth-bound: sweeps local portion of array once. */
static void stream_kernel(double *a, double *b, double *c, long n)
{
    #pragma omp parallel for schedule(static)
    for (long i = 0; i < n; i++)
        c[i] = a[i] + b[i];
}

/*
 * Compute-bound: small array, many arithmetic ops per element.
 * Each element does COMPUTE_ITERS multiply-adds (no memory traffic
 * after first touch).
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

    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    /* Distribute stream arrays across ranks (strong scaling) */
    long local_stream_n = STREAM_N_TOTAL / nprocs;

    double *a = malloc(local_stream_n * sizeof(double));
    double *b = malloc(local_stream_n * sizeof(double));
    double *c = malloc(local_stream_n * sizeof(double));
    double *x = malloc(COMPUTE_N * sizeof(double));

    if (!a || !b || !c || !x) {
        fprintf(stderr, "rank %d: malloc failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* Initialise (touch pages so they're mapped) */
    for (long i = 0; i < local_stream_n; i++) {
        a[i] = 1.0;
        b[i] = 2.0;
        c[i] = 0.0;
    }
    for (long i = 0; i < COMPUTE_N; i++) x[i] = (double)(i + rank);

    /* ── Phase 2 instrumentation ── */

    MPI_Barrier(MPI_COMM_WORLD);

    WSS_BEGIN();
    stream_kernel(a, b, c, local_stream_n);
    WSS_END("stream_kernel");

    /* Allreduce to add real MPI communication */
    double local_sum = 0.0;
    for (long i = 0; i < 8; i++) local_sum += c[i];
    double global_sum = 0.0;
    MPI_Allreduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);

    WSS_BEGIN();
    compute_kernel(x, COMPUTE_N, COMPUTE_ITERS);
    WSS_END("compute_kernel");

    /* Allreduce after compute */
    local_sum = 0.0;
    for (long i = 0; i < 8; i++) local_sum += x[i];
    MPI_Allreduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);

    /*
     * Expected output (rank 0, approximate, with -np 4):
     *
     * [WSS] stream_kernel      ~96.0 MB hot   ~0.000 GFLOP   ~0.00 FLOP/byte
     *   (3 arrays × 32 MB/rank = 96 MB; 1 FLOP per 3 reads ≈ tiny)
     *   Hot MB scales down with more ranks: ~48 MB at -np 8, etc.
     *
     * [WSS] compute_kernel     ~32.0 MB hot   ~3.2 GFLOP    ~100 FLOP/byte
     *   (4M doubles × 100 iters × 2 FLOPs = 800M FLOPs; 32 MB working set)
     *   Hot MB stays constant regardless of rank count.
     *
     * At -np 4 the split is roughly 75/25 (stream vs compute),
     * while peak allocation is ~128 MB. This demonstrates that
     * individual kernels use less memory than total allocation.
     *
     * Note: FLOP counts depend on PAPI availability and OpenMP thread
     * counting (main thread only). Hot MB includes 4 KB page rounding
     * plus a few MB of smaps noise (stack, libs).
     */

    if (rank == 0) {
        printf("checksum: %g\n", global_sum);
    }

    free(a); free(b); free(c); free(x);
    MPI_Finalize();
    return 0;
}
