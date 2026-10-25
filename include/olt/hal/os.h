/*
 * olt/hal/os.h - operating system abstraction layer.
 *
 * Plain C API for everything the harness needs from the OS: clocks, CPU
 * affinity, real-time scheduling, memory locking and platform discovery.
 * The Linux implementation lives in src/hal/linux/os_linux.c. A port to
 * another OS implements the same functions in src/hal/<os>/.
 *
 * Functions returning int return 0 on success or a positive errno-style code.
 * String getters write a NUL-terminated string (possibly empty) into buf.
 */
#ifndef OLT_HAL_OS_H
#define OLT_HAL_OS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char* olt_os_name(void);

/* Monotonic clock shared by all threads and CPUs, in nanoseconds. */
uint64_t olt_os_monotonic_ns(void);
/* Reported resolution of that clock, in nanoseconds. */
uint64_t olt_os_monotonic_resolution_ns(void);
/* CPU time consumed by the calling thread so far, in nanoseconds. */
uint64_t olt_os_thread_cpu_ns(void);

/* Number of online CPUs. */
int olt_os_cpu_count(void);
/* CPUs the process may run on; returns how many were written to cpus. */
int olt_os_allowed_cpus(int* cpus, int max);
/* CPU the calling thread runs on right now, or -1. */
int olt_os_current_cpu(void);
/* Pin the calling thread to a single CPU. */
int olt_os_pin_current_thread(int cpu);
/* Switch the calling thread to a real-time FIFO policy at priority prio. */
int olt_os_set_rt_priority(int prio);
/* Lock current and future pages in RAM (avoids page-fault latency). */
int olt_os_lock_memory(void);
/* What olt_os_limit_idle() applied. */
typedef struct {
    int latency_us;  /* limit in microseconds; 0 allows only polling */
    int per_cpu;     /* 1: only the requested CPUs are limited, 0: all CPUs are */
    char states[96]; /* idle states the limit still allows, e.g. "POLL C1"; "" if unknown */
    int had_limit;   /* 1 if a CPU already had a per-CPU limit, which comes back at exit */
} olt_idle_limit_t;

/* Keep CPUs out of deep idle (C-)states for the rest of the process lifetime,
 * so blocked threads wake up quickly. latency_us < 0 picks the exit latency
 * of the shallowest state that halts the CPU (C1 on x86): idle CPUs still stop
 * instead of spinning. 0 allows only polling, so idle CPUs spin and use up
 * power and SMT resources. The limit goes on cpus[0..n) only when the OS
 * supports per-CPU limits; otherwise, or with n == 0, on all CPUs. Fills *out
 * (may be NULL) with what was applied. Linux: per CPU through
 * /sys/devices/system/cpu/cpuN/power/pm_qos_resume_latency_us (the previous
 * values come back at exit), for all CPUs through /dev/cpu_dma_latency (held
 * open); both usually need root. */
int olt_os_limit_idle(const int* cpus, int n, int latency_us, olt_idle_limit_t* out);

/* If the process runs elevated on behalf of another user (Linux: sudo, via
 * SUDO_UID/SUDO_GID), give ownership of `path` back to that user, so result
 * files stay editable. No-op otherwise. */
int olt_os_restore_owner(const char* path);

/* Physical RAM in bytes, 0 if unknown. */
uint64_t olt_os_physical_memory(void);
/* Memory available for new allocations without swapping, 0 if unknown. */
uint64_t olt_os_memory_available(void);
/* Fraction of time CPUs were busy over a sample_ms window (0..1), system wide
 * in *overall and per CPU index in per_cpu[0..max_cpus) (-1 where unknown).
 * The caller sleeps during the window, so it measures other activity. */
int olt_os_cpu_busy(unsigned sample_ms, double* overall, double* per_cpu, int max_cpus);
/* Non-zero if standard input is an interactive terminal. */
int olt_os_stdin_is_terminal(void);
/* Page-aligned anonymous buffer, every page touched (no first-use faults later).
 * huge != 0 asks for transparent huge pages, huge == 0 forbids them so TLB
 * effects of base pages stay visible. Returns NULL on failure. */
void* olt_os_alloc_buffer(size_t size, int huge);
void  olt_os_free_buffer(void* p, size_t size);
/* Transparent huge page policy, e.g. "always", "madvise", "never", or "". */
void olt_os_thp_mode(char* buf, size_t len);

typedef struct olt_cache_level {
    int      level;        /* 1, 2, 3, ... */
    char     type[16];     /* "Data", "Instruction", "Unified" */
    uint64_t size_bytes;
    int      line_size;    /* bytes, 0 if unknown */
    int      ways;         /* associativity, 0 if unknown */
    char     shared_cpus[64]; /* CPU list sharing this cache, e.g. "0-3" */
} olt_cache_level_t;

/* Cache hierarchy seen by a CPU; returns the number of entries written. */
int olt_os_cache_topology(int cpu, olt_cache_level_t* out, int max);

void olt_os_cpu_model(char* buf, size_t len);      /* CPU name or implementer/part */
void olt_os_board_model(char* buf, size_t len);    /* device-tree / DMI board name */
void olt_os_kernel_release(char* buf, size_t len); /* e.g. "6.8.0-45-generic" */
void olt_os_kernel_version(char* buf, size_t len); /* build string, shows PREEMPT flags */
void olt_os_preempt_model(char* buf, size_t len);  /* "PREEMPT_RT", "PREEMPT_DYNAMIC", "PREEMPT", "VOLUNTARY/NONE" */
void olt_os_cpu_governor(int cpu, char* buf, size_t len);
void olt_os_cpuidle_driver(char* buf, size_t len);   /* e.g. "intel_idle", "psci_idle", "" */
void olt_os_isolated_cpus(char* buf, size_t len);
/* CPU vulnerabilities and their mitigation state, one "name: status" line per
 * entry (Linux: /sys/devices/system/cpu/vulnerabilities), sorted; "" if unknown. */
void olt_os_cpu_vulnerabilities(char* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OLT_HAL_OS_H */
