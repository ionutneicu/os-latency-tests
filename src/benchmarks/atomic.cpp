// Atomics: what a shared counter costs as cores are added, and what each
// memory order costs on one core.
//
// multicore.atomic_contention [op, threads]
//   1 to N threads, one per physical core (SMT siblings skipped), each add 1
//   to a counter 65536 times per sample:
//     fetch_add  one shared std::atomic, fetch_add(1, relaxed)
//     cas        one shared std::atomic, a compare_exchange_weak loop (how
//                lock-free code updates anything more than a number);
//                metrics.retries = failed attempts per increment
//     sharded    one counter per thread, each on its own 128-byte block, summed
//                on read: no sharing at all
//   Result: ns per increment as each thread sees it (wall time / increments
//   per thread). A shared line moves between cores on every increment, so the
//   cost grows with the thread count; sharded stays flat.
//
// multicore.atomic_order [op]
//   One thread, a counter on its own line, uncontended; ns per operation in a
//   loop of 1024: load, store and fetch_add with relaxed, acquire/release and
//   seq_cst ordering, and a seq_cst fence between plain stores. On x86 only
//   the seq_cst store (an xchg) and the fence (mfence) cost more than relaxed;
//   on ARM, acquire/release and seq_cst use different instructions (ldar/stlr,
//   ldaddal) and every step can cost.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/hal/os.h"
#include "sync_util.hpp"
#include "topo_util.hpp"

