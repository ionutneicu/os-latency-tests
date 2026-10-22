# Latency report

Intel(R) Xeon(R) Processor @ 2.10GHz · x86_64 · Linux 6.18.44-fc-v140 [PREEMPT_DYNAMIC]

| | |
|---|---|
| CPUs | 4 online |
| Data caches | L1 48 KiB, 12-way, per core; L2 2 MiB, 16-way, per core; L3 260 MiB, 20-way, shared by CPUs 0-3 |
| Cache line | 64 B |
| Memory | 15.7 GiB |
| Huge pages | THP madvise; sweep used base pages |
| Power | governor n/a, cpuidle none, deep idle blocked during run |
| Perf counters | linux-perf_event: context_switches, cpu_migrations, page_faults |
| Run | 10000 samples, affinity different (CPU 3/2), RT priority 80, mlock on |
| Compiler | gcc 13.3.0 (RelWithDebInfo) |

> [!WARNING]
> Hot path only: the techniques these results justify (spinning instead of blocking, lock-free queues, pinning, cache-sized buffers, arenas, branchless code, intrinsics) trade readability, maintainability and portability for nanoseconds. Apply them only to the measured hot path of ultra-low-latency or high-performance code, after profiling shows it matters, and keep the rest simple and portable. The numbers are for this machine; re-measure on each target.

