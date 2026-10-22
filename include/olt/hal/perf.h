/*
 * olt/hal/perf.h - optional hardware/software performance counters.
 *
 * Counters explain timings (cache/TLB/branch misses, context switches) but
 * are never required: availability differs widely between CPUs, kernels,
 * VMs and permission settings (perf_event_paranoid). Callers must treat every
 * event as possibly unavailable.
 *
 * Backends, selected at configure time with -DOLT_WITH_PERF=AUTO|ON|OFF:
 *  - src/hal/linux/perf_linux.c: perf_event_open() with the kernel's generic
 *    hardware/cache events plus software events.
 *  - src/hal/stub/perf_stub.c: no counters, every event unavailable.
 * Counters are meant to be read around a batch of samples, not per sample.
 */
#ifndef OLT_HAL_PERF_H
#define OLT_HAL_PERF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum olt_perf_event {
    OLT_PERF_CYCLES = 0,
    OLT_PERF_INSTRUCTIONS,
    OLT_PERF_CACHE_REFERENCES,
    OLT_PERF_CACHE_MISSES,
    OLT_PERF_BRANCH_MISSES,
    OLT_PERF_L1D_READ_MISSES,
    OLT_PERF_LLC_READ_MISSES,
    OLT_PERF_DTLB_READ_MISSES,
    OLT_PERF_CONTEXT_SWITCHES, /* software events: usually available even without a PMU */
    OLT_PERF_CPU_MIGRATIONS,
    OLT_PERF_PAGE_FAULTS,
    OLT_PERF_EVENT_COUNT
} olt_perf_event_t;

typedef struct olt_perf_group olt_perf_group_t;

/* Backend compiled in: "linux-perf_event" or "none". */
const char* olt_perf_backend(void);
const char* olt_perf_event_name(olt_perf_event_t ev);

/* Open counters for the calling thread. Returns NULL if none could be opened. */
olt_perf_group_t* olt_perf_open(const olt_perf_event_t* events, int count);
/* Non-zero if the given event was opened successfully in this group. */
int  olt_perf_has(const olt_perf_group_t* g, olt_perf_event_t ev);
int  olt_perf_start(olt_perf_group_t* g);
int  olt_perf_stop(olt_perf_group_t* g);
/* values is indexed by olt_perf_event_t; unavailable events are left untouched. */
int  olt_perf_read(olt_perf_group_t* g, uint64_t values[OLT_PERF_EVENT_COUNT]);
void olt_perf_close(olt_perf_group_t* g);

#ifdef __cplusplus
}
#endif

#endif /* OLT_HAL_PERF_H */
