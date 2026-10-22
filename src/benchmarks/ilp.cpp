// Instruction-level parallelism: how many independent dependency chains one
// core overlaps, i.e. how far a loop must be unrolled (how many independent
// accumulators it needs) before the core's execution units are saturated.
//
// Each sample runs N independent chains of the same operation, each step of a
// chain depending on its previous result:
//
//   acc[0] = op(acc[0]); acc[1] = op(acc[1]); ... acc[N-1] = op(acc[N-1]);
//
// With N = 1 the loop runs at the operation's latency. As N grows the
// out-of-order core overlaps the chains until it hits the throughput limit
// (execution ports, load units, outstanding cache misses). The result is ns
// per operation (total time / (iterations * N)) for N = 1 .. 16:
//
//   cpu.ilp [op=int_mul|fp_add|fp_mul]          ALU chains on registers
//   cpu.ilp [op=load_l1]                        pointer chases inside L1
//   cpu.ilp [op=load_mem]                       pointer chases through DRAM:
//                                               memory-level parallelism,
//                                               the number of misses in flight
//
// The operand goes through olt_opaque_*() before every step, so the compiler
// cannot fold a chain into a closed form (x * m * m ... = x * m^n), and this
// file is built with vectorisation disabled (CMakeLists.txt), so it cannot
// pack several chains into one SIMD instruction: the hardware, not the
// optimiser, decides the overlap. Floating-point and load chains are unrolled
// four steps per loop iteration so the loop branch never limits them; the
// integer multiply is not, because integer arithmetic may be reassociated
// inside an unrolled body ((x * m1) * m2 = x * (m1 * m2)), which would break
// the chain. An integer add (1 cycle) is not measured for the same reason:
// without unrolling the loop overhead, not the add, would set the pace.

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/hal/arch.h"
#include "olt/hal/os.h"
#include "mem_util.hpp"

namespace {

constexpr size_t k_alu_iters = size_t(1) << 16;  // steps per chain per sample (ALU, L1)
constexpr size_t k_mem_iters = size_t(1) << 12;  // steps per chain per sample (DRAM)
constexpr size_t k_l1_bytes = 4096;

struct IntMul {
    static constexpr int unroll = 1;
    uint64_t m;
    void hide() { olt_opaque_u64(&m); }
    uint64_t operator()(uint64_t x) const { return x * m; }
};
struct FpAdd {
    static constexpr int unroll = 4;
    double a;
    void hide() { olt_opaque_f64(&a); }
    double operator()(double x) const { return x + a; }
};
struct FpMul {
    static constexpr int unroll = 4;
    double m;
    void hide() { olt_opaque_f64(&m); }
    double operator()(double x) const { return x * m; }
};
struct Chase {
    static constexpr int unroll = 4;
    void hide() {}
    void* operator()(void* p) const { return *static_cast<void**>(p); }
};

// ns per operation for N chains, continuing from (and updating) acc[]. The
// chains run on a local copy so the compiler keeps every accumulator in its
// own register; nothing touches memory inside the timed loop except the loads
// being measured.
template <class T, class Op, size_t... I>
double run_chains(Op op, T (&state)[sizeof...(I)], size_t iters, std::index_sequence<I...>)
{
    T acc[sizeof...(I)] = {state[I]...};
    const uint64_t t0 = olt::now_ns();
    static_assert(Op::unroll == 1 || Op::unroll == 4, "unroll by 1 or 4");
    for (size_t i = 0; i < iters; i += Op::unroll) {
        op.hide();
        ((acc[I] = op(acc[I])), ...);
        if constexpr (Op::unroll == 4) {  // written out: a nested loop here is not reliably unrolled
            op.hide();
            ((acc[I] = op(acc[I])), ...);
            op.hide();
            ((acc[I] = op(acc[I])), ...);
            op.hide();
            ((acc[I] = op(acc[I])), ...);
        }
    }
    const uint64_t t1 = olt::now_ns();
    ((state[I] = acc[I]), ...);
    return double(t1 - t0) / double(iters * sizeof...(I));
}

template <size_t N, class T, class Op, class Seed>
std::vector<double> samples(const olt::Config& cfg, Op op, Seed seed, size_t iters)
{
    T state[N];
    for (size_t j = 0; j < N; ++j) state[j] = seed(j, N);
    std::vector<double> out;
    for (size_t s = 0; s < cfg.sweep_samples + 1; ++s) {  // first sample warms up
        // pointer chases continue where the last sample stopped, so DRAM
        // chains keep reaching lines that are not cached yet
        const double v = run_chains(op, state, iters, std::make_index_sequence<N>{});
        if (s > 0) out.push_back(v);
    }
    olt::do_not_optimize(state);
    return out;
}

template <class T, class Op, class Seed>
void sweep(olt::Context& ctx, const char* name, Op op, Seed seed, size_t iters = k_alu_iters)
{
    auto add = [&](size_t n, std::vector<double> v) {
        ctx.add_result("cpu.ilp", {{"op", name}, {"chains", std::to_string(n)}}, std::move(v));
    };
    const olt::Config& cfg = ctx.config();
    add(1, samples<1, T>(cfg, op, seed, iters));
    add(2, samples<2, T>(cfg, op, seed, iters));
    add(3, samples<3, T>(cfg, op, seed, iters));
    add(4, samples<4, T>(cfg, op, seed, iters));
    add(6, samples<6, T>(cfg, op, seed, iters));
    add(8, samples<8, T>(cfg, op, seed, iters));
    add(10, samples<10, T>(cfg, op, seed, iters));
    add(12, samples<12, T>(cfg, op, seed, iters));
    add(16, samples<16, T>(cfg, op, seed, iters));
}

}  // namespace

