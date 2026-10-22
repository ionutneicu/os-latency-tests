// Design pattern: single-producer / single-consumer hand-off.
//
// The same bounded ring of 64-bit items, synchronised four ways:
//
//   mutex_condvar  std::mutex + std::condition_variable; the consumer blocks
//   mutex_poll     std::mutex; the consumer polls try_pop() in a spin loop
//   spinlock       test-and-test-and-set spinlock (std::atomic + cpu_relax)
//   lockfree       lock-free SPSC ring: producer and consumer each own one
//                  index on its own cache line, acquire/release ordering
//   lockfree_cached  same ring, but each side keeps a private copy of the
//                  other side's index and only re-reads the shared one when
//                  the copy says full/empty, so the index cache lines stop
//                  bouncing between cores on every item
//
// pattern.spsc_latency     one-way hand-off latency, measured as half the
//                          round trip of a ping-pong over two queues (ns)
// pattern.spsc_throughput  cost per item when streaming 256 Ki items through
//                          one queue as fast as possible (ns/item)
//
// Each result also carries metric cpu_cores: CPU time used by producer plus
// consumer divided by wall time (2.0 = both threads kept a core fully busy,
// near 0 = they mostly slept). This is the price paid for polling.
//
// Polling loops fall back to yield() after a while so that two threads
// pinned to the same CPU still make progress.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/hal/arch.h"
#include "olt/hal/os.h"

namespace {

constexpr size_t k_capacity = 1024;  // power of two
constexpr size_t k_stream_items = size_t(1) << 18;
constexpr size_t k_cache_line = 64;

inline void backoff(unsigned& spins)
{
    if (++spins < 4096) olt_cpu_relax();
    else std::this_thread::yield();
}

class Ring {
public:
    bool push(uint64_t v)
    {
        if (tail_ - head_ == k_capacity) return false;
        buf_[tail_++ & (k_capacity - 1)] = v;
        return true;
    }
    bool pop(uint64_t& v)
    {
        if (head_ == tail_) return false;
        v = buf_[head_++ & (k_capacity - 1)];
        return true;
    }
    bool empty() const { return head_ == tail_; }

private:
    uint64_t buf_[k_capacity];
    size_t head_ = 0, tail_ = 0;
};

struct MutexCondvarQueue {
    static constexpr const char* name = "mutex_condvar";
    std::mutex m;
    std::condition_variable not_empty, not_full;
    Ring ring;

    void push(uint64_t v)
    {
        std::unique_lock<std::mutex> lk(m);
        not_full.wait(lk, [&] { return ring.push(v); });
        lk.unlock();
        not_empty.notify_one();
    }
    uint64_t pop()
    {
        uint64_t v;
        std::unique_lock<std::mutex> lk(m);
        not_empty.wait(lk, [&] { return ring.pop(v); });
        lk.unlock();
        not_full.notify_one();
        return v;
    }
};

struct MutexPollQueue {
    static constexpr const char* name = "mutex_poll";
    std::mutex m;
    Ring ring;

    void push(uint64_t v)
    {
        for (unsigned spins = 0;; backoff(spins)) {
            std::lock_guard<std::mutex> lk(m);
            if (ring.push(v)) return;
        }
    }
    uint64_t pop()
    {
        uint64_t v;
        for (unsigned spins = 0;; backoff(spins)) {
            std::lock_guard<std::mutex> lk(m);
            if (ring.pop(v)) return v;
        }
    }
};

class Spinlock {
public:
    void lock()
    {
        for (unsigned spins = 0;;) {
            if (!locked_.exchange(true, std::memory_order_acquire)) return;
            while (locked_.load(std::memory_order_relaxed)) backoff(spins);
        }
    }
    void unlock() { locked_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> locked_{false};
};

struct SpinlockQueue {
    static constexpr const char* name = "spinlock";
    Spinlock lock;
    Ring ring;

    void push(uint64_t v)
    {
        for (unsigned spins = 0;; backoff(spins)) {
            std::lock_guard<Spinlock> lk(lock);
            if (ring.push(v)) return;
        }
    }
    uint64_t pop()
    {
        uint64_t v;
        for (unsigned spins = 0;; backoff(spins)) {
            std::lock_guard<Spinlock> lk(lock);
            if (ring.pop(v)) return v;
        }
    }
};

struct LockFreeQueue {
    static constexpr const char* name = "lockfree";
    alignas(k_cache_line) std::atomic<size_t> head{0};  // written by the consumer only
    alignas(k_cache_line) std::atomic<size_t> tail{0};  // written by the producer only
    alignas(k_cache_line) uint64_t buf[k_capacity];

