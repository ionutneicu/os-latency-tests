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
//
// multicore.false_sharing: two threads on two physical cores each increment
// their own counter, the counters `distance` bytes apart in one page, either
// with a plain load + store (op=plain, a non-atomic `++field`) or with an
// atomic fetch_add (op=atomic, e.g. per-thread statistics counters). Below
// the coherence granule (64 B on most CPUs, effectively 128 B where the
// adjacent-line prefetcher pairs lines) every write steals the line from the
// other core. Plain stores hide part of the cost in the store buffer, atomic
// read-modify-writes pay every transfer. The result is ns per increment against distance;
// multicore.false_sharing_alone is the same loop on one core, the floor.

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/hal/arch.h"
#include "olt/hal/os.h"
#include "topo_util.hpp"

namespace {

constexpr uint64_t k_round_trips = 1000;  // per sample
constexpr uint64_t k_stop = ~uint64_t(0);

struct alignas(64) Line {
    std::atomic<uint64_t> v{0};
};

// Waits for `ready` to reach `n` while yielding: a new thread starts on its
// creator's CPU and only then pins itself, so under SCHED_FIFO a thread that
// busy-waits before its peer has migrated would starve that peer forever.
void wait_ready(const std::atomic<int>& ready, int n)
{
    while (ready.load(std::memory_order_acquire) < n) std::this_thread::yield();
}

// Returns per-sample one-way latencies, or an empty vector if pinning failed.
std::vector<double> ping_pong(int cpu_a, int cpu_b, size_t samples)
{
    Line line;
    std::atomic<int> ready{0};
    std::atomic<bool> failed{false};

    std::thread responder([&] {
        if (olt_os_pin_current_thread(cpu_b) != 0) failed.store(true);
        ready.fetch_add(1);
        wait_ready(ready, 2);  // both threads are on their own CPUs from here on
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
        wait_ready(ready, 2);
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

constexpr uint64_t k_increments = uint64_t(1) << 17;  // per thread per sample
constexpr size_t k_distances[] = {8, 16, 32, 64, 128, 256, 512};

struct alignas(4096) Page {
    unsigned char bytes[4096];
};

// True if CPUs a and c share an L1 (or are the same CPU): an SMT sibling
// shares the line and would hide the effect.
bool shares_l1(int a, int c)
{
    if (a == c) return true;
    for (int s : olt::l1_siblings(a))
        if (s == c) return true;
    return false;
}

// Second CPU for the false-sharing pair: the worker CPU when it is a different
// physical core from `a`, else the first allowed CPU that is, or -1.
int second_core(int a, const olt::Config& cfg)
{
    if (cfg.worker_cpu >= 0 && !shares_l1(a, cfg.worker_cpu)) return cfg.worker_cpu;
    for (int c : cfg.allowed_cpus)
        if (!shares_l1(a, c)) return c;
    return -1;
}

// ns per increment of the slower thread; cpu_b < 0 runs one thread alone.
// Returns a negative value if pinning failed.
double increment_race(int cpu_a, int cpu_b, size_t distance, bool atomic_rmw)
{
    auto page = std::make_unique<Page>();
    auto* first = new (page->bytes) std::atomic<uint64_t>(0);
    auto* second = new (page->bytes + distance) std::atomic<uint64_t>(0);
    const int threads = cpu_b < 0 ? 1 : 2;
    std::atomic<int> ready{0};
    std::atomic<bool> failed{false};
    uint64_t ns[2] = {0, 0};

    auto body = [&](int idx, int cpu, std::atomic<uint64_t>* counter) {
        if (olt_os_pin_current_thread(cpu) != 0) failed.store(true);
        ready.fetch_add(1);
        wait_ready(ready, threads);
        const uint64_t t0 = olt::now_ns();
        if (atomic_rmw) {
            for (uint64_t i = 0; i < k_increments; ++i) counter->fetch_add(1, std::memory_order_relaxed);
        } else {  // plain load + store, like `++field` on a non-atomic member
            for (uint64_t i = 0; i < k_increments; ++i)
                counter->store(counter->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
        ns[idx] = olt::now_ns() - t0;
    };
    std::thread ta(body, 0, cpu_a, first);
    std::thread tb;
    if (threads == 2) tb = std::thread(body, 1, cpu_b, second);
    ta.join();
    if (tb.joinable()) tb.join();
    olt::do_not_optimize(first->load() + second->load());
    if (failed.load()) return -1;
    return double(ns[0] > ns[1] ? ns[0] : ns[1]) / double(k_increments);
}

}  // namespace

OLT_BENCHMARK("multicore.false_sharing",
              "False sharing: two cores increment their own counters placed 8 to 512 bytes apart; shows the "
              "padding this CPU needs between fields written by different threads (multicore.false_sharing)")
{
    const olt::Config& cfg = ctx.config();
    const std::vector<int>& cpus = cfg.allowed_cpus;
    const int a = cfg.main_cpu >= 0 ? cfg.main_cpu : (cpus.empty() ? -1 : cpus[0]);
    const int b = a < 0 ? -1 : second_core(a, cfg);
    if (b < 0) {
        ctx.add_result("multicore.false_sharing", {}, {}, "needs two allowed CPUs on different physical cores");
        return;
    }
    const olt::Params pair{{"cpu_a", std::to_string(a)}, {"cpu_b", std::to_string(b)}};
    auto samples = [&](int cpu_b, size_t distance, bool rmw, std::vector<double>& out) {
        for (size_t s = 0; s < cfg.sweep_samples + 1; ++s) {  // first sample warms up
            const double v = increment_race(a, cpu_b, distance, rmw);
            if (v < 0) return false;
            if (s > 0) out.push_back(v);
        }
        return true;
    };
    for (const char* op : {"plain", "atomic"}) {
        const bool rmw = op[0] == 'a';
        std::vector<double> alone;
        if (!samples(-1, 0, rmw, alone)) return;  // pinning not permitted
        olt::Params params{{"op", op}};
        params.insert(params.end(), pair.begin(), pair.end());
        ctx.add_result("multicore.false_sharing_alone", params, std::move(alone));
        // Distances interleaved within each round, so a slow stretch of the machine
        // hits every distance alike instead of landing on one of them.
        constexpr size_t nd = sizeof(k_distances) / sizeof(k_distances[0]);
        std::vector<std::vector<double>> lat(nd);
        for (size_t s = 0; s < cfg.sweep_samples + 1; ++s) {  // the first round warms up
            for (size_t i = 0; i < nd; ++i) {
                const double v = increment_race(a, b, k_distances[i], rmw);
                if (v < 0) return;  // pinning not permitted
                if (s > 0) lat[i].push_back(v);
            }
        }
        for (size_t i = 0; i < nd; ++i) {
            olt::Params dp{{"op", op}, {"distance", std::to_string(k_distances[i])}};
            dp.insert(dp.end(), pair.begin(), pair.end());
            ctx.add_result("multicore.false_sharing", dp, std::move(lat[i]));
        }
    }
}

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
