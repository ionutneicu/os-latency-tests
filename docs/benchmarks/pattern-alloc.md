# Container memory: default allocator, reserve, pmr arenas

**Benchmark:** `pattern.alloc` → results `pattern.alloc_vector`, `pattern.alloc_list` per allocator ·
source: [src/benchmarks/pattern_alloc.cpp](../../src/benchmarks/pattern_alloc.cpp)

## Goal

Compare where container memory comes from, for a contiguous container
(`std::vector`) and a node container (`std::list`, one allocation per element).

## What is measured

Build a 256-element container with `push_back`, then destroy it; the result is
ns per element:

```cpp
// pmr_monotonic: bump-pointer allocation from a stack buffer, free is a no-op
alignas(std::max_align_t) char buf[k_elems * 48];
std::pmr::monotonic_buffer_resource arena(buf, sizeof(buf));
std::pmr::list<int> l(&arena);
for (size_t i = 0; i < k_elems; ++i) l.push_back(int(i));
// all memory released at once when `arena` goes out of scope
```

Variants: `new_delete` (default `std::allocator`), `reserve` (vector only, one
allocation up front), `pmr_monotonic`, `pmr_pool`
(`std::pmr::unsynchronized_pool_resource` kept across iterations).

## Reading the result

* For vectors, `reserve()` removes the regrowth copies; arenas add little on
  top.
* For lists and maps, the allocator dominates: a monotonic arena is typically
  several times cheaper than new/delete per node.

## Using it in C++

Per-request or per-frame data that dies together belongs in an arena:

```cpp
#include <memory_resource>

void handle(const Request& req) {
    std::array<std::byte, 16 * 1024> stack_buf;              // no heap at all if it fits
    std::pmr::monotonic_buffer_resource arena(stack_buf.data(), stack_buf.size());

    std::pmr::vector<Field> fields(&arena);
    std::pmr::unordered_map<std::pmr::string, int> index(&arena);
    parse(req, fields, index);
    respond(fields, index);
}   // everything freed in one step, no per-node delete
```

* Use `reserve()` whenever the final size is known or bounded.
* `unsynchronized_pool_resource` for long-lived containers that keep inserting
  and erasing on one thread; `synchronized_pool_resource` across threads.
* The arena never reuses freed memory: do not use it for containers that live
  long and churn.
