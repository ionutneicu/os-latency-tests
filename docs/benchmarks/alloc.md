# Heap allocator: malloc and free

**Benchmark:** `alloc.malloc_free` → results `alloc.malloc`, `alloc.free` per block size ·
source: [src/benchmarks/alloc.cpp](../../src/benchmarks/alloc.cpp)

## Goal

Know what one `malloc()`/`new` and one `free()`/`delete` cost per block size,
including the tail, before putting them on the hot path.

## What is measured

One allocation immediately freed, for sizes 16 B to 16 MiB:

```cpp
const uint64_t t0 = olt::now_ns();
void* p = std::malloc(size);
const uint64_t t1 = olt::now_ns();
std::free(p);
const uint64_t t2 = olt::now_ns();   // malloc = t1 - t0, free = t2 - t1
```

This is the allocator's best case: the same block is recycled through its
per-thread cache and never touched, so first-touch page faults are excluded.
Sizes above the mmap threshold (128 KiB by default in glibc) go to
`mmap`/`munmap` and are much slower.

## Reading the result

* Small blocks: tens of nanoseconds at p50 with glibc, but p99.99 can reach
  microseconds (cache refills, lock contention, consolidation).
* Large blocks: the jump at the mmap threshold, and free() costing as much as
  malloc() because `munmap` flushes the TLB.
* Real programs are worse than this best case: fragmented heaps, other threads
  allocating, and first use of fresh pages.

## Using it in C++

Allocate up front; reuse on the hot path:

```cpp
// Before: one heap allocation per message.
void on_message(const Msg& m) {
    auto buf = std::make_unique<char[]>(m.size);   // malloc on every call
    decode(m, buf.get());
}

// After: a buffer kept for the life of the handler.
class Handler {
    std::vector<char> buf_;                        // grows once, then reused
public:
    explicit Handler(size_t max) { buf_.reserve(max); }
    void on_message(const Msg& m) {
        buf_.resize(m.size);                       // no allocation once warmed up
        decode(m, buf_.data());
    }
};
```

* For many small short-lived objects, use an arena or pool (see
  [Container memory](pattern-alloc.md)).
* Touch (pre-fault) buffers at start-up and consider `mlockall` so the first
  hot-path use does not page-fault.
* Allocator replacements (jemalloc, tcmalloc, mimalloc) change these numbers:
  run the benchmark with `LD_PRELOAD` to compare.
