# OS jitter: how often is my core taken away?

**Benchmark:** `os.jitter` → results `os.jitter_gap`, `os.jitter_rate` per threshold ·
source: [src/benchmarks/jitter.cpp](../../src/benchmarks/jitter.cpp)

## Goal

Find out how much time a thread loses to the rest of the system while it runs
on a core: interrupts, timers, kernel threads, other processes. The answer is
the floor under every latency budget on that core. A deadline shorter than the
longest gap cannot be met there, however fast the code is.

## What is measured

A tight loop reads the clock as fast as it can and looks at the gap between
consecutive reads:

```cpp
uint64_t prev = now_ns();
for (;;) {
    const uint64_t t = now_ns();
    const uint64_t gap = t - prev;   // undisturbed: one clock read, tens of ns
    prev = t;
    if (gap > 1000) ++over_1us;      // this thread was not running for that long
    // ...
}
```

Windows of 20 ms, on the main benchmark CPU. The results are:

* `os.jitter_gap`: the longest gap in each window, in ns.
* `os.jitter_rate [threshold_us]`: gaps longer than 1 and 10 µs per second.
  A 10 µs gap also counts as a 1 µs gap.

Each window is independent. The percentiles are over windows: p50 is a typical
window and p99 the worst 1%. The mean rate in the JSON is the average over all
windows, the figure to compare between machines.

## Reading the result

* **Undisturbed windows** have a longest gap of a few microseconds and no
  gaps above 10 µs. On an idle, tuned machine most windows look like this.
* **Interruptions** are the gaps above 1 µs. Each one is something else
  running on this CPU: a timer tick, a device interrupt, a kernel worker, a
  process that was scheduled here.
* **Long gaps** (hundreds of µs to milliseconds) are the ones that break
  deadlines. On a VM they can be the hypervisor taking the CPU away, which
  this benchmark cannot tell apart from a guest task.
* **Compare the mean rate**, not one window: the rate changes with what else
  the machine does during the run. Run on an idle machine, with nothing else
  running, for a fair figure.

The loop itself is what the OS sees, so the number includes every source of
noise on the core, including the benchmark's own clock reads, which cost
about 14 ns and never show up as gaps.

## Using it in C++

* **Budget deadlines above the tail.** If p99 of the longest gap is 3 ms, a
  1 ms deadline on that core fails about one time in a hundred windows. Look
  at the tail the report shows, not the average.
* **Keep the critical thread on a CPU nothing else uses.** Isolated CPUs
  (`isolcpus=`, `nohz_full=`, `rcu_nocbs=`) remove most of the kernel's own
  work from a core. That needs the kernel command line, so it sits in the
  expert tier: see [expert-setup.md](../expert-setup.md) for what to expect.
* **Move housekeeping away.** IRQ affinity (`/proc/irq/*/smp_affinity`) keeps
  device interrupts off the critical core. Some of this is standard
  configuration, not expert tuning.
* **Measure your own loop too.** The same gap loop, run in your process, shows
  the jitter that your deployment actually gets, with your threads and your
  load.
