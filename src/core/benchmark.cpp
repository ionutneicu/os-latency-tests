#include "olt/benchmark.hpp"

#include <atomic>
#include <thread>

#include "olt/hal/arch.h"
#include "olt/hal/os.h"

namespace olt {

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
    if (config_.cpu_warmup_ms == 0) return;
    const uint64_t deadline = now_ns() + uint64_t(config_.cpu_warmup_ms) * 1000000ull;
    auto spin = [deadline] {
        uint64_t x = 0;
        while (now_ns() < deadline)
            for (int i = 0; i < 1000; ++i) do_not_optimize(x += uint64_t(i));
    };
    // The worker starts on the main thread's CPU; let it pin itself away before
    // the main thread starts spinning, or under SCHED_FIFO it would never run.
    std::atomic<bool> pinned{false};
    std::thread worker([&] {
        setup_worker_thread();
        pinned.store(true, std::memory_order_release);
        spin();
    });
    while (!pinned.load(std::memory_order_acquire)) std::this_thread::yield();
    spin();
    worker.join();
}

void Context::add_result(std::string name, Params params, std::vector<double> samples, std::string note)
{
    Result r;
    r.name = std::move(name);
    r.params = std::move(params);
    r.samples = std::move(samples);
    r.note = std::move(note);
    r.source = source_;
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
