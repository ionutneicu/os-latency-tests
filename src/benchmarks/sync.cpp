// Blocking synchronisation wake-up latencies between two threads.
//
// A persistent worker thread is used so that thread creation cost does not
// leak into the measurement. In every iteration the waiting side is first
// allowed to block in the kernel (--park-us), then the other side records a
// timestamp and releases it; the sample is the time until the waiter runs.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "olt/benchmark.hpp"
#include "sync_util.hpp"

OLT_BENCHMARK("sync.mutex_wakeup",
              "Main thread blocks in std::mutex::lock() while the worker holds the mutex; time from "
              "the worker's unlock() to lock() returning in the main thread")
{
    const olt::Config& cfg = ctx.config();
    const size_t total = cfg.warmup + cfg.samples;
    std::mutex m;
    std::atomic<size_t> locked{0}, arrived{0}, done{0};
    uint64_t t_release = 0;  // written before unlock(), read after lock(): ordered by the mutex

    std::thread worker([&] {
        ctx.setup_worker_thread();
        for (size_t i = 1; i <= total; ++i) {
            m.lock();
            locked.store(i, std::memory_order_release);
            olt::wait_for(arrived, i);
            olt::park(cfg.park_us);
            t_release = olt::now_ns();
            m.unlock();
            olt::wait_for(done, i);
        }
    });

    std::vector<double> samples;
    samples.reserve(cfg.samples);
    for (size_t i = 1; i <= total; ++i) {
        olt::wait_for(locked, i);
        arrived.store(i, std::memory_order_release);
        m.lock();
        const uint64_t t_wake = olt::now_ns();
        const uint64_t released = t_release;
        m.unlock();
        if (i > cfg.warmup) samples.push_back(static_cast<double>(t_wake - released));
        done.store(i, std::memory_order_release);
    }
    worker.join();
    ctx.add_result("sync.mutex_wakeup", {}, std::move(samples));
}

OLT_BENCHMARK("sync.condvar_wakeup",
              "Main thread waits on std::condition_variable; time from the worker setting the "
              "predicate (before unlock + notify_one) to wait() returning in the main thread")
{
    const olt::Config& cfg = ctx.config();
    const size_t total = cfg.warmup + cfg.samples;
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    uint64_t t_release = 0;  // protected by m
    std::atomic<size_t> arrived{0}, done{0};

    std::thread worker([&] {
        ctx.setup_worker_thread();
        for (size_t i = 1; i <= total; ++i) {
            olt::wait_for(arrived, i);
            olt::park(cfg.park_us);
            {
                std::lock_guard<std::mutex> lk(m);
                t_release = olt::now_ns();
                ready = true;
            }
            cv.notify_one();  // after unlock, so the waiter does not wake into a held mutex
            olt::wait_for(done, i);
        }
    });

    std::vector<double> samples;
    samples.reserve(cfg.samples);
    for (size_t i = 1; i <= total; ++i) {
        uint64_t t_wake, released;
        {
            std::unique_lock<std::mutex> lk(m);
            arrived.store(i, std::memory_order_release);
            cv.wait(lk, [&] { return ready; });
            t_wake = olt::now_ns();
            released = t_release;
            ready = false;
        }
        if (i > cfg.warmup) samples.push_back(static_cast<double>(t_wake - released));
        done.store(i, std::memory_order_release);
    }
    worker.join();
    ctx.add_result("sync.condvar_wakeup", {}, std::move(samples),
                   "includes the worker's mutex unlock and notify_one() call");
}
