# Instruction-level parallelism: how far to unroll

**Benchmark:** `cpu.ilp` → results `cpu.ilp` per op and number of chains ·
source: [src/benchmarks/ilp.cpp](../../src/benchmarks/ilp.cpp)

## Goal

Find how many independent operations one core keeps in flight, so you know how
many accumulators a reduction needs, how far to unroll a loop, and how many
independent memory lookups to issue together.

## What is measured

A loop runs N independent dependency chains of one operation. Each step of a
chain needs the previous result, but the chains do not depend on each other:

```cpp
// N = 4 chains of double additions
for (size_t i = 0; i < iters; ++i) {
    acc0 = acc0 + a;
    acc1 = acc1 + a;
    acc2 = acc2 + a;
    acc3 = acc3 + a;
}
// result: elapsed / (iters * N) = ns per addition
```

With one chain every step waits for the previous one, so the loop runs at the
operation's **latency**. With more chains the out-of-order core overlaps them,
until the **throughput** limit: execution ports for arithmetic, load units for
L1 hits, miss buffers (line fill buffers) for DRAM accesses.

Operations: integer multiply, double add, double multiply, a pointer chase
inside L1 (4 KiB), and a pointer chase through a buffer much larger than the
last-level cache (memory-level parallelism). N = 1, 2, 3, 4, 6, 8, 10, 12, 16.

The benchmark stops the compiler from interfering: the operand goes through an
empty `asm` barrier every step (no constant folding into `x + n*a`) and
`ilp.cpp` is compiled without vectorisation (no packing of chains into SIMD).

## Reading the result

The report plots the speed-up over one chain against N. The curve follows the
diagonal (perfect overlap) and then flattens:

* where it flattens is the number of chains worth having: roughly
  latency × instructions per cycle for that operation;
* the height of the plateau is the throughput gain available from unrolling;
* for DRAM loads the plateau is the number of cache misses one core keeps in
  flight, often 10 to 20 on desktop and server cores, fewer on small ARM cores.

A curve that drops again at high N usually means the compiler ran out of
registers and spilled accumulators to the stack.

## Using it in C++

**Reductions.** A single accumulator runs at the add latency:

```cpp
double sum(const double* x, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += x[i];   // one chain: one add per latency
    return s;
}
```

Split it into as many partial sums as the report's "chains to get 90%" for
`double add` (4 on many x86 cores, often more on ARM):

```cpp
double sum(const double* x, size_t n) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += x[i];
        s1 += x[i + 1];
        s2 += x[i + 2];
        s3 += x[i + 3];
    }
    for (; i < n; ++i) s0 += x[i];
    return (s0 + s1) + (s2 + s3);   // note: changes rounding slightly
}
```

Compilers do this on their own for integers. For floating point they may not,
because it changes the rounding of the result; `-ffast-math` or
`-fassociative-math` (or `#pragma omp simd reduction(+:s)`) allows it.

**Independent lookups.** A loop of dependent misses (linked list, tree) runs
at one DRAM latency per element. Interleave independent work so misses
overlap:

```cpp
// One lookup at a time: each probe waits for the previous one's miss.
for (auto key : keys) out.push_back(table.find(key));

// Batches of K lookups: hash all first, prefetch, then probe.
constexpr size_t K = 8;          // from the report's "chains" for DRAM loads
for (size_t i = 0; i + K <= keys.size(); i += K) {
    size_t slot[K];
    for (size_t j = 0; j < K; ++j) {
        slot[j] = table.slot_of(keys[i + j]);
        __builtin_prefetch(&table.buckets[slot[j]]);
    }
    for (size_t j = 0; j < K; ++j) out.push_back(table.probe(slot[j], keys[i + j]));
}
```

* Unrolling beyond the plateau buys nothing and costs registers and code size.
* Most code is not bound by one dependency chain: unroll only loops the
  profiler shows are latency-bound.
