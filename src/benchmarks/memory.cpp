// Memory hierarchy sweeps: access cost as a function of working-set size, in
// one pre-allocated, pre-faulted buffer. Plotted against buffer size, the
// steps in these curves line up with L1/L2/LLC capacity and TLB reach.
//
//  mem.latency_random  dependent loads along a random cyclic permutation of
//                      cache lines (pointer chase). Defeats hardware
//                      prefetchers, so it shows raw load-to-use latency of
//                      the level the working set fits in. Unit: ns/access.
//  mem.read_seq        sequential read of the buffer. Unit: ns per cache line;
//                      GB/s = line/ns.
//  mem.write_seq       sequential write of the buffer. Unit: ns per cache line.
//  mem.write_nt        sequential non-temporal (streaming) write: stores that
//                      bypass the caches. Unit: ns per cache line.
//  mem.write_random    one 8-byte store per cache line, lines in random order:
//                      every store to a line not in cache must fetch it first
//                      (read for ownership), and the prefetchers cannot guess
//                      the next line. The stores are independent, so their
//                      misses overlap: this is store throughput per level, not
//                      latency. Unit: ns per store.
//
// Sequential read and write use 16-byte vectors (SSE2 / NEON, the baseline of
// both architectures), two per loop iteration, so the loop itself is not the
// bottleneck. Code built for AVX2/AVX-512 can move up to 2-4x more per cycle
// in L1 and L2; from L3 outwards the memory, not the instruction width, limits.

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
constexpr size_t k_random_stores = size_t(1) << 17;   // stores per random-write sample

// 16-byte vector of two u64 (GCC/Clang vector extension: SSE2 on x86-64, NEON
// on AArch64); may_alias because it is laid over a char buffer.
typedef uint64_t v2u64 __attribute__((vector_size(16), may_alias));

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
    const v2u64* v = reinterpret_cast<const v2u64*>(buf);
    const size_t vecs = size / sizeof(v2u64);
    // two accumulators so the loop is bound by load throughput, not by the
    // latency of one add chain (sizes are whole lines, lines >= 32 B)
    v2u64 s0 = {0, 0}, s1 = {0, 0};
    const uint64_t t0 = olt::now_ns();
    for (size_t p = 0; p < passes; ++p) {
        for (size_t i = 0; i < vecs; i += 2) {
            s0 += v[i];
            s1 += v[i + 1];
        }
        olt_compiler_barrier();  // keep passes from being merged
    }
    const uint64_t t1 = olt::now_ns();
    const v2u64 s = s0 + s1;
    olt::do_not_optimize(s[0] + s[1]);
    return double(t1 - t0) / (double(passes) * double(size) / double(line));
}

double write_sample(char* buf, size_t size, size_t line)
{
    const size_t passes = std::max<size_t>(1, k_stream_bytes / size);
    v2u64* v = reinterpret_cast<v2u64*>(buf);
    const size_t vecs = size / sizeof(v2u64);
    const uint64_t t0 = olt::now_ns();
    for (size_t p = 0; p < passes; ++p) {
        // a value the compiler cannot see, so the loop cannot become memset
        uint64_t x = p + 1;
        olt_opaque_u64(&x);
        const v2u64 val = {x, ~x};
        for (size_t i = 0; i < vecs; i += 2) {
            v[i] = val;
            v[i + 1] = val;
        }
        olt_compiler_barrier();
    }
    const uint64_t t1 = olt::now_ns();
    return double(t1 - t0) / (double(passes) * double(size) / double(line));
}

double write_nt_sample(char* buf, size_t size, size_t line)
{
    const size_t passes = std::max<size_t>(1, k_stream_bytes / size);
    const uint64_t t0 = olt::now_ns();
    for (size_t p = 0; p < passes; ++p) {
        for (size_t off = 0; off < size; off += 32) {
            olt_stream_store_16(buf + off, p, off);
            olt_stream_store_16(buf + off + 16, p, off);
        }
        olt_stream_fence();  // the streaming stores are complete before the clock is read
    }
    const uint64_t t1 = olt::now_ns();
    return double(t1 - t0) / (double(passes) * double(size) / double(line));
}

// Stores to the lines of `order` (a random permutation), continuing at `pos`
// so successive samples keep reaching lines that are not cached yet.
double write_random_sample(char* buf, const std::vector<uint32_t>& order, size_t line, size_t& pos)
{
    const uint32_t* o = order.data();
    const size_t lines = order.size();
    size_t k = pos;
    const uint64_t t0 = olt::now_ns();
    for (size_t i = 0; i < k_random_stores; ++i) {
        *reinterpret_cast<uint64_t*>(buf + size_t(o[k]) * line) = i;
        if (++k == lines) k = 0;
    }
    olt_compiler_barrier();
    const uint64_t t1 = olt::now_ns();
    pos = k;
    return double(t1 - t0) / double(k_random_stores);
}

}  // namespace

OLT_BENCHMARK("mem.cache_sweep",
              "Working-set sweep over one pre-faulted buffer: random pointer-chase latency "
              "(mem.latency_random), sequential read, write and streaming-write cost per cache line "
              "(mem.read_seq, mem.write_seq, mem.write_nt) and random-order store throughput "
              "(mem.write_random), for buffer sizes from --mem-min to --mem-max")
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

        const std::vector<uint32_t> order = olt::build_chase(buf, size / line, line, rng);
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

        write_nt_sample(buf, size, line);
        std::vector<double> nt;
        for (size_t s = 0; s < cfg.sweep_samples; ++s) nt.push_back(write_nt_sample(buf, size, line));
        ctx.add_result("mem.write_nt", params, std::move(nt));
        ctx.results().back().unit = "ns/line";

        size_t pos = 0;
        write_random_sample(buf, order, line, pos);  // warm-up
        std::vector<double> wrr;
        for (size_t s = 0; s < cfg.sweep_samples; ++s) wrr.push_back(write_random_sample(buf, order, line, pos));
        ctx.add_result("mem.write_random", params, std::move(wrr));
        ctx.results().back().unit = "ns/store";
    }
    olt_os_free_buffer(buf, sizes.back());
}
