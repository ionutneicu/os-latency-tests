// olt/platform.hpp - description of the machine a run was made on.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "olt/clock.hpp"
#include "olt/hal/os.h"

namespace olt {

// Whether this binary's C and C++ code was compiled with optimisation: 1 yes,
// 0 no, -1 unknown (the compiler does not say). Unoptimised code measures the
// compiler's stack traffic, not the hardware.
struct BuildOptimization {
    int c = -1;    // the C kernels (src/hal/stream.c)
    int cxx = -1;  // the C++ harness and benchmarks
};

BuildOptimization build_optimization();
// "C++", "C", "C++ and C" for the languages known to be unoptimised, "" if none.
std::string unoptimised_languages(const BuildOptimization& b);

struct PlatformInfo {
    std::string arch;
    std::string os;
    std::string cpu_model;
    std::string board;
    std::string kernel_release;
    std::string kernel_version;
    std::string preempt_model;
    std::string governor;       // of the main benchmark CPU
    std::string cpuidle_driver;
    std::string isolated_cpus;
    std::vector<std::pair<std::string, std::string>> cpu_vulnerabilities;  // name, mitigation status
    std::string thp_mode;       // transparent huge pages policy
    uint64_t physical_memory = 0;
    int cpu_count = 0;
    size_t cache_line_size = 0;
    std::vector<olt_cache_level_t> caches;  // as seen by the main benchmark CPU
    std::string perf_backend;               // "none" when built without perf support
    std::vector<std::string> perf_events;   // counters that could actually be opened
    std::string compiler;
    std::string build_type;
    BuildOptimization optimized;
    std::string git_commit;  // commit the sources were built from, "" if unknown
    bool git_dirty = false;  // built with uncommitted changes to tracked files
    TickCalibration ticks;
    TimerOverhead timer;
};

PlatformInfo collect_platform_info(int cpu, const TickCalibration& cal, const TimerOverhead& timer);

}  // namespace olt