OLT_BENCHMARK("cpu.ilp",
              "Instruction-level parallelism: ns per operation with 1 to 16 independent dependency chains "
              "(integer multiply, double add/multiply, L1 loads, DRAM loads); shows how many accumulators a loop "
              "needs to reach the core's throughput (cpu.ilp)")
{
    // Operands come from volatile reads so the compiler cannot constant-fold them.
    volatile uint64_t v_mul = 0x9E3779B97F4A7C15ull;
    volatile double v_fadd = 1e-9, v_fmul = 1.0;
    const uint64_t m = v_mul;
    const double fa = v_fadd, fm = v_fmul;
    auto int_seed = [](size_t j, size_t) { return uint64_t(j + 1); };
    auto fp_seed = [](size_t j, size_t) { return 1.0 + double(j); };

    sweep<uint64_t>(ctx, "int_mul", IntMul{m}, int_seed);
    sweep<double>(ctx, "fp_add", FpAdd{fa}, fp_seed);
    sweep<double>(ctx, "fp_mul", FpMul{fm}, fp_seed);

    // Pointer chases: one random cycle through the buffer, chains start evenly
    // spread along it so they never meet.
    const size_t line = olt_cache_line_size() ? olt_cache_line_size() : 64;
    auto run_chase = [&](const char* name, size_t bytes, size_t iters) {
        char* buf = static_cast<char*>(olt_os_alloc_buffer(bytes, ctx.config().hugepages));
        if (!buf) {
            ctx.add_result("cpu.ilp", {{"op", name}}, {}, "buffer allocation failed");
            return;
        }
        std::mt19937_64 rng(42);
        const size_t lines = bytes / line;
        const std::vector<uint32_t> order = olt::build_chase(buf, lines, line, rng);
        auto seed = [&](size_t j, size_t n) -> void* { return buf + size_t(order[j * lines / n]) * line; };
        sweep<void*>(ctx, name, Chase{}, seed, iters);
        olt_os_free_buffer(buf, bytes);
    };
    run_chase("load_l1", k_l1_bytes, k_alu_iters);
    run_chase("load_mem", olt::resolve_max_bytes(ctx.config()), k_mem_iters);
}
