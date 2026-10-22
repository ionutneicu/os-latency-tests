/*
 * perf_linux.c - olt/hal/perf.h on top of Linux perf_event_open().
 *
 * Each event is opened as an independent counter so that a missing event
 * (no PMU in a VM, event not supported by the CPU, perf_event_paranoid)
 * disables only that event. Values are scaled by time_enabled/time_running
 * when the kernel multiplexes counters.
 */
#define _GNU_SOURCE
#include "olt/hal/perf.h"

#include <errno.h>
#include <linux/perf_event.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

struct olt_perf_group {
    int fd[OLT_PERF_EVENT_COUNT];
};

static const char* const k_names[OLT_PERF_EVENT_COUNT] = {
    "cycles", "instructions", "cache_references", "cache_misses", "branch_misses",
    "l1d_read_misses", "llc_read_misses", "dtlb_read_misses",
    "context_switches", "cpu_migrations", "page_faults",
};

const char* olt_perf_backend(void) { return "linux-perf_event"; }

const char* olt_perf_event_name(olt_perf_event_t ev)
{
    return (ev >= 0 && ev < OLT_PERF_EVENT_COUNT) ? k_names[ev] : "unknown";
}

static uint64_t cache_config(uint64_t cache, uint64_t op, uint64_t result)
{
    return cache | (op << 8) | (result << 16);
}

static int describe(olt_perf_event_t ev, struct perf_event_attr* a)
{
    switch (ev) {
    case OLT_PERF_CYCLES:           a->type = PERF_TYPE_HARDWARE; a->config = PERF_COUNT_HW_CPU_CYCLES; break;
    case OLT_PERF_INSTRUCTIONS:     a->type = PERF_TYPE_HARDWARE; a->config = PERF_COUNT_HW_INSTRUCTIONS; break;
    case OLT_PERF_CACHE_REFERENCES: a->type = PERF_TYPE_HARDWARE; a->config = PERF_COUNT_HW_CACHE_REFERENCES; break;
    case OLT_PERF_CACHE_MISSES:     a->type = PERF_TYPE_HARDWARE; a->config = PERF_COUNT_HW_CACHE_MISSES; break;
    case OLT_PERF_BRANCH_MISSES:    a->type = PERF_TYPE_HARDWARE; a->config = PERF_COUNT_HW_BRANCH_MISSES; break;
    case OLT_PERF_L1D_READ_MISSES:
        a->type = PERF_TYPE_HW_CACHE;
        a->config = cache_config(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS);
        break;
    case OLT_PERF_LLC_READ_MISSES:
        a->type = PERF_TYPE_HW_CACHE;
        a->config = cache_config(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS);
        break;
    case OLT_PERF_DTLB_READ_MISSES:
        a->type = PERF_TYPE_HW_CACHE;
        a->config = cache_config(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS);
        break;
    case OLT_PERF_CONTEXT_SWITCHES: a->type = PERF_TYPE_SOFTWARE; a->config = PERF_COUNT_SW_CONTEXT_SWITCHES; break;
    case OLT_PERF_CPU_MIGRATIONS:   a->type = PERF_TYPE_SOFTWARE; a->config = PERF_COUNT_SW_CPU_MIGRATIONS; break;
    case OLT_PERF_PAGE_FAULTS:      a->type = PERF_TYPE_SOFTWARE; a->config = PERF_COUNT_SW_PAGE_FAULTS; break;
    default: return EINVAL;
    }
    return 0;
}

static int open_event(olt_perf_event_t ev)
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    if (describe(ev, &a) != 0) return -1;
    a.disabled = 1;
    a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    int fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    if (fd < 0 && (errno == EACCES || errno == EPERM)) {
        /* perf_event_paranoid >= 2 only allows user-space counting */
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    }
    return fd;
}

olt_perf_group_t* olt_perf_open(const olt_perf_event_t* events, int count)
{
    olt_perf_group_t* g = (olt_perf_group_t*)malloc(sizeof(*g));
    if (!g) return NULL;
    int opened = 0;
    for (int i = 0; i < OLT_PERF_EVENT_COUNT; ++i) g->fd[i] = -1;
    for (int i = 0; i < count; ++i) {
        olt_perf_event_t ev = events[i];
        if (ev < 0 || ev >= OLT_PERF_EVENT_COUNT || g->fd[ev] >= 0) continue;
        g->fd[ev] = open_event(ev);
        if (g->fd[ev] >= 0) ++opened;
    }
    if (opened == 0) {
        free(g);
        return NULL;
    }
    return g;
}

int olt_perf_has(const olt_perf_group_t* g, olt_perf_event_t ev)
{
    return g && ev >= 0 && ev < OLT_PERF_EVENT_COUNT && g->fd[ev] >= 0;
}

int olt_perf_start(olt_perf_group_t* g)
{
    if (!g) return EINVAL;
    for (int i = 0; i < OLT_PERF_EVENT_COUNT; ++i) {
        if (g->fd[i] < 0) continue;
        ioctl(g->fd[i], PERF_EVENT_IOC_RESET, 0);
        ioctl(g->fd[i], PERF_EVENT_IOC_ENABLE, 0);
    }
    return 0;
}

int olt_perf_stop(olt_perf_group_t* g)
{
    if (!g) return EINVAL;
    for (int i = 0; i < OLT_PERF_EVENT_COUNT; ++i)
        if (g->fd[i] >= 0) ioctl(g->fd[i], PERF_EVENT_IOC_DISABLE, 0);
    return 0;
}

int olt_perf_read(olt_perf_group_t* g, uint64_t values[OLT_PERF_EVENT_COUNT])
{
    if (!g) return EINVAL;
    for (int i = 0; i < OLT_PERF_EVENT_COUNT; ++i) {
        if (g->fd[i] < 0) continue;
        uint64_t buf[3] = {0, 0, 0}; /* value, time_enabled, time_running */
        if (read(g->fd[i], buf, sizeof(buf)) != (ssize_t)sizeof(buf)) continue;
        if (buf[2] != 0 && buf[2] < buf[1])
            values[i] = (uint64_t)((double)buf[0] * (double)buf[1] / (double)buf[2]);
        else
            values[i] = buf[0];
    }
    return 0;
}

void olt_perf_close(olt_perf_group_t* g)
{
    if (!g) return;
    for (int i = 0; i < OLT_PERF_EVENT_COUNT; ++i)
        if (g->fd[i] >= 0) close(g->fd[i]);
    free(g);
}
