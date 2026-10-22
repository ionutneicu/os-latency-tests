// olt/clock.hpp - timing primitives for benchmarks.
//
// Two time sources are used:
//  - now_ns(): OS monotonic clock, comparable across threads and CPUs. Use it
//    for anything measured between two threads (wake-ups, hand-offs).
//  - cycles(): CPU counter via the arch HAL, cheapest to read but only
//    meaningful on one CPU. Convert with TickCalibration::to_ns().
#pragma once

#include <cstdint>

#include "olt/hal/arch.h"
#include "olt/hal/os.h"

namespace olt {

inline uint64_t now_ns() { return olt_os_monotonic_ns(); }
inline uint64_t cycles() { return olt_cycles(); }

// Keeps the compiler from optimising a value (and the work producing it) away.
template <class T>
inline void do_not_optimize(T const& value)
{
    asm volatile("" : : "r,m"(value) : "memory");
}

template <class T>
inline void do_not_optimize(T& value)
{
#if defined(__clang__)
    asm volatile("" : "+r,m"(value) : : "memory");
#else
    asm volatile("" : "+m,r"(value) : : "memory");
#endif
}

struct TickCalibration {
    double ticks_per_ns = 1.0;   // olt_cycles() ticks per nanosecond
    double tick_ns = 1.0;        // duration of one tick
    bool from_hardware = false;  // frequency read from the CPU instead of measured
    double to_ns(uint64_t ticks) const { return static_cast<double>(ticks) / ticks_per_ns; }
};

// Determines the olt_cycles() frequency (hardware value if the arch exposes
// one, otherwise measured against the monotonic clock over ~duration_ms).
TickCalibration calibrate_ticks(unsigned duration_ms = 50);

struct TimerOverhead {
    double now_ns_min = 0;      // back-to-back now_ns() delta, minimum
    double now_ns_median = 0;
    double cycles_ns_min = 0;   // back-to-back cycles() delta in ns, minimum
    double cycles_ns_median = 0;
    uint64_t os_resolution_ns = 0;
};

TimerOverhead measure_timer_overhead(const TickCalibration& cal, size_t iterations = 10000);

}  // namespace olt
