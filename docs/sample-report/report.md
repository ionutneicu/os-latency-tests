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

- A random access that misses all caches costs about 114x an L1 hit (174 ns vs 1.53 ns). Keep the data a hot loop touches under ~36 KiB per core where possible.
- Working sets up to ~1.5 MiB still get L2 latency (5.26 ns): size per-thread buffers, lookup tables and queues to fit there.
- Beyond the last cache, sequential reads cost 4.17 ns per 64-byte line vs 174 ns per random access: prefer contiguous arrays and streaming over pointer-heavy structures.
- A store that misses to DRAM costs 17.4 ns of throughput vs 174 ns for a dependent load: the store buffer lets the core move on and overlaps the misses. Scattered writes (histograms, scatter, logging) hurt far less than scattered reads; what stalls is reading the data back or filling the store buffer.
- Writing a buffer larger than the last cache: streaming (non-temporal) stores reach 17.7 GB/s vs 7.99 GB/s for normal stores, which first read each line in. Use them for large output written once and not read again soon (_mm_stream_si128 / stnp, or a memcpy/memset that switches to them).
- A blocking hand-off (mutex/condvar wake-up) costs 28.5 us typically and 116 us at p99: avoid blocking on the critical path below that budget.
- Starting a thread costs 6.62 us typically but 90.5 us at p99 (max 2.36 ms): reuse threads from a pool instead of spawning per task.
- Where threads run matters: handing a cache line to another CPU costs 18.2 ns on the fastest pair but 58.9 ns on the slowest (3x). Keep communicating threads on nearby CPUs (see the core-to-core matrix).
- Loop unrolling: a floating-point sum with one accumulator runs at 0.588 ns per add (the add latency); with 4 independent accumulators it reaches 0.147 ns (4.0x). Split reductions into 4 partial sums (the compiler does it for integers, for floating point only with -ffast-math / -fassociative-math).
- Memory-level parallelism: one chain of dependent DRAM misses costs 183 ns per load, but 12 independent chains overlap to 14.2 ns (13x). Batch independent lookups (hash probes, tree searches, pointer chasing) 12 at a time, or prefetch ahead, instead of finishing one before starting the next.
- False sharing: two cores updating counters in the same cache line pay 29.3 ns per atomic increment instead of 5.52 ns (5x). Keep data written by different threads at least 64 bytes apart (alignas(64) or padding).
- Thread-to-thread hand-off: a blocking mutex+condvar queue takes 11 us (p99 31.8 us), a polling lock-free SPSC ring 243 ns (p99 337 ns): 45x faster, at the price of a core spinning while it waits. Give each polling thread its own core.
- Streaming between two threads: lockfree moves 64.1 M items/s, 16.7x more than mutex_poll.
- Node-based containers: a std::pmr::monotonic_buffer_resource arena makes building a list 3.9x cheaper than new/delete (7.82 ns vs 30.8 ns per node). Use arenas for short-lived, per-request or per-frame data that is freed all at once.
- std::vector: reserve() up front saves 27% of the build cost by avoiding regrowth copies.

<a id="memory"></a>

## Memory hierarchy

Dependent loads in random order through a buffer of the given size, so hardware prefetchers cannot help. Each step up is the working set spilling out of a cache level; dashed lines are the cache sizes reported by the OS. Band: min to p90 across samples. Second line: independent stores to random lines of the same buffer. A store miss does not stall the core (the store buffer holds it while the line is fetched), so misses overlap and a store costs far less than a dependent load at every level.

![Random access cost vs working set](charts/mem-latency.svg)

> Base pages were used, so latency past the TLB reach also includes page walks. Re-run with `--hugepages` to separate TLB misses from cache misses.

Sequential read and write over the same buffer with AVX-512 vector loads and stores: prefetchers are effective, so the drops are smaller than for random access and show each level's bandwidth (log scale: equal steps are equal ratios). A normal store must first read its line in, so each written line crosses the bus twice: read for ownership, then write-back. Dashed curve: streaming (non-temporal) stores, which skip that read and the caches: slow while the data would fit in cache, faster beyond it. Narrower vectors (scalar or SSE code) cannot reach the L1 and L2 figures: those levels deliver as many bytes per cycle as the core can request.

