/*
 * wss_profiler.h — Per-kernel working set size (WSS), FLOP, and memory
 *                  traffic profiler.
 *
 * Usage:
 *   1. #include "wss_profiler.h" in any file where you call WSS_* macros.
 *   2. Call WSS_INIT() once after MPI_Init().
 *   3. Wrap each kernel call with WSS_BEGIN() / WSS_END("kernel_name").
 *   4. Build with: -DPROFILE_WSS -lwss_profiler -lpapi
 *      (Without -DPROFILE_WSS, all macros compile away to nothing.)
 *      The header uses extern declarations; the definitions are in
 *      wss_profiler.c, compiled into libwss_profiler.a in the container.
 *
 * Output (to stderr, rank 0 only):
 *   [WSS] kernel_name     512.0 MB hot   1024.0 MB accessed   0.480 GFLOP   0.98 FLOP/byte-hot   0.47 FLOP/byte-accessed
 *
 *   "MB accessed" and "FLOP/byte-accessed" are only reported when PAPI
 *   load/store counters are available. Otherwise they show as 0.
 *
 * FP counting:
 *   PAPI is tried first. On aarch64 machines where PAPI's perf_event
 *   component cannot initialize (e.g. libpfm4 doesn't know the CPU),
 *   the profiler falls back to perf_event_open with raw ARM PMU event
 *   0x74 (FP_FIXED_OPS_SPEC). This covers fixed-width FP ops (scalar +
 *   NEON/ASIMD). SVE variable-width FP ops (event 0x75) are NOT counted
 *   as their element count depends on vector length.
 *
 * Requirements:
 *   - Linux amd64 or aarch64 (uses /proc/self/clear_refs and /proc/self/smaps)
 *   - PAPI (libpapi-dev) for FP and load/store counting (optional: degrades
 *     gracefully; on aarch64 FP falls back to perf_event_open)
 *   - MPI (for rank filtering; works with or without OpenMP)
 *   - CAP_SYS_RESOURCE or root to write clear_refs (run container --privileged
 *     or --fakeroot)
 */

#pragma once
#ifndef WSS_PROFILER_H
#define WSS_PROFILER_H

#ifdef PROFILE_WSS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <mpi.h>
#include <papi.h>

/* ── internal state ─────────────────────────────────────────────────────── */
/*
 * Declared extern so all translation units share one copy.
 * The definitions live in wss_profiler.c, compiled into libwss_profiler.a.
 * Link with: -lwss_profiler -lpapi
 */
extern int    _wss_rank;
extern int    _wss_eventset;
extern int    _wss_nfp_events;    /* how many PAPI FP counters are live */
extern int    _wss_nmem_events;   /* how many PAPI load/store counters are live */
extern int    _wss_nevents;       /* total PAPI counters: FP + mem */
extern int    _wss_papi_ok;       /* PAPI library initialized */
extern int    _wss_active;        /* 1 between WSS_BEGIN and WSS_END */
extern int    _wss_fp_fd;         /* perf_event_open fd for FP fallback (-1 = unused) */

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

    /* ── FP counters ── */
    /* Prefer separate DP/SP counters; fall back to combined FP counter. */
    int has_dp = (PAPI_add_event(_wss_eventset, PAPI_DP_OPS) == PAPI_OK);
    int has_sp = (PAPI_add_event(_wss_eventset, PAPI_SP_OPS) == PAPI_OK);

    if (!has_dp && !has_sp) {
        if (PAPI_add_event(_wss_eventset, PAPI_FP_OPS) == PAPI_OK) {
            _wss_nfp_events = 1;
            fprintf(stderr,
                    "[WSS] Using PAPI_FP_OPS (DP_OPS/SP_OPS unavailable)\n");
        } else {
            fprintf(stderr,
                    "[WSS] PAPI: no FP events available\n");
        }
    } else {
        _wss_nfp_events = has_dp + has_sp;
    }

    /* ── Load/Store counters for total bytes accessed ── */
    int has_ld = (PAPI_add_event(_wss_eventset, PAPI_LD_INS) == PAPI_OK);
    int has_sr = (PAPI_add_event(_wss_eventset, PAPI_SR_INS) == PAPI_OK);
    _wss_nmem_events = has_ld + has_sr;

    if (_wss_nmem_events == 0) {
        fprintf(stderr,
                "[WSS] No load/store PAPI events — total bytes accessed will be 0\n");
    }

    _wss_nevents = _wss_nfp_events + _wss_nmem_events;

    if (_wss_nevents == 0) {
        PAPI_destroy_eventset(&_wss_eventset);
        _wss_eventset = PAPI_NULL;
    }
}

/*
 * _wss_perf_fp_init() — open a perf_event_open fd for FP counting.
 *
 * Called after _wss_papi_init(). Only activates when PAPI provided no FP
 * events (_wss_nfp_events == 0). On non-aarch64 this is a no-op — those
 * platforms should have working PAPI FP events.
 *
 * Event 0x74 = FP_FIXED_OPS_SPEC: fixed-width FP ops speculatively executed
 * (scalar + NEON/ASIMD). Defined in the ARM PMU v3 architecture spec and
 * available on chips where libpfm4 may not yet have a PMU entry.
 */
