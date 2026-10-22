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
#include <signal.h>
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

uint64_t olt_os_thread_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
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

/* ---- idle (C-)state limits ---------------------------------------------- */

#define IDLE_MAX_CPUS 16

/* Per-CPU PM QoS values to write back at exit. The table is filled before a
 * limit is written and not changed afterwards, so the signal handler below
 * can use it. */
static struct {
    char path[96];
    char value[16];
    size_t len;
} idle_saved[IDLE_MAX_CPUS];
static volatile sig_atomic_t idle_saved_count = 0;

/* Async-signal-safe: open, write and close only. */
static void idle_restore(void)
{
    const int n = idle_saved_count;
    idle_saved_count = 0;
    for (int i = 0; i < n; ++i) {
        int fd = open(idle_saved[i].path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t rc = write(fd, idle_saved[i].value, idle_saved[i].len);
        (void)rc;
        close(fd);
    }
}

static void idle_restore_on_signal(int sig)
{
    idle_restore();
    raise(sig); /* SA_RESETHAND restored the default action */
}

/* Unlike /dev/cpu_dma_latency, a per-CPU limit outlives the process, so put
 * the old values back on normal exit and on the signals that end a run
 * (Ctrl-C, kill, crashes). Handlers others installed are left alone. Only
 * SIGKILL leaves the limit in place until reboot or a manual write. */
static void idle_install_restore(void)
{
    static int installed = 0;
    if (installed) return;
    installed = 1;
    atexit(idle_restore);
    const int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i) {
        struct sigaction old;
        if (sigaction(sigs[i], NULL, &old) != 0 || old.sa_handler != SIG_DFL) continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = idle_restore_on_signal;
        sa.sa_flags = SA_RESETHAND;
        sigemptyset(&sa.sa_mask);
        sigaction(sigs[i], &sa, NULL);
    }
}

/* Exit latency (us) of the shallowest enabled idle state of `cpu` that halts
 * it, i.e. any state but polling; -1 if the CPU has no such state. */
static int idle_halt_latency(int cpu)
{
    int best = -1;
    for (int s = 0; s < 32; ++s) {
        char path[128], val[64];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/name", cpu, s);
        if (read_line(path, val, sizeof(val)) != 0) break;
        if (strcmp(val, "POLL") == 0) continue;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/disable", cpu, s);
        if (read_line(path, val, sizeof(val)) == 0 && atoi(val) != 0) continue;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/latency", cpu, s);
        if (read_line(path, val, sizeof(val)) != 0) continue;
        const int lat = atoi(val);
        if (best < 0 || lat < best) best = lat;
    }
    return best;
}

/* Space-separated names of the enabled idle states of `cpu` whose exit
 * latency fits within limit_us; "" if the CPU lists no idle states. */
static void idle_allowed_states(int cpu, int limit_us, char* buf, size_t len)
{
    size_t used = 0;
    buf[0] = '\0';
    for (int s = 0; s < 32; ++s) {
        char path[128], name[32], val[64];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/name", cpu, s);
        if (read_line(path, name, sizeof(name)) != 0) break;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/disable", cpu, s);
        if (read_line(path, val, sizeof(val)) == 0 && atoi(val) != 0) continue;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/latency", cpu, s);
        if (read_line(path, val, sizeof(val)) != 0 || atoi(val) > limit_us) continue;
        const int w = snprintf(buf + used, len - used, "%s%s", used ? " " : "", name);
        if (w < 0 || (size_t)w >= len - used) break;
        used += (size_t)w;
    }
}

/* Per-CPU limits through the device PM QoS files. In those files "0" means
 * no limit and "n/a" means no latency at all, i.e. polling only. Sets
 * *had_limit if a CPU already had a limit (say, left by a killed run). */
static int idle_limit_per_cpu(const int* cpus, int n, int latency_us, int* had_limit)
{
    if (n > IDLE_MAX_CPUS) return E2BIG;
    char value[16];
    if (latency_us == 0) copy_str(value, sizeof(value), "n/a");
    else snprintf(value, sizeof(value), "%d", latency_us);

    idle_install_restore();
    idle_restore(); /* a second call replaces the first limit */
    int count = 0;
    for (int i = 0; i < n; ++i) {
        int dup = 0;
        for (int j = 0; j < i; ++j) dup = dup || cpus[j] == cpus[i];
        if (dup || cpus[i] < 0) continue;
        char path[96], old[16];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/power/pm_qos_resume_latency_us", cpus[i]);
        int rc = read_line(path, old, sizeof(old));
        int fd = rc == 0 ? open(path, O_WRONLY | O_CLOEXEC) : -1;
        if (fd < 0) {
            if (rc == 0) rc = errno;
            idle_saved_count = count;
            idle_restore();
            return rc;
        }
        if (strcmp(old, "0") != 0) *had_limit = 1;
        copy_str(idle_saved[count].path, sizeof(idle_saved[count].path), path);
        copy_str(idle_saved[count].value, sizeof(idle_saved[count].value), old);
        idle_saved[count].len = strlen(idle_saved[count].value);
        idle_saved_count = ++count;
        const ssize_t w = write(fd, value, strlen(value));
        rc = w == (ssize_t)strlen(value) ? 0 : (errno ? errno : EIO);
        close(fd);
        if (rc != 0) {
            idle_restore();
            return rc;
        }
    }
    return count > 0 ? 0 : EINVAL;
}

