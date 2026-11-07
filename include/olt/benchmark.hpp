// olt/benchmark.hpp - benchmark registry and run context.
//
// A benchmark is a function that receives a Context and records one or more
// Results (a result per parameter value, e.g. one per block size). Register
// it at namespace scope in any source file linked into the executable:
//
//   OLT_BENCHMARK("sync.mutex_wakeup", "Time from unlock() in one thread to "
//                 "lock() returning in the blocked thread") {
//       ctx.add_result("sync.mutex_wakeup", {}, ctx.run_samples([&] { ...; return ns; }));
//   }
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "olt/clock.hpp"

namespace olt {

enum class Affinity { None, SameCpu, DifferentCpus };

// The idle-state limit --no-idle asked for, as the OS accepted it.
struct IdleLimit {
    bool applied = false;  // false: not requested, or the OS refused it (deep idle stays allowed)
    int latency_us = -1;   // exit-latency limit, 0 = polling only
    bool per_cpu = false;  // only the benchmark CPUs are limited (otherwise all CPUs are)
    std::string states;    // idle states still allowed, e.g. "POLL C1"; "" if the OS lists none
};

struct Config {
    size_t samples = 10000;
    size_t warmup = 100;
    Affinity affinity = Affinity::DifferentCpus;
    int main_cpu = -1;    // resolved CPU for the main thread, -1 = not pinned
    int worker_cpu = -1;  // resolved CPU for worker threads, -1 = not pinned
    std::vector<int> allowed_cpus;  // CPUs the process may use, read before any pinning
    int rt_priority = 0;  // SCHED_FIFO priority, 0 = keep default policy
    bool lock_memory = false;
    bool block_deep_idle = false;  // --no-idle: keep the benchmark CPUs out of deep idle states
    int idle_latency_us = -1;      // their exit-latency limit; -1 = the shallowest halting state (C1)
    IdleLimit idle_limit;          // what was applied, filled in at start-up
    unsigned park_us = 200;  // how long a signalling thread waits so the other side is truly blocked
    unsigned cpu_warmup_ms = 200;  // busy-spin main and worker CPUs before each benchmark (frequency ramp-up)

    // memory hierarchy sweeps
    size_t mem_min_bytes = 1024;
    size_t mem_max_bytes = 0;      // 0 = automatic: 4x the largest cache, within RAM limits
    unsigned sweep_steps_per_octave = 4;
    size_t sweep_samples = 15;     // samples per buffer size (each sample is many accesses)
    bool hugepages = false;
};

using Params = std::vector<std::pair<std::string, std::string>>;

// Where a result's code lives, so the report can link to it. OLT_HERE marks the
// current line; a benchmark that compares variants sets each result's source
// to its variant's code (e.g. a static member `source = OLT_HERE` in each class).
struct SourceLine {
    const char* file = nullptr;  // as the compiler saw it (__FILE__)
    int line = 0;
};
#define OLT_HERE (::olt::SourceLine{__FILE__, __LINE__})

// Path of a source file relative to the source tree ("src/benchmarks/x.cpp").
std::string source_path(const char* file);

// Samples from time_batch or time_each, with the number of operations each one covers.
struct Timed {
    std::vector<double> samples;  // ns per operation
    uint64_t ops_per_sample = 1;
};

struct Result {
    std::string name;
    Params params;
    std::string unit = "ns";
    std::vector<double> samples;
    std::string note;  // caveats shown next to the result
    std::vector<std::pair<std::string, double>> metrics;  // extra per-result figures, e.g. cpu_cores
    SourceLine source;  // the code measured; defaults to the benchmark's OLT_BENCHMARK line
    uint64_t ops_per_sample = 1;  // operations behind one sample (batch timing divides by this)
    bool at_floor = false;        // per-call value below ~3x the clock-read cost: not a measurement
};

class Context {
public:
    explicit Context(const Config& cfg, const TickCalibration& cal) : config_(cfg), cal_(cal) {}

    const Config& config() const { return config_; }
    const TickCalibration& ticks() const { return cal_; }