- [Design hints for this machine](#hints)
- [Memory hierarchy](#memory)
- [Blocking wake-up latency](#sync)
- [Thread lifecycle](#thread)
- [Heap allocator](#alloc)
- [Multicore communication](#multicore)
- [Design patterns](#patterns)
- [All latency results](#table)

<a id="hints"></a>

## Design hints for this machine

- A random access that misses all caches costs about 167x an L1 hit (259 ns vs 1.55 ns). Keep the data a hot loop touches under ~36 KiB per core where possible.
- Working sets up to ~1.5 MiB still get L2 latency (5.23 ns): size per-thread buffers, lookup tables and queues to fit there.
- Beyond the last cache, sequential reads cost 5.53 ns per 64-byte line vs 259 ns per random access: prefer contiguous arrays and streaming over pointer-heavy structures.
- A blocking hand-off (mutex/condvar wake-up) costs 35.8 us typically and 155 us at p99: avoid blocking on the critical path below that budget.
- Starting a thread costs 5.27 us typically but 191 us at p99 (max 1.94 ms): reuse threads from a pool instead of spawning per task.
- False sharing: two cores updating counters in the same cache line pay 51.9 ns per atomic increment instead of 5.63 ns (9x). Keep data written by different threads at least 64 bytes apart (alignas(64) or padding).
- Thread-to-thread hand-off: a blocking mutex+condvar queue takes 11.4 us (p99 23.6 us), a polling lock-free SPSC ring 403 ns (p99 532 ns): 28x faster, at the price of a core spinning while it waits. Give each polling thread its own core.
- Streaming between two threads: lockfree_cached moves 15.2 M items/s, 8.4x more than spinlock.
- Node-based containers: a std::pmr::monotonic_buffer_resource arena makes building a list 5.6x cheaper than new/delete (3.9 ns vs 21.8 ns per node). Use arenas for short-lived, per-request or per-frame data that is freed all at once.
- std::vector: reserve() up front saves 29% of the build cost by avoiding regrowth copies.

<a id="memory"></a>

## Memory hierarchy

Dependent loads in random order through a buffer of the given size, so hardware prefetchers cannot help. Each step up is the working set spilling out of a cache level; dashed lines are the cache sizes reported by the OS. Band: min to p90 across samples.

![Random load latency vs working set](charts/mem-latency.svg)

> Base pages were used, so latency past the TLB reach also includes page walks. Re-run with `--hugepages` to separate TLB misses from cache misses.

Streaming read and write over the same buffer: prefetchers are effective, so the drops are smaller than for random access and show each level's bandwidth.

![Sequential bandwidth vs working set](charts/mem-bandwidth.svg)

### Per-level summary

| data fits in | sizes used | random load | seq read | seq write |
|---|---|---|---|---|
| L1 | up to 36 KiB | 1.55 ns | 45.2 GB/s | 22.7 GB/s |
| L2 | 72 KiB – 1.5 MiB | 5.23 ns | 71.7 GB/s | 34.4 GB/s |
| L3 | 3 MiB – 195 MiB | 158 ns | 20.2 GB/s | 16.8 GB/s |
| DRAM | > 390 MiB | 259 ns | 11.6 GB/s | 6.19 GB/s |

Benchmark guide: [memory hierarchy](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/memory.md), what is measured and how to apply the result in C++.

<a id="sync"></a>

## Blocking wake-up latency

Time from one thread releasing a mutex / signalling a condition variable to the blocked thread running again. The curve reads left to right from the fastest sample to the tail: the further right it rises, the rarer but costlier the outliers. Percentiles with fewer than ~10 samples beyond them are not drawn.

![Blocking wake-up latency: latency by percentile](charts/sync-percentiles.svg)

Benchmark guide: [blocking wake-up](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/sync.md), what is measured and how to apply the result in C++.

<a id="thread"></a>

## Thread lifecycle

Cost of starting a new thread and of joining a finished one. The curve reads left to right from the fastest sample to the tail: the further right it rises, the rarer but costlier the outliers. Percentiles with fewer than ~10 samples beyond them are not drawn.

![Thread lifecycle: latency by percentile](charts/thread-percentiles.svg)

Benchmark guide: [threads: spawn or pool](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/threads.md), what is measured and how to apply the result in C++.

<a id="alloc"></a>

## Heap allocator

Cost of one `malloc()` and one `free()` per block size, in the allocator's best case (the block is reused every iteration and never touched). Solid: p50, dashed: p99.

![malloc / free latency vs block size](charts/alloc-size.svg)

Benchmark guide: [heap allocator](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/alloc.md), what is measured and how to apply the result in C++.

<a id="multicore"></a>

## Multicore communication

Two threads pinned to a pair of CPUs ping-pong one value through a `std::atomic` on its own cache line; each cell is the one-way hand-off latency (half a round trip, p50). This is the floor for any inter-thread communication between those CPUs: SMT siblings share L1/L2 and are fastest, other cores on the same die or cluster come next, other clusters, CCXs or sockets are slowest.

![Core-to-core latency (one-way, p50)](charts/multicore-c2c.svg)

> Fastest pair: CPU 2 ↔ 3 (55.8 ns); slowest: CPU 1 ↔ 2 (87.5 ns); median 77.3 ns. Pin threads that talk a lot (producer/consumer pairs) to a fast pair with `--cpus A,B` or in your application.

Benchmark guide: [core-to-core latency](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/c2c.md), what is measured and how to apply the result in C++.

### False sharing: how far apart must per-thread data be?

Threads on CPU 3 and CPU 0 (different physical cores) each increment their own counter; the counters sit the given distance apart. While both share a cache line, every write pulls the line away from the other core. Atomic read-modify-writes pay each transfer; plain stores hide most of it in the store buffer, so the penalty there is smaller but not zero. Dashed: the same loop on one core alone. Band: min to p90.

![False sharing: cost per increment vs distance between counters](charts/multicore-false-sharing.svg)

> On this CPU, counters written by different threads need at least 64 bytes between them (vertical line: the 64-byte cache line).

Benchmark guide: [false sharing](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/false-sharing.md), what is measured and how to apply the result in C++.

<a id="patterns"></a>

## Design patterns

### Container memory: heap vs reserve vs pmr arenas

Build and destroy a 256-element container; cost per element (p50, shorter is better, compared with the fastest in brackets). Node containers allocate once per element, so the allocator matters far more there.

![std::vector&lt;int&gt;, 256 push_back: ns per element](charts/pattern-alloc-vector.svg)

![std::list&lt;int&gt;, 256 push_back: ns per element](charts/pattern-alloc-list.svg)

Benchmark guide: [container memory](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/pattern-alloc.md), what is measured and how to apply the result in C++.

### Single producer / single consumer: mutex vs spinlock vs lock-free

The same bounded ring hand-off synchronised five ways. Latency is half a ping-pong round trip; throughput streams 256 Ki items through one queue.

![SPSC hand-off latency by percentile](charts/pattern-spsc-latency.svg)

![SPSC streaming throughput](charts/pattern-spsc-throughput.svg)

What it costs: CPU time burnt by producer plus consumer during the latency test, divided by wall time. 2.0 means both threads kept a core fully busy (polling); low values mean they slept while waiting.

![SPSC CPU usage while handing off](charts/pattern-spsc-cpu.svg)

Benchmark guide: [SPSC queues](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/pattern-spsc.md), what is measured and how to apply the result in C++.

<a id="table"></a>

## All latency results

Grey values have fewer than ~10 samples beyond the percentile and are not statistically reliable.

| benchmark | n | min | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|---|
| alloc.malloc [size=16] | 10000 | 29 ns | 41 ns | 43 ns | 46 ns | 53 ns | 3.55 us | 16.5 us |
| alloc.free [size=16] | 10000 | 30 ns | 43 ns | 45 ns | 47 ns | 55 ns | 207 ns | 3.72 us |
| alloc.malloc [size=64] | 10000 | 29 ns | 41 ns | 43 ns | 46 ns | 53 ns | 93 ns | 182 ns |
| alloc.free [size=64] | 10000 | 30 ns | 43 ns | 45 ns | 48 ns | 63 ns | 286 ns | 5.77 us |
| alloc.malloc [size=256] | 10000 | 29 ns | 41 ns | 43 ns | 46 ns | 53 ns | 3.56 us | 3.59 us |
| alloc.free [size=256] | 10000 | 30 ns | 43 ns | 45 ns | 48 ns | 56 ns | 394 ns | 47.4 us |
| alloc.malloc [size=1K] | 10000 | 30 ns | 41 ns | 43 ns | 45 ns | 51 ns | 166 ns | 5 us |
| alloc.free [size=1K] | 10000 | 30 ns | 43 ns | 45 ns | 47 ns | 55 ns | 324 ns | 14.9 us |
| alloc.malloc [size=4K] | 10000 | 58 ns | 82 ns | 86 ns | 91 ns | 218 ns | 3.59 us | 16.9 us |
| alloc.free [size=4K] | 10000 | 46 ns | 63 ns | 66 ns | 71 ns | 121 ns | 3.59 us | 43.9 us |
| alloc.malloc [size=16K] | 10000 | 55 ns | 74 ns | 78 ns | 86 ns | 210 ns | 22.9 us | 23.3 us |
| alloc.free [size=16K] | 10000 | 45 ns | 62 ns | 65 ns | 74 ns | 109 ns | 315 ns | 376 ns |
| alloc.malloc [size=64K] | 10000 | 55 ns | 75 ns | 78 ns | 86 ns | 172 ns | 262 ns | 291 ns |
| alloc.free [size=64K] | 10000 | 46 ns | 63 ns | 65 ns | 71 ns | 91 ns | 182 ns | 14.7 us |
| alloc.malloc [size=256K] | 10000 | 43 ns | 60 ns | 63 ns | 67 ns | 156 ns | 243 ns | 379 ns |
| alloc.free [size=256K] | 10000 | 43 ns | 59 ns | 61 ns | 64 ns | 79 ns | 5.48 us | 21.9 us |
| alloc.malloc [size=1M] | 10000 | 42 ns | 60 ns | 63 ns | 83 ns | 230 ns | 22.1 us | 51 us |
| alloc.free [size=1M] | 10000 | 42 ns | 58 ns | 61 ns | 65 ns | 87 ns | 5.69 us | 22.8 us |
| alloc.malloc [size=4M] | 10000 | 42 ns | 60 ns | 63 ns | 173 ns | 320 ns | 37.4 us | 42.9 us |
| alloc.free [size=4M] | 10000 | 42 ns | 58 ns | 60 ns | 63 ns | 80 ns | 8.47 us | 17.1 us |
| alloc.malloc [size=16M] | 10000 | 43 ns | 60 ns | 63 ns | 235 ns | 393 ns | 433 ns | 27.8 us |
| alloc.free [size=16M] | 10000 | 42 ns | 58 ns | 61 ns | 64 ns | 82 ns | 237 ns | 3.59 us |
| multicore.false_sharing_alone [op=plain, cpu_a=3, cpu_b=0] | 15 | 0.305 ns | 0.306 ns | 0.323 ns | 0.337 ns | 0.337 ns | 0.337 ns | 0.337 ns |
| multicore.false_sharing [op=plain, distance=8, cpu_a=3, cpu_b=0] | 15 | 0.325 ns | 0.342 ns | 0.425 ns | 0.446 ns | 0.447 ns | 0.447 ns | 0.447 ns |
| multicore.false_sharing [op=plain, distance=16, cpu_a=3, cpu_b=0] | 15 | 0.325 ns | 0.328 ns | 0.428 ns | 0.505 ns | 0.511 ns | 0.512 ns | 0.512 ns |
| multicore.false_sharing [op=plain, distance=32, cpu_a=3, cpu_b=0] | 15 | 0.324 ns | 0.329 ns | 0.357 ns | 0.37 ns | 0.371 ns | 0.371 ns | 0.371 ns |
| multicore.false_sharing [op=plain, distance=64, cpu_a=3, cpu_b=0] | 15 | 0.306 ns | 0.307 ns | 0.48 ns | 0.585 ns | 0.597 ns | 0.598 ns | 0.598 ns |
| multicore.false_sharing [op=plain, distance=128, cpu_a=3, cpu_b=0] | 15 | 0.306 ns | 0.307 ns | 0.376 ns | 0.441 ns | 0.447 ns | 0.447 ns | 0.448 ns |
| multicore.false_sharing [op=plain, distance=256, cpu_a=3, cpu_b=0] | 15 | 0.307 ns | 0.307 ns | 0.327 ns | 0.385 ns | 0.392 ns | 0.393 ns | 0.393 ns |
| multicore.false_sharing [op=plain, distance=512, cpu_a=3, cpu_b=0] | 15 | 0.306 ns | 0.308 ns | 0.493 ns | 0.591 ns | 0.602 ns | 0.603 ns | 0.603 ns |
| multicore.false_sharing_alone [op=atomic, cpu_a=3, cpu_b=0] | 15 | 5.46 ns | 5.63 ns | 5.9 ns | 6.15 ns | 6.19 ns | 6.19 ns | 6.19 ns |
| multicore.false_sharing [op=atomic, distance=8, cpu_a=3, cpu_b=0] | 15 | 19.9 ns | 44.7 ns | 52.1 ns | 53 ns | 53.1 ns | 53.1 ns | 53.1 ns |
| multicore.false_sharing [op=atomic, distance=16, cpu_a=3, cpu_b=0] | 15 | 21.3 ns | 44.7 ns | 56.3 ns | 58.2 ns | 58.4 ns | 58.4 ns | 58.4 ns |
| multicore.false_sharing [op=atomic, distance=32, cpu_a=3, cpu_b=0] | 15 | 39.7 ns | 51.9 ns | 62.6 ns | 62.9 ns | 63 ns | 63 ns | 63 ns |
| multicore.false_sharing [op=atomic, distance=64, cpu_a=3, cpu_b=0] | 15 | 5.6 ns | 5.66 ns | 5.99 ns | 6.14 ns | 6.15 ns | 6.15 ns | 6.15 ns |
| multicore.false_sharing [op=atomic, distance=128, cpu_a=3, cpu_b=0] | 15 | 5.69 ns | 6.03 ns | 6.66 ns | 6.75 ns | 6.77 ns | 6.77 ns | 6.77 ns |
| multicore.false_sharing [op=atomic, distance=256, cpu_a=3, cpu_b=0] | 15 | 5.48 ns | 5.83 ns | 7.4 ns | 8.93 ns | 9.13 ns | 9.15 ns | 9.15 ns |
| multicore.false_sharing [op=atomic, distance=512, cpu_a=3, cpu_b=0] | 15 | 5.93 ns | 6.81 ns | 9.95 ns | 12.3 ns | 12.6 ns | 12.6 ns | 12.6 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=1] | 15 | 73.3 ns | 74.6 ns | 84.8 ns | 96.3 ns | 97.8 ns | 98 ns | 98 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=2] | 15 | 85.1 ns | 86.3 ns | 93.1 ns | 106 ns | 107 ns | 108 ns | 108 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=3] | 15 | 77.2 ns | 80 ns | 96.1 ns | 338 ns | 373 ns | 377 ns | 377 ns |
| multicore.c2c_latency [cpu_a=1, cpu_b=2] | 15 | 84.8 ns | 87.5 ns | 102 ns | 117 ns | 119 ns | 119 ns | 119 ns |
| multicore.c2c_latency [cpu_a=1, cpu_b=3] | 15 | 67.9 ns | 69.7 ns | 105 ns | 197 ns | 208 ns | 209 ns | 210 ns |
| multicore.c2c_latency [cpu_a=2, cpu_b=3] | 15 | 55.6 ns | 55.8 ns | 64.6 ns | 70.1 ns | 70.3 ns | 70.3 ns | 70.3 ns |
| pattern.alloc_vector [allocator=new_delete] | 10000 | 1.11 ns | 1.13 ns | 1.71 ns | 2.5 ns | 12.7 ns | 63.2 ns | 64.3 ns |
| pattern.alloc_vector [allocator=reserve] | 10000 | 0.781 ns | 0.797 ns | 0.965 ns | 1.55 ns | 2.43 ns | 51.7 ns | 62.2 ns |
| pattern.alloc_vector [allocator=pmr_monotonic] | 10000 | 1.02 ns | 1.37 ns | 1.83 ns | 2.41 ns | 15.3 ns | 103 ns | 110 ns |
| pattern.alloc_vector [allocator=pmr_pool] | 10000 | 1.53 ns | 1.59 ns | 2.34 ns | 2.97 ns | 19.7 ns | 94.5 ns | 673 ns |
| pattern.alloc_list [allocator=new_delete] | 10000 | 21.4 ns | 21.8 ns | 27.6 ns | 42.3 ns | 142 ns | 561 ns | 626 ns |
| pattern.alloc_list [allocator=pmr_monotonic] | 10000 | 3.84 ns | 3.9 ns | 4.19 ns | 6.18 ns | 49.4 ns | 72.6 ns | 279 ns |
| pattern.alloc_list [allocator=pmr_pool] | 10000 | 13.6 ns | 17.6 ns | 24.7 ns | 38.2 ns | 152 ns | 7.94 us | 9.39 us |
| pattern.spsc_latency [queue=mutex_condvar] | 10000 | 376 ns | 11.4 us | 12.2 us | 23.6 us | 38.7 us | 97.6 us | 105 us |
| pattern.spsc_throughput [queue=mutex_condvar] | 15 | 262 ns | 331 ns | 371 ns | 395 ns | 397 ns | 397 ns | 397 ns |
| pattern.spsc_latency [queue=mutex_poll] | 10000 | 516 ns | 1.72 us | 2.51 us | 15 us | 30.5 us | 48.8 us | 78.8 us |
| pattern.spsc_throughput [queue=mutex_poll] | 15 | 194 ns | 243 ns | 279 ns | 365 ns | 376 ns | 377 ns | 377 ns |
| pattern.spsc_latency [queue=spinlock] | 10000 | 348 ns | 791 ns | 982 ns | 1.26 us | 11.9 us | 53.8 us | 57.7 us |
| pattern.spsc_throughput [queue=spinlock] | 15 | 461 ns | 553 ns | 661 ns | 809 ns | 830 ns | 832 ns | 832 ns |
| pattern.spsc_latency [queue=lockfree] | 10000 | 222 ns | 370 ns | 451 ns | 531 ns | 13.8 us | 60.2 us | 97.9 us |
| pattern.spsc_throughput [queue=lockfree] | 15 | 63.6 ns | 69.3 ns | 92.2 ns | 159 ns | 168 ns | 169 ns | 169 ns |
| pattern.spsc_latency [queue=lockfree_cached] | 10000 | 288 ns | 403 ns | 486 ns | 532 ns | 8.92 us | 17 us | 18.6 us |
| pattern.spsc_throughput [queue=lockfree_cached] | 15 | 58.6 ns | 65.8 ns | 69.4 ns | 77.3 ns | 78.3 ns | 78.5 ns | 78.5 ns |
| sync.mutex_wakeup | 10000 | 5.74 us | 35.8 us | 78.3 us | 155 us | 1.23 ms | 3.78 ms | 10.4 ms |
| sync.condvar_wakeup | 10000 | 6.5 us | 35.9 us | 87.8 us | 166 us | 678 us | 2.2 ms | 2.22 ms |
| thread.create_to_start | 10000 | 3.9 us | 5.27 us | 25.6 us | 191 us | 454 us | 761 us | 1.94 ms |
| thread.exit_to_join | 10000 | 2.77 us | 3.1 us | 4.3 us | 9.7 us | 52.7 us | 181 us | 1.78 ms |

*Generated from results.json by tools/olt-report.py. An interactive version with hover details is in report.html next to this file.*
