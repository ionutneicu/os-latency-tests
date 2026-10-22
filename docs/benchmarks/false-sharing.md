# False sharing: padding between per-thread data

**Benchmark:** `multicore.false_sharing` → results `multicore.false_sharing` per op and distance, `multicore.false_sharing_alone` ·
source: [src/benchmarks/multicore.cpp](../../src/benchmarks/multicore.cpp)

## Goal

Find the distance that data written by different threads must keep on this
CPU, so per-thread counters, queue indices and locks do not slow each other
down.

## What is measured

Two threads on different physical cores (never SMT siblings, which share the
L1) each increment their own counter. The counters sit 8 to 512 bytes apart in
one page:

```cpp
// op=atomic: each increment is a locked read-modify-write
for (uint64_t i = 0; i < k_increments; ++i) counter->fetch_add(1, std::memory_order_relaxed);

// op=plain: load + store, like ++field on a non-atomic member
for (uint64_t i = 0; i < k_increments; ++i)
    counter->store(counter->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
```

The result is ns per increment of the slower thread. `false_sharing_alone`
runs the same loop on one core, the floor.

## Reading the result

* Below the coherence granule, every write pulls the line away from the other
  core: atomic increments get several times slower.
* Plain stores suffer less, because the store buffer batches them; real code
  that also reads shared data suffers more.
* The report names the smallest distance at which the cost returns to the
  single-core value. That is often 128 bytes on x86, not 64, because the
  adjacent-line prefetcher fetches cache lines in pairs.

## Using it in C++

Give each thread's hot data its own line (or line pair):

```cpp
#include <new>

// Read the padding from the report; 128 is a safe default for x86 and Apple/Neoverse ARM.
inline constexpr std::size_t k_pad = 128;

// Bad: eight threads' counters packed into one or two cache lines.
std::atomic<uint64_t> hits[8];

// Good: one counter per padded slot.
struct alignas(k_pad) PaddedCounter {
    std::atomic<uint64_t> value{0};
};
PaddedCounter hits_padded[8];
```

* `std::hardware_destructive_interference_size` (C++17, `<new>`) is the
  standard name for this value, but compilers often report 64 and GCC warns that
  it is not ABI-stable: prefer the measured number.
* Padding costs memory and cache capacity: pad only data written by different
  threads on the hot path, not every struct.
* Better still, avoid sharing: per-thread counters summed on demand, or one
  writer per cache line by design.
