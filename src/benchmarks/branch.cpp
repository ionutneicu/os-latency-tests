// Branch misprediction and the branchless crossover.
//
// A conditional sum over 8192 uint32 values (32 KiB, L1-resident on most
// cores), with a given share of the elements passing the test; each passing
// element costs a few cycles of work (a hash mix, two multiplies):
//
//   for (x : data) if (x >= threshold) sum += mix(x);
//
// written three ways:
//
//   branchy     a real conditional branch (an empty asm statement in the taken
//               path stops the compiler from turning it into cmov)
//   branchless  sum += mix(x) & -(x >= threshold): always does the work, then
//               masks it away; no branch at all
//   ternary     sum += x >= threshold ? mix(x) : 0, left to the compiler, which
//               picks a branch or a cmov/csel on its own
//
// Branchy code pays only for passing elements, plus a pipeline flush per
// mispredict; branchless code always pays for the work. Where the two curves
// cross is the predictability below which a branch is the better choice.
//
// Results, in ns per element:
//
//   cpu.branch [code, taken=0..50]   passing elements at random positions,
//                                    taken percent of them: the outcome is
//                                    unpredictable about min(p, 1-p) of the time
//   cpu.branch_sorted [code]         50% passing, but sorted: perfectly predictable
//
// Every sample uses freshly generated data (not timed): with a fixed array the
// branch predictor of a modern core learns thousands of "random" outcomes
// after a few repetitions and the branchy loop would look far too good. The
// file is built without auto-vectorisation (CMakeLists.txt), so the three
// scalar forms are compared, not SIMD. When perf counters are available, each
// result also records branch misses per element (metrics.branch_misses).

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/hal/perf.h"

namespace {

constexpr size_t k_elems = 8192;
constexpr uint32_t k_threshold = 1u << 30;
const int k_taken_percent[] = {0, 1, 2, 5, 10, 20, 30, 40, 50};

struct Rng {  // xorshift64*: fast, so regenerating the data stays cheap
    uint64_t s;
    uint32_t next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return uint32_t((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
};

// Each element passes (>= k_threshold) with probability percent/100.
void fill(std::vector<uint32_t>& v, int percent, Rng& rng)
{
    const uint64_t cut = uint64_t(percent) * (uint64_t(1) << 32) / 100;
    for (auto& x : v) {
        const uint32_t value = rng.next() & (k_threshold - 1);
        x = rng.next() < cut ? value | k_threshold : value;
    }
}

inline uint32_t mix(uint32_t x)  // the work done for a passing element
{
    x = (x ^ (x >> 15)) * 0x2C1B3C6Du;
    return (x ^ (x >> 12)) * 0x297A2D39u;
}

constexpr olt::SourceLine at_branchy = OLT_HERE;  // linked from the report
uint64_t sum_branchy(const uint32_t* a, size_t n, uint32_t t)
{
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] >= t) {
            s += mix(a[i]);
            asm volatile("" : "+r"(s));  // a side effect: the compiler must keep the branch
        }
    }
    return s;
}

constexpr olt::SourceLine at_branchless = OLT_HERE;
uint64_t sum_branchless(const uint32_t* a, size_t n, uint32_t t)
{
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) s += mix(a[i]) & (0u - uint32_t(a[i] >= t));
    return s;
}

constexpr olt::SourceLine at_ternary = OLT_HERE;
uint64_t sum_ternary(const uint32_t* a, size_t n, uint32_t t)
{
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) s += a[i] >= t ? mix(a[i]) : 0;
    return s;
}

using SumFn = uint64_t (*)(const uint32_t*, size_t, uint32_t);
struct Code {
    const char* name;
    SumFn fn;
    olt::SourceLine source;
};
const Code k_codes[] = {{"branchy", sum_branchy, at_branchy},
                        {"branchless", sum_branchless, at_branchless},
                        {"ternary", sum_ternary, at_ternary}};

// ns per element over `samples` runs, each on fresh data; branch misses per
// element are stored in *misses (-1 if the counter is unavailable).
std::vector<double> run(const Code& c, int percent, bool sorted, size_t samples, olt_perf_group_t* perf,
                        double* misses)
{
    std::vector<uint32_t> data(k_elems);
    Rng rng{0x9E3779B97F4A7C15ull + uint64_t(percent)};
    volatile uint32_t v_threshold = k_threshold;  // unknown to the compiler
    const uint32_t t = v_threshold;
    std::vector<double> out;
    uint64_t miss_total = 0;
    bool have_misses = perf && olt_perf_has(perf, OLT_PERF_BRANCH_MISSES);
    for (size_t s = 0; s < samples + 1; ++s) {  // the first run warms up
        fill(data, percent, rng);
        if (sorted) std::sort(data.begin(), data.end());
        uint64_t before[OLT_PERF_EVENT_COUNT] = {}, after[OLT_PERF_EVENT_COUNT] = {};
        if (have_misses) olt_perf_read(perf, before);
        const uint64_t t0 = olt::now_ns();
        uint64_t sum = c.fn(data.data(), k_elems, t);
        const uint64_t t1 = olt::now_ns();
        if (have_misses) olt_perf_read(perf, after);
        olt::do_not_optimize(sum);
        if (s == 0) continue;
        out.push_back(double(t1 - t0) / double(k_elems));
        miss_total += after[OLT_PERF_BRANCH_MISSES] - before[OLT_PERF_BRANCH_MISSES];
    }
    *misses = have_misses ? double(miss_total) / double(samples * k_elems) : -1.0;
    return out;
}

}  // namespace

OLT_BENCHMARK("cpu.branch",
              "Branch misprediction: ns per element of a conditional sum with 0 to 50% of the elements passing at "
              "random positions (cpu.branch) or sorted (cpu.branch_sorted), as a branch, a branchless mask and a "
              "ternary left to the compiler; shows the cost of a mispredict and where branchless code wins")
{
    const size_t samples = std::max<size_t>(ctx.config().sweep_samples * 4, 20);
    const olt_perf_event_t events[] = {OLT_PERF_BRANCH_MISSES};
    olt_perf_group_t* perf = olt_perf_open(events, 1);
    if (perf) olt_perf_start(perf);

    auto add = [&](const char* name, olt::Params params, const Code& c, std::vector<double> v, double misses) {
        ctx.add_result(name, std::move(params), std::move(v));
        olt::Result& r = ctx.results().back();
        r.unit = "ns/element";
        r.source = c.source;
        if (misses >= 0) r.metrics.push_back({"branch_misses", misses});
    };
    for (const Code& c : k_codes) {
        for (int p : k_taken_percent) {
            double misses;
            std::vector<double> v = run(c, p, false, samples, perf, &misses);
            add("cpu.branch", {{"code", c.name}, {"taken", std::to_string(p)}}, c, std::move(v), misses);
        }
        double misses;
        std::vector<double> v = run(c, 50, true, samples, perf, &misses);
        add("cpu.branch_sorted", {{"code", c.name}}, c, std::move(v), misses);
    }
    if (perf) {
        olt_perf_stop(perf);
        olt_perf_close(perf);
    }
}