![Sequential bandwidth vs working set](charts/mem-bandwidth.svg)

### Per-level summary

| data fits in | sizes used | random load | random store | seq read | seq write | streaming write |
|---|---|---|---|---|---|---|
| L1 | up to 36 KiB | 1.53 ns | 0.693 ns | 274 GB/s | 204 GB/s | 13.2 GB/s |
| L2 | 72 KiB – 1.5 MiB | 5.26 ns | 1.39 ns | 156 GB/s | 52 GB/s | 18.1 GB/s |
| L3 | 3 MiB – 195 MiB | 78.2 ns | 5.42 ns | 29 GB/s | 26.5 GB/s | 18.1 GB/s |
| DRAM | > 390 MiB | 174 ns | 17.4 ns | 15.4 GB/s | 7.99 GB/s | 17.7 GB/s |

Benchmark guide: [memory hierarchy](https://github.com/ionutneicu/os-latency-tests/blob/main/docs/benchmarks/memory.md), what is measured and how to apply the result in C++.

<a id="cpu"></a>

## Instruction-level parallelism

A loop runs N independent dependency chains of the same operation (N accumulators, N pointer chases). With one chain every step waits for the previous result, so it runs at the operation's latency; with more chains the out-of-order core overlaps them until an execution port, the load units or the miss buffers are saturated. Where a curve flattens is how far to unroll such a loop on this core. Dashed: perfect overlap.

![Instruction-level parallelism: arithmetic](charts/cpu-ilp-alu.svg)

![Instruction-level parallelism: loads](charts/cpu-ilp-mem.svg)

| operation | one chain (latency) | best per op | speed-up | chains to get 90% of it |
|---|---|---|---|---|
| integer multiply | 0.883 ns | 0.294 ns | 3.0x | 3 |
| double add | 0.588 ns | 0.147 ns | 4.0x | 4 |
| double multiply | 1.33 ns | 0.161 ns | 8.3x | 8 |
| load hitting L1 | 1.47 ns | 0.156 ns | 9.4x | 10 |
| load missing to DRAM | 183 ns | 14.2 ns | 12.9x | 12 |

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

> Fastest pair: CPU 2 ↔ 3 (18.2 ns); slowest: CPU 0 ↔ 1 (58.9 ns); median 50.3 ns. Pin threads that talk a lot (producer/consumer pairs) to a fast pair with `--cpus A,B` or in your application.

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
| alloc.malloc [size=16] | 10000 | 28 ns | 30 ns | 33 ns | 40 ns | 50 ns | 395 ns | 480 ns |
| alloc.free [size=16] | 10000 | 28 ns | 30 ns | 36 ns | 45 ns | 166 ns | 244 us | 276 us |
| alloc.malloc [size=64] | 10000 | 28 ns | 30 ns | 31 ns | 32 ns | 42 ns | 189 ns | 5.27 us |
| alloc.free [size=64] | 10000 | 28 ns | 30 ns | 31 ns | 32 ns | 47 ns | 171 ns | 210 ns |
| alloc.malloc [size=256] | 10000 | 28 ns | 30 ns | 31 ns | 40 ns | 47 ns | 108 ns | 198 ns |
| alloc.free [size=256] | 10000 | 28 ns | 30 ns | 32 ns | 45 ns | 60 ns | 192 ns | 3.66 us |
| alloc.malloc [size=1K] | 10000 | 28 ns | 30 ns | 31 ns | 31 ns | 45 ns | 131 ns | 196 ns |
| alloc.free [size=1K] | 10000 | 28 ns | 30 ns | 31 ns | 32 ns | 55 ns | 3.68 us | 4.52 us |
| alloc.malloc [size=4K] | 10000 | 52 ns | 55 ns | 57 ns | 76 ns | 250 ns | 21.1 us | 52.3 us |
| alloc.free [size=4K] | 10000 | 43 ns | 46 ns | 46 ns | 55 ns | 67 ns | 3.39 us | 21.7 us |
| alloc.malloc [size=16K] | 10000 | 53 ns | 55 ns | 56 ns | 70 ns | 72 ns | 169 ns | 178 ns |
| alloc.free [size=16K] | 10000 | 43 ns | 46 ns | 46 ns | 54 ns | 62 ns | 84 ns | 9.88 us |
| alloc.malloc [size=64K] | 10000 | 52 ns | 57 ns | 60 ns | 81 ns | 164 ns | 256 ns | 42.2 us |
| alloc.free [size=64K] | 10000 | 43 ns | 46 ns | 53 ns | 69 ns | 117 ns | 332 ns | 1.09 us |
| alloc.malloc [size=256K] | 10000 | 41 ns | 44 ns | 44 ns | 50 ns | 164 ns | 286 ns | 292 ns |
| alloc.free [size=256K] | 10000 | 41 ns | 43 ns | 44 ns | 45 ns | 60 ns | 12.4 us | 14.6 us |
| alloc.malloc [size=1M] | 10000 | 41 ns | 44 ns | 53 ns | 138 ns | 315 ns | 414 ns | 36.2 us |
| alloc.free [size=1M] | 10000 | 40 ns | 43 ns | 48 ns | 64 ns | 94 ns | 302 ns | 32.5 us |
| alloc.malloc [size=4M] | 10000 | 41 ns | 44 ns | 44 ns | 150 ns | 307 ns | 380 ns | 406 ns |
| alloc.free [size=4M] | 10000 | 41 ns | 43 ns | 44 ns | 45 ns | 61 ns | 76.5 ns | 14.7 us |
| alloc.malloc [size=16M] | 10000 | 41 ns | 44 ns | 44 ns | 149 ns | 322 ns | 405 ns | 23.2 us |
| alloc.free [size=16M] | 10000 | 41 ns | 43 ns | 44 ns | 45 ns | 55 ns | 73.4 ns | 4.07 us |
| cpu.ilp [op=int_mul, chains=1] | 15 | 0.882 ns | 0.883 ns | 0.906 ns | 1.02 ns | 1.03 ns | 1.04 ns | 1.04 ns |
| cpu.ilp [op=int_mul, chains=2] | 15 | 0.441 ns | 0.441 ns | 0.46 ns | 0.515 ns | 0.521 ns | 0.522 ns | 0.522 ns |
| cpu.ilp [op=int_mul, chains=3] | 15 | 0.294 ns | 0.294 ns | 0.294 ns | 0.31 ns | 0.313 ns | 0.313 ns | 0.313 ns |
| cpu.ilp [op=int_mul, chains=4] | 15 | 0.294 ns | 0.294 ns | 0.312 ns | 0.374 ns | 0.383 ns | 0.384 ns | 0.384 ns |
| cpu.ilp [op=int_mul, chains=6] | 15 | 0.294 ns | 0.294 ns | 0.311 ns | 0.317 ns | 0.317 ns | 0.317 ns | 0.317 ns |
| cpu.ilp [op=int_mul, chains=8] | 15 | 0.294 ns | 0.294 ns | 0.307 ns | 0.338 ns | 0.342 ns | 0.343 ns | 0.343 ns |
| cpu.ilp [op=int_mul, chains=10] | 15 | 0.294 ns | 0.294 ns | 0.308 ns | 0.308 ns | 0.308 ns | 0.308 ns | 0.308 ns |
| cpu.ilp [op=int_mul, chains=12] | 15 | 0.294 ns | 0.298 ns | 0.32 ns | 0.321 ns | 0.321 ns | 0.321 ns | 0.321 ns |
| cpu.ilp [op=int_mul, chains=16] | 15 | 0.294 ns | 0.297 ns | 0.316 ns | 0.321 ns | 0.321 ns | 0.321 ns | 0.321 ns |
| cpu.ilp [op=fp_add, chains=1] | 15 | 0.588 ns | 0.588 ns | 0.613 ns | 0.71 ns | 0.722 ns | 0.723 ns | 0.723 ns |
| cpu.ilp [op=fp_add, chains=2] | 15 | 0.294 ns | 0.294 ns | 0.294 ns | 0.294 ns | 0.294 ns | 0.294 ns | 0.294 ns |
| cpu.ilp [op=fp_add, chains=3] | 15 | 0.2 ns | 0.2 ns | 0.209 ns | 0.244 ns | 0.249 ns | 0.249 ns | 0.249 ns |
| cpu.ilp [op=fp_add, chains=4] | 15 | 0.15 ns | 0.15 ns | 0.15 ns | 0.181 ns | 0.185 ns | 0.186 ns | 0.186 ns |
| cpu.ilp [op=fp_add, chains=6] | 15 | 0.147 ns | 0.147 ns | 0.147 ns | 0.198 ns | 0.205 ns | 0.206 ns | 0.206 ns |
| cpu.ilp [op=fp_add, chains=8] | 15 | 0.147 ns | 0.147 ns | 0.151 ns | 0.163 ns | 0.164 ns | 0.164 ns | 0.164 ns |
| cpu.ilp [op=fp_add, chains=10] | 15 | 0.147 ns | 0.149 ns | 0.152 ns | 0.16 ns | 0.161 ns | 0.161 ns | 0.161 ns |
| cpu.ilp [op=fp_add, chains=12] | 15 | 0.148 ns | 0.163 ns | 0.166 ns | 0.199 ns | 0.204 ns | 0.204 ns | 0.204 ns |
| cpu.ilp [op=fp_add, chains=16] | 15 | 0.164 ns | 0.168 ns | 0.186 ns | 0.207 ns | 0.21 ns | 0.211 ns | 0.211 ns |
| cpu.ilp [op=fp_mul, chains=1] | 15 | 1.33 ns | 1.33 ns | 1.39 ns | 1.47 ns | 1.48 ns | 1.48 ns | 1.48 ns |
| cpu.ilp [op=fp_mul, chains=2] | 15 | 0.667 ns | 0.667 ns | 0.688 ns | 0.778 ns | 0.79 ns | 0.791 ns | 0.791 ns |
| cpu.ilp [op=fp_mul, chains=3] | 15 | 0.444 ns | 0.445 ns | 0.502 ns | 0.676 ns | 0.698 ns | 0.7 ns | 0.7 ns |
| cpu.ilp [op=fp_mul, chains=4] | 15 | 0.323 ns | 0.333 ns | 0.341 ns | 0.372 ns | 0.376 ns | 0.377 ns | 0.377 ns |
| cpu.ilp [op=fp_mul, chains=6] | 15 | 0.215 ns | 0.216 ns | 0.233 ns | 0.261 ns | 0.264 ns | 0.264 ns | 0.264 ns |
| cpu.ilp [op=fp_mul, chains=8] | 15 | 0.161 ns | 0.161 ns | 0.17 ns | 0.23 ns | 0.239 ns | 0.24 ns | 0.24 ns |
| cpu.ilp [op=fp_mul, chains=10] | 15 | 0.161 ns | 0.163 ns | 0.181 ns | 0.256 ns | 0.266 ns | 0.267 ns | 0.267 ns |
| cpu.ilp [op=fp_mul, chains=12] | 15 | 0.153 ns | 0.163 ns | 0.171 ns | 0.201 ns | 0.205 ns | 0.206 ns | 0.206 ns |
| cpu.ilp [op=fp_mul, chains=16] | 15 | 0.163 ns | 0.163 ns | 0.172 ns | 0.175 ns | 0.176 ns | 0.176 ns | 0.176 ns |
| cpu.ilp [op=load_l1, chains=1] | 15 | 1.47 ns | 1.47 ns | 1.51 ns | 1.79 ns | 1.83 ns | 1.83 ns | 1.83 ns |
| cpu.ilp [op=load_l1, chains=2] | 15 | 0.735 ns | 0.735 ns | 0.786 ns | 0.824 ns | 0.827 ns | 0.827 ns | 0.827 ns |
| cpu.ilp [op=load_l1, chains=3] | 15 | 0.49 ns | 0.49 ns | 0.511 ns | 0.532 ns | 0.535 ns | 0.535 ns | 0.535 ns |
| cpu.ilp [op=load_l1, chains=4] | 15 | 0.368 ns | 0.368 ns | 0.384 ns | 0.404 ns | 0.406 ns | 0.407 ns | 0.407 ns |
| cpu.ilp [op=load_l1, chains=6] | 15 | 0.245 ns | 0.245 ns | 0.266 ns | 0.268 ns | 0.268 ns | 0.268 ns | 0.268 ns |
| cpu.ilp [op=load_l1, chains=8] | 15 | 0.184 ns | 0.184 ns | 0.191 ns | 0.221 ns | 0.226 ns | 0.226 ns | 0.226 ns |
| cpu.ilp [op=load_l1, chains=10] | 15 | 0.169 ns | 0.171 ns | 0.19 ns | 0.196 ns | 0.196 ns | 0.196 ns | 0.196 ns |
| cpu.ilp [op=load_l1, chains=12] | 15 | 0.162 ns | 0.162 ns | 0.184 ns | 0.191 ns | 0.191 ns | 0.191 ns | 0.191 ns |
| cpu.ilp [op=load_l1, chains=16] | 15 | 0.156 ns | 0.156 ns | 0.164 ns | 0.164 ns | 0.164 ns | 0.164 ns | 0.164 ns |
| cpu.ilp [op=load_mem, chains=1] | 15 | 180 ns | 183 ns | 189 ns | 193 ns | 194 ns | 194 ns | 194 ns |
| cpu.ilp [op=load_mem, chains=2] | 15 | 85.3 ns | 90.3 ns | 95.8 ns | 97.4 ns | 97.4 ns | 97.4 ns | 97.4 ns |
| cpu.ilp [op=load_mem, chains=3] | 15 | 56.2 ns | 60.3 ns | 90.6 ns | 146 ns | 152 ns | 152 ns | 152 ns |
| cpu.ilp [op=load_mem, chains=4] | 15 | 40.7 ns | 43.2 ns | 44.6 ns | 45.9 ns | 46.1 ns | 46.1 ns | 46.1 ns |
| cpu.ilp [op=load_mem, chains=6] | 15 | 27.1 ns | 28.7 ns | 30.1 ns | 30.2 ns | 30.2 ns | 30.2 ns | 30.2 ns |
| cpu.ilp [op=load_mem, chains=8] | 15 | 20.8 ns | 21.6 ns | 22.7 ns | 23.7 ns | 23.8 ns | 23.8 ns | 23.8 ns |
| cpu.ilp [op=load_mem, chains=10] | 15 | 17.5 ns | 18.1 ns | 18.9 ns | 19.3 ns | 19.4 ns | 19.4 ns | 19.4 ns |
| cpu.ilp [op=load_mem, chains=12] | 15 | 15.1 ns | 15.6 ns | 16.2 ns | 16.3 ns | 16.3 ns | 16.3 ns | 16.3 ns |
| cpu.ilp [op=load_mem, chains=16] | 15 | 13.7 ns | 14.2 ns | 15.1 ns | 15.2 ns | 15.2 ns | 15.2 ns | 15.2 ns |
| multicore.false_sharing_alone [op=plain, cpu_a=3, cpu_b=0] | 15 | 0.295 ns | 0.298 ns | 0.331 ns | 0.476 ns | 0.496 ns | 0.498 ns | 0.498 ns |
| multicore.false_sharing [op=plain, distance=8, cpu_a=3, cpu_b=0] | 15 | 0.333 ns | 0.335 ns | 0.72 ns | 1.04 ns | 1.05 ns | 1.06 ns | 1.06 ns |
| multicore.false_sharing [op=plain, distance=16, cpu_a=3, cpu_b=0] | 15 | 0.343 ns | 0.352 ns | 0.381 ns | 0.547 ns | 0.571 ns | 0.574 ns | 0.574 ns |
| multicore.false_sharing [op=plain, distance=32, cpu_a=3, cpu_b=0] | 15 | 0.342 ns | 0.346 ns | 0.415 ns | 0.46 ns | 0.466 ns | 0.466 ns | 0.466 ns |
| multicore.false_sharing [op=plain, distance=64, cpu_a=3, cpu_b=0] | 15 | 0.325 ns | 0.327 ns | 0.356 ns | 0.366 ns | 0.367 ns | 0.367 ns | 0.367 ns |
| multicore.false_sharing [op=plain, distance=128, cpu_a=3, cpu_b=0] | 15 | 0.352 ns | 0.355 ns | 1.14 ns | 1.19 ns | 1.2 ns | 1.2 ns | 1.2 ns |
| multicore.false_sharing [op=plain, distance=256, cpu_a=3, cpu_b=0] | 15 | 0.341 ns | 0.355 ns | 0.519 ns | 0.608 ns | 0.612 ns | 0.612 ns | 0.612 ns |
| multicore.false_sharing [op=plain, distance=512, cpu_a=3, cpu_b=0] | 15 | 0.317 ns | 0.346 ns | 0.355 ns | 0.385 ns | 0.389 ns | 0.39 ns | 0.39 ns |
| multicore.false_sharing_alone [op=atomic, cpu_a=3, cpu_b=0] | 15 | 5.32 ns | 5.52 ns | 5.91 ns | 6.14 ns | 6.17 ns | 6.18 ns | 6.18 ns |
| multicore.false_sharing [op=atomic, distance=8, cpu_a=3, cpu_b=0] | 15 | 27.8 ns | 29.3 ns | 30.8 ns | 32.3 ns | 32.5 ns | 32.5 ns | 32.5 ns |
| multicore.false_sharing [op=atomic, distance=16, cpu_a=3, cpu_b=0] | 15 | 27.4 ns | 28.9 ns | 31.1 ns | 32.2 ns | 32.4 ns | 32.4 ns | 32.4 ns |
| multicore.false_sharing [op=atomic, distance=32, cpu_a=3, cpu_b=0] | 15 | 14 ns | 27.8 ns | 28.4 ns | 28.9 ns | 29 ns | 29 ns | 29 ns |
| multicore.false_sharing [op=atomic, distance=64, cpu_a=3, cpu_b=0] | 15 | 5.5 ns | 6.02 ns | 7.1 ns | 9.75 ns | 10.1 ns | 10.2 ns | 10.2 ns |
| multicore.false_sharing [op=atomic, distance=128, cpu_a=3, cpu_b=0] | 15 | 5.33 ns | 5.84 ns | 6.38 ns | 9.23 ns | 9.63 ns | 9.67 ns | 9.67 ns |
| multicore.false_sharing [op=atomic, distance=256, cpu_a=3, cpu_b=0] | 15 | 5.32 ns | 5.59 ns | 5.85 ns | 5.91 ns | 5.91 ns | 5.92 ns | 5.92 ns |
| multicore.false_sharing [op=atomic, distance=512, cpu_a=3, cpu_b=0] | 15 | 5.32 ns | 5.44 ns | 5.65 ns | 5.77 ns | 5.78 ns | 5.78 ns | 5.78 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=1] | 15 | 57.8 ns | 58.9 ns | 66.4 ns | 71.1 ns | 71.6 ns | 71.7 ns | 71.7 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=2] | 15 | 50.4 ns | 51.7 ns | 56.9 ns | 73.1 ns | 75.3 ns | 75.5 ns | 75.5 ns |
| multicore.c2c_latency [cpu_a=0, cpu_b=3] | 15 | 50.7 ns | 51.4 ns | 65.4 ns | 77.9 ns | 79.6 ns | 79.7 ns | 79.8 ns |
| multicore.c2c_latency [cpu_a=1, cpu_b=2] | 15 | 46.9 ns | 48.6 ns | 55.6 ns | 71 ns | 73.1 ns | 73.3 ns | 73.3 ns |
| multicore.c2c_latency [cpu_a=1, cpu_b=3] | 15 | 48.5 ns | 49.1 ns | 56.4 ns | 213 ns | 235 ns | 237 ns | 238 ns |
| multicore.c2c_latency [cpu_a=2, cpu_b=3] | 15 | 15.3 ns | 18.2 ns | 19.2 ns | 19.3 ns | 19.4 ns | 19.4 ns | 19.4 ns |
| pattern.alloc_vector [allocator=new_delete] | 10000 | 1.46 ns | 1.54 ns | 1.78 ns | 3.21 ns | 4.29 ns | 65.3 ns | 102 ns |
| pattern.alloc_vector [allocator=reserve] | 10000 | 1.1 ns | 1.12 ns | 1.13 ns | 1.55 ns | 2.65 ns | 61.5 ns | 63 ns |
| pattern.alloc_vector [allocator=pmr_monotonic] | 10000 | 1.4 ns | 1.43 ns | 2.86 ns | 3.33 ns | 3.77 ns | 46.2 ns | 61.6 ns |
| pattern.alloc_vector [allocator=pmr_pool] | 10000 | 1.68 ns | 1.81 ns | 1.96 ns | 3.15 ns | 13.6 ns | 55.3 ns | 57.1 ns |
| pattern.alloc_list [allocator=new_delete] | 10000 | 27.8 ns | 30.8 ns | 39.6 ns | 71.1 ns | 254 ns | 3.48 us | 3.7 us |
| pattern.alloc_list [allocator=pmr_monotonic] | 10000 | 6.89 ns | 7.82 ns | 9.78 ns | 11.4 ns | 87.2 ns | 287 ns | 304 ns |
| pattern.alloc_list [allocator=pmr_pool] | 10000 | 17.4 ns | 19.4 ns | 20.4 ns | 37.4 ns | 97.1 ns | 168 ns | 173 ns |
| pattern.spsc_latency [queue=mutex_condvar] | 10000 | 5.24 us | 11 us | 13.3 us | 31.8 us | 142 us | 913 us | 975 us |
| pattern.spsc_throughput [queue=mutex_condvar] | 15 | 190 ns | 202 ns | 206 ns | 231 ns | 235 ns | 235 ns | 235 ns |
| pattern.spsc_latency [queue=mutex_poll] | 10000 | 176 ns | 854 ns | 1.07 us | 1.65 us | 15.8 us | 32.8 us | 52.3 us |
| pattern.spsc_throughput [queue=mutex_poll] | 15 | 212 ns | 261 ns | 302 ns | 324 ns | 326 ns | 326 ns | 326 ns |
| pattern.spsc_latency [queue=spinlock] | 10000 | 148 ns | 407 ns | 534 ns | 684 ns | 6.3 us | 33.8 us | 34.8 us |
| pattern.spsc_throughput [queue=spinlock] | 15 | 116 ns | 133 ns | 146 ns | 157 ns | 158 ns | 158 ns | 158 ns |
| pattern.spsc_latency [queue=lockfree] | 10000 | 172 ns | 232 ns | 267 ns | 318 ns | 2.22 us | 14 us | 60 us |
| pattern.spsc_throughput [queue=lockfree] | 15 | 12.3 ns | 15.6 ns | 18.3 ns | 103 ns | 115 ns | 116 ns | 116 ns |
| pattern.spsc_latency [queue=lockfree_cached] | 10000 | 174 ns | 243 ns | 282 ns | 337 ns | 5.34 us | 30.4 us | 92.8 us |
| pattern.spsc_throughput [queue=lockfree_cached] | 15 | 75 ns | 83.4 ns | 86.9 ns | 93.3 ns | 94.2 ns | 94.3 ns | 94.3 ns |
| sync.mutex_wakeup | 10000 | 5.66 us | 28.5 us | 53.1 us | 116 us | 367 us | 1.95 ms | 2.16 ms |
| sync.condvar_wakeup | 10000 | 8.64 us | 28 us | 51.5 us | 118 us | 627 us | 2.35 ms | 4.27 ms |
| thread.create_to_start | 10000 | 4.47 us | 6.62 us | 11 us | 90.5 us | 279 us | 854 us | 2.36 ms |
| thread.exit_to_join | 10000 | 3.42 us | 3.79 us | 5.61 us | 8.75 us | 58.6 us | 259 us | 490 us |

*Generated from results.json by tools/olt-report.py. An interactive version with hover details is in report.html next to this file.*
