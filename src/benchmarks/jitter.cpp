// OS jitter on the measurement core: how often the kernel (interrupts, timers,
// softirqs, other tasks) takes the CPU away from a tight loop, and for how long.
//
// The loop reads the clock as fast as it can and looks at the gap between
// consecutive reads. Undisturbed, a gap is one clock read (about 14 ns through
// the vDSO). A gap of a microsecond or more means this thread was not running:
// something else had the CPU, in the same way osnoise traces it. Measured in
// windows of 20 ms, on the main benchmark CPU (the thread is pinned there):
//
//   os.jitter_gap                  the longest gap in each window, ns
//   os.jitter_rate [threshold_us]  gaps longer than the threshold, per second
//                                  (1 and 10 us; a 10 us gap also counts as a 1 us gap)
//
// Every window is a fresh run of the loop, so the samples are independent of
// each other. The percentiles are over windows: p50 is a typical window, p99
// the window in the worst 1%. The mean rate in the JSON is the average over all
// windows, which is the figure to compare between machines.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "olt/benchmark.hpp"

namespace {

constexpr uint64_t k_window_ns = 20000000;  // 20 ms per window
constexpr uint64_t k_thresholds_ns[] = {1000, 10000};
constexpr size_t k_thresholds = sizeof(k_thresholds_ns) / sizeof(k_thresholds_ns[0]);
constexpr size_t k_warmup_windows = 5;

struct Window {
    uint64_t max_gap = 0;
    uint64_t over[k_thresholds] = {};  // gaps longer than each threshold
};

// Reads the clock for one window and records the gaps between reads.
Window run_window()
{
    Window w;
    const uint64_t start = olt::now_ns();
    uint64_t prev = start;
    for (;;) {
        const uint64_t t = olt::now_ns();
        const uint64_t gap = t - prev;
        prev = t;
        if (gap > w.max_gap) w.max_gap = gap;
        for (size_t k = 0; k < k_thresholds; ++k)
            if (gap > k_thresholds_ns[k]) ++w.over[k];
        if (t - start >= k_window_ns) break;
    }
    return w;
}

}  // namespace

OLT_BENCHMARK("os.jitter",
              "OS jitter on the measurement core: the longest gap in a 20 ms tight clock-reading loop, and how many "
              "gaps per second are longer than 1 and 10 us (os.jitter_gap, os.jitter_rate)")
{
    const size_t windows = std::max<size_t>(200, ctx.config().samples / 10);
    std::vector<double> worst;
    std::vector<double> rate[k_thresholds];
    worst.reserve(windows);
    for (auto& r : rate) r.reserve(windows);
    for (size_t i = 0; i < windows + k_warmup_windows; ++i) {
        const Window w = run_window();
        if (i < k_warmup_windows) continue;
        worst.push_back(double(w.max_gap));
        for (size_t k = 0; k < k_thresholds; ++k)
            rate[k].push_back(double(w.over[k]) * 1e9 / double(k_window_ns));
    }
    const char* note = "gaps between consecutive clock reads on the main benchmark CPU";
    ctx.add_result("os.jitter_gap", {}, std::move(worst), note);
    ctx.results().back().unit = "ns";
    for (size_t k = 0; k < k_thresholds; ++k) {
        ctx.add_result("os.jitter_rate", {{"threshold_us", std::to_string(k_thresholds_ns[k] / 1000)}},
                       std::move(rate[k]), note);
        ctx.results().back().unit = "per s";
    }
}