/* Global limit: the PM QoS request lasts while the file stays open, so the
 * descriptor is kept until the process exits. */
static int idle_limit_all(int latency_us)
{
    static int fd = -1;
    if (fd < 0) fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    const int32_t v = latency_us;
    if (write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v)) {
        const int rc = errno ? errno : EIO;
        close(fd);
        fd = -1;
        return rc;
    }
    return 0;
}

int olt_os_limit_idle(const int* cpus, int n, int latency_us, olt_idle_limit_t* out)
{
    const int probe = n > 0 && cpus[0] >= 0 ? cpus[0] : 0;
    if (latency_us < 0) {
        latency_us = idle_halt_latency(probe);
        if (latency_us < 0) latency_us = 0; /* no halting state is listed: nothing deeper to allow */
    }
    int per_cpu = 0, had_limit = 0;
    int rc = n > 0 ? idle_limit_per_cpu(cpus, n, latency_us, &had_limit) : EINVAL;
    if (rc == 0) per_cpu = 1;
    else rc = idle_limit_all(latency_us);
    if (out) {
        out->latency_us = latency_us;
        out->per_cpu = per_cpu;
        out->had_limit = per_cpu && had_limit;
        idle_allowed_states(probe, latency_us, out->states, sizeof(out->states));
    }
    return rc;
}

int olt_os_restore_owner(const char* path)
{
    const char* uid = getenv("SUDO_UID");
    const char* gid = getenv("SUDO_GID");
    if (!uid || !gid || geteuid() != 0) return 0;
    return chown(path, (uid_t)strtoul(uid, NULL, 10), (gid_t)strtoul(gid, NULL, 10)) == 0 ? 0 : errno;
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

uint64_t olt_os_memory_available(void)
{
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    unsigned long long kb = 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    fclose(f);
    return (uint64_t)kb * 1024ull;
}

/* Reads busy/total jiffies for "cpu" (index -1) and "cpuN" lines. */
static int read_cpu_times(unsigned long long* busy, unsigned long long* total, int max_cpus)
{
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return errno ? errno : ENOENT;
    for (int i = 0; i <= max_cpus; ++i) busy[i] = total[i] = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        int idx = -1;
        const char* p = line + 3;
        if (isdigit((unsigned char)*p)) idx = atoi(p);
        while (*p && !isspace((unsigned char)*p)) ++p;
        unsigned long long v[10] = {0};
        int n = sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
        if (n < 4 || idx >= max_cpus) continue;
        unsigned long long t = 0;
        for (int k = 0; k < n && k < 8; ++k) t += v[k]; /* guest time is already in user */
        const unsigned long long idle = v[3] + (n > 4 ? v[4] : 0); /* idle + iowait */
        busy[idx + 1] = t - idle;
        total[idx + 1] = t;
    }
    fclose(f);
    return 0;
}

int olt_os_cpu_busy(unsigned sample_ms, double* overall, double* per_cpu, int max_cpus)
{
    if (max_cpus < 0) max_cpus = 0;
    unsigned long long *b0 = calloc((size_t)max_cpus + 1, sizeof(*b0)), *t0 = calloc((size_t)max_cpus + 1, sizeof(*t0));
    unsigned long long *b1 = calloc((size_t)max_cpus + 1, sizeof(*b1)), *t1 = calloc((size_t)max_cpus + 1, sizeof(*t1));
    int rc = (b0 && t0 && b1 && t1) ? read_cpu_times(b0, t0, max_cpus) : ENOMEM;
    if (rc == 0) {
        struct timespec ts = {(time_t)(sample_ms / 1000), (long)(sample_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        rc = read_cpu_times(b1, t1, max_cpus);
    }
    if (rc == 0) {
        for (int i = 0; i <= max_cpus; ++i) {
            const unsigned long long dt = t1[i] - t0[i];
            const double frac = dt ? (double)(b1[i] - b0[i]) / (double)dt : -1.0;
            if (i == 0) { if (overall) *overall = frac; }
            else if (per_cpu) per_cpu[i - 1] = frac;
        }
    }
    free(b0); free(t0); free(b1); free(t1);
    return rc;
}

int olt_os_stdin_is_terminal(void) { return isatty(STDIN_FILENO); }

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
