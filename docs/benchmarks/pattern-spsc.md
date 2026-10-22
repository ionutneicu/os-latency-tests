# SPSC queues: mutex vs spinlock vs lock-free

**Benchmark:** `pattern.spsc` → results `pattern.spsc_latency`, `pattern.spsc_throughput` per queue ·
source: [src/benchmarks/pattern_spsc.cpp](../../src/benchmarks/pattern_spsc.cpp)

## Goal

Choose how to pass data from one producer thread to one consumer thread: the
most common hand-off in pipelines (network thread to logic thread, logic to
logger).

## What is measured

The same bounded ring (1024 slots) synchronised five ways: `mutex_condvar`,
`mutex_poll`, `spinlock` (test-and-test-and-set), `lockfree` (acquire/release
indices on separate cache lines) and `lockfree_cached` (each side caches the
other's index).

* **Latency**: ping-pong over two queues; one-way hand-off = half the round trip.
* **Throughput**: stream 256 Ki items through one queue, ns per item.
* **CPU cost**: CPU time of both threads / wall time (2.0 = two cores busy).

The lock-free pop at the heart of it:

```cpp
uint64_t pop() {
    const size_t h = head.load(std::memory_order_relaxed);
    while (tail.load(std::memory_order_acquire) == h) backoff(spins);   // empty: wait
    const uint64_t v = buf[h & (k_capacity - 1)];
    head.store(h + 1, std::memory_order_release);                        // free the slot
    return v;
}
```

## Reading the result

* Producer and consumer on different cores: lock-free hand-off is usually tens
  of times faster than mutex + condvar, and `lockfree_cached` streams several
  times faster than `lockfree`.
* The CPU cost chart shows the price: polling queues keep cores at 100%.
* On the same core, spinning loses badly: the spinner burns the time slice its
  peer needs. The report warns when that happens.

## Using it in C++

A minimal lock-free SPSC ring with cached indices:

```cpp
template <class T, size_t N>   // N: power of two
class SpscRing {
    alignas(128) std::atomic<size_t> head_{0};   // consumer writes
    size_t tail_cache_ = 0;                       // consumer's copy of tail
    alignas(128) std::atomic<size_t> tail_{0};   // producer writes
    size_t head_cache_ = 0;                       // producer's copy of head
    alignas(128) T buf_[N];
public:
    bool try_push(const T& v) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ == N && t - (head_cache_ = head_.load(std::memory_order_acquire)) == N)
            return false;                         // full
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    bool try_pop(T& v) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_ && h == (tail_cache_ = tail_.load(std::memory_order_acquire)))
            return false;                         // empty
        v = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
};
```

* Exactly one producer and one consumer thread; for more, use one ring per
  producer or a proper MPMC queue.
* Pin producer and consumer to different cores, ideally a fast pair from the
  core-to-core matrix; padding uses the value from the false-sharing benchmark.
* If the consumer may wait a long time, add spin-then-park instead of pure
  polling.
