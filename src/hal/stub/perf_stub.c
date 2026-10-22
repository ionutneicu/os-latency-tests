/*
 * perf_stub.c - placeholder for olt/hal/perf.h: no counters available.
 */
#include "olt/hal/perf.h"

#include <errno.h>
#include <stddef.h>

static const char* const k_names[OLT_PERF_EVENT_COUNT] = {
    "cycles", "instructions", "cache_references", "cache_misses", "branch_misses",
    "l1d_read_misses", "llc_read_misses", "dtlb_read_misses",
    "context_switches", "cpu_migrations", "page_faults",
};

const char* olt_perf_backend(void) { return "none"; }

const char* olt_perf_event_name(olt_perf_event_t ev)
{
    return (ev >= 0 && ev < OLT_PERF_EVENT_COUNT) ? k_names[ev] : "unknown";
}

olt_perf_group_t* olt_perf_open(const olt_perf_event_t* events, int count)
{
    (void)events;
    (void)count;
    return NULL;
}

int olt_perf_has(const olt_perf_group_t* g, olt_perf_event_t ev) { (void)g; (void)ev; return 0; }
int olt_perf_start(olt_perf_group_t* g) { (void)g; return ENOSYS; }
int olt_perf_stop(olt_perf_group_t* g) { (void)g; return ENOSYS; }
int olt_perf_read(olt_perf_group_t* g, uint64_t values[OLT_PERF_EVENT_COUNT]) { (void)g; (void)values; return ENOSYS; }
void olt_perf_close(olt_perf_group_t* g) { (void)g; }
