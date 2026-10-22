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
- [Instruction-level parallelism](#cpu)
- [Blocking wake-up latency](#sync)
- [Thread lifecycle](#thread)
- [Heap allocator](#alloc)
- [Multicore communication](#multicore)
- [Design patterns](#patterns)
- [All latency results](#table)

<a id="hints"></a>

## Design hints for this machine

- A random access that misses all caches costs about 107x an L1 hit (174 ns vs 1.63 ns). Keep the data a hot loop touches under ~36 KiB per core where possible.
- Working sets up to ~1.5 MiB still get L2 latency (5.36 ns): size per-thread buffers, lookup tables and queues to fit there.
- Beyond the last cache, sequential reads cost 7.08 ns per 64-byte line vs 174 ns per random access: prefer contiguous arrays and streaming over pointer-heavy structures.
- A blocking hand-off (mutex/condvar wake-up) costs 27 us typically and 105 us at p99: avoid blocking on the critical path below that budget.
- Starting a thread costs 5.77 us typically but 108 us at p99 (max 1.68 ms): reuse threads from a pool instead of spawning per task.
- Loop unrolling: a floating-point sum with one accumulator runs at 0.645 ns per add (the add latency); with 6 independent accumulators it reaches 0.161 ns (4.0x). Split reductions into 6 partial sums (the compiler does it for integers, for floating point only with -ffast-math / -fassociative-math).
- Memory-level parallelism: one chain of dependent DRAM misses costs 171 ns per load, but 12 independent chains overlap to 14.8 ns (12x). Batch independent lookups (hash probes, tree searches, pointer chasing) 12 at a time, or prefetch ahead, instead of finishing one before starting the next.
- False sharing: two cores updating counters in the same cache line pay 37.1 ns per atomic increment instead of 5.51 ns (7x). Keep data written by different threads at least 256 bytes apart (alignas(256) or padding). That is more than the 64-byte cache line because the adjacent-line prefetcher pulls lines in pairs: alignas(64) is not enough here, whatever std::hardware_destructive_interference_size says.
- Thread-to-thread hand-off: a blocking mutex+condvar queue takes 12.1 us (p99 24.3 us), a polling lock-free SPSC ring 35.5 ns (p99 44 ns): 340x faster, at the price of a core spinning while it waits. Give each polling thread its own core.
- Streaming between two threads: lockfree_cached moves 162 M items/s, 39.8x more than mutex_poll.
- Node-based containers: a std::pmr::monotonic_buffer_resource arena makes building a list 5.0x cheaper than new/delete (6.61 ns vs 32.9 ns per node). Use arenas for short-lived, per-request or per-frame data that is freed all at once.
- std::vector: reserve() up front saves 24% of the build cost by avoiding regrowth copies.

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
| L1 | up to 36 KiB | 1.63 ns | 72.4 GB/s | 23.7 GB/s |
| L2 | 72 KiB – 1.5 MiB | 5.36 ns | 71 GB/s | 24.2 GB/s |
| L3 | 3 MiB – 195 MiB | 67.1 ns | 29.4 GB/s | 22 GB/s |
| DRAM | > 390 MiB | 174 ns | 9.04 GB/s | 6.59 GB/s |

Benchmark guide: [memory hierarchy](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/memory.md), what is measured and how to apply the result in C++.

<a id="cpu"></a>

## Instruction-level parallelism

A loop runs N independent dependency chains of the same operation (N accumulators, N pointer chases). With one chain every step waits for the previous result, so it runs at the operation's latency; with more chains the out-of-order core overlaps them until an execution port, the load units or the miss buffers are saturated. Where a curve flattens is how far to unroll such a loop on this core. Dashed: perfect overlap.

![Instruction-level parallelism: arithmetic](charts/cpu-ilp-alu.svg)

![Instruction-level parallelism: loads](charts/cpu-ilp-mem.svg)

| operation | one chain (latency) | best per op | speed-up | chains to get 90% of it |
|---|---|---|---|---|
| integer multiply | 0.968 ns | 0.294 ns | 3.3x | 3 |
| double add | 0.645 ns | 0.161 ns | 4.0x | 6 |
| double multiply | 1.33 ns | 0.162 ns | 8.3x | 8 |
| load hitting L1 | 1.67 ns | 0.191 ns | 8.7x | 8 |
| load missing to DRAM | 171 ns | 14.8 ns | 11.5x | 12 |

> Curves can drop again at high chain counts: once accumulators outnumber the registers (16 general-purpose and 16 vector registers on x86-64, 32 of each on AArch64) the compiler spills them to the stack. More chains is not always better.

Benchmark guide: [instruction-level parallelism](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/ilp.md), what is measured and how to apply the result in C++.

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

> Fastest pair: CPU 0 ↔ 2 (53.7 ns); slowest: CPU 1 ↔ 3 (66.6 ns); median 58.3 ns. Pin threads that talk a lot (producer/consumer pairs) to a fast pair with `--cpus A,B` or in your application.

Benchmark guide: [core-to-core latency](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/c2c.md), what is measured and how to apply the result in C++.

### False sharing: how far apart must per-thread data be?

Threads on CPU 3 and CPU 0 (different physical cores) each increment their own counter; the counters sit the given distance apart. While both share a cache line, every write pulls the line away from the other core. Atomic read-modify-writes pay each transfer; plain stores hide most of it in the store buffer, so the penalty there is smaller but not zero. Dashed: the same loop on one core alone. Band: min to p90.

![False sharing: cost per increment vs distance between counters](charts/multicore-false-sharing.svg)

> On this CPU, counters written by different threads need at least 256 bytes between them (vertical line: the 64-byte cache line).

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
| alloc.malloc [size=16] | 10000 | 32 ns | 44 ns | 48 ns | 63 ns | 129 ns | 257 ns | 16.2 us |
| alloc.free [size=16] | 10000 | 32 ns | 47 ns | 51 ns | 66 ns | 130 ns | 305 ns | 5.55 us |
| alloc.malloc [size=64] | 10000 | 32 ns | 44 ns | 48 ns | 55 ns | 120 ns | 175 ns | 374 ns |
| alloc.free [size=64] | 10000 | 32 ns | 47 ns | 51 ns | 57 ns | 123 ns | 4.95 us | 17.7 us |
| alloc.malloc [size=256] | 10000 | 32 ns | 44 ns | 48 ns | 55 ns | 120 ns | 5.96 us | 29.2 us |
| alloc.free [size=256] | 10000 | 32 ns | 47 ns | 51 ns | 59 ns | 124 ns | 176 ns | 191 ns |
| alloc.malloc [size=1K] | 10000 | 32 ns | 44 ns | 48 ns | 63 ns | 119 ns | 155 ns | 4.96 us |
| alloc.free [size=1K] | 10000 | 32 ns | 47 ns | 51 ns | 72 ns | 128 ns | 251 ns | 16.7 us |
| alloc.malloc [size=4K] | 10000 | 61 ns | 91 ns | 98 ns | 148 ns | 346 ns | 8.03 us | 16.7 us |
| alloc.free [size=4K] | 10000 | 50 ns | 72 ns | 78 ns | 89 ns | 157 ns | 197 ns | 218 ns |
| alloc.malloc [size=16K] | 10000 | 60 ns | 84 ns | 90 ns | 110 ns | 226 ns | 280 ns | 4.84 us |
| alloc.free [size=16K] | 10000 | 50 ns | 72 ns | 77 ns | 89 ns | 173 ns | 15.7 us | 17.1 us |
| alloc.malloc [size=64K] | 10000 | 62 ns | 83 ns | 89 ns | 109 ns | 207 ns | 16.1 us | 26.9 us |
| alloc.free [size=64K] | 10000 | 51 ns | 72 ns | 77 ns | 89 ns | 151 ns | 4.47 us | 14.8 us |
| alloc.malloc [size=256K] | 10000 | 47 ns | 67 ns | 73 ns | 147 ns | 267 ns | 4.51 us | 15.5 us |
| alloc.free [size=256K] | 10000 | 47 ns | 66 ns | 71 ns | 99 ns | 165 ns | 4.05 us | 19.2 us |
| alloc.malloc [size=1M] | 10000 | 43 ns | 52 ns | 72 ns | 154 ns | 316 ns | 651 ns | 91.7 us |
| alloc.free [size=1M] | 10000 | 44 ns | 49 ns | 69 ns | 75 ns | 120 ns | 8.76 us | 11 us |
| alloc.malloc [size=4M] | 10000 | 41 ns | 44 ns | 44 ns | 134 ns | 352 ns | 3.87 us | 12.6 us |
| alloc.free [size=4M] | 10000 | 41 ns | 43 ns | 44 ns | 45 ns | 63 ns | 83.6 ns | 26.1 us |
| alloc.malloc [size=16M] | 10000 | 42 ns | 48 ns | 49 ns | 136 ns | 316 ns | 410 ns | 4.61 us |
| alloc.free [size=16M] | 10000 | 41 ns | 47 ns | 48 ns | 49 ns | 60 ns | 472 ns | 13.6 us |
| cpu.ilp [op=int_mul, chains=1] | 15 | 0.968 ns | 0.968 ns | 1.07 ns | 1.27 ns | 1.29 ns | 1.3 ns | 1.3 ns |
| cpu.ilp [op=int_mul, chains=2] | 15 | 0.484 ns | 0.484 ns | 0.496 ns | 0.559 ns | 0.567 ns | 0.568 ns | 0.568 ns |
| cpu.ilp [op=int_mul, chains=3] | 15 | 0.323 ns | 0.323 ns | 0.333 ns | 0.381 ns | 0.387 ns | 0.388 ns | 0.388 ns |
| cpu.ilp [op=int_mul, chains=4] | 15 | 0.323 ns | 0.323 ns | 0.366 ns | 0.401 ns | 0.401 ns | 0.401 ns | 0.401 ns |
| cpu.ilp [op=int_mul, chains=6] | 15 | 0.294 ns | 0.294 ns | 0.327 ns | 0.356 ns | 0.36 ns | 0.36 ns | 0.36 ns |
| cpu.ilp [op=int_mul, chains=8] | 15 | 0.294 ns | 0.295 ns | 0.324 ns | 0.334 ns | 0.335 ns | 0.335 ns | 0.335 ns |
| cpu.ilp [op=int_mul, chains=10] | 15 | 0.294 ns | 0.316 ns | 0.337 ns | 0.352 ns | 0.354 ns | 0.354 ns | 0.354 ns |
| cpu.ilp [op=int_mul, chains=12] | 15 | 0.323 ns | 0.326 ns | 0.336 ns | 0.344 ns | 0.345 ns | 0.345 ns | 0.345 ns |
| cpu.ilp [op=int_mul, chains=16] | 15 | 0.323 ns | 0.332 ns | 0.363 ns | 0.383 ns | 0.385 ns | 0.385 ns | 0.385 ns |
| cpu.ilp [op=fp_add, chains=1] | 15 | 0.594 ns | 0.645 ns | 0.658 ns | 0.699 ns | 0.705 ns | 0.706 ns | 0.706 ns |
| cpu.ilp [op=fp_add, chains=2] | 15 | 0.294 ns | 0.299 ns | 0.319 ns | 0.404 ns | 0.415 ns | 0.416 ns | 0.416 ns |
| cpu.ilp [op=fp_add, chains=3] | 15 | 0.2 ns | 0.201 ns | 0.227 ns | 0.299 ns | 0.31 ns | 0.311 ns | 0.311 ns |
| cpu.ilp [op=fp_add, chains=4] | 15 | 0.173 ns | 0.199 ns | 0.286 ns | 0.295 ns | 0.296 ns | 0.296 ns | 0.296 ns |
| cpu.ilp [op=fp_add, chains=6] | 15 | 0.161 ns | 0.161 ns | 0.175 ns | 0.494 ns | 0.54 ns | 0.545 ns | 0.545 ns |
| cpu.ilp [op=fp_add, chains=8] | 15 | 0.161 ns | 0.161 ns | 0.167 ns | 0.185 ns | 0.188 ns | 0.188 ns | 0.188 ns |
| cpu.ilp [op=fp_add, chains=10] | 15 | 0.161 ns | 0.163 ns | 0.182 ns | 0.251 ns | 0.261 ns | 0.262 ns | 0.262 ns |
| cpu.ilp [op=fp_add, chains=12] | 15 | 0.161 ns | 0.163 ns | 0.188 ns | 0.21 ns | 0.212 ns | 0.213 ns | 0.213 ns |
| cpu.ilp [op=fp_add, chains=16] | 15 | 0.186 ns | 0.189 ns | 0.236 ns | 0.256 ns | 0.258 ns | 0.258 ns | 0.258 ns |
| cpu.ilp [op=fp_mul, chains=1] | 15 | 1.33 ns | 1.33 ns | 1.51 ns | 1.64 ns | 1.65 ns | 1.65 ns | 1.65 ns |
| cpu.ilp [op=fp_mul, chains=2] | 15 | 0.645 ns | 0.667 ns | 0.684 ns | 0.726 ns | 0.73 ns | 0.731 ns | 0.731 ns |
| cpu.ilp [op=fp_mul, chains=3] | 15 | 0.444 ns | 0.446 ns | 0.504 ns | 0.506 ns | 0.506 ns | 0.506 ns | 0.506 ns |
| cpu.ilp [op=fp_mul, chains=4] | 15 | 0.323 ns | 0.333 ns | 0.368 ns | 0.381 ns | 0.382 ns | 0.382 ns | 0.382 ns |
| cpu.ilp [op=fp_mul, chains=6] | 15 | 0.215 ns | 0.216 ns | 0.235 ns | 0.248 ns | 0.249 ns | 0.249 ns | 0.249 ns |
| cpu.ilp [op=fp_mul, chains=8] | 15 | 0.161 ns | 0.167 ns | 0.175 ns | 0.189 ns | 0.19 ns | 0.191 ns | 0.191 ns |
| cpu.ilp [op=fp_mul, chains=10] | 15 | 0.161 ns | 0.162 ns | 0.179 ns | 0.199 ns | 0.201 ns | 0.202 ns | 0.202 ns |
| cpu.ilp [op=fp_mul, chains=12] | 15 | 0.161 ns | 0.173 ns | 0.183 ns | 0.194 ns | 0.196 ns | 0.196 ns | 0.196 ns |
| cpu.ilp [op=fp_mul, chains=16] | 15 | 0.179 ns | 0.179 ns | 0.19 ns | 0.196 ns | 0.197 ns | 0.197 ns | 0.197 ns |
| cpu.ilp [op=load_l1, chains=1] | 15 | 1.61 ns | 1.67 ns | 1.82 ns | 1.94 ns | 1.96 ns | 1.96 ns | 1.96 ns |
| cpu.ilp [op=load_l1, chains=2] | 15 | 0.833 ns | 0.833 ns | 0.896 ns | 0.92 ns | 0.921 ns | 0.921 ns | 0.921 ns |
| cpu.ilp [op=load_l1, chains=3] | 15 | 0.556 ns | 0.556 ns | 0.57 ns | 0.604 ns | 0.609 ns | 0.61 ns | 0.61 ns |
| cpu.ilp [op=load_l1, chains=4] | 15 | 0.404 ns | 0.417 ns | 0.478 ns | 0.5 ns | 0.503 ns | 0.504 ns | 0.504 ns |
| cpu.ilp [op=load_l1, chains=6] | 15 | 0.269 ns | 0.27 ns | 0.32 ns | 0.346 ns | 0.35 ns | 0.35 ns | 0.35 ns |
| cpu.ilp [op=load_l1, chains=8] | 15 | 0.208 ns | 0.21 ns | 0.24 ns | 0.248 ns | 0.249 ns | 0.249 ns | 0.249 ns |
| cpu.ilp [op=load_l1, chains=10] | 15 | 0.192 ns | 0.194 ns | 0.21 ns | 0.217 ns | 0.218 ns | 0.218 ns | 0.218 ns |
| cpu.ilp [op=load_l1, chains=12] | 15 | 0.19 ns | 0.191 ns | 0.21 ns | 0.214 ns | 0.214 ns | 0.214 ns | 0.214 ns |
| cpu.ilp [op=load_l1, chains=16] | 15 | 0.185 ns | 0.195 ns | 0.231 ns | 0.264 ns | 0.269 ns | 0.27 ns | 0.27 ns |
| cpu.ilp [op=load_mem, chains=1] | 15 | 167 ns | 171 ns | 183 ns | 188 ns | 189 ns | 189 ns | 189 ns |
| cpu.ilp [op=load_mem, chains=2] | 15 | 84.1 ns | 87.4 ns | 90.6 ns | 91.2 ns | 91.2 ns | 91.2 ns | 91.2 ns |
| cpu.ilp [op=load_mem, chains=3] | 15 | 56.3 ns | 57.5 ns | 62.2 ns | 72.7 ns | 74.1 ns | 74.3 ns | 74.3 ns |
| cpu.ilp [op=load_mem, chains=4] | 15 | 41.6 ns | 44.1 ns | 47 ns | 49.1 ns | 49.3 ns | 49.4 ns | 49.4 ns |
| cpu.ilp [op=load_mem, chains=6] | 15 | 27.5 ns | 29.3 ns | 30.3 ns | 30.9 ns | 31 ns | 31 ns | 31 ns |
| cpu.ilp [op=load_mem, chains=8] | 15 | 21.4 ns | 22.1 ns | 23 ns | 27.1 ns | 27.7 ns | 27.8 ns | 27.8 ns |
| cpu.ilp [op=load_mem, chains=10] | 15 | 18 ns | 18.3 ns | 19 ns | 20.5 ns | 20.8 ns | 20.8 ns | 20.8 ns |
| cpu.ilp [op=load_mem, chains=12] | 15 | 15.4 ns | 16.2 ns | 17.4 ns | 18.7 ns | 18.8 ns | 18.8 ns | 18.8 ns |
| cpu.ilp [op=load_mem, chains=16] | 15 | 14.3 ns | 14.8 ns | 15.9 ns | 16.2 ns | 16.2 ns | 16.2 ns | 16.2 ns |
| multicore.false_sharing_alone [op=plain, cpu_a=3, cpu_b=0] | 15 | 0.348 ns | 0.348 ns | 0.417 ns | 0.596 ns | 0.618 ns | 0.62 ns | 0.621 ns |
| multicore.false_sharing [op=plain, distance=8, cpu_a=3, cpu_b=0] | 15 | 0.399 ns | 0.4 ns | 0.574 ns | 0.595 ns | 0.597 ns | 0.598 ns | 0.598 ns |
| multicore.false_sharing [op=plain, distance=16, cpu_a=3, cpu_b=0] | 15 | 0.398 ns | 0.404 ns | 0.891 ns | 1.58 ns | 1.66 ns | 1.67 ns | 1.67 ns |
| multicore.false_sharing [op=plain, distance=32, cpu_a=3, cpu_b=0] | 15 | 0.398 ns | 0.404 ns | 0.691 ns | 1.85 ns | 2.02 ns | 2.04 ns | 2.04 ns |
| multicore.false_sharing [op=plain, distance=64, cpu_a=3, cpu_b=0] | 15 | 0.337 ns | 0.367 ns | 0.432 ns | 0.635 ns | 0.66 ns | 0.663 ns | 0.663 ns |
| multicore.false_sharing [op=plain, distance=128, cpu_a=3, cpu_b=0] | 15 | 0.326 ns | 0.356 ns | 0.511 ns | 1.5 ns | 1.65 ns | 1.66 ns | 1.67 ns |
| multicore.false_sharing [op=plain, distance=256, cpu_a=3, cpu_b=0] | 15 | 0.309 ns | 0.335 ns | 1.42 ns | 1.92 ns | 1.97 ns | 1.98 ns | 1.98 ns |
| multicore.false_sharing [op=plain, distance=512, cpu_a=3, cpu_b=0] | 15 | 0.297 ns | 0.324 ns | 0.912 ns | 1.02 ns | 1.03 ns | 1.03 ns | 1.03 ns |
| multicore.false_sharing_alone [op=atomic, cpu_a=3, cpu_b=0] | 15 | 5.32 ns | 5.51 ns | 6 ns | 6.42 ns | 6.48 ns | 6.49 ns | 6.49 ns |
| multicore.false_sharing [op=atomic, distance=8, cpu_a=3, cpu_b=0] | 15 | 19.2 ns | 27.5 ns | 31.4 ns | 31.6 ns | 31.6 ns | 31.6 ns | 31.6 ns |
| multicore.false_sharing [op=atomic, distance=16, cpu_a=3, cpu_b=0] | 15 | 14.1 ns | 34 ns | 38.8 ns | 39.1 ns | 39.2 ns | 39.2 ns | 39.2 ns |
| multicore.false_sharing [op=atomic, distance=32, cpu_a=3, cpu_b=0] | 15 | 33.3 ns | 37.1 ns | 40.2 ns | 41.1 ns | 41.1 ns | 41.2 ns | 41.2 ns |
| multicore.false_sharing [op=atomic, distance=64, cpu_a=3, cpu_b=0] | 15 | 6.87 ns | 7.39 ns | 8.12 ns | 8.25 ns | 8.26 ns | 8.26 ns | 8.26 ns |
| multicore.false_sharing [op=atomic, distance=128, cpu_a=3, cpu_b=0] | 15 | 5.81 ns | 7.97 ns | 19.9 ns | 36.2 ns | 38.3 ns | 38.5 ns | 38.6 ns |
| multicore.false_sharing [op=atomic, distance=256, cpu_a=3, cpu_b=0] | 15 | 5.32 ns | 5.53 ns | 5.73 ns | 6.03 ns | 6.07 ns | 6.08 ns | 6.08 ns |
| multicore.false_sharing [op=atomic, distance=512, cpu_a=3, cpu_b=0] | 15 | 5.4 ns | 6.2 ns | 6.96 ns | 7.62 ns | 7.69 ns | 7.7 ns | 7.7 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=1] | 15 | 57.4 ns | 59.7 ns | 69.8 ns | 81.4 ns | 82.9 ns | 83.1 ns | 83.1 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=2] | 15 | 51.8 ns | 53.7 ns | 64.3 ns | 69.1 ns | 69.3 ns | 69.3 ns | 69.3 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=3] | 15 | 52.8 ns | 54.5 ns | 63.5 ns | 68.3 ns | 68.6 ns | 68.7 ns | 68.7 ns |
| multicore.c2c_latency [cpu_a=1, cpu_b=2] | 15 | 59.5 ns | 61.9 ns | 76.2 ns | 80.3 ns | 80.8 ns | 80.9 ns | 80.9 ns |
| multicore.c2c_latency [cpu_a=1, cpu_b=3] | 15 | 62.9 ns | 66.6 ns | 72.6 ns | 78 ns | 78.6 ns | 78.7 ns | 78.7 ns |
| multicore.c2c_latency [cpu_a=2, cpu_b=3] | 15 | 52.7 ns | 56.9 ns | 68.9 ns | 72.1 ns | 72.4 ns | 72.4 ns | 72.4 ns |
| pattern.alloc_vector [allocator=new_delete] | 10000 | 1.43 ns | 1.45 ns | 1.5 ns | 3.15 ns | 14.4 ns | 94.6 ns | 180 ns |
| pattern.alloc_vector [allocator=reserve] | 10000 | 1.06 ns | 1.1 ns | 1.13 ns | 2.69 ns | 2.99 ns | 57 ns | 89.8 ns |
| pattern.alloc_vector [allocator=pmr_monotonic] | 10000 | 1.34 ns | 1.4 ns | 2.1 ns | 3.18 ns | 113 ns | 472 ns | 2.63 us |
| pattern.alloc_vector [allocator=pmr_pool] | 10000 | 2.43 ns | 3.21 ns | 3.62 ns | 4.45 ns | 111 ns | 169 ns | 894 ns |
| pattern.alloc_list [allocator=new_delete] | 10000 | 27.9 ns | 32.9 ns | 40.8 ns | 78.1 ns | 380 ns | 2.95 us | 5.89 us |
| pattern.alloc_list [allocator=pmr_monotonic] | 10000 | 6.01 ns | 6.61 ns | 8.84 ns | 9.9 ns | 72.1 ns | 4.37 us | 8.09 us |
| pattern.alloc_list [allocator=pmr_pool] | 10000 | 18.4 ns | 21.1 ns | 22.7 ns | 41 ns | 120 ns | 300 ns | 768 ns |
| pattern.spsc_latency [queue=mutex_condvar] | 10000 | 387 ns | 12.1 us | 13.3 us | 24.3 us | 36.2 us | 49.9 us | 56.3 us |
| pattern.spsc_throughput [queue=mutex_condvar] | 15 | 160 ns | 236 ns | 266 ns | 313 ns | 320 ns | 321 ns | 321 ns |
| pattern.spsc_latency [queue=mutex_poll] | 10000 | 172 ns | 1.04 us | 1.3 us | 2.21 us | 17.4 us | 27 us | 27.4 us |
| pattern.spsc_throughput [queue=mutex_poll] | 15 | 210 ns | 245 ns | 293 ns | 326 ns | 330 ns | 330 ns | 330 ns |
| pattern.spsc_latency [queue=spinlock] | 10000 | 260 ns | 478 ns | 642 ns | 848 ns | 9.21 us | 13.4 us | 22.5 us |
| pattern.spsc_throughput [queue=spinlock] | 15 | 28.5 ns | 121 ns | 149 ns | 151 ns | 151 ns | 151 ns | 151 ns |
| pattern.spsc_latency [queue=lockfree] | 10000 | 26.5 ns | 36.5 ns | 39.5 ns | 45 ns | 130 ns | 1.68 us | 8.17 us |
| pattern.spsc_throughput [queue=lockfree] | 15 | 8.69 ns | 8.96 ns | 9.47 ns | 9.55 ns | 9.55 ns | 9.56 ns | 9.56 ns |
| pattern.spsc_latency [queue=lockfree_cached] | 10000 | 25 ns | 35.5 ns | 40 ns | 44 ns | 78.5 ns | 1.75 us | 5.87 us |
| pattern.spsc_throughput [queue=lockfree_cached] | 15 | 5.74 ns | 6.17 ns | 6.38 ns | 6.4 ns | 6.4 ns | 6.4 ns | 6.4 ns |
| sync.mutex_wakeup | 10000 | 9.76 us | 27 us | 45.2 us | 105 us | 373 us | 2.26 ms | 2.77 ms |
| sync.condvar_wakeup | 10000 | 4.47 us | 30.6 us | 44.1 us | 98.5 us | 282 us | 1.64 ms | 1.72 ms |
| thread.create_to_start | 10000 | 3.96 us | 5.77 us | 22.2 us | 108 us | 246 us | 1.03 ms | 1.68 ms |
| thread.exit_to_join | 10000 | 3.1 us | 3.6 us | 4.82 us | 7.14 us | 32.5 us | 173 us | 959 us |

*Generated from results.json by tools/olt-report.py. An interactive version with hover details is in report.html next to this file.*
