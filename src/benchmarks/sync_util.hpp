// sync_util.hpp - helpers shared by multi-threaded benchmarks.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "olt/hal/arch.h"

namespace olt {

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
