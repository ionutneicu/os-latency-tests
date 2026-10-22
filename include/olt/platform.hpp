// olt/platform.hpp - description of the machine a run was made on.
#pragma once

#include <string>
#include <vector>

#include "olt/clock.hpp"
#include "olt/hal/os.h"

namespace olt {

struct PlatformInfo {
    std::string arch;
    std::string os;
    std::string cpu_model;
    std::string board;
    std::string kernel_release;
    std::string kernel_version;
    std::string preempt_model;
    std::string governor;       // of the main benchmark CPU
    std::string isolated_cpus;
    int cpu_count = 0;
    size_t cache_line_size = 0;
    std::vector<olt_cache_level_t> caches;  // as seen by the main benchmark CPU
    std::string perf_backend;               // "none" when built without perf support
    std::vector<std::string> perf_events;   // counters that could actually be opened
    std::string compiler;
    std::string build_type;
    TickCalibration ticks;
    TimerOverhead timer;
};

PlatformInfo collect_platform_info(int cpu, const TickCalibration& cal, const TimerOverhead& timer);

}  // namespace olt