    void push(uint64_t v)
    {
        const size_t t = tail.load(std::memory_order_relaxed);
        for (unsigned spins = 0; t - head.load(std::memory_order_acquire) == k_capacity;) backoff(spins);
        buf[t & (k_capacity - 1)] = v;
        tail.store(t + 1, std::memory_order_release);
    }
    uint64_t pop()
    {
        const size_t h = head.load(std::memory_order_relaxed);
        for (unsigned spins = 0; tail.load(std::memory_order_acquire) == h;) backoff(spins);
        const uint64_t v = buf[h & (k_capacity - 1)];
        head.store(h + 1, std::memory_order_release);
        return v;
    }
};

struct LockFreeCachedQueue {
    static constexpr const char* name = "lockfree_cached";
    alignas(k_cache_line) std::atomic<size_t> head{0};
    size_t tail_cache = 0;  // consumer's copy of tail
    alignas(k_cache_line) std::atomic<size_t> tail{0};
    size_t head_cache = 0;  // producer's copy of head
    alignas(k_cache_line) uint64_t buf[k_capacity];

    void push(uint64_t v)
    {
        const size_t t = tail.load(std::memory_order_relaxed);
        if (t - head_cache == k_capacity) {
            for (unsigned spins = 0; t - (head_cache = head.load(std::memory_order_acquire)) == k_capacity;)
                backoff(spins);
        }
        buf[t & (k_capacity - 1)] = v;
        tail.store(t + 1, std::memory_order_release);
    }
    uint64_t pop()
    {
        const size_t h = head.load(std::memory_order_relaxed);
        if (h == tail_cache) {
            for (unsigned spins = 0; (tail_cache = tail.load(std::memory_order_acquire)) == h;) backoff(spins);
        }
        const uint64_t v = buf[h & (k_capacity - 1)];
        head.store(h + 1, std::memory_order_release);
        return v;
    }
};

template <class Q>
void measure(olt::Context& ctx)
{
    const olt::Config& cfg = ctx.config();
    const olt::Params params{{"queue", Q::name}};

    // ping-pong latency
    {
        const size_t total = cfg.warmup + cfg.samples;
        auto ping = std::make_unique<Q>();
        auto pong = std::make_unique<Q>();
        uint64_t echo_cpu = 0;
        std::thread echo([&] {
            ctx.setup_worker_thread();
            const uint64_t c0 = olt_os_thread_cpu_ns();
            for (size_t i = 0; i < total; ++i) pong->push(ping->pop());
            echo_cpu = olt_os_thread_cpu_ns() - c0;
        });
        std::vector<double> lat;
        lat.reserve(cfg.samples);
        const uint64_t wall0 = olt::now_ns();
        const uint64_t cpu0 = olt_os_thread_cpu_ns();
        for (size_t i = 0; i < total; ++i) {
            const uint64_t t0 = olt::now_ns();
            ping->push(t0);
            const uint64_t back = pong->pop();
            const uint64_t t1 = olt::now_ns();
            if (i >= cfg.warmup) lat.push_back(double(t1 - back) / 2.0);
        }
        const uint64_t main_cpu = olt_os_thread_cpu_ns() - cpu0;
        echo.join();
        const uint64_t wall = olt::now_ns() - wall0;
        ctx.add_result("pattern.spsc_latency", params, std::move(lat));
        ctx.results().back().metrics.push_back({"cpu_cores", double(main_cpu + echo_cpu) / double(wall)});
    }

    // streaming throughput
    {
        std::vector<double> per_item;
        auto q = std::make_unique<Q>();
        uint64_t cpu_total = 0, wall_total = 0;
        for (size_t s = 0; s < cfg.sweep_samples + 1; ++s) {  // first run is warm-up
            std::atomic<bool> go{false};
            uint64_t checksum = 0, consumer_cpu = 0;
            std::thread consumer([&] {
                ctx.setup_worker_thread();
                while (!go.load(std::memory_order_acquire)) olt_cpu_relax();
                const uint64_t c0 = olt_os_thread_cpu_ns();
                for (size_t i = 0; i < k_stream_items; ++i) checksum += q->pop();
                consumer_cpu = olt_os_thread_cpu_ns() - c0;
            });
            const uint64_t t0 = olt::now_ns();
            const uint64_t c0 = olt_os_thread_cpu_ns();
            go.store(true, std::memory_order_release);
            for (size_t i = 0; i < k_stream_items; ++i) q->push(i);
            const uint64_t producer_cpu = olt_os_thread_cpu_ns() - c0;
            consumer.join();
            const uint64_t t1 = olt::now_ns();
            olt::do_not_optimize(checksum);
            if (s > 0) {
                per_item.push_back(double(t1 - t0) / double(k_stream_items));
                cpu_total += producer_cpu + consumer_cpu;
                wall_total += t1 - t0;
            }
        }
        ctx.add_result("pattern.spsc_throughput", params, std::move(per_item));
        ctx.results().back().unit = "ns/item";
        ctx.results().back().metrics.push_back({"cpu_cores", double(cpu_total) / double(wall_total)});
    }
}

}  // namespace

OLT_BENCHMARK("pattern.spsc",
              "Single-producer/single-consumer queue: mutex+condvar, mutex polling, spinlock, "
              "lock-free ring and lock-free ring with cached indices; ping-pong hand-off latency (pattern.spsc_latency) and streaming cost "
              "per item (pattern.spsc_throughput)")
{
    measure<MutexCondvarQueue>(ctx);
    measure<MutexPollQueue>(ctx);
    measure<SpinlockQueue>(ctx);
    measure<LockFreeQueue>(ctx);
    measure<LockFreeCachedQueue>(ctx);
}
