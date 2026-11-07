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

* The p50 depends on the idle state of the sleeping core. On the laptop, after
  the optimised rebuild (issue #12), it was 1.9 us with C1 allowed and 1.4 us with
  polling only (issue #11). A deeper idle state adds its exit time to every
  wake-up. A second cluster at 10 to 20 us can appear when C1E is entered (see the
  glossary); `--idle-latency-us 0` measures without idle exits. Figures from before
  the rebuild came from an unoptimised build and are not valid.
* The tail (p99.9, max) shows scheduler and interrupt interference; real-time
  priority (`-p`) and isolated cores reduce it.
* `over_100us` and `over_1ms` in the JSON are the share of wake-ups slower than
  100 us and 1 ms. They stay comparable across sample counts, which a tail
  percentile does not.

## The deep tail

A percentile needs about 10 samples beyond it, so p99.99 needs 100,000
wake-ups and p99.999 needs 1,000,000. Each sample waits `--park-us` (200 us by
default) so the peer is really asleep, so a million samples take about 200
seconds per benchmark. The default run stops at 10,000 samples and marks the
deeper percentiles unreliable.

To read the deep tail, raise the sample count for these benchmarks only:

    os-latency-tests -t sync.mutex_wakeup -n 1000000 --cpus 2,3 --yes

Run it with real-time priority and a quiet machine. The deep tail mostly shows
interrupts, other tasks and frequency changes, not the lock: unprivileged, it
says little about the code.

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
