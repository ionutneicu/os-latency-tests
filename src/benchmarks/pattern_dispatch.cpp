// Design pattern: calling one of several implementations, chosen at run time
// (dynamic dispatch) or at compile time (templates).
//
// 1024 small objects, each with its own parameter k, implement one of four
// cheap operations (x * k, x + k, x ^ k, (x >> 3) + k). One sample calls
// every object once per pass and sums the results:
//
//   for (i : order) sum += objects[i].eval(i);
//
// The same objects are stored and called six ways:
//
//   virtual        std::vector<std::unique_ptr<Base>>, virtual eval() (vtable)
//   std_function   std::vector<std::function<uint64_t(uint64_t)>> of small lambdas
//   function_ptr   std::vector<{function pointer, k}>: the C way
//   switch         std::vector<{enum kind, k}> and a switch
//   variant        std::vector<std::variant<Mul, Add, Xor, Shift>> + std::visit
//   template       compile-time dispatch, no choice left at run time: a function
//                  template instantiated per type (what CRTP and policy classes
//                  compile to). With mixed types the objects are kept in one
//                  std::vector per type (a std::tuple of vectors), so every loop
//                  knows its type: the zero-cost-abstraction way
//
// Two call-site mixes:
//
//   pattern.dispatch_one_type  every object has the same type: the branch
//                              predictor always guesses the target right
//   pattern.dispatch_mixed     the four types in random order, called in a new
//                              random order every sample: the target is
//                              unpredictable, each miss flushes the pipeline
//
// Unit: ns per call, including the operation (about one cycle) and the loads.
// Every variant must produce the same sum; a mismatch is reported in the note.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "olt/benchmark.hpp"

namespace {

constexpr size_t k_objects = 1024;
constexpr size_t k_passes = 4;  // calls per sample = k_objects * k_passes

// ---- the four operations as plain types (variant, template)
struct Mul {
    static constexpr size_t kind = 0;
    uint64_t k;
    uint64_t eval(uint64_t x) const { return x * k; }
};
struct Add {
    static constexpr size_t kind = 1;
    uint64_t k;
    uint64_t eval(uint64_t x) const { return x + k; }
};
struct Xor {
    static constexpr size_t kind = 2;
    uint64_t k;
    uint64_t eval(uint64_t x) const { return x ^ k; }
};
struct Shift {
    static constexpr size_t kind = 3;
    uint64_t k;
    uint64_t eval(uint64_t x) const { return (x >> 3) + k; }
};

// ---- behind a vtable
struct Base {
    virtual ~Base() = default;
    virtual uint64_t eval(uint64_t x) const = 0;
};
template <class Op>
struct Virtual final : Base {
    explicit Virtual(Op o) : op(o) {}
    uint64_t eval(uint64_t x) const override { return op.eval(x); }
    Op op;
};

// ---- function pointer + state; enum tag + switch
struct FnPtr {
    uint64_t (*fn)(uint64_t k, uint64_t x);
    uint64_t k;
};
template <class Op>
uint64_t call_op(uint64_t k, uint64_t x)
{
    return Op{k}.eval(x);
}

struct Tagged {
    uint8_t kind;
    uint64_t k;
    uint64_t eval(uint64_t x) const
    {
        switch (kind) {
        case Mul::kind: return x * k;
        case Add::kind: return x + k;
        case Xor::kind: return x ^ k;
        default: return (x >> 3) + k;
        }
    }
};

using Var = std::variant<Mul, Add, Xor, Shift>;
template <class Op>
struct Group {           // template: objects of one type, with their call index
    std::vector<Op> ops; // so the sum matches the other variants
    std::vector<uint64_t> idx;
};
using ByType = std::tuple<Group<Mul>, Group<Add>, Group<Xor>, Group<Shift>>;

// One set of objects, stored every way.
struct Objects {
    std::vector<std::unique_ptr<Base>> virt;
    std::vector<std::function<uint64_t(uint64_t)>> func;
    std::vector<FnPtr> fptr;
    std::vector<Tagged> tagged;
    std::vector<Var> var;
    ByType by_type;

