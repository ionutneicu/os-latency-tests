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

- A random access that misses all caches costs about 175x an L1 hit (292 ns vs 1.67 ns). Keep the data a hot loop touches under ~36 KiB per core where possible.
- Working sets up to ~1.5 MiB still get L2 latency (6.14 ns): size per-thread buffers, lookup tables and queues to fit there.
- Beyond the last cache, sequential reads cost 8.1 ns per 64-byte line vs 292 ns per random access: prefer contiguous arrays and streaming over pointer-heavy structures.
- A blocking hand-off (mutex/condvar wake-up) costs 39.1 us typically and 215 us at p99: avoid blocking on the critical path below that budget.
- Starting a thread costs 6.72 us typically but 79 us at p99 (max 5.06 ms): reuse threads from a pool instead of spawning per task.
- Thread-to-thread hand-off: a blocking mutex+condvar queue takes 13.4 us (p99 38.2 us), a polling lock-free SPSC ring 387 ns (p99 632 ns): 35x faster, at the price of a core spinning while it waits. Give each polling thread its own core.
- Streaming between two threads: lockfree_cached moves 29.7 M items/s, 12.1x more than spinlock.
- Node-based containers: a std::pmr::monotonic_buffer_resource arena makes building a list 4.2x cheaper than new/delete (6.88 ns vs 29.1 ns per node). Use arenas for short-lived, per-request or per-frame data that is freed all at once.
- std::vector: reserve() up front saves 27% of the build cost by avoiding regrowth copies.

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
| L1 | up to 36 KiB | 1.67 ns | 44 GB/s | 20.1 GB/s |
| L2 | 72 KiB – 1.5 MiB | 6.14 ns | 49.6 GB/s | 21.3 GB/s |
| L3 | 3 MiB – 195 MiB | 176 ns | 17.8 GB/s | 12.9 GB/s |
| DRAM | > 390 MiB | 292 ns | 7.9 GB/s | 5.97 GB/s |

<a id="sync"></a>

## Blocking wake-up latency

Time from one thread releasing a mutex / signalling a condition variable to the blocked thread running again. The curve reads left to right from the fastest sample to the tail: the further right it rises, the rarer but costlier the outliers. Percentiles with fewer than ~10 samples beyond them are not drawn.

![Blocking wake-up latency: latency by percentile](charts/sync-percentiles.svg)

<a id="thread"></a>

## Thread lifecycle

Cost of starting a new thread and of joining a finished one. The curve reads left to right from the fastest sample to the tail: the further right it rises, the rarer but costlier the outliers. Percentiles with fewer than ~10 samples beyond them are not drawn.

![Thread lifecycle: latency by percentile](charts/thread-percentiles.svg)

<a id="alloc"></a>

## Heap allocator

Cost of one `malloc()` and one `free()` per block size, in the allocator's best case (the block is reused every iteration and never touched). Solid: p50, dashed: p99.

![malloc / free latency vs block size](charts/alloc-size.svg)

<a id="multicore"></a>

## Multicore communication

Two threads pinned to a pair of CPUs ping-pong one value through a `std::atomic` on its own cache line; each cell is the one-way hand-off latency (half a round trip, p50). This is the floor for any inter-thread communication between those CPUs: SMT siblings share L1/L2 and are fastest, other cores on the same die or cluster come next, other clusters, CCXs or sockets are slowest.

![Core-to-core latency (one-way, p50)](charts/multicore-c2c.svg)

> Fastest pair: CPU 0 ↔ 1 (82.6 ns); slowest: CPU 0 ↔ 2 (95.9 ns); median 85.1 ns. Pin threads that talk a lot (producer/consumer pairs) to a fast pair with `--cpus A,B` or in your application.

<a id="patterns"></a>

## Design patterns

### Container memory: heap vs reserve vs pmr arenas

Build and destroy a 256-element container; cost per element (p50, shorter is better, compared with the fastest in brackets). Node containers allocate once per element, so the allocator matters far more there.

![std::vector&lt;int&gt;, 256 push_back: ns per element](charts/pattern-alloc-vector.svg)

![std::list&lt;int&gt;, 256 push_back: ns per element](charts/pattern-alloc-list.svg)

### Single producer / single consumer: mutex vs spinlock vs lock-free

The same bounded ring hand-off synchronised five ways. Latency is half a ping-pong round trip; throughput streams 256 Ki items through one queue.

![SPSC hand-off latency by percentile](charts/pattern-spsc-latency.svg)

![SPSC streaming throughput](charts/pattern-spsc-throughput.svg)

