# Dispatch: virtual vs std::function vs std::variant vs templates

**Benchmark:** `pattern.dispatch` → results `pattern.dispatch_one_type`, `pattern.dispatch_mixed` per variant ·
source: [src/benchmarks/pattern_dispatch.cpp](../../src/benchmarks/pattern_dispatch.cpp)

## Goal

Decide how a hot loop calls one of several implementations: through a run-time
mechanism (vtable, `std::function`, function pointer, `switch`,
`std::variant`), or with the type fixed at compile time (templates, CRTP,
policy classes), the "zero-cost abstraction" of modern C++.

## What is measured

1024 small objects, each with its own parameter `k`, implement one of four
one-cycle operations (`x * k`, `x + k`, `x ^ k`, `(x >> 3) + k`). One sample
calls every object and sums the results; the unit is ns per call:

```cpp
// virtual: the classic object-oriented way
for (uint32_t i : order) s += objects[i]->eval(i);       // std::vector<std::unique_ptr<Base>>

// variant: closed set of types, stored inline, std::visit picks the code
for (uint32_t i : order) s += std::visit([i](const auto& op) { return op.eval(i); }, vars[i]);

// template: one loop per type, eval() is inlined, no choice left at run time
template <class Op>
uint64_t sum_group(const Group<Op>& g) {
    uint64_t s = 0;
    for (size_t j = 0; j < g.ops.size(); ++j) s += g.ops[j].eval(g.idx[j]);
    return s;
}
uint64_t sum_template(const ByType& t) {               // std::tuple<Group<Mul>, Group<Add>, ...>
    return std::apply([](const auto&... g) { return (sum_group(g) + ...); }, t);
}
```

Variants: `virtual`, `std_function` (small lambdas, no heap), `function_ptr`
(pointer + state), `switch` (enum tag), `variant` (`std::visit`), `template`.

Two call-site mixes:

* `dispatch_one_type`: every object has the same type, called in sequence. The
  branch predictor always guesses the target, so this is the best case for
  run-time dispatch.
* `dispatch_mixed`: the four types in equal shares, called in a **new random
  order every sample** (the shuffle is not timed). With a fixed order, modern
  predictors learn the whole sequence after a few thousand repetitions and the
  result would look far better than real code with data-dependent types.

Every variant computes the same sum; a mismatch marks the result
"self-check failed".

## Reading the result

* **One type**: run-time dispatch costs a few cycles over `template`: an
  indirect call that is predicted, plus the call itself (arguments, return),
  which also stops the compiler from optimising across it.
* **Mixed types**: the target is mispredicted about three times in four, and
  each miss costs a pipeline flush (about 15 to 20 cycles on current x86 and
  ARM cores). Indirect calls (`virtual`, `std::function`, function pointer)
  suffer most. A `switch` with few cases becomes a compare-and-branch chain
  with inlined code, often about half the cost. `std::visit` depends on the
  compiler: GCC 13 emits a branch chain (as fast as `switch`), Clang 18 a jump
  table, an indirect branch that mispredicts like a virtual call.
* **`template`** stays at about one cycle per call in both cases: the objects
  are grouped by type, so there is nothing left to predict.
* `std::function` adds one more indirection than a function pointer; the
  difference is usually small next to the misprediction.

## Using it in C++

Keep the type known where the loop is hot, and push the run-time choice
outside the loop:

```cpp
// Instead of std::vector<std::unique_ptr<Shape>> processed in arrival order:
struct Shapes {
    std::vector<Circle> circles;
    std::vector<Square> squares;
};
template <class F>
void for_each_shape(Shapes& s, F&& f) {
    for (auto& c : s.circles) f(c);   // f(Circle&) is inlined
    for (auto& q : s.squares) f(q);   // f(Square&) is inlined
}
```

* **Closed set of types** (you own them all): `std::variant` stores them
  inline (no heap, better locality) and lets the compiler inline each case.
* **Policies fixed at build time** (logging on/off, allocator, lock type): make
  them template parameters or `if constexpr`, not run-time flags or virtual
  interfaces.
* **CRTP** gives static polymorphism with a familiar base-class interface:

  ```cpp
  template <class Derived>
  struct Codec {
      size_t encode(const Msg& m, char* out) { return static_cast<Derived*>(this)->encode_impl(m, out); }
  };
  struct Fix : Codec<Fix> { size_t encode_impl(const Msg&, char*); };
  ```

* **Open set of types** (plugins, user extensions): keep `virtual`, and if the
  hot loop sees mixed types, sort or bucket objects by type first so that
  consecutive calls go to the same target.
* `final` on a class or member lets the compiler devirtualise calls made
  through that exact type.

Costs of the compile-time way: every instantiation is more code (watch the
instruction cache and the binary size), compile times grow, and the set of
types must be known when the code is built. Use it on the measured hot path
only.