    template <class Op>
    void add(uint64_t k)
    {
        const Op op{k};
        const uint64_t i = fptr.size();
        virt.push_back(std::make_unique<Virtual<Op>>(op));
        func.emplace_back([op](uint64_t x) { return op.eval(x); });
        fptr.push_back({&call_op<Op>, k});
        tagged.push_back({uint8_t(Op::kind), k});
        var.emplace_back(op);
        std::get<Op::kind>(by_type).ops.push_back(op);
        std::get<Op::kind>(by_type).idx.push_back(i);
    }
};

// ---- the timed loops, one per variant
// order[] is the call order: in sequence for one type, a fresh random
// permutation per sample for mixed types, so the branch predictor cannot learn
// the sequence of targets (it would, for a fixed order repeated 10000 times).
uint64_t sum_virtual(const Objects& o, const uint32_t* order)
{
    uint64_t s = 0;
    for (size_t j = 0; j < o.virt.size(); ++j) {
        const uint32_t i = order[j];
        s += o.virt[i]->eval(i);
    }
    return s;
}
uint64_t sum_function(const Objects& o, const uint32_t* order)
{
    uint64_t s = 0;
    for (size_t j = 0; j < o.func.size(); ++j) {
        const uint32_t i = order[j];
        s += o.func[i](i);
    }
    return s;
}
uint64_t sum_fptr(const Objects& o, const uint32_t* order)
{
    uint64_t s = 0;
    for (size_t j = 0; j < o.fptr.size(); ++j) {
        const uint32_t i = order[j];
        s += o.fptr[i].fn(o.fptr[i].k, i);
    }
    return s;
}
uint64_t sum_switch(const Objects& o, const uint32_t* order)
{
    uint64_t s = 0;
    for (size_t j = 0; j < o.tagged.size(); ++j) {
        const uint32_t i = order[j];
        s += o.tagged[i].eval(i);
    }
    return s;
}
uint64_t sum_variant(const Objects& o, const uint32_t* order)
{
    uint64_t s = 0;
    for (size_t j = 0; j < o.var.size(); ++j) {
        const uint32_t i = order[j];
        s += std::visit([i](const auto& op) { return op.eval(i); }, o.var[i]);
    }
    return s;
}
template <class Op>
uint64_t sum_group(const Group<Op>& g)  // the type is known here: eval() inlines, the loop can vectorise
{
    uint64_t s = 0;
    for (size_t j = 0; j < g.ops.size(); ++j) s += g.ops[j].eval(g.idx[j]);
    return s;
}
uint64_t sum_template(const Objects& o, const uint32_t*)  // grouped by type: the order does not matter
{
    return std::apply([](const auto&... g) { return (sum_group(g) + ...); }, o.by_type);
}

template <class Sum>
double per_call(const Objects& o, const uint32_t* order, Sum sum, uint64_t& check)
{
    uint64_t s = 0;
    const uint64_t t0 = olt::now_ns();
    for (size_t p = 0; p < k_passes; ++p) {
        s += sum(o, order);
        olt::do_not_optimize(s);  // keeps the passes from being merged
    }
    const uint64_t t1 = olt::now_ns();
    check = s;
    return double(t1 - t0) / double(k_objects * k_passes);
}

void measure(olt::Context& ctx, const char* name, const Objects& o, bool shuffle)
{
    std::vector<uint32_t> order(k_objects);
    for (size_t i = 0; i < k_objects; ++i) order[i] = uint32_t(i);
    std::mt19937 rng(7);
    uint64_t want = 0;
    for (size_t p = 0; p < k_passes; ++p) want += sum_switch(o, order.data());
    auto one = [&](const char* impl, auto sum) {
        uint64_t got = want;
        std::vector<double> v = ctx.run_samples([&] {
            if (shuffle) std::shuffle(order.begin(), order.end(), rng);  // not timed
            uint64_t c;
            const double ns = per_call(o, order.data(), sum, c);
            if (c != want) got = c;
            return ns;
        });
        ctx.add_result(name, {{"impl", impl}}, std::move(v), got == want ? "" : "self-check failed: wrong sum");
        ctx.results().back().unit = "ns/call";
    };
    one("virtual", sum_virtual);
    one("std_function", sum_function);
    one("function_ptr", sum_fptr);
    one("switch", sum_switch);
    one("variant", sum_variant);
    one("template", sum_template);
}

}  // namespace

OLT_BENCHMARK("pattern.dispatch",
              "Run-time vs compile-time dispatch: ns per call through virtual, std::function, function pointer, "
              "switch, std::variant and a template, with one type at the call site (pattern.dispatch_one_type) "
              "and four types in random order (pattern.dispatch_mixed)")
{
    volatile uint64_t v_base = 0x9E3779B97F4A7C15ull;  // the compiler must not know the parameters
    const uint64_t base = v_base;

    Objects one;
    for (size_t i = 0; i < k_objects; ++i) one.add<Mul>(base + i);
    measure(ctx, "pattern.dispatch_one_type", one, false);

    std::mt19937_64 rng(42);
    std::vector<size_t> kinds(k_objects);
    for (size_t i = 0; i < k_objects; ++i) kinds[i] = i % 4;  // equal shares, random order
    std::shuffle(kinds.begin(), kinds.end(), rng);
    Objects mixed;
    for (size_t i = 0; i < k_objects; ++i) {
        const uint64_t k = base + i;
        switch (kinds[i]) {
        case Mul::kind: mixed.add<Mul>(k); break;
        case Add::kind: mixed.add<Add>(k); break;
        case Xor::kind: mixed.add<Xor>(k); break;
        default: mixed.add<Shift>(k); break;
        }
    }
    measure(ctx, "pattern.dispatch_mixed", mixed, true);
}