What it costs: CPU time burnt by producer plus consumer during the latency test, divided by wall time. 2.0 means both threads kept a core fully busy (polling); low values mean they slept while waiting.

![SPSC CPU usage while handing off](charts/pattern-spsc-cpu.svg)

<a id="table"></a>

## All latency results

Grey values have fewer than ~10 samples beyond the percentile and are not statistically reliable.

| benchmark | n | min | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|---|
| alloc.malloc [size=16] | 10000 | 29 ns | 31 ns | 32 ns | 36 ns | 44 ns | 298 ns | 18.2 us |
| alloc.free [size=16] | 10000 | 30 ns | 33 ns | 34 ns | 47 ns | 220 ns | 294 ns | 3.46 us |
| alloc.malloc [size=64] | 10000 | 28 ns | 31 ns | 32 ns | 36 ns | 56 ns | 109 ns | 3.55 us |
| alloc.free [size=64] | 10000 | 30 ns | 33 ns | 34 ns | 45 ns | 171 ns | 16.9 us | 41.6 us |
| alloc.malloc [size=256] | 10000 | 29 ns | 31 ns | 32 ns | 41 ns | 74 ns | 9 us | 11.7 us |
| alloc.free [size=256] | 10000 | 28 ns | 31 ns | 33 ns | 44 ns | 80 ns | 145 ns | 212 ns |
| alloc.malloc [size=1K] | 10000 | 29 ns | 31 ns | 32 ns | 37 ns | 58 ns | 88 ns | 119 ns |
| alloc.free [size=1K] | 10000 | 28 ns | 31 ns | 32 ns | 40 ns | 69 ns | 11.3 us | 16.9 us |
| alloc.malloc [size=4K] | 10000 | 54 ns | 57 ns | 58 ns | 76 ns | 110 ns | 3.08 us | 10.7 us |
| alloc.free [size=4K] | 10000 | 42 ns | 45 ns | 46 ns | 62 ns | 105 ns | 260 ns | 262 ns |
| alloc.malloc [size=16K] | 10000 | 51 ns | 54 ns | 56 ns | 81 ns | 253 ns | 411 ns | 4.45 us |
| alloc.free [size=16K] | 10000 | 43 ns | 45 ns | 48 ns | 63 ns | 115 ns | 6.66 us | 27.7 us |
| alloc.malloc [size=64K] | 10000 | 52 ns | 55 ns | 56 ns | 68 ns | 106 ns | 11.3 us | 18.1 us |
| alloc.free [size=64K] | 10000 | 42 ns | 45 ns | 46 ns | 62 ns | 70 ns | 235 ns | 289 ns |
| alloc.malloc [size=256K] | 10000 | 40 ns | 43 ns | 44 ns | 103 ns | 249 ns | 359 ns | 15.8 us |
| alloc.free [size=256K] | 10000 | 40 ns | 43 ns | 44 ns | 57 ns | 91 ns | 220 ns | 36.7 us |
| alloc.malloc [size=1M] | 10000 | 41 ns | 43 ns | 44 ns | 179 ns | 268 ns | 423 ns | 26.1 us |
| alloc.free [size=1M] | 10000 | 40 ns | 43 ns | 44 ns | 46 ns | 74 ns | 7.98 us | 14.2 us |
| alloc.malloc [size=4M] | 10000 | 41 ns | 43 ns | 44 ns | 182 ns | 345 ns | 783 ns | 1.18 us |
| alloc.free [size=4M] | 10000 | 40 ns | 43 ns | 44 ns | 55 ns | 70 ns | 4.2 us | 26.9 us |
| alloc.malloc [size=16M] | 10000 | 43 ns | 52 ns | 133 ns | 287 ns | 453 ns | 27 us | 72.6 us |
| alloc.free [size=16M] | 10000 | 42 ns | 50 ns | 66 ns | 74 ns | 193 ns | 684 ns | 16 us |
| multicore.c2c_latency [cpu_a=0, cpu_b=1] | 15 | 78.9 ns | 82.6 ns | 99.5 ns | 104 ns | 105 ns | 105 ns | 105 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=2] | 15 | 84.5 ns | 95.9 ns | 115 ns | 134 ns | 136 ns | 136 ns | 136 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=3] | 15 | 80.8 ns | 83.2 ns | 93.1 ns | 101 ns | 101 ns | 101 ns | 101 ns |
| multicore.c2c_latency [cpu_a=1, cpu_b=2] | 15 | 82 ns | 86.9 ns | 107 ns | 117 ns | 118 ns | 118 ns | 118 ns |
| multicore.c2c_latency [cpu_a=1, cpu_b=3] | 15 | 80.8 ns | 83.2 ns | 88.2 ns | 112 ns | 116 ns | 116 ns | 116 ns |
| multicore.c2c_latency [cpu_a=2, cpu_b=3] | 15 | 85.9 ns | 87.4 ns | 102 ns | 108 ns | 108 ns | 108 ns | 108 ns |
| pattern.alloc_vector [allocator=new_delete] | 10000 | 1.36 ns | 1.39 ns | 1.93 ns | 3.55 ns | 20.4 ns | 293 ns | 1.14 us |
| pattern.alloc_vector [allocator=reserve] | 10000 | 0.996 ns | 1.01 ns | 1.02 ns | 1.71 ns | 12.1 ns | 68.5 ns | 126 ns |
| pattern.alloc_vector [allocator=pmr_monotonic] | 10000 | 1.37 ns | 1.47 ns | 1.72 ns | 3.04 ns | 17 ns | 61.1 ns | 77.4 ns |
| pattern.alloc_vector [allocator=pmr_pool] | 10000 | 1.9 ns | 1.97 ns | 2.26 ns | 3.63 ns | 55.8 ns | 984 ns | 2.15 us |
| pattern.alloc_list [allocator=new_delete] | 10000 | 27.4 ns | 29.1 ns | 37.7 ns | 85.2 ns | 184 ns | 833 ns | 963 ns |
| pattern.alloc_list [allocator=pmr_monotonic] | 10000 | 4.98 ns | 6.88 ns | 8.17 ns | 8.84 ns | 102 ns | 281 ns | 299 ns |
| pattern.alloc_list [allocator=pmr_pool] | 10000 | 21.8 ns | 30.8 ns | 40.2 ns | 92.1 ns | 299 ns | 476 ns | 575 ns |
| pattern.spsc_latency [queue=mutex_condvar] | 10000 | 6.56 us | 13.4 us | 17.8 us | 38.2 us | 143 us | 518 us | 651 us |
| pattern.spsc_throughput [queue=mutex_condvar] | 15 | 202 ns | 229 ns | 249 ns | 253 ns | 253 ns | 253 ns | 253 ns |
| pattern.spsc_latency [queue=mutex_poll] | 10000 | 412 ns | 1.33 us | 8.89 us | 18.3 us | 72.4 us | 171 us | 511 us |
| pattern.spsc_throughput [queue=mutex_poll] | 15 | 164 ns | 185 ns | 193 ns | 241 ns | 248 ns | 248 ns | 248 ns |
| pattern.spsc_latency [queue=spinlock] | 10000 | 376 ns | 824 ns | 982 ns | 1.3 us | 10.1 us | 164 us | 547 us |
| pattern.spsc_throughput [queue=spinlock] | 15 | 274 ns | 409 ns | 548 ns | 580 ns | 583 ns | 583 ns | 583 ns |
| pattern.spsc_latency [queue=lockfree] | 10000 | 231 ns | 344 ns | 520 ns | 563 ns | 6.65 us | 17.9 us | 24 us |
| pattern.spsc_throughput [queue=lockfree] | 15 | 50.4 ns | 61.3 ns | 92.5 ns | 230 ns | 249 ns | 251 ns | 251 ns |
| pattern.spsc_latency [queue=lockfree_cached] | 10000 | 256 ns | 387 ns | 470 ns | 632 ns | 4.93 us | 34 us | 35.9 us |
| pattern.spsc_throughput [queue=lockfree_cached] | 15 | 24.5 ns | 33.7 ns | 44.5 ns | 47.5 ns | 47.8 ns | 47.8 ns | 47.8 ns |
| sync.mutex_wakeup | 10000 | 13.6 us | 39.1 us | 101 us | 215 us | 1.25 ms | 2.11 ms | 2.62 ms |
| sync.condvar_wakeup | 10000 | 14.1 us | 39.7 us | 104 us | 238 us | 1.65 ms | 5.76 ms | 7.55 ms |
| thread.create_to_start | 10000 | 4.06 us | 6.72 us | 12.5 us | 79 us | 527 us | 3.23 ms | 5.06 ms |
| thread.exit_to_join | 10000 | 2.77 us | 4 us | 5.05 us | 14.9 us | 121 us | 481 us | 845 us |

*Generated from results.json by tools/olt-report.py. An interactive version with hover details is in report.html next to this file.*
