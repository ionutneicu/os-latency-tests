// sync_util.hpp - helpers shared by multi-threaded benchmarks.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "olt/hal/arch.h"

namespace olt {

// Share of wake-ups slower than `ns`. Unlike a tail percentile it does not
// need a million samples to be read, and it is comparable between runs with
// different sample counts. Reported as metrics.over_<ns> on the result.
inline double share_over(const std::vector<double>& samples, double ns)
{
    if (samples.empty()) return 0;
    size_t n = 0;
    for (double v : samples) n += v > ns;
    return double(n) / double(samples.size());
}

// Waits until `a` holds `value`. Yields instead of busy-spinning so that two
// SCHED_FIFO threads pinned to the same CPU cannot starve each other; this is
// only used for phase coordination, never on the measured path.
template <class T>
inline void wait_for(const std::atomic<T>& a, T value)
{
    for (int spins = 0; a.load(std::memory_order_acquire) != value; ++spins) {
        if (spins < 64) olt_cpu_relax();
        else std::this_thread::yield();
    }
}

// Gives the peer thread time to block in the kernel before we signal it.
inline void park(unsigned us)
{
    if (us) std::this_thread::sleep_for(std::chrono::microseconds(us));
}

}  // namespace olt
