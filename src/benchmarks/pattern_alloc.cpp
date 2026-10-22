// Design pattern: where container memory comes from.
//
// Builds a small container (256 push_backs) and destroys it, per allocation
// strategy. Contiguous (std::vector) and node-based (std::list) containers
// are measured separately because node containers allocate once per element.
//
//   new_delete     default std::allocator (global operator new / malloc)
//   reserve        std::vector with reserve() up front: one allocation
//   pmr_monotonic  std::pmr::monotonic_buffer_resource over a stack buffer:
//                  bump-pointer allocation, free is a no-op, all memory is
//                  released at once when the resource goes out of scope
//   pmr_pool       std::pmr::unsynchronized_pool_resource kept alive across
//                  iterations: recycled fixed-size blocks, single-threaded
//
// Unit: ns per element (build + destroy, divided by the element count).

#include <list>
#include <memory_resource>
#include <vector>

#include "olt/benchmark.hpp"

namespace {

constexpr size_t k_elems = 256;

template <class Build>
double per_element(Build&& build)
{
    const uint64_t t0 = olt::now_ns();
    build();
    const uint64_t t1 = olt::now_ns();
    return double(t1 - t0) / double(k_elems);
}

}  // namespace

OLT_BENCHMARK("pattern.alloc",
              "Container memory strategies: build and destroy a 256-element std::vector "
              "(pattern.alloc_vector) and std::list (pattern.alloc_list) with new/delete, reserve, "
              "pmr monotonic buffer and pmr unsynchronized pool")
{
    std::pmr::unsynchronized_pool_resource pool;

    // --- contiguous container
    ctx.add_result("pattern.alloc_vector", {{"allocator", "new_delete"}}, ctx.run_samples([] {
        return per_element([] {
            std::vector<int> v;
            for (size_t i = 0; i < k_elems; ++i) v.push_back(int(i));
            olt::do_not_optimize(v.data());
        });
    }));
    ctx.add_result("pattern.alloc_vector", {{"allocator", "reserve"}}, ctx.run_samples([] {
        return per_element([] {
            std::vector<int> v;
            v.reserve(k_elems);
            for (size_t i = 0; i < k_elems; ++i) v.push_back(int(i));
            olt::do_not_optimize(v.data());
        });
    }));
    ctx.add_result("pattern.alloc_vector", {{"allocator", "pmr_monotonic"}}, ctx.run_samples([] {
        return per_element([] {
            alignas(std::max_align_t) char buf[4 * k_elems * sizeof(int)];  // room for growth copies
            std::pmr::monotonic_buffer_resource arena(buf, sizeof(buf));
            std::pmr::vector<int> v(&arena);
            for (size_t i = 0; i < k_elems; ++i) v.push_back(int(i));
            olt::do_not_optimize(v.data());
        });
    }));
    ctx.add_result("pattern.alloc_vector", {{"allocator", "pmr_pool"}}, ctx.run_samples([&pool] {
        return per_element([&pool] {
            std::pmr::vector<int> v(&pool);
            for (size_t i = 0; i < k_elems; ++i) v.push_back(int(i));
            olt::do_not_optimize(v.data());
        });
    }));

    // --- node-based container: one allocation per element
    ctx.add_result("pattern.alloc_list", {{"allocator", "new_delete"}}, ctx.run_samples([] {
        return per_element([] {
            std::list<int> l;
            for (size_t i = 0; i < k_elems; ++i) l.push_back(int(i));
            olt::do_not_optimize(l.back());
        });
    }));
    ctx.add_result("pattern.alloc_list", {{"allocator", "pmr_monotonic"}}, ctx.run_samples([] {
        return per_element([] {
            alignas(std::max_align_t) char buf[k_elems * 48];  // list node: 2 pointers + int, padded
            std::pmr::monotonic_buffer_resource arena(buf, sizeof(buf));
            std::pmr::list<int> l(&arena);
            for (size_t i = 0; i < k_elems; ++i) l.push_back(int(i));
            olt::do_not_optimize(l.back());
        });
    }));
    ctx.add_result("pattern.alloc_list", {{"allocator", "pmr_pool"}}, ctx.run_samples([&pool] {
        return per_element([&pool] {
            std::pmr::list<int> l(&pool);
            for (size_t i = 0; i < k_elems; ++i) l.push_back(int(i));
            olt::do_not_optimize(l.back());
        });
    }));
    for (auto& r : ctx.results())
        if (r.name.rfind("pattern.alloc_", 0) == 0) r.unit = "ns/element";
}
