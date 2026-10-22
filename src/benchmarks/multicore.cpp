// Multicore communication: how long one cache line takes to travel between
// two specific CPUs.
//
// For every pair of allowed CPUs (a, b) two threads, pinned to a and b,
// ping-pong a counter in one std::atomic on its own cache line: a writes an
// odd value, b answers with the next even one. Each sample times a batch of
// round trips; the result is the one-way latency (half a round trip), i.e.
// the cost of handing a value to a thread on that other CPU through shared
// memory. SMT siblings share L1/L2 and are fastest, CPUs on the same die or
// cluster come next, other clusters / sockets / CCXs are slowest.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/hal/arch.h"
#include "olt/hal/os.h"

namespace {

constexpr uint64_t k_round_trips = 1000;  // per sample
constexpr uint64_t k_stop = ~uint64_t(0);

struct alignas(64) Line {
    std::atomic<uint64_t> v{0};
};

// Returns per-sample one-way latencies, or an empty vector if pinning failed.
std::vector<double> ping_pong(int cpu_a, int cpu_b, size_t samples)
{
    Line line;
    std::atomic<int> ready{0};
    std::atomic<bool> failed{false};

    std::thread responder([&] {
        if (olt_os_pin_current_thread(cpu_b) != 0) failed.store(true);
        ready.fetch_add(1);
        uint64_t expect = 1;
        for (;;) {
            uint64_t v;
            while ((v = line.v.load(std::memory_order_acquire)) != expect) {
                if (v == k_stop || failed.load(std::memory_order_relaxed)) return;
                olt_cpu_relax();
            }
            line.v.store(expect + 1, std::memory_order_release);
            expect += 2;
        }
    });

    std::vector<double> out;
    std::thread initiator([&] {
        if (olt_os_pin_current_thread(cpu_a) != 0) failed.store(true);
        ready.fetch_add(1);
        while (ready.load() < 2) olt_cpu_relax();
        if (failed.load()) {
            line.v.store(k_stop, std::memory_order_release);
            return;
        }
        uint64_t next = 1;
        for (size_t s = 0; s < samples + 1; ++s) {  // first sample warms the path up
            const uint64_t t0 = olt::now_ns();
            for (uint64_t r = 0; r < k_round_trips; ++r) {
                line.v.store(next, std::memory_order_release);
                while (line.v.load(std::memory_order_acquire) != next + 1) olt_cpu_relax();
                next += 2;
            }
            const uint64_t t1 = olt::now_ns();
            if (s > 0) out.push_back(double(t1 - t0) / double(2 * k_round_trips));
        }
        line.v.store(k_stop, std::memory_order_release);
    });
    initiator.join();
    responder.join();
    if (failed.load()) out.clear();
    return out;
}

}  // namespace

OLT_BENCHMARK("multicore.c2c",
              "Core-to-core latency: one-way cache-line hand-off through a std::atomic ping-pong, for "
              "every pair of allowed CPUs (multicore.c2c_latency)")
{
    const std::vector<int>& cpus = ctx.config().allowed_cpus;
    const int n = static_cast<int>(cpus.size());
    if (n < 2) {
        ctx.add_result("multicore.c2c_latency", {}, {}, "needs at least two allowed CPUs");
        return;
    }
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            std::vector<double> lat = ping_pong(cpus[i], cpus[j], ctx.config().sweep_samples);
            if (lat.empty()) continue;  // CPU went offline or pinning not permitted
            ctx.add_result("multicore.c2c_latency",
                           {{"cpu_a", std::to_string(cpus[i])}, {"cpu_b", std::to_string(cpus[j])}}, std::move(lat));
        }
    }
}
