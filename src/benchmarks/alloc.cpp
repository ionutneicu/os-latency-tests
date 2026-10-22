// Heap allocator latencies: malloc() immediately followed by free(), per size.
// This is the allocator's best case: the freed block is reused by the next
// malloc (thread caches / free lists), and the block is never touched, so
// page-fault cost of first use is excluded. Large sizes typically go straight
// to mmap/munmap. Fragmented-heap and first-touch variants come later.

#include <cstdlib>
#include <string>

#include "olt/benchmark.hpp"

namespace {

std::string size_label(size_t bytes)
{
    if (bytes >= (1u << 20)) return std::to_string(bytes >> 20) + "M";
    if (bytes >= (1u << 10)) return std::to_string(bytes >> 10) + "K";
    return std::to_string(bytes);
}

}  // namespace

OLT_BENCHMARK("alloc.malloc_free",
              "malloc() and free() latency of a single block, for block sizes from 16 B to 16 MiB "
              "(alloc.malloc, alloc.free)")
{
    const olt::Config& cfg = ctx.config();
    const size_t total = cfg.warmup + cfg.samples;
    for (size_t size = 16; size <= (16u << 20); size *= 4) {
        std::vector<double> m, f;
        m.reserve(cfg.samples);
        f.reserve(cfg.samples);
        for (size_t i = 0; i < total; ++i) {
            const uint64_t t0 = olt::now_ns();
            void* p = std::malloc(size);
            olt::do_not_optimize(p);
            const uint64_t t1 = olt::now_ns();
            std::free(p);
            const uint64_t t2 = olt::now_ns();
            if (i >= cfg.warmup) {
                m.push_back(static_cast<double>(t1 - t0));
                f.push_back(static_cast<double>(t2 - t1));
            }
        }
        const olt::Params params{{"size", size_label(size)}};
        const char* note = size == 16 ? "best case: same block reused every iteration, memory never touched" : "";
        ctx.add_result("alloc.malloc", params, std::move(m), note);
        ctx.add_result("alloc.free", params, std::move(f));
    }
}
