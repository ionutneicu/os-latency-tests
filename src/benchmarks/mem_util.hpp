// Helpers shared by the memory benchmarks: buffer sizing and pointer-chase setup.
#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/hal/os.h"

namespace olt {

inline uint64_t largest_cache(int cpu)
{
    olt_cache_level_t levels[16];
    const int n = olt_os_cache_topology(cpu >= 0 ? cpu : 0, levels, 16);
    uint64_t m = 0;
    for (int i = 0; i < n; ++i) m = std::max<uint64_t>(m, levels[i].size_bytes);
    return m;
}

inline size_t resolve_max_bytes(const olt::Config& cfg)
{
    if (cfg.mem_max_bytes) return cfg.mem_max_bytes;
    // 4x the largest cache so the DRAM plateau is clearly visible, at least
    // 64 MiB, at most 512 MiB and a quarter of RAM (small boards).
    uint64_t want = std::max<uint64_t>(largest_cache(cfg.main_cpu) * 4, uint64_t(64) << 20);
    want = std::min<uint64_t>(want, uint64_t(512) << 20);
    if (const uint64_t ram = olt_os_physical_memory()) want = std::min<uint64_t>(want, ram / 4);
    return static_cast<size_t>(want);
}

// Links the first `n` lines of `buf` into one random cycle (Sattolo's
// algorithm), each line's first word pointing at the next line. Returns the
// visiting order (line indices), e.g. to start several chases spread over it.
inline std::vector<uint32_t> build_chase(char* buf, size_t n, size_t line, std::mt19937_64& rng)
{
    std::vector<uint32_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<uint32_t>(i);
    for (size_t i = n - 1; i > 0; --i) {
        std::uniform_int_distribution<size_t> pick(0, i - 1);
        std::swap(order[i], order[pick(rng)]);
    }
    for (size_t i = 0; i < n; ++i) {
        void** slot = reinterpret_cast<void**>(buf + size_t(order[i]) * line);
        *slot = buf + size_t(order[(i + 1) % n]) * line;
    }
    return order;
}

}  // namespace olt
