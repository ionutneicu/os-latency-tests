/*
 * os_linux.c - Linux implementation of olt/hal/os.h.
 */
#define _GNU_SOURCE
#include "olt/hal/os.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* ---- small helpers ----------------------------------------------------- */

/* Copy with truncation to fit buf (always NUL-terminated). */
static void copy_str(char* buf, size_t len, const char* s)
{
    if (!buf || len == 0) return;
    size_t n = s ? strnlen(s, len - 1) : 0;
    if (n) memcpy(buf, s, n);
    buf[n] = '\0';
}

static void rstrip(char* s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/* Read the first line of a small text file; returns 0 on success. */
static int read_line(const char* path, char* buf, size_t len)
{
    FILE* f = fopen(path, "r");
    if (!f) return errno ? errno : ENOENT;
    int rc = fgets(buf, (int)len, f) ? 0 : EIO;
    fclose(f);
    if (rc == 0) rstrip(buf);
    return rc;
}

/* Read a whole small file (device tree strings may contain NULs). */
static size_t read_all(const char* path, char* buf, size_t len)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

/* Parse "32K", "1024K", "8M" as found in sysfs cache sizes. */
static uint64_t parse_size(const char* s)
{
    char* end = NULL;
    uint64_t v = strtoull(s, &end, 10);
    if (end) {
        if (*end == 'K' || *end == 'k') v <<= 10;
        else if (*end == 'M' || *end == 'm') v <<= 20;
        else if (*end == 'G' || *end == 'g') v <<= 30;
    }
    return v;
}

/* ---- clocks ------------------------------------------------------------ */

const char* olt_os_name(void) { return "linux"; }

uint64_t olt_os_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t olt_os_monotonic_resolution_ns(void)
{
    struct timespec ts;
    if (clock_getres(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- CPUs and scheduling ----------------------------------------------- */

int olt_os_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

int olt_os_allowed_cpus(int* cpus, int max)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return 0;
    int n = 0;
    for (int c = 0; c < CPU_SETSIZE && n < max; ++c)
        if (CPU_ISSET(c, &set)) cpus[n++] = c;
    return n;
}

int olt_os_current_cpu(void) { return sched_getcpu(); }

int olt_os_pin_current_thread(int cpu)
{
    if (cpu < 0 || cpu >= CPU_SETSIZE) return EINVAL;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

int olt_os_set_rt_priority(int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

int olt_os_lock_memory(void)
{
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0 ? 0 : errno;
}

int olt_os_block_deep_idle(void)
{
    /* The PM QoS request lasts while the file stays open, so the descriptor
     * is intentionally kept until the process exits. */
    static int fd = -1;
    if (fd >= 0) return 0;
    fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    int32_t zero = 0;
    if (write(fd, &zero, sizeof(zero)) != (ssize_t)sizeof(zero)) {
        int rc = errno ? errno : EIO;
        close(fd);
        fd = -1;
        return rc;
    }
    return 0;
}

/* ---- memory ------------------------------------------------------------ */

uint64_t olt_os_physical_memory(void)
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long page = sysconf(_SC_PAGESIZE);
    return (pages > 0 && page > 0) ? (uint64_t)pages * (uint64_t)page : 0;
}

void* olt_os_alloc_buffer(size_t size, int huge)
{
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
#if defined(MADV_HUGEPAGE) && defined(MADV_NOHUGEPAGE)
    madvise(p, size, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
#else
    (void)huge;
#endif
    memset(p, 0x5a, size);
    return p;
}

void olt_os_free_buffer(void* p, size_t size)
{
    if (p) munmap(p, size);
}

void olt_os_thp_mode(char* buf, size_t len)
{
    char val[128];
    copy_str(buf, len, "");
    if (read_line("/sys/kernel/mm/transparent_hugepage/enabled", val, sizeof(val)) != 0) return;
    /* format: "always [madvise] never" - the bracketed word is active */
    char* open = strchr(val, '[');
    char* close = open ? strchr(open, ']') : NULL;
    if (open && close) {
        *close = '\0';
        copy_str(buf, len, open + 1);
    }
}

/* ---- platform discovery ------------------------------------------------ */

int olt_os_cache_topology(int cpu, olt_cache_level_t* out, int max)
{
    int n = 0;
    for (int idx = 0; n < max; ++idx) {
        char base[128], path[192], val[128];
        snprintf(base, sizeof(base), "/sys/devices/system/cpu/cpu%d/cache/index%d", cpu, idx);
        snprintf(path, sizeof(path), "%s/level", base);
        if (read_line(path, val, sizeof(val)) != 0) break;

        olt_cache_level_t* c = &out[n];
        memset(c, 0, sizeof(*c));
        c->level = atoi(val);
        snprintf(path, sizeof(path), "%s/type", base);
        if (read_line(path, val, sizeof(val)) == 0) copy_str(c->type, sizeof(c->type), val);
        snprintf(path, sizeof(path), "%s/size", base);
        if (read_line(path, val, sizeof(val)) == 0) c->size_bytes = parse_size(val);
        snprintf(path, sizeof(path), "%s/coherency_line_size", base);
        if (read_line(path, val, sizeof(val)) == 0) c->line_size = atoi(val);
        snprintf(path, sizeof(path), "%s/ways_of_associativity", base);
        if (read_line(path, val, sizeof(val)) == 0) c->ways = atoi(val);
        snprintf(path, sizeof(path), "%s/shared_cpu_list", base);
        if (read_line(path, val, sizeof(val)) == 0) copy_str(c->shared_cpus, sizeof(c->shared_cpus), val);
        ++n;
    }
    return n;
}

void olt_os_cpu_model(char* buf, size_t len)
{
    copy_str(buf, len, "");
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[512], implementer[32] = "", part[32] = "", variant[32] = "";
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char* colon = strchr(line, ':');
        if (!colon) continue;
        char* value = colon + 1;
        while (*value == ' ' || *value == '\t') ++value;
        rstrip(value);
        if (strncmp(line, "model name", 10) == 0) {
            copy_str(buf, len, value);
            found = 1;
            break;
        }
        if (strncmp(line, "CPU implementer", 15) == 0 && !implementer[0]) copy_str(implementer, sizeof(implementer), value);
        else if (strncmp(line, "CPU part", 8) == 0 && !part[0]) copy_str(part, sizeof(part), value);
        else if (strncmp(line, "CPU variant", 11) == 0 && !variant[0]) copy_str(variant, sizeof(variant), value);
    }
    fclose(f);
    /* ARM kernels expose MIDR fields instead of a name; report them raw. */
    if (!found && (implementer[0] || part[0]))
        snprintf(buf, len, "implementer %s part %s variant %s", implementer, part, variant);
}

void olt_os_board_model(char* buf, size_t len)
{
    char tmp[256];
    if (read_all("/sys/firmware/devicetree/base/model", tmp, sizeof(tmp)) > 0) {
        copy_str(buf, len, tmp);
        return;
    }
    char vendor[128] = "", product[128] = "";
    read_line("/sys/class/dmi/id/sys_vendor", vendor, sizeof(vendor));
    read_line("/sys/class/dmi/id/product_name", product, sizeof(product));
    if (vendor[0] || product[0])
        snprintf(buf, len, "%s%s%s", vendor, (vendor[0] && product[0]) ? " " : "", product);
    else
        copy_str(buf, len, "");
}

void olt_os_kernel_release(char* buf, size_t len)
{
    struct utsname u;
    copy_str(buf, len, uname(&u) == 0 ? u.release : "");
}

void olt_os_kernel_version(char* buf, size_t len)
{
    struct utsname u;
    copy_str(buf, len, uname(&u) == 0 ? u.version : "");
}

void olt_os_preempt_model(char* buf, size_t len)
{
    char val[64];
    /* Present and "1" only on PREEMPT_RT kernels. */
    if (read_line("/sys/kernel/realtime", val, sizeof(val)) == 0 && val[0] == '1') {
        copy_str(buf, len, "PREEMPT_RT");
        return;
    }
    struct utsname u;
    if (uname(&u) != 0) { copy_str(buf, len, "unknown"); return; }
    if (strstr(u.version, "PREEMPT_RT")) copy_str(buf, len, "PREEMPT_RT");
    else if (strstr(u.version, "PREEMPT_DYNAMIC")) copy_str(buf, len, "PREEMPT_DYNAMIC");
    else if (strstr(u.version, "PREEMPT")) copy_str(buf, len, "PREEMPT");
    else copy_str(buf, len, "VOLUNTARY/NONE");
}

void olt_os_cpu_governor(int cpu, char* buf, size_t len)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
    if (read_line(path, buf, len) != 0) copy_str(buf, len, "");
}

void olt_os_cpuidle_driver(char* buf, size_t len)
{
    if (read_line("/sys/devices/system/cpu/cpuidle/current_driver", buf, len) != 0) copy_str(buf, len, "");
}

void olt_os_isolated_cpus(char* buf, size_t len)
{
    if (read_line("/sys/devices/system/cpu/isolated", buf, len) != 0) copy_str(buf, len, "");
}
