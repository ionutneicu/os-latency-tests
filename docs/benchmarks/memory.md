# Memory hierarchy: cache sizes and where performance drops

**Benchmark:** `mem.cache_sweep` → results `mem.latency_random`, `mem.write_random`, `mem.read_seq`, `mem.write_seq`, `mem.write_nt` per buffer size ·
source: [src/benchmarks/memory.cpp](../../src/benchmarks/memory.cpp)

## Goal

Show, on this machine, how access cost grows as the working set outgrows L1,
L2, the last-level cache and the TLB, so data structures can be sized to stay
in the fast levels.

## What is measured

One pre-faulted buffer, swept from 1 KiB up to well past the last-level cache.

**Random load latency**: a pointer chase through every cache line in random
order (a single cycle built with Sattolo's algorithm), so each load depends on
the previous one and prefetchers cannot guess the next address:

```cpp
void* p = start;
for (size_t i = 0; i < k_chase_accesses; ++i)
    p = *static_cast<void**>(p);       // one dependent load per iteration
// sample = elapsed / k_chase_accesses  (ns per load)
```

**Random store throughput**: one 8-byte store per cache line, visiting the
lines in the same random order. A store to a line that is not in cache must
fetch it first (read for ownership), and the prefetchers cannot guess the next
line. But nothing depends on a store, so the core does not wait: the store
buffer holds it and many misses overlap. Reported as ns per store.

```cpp
for (size_t i = 0; i < k_random_stores; ++i)
    *reinterpret_cast<uint64_t*>(buf + order[i % lines] * line) = i;   // independent stores
```

**Sequential read and write**: streaming through the same buffer with the
widest vector loads and stores the CPU has (AVX-512, AVX2 or SSE2 on x86-64,
picked at run time; NEON on AArch64), four independent accumulators for reads
and two stores per iteration, so neither the instruction width nor the loop
overhead hides the cache's bandwidth. The prefetcher-friendly best case.
Reported as ns per cache line and GB/s; the ISA used is in the result's `isa`
parameter. Kernels: [src/hal/stream.c](../../src/hal/stream.c).

**Streaming write**: the same sequential write with non-temporal stores
(`movntdq` on x86-64, `stnp` on AArch64), which go towards memory without
filling the caches and without reading the line first.

## Reading the result

* Flat steps separated by cliffs: each step is the latency of one level; the
  cliff sits near that level's size (marked from the OS-reported cache sizes).
* The jump past the last cache to DRAM is typically 50 to 200x an L1 hit.
* Without `--hugepages`, latency past the TLB reach also includes page walks.
* Sequential bandwidth drops far less than random latency: prefetching works.
* Writes have steps too, but much smaller ones than random loads, because
  stores never make the core wait. A normal write to DRAM is slower than a
  read: each line is read in (read for ownership), then written back.
* Random stores cost a fraction of a random load at every level (often ~10x
  less in DRAM): that is store throughput with overlapped misses, not latency.
* Streaming writes are slow while the buffer would fit in cache (the data has
  to go all the way out) and fastest of all beyond the last cache, often
  2-3x a normal write, because they skip the read for ownership.
* L1 and L2 bandwidth depend on the vector width: L1 serves as many bytes per
  cycle as the core requests (two 64-byte loads per cycle on recent x86), so
  scalar or SSE code reaches only a quarter to a half of the figures here. From
  L3 outwards the memory, not the instruction width, is the limit.

## Using it in C++

Size hot data to fit a level, and lay it out so access is sequential:

```cpp
// Pointer-heavy: every node is a potential cache miss.
std::list<Order> orders;
std::map<int, Level> book;

// Contiguous: one cache line serves several elements, and the prefetcher helps.
std::vector<Order> orders;                         // iterate in order
std::vector<std::pair<int, Level>> book;           // sorted, binary search
```

* Keep the per-core hot working set under about 3/4 of L1 (the report's hint
  gives the number for this CPU) and per-thread tables under about 3/4 of L2.
* Split hot fields from cold ones (structure splitting) so the hot loop touches
  fewer lines.
* For large lookup tables, try huge pages (`madvise(MADV_HUGEPAGE)`) and compare
  with the `--hugepages` sweep.

**Writes.** Scattered writes (histogram bins, scatter, log records) are much
cheaper than scattered reads, as long as nothing reads the data back right
away and the store buffer does not fill up. For a large output buffer written
once and not read again soon (frame dumps, packet capture, bulk copies larger
than the last cache), streaming stores avoid reading every line in and keep
the useful data in cache:

```cpp
#include <immintrin.h>   // x86-64; on AArch64 use stnp

// dst 16-byte aligned, n multiple of 16, larger than the last-level cache
void copy_stream(void* dst, const void* src, size_t n) {
    auto* d = static_cast<__m128i*>(dst);
    auto* s = static_cast<const __m128i*>(src);
    for (size_t i = 0; i < n / 16; ++i)
        _mm_stream_si128(d + i, _mm_loadu_si128(s + i));
    _mm_sfence();   // streaming stores are weakly ordered: fence before publishing
}
```

* Do not stream data that will be read again soon: it is no longer in cache.
* glibc `memcpy`/`memset` already switch to streaming stores for very large
  sizes; measure before writing your own.
