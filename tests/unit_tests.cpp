// Unit tests for the statistics, glob filter, HAL primitives and build checks.
// Deliberately dependency-free so it builds on any target toolchain.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/clock.hpp"
#include "olt/hal/arch.h"
#include "olt/hal/os.h"
#include "olt/hal/perf.h"
#include "olt/platform.hpp"
#include "olt/stats.hpp"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

static void test_percentiles()
{
    // Reference values from numpy.percentile (linear interpolation).
    const std::vector<double> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK_NEAR(olt::percentile_sorted(v, 0), 1.0, 1e-12);
    CHECK_NEAR(olt::percentile_sorted(v, 50), 5.5, 1e-12);
    CHECK_NEAR(olt::percentile_sorted(v, 90), 9.1, 1e-12);
    CHECK_NEAR(olt::percentile_sorted(v, 99), 9.91, 1e-12);
    CHECK_NEAR(olt::percentile_sorted(v, 100), 10.0, 1e-12);
    CHECK(std::isnan(olt::percentile_sorted({}, 50)));
    CHECK_NEAR(olt::percentile_sorted({42}, 99.9), 42.0, 1e-12);
}

static void test_summary()
{
    std::vector<double> v;
    for (int i = 100; i >= 1; --i) v.push_back(i);  // unsorted on purpose
    const olt::Summary s = olt::summarize(v);
    CHECK(s.count == 100);
    CHECK_NEAR(s.min, 1.0, 1e-12);
    CHECK_NEAR(s.max, 100.0, 1e-12);
    CHECK_NEAR(s.mean, 50.5, 1e-12);
    CHECK_NEAR(s.stddev, 29.011491975882016, 1e-9);
    CHECK_NEAR(s.at(50), 50.5, 1e-12);
    CHECK_NEAR(s.at(99), 99.01, 1e-9);
    CHECK_NEAR(s.mad, 25.0, 1e-12);
    // 100 samples: p90 needs 100 (reliable), p99 needs 1000 (not reliable)
    for (const auto& pc : s.percentiles) {
        if (pc.p == 90.0) CHECK(pc.reliable);
        if (pc.p == 99.0) CHECK(!pc.reliable);
    }
    CHECK(olt::min_samples_for(99.9) == 10000);
    CHECK(olt::min_samples_for(99.99) == 100000);
}

static void test_glob()
{
    CHECK(olt::glob_match("sync.*", "sync.mutex_wakeup"));
    CHECK(olt::glob_match("*wakeup", "sync.condvar_wakeup"));
    CHECK(olt::glob_match("thread.?reate", "thread.create"));
    CHECK(!olt::glob_match("sync.*", "thread.create"));
    CHECK(olt::glob_match("*", ""));
    CHECK(!olt::glob_match("a", ""));
}

static void test_hal()
{
    const uint64_t c0 = olt_cycles();
    const uint64_t t0 = olt_os_monotonic_ns();
    volatile uint64_t sink = 0;
    for (int i = 0; i < 1000000; ++i) sink = sink + static_cast<uint64_t>(i);
    const uint64_t c1 = olt_cycles();
    const uint64_t t1 = olt_os_monotonic_ns();
    CHECK(c1 > c0);
    CHECK(t1 > t0);

    const size_t line = olt_cache_line_size();
    CHECK(line >= 16 && line <= 512 && (line & (line - 1)) == 0);

    alignas(256) static char buf[4096];
    olt_prefetch_r(buf);
    olt_prefetch_r_l2(buf + 64);
    olt_prefetch_r_l3(buf + 128);
    olt_prefetch_r_nta(buf + 192);
    olt_prefetch_w(buf + 256);
    olt_mb();
    olt_rmb();
    olt_wmb();
    olt_cpu_relax();
    buf[0] = 1;
    olt_cache_flush_line(buf);
    CHECK(buf[0] == 1);

    const olt::TickCalibration cal = olt::calibrate_ticks(30);
    CHECK(cal.ticks_per_ns > 0);

    CHECK(olt_os_cpu_count() >= 1);
    int cpus[1024];
    const int n = olt_os_allowed_cpus(cpus, 1024);
    CHECK(n >= 1);
    if (n >= 1) CHECK(olt_os_pin_current_thread(cpus[0]) == 0);

    // Perf counters may legitimately be unavailable; only check consistency.
    olt_perf_event_t ev = OLT_PERF_CONTEXT_SWITCHES;
    if (olt_perf_group_t* g = olt_perf_open(&ev, 1)) {
        CHECK(olt_perf_has(g, ev));
        uint64_t values[OLT_PERF_EVENT_COUNT] = {};
        CHECK(olt_perf_start(g) == 0);
        CHECK(olt_perf_stop(g) == 0);
        CHECK(olt_perf_read(g, values) == 0);
        olt_perf_close(g);
    }
}

static void test_build_optimization()
{
    // This file is built with the same C++ flags as the library, so they must agree.
    const olt::BuildOptimization b = olt::build_optimization();
#if defined(__OPTIMIZE__)
    CHECK(b.cxx == 1);
#elif defined(__GNUC__)
    CHECK(b.cxx == 0);
#endif
    CHECK(b.c >= -1 && b.c <= 1);
    CHECK(olt::unoptimised_languages({1, 1}).empty());
    CHECK(olt::unoptimised_languages({-1, -1}).empty());
    CHECK(olt::unoptimised_languages({1, 0}) == "C++");
    CHECK(olt::unoptimised_languages({0, 1}) == "C");
    CHECK(olt::unoptimised_languages({0, 0}) == "C++ and C");
}

int main()
{
    test_percentiles();
    test_summary();
    test_glob();
    test_hal();
    test_build_optimization();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all unit tests passed (arch %s, perf backend %s)\n", olt_arch_name(), olt_perf_backend());
    return EXIT_SUCCESS;
}
