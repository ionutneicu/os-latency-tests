#include "olt/clock.hpp"

#include <algorithm>
#include <vector>

#include "olt/stats.hpp"

namespace olt {

TickCalibration calibrate_ticks(unsigned duration_ms)
{
    TickCalibration cal;
    const uint64_t hz = olt_cycles_hz_hint();
    if (hz != 0) {
        cal.ticks_per_ns = static_cast<double>(hz) / 1e9;
        cal.from_hardware = true;
    } else {
        // Take the best of a few short windows to reject preemption noise.
        double best = 0;
        for (int round = 0; round < 3; ++round) {
            const uint64_t t0 = now_ns();
            const uint64_t c0 = cycles();
            const uint64_t deadline = t0 + static_cast<uint64_t>(duration_ms) * 1000000ull / 3;
            uint64_t t1;
            do { t1 = now_ns(); } while (t1 < deadline);
            const uint64_t c1 = cycles();
            const double rate = static_cast<double>(c1 - c0) / static_cast<double>(t1 - t0);
            best = std::max(best, rate);
        }
        cal.ticks_per_ns = best > 0 ? best : 1.0;
    }
    cal.tick_ns = 1.0 / cal.ticks_per_ns;
    return cal;
}

TimerOverhead measure_timer_overhead(const TickCalibration& cal, size_t iterations)
{
    TimerOverhead o;
    o.os_resolution_ns = olt_os_monotonic_resolution_ns();

    std::vector<double> d(iterations);
    for (size_t i = 0; i < iterations; ++i) {
        const uint64_t a = now_ns();
        const uint64_t b = now_ns();
        d[i] = static_cast<double>(b - a);
    }
    Summary s = summarize(d, {50.0});
    o.now_ns_min = s.min;
    o.now_ns_median = s.at(50.0);

    for (size_t i = 0; i < iterations; ++i) {
        const uint64_t a = cycles();
        const uint64_t b = cycles();
        d[i] = cal.to_ns(b - a);
    }
    s = summarize(d, {50.0});
    o.cycles_ns_min = s.min;
    o.cycles_ns_median = s.at(50.0);
    return o;
}

}  // namespace olt