namespace {

constexpr uint64_t k_increments = uint64_t(1) << 16;  // per thread per sample
constexpr size_t k_order_ops = 1024;                  // per multicore.atomic_order sample

struct alignas(128) Counter {  // 128: two lines, so the adjacent-line prefetcher never pairs neighbours
    std::atomic<uint64_t> v{0};
};

enum class Op { fetch_add, cas, sharded };

struct RaceResult {
    double ns = -1;  // ns per increment of the slowest thread; < 0 if pinning failed
    double retries = 0;
};

RaceResult race(const std::vector<int>& cpus, Op op)
{
    const int n = int(cpus.size());
    auto counters = std::make_unique<Counter[]>(size_t(n) + 1);  // [n] is the shared one
    std::atomic<int> ready{0};
    std::atomic<bool> go{false}, failed{false};
    std::vector<uint64_t> ns(size_t(n), 0), fails(size_t(n), 0);

    auto body = [&](int idx) {
        if (olt_os_pin_current_thread(cpus[size_t(idx)]) != 0) failed.store(true);
        ready.fetch_add(1);
        olt::wait_for(go, true);
        std::atomic<uint64_t>& shared = counters[size_t(n)].v;
        std::atomic<uint64_t>& own = counters[size_t(idx)].v;
        uint64_t f = 0;
        const uint64_t t0 = olt::now_ns();
        switch (op) {
        case Op::fetch_add:
            for (uint64_t i = 0; i < k_increments; ++i) shared.fetch_add(1, std::memory_order_relaxed);
            break;
        case Op::cas:
            for (uint64_t i = 0; i < k_increments; ++i) {
                uint64_t v = shared.load(std::memory_order_relaxed);
                while (!shared.compare_exchange_weak(v, v + 1, std::memory_order_relaxed)) ++f;
            }
            break;
        case Op::sharded:
            for (uint64_t i = 0; i < k_increments; ++i) own.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        ns[size_t(idx)] = olt::now_ns() - t0;
        fails[size_t(idx)] = f;
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i) threads.emplace_back(body, i);
    olt::wait_for(ready, n);  // every thread pinned and waiting: start them together
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    RaceResult r;
    if (failed.load()) return r;
    uint64_t total = 0;
    for (int i = 0; i <= n; ++i) total += counters[size_t(i)].v.load();
    if (total != uint64_t(n) * k_increments) return r;  // cannot happen with correct atomics
    r.ns = double(*std::max_element(ns.begin(), ns.end())) / double(k_increments);
    uint64_t f = 0;
    for (uint64_t x : fails) f += x;
    r.retries = double(f) / double(uint64_t(n) * k_increments);
    return r;
}

// Thread counts to measure: 1, 2, 3, 4, then doubling, and the maximum.
std::vector<int> thread_counts(int max)
{
    std::vector<int> out;
    for (int t = 1; t <= max; t = t < 4 ? t + 1 : t * 2) out.push_back(t);
    if (out.back() != max) out.push_back(max);
    return out;
}

}  // namespace

OLT_BENCHMARK("multicore.atomic_contention",
              "Atomic counter under contention: ns per increment with 1 to N cores updating one shared "
              "std::atomic (fetch_add, compare-exchange loop) vs one counter per thread (sharded)")
{
    const olt::Config& cfg = ctx.config();
    // The main benchmark CPU goes first, so the 1-thread case runs there (as
    // with --cpus) and its SMT sibling is the one skipped.
    std::vector<int> order;
    if (cfg.main_cpu >= 0) order.push_back(cfg.main_cpu);
    for (int c : cfg.allowed_cpus)
        if (c != cfg.main_cpu) order.push_back(c);
    const std::vector<int> cores = olt::one_per_core(order);
    if (cores.empty()) {
        ctx.add_result("multicore.atomic_contention", {}, {}, "no allowed CPUs");
        return;
    }
    const std::pair<Op, const char*> ops[] = {{Op::fetch_add, "fetch_add"}, {Op::cas, "cas"}, {Op::sharded, "sharded"}};
    for (const auto& [op, name] : ops) {
        for (int t : thread_counts(int(cores.size()))) {
            const std::vector<int> cpus(cores.begin(), cores.begin() + t);
            std::vector<double> v;
            double retries = 0;
            for (size_t s = 0; s < cfg.sweep_samples + 1; ++s) {  // the first sample warms up
                const RaceResult r = race(cpus, op);
                if (r.ns < 0) {
                    ctx.add_result("multicore.atomic_contention", {{"op", name}, {"threads", std::to_string(t)}}, {},
                                   "pinning failed");
                    return;
                }
                if (s == 0) continue;
                v.push_back(r.ns);
                retries += r.retries;
            }
            ctx.add_result("multicore.atomic_contention", {{"op", name}, {"threads", std::to_string(t)}}, std::move(v));
            olt::Result& res = ctx.results().back();
            res.unit = "ns/op";
            if (op == Op::cas) res.metrics.push_back({"retries", retries / double(cfg.sweep_samples)});
        }
    }
}

OLT_BENCHMARK("multicore.atomic_order",
              "Memory ordering cost on one core, uncontended: ns per atomic load, store and fetch_add with "
              "relaxed, acquire/release and seq_cst order, and a seq_cst fence")
{
    Counter c;
    std::atomic<uint64_t>& a = c.v;
    alignas(64) volatile uint64_t plain = 0;
    auto measure = [&](const char* name, auto body) {
        ctx.add_result("multicore.atomic_order", {{"op", name}}, ctx.run_samples([&] {
            const uint64_t t0 = olt::now_ns();
            body();
            return double(olt::now_ns() - t0) / double(k_order_ops);
        }));
        ctx.results().back().unit = "ns/op";
    };
    measure("load_relaxed", [&] {
        uint64_t s = 0;
        for (size_t i = 0; i < k_order_ops; ++i) s += a.load(std::memory_order_relaxed);
        olt::do_not_optimize(s);
    });
    measure("load_acquire", [&] {
        uint64_t s = 0;
        for (size_t i = 0; i < k_order_ops; ++i) s += a.load(std::memory_order_acquire);
        olt::do_not_optimize(s);
    });
    measure("load_seq_cst", [&] {
        uint64_t s = 0;
        for (size_t i = 0; i < k_order_ops; ++i) s += a.load(std::memory_order_seq_cst);
        olt::do_not_optimize(s);
    });
    measure("store_relaxed", [&] {
        for (size_t i = 0; i < k_order_ops; ++i) a.store(i, std::memory_order_relaxed);
    });
    measure("store_release", [&] {
        for (size_t i = 0; i < k_order_ops; ++i) a.store(i, std::memory_order_release);
    });
    measure("store_seq_cst", [&] {
        for (size_t i = 0; i < k_order_ops; ++i) a.store(i, std::memory_order_seq_cst);
    });
    measure("fetch_add_relaxed", [&] {
        for (size_t i = 0; i < k_order_ops; ++i) a.fetch_add(1, std::memory_order_relaxed);
    });
    measure("fetch_add_acq_rel", [&] {
        for (size_t i = 0; i < k_order_ops; ++i) a.fetch_add(1, std::memory_order_acq_rel);
    });
    measure("fetch_add_seq_cst", [&] {
        for (size_t i = 0; i < k_order_ops; ++i) a.fetch_add(1, std::memory_order_seq_cst);
    });
    measure("fence_seq_cst", [&] {  // plain store + full fence: what Dekker-style handshakes need
        for (size_t i = 0; i < k_order_ops; ++i) {
            plain = i;
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
    });
}
