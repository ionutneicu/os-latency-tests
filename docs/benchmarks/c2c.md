# Core-to-core latency

**Benchmark:** `multicore.c2c` → result `multicore.c2c_latency` per CPU pair ·
source: [src/benchmarks/multicore.cpp](../../src/benchmarks/multicore.cpp)

## Goal

Map how long a cache line takes to travel between each pair of CPUs, so that
threads which talk to each other can be placed on a fast pair.

## What is measured

Two threads pinned to CPUs a and b ping-pong a counter in one
`std::atomic<uint64_t>` on its own cache line. The initiator writes an odd
value, the responder answers with the next even one:

```cpp
// initiator (CPU a)                          // responder (CPU b)
line.v.store(next, release);                  // while (line.v.load(acquire) != expect) cpu_relax();
while (line.v.load(acquire) != next + 1)      // line.v.store(expect + 1, release);
    cpu_relax();
// one-way latency = batch time / (2 * round trips)
```

Every pair of allowed CPUs is measured; the report draws a heatmap.

## Reading the result

* SMT siblings (two hardware threads of one core) are fastest: they share L1.
* Cores in the same cluster, die or CCX come next; other clusters, chiplets or
  sockets are slowest, sometimes 2 to 5x.
* The values are a floor for any inter-thread communication between those CPUs,
  whatever the queue or lock on top.

## Using it in C++

Pin communicating threads to a fast pair, and keep unrelated threads apart:

```cpp
#include <pthread.h>

void pin_to(std::thread& t, int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(t.native_handle(), sizeof(set), &set);
}

std::thread producer(produce), consumer(consume);
pin_to(producer, 2);   // a fast pair from the report's heatmap
pin_to(consumer, 3);
```

* The pair that wins is machine-specific; read it from the report rather than
  assuming neighbouring CPU numbers are close (on many Intel laptops CPU n and
  n+4 are SMT siblings).
* SMT siblings are fastest to talk to but share execution units: a busy
  sibling slows both. For two busy threads, prefer two cores in one cluster.
