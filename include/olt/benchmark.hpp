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

struct Config {
    size_t samples = 10000;
    size_t warmup = 100;
    Affinity affinity = Affinity::DifferentCpus;
    int main_cpu = -1;    // resolved CPU for the main thread, -1 = not pinned
    int worker_cpu = -1;  // resolved CPU for worker threads, -1 = not pinned
    std::vector<int> allowed_cpus;  // CPUs the process may use, read before any pinning
    int rt_priority = 0;  // SCHED_FIFO priority, 0 = keep default policy
    bool lock_memory = false;
    bool block_deep_idle = false;  // hold PM QoS cpu_dma_latency at 0 during the run
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

struct Result {
    std::string name;
    Params params;
    std::string unit = "ns";
    std::vector<double> samples;
    std::string note;  // caveats shown next to the result
    std::vector<std::pair<std::string, double>> metrics;  // extra per-result figures, e.g. cpu_cores
};

class Context {
public:
    explicit Context(const Config& cfg, const TickCalibration& cal) : config_(cfg), cal_(cal) {}

    const Config& config() const { return config_; }
    const TickCalibration& ticks() const { return cal_; }

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

    std::vector<Result>& results() { return results_; }

private:
    const Config& config_;
    const TickCalibration& cal_;
    std::vector<Result> results_;
};

struct BenchmarkInfo {
    std::string name;
    std::string description;
    std::function<void(Context&)> run;
};

std::vector<BenchmarkInfo>& registry();

struct Registrar {
    Registrar(const char* name, const char* description, void (*fn)(Context&))
    {
        registry().push_back({name, description, fn});
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
                                                                 &OLT_CONCAT(olt_bench_fn_, __LINE__)); \
    static void OLT_CONCAT(olt_bench_fn_, __LINE__)(::olt::Context & ctx)