static inline void _wss_perf_fp_init(void)
{
#ifdef __aarch64__
    if (_wss_nfp_events > 0)
        return;  /* PAPI already has FP covered */

    struct perf_event_attr pe = {0};
    pe.size           = sizeof(struct perf_event_attr);
    pe.type           = PERF_TYPE_RAW;
    pe.config         = 0x74;  /* FP_FIXED_OPS_SPEC */
    pe.disabled       = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    pe.inherit        = 0;

    _wss_fp_fd = (int)syscall(__NR_perf_event_open, &pe,
                               0 /*pid: self*/, -1 /*cpu: any*/,
                               -1 /*group*/, 0 /*flags*/);
    if (_wss_fp_fd < 0) {
        fprintf(stderr, "[WSS] perf_event_open(FP_FIXED_OPS_SPEC) failed: %m"
                        " — FLOPs will be 0\n");
    } else {
        fprintf(stderr, "[WSS] Using perf_event_open for FP counting"
                        " (aarch64 fallback, event 0x74)\n");
    }
#endif
}

static inline void _wss_clear_refs(void)
{
    FILE *f = fopen("/proc/self/clear_refs", "w");
    if (!f) {
        fprintf(stderr, "[WSS] cannot open /proc/self/clear_refs: %m\n"
                        "[WSS] need --privileged or CAP_SYS_RESOURCE\n");
        return;
    }
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
 * Identifies rank 0, initialises PAPI (and perf fallback if needed),
 * prints a startup message.
 */
#define WSS_INIT() \
    do { \
        MPI_Comm_rank(MPI_COMM_WORLD, &_wss_rank); \
        if (_wss_rank == 0) { \
            _wss_papi_init(); \
            _wss_perf_fp_init(); \
            fprintf(stderr, "[WSS] Profiling active on rank 0\n"); \
        } \
    } while (0)

/*
 * WSS_BEGIN() — place immediately before the kernel call.
 * Clears the Referenced bits and starts counters (PAPI and/or perf).
 */
#define WSS_BEGIN() \
    do { \
        if (_wss_rank == 0) { \
            if (_wss_active) { \
                fprintf(stderr, \
                        "[WSS] ERROR: WSS_BEGIN() called while already active" \
                        " — nesting is not supported. End the current" \
                        " measurement with WSS_END() before starting a" \
                        " new one.\n"); \
            } else { \
                _wss_active = 1; \
                _wss_clear_refs(); \
                if (_wss_eventset != PAPI_NULL) \
                    PAPI_start(_wss_eventset); \
                if (_wss_fp_fd >= 0) { \
                    ioctl(_wss_fp_fd, PERF_EVENT_IOC_RESET, 0); \
                    ioctl(_wss_fp_fd, PERF_EVENT_IOC_ENABLE, 0); \
                } \
            } \
        } \
    } while (0)

/*
 * WSS_END(name) — place immediately after the kernel call.
 * Stops counters, reads /proc/self/smaps, prints the report line.
 *
 * name: string literal identifying this kernel in the output.
 *
 * Counter layout in _wss_vals[]:
 *   [0..nfp-1]  = FP counters (DP_OPS, SP_OPS, or FP_OPS)
 *   [nfp..nfp+nmem-1] = memory counters (LD_INS, SR_INS)
 */
#define WSS_END(name) \
    do { \
        if (_wss_rank == 0) { \
            if (!_wss_active) { \
                fprintf(stderr, \
                        "[WSS] ERROR: WSS_END(\"%s\") called without a" \
                        " matching WSS_BEGIN().\n", (name)); \
            } else { \
            _wss_active = 0; \
            long long _wss_vals[8] = {0}; \
            if (_wss_eventset != PAPI_NULL) \
                PAPI_stop(_wss_eventset, _wss_vals); \
            long long _wss_perf_fp = 0; \
            if (_wss_fp_fd >= 0) { \
                ioctl(_wss_fp_fd, PERF_EVENT_IOC_DISABLE, 0); \
                read(_wss_fp_fd, &_wss_perf_fp, sizeof(long long)); \
            } \
            long long _wss_ref_kb  = _wss_read_referenced_kb(); \
            long long _wss_flops   = _wss_perf_fp; \
            for (int _i = 0; _i < _wss_nfp_events; _i++) \
                _wss_flops += _wss_vals[_i]; \
            long long _wss_memops  = 0; \
            for (int _i = _wss_nfp_events; \
                 _i < _wss_nfp_events + _wss_nmem_events; _i++) \
                _wss_memops += _wss_vals[_i]; \
            double _wss_hot_mb     = (_wss_ref_kb >= 0) \
                                     ? _wss_ref_kb / 1024.0 : 0.0; \
            double _wss_gflop      = _wss_flops / 1.0e9; \
            /* Estimate total bytes accessed: each LD/SR is ~8 bytes (double) */ \
            double _wss_accessed_mb = (_wss_memops * 8.0) / (1024.0 * 1024.0); \
            double _wss_fpb_hot    = (_wss_ref_kb > 0) \
                                     ? (double)_wss_flops \
                                       / ((double)_wss_ref_kb * 1024.0) \
                                     : 0.0; \
            double _wss_fpb_acc    = (_wss_accessed_mb > 0.0) \
                                     ? _wss_gflop * 1024.0 / _wss_accessed_mb \
                                     : 0.0; \
            fprintf(stderr, \
                    "[WSS] %-24s %8.1f MB hot  %8.1f MB accessed" \
                    "  %8.3f GFLOP  %6.2f FLOP/B-hot  %6.2f FLOP/B-acc\n", \
                    (name), _wss_hot_mb, _wss_accessed_mb, \
                    _wss_gflop, _wss_fpb_hot, _wss_fpb_acc); \
            } \
        } \
    } while (0)

#else /* ── PROFILE_WSS not defined: compile everything away ─────────────── */

#define WSS_INIT()      do {} while (0)
#define WSS_BEGIN()     do {} while (0)
#define WSS_END(name)   do {} while (0)

#endif /* PROFILE_WSS */
#endif /* WSS_PROFILER_H */
