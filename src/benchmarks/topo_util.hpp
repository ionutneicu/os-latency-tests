// topo_util.hpp - CPU topology helpers shared by multi-threaded benchmarks.
#pragma once

#include <cstdlib>
#include <vector>

#include "olt/hal/os.h"

namespace olt {

// Parses a sysfs CPU list such as "0,4" or "0-3,8".
inline std::vector<int> parse_cpu_list(const char* s)
{
    std::vector<int> out;
    while (*s) {
        char* end;
        const long a = std::strtol(s, &end, 10);
        if (end == s) break;
        long b = a;
        if (*end == '-') b = std::strtol(end + 1, &end, 10);
        for (long c = a; c <= b; ++c) out.push_back(static_cast<int>(c));
        s = (*end == ',') ? end + 1 : end;
    }
    return out;
}

// CPUs sharing the L1 data cache with `cpu` (its SMT siblings), `cpu` included.
inline std::vector<int> l1_siblings(int cpu)
{
    olt_cache_level_t levels[16];
    std::vector<int> siblings{cpu};
    const int n = olt_os_cache_topology(cpu, levels, 16);
    for (int i = 0; i < n; ++i)
        if (levels[i].level == 1 && levels[i].type[0] != 'I') siblings = parse_cpu_list(levels[i].shared_cpus);
    return siblings;
}

// One CPU per physical core from `cpus`, in order: SMT siblings of a CPU
// already taken are skipped.
inline std::vector<int> one_per_core(const std::vector<int>& cpus)
{
    std::vector<int> out, taken;
    for (int c : cpus) {
        bool skip = false;
        for (int t : taken) skip = skip || t == c;
        if (skip) continue;
        out.push_back(c);
        for (int s : l1_siblings(c)) taken.push_back(s);
    }
    return out;
}

}  // namespace olt
