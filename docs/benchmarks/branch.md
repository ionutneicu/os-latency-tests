# Branch misprediction: branch or branchless?

**Benchmark:** `cpu.branch` → results `cpu.branch` per code and share of passing elements, `cpu.branch_sorted` per code ·
source: [src/benchmarks/branch.cpp](../../src/benchmarks/branch.cpp)

## Goal

Measure what a mispredicted branch costs on this core, and find how
unpredictable a condition must be before branchless code (masks, `cmov` on x86,
`csel` on ARM) beats a plain `if`.

## What is measured

A conditional sum over 8192 `uint32_t` values (32 KiB, L1-resident on most
cores). Each element that passes the test costs a few cycles of work, a hash mix
with two multiplies. The same loop is written three ways:

```cpp
// branchy: a real branch (the empty asm keeps the compiler from making it a cmov)
for (size_t i = 0; i < n; ++i)
    if (a[i] >= t) { s += mix(a[i]); asm volatile("" : "+r"(s)); }

// branchless: always do the work, then mask it away
for (size_t i = 0; i < n; ++i) s += mix(a[i]) & (0u - uint32_t(a[i] >= t));

// ternary: left to the compiler, which picks a branch or a cmov/csel
for (size_t i = 0; i < n; ++i) s += a[i] >= t ? mix(a[i]) : 0;
```

* `cpu.branch [code, taken]`: 0, 1, 2, 5, 10, 20, 30, 40 and 50% of the
  elements pass, at random positions. The branch goes the unlikely way about
  min(p, 100 - p)% of the time, so at 50% the predictor misses about every other
  element.
* `cpu.branch_sorted [code]`: 50% pass, but the data is sorted, so the outcome
  is perfectly predictable. It does the same work as the 50% random case.

The result is ns per element. Every sample runs on freshly generated data (the
generation is not timed). With one fixed array, a modern predictor learns
thousands of "random" outcomes after a few repetitions and the branchy loop
would look far better than it is on real data. The file is built without
auto-vectorisation, so scalar code is compared with scalar code. When perf
counters are available, each result also records `metrics.branch_misses` per
element.

## Reading the result

* **Branchless** is flat: it costs the same whatever the data.
* **Branchy** starts below it, because it skips the work for elements that
  fail, and rises with every mispredict.
* **Mispredict cost** = (50% random − 50% sorted) / misses per element. The two
  runs do the same work, so the difference is the flushes alone. Expect about
  15 to 20 cycles on current x86 and ARM cores, more on deep pipelines.
* **Crossover**: the share of passing elements where the branchless curve
  drops below the branchy one. Below it, a branch is the better choice.
* **Ternary** shows what your compiler does unprompted. GCC and Clang often
  keep a branch when the work on one side is more than a move, so check the
  disassembly rather than assuming you got a `cmov`.

The crossover depends on the work per element. The more work the taken side
does, the longer the branch stays ahead, because branchless code always pays
for it. The mispredict cost is a property of the core.

## Using it in C++

```cpp
// branchless clamp / select: the compiler emits cmov (x86) or csel (ARM)
int clamp(int x, int lo, int hi) { return std::min(std::max(x, lo), hi); }

// branchless conditional add: build a mask from the comparison
sum += value & -static_cast<uint32_t>(value >= limit);

// branchless partition step for a quicksort or a binary search
size_t step = (key < data[mid]) ? 0 : half;     // usually becomes cmov
```

* Go branchless when the condition depends on data that looks random (hash
  bits, sensor noise, unsorted input) and both sides are cheap.
* Keep the branch when one side is rare (error paths, bounds checks), when the
  data is sorted or clustered, or when the skipped work is expensive.
* Sorting or partitioning the input first makes later branches predictable;
  it pays off when the same data is scanned several times.
* `[[likely]]` and `[[unlikely]]` change the code layout, not the prediction:
  they do not fix a branch that is unpredictable.
* Compilers decide by heuristics. Read the disassembly
  (`objdump -d --no-show-raw-insn -C`) or use `__builtin_expect_with_probability`
  or explicit masks when it matters.
