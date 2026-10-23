#include "olt/platform.hpp"

#include "olt/hal/arch.h"
#include "olt/hal/perf.h"
#include "olt/hal/stream.h"

#ifndef OLT_BUILD_TYPE
#define OLT_BUILD_TYPE "unknown"
#endif

namespace olt {

namespace {

template <size_t N, class Fn>
std::string get_str(Fn fn)
{
    char buf[N] = {};
    fn(buf, sizeof(buf));
    return buf;
}

std::string compiler_id()
{
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "unknown";
#endif
}

}  // namespace

BuildOptimization build_optimization()
{
    BuildOptimization b;
    b.c = olt_stream_optimized();
#if defined(__OPTIMIZE__)
    b.cxx = 1;
#elif defined(__GNUC__)
    b.cxx = 0;
#endif
    return b;
}

std::string unoptimised_languages(const BuildOptimization& b)
{
    std::string s = b.cxx == 0 ? "C++" : "";
    if (b.c == 0) s += s.empty() ? "C" : " and C";
    return s;
}

PlatformInfo collect_platform_info(int cpu, const TickCalibration& cal, const TimerOverhead& timer)
{
    PlatformInfo p;
    p.arch = olt_arch_name();
    p.os = olt_os_name();
    p.cpu_model = get_str<256>(olt_os_cpu_model);
    p.board = get_str<256>(olt_os_board_model);
    p.kernel_release = get_str<256>(olt_os_kernel_release);
    p.kernel_version = get_str<512>(olt_os_kernel_version);
    p.preempt_model = get_str<64>(olt_os_preempt_model);
    const int topo_cpu = cpu >= 0 ? cpu : 0;
    p.governor = get_str<64>([topo_cpu](char* b, size_t l) { olt_os_cpu_governor(topo_cpu, b, l); });
    p.isolated_cpus = get_str<256>(olt_os_isolated_cpus);
    p.cpuidle_driver = get_str<64>(olt_os_cpuidle_driver);
    p.thp_mode = get_str<64>(olt_os_thp_mode);
    p.physical_memory = olt_os_physical_memory();
    p.cpu_count = olt_os_cpu_count();
    p.cache_line_size = olt_cache_line_size();

    olt_cache_level_t levels[16];
    const int n = olt_os_cache_topology(topo_cpu, levels, 16);
    p.caches.assign(levels, levels + n);

    p.perf_backend = olt_perf_backend();
    olt_perf_event_t all[OLT_PERF_EVENT_COUNT];
    for (int i = 0; i < OLT_PERF_EVENT_COUNT; ++i) all[i] = static_cast<olt_perf_event_t>(i);
    if (olt_perf_group_t* g = olt_perf_open(all, OLT_PERF_EVENT_COUNT)) {
        for (int i = 0; i < OLT_PERF_EVENT_COUNT; ++i)
            if (olt_perf_has(g, all[i])) p.perf_events.push_back(olt_perf_event_name(all[i]));
        olt_perf_close(g);
    }

    p.compiler = compiler_id();
    p.build_type = OLT_BUILD_TYPE;
    p.optimized = build_optimization();
    p.ticks = cal;
    p.timer = timer;
    return p;
}

}  // namespace olt
