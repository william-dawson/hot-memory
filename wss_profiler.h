/*
 * wss_profiler.h — Per-kernel working set size (WSS) and FLOP profiler.
 *
 * Usage:
 *   1. Copy this header into your source directory.
 *   2. #include "wss_profiler.h" in the file containing main().
 *   3. Call WSS_INIT() once after MPI_Init().
 *   4. Wrap each kernel call with WSS_BEGIN() / WSS_END("kernel_name").
 *   5. Build with: -DPROFILE_WSS -lpapi
 *      (Without -DPROFILE_WSS, all macros compile away to nothing.)
 *
 * Output (to stderr, rank 0 only):
 *   [WSS] kernel_name       512.0 MB hot     0.480 GFLOP     0.98 FLOP/byte
 *
 * Requirements:
 *   - Linux (uses /proc/self/clear_refs and /proc/self/smaps)
 *   - PAPI (libpapi-dev)
 *   - MPI (for rank filtering; works with or without OpenMP)
 *   - CAP_SYS_RESOURCE or root to write clear_refs (run container --privileged)
 */

#pragma once
#ifndef WSS_PROFILER_H
#define WSS_PROFILER_H

#ifdef PROFILE_WSS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <papi.h>

/* ── internal state ─────────────────────────────────────────────────────── */

static int    _wss_rank     = -1;
static int    _wss_eventset = PAPI_NULL;
static int    _wss_nevents  = 0;   /* how many counters are live */
static int    _wss_papi_ok  = 0;   /* PAPI library initialized */

/* ── internal helpers (defined inline to avoid duplicate-symbol errors) ─── */

static inline void _wss_papi_init(void)
{
    int ret = PAPI_library_init(PAPI_VER_CURRENT);
    if (ret != PAPI_VER_CURRENT) {
        fprintf(stderr, "[WSS] PAPI library init failed: %s\n",
                PAPI_strerror(ret));
        return;
    }
    _wss_papi_ok = 1;

    if (PAPI_create_eventset(&_wss_eventset) != PAPI_OK) {
        fprintf(stderr, "[WSS] PAPI_create_eventset failed\n");
        return;
    }

    /* Prefer separate DP/SP counters; fall back to combined FP counter. */
    int has_dp = (PAPI_add_event(_wss_eventset, PAPI_DP_OPS) == PAPI_OK);
    int has_sp = (PAPI_add_event(_wss_eventset, PAPI_SP_OPS) == PAPI_OK);

    if (!has_dp && !has_sp) {
        /* Some microarchitectures only expose PAPI_FP_OPS. */
        if (PAPI_add_event(_wss_eventset, PAPI_FP_OPS) == PAPI_OK) {
            _wss_nevents = 1;
            fprintf(stderr,
                    "[WSS] Using PAPI_FP_OPS (DP_OPS/SP_OPS unavailable)\n");
        } else {
            fprintf(stderr,
                    "[WSS] No FP PAPI events available — FLOPs will be 0\n");
            PAPI_destroy_eventset(&_wss_eventset);
            _wss_eventset = PAPI_NULL;
        }
    } else {
        _wss_nevents = has_dp + has_sp;
    }
}

static inline void _wss_clear_refs(void)
{
    FILE *f = fopen("/proc/self/clear_refs", "w");
    if (!f) {
        fprintf(stderr, "[WSS] cannot open /proc/self/clear_refs: %m\n"
                        "[WSS] need --privileged or CAP_SYS_RESOURCE\n");
        return;
    }
    /* 1 = clear Referenced bits for all pages */
    fputs("1\n", f);
    fclose(f);
}

static inline long long _wss_read_referenced_kb(void)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) {
        fprintf(stderr, "[WSS] cannot open /proc/self/smaps: %m\n");
        return -1;
    }
    char line[256];
    long long total = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Referenced:", 11) == 0) {
            long long kb = 0;
            sscanf(line + 11, "%lld", &kb);
            total += kb;
        }
    }
    fclose(f);
    return total;
}

/* ── public macros ──────────────────────────────────────────────────────── */

/*
 * WSS_INIT() — call once after MPI_Init().
 * Identifies rank 0, initialises PAPI, prints a startup message.
 */
#define WSS_INIT() \
    do { \
        MPI_Comm_rank(MPI_COMM_WORLD, &_wss_rank); \
        if (_wss_rank == 0) { \
            _wss_papi_init(); \
            fprintf(stderr, "[WSS] Profiling active on rank 0\n"); \
        } \
    } while (0)

/*
 * WSS_BEGIN() — place immediately before the kernel call.
 * Clears the Referenced bits and starts PAPI counters.
 */
#define WSS_BEGIN() \
    do { \
        if (_wss_rank == 0) { \
            _wss_clear_refs(); \
            if (_wss_eventset != PAPI_NULL) \
                PAPI_start(_wss_eventset); \
        } \
    } while (0)

/*
 * WSS_END(name) — place immediately after the kernel call.
 * Stops counters, reads /proc/self/smaps, prints the report line.
 *
 * name: string literal identifying this kernel in the output.
 */
#define WSS_END(name) \
    do { \
        if (_wss_rank == 0) { \
            long long _wss_vals[4] = {0, 0, 0, 0}; \
            if (_wss_eventset != PAPI_NULL) \
                PAPI_stop(_wss_eventset, _wss_vals); \
            long long _wss_ref_kb  = _wss_read_referenced_kb(); \
            long long _wss_flops   = 0; \
            for (int _i = 0; _i < _wss_nevents; _i++) \
                _wss_flops += _wss_vals[_i]; \
            double _wss_hot_mb     = (_wss_ref_kb >= 0) \
                                     ? _wss_ref_kb / 1024.0 : 0.0; \
            double _wss_gflop      = _wss_flops / 1.0e9; \
            double _wss_fpb        = (_wss_ref_kb > 0) \
                                     ? (double)_wss_flops \
                                       / ((double)_wss_ref_kb * 1024.0) \
                                     : 0.0; \
            fprintf(stderr, \
                    "[WSS] %-32s %8.1f MB hot  %8.3f GFLOP  %8.2f FLOP/byte\n",\
                    (name), _wss_hot_mb, _wss_gflop, _wss_fpb); \
        } \
    } while (0)

#else /* ── PROFILE_WSS not defined: compile everything away ─────────────── */

#define WSS_INIT()      do {} while (0)
#define WSS_BEGIN()     do {} while (0)
#define WSS_END(name)   do {} while (0)

#endif /* PROFILE_WSS */
#endif /* WSS_PROFILER_H */
