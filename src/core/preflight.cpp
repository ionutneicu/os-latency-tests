#include "olt/preflight.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

#include "olt/hal/os.h"
#include "olt/platform.hpp"

namespace olt {

namespace {

std::string pct(double f)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f%%", f * 100.0);
    return buf;
}

std::string mib(uint64_t b) { return std::to_string(b >> 20) + " MiB"; }

std::string join(const std::vector<int>& v)
{
    std::string s;
    for (int x : v) s += (s.empty() ? "" : ",") + std::to_string(x);
    return s;
}

}  // namespace

std::vector<std::string> preflight_checks(const Config& cfg, const PreflightInput& in)
{
    std::vector<std::string> w;

    // 0. unoptimised build: the numbers would describe the compiler, not the machine
    const std::string unopt = unoptimised_languages(build_optimization());
    if (!unopt.empty())
        w.push_back("This binary's " + unopt + " code was compiled without optimisation: results measure the "
                    "compiler's stack traffic, not the hardware, and come out up to several times too slow. "
                    "Rebuild in a fresh directory: rm -rf build && cmake -S . -B build "
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j");

    // CPUs the benchmarks run on (pinned ones, otherwise all allowed)
    std::set<int> used;
    if (cfg.main_cpu >= 0) used.insert(cfg.main_cpu);
    if (cfg.worker_cpu >= 0) used.insert(cfg.worker_cpu);
    if (used.empty()) used.insert(cfg.allowed_cpus.begin(), cfg.allowed_cpus.end());

    // 1. frequency governor
    std::vector<int> slow;
    std::string gov_name;
    for (int c : used) {
        char gov[64] = {};
        olt_os_cpu_governor(c, gov, sizeof(gov));
        if (gov[0] && std::string(gov) != "performance") {
            slow.push_back(c);
            gov_name = gov;
        }
    }
    if (!slow.empty())
        w.push_back("CPU frequency governor is '" + gov_name + "' on CPU " + join(slow) +
                    ": clocks scale up and down during the run, adding latency noise. For stable results: "
                    "sudo cpupower frequency-set -g performance");

    // 2. other processes using the CPUs (we sleep while sampling)
    const int ncpu = std::max(olt_os_cpu_count(), used.empty() ? 1 : *used.rbegin() + 1);
    std::vector<double> per(static_cast<size_t>(ncpu), -1.0);
    double overall = -1.0;
    if (olt_os_cpu_busy(300, &overall, per.data(), ncpu) == 0) {
        std::vector<int> busy_used;
        double worst = 0;
        for (int c : used)
            if (c < ncpu && per[c] > 0.10) {
                busy_used.push_back(c);
                worst = std::max(worst, per[c]);
            }
        if (!busy_used.empty())
            w.push_back("CPU " + join(busy_used) + " used by the benchmarks " + (busy_used.size() > 1 ? "are" : "is") +
                        " already up to " + pct(worst) + " busy with other work: expect inflated tails. Close other "
                        "programs (see top/htop) or pick idle CPUs with --cpus.");
        else if (overall > 0.25)
            w.push_back("Other processes keep the system " + pct(overall) +
                        " busy: shared caches, memory bandwidth and interrupts will add noise. Close other programs "
                        "(see top/htop) for clean results.");
    }

    // 3. memory pressure
    const uint64_t total = olt_os_physical_memory();
    const uint64_t avail = olt_os_memory_available();
    if (total && avail) {
        const uint64_t need = in.memory_sweep_selected
                                  ? 2 * (cfg.mem_max_bytes ? uint64_t(cfg.mem_max_bytes) : uint64_t(512) << 20)
                                  : uint64_t(256) << 20;
        if (avail < total / 10 || avail < need)
            w.push_back("Only " + mib(avail) + " of " + mib(total) + " memory available" +
                        (in.memory_sweep_selected ? " (the memory sweep needs up to " + mib(need / 2) + ")" : "") +
                        ": swapping or page reclaim would distort results. Close memory-hungry programs" +
                        (in.memory_sweep_selected ? " or lower --mem-max." : "."));
    }
    return w;
}

}  // namespace olt
