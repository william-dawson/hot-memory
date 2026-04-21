/*
 * wss_profiler.c — Global variable definitions for wss_profiler.h.
 *
 * Compiled once into libwss_profiler.a so that all translation units
 * that include wss_profiler.h share a single copy of the profiler state.
 * Without this, each .c/.cc file that includes the header gets its own
 * static copy, and WSS_INIT() in main never initialises the _wss_rank
 * seen by WSS_BEGIN() in other files.
 */

#ifdef PROFILE_WSS

#include <papi.h>

int _wss_rank        = -1;
int _wss_eventset    = PAPI_NULL;  /* relies on PAPI_NULL == -1 */
int _wss_nfp_events  = 0;
int _wss_nmem_events = 0;
int _wss_nevents     = 0;
int _wss_papi_ok     = 0;
int _wss_active      = 0;

#endif
