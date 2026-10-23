# Atomics: shared counters and memory ordering

**Benchmarks:** `multicore.atomic_contention` (per op and thread count), `multicore.atomic_order` (per op) ·
source: [src/benchmarks/atomic.cpp](../../src/benchmarks/atomic.cpp)

## Goal

Find out what an atomic read-modify-write costs once several cores update the
same variable, and what each `std::memory_order` costs when nothing is
contended. These two numbers decide how to write counters, statistics,
reference counts and the indices of lock-free structures.

## What is measured

**Contention.** 1 to N threads, one per physical core (SMT siblings skipped,
starting with the main benchmark CPU), start together and each add 1 to a
counter 65536 times:

```cpp
// fetch_add: one shared counter
shared.fetch_add(1, std::memory_order_relaxed);

// cas: the compare-exchange loop lock-free code uses for anything beyond "+1"
uint64_t v = shared.load(std::memory_order_relaxed);
while (!shared.compare_exchange_weak(v, v + 1, std::memory_order_relaxed)) {}

// sharded: one counter per thread, each in its own 128-byte block
own[thread].fetch_add(1, std::memory_order_relaxed);
```

The result is ns per increment as each thread sees it: the slowest thread's
time divided by its increments. For `cas`, `metrics.retries` counts failed
attempts per increment. The run checks that the counters add up.

**Memory ordering.** One thread and one counter on its own line, with no
contention. Each sample is a loop of 1024 operations:

* `load_relaxed`, `load_acquire`, `load_seq_cst`
* `store_relaxed`, `store_release`, `store_seq_cst`
* `fetch_add_relaxed`, `fetch_add_acq_rel`, `fetch_add_seq_cst`
* `fence_seq_cst`: a plain store followed by
  `std::atomic_thread_fence(seq_cst)`

## Reading the result

* **Shared counters:** with one thread, an atomic increment costs about
  15 to 20 cycles on x86 (`lock add`) and less on ARM (an `ldadd`, executed in
  the cache). From two cores on, the cache line moves to the incrementing core
  on every increment, so each one costs a core-to-core transfer (see
  `multicore.c2c`). The total throughput of all cores together is often lower
  than one core alone.
* **Compare-exchange** is worse under contention than `fetch_add`: a thread
  can lose the race and retry, and every retry is another line transfer.
* **Sharded** counters stay at the single-core cost whatever the core count.
* **Memory order on x86:** every load is already acquire and every store
  already release, so those cost the same as relaxed. The seq_cst store (an
  `xchg`) and the fence (`mfence` or a locked instruction) cost about as much
  as an atomic increment, which is 10 to 20 times a plain store. Read-modify-
  writes are always full barriers on x86, whatever order you ask for.
* **Memory order on ARM:** acquire loads (`ldar`/`ldapr`), release stores
  (`stlr`), seq_cst and the various `ldadd` forms are separate instructions,
  so weaker orders can be cheaper even for read-modify-writes.

## Using it in C++

```cpp
// Per-thread statistics: no sharing on the hot path, a sum when someone reads
struct alignas(128) Slot { std::atomic<uint64_t> n{0}; };
std::array<Slot, kMaxThreads> hits;

void count(unsigned tid) { hits[tid].n.fetch_add(1, std::memory_order_relaxed); }
uint64_t total() {
    uint64_t s = 0;
    for (auto& h : hits) s += h.n.load(std::memory_order_relaxed);
    return s;
}

// Publishing data to another thread: release / acquire, not seq_cst
data = compute();
ready.store(true, std::memory_order_release);      // not ready = true (seq_cst)
...
if (ready.load(std::memory_order_acquire)) use(data);
```

* `std::atomic::operator=`, `operator++` and plain `store()` default to
  `seq_cst`. On a hot path, spell out the order you need.
* Keep counters that are only read for statistics as `relaxed`, one per
  thread.
* A reference count (`shared_ptr`) is a shared atomic: copying a
  `std::shared_ptr` across threads on a hot path pays the contention cost
  shown here. Pass `const&` or move instead.
* `seq_cst` is needed only when correctness depends on a single total order
  of stores to *different* variables (Dekker-style flag handshakes). Use it
  there, and say why in a comment.
