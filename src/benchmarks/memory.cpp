// Memory hierarchy sweeps: access cost as a function of working-set size, in
// one pre-allocated, pre-faulted buffer. Plotted against buffer size, the
// steps in these curves line up with L1/L2/LLC capacity and TLB reach.
//
//  mem.latency_random  dependent loads along a random cyclic permutation of
//                      cache lines (pointer chase). Defeats hardware
//                      prefetchers, so it shows raw load-to-use latency of
//                      the level the working set fits in. Unit: ns/access.
//  mem.read_seq        sequential read of the buffer (prefetch friendly,
//                      vectorisable). Unit: ns per cache line; GB/s = line/ns.
//  mem.write_seq       sequential write of the buffer. Unit: ns per cache line.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/hal/arch.h"
#include "olt/hal/os.h"
#include "mem_util.hpp"

namespace {

constexpr size_t k_chase_accesses = size_t(1) << 17;  // loads per latency sample
constexpr size_t k_stream_bytes = size_t(32) << 20;   // bytes touched per bandwidth sample

std::string size_label(size_t bytes)
{
    char buf[32];
    if (bytes >= (size_t(1) << 30) && bytes % (size_t(1) << 20) == 0)
        std::snprintf(buf, sizeof(buf), "%.3gG", double(bytes) / double(size_t(1) << 30));
    else if (bytes >= (size_t(1) << 20))
        std::snprintf(buf, sizeof(buf), "%.3gM", double(bytes) / double(size_t(1) << 20));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.3gK", double(bytes) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%zu", bytes);
    return buf;
}

// Largest cache visible to `cpu`, 0 if unknown.
// Geometric size series, steps_per_octave points per doubling, rounded to whole lines.
std::vector<size_t> sweep_sizes(size_t lo, size_t hi, unsigned steps_per_octave, size_t line)
{
    std::vector<size_t> sizes;
    lo = std::max(lo, 2 * line);
    for (unsigned k = 0;; ++k) {
        const double s = double(lo) * std::pow(2.0, double(k) / double(steps_per_octave));
        if (s > double(hi) * 1.0001) break;
        const size_t rounded = std::max(line, size_t(s) / line * line);
        if (sizes.empty() || rounded != sizes.back()) sizes.push_back(rounded);
    }
    return sizes;
}

double chase_sample(void*& p)
{
    void* q = p;
    const uint64_t t0 = olt::now_ns();
    for (size_t i = 0; i < k_chase_accesses; i += 8) {
        q = *static_cast<void**>(q); q = *static_cast<void**>(q);
        q = *static_cast<void**>(q); q = *static_cast<void**>(q);
        q = *static_cast<void**>(q); q = *static_cast<void**>(q);
        q = *static_cast<void**>(q); q = *static_cast<void**>(q);
    }
    const uint64_t t1 = olt::now_ns();
    olt::do_not_optimize(q);
    p = q;
    return double(t1 - t0) / double(k_chase_accesses);
}

double read_sample(const char* buf, size_t size, size_t line)
{
    const size_t passes = std::max<size_t>(1, k_stream_bytes / size);
    const uint64_t* w = reinterpret_cast<const uint64_t*>(buf);
    const size_t words = size / sizeof(uint64_t);
    // independent accumulators so the loop is bound by load throughput, not by
    // the latency of a single add chain (sizes are whole lines, lines >= 32 B)
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    const uint64_t t0 = olt::now_ns();
    for (size_t p = 0; p < passes; ++p) {
        for (size_t i = 0; i < words; i += 4) {
            s0 += w[i];
            s1 += w[i + 1];
            s2 += w[i + 2];
            s3 += w[i + 3];
        }
        olt_compiler_barrier();  // keep passes from being merged
    }
    const uint64_t t1 = olt::now_ns();
    olt::do_not_optimize(s0 + s1 + s2 + s3);
    return double(t1 - t0) / (double(passes) * double(size) / double(line));
}

double write_sample(char* buf, size_t size, size_t line)
{
    const size_t passes = std::max<size_t>(1, k_stream_bytes / size);
    uint64_t* w = reinterpret_cast<uint64_t*>(buf);
    const size_t words = size / sizeof(uint64_t);
    const uint64_t t0 = olt::now_ns();
    for (size_t p = 0; p < passes; ++p) {
        // index-dependent value keeps the compiler from turning this into memset
        for (size_t i = 0; i < words; ++i) w[i] = i ^ p;
        olt_compiler_barrier();
    }
    const uint64_t t1 = olt::now_ns();
    return double(t1 - t0) / (double(passes) * double(size) / double(line));
}

}  // namespace

OLT_BENCHMARK("mem.cache_sweep",
              "Working-set sweep over one pre-faulted buffer: random pointer-chase latency "
              "(mem.latency_random), sequential read and write cost per cache line (mem.read_seq, "
              "mem.write_seq), for buffer sizes from --mem-min to --mem-max")
{
    const olt::Config& cfg = ctx.config();
    const size_t line = std::max<size_t>(olt_cache_line_size(), sizeof(void*));
    const size_t max_bytes = std::max(olt::resolve_max_bytes(cfg), 2 * line);
    const std::vector<size_t> sizes = sweep_sizes(cfg.mem_min_bytes, max_bytes, cfg.sweep_steps_per_octave, line);

    char* buf = static_cast<char*>(olt_os_alloc_buffer(sizes.back(), cfg.hugepages ? 1 : 0));
    if (!buf) {
        ctx.add_result("mem.latency_random", {}, {}, "buffer allocation failed, lower --mem-max");
        return;
    }

    const std::string pages = cfg.hugepages ? "huge" : "base";
    const std::string tlb_note = "latency includes TLB misses once the buffer exceeds TLB reach (try --hugepages)";
    std::mt19937_64 rng(0x6f6c74);  // fixed seed: identical access pattern across runs and machines
    for (size_t size : sizes) {
        const olt::Params params{{"size", size_label(size)}, {"bytes", std::to_string(size)}, {"pages", pages}};

        olt::build_chase(buf, size / line, line, rng);
        void* p = buf;
        chase_sample(p);  // warm-up: bring the working set into cache
        std::vector<double> lat;
        for (size_t s = 0; s < cfg.sweep_samples; ++s) lat.push_back(chase_sample(p));
        ctx.add_result("mem.latency_random", params, std::move(lat), size == sizes.front() && !cfg.hugepages ? tlb_note : "");
        ctx.results().back().unit = "ns/access";

        read_sample(buf, size, line);
        std::vector<double> rd;
        for (size_t s = 0; s < cfg.sweep_samples; ++s) rd.push_back(read_sample(buf, size, line));
        ctx.add_result("mem.read_seq", params, std::move(rd));
        ctx.results().back().unit = "ns/line";

        write_sample(buf, size, line);
        std::vector<double> wr;
        for (size_t s = 0; s < cfg.sweep_samples; ++s) wr.push_back(write_sample(buf, size, line));
        ctx.add_result("mem.write_seq", params, std::move(wr));
        ctx.results().back().unit = "ns/line";
    }
    olt_os_free_buffer(buf, sizes.back());
}
