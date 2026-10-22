# Memory hierarchy: cache sizes and where performance drops

**Benchmark:** `mem.cache_sweep` → results `mem.latency_random`, `mem.read_seq`, `mem.write_seq` per buffer size ·
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

**Sequential read and write**: streaming through the same buffer with
independent accumulators, the prefetcher-friendly best case. Reported as ns per
cache line and GB/s.

## Reading the result

* Flat steps separated by cliffs: each step is the latency of one level; the
  cliff sits near that level's size (marked from the OS-reported cache sizes).
* The jump past the last cache to DRAM is typically 50 to 200x an L1 hit.
* Without `--hugepages`, latency past the TLB reach also includes page walks.
* Sequential bandwidth drops far less than random latency: prefetching works.

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