    // Cost of one back-to-back clock read pair, in ns (from measure_timer_overhead).
    // Results whose per-call value is within a few times this are marked at floor.
    void set_clock_floor_ns(double ns) { clock_floor_ns_ = ns; }
    double clock_floor_ns() const { return clock_floor_ns_; }

    // Applies worker affinity and RT priority to the calling (worker) thread.
    void setup_worker_thread() const;

    // Busy-spins the main and the worker CPU for config.cpu_warmup_ms so that
    // frequency scaling has ramped up before measuring.
    void warm_cpus() const;

    // Calls fn() config.warmup times (discarded) then config.samples times;
    // fn returns one sample, in the result's unit.
    template <class Fn>
    std::vector<double> run_samples(Fn&& fn) const
    {
        for (size_t i = 0; i < config_.warmup; ++i) do_not_optimize(fn());
        std::vector<double> out;
        out.reserve(config_.samples);
        for (size_t i = 0; i < config_.samples; ++i) out.push_back(fn());
        return out;
    }

    void add_result(std::string name, Params params, std::vector<double> samples, std::string note = {});
    // Same, for samples from time_batch or time_each (records ops_per_sample).
    void add_timed_result(std::string name, Params params, Timed timed, std::string note = {});

    // Source of the benchmark now running: the default for its results.
    void set_benchmark_source(SourceLine s) { source_ = s; }

    std::vector<Result>& results() { return results_; }

private:
    const Config& config_;
    const TickCalibration& cal_;
    std::vector<Result> results_;
    SourceLine source_;
    double clock_floor_ns_ = 0;
};

// Per-call timing: one clock pair around each operation. Only meaningful when the
// operation is much slower than the clock read; otherwise use time_batch.
template <class Fn>
Timed time_each(const Context& ctx, Fn&& op)
{
    return Timed{ctx.run_samples(std::forward<Fn>(op)), 1};
}

// Batch timing: one clock pair around n operations, divided by n. n doubles until
// one batch lasts at least kBatchFloorMultiple clock-read costs, so the clock read
// is a negligible share of each sample.
inline constexpr double kBatchFloorMultiple = 100.0;

template <class Fn>
Timed time_batch(const Context& ctx, Fn&& op)
{
    const double target_ns = kBatchFloorMultiple * (ctx.clock_floor_ns() > 1.0 ? ctx.clock_floor_ns() : 1.0);
    auto run = [&op](uint64_t n) {
        const uint64_t t0 = now_ns();
        for (uint64_t i = 0; i < n; ++i) op();
        return now_ns() - t0;
    };
    uint64_t n = 1;
    while (n < (uint64_t(1) << 40) && static_cast<double>(run(n)) < target_ns) n *= 2;

    const Config& cfg = ctx.config();
    for (size_t i = 0; i < cfg.warmup; ++i) run(n);
    Timed out;
    out.ops_per_sample = n;
    out.samples.reserve(cfg.samples);
    for (size_t i = 0; i < cfg.samples; ++i) out.samples.push_back(static_cast<double>(run(n)) / static_cast<double>(n));
    return out;
}

struct BenchmarkInfo {
    std::string name;
    std::string description;
    std::function<void(Context&)> run;
    SourceLine source;
};

std::vector<BenchmarkInfo>& registry();

struct Registrar {
    Registrar(const char* name, const char* description, void (*fn)(Context&), SourceLine source)
    {
        registry().push_back({name, description, fn, source});
    }
};

// Glob match supporting '*' and '?', used for --test filters.
bool glob_match(const std::string& pattern, const std::string& text);

}  // namespace olt

#define OLT_CONCAT_INNER(a, b) a##b
#define OLT_CONCAT(a, b) OLT_CONCAT_INNER(a, b)
#define OLT_BENCHMARK(name, description)                                                       \
    static void OLT_CONCAT(olt_bench_fn_, __LINE__)(::olt::Context & ctx);                     \
    static ::olt::Registrar OLT_CONCAT(olt_bench_reg_, __LINE__)(name, description,            \
                                                                 &OLT_CONCAT(olt_bench_fn_, __LINE__), OLT_HERE); \
    static void OLT_CONCAT(olt_bench_fn_, __LINE__)(::olt::Context & ctx)
