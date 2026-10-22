# Blocking wake-up: mutex and condition variable

**Benchmarks:** `sync.mutex_wakeup`, `sync.condvar_wakeup` ·
source: [src/benchmarks/sync.cpp](../../src/benchmarks/sync.cpp)

## Goal

Measure the price of letting a thread sleep in the kernel and waking it: the
floor for any design built on `std::mutex` contention or
`std::condition_variable` hand-offs.

## What is measured

A persistent worker and the main thread take turns. The waiting side is first
given time to really block in the kernel (`--park-us`, default 200 us), then
the other side timestamps and releases it:

```cpp
// worker                                // main thread
m.lock();                                //
...                                      // m.lock();   <- blocks in futex_wait
olt::park(cfg.park_us);                  //
t_release = olt::now_ns();               //
m.unlock();                              // returns: sample = now_ns() - t_release
```

The condvar variant does the same with `cv.notify_one()` and a predicate wait.
The time covers the futex wake system call, the scheduler deciding to run the
waiter, and, if that CPU was idle, its exit from a C-state.

## Reading the result

* With deep idle allowed, p50 is often 10 to 60 us on laptops: most of it is the
  CPU waking up. With `--no-idle` (C-states blocked) it drops to 1 to 5 us, the
  real software cost.
* The tail (p99.9, max) shows scheduler and interrupt interference; real-time
  priority (`-p`) and isolated cores reduce it.

## Using it in C++

Blocking is the right default: it costs no CPU while waiting. On a hot path
whose budget is below the measured wake-up p99, avoid blocking:

```cpp
// Instead of a condition variable hand-off...
std::unique_lock lk(m);
cv.wait(lk, [&] { return ready; });

// ...poll a flag, with a pause instruction, on a core you own.
while (!ready.load(std::memory_order_acquire))
    _mm_pause();                       // or __builtin_ia32_pause / `yield` on ARM
```

* Polling burns a whole core and only pays off when the waiter has a CPU to
  itself; the SPSC page measures that trade-off directly.
* A middle ground: spin for a bounded time, then block (spin-then-park).
* If you must block, keep the core out of deep C-states for the latency-critical
  phase rather than spinning: PM QoS, through `/dev/cpu_dma_latency` for all
  CPUs or `pm_qos_resume_latency_us` for one, which is what `--no-idle` does.
