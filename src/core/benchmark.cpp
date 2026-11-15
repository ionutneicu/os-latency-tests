#include "olt/benchmark.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>

#include "olt/hal/arch.h"
#include "olt/hal/os.h"

namespace olt {

namespace {

// Reference loop for the warm-up: a dependent multiply chain, so each step waits for
// the previous one and the time follows the core's speed. The operand is read from
// a volatile so the chain cannot be folded away.
volatile uint64_t g_reference_operand = 3;
constexpr uint64_t kReferenceSteps = 1000000;  // about 1 ms at a few GHz
constexpr double kSettleBand = 0.03;           // windows within 3% of their median agree
constexpr size_t kSettleWindows = 5;
constexpr double kMinHoldMs = 50;

// One window of the reference loop; returns ns per multiply.
double reference_window()
{
    const uint64_t k = g_reference_operand;
    uint64_t x = 1;
    const uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < kReferenceSteps; ++i) x *= k;
    const uint64_t t1 = now_ns();
    do_not_optimize(x);
    return static_cast<double>(t1 - t0) / static_cast<double>(kReferenceSteps);
}

double median3(double a, double b, double c)
{
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    return b;
}

}  // namespace

std::string source_path(const char* file)
{
    if (!file) return {};
    std::string f = file;
    const std::string root = OLT_SOURCE_DIR;
    if (f.compare(0, root.size(), root) == 0) f.erase(0, root.size());
    return f;
}

std::vector<BenchmarkInfo>& registry()
{
    static std::vector<BenchmarkInfo> r;
    return r;
}

void Context::setup_worker_thread() const
{
    if (config_.worker_cpu >= 0) olt_os_pin_current_thread(config_.worker_cpu);
    if (config_.rt_priority > 0) olt_os_set_rt_priority(config_.rt_priority);
}

void Context::warm_cpus() const
{
    warmup_ = WarmupInfo{};
    if (config_.cpu_warmup_ms == 0) return;
    const double cap_ms = static_cast<double>(config_.cpu_warmup_ms);

    // The worker starts on the main thread's CPU; let it pin itself away before
    // the main thread starts spinning, or under SCHED_FIFO it would never run.
    std::atomic<bool> pinned{false};
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        setup_worker_thread();
        pinned.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) reference_window();
    });
    while (!pinned.load(std::memory_order_acquire)) std::this_thread::yield();

    // Measure windows until they settle, or give up at the cap.
    const uint64_t t0 = now_ns();
    std::vector<double> trace;
    std::vector<double> window_end_ms;
    bool stable = false;
    double ramp_ms = 0;
    for (;;) {
        trace.push_back(reference_window());
        const double now_ms = static_cast<double>(now_ns() - t0) / 1e6;
        window_end_ms.push_back(now_ms);
        if (trace_settled(trace, kSettleWindows, kSettleBand)) {
            // The settled run starts kSettleWindows - 1 windows before this one.
            ramp_ms = window_end_ms[window_end_ms.size() - kSettleWindows];
            stable = true;
            break;
        }
        if (now_ms >= cap_ms) break;
    }

    // Hold for twice the ramp, at least kMinHoldMs, within the cap.
    if (stable) {
        const double hold = warmup_hold_ms(ramp_ms, kMinHoldMs, cap_ms);
        while (static_cast<double>(now_ns() - t0) / 1e6 < hold) reference_window();
    }

    const double after_hold = median3(reference_window(), reference_window(), reference_window());
    stop.store(true, std::memory_order_release);
    worker.join();

    warmup_.ran = true;
    warmup_.stable = stable;
    warmup_.ramp_ms = ramp_ms;
    warmup_.total_ms = static_cast<double>(now_ns() - t0) / 1e6;
    warmup_.reference_ns = after_hold;
}

double Context::clock_check() const
{
    if (config_.cpu_warmup_ms == 0) return 0;
    return median3(reference_window(), reference_window(), reference_window());
}

void Context::add_result(std::string name, Params params, std::vector<double> samples, std::string note)
{
    // Plain samples do not declare their operation count, so no floor check applies.
    add_timed_result(std::move(name), std::move(params), Timed{std::move(samples), 0}, std::move(note));
}

void Context::add_timed_result(std::string name, Params params, Timed timed, std::string note)
{
    Result r;
    r.name = std::move(name);
    r.params = std::move(params);
    r.samples = std::move(timed.samples);
    r.ops_per_sample = timed.ops_per_sample;
    r.note = std::move(note);
    r.source = source_;
    if (warmup_.ran) {
        r.metrics.push_back({"warmup_ramp_ms", warmup_.ramp_ms});
        r.metrics.push_back({"warmup_total_ms", warmup_.total_ms});
        r.metrics.push_back({"warmup_stable", warmup_.stable ? 1.0 : 0.0});
        r.metrics.push_back({"reference_ns", warmup_.reference_ns});
    }

    // A per-call sample under 3x the clock-read pair is dominated by the clock itself.
    if (r.ops_per_sample == 1 && clock_floor_ns_ > 0 && !r.samples.empty()) {
        std::vector<double> sorted = r.samples;
        const auto mid = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
        std::nth_element(sorted.begin(), mid, sorted.end());
        r.at_floor = *mid < 3.0 * clock_floor_ns_;
    }
    results_.push_back(std::move(r));
}

bool glob_match(const std::string& pattern, const std::string& text)
{
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

}  // namespace olt
