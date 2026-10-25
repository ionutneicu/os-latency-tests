# Roadmap

New measurements are chosen for what a C, C++ or Rust developer can act on when
optimising a hot path: each one must change a line of code, differ between
platforms, be measurable from user space and fit in one report hint.

They land one per pull request, or a few related ones together when they share
code. Tick an item when its PR is merged.

> These benchmarks justify techniques that cost maintainability and portability.
> The report says so at the top: apply them only to the measured hot path of
> ultra-low-latency or high-performance code.

Tiers:

* **Standard**: runs on a stock distribution kernel, with root where needed
  (RT priority, mlock, `/dev/cpu_dma_latency`, perf counters). Part of
  `./run-all.sh`.
* **Expert**: needs a custom kernel, kernel command line, BIOS settings or
  special hardware. Opt-in only (`--expert` / a separate script), documented
  in [expert-setup.md](expert-setup.md) with what to expect and whether it is
  worth trying.

## Categories

Every item carries one tag. The tags also order the benchmark guide and the report.

* **[HW] Hardware**: what the CPU, caches, predictor and cores do to the same
  code: cache levels, ILP, branches, atomics, core-to-core and false sharing.
* **[OS] OS latency**: what the kernel and scheduler add when a thread waits,
  wakes, sleeps, spawns or is interrupted: wake-ups, thread creation, syscalls,
  timers, jitter, idle states, the network stack.
* **[SW] Software patterns**: the C++ choices made in your own code: containers,
  allocators, queues, dispatch, compile-time techniques, build flags and
  language features.

Items with no tag are infrastructure or platform work, not measurements.

## Done

- [x] [OS] Thread create/join, mutex and condvar wake-up
- [x] [SW] malloc/free by block size
- [x] [HW] Cache and memory sweep (random-load latency, sequential bandwidth vs working set)
- [x] [HW] Write cost per level: vector sequential writes, random-line stores (read for
      ownership, overlapped misses), non-temporal streaming stores
- [x] [SW] Container allocation strategies (new/delete, reserve, pmr monotonic, pmr pool)
- [x] [SW] SPSC queues (mutex+condvar, mutex polling, spinlock, lock-free, cached indices) with CPU cost
- [x] [HW] Core-to-core atomic latency heatmap
- [x] Pre-flight checks (governor, busy CPUs, memory), HTML + Markdown report

## Standard tier: first batch

The six with the biggest x86 vs ARM differences and the clearest hints.

- [x] [HW] **False sharing vs padding distance**: two threads on adjacent vs padded
  fields at 8 to 256 B; shows whether this CPU needs 64 or 128 B padding
- [x] [HW] **Atomic RMW and memory ordering**: fetch_add/CAS vs thread count, sharded
  counters; seq_cst vs acq_rel vs relaxed (`multicore.atomic_contention`,
  `multicore.atomic_order`)
- [x] [HW] **Branch misprediction and branchless crossover**: sorted vs random data,
  branchy vs cmov/mask; ns per mispredict and the predictability where
  branchless wins (`cpu.branch`)
- [x] [SW] **Dynamic dispatch**: virtual, `std::function`, function pointer,
  `switch`, `std::variant` + visit vs compile-time templates (CRTP);
  one type vs mixed types at the call site (`pattern.dispatch`)
- [x] [OS] **Syscall, vDSO and timer accuracy**: no-op syscall, `clock_gettime` per
  clock, rdtsc/cntvct; `nanosleep`/`timerfd`/epoll timeout overshoot; active
  CPU vulnerability mitigations listed in the report (`os.call`,
  `os.timer_overshoot`)
- [x] [OS] **OS jitter on the measurement core**: tight timestamp loop recording gaps
  (osnoise style); interruptions per second and worst gap (`os.jitter`)
- [ ] [HW] **SMT siblings: logical vs physical cores**: the report's platform block
  shows the topology (which logical CPUs share a physical core and its L1/L2)
  and warns when two benchmark threads land on siblings. A new benchmark runs
  a latency-sensitive loop (L1-resident pointer chase, ALU chain, SPSC
  round trip) alone, with a busy thread on its SMT sibling (ALU-heavy, then
  cache-thrashing), and with the same thread on another physical core; plus
  two communicating threads on siblings vs separate cores (siblings are
  fastest for hand-offs, since they share L1). Hint: how much a busy sibling
  costs on this CPU, and whether to keep one hot thread per physical core

## Standard tier: second batch

- [x] [HW] **Instruction-level parallelism / loop unrolling**: 1 to 16 independent
  dependency chains of integer multiply, double add/multiply, L1 loads and DRAM
  loads; how many accumulators a loop needs and how many cache misses overlap

- [ ] [HW] **SIMD payoff**: scalar vs auto-vectorised vs intrinsics (AVX2/AVX-512,
  NEON/SVE) from L1 to DRAM, with the x86 frequency-licence caveat
- [ ] [HW] **memcpy/memset by size**: loop vs memcpy vs `rep movsb`, aligned and not,
  hot and cold; the size thresholds
- [ ] [SW] **Container choice by size**: `std::map`, `unordered_map`, sorted vector,
  `flat_map`; lookup/insert at 16 to 1M entries with cache levels marked
- [ ] [OS] **Spin-then-park knee**: hybrid wait with spin budget 0 to 100 us vs
  latency and CPU cost
- [ ] [OS] **Page faults and mmap**: first touch, minor vs major fault, `MAP_POPULATE`,
  4 KiB vs 2 MiB pages (THP)
- [ ] [HW] **Huge pages in the cache sweep**: 4 KiB vs 2 MiB curves on one chart, TLB
  reach marked
- [ ] [OS] **Context switch through the kernel**: eventfd/pipe ping-pong on the same
  vs another core
- [ ] [HW] **AoS vs SoA**, **software prefetch distance**, **unaligned and split-line
  loads**
- [ ] [HW] **Instruction cache (L1i), uop cache and code size**: generated straight-line
  code blocks (a JIT-free template: N copies of a cheap independent op, or a
  chain of N small functions) executed in a loop, footprint swept from 1 KiB to
  a few MiB with L1i, uop cache (DSB / MOP cache), L2 and iTLB reach marked.
  Sub-tests:
  - code footprint sweep: ns per instruction as the hot code outgrows each level
  - taken-branch density: same footprint, jumps every 16/32/64 B (fetch
    bandwidth, BTB capacity)
  - call chain depth and indirect calls (virtual / function pointer) spread
    over many targets vs one
  - hot/cold splitting: the same loop with an error path inlined vs moved out
    (`[[unlikely]]`, `__attribute__((cold))`), and `-O2` vs `-O3`/`-Os` code size
  - optional (OLT_WITH_PERF): L1i misses, iTLB misses, frontend-bound cycles
  Report: "keep the hot path under N KiB of code" and when inlining or
  unrolling starts to cost more than it saves.
- [ ] [HW] **Integer division**, **denormals and FTZ/DAZ**
- [ ] [SW] **MPSC/MPMC queues** and **read-mostly patterns** (`shared_mutex`, seqlock,
  RCU-style pointer swap)

## Standard tier: compiler and language choices

How build flags and C++ constructs change the speed of the same hot code. The
harness stays C++17; these kernels build with the newest `-std` the compiler
supports (C++20/23 for `[[likely]]`, `[[assume]]`, `std::expected`,
`std::unreachable`) and report n/a where a feature is missing. Every item
reports speed and code size next to the plain `-O2` version, for GCC and Clang.

- [ ] [SW] **Build-variant harness** (needed by the items below): the same kernel
  sources compiled several times with different flags into one binary (one
  namespace per variant, set up by CMake), so one run compares them side by
  side. Whole-program options (LTO, PGO, static linking, `-fno-exceptions`
  everywhere) need separate builds: those use the multi-report comparison view
  (see Platforms). Kernels, small and representative: hash-map lookup, sort,
  dot product and small matrix multiply, byte parser (state machine), virtual
  dispatch loop, SPSC ring push/pop
- [ ] [SW] **Optimisation level and target CPU**: `-O0` to `-O3`, `-Os`/`-Oz`;
  `-march` baseline vs x86-64-v2/v3/v4 and `native` (ARM: `armv8-a` vs
  `armv8.2-a` and later, `-mcpu=native`); GCC vs Clang on the same kernels.
  Hint: which kernels gain from `-O3` or a newer `-march`, at what code-size
  cost, and the portability catch (a `-march=native` binary dies with SIGILL on
  older CPUs; runtime dispatch or `target_clones` instead)
- [ ] [SW] **LTO and PGO**: `-flto` (full and thin) and profile-guided optimisation
  (two-pass `-fprofile-generate` / `-fprofile-use`): cross-file inlining,
  devirtualisation, hot/cold code layout. BOLT and AutoFDO are in the expert
  tier
- [ ] [SW] **Floating-point flags**: `-ffast-math` and its parts (`-fassociative-math`,
  `-fno-math-errno`, `-ffinite-math-only`, `-ffp-contract=fast` for FMA): the
  speed-up next to the change in the result (in ULPs), so the trade is visible.
  Ties in with the ILP unrolling hint
- [ ] [SW] **Hardening and distribution defaults**: `_FORTIFY_SOURCE=3`,
  `_GLIBCXX_ASSERTIONS` (bounds-checked `operator[]`) and the libc++ hardening
  modes, `at()` vs `operator[]`, `-fstack-protector-strong`,
  `-fstack-clash-protection`, `-fcf-protection` (Intel CET),
  `-mbranch-protection=standard` (ARM PAC/BTI), `-ftrivial-auto-var-init=zero`,
  frame pointers (on by default in Ubuntu 24.04 and Fedora 38 packages). What
  each costs on the hot kernels: distribution packages, ours included, are built
  with most of them
- [ ] [SW] **Linking and calls**: static vs dynamic, PIE vs non-PIE, PLT vs
  `-fno-plt`, `-fvisibility=hidden` and `-fno-semantic-interposition`; a call
  into a shared library vs the same call inside the executable; `thread_local`
  access from an executable vs a shared library (TLS models, `__tls_get_addr`);
  function-local `static` guard checks and `-fno-threadsafe-statics`
- [ ] [SW] **Exceptions and RTTI**:
  - happy path: the same loop built with exceptions and with `-fno-exceptions`,
    callees `noexcept` or not (speed and code size)
  - throw cost: throw-to-catch latency vs unwind depth (1 to 100 frames) and vs
    threads throwing at once (unwinder scalability, glibc 2.35+
    `_dl_find_object` vs older)
  - error handling at error rates of 0, 0.1, 1 and 10%: exceptions, error
    codes, `std::optional`, `std::expected` (C++23, or `tl::expected` on C++17)
  - `new` vs `new (std::nothrow)`; `noexcept` move constructors in
    `std::vector` growth (without `noexcept` the vector copies instead of
    moving)
  - `-fno-rtti`: `dynamic_cast` and `typeid` vs a stored type tag
  Hint: when exceptions are free, when they are not, and the error rate where
  `std::expected` wins
- [ ] [SW] **Optimiser hints**: `[[likely]]`/`[[unlikely]]` and `__builtin_expect`
  (code layout, not prediction); `[[assume(expr)]]` (C++23; `__builtin_assume`
  in Clang, the `if (!x) __builtin_unreachable()` idiom in older GCC) and
  `std::unreachable()`; `std::assume_aligned` / `__builtin_assume_aligned`;
  `__restrict`; `[[gnu::always_inline]]`, `noinline`, `flatten`, `hot`, `cold`;
  `final`. Each on a kernel where it should help (an assumed multiple of 8 drops
  the remainder loop, `__restrict` lets a loop vectorise, `final` lets a virtual
  call inline), with ns and instruction count next to the plain version and a
  note when the compiler already does it alone. A false `[[assume]]` is
  undefined behaviour: the guide says so
- [ ] [SW] **Zero-cost abstractions, checked**: small types by value vs `const&`,
  `std::string_view` and `std::span` by value, types with a non-trivial
  destructor such as `std::unique_ptr` (passed through memory on the Itanium
  ABI); type punning with `std::bit_cast` / `memcpy` vs `reinterpret_cast`;
  signed vs unsigned loop indices (`constexpr` tables: see compile-time C++)
- [ ] [SW] **Standard library on the hot path**: number formatting and parsing
  (`std::to_chars` / `from_chars` vs `snprintf` / `strtod` vs iostreams vs
  `std::format`); `std::string` and `std::function` small-buffer thresholds

## Standard tier: compile-time C++ (constexpr and metaprogramming)

Modern C++ moves more work to compile time (`constexpr`, `consteval`,
`if constexpr`, fold expressions, concepts). Each item sets a compile-time
version against the classic run-time one on the same task, and reports speed,
code size and compile time, so the "zero cost" claim is checked rather than
assumed. Like the compiler items, these kernels may build with C++20/23.

- [ ] [SW] **Lookup tables**: a `constexpr` table (CRC32, sine, bit reversal)
  vs the same table filled at start-up vs computing the value each time; table
  size against the cache levels from `mem.cache_sweep`
- [ ] [SW] **Policies: compile-time vs run-time switches**: logging, statistics or
  bounds checks as a template parameter or `if constexpr` vs a run-time `bool`
  vs a virtual policy object; what an "off" feature costs in each case
- [ ] [SW] **Fixed-size loops**: `std::index_sequence` and fold-expression unrolling
  vs a plain loop the compiler may unroll itself, for N = 2 to 64 (small
  vectors, matrices, checksums); where explicit unrolling stops helping
- [ ] [SW] **Expression templates**: `a = b + c * d` on vectors with naive operator
  overloading (temporaries) vs expression templates (one fused loop) vs the
  hand-written loop
- [ ] [SW] **State machines**: a byte parser as `switch` on an enum, as
  `std::variant` states with `std::visit`, as a table of function pointers, and
  as a virtual State pattern; on predictable and random input
- [ ] [SW] **Data layout by template**: array of structs vs struct of arrays
  generated by a template (`soa_vector<T>`), for loops that touch one field
  and loops that touch all fields
- [ ] [SW] **Code size vs speed**: N instantiations of the same template vs one
  type-erased function; where template bloat starts to miss the instruction
  cache (ties in with the front-end item)

## Standard tier: network (kernel stack)

Measures the stock Linux network stack first. The interfaces stay generic
(a "transport" with send/receive/poll) so kernel-bypass back ends can plug in
later without touching the benchmarks or the report.

- [ ] [OS] **Loopback UDP and TCP round trip**: blocking, non-blocking poll and
  `SO_BUSY_POLL`; RTT percentiles
- [ ] [OS] **Event loop models**: blocking read vs epoll vs io_uring on eventfd,
  pipes and sockets
- [ ] [OS] **Socket options that matter**: `TCP_NODELAY`, `SO_RCVBUF`, `SO_TIMESTAMPING`
  (software vs hardware timestamps where the NIC supports them)
- [ ] [OS] **Real NIC round trip** between two hosts or two ports (optional, needs a
  peer): kernel stack baseline for the bypass comparison
- [ ] [OS] Transport abstraction so bypass back ends can be added (below)

## Expert tier

Opt-in, documented in [expert-setup.md](expert-setup.md).

- [ ] [OS] `--expert` mode / script: detects the boot parameters and kernel features in
  use and records them in results.json
- [ ] [OS] Isolated-core runs (`isolcpus`, `nohz_full`, `rcu_nocbs`, IRQ affinity):
  jitter and latency vs a stock core, side by side
- [ ] [OS] PREEMPT_RT vs generic vs lowlatency kernel comparison report (same
  machine, several results.json)
- [ ] [OS] Mitigations on vs off (`mitigations=off`): syscall and context-switch cost
- [ ] [OS] `idle=poll` / C-states disabled in BIOS vs `/dev/cpu_dma_latency`
- [ ] [OS] SMT on vs off (`/sys/devices/system/cpu/smt/control`, `nosmt`, BIOS):
  the full suite in both states, compared side by side, against "SMT on, both
  siblings isolated, one thread pinned"
- [ ] [OS] Reserved huge pages at boot (`hugepagesz=1G hugepages=N`)
- [ ] Post-link and sampling-based optimisation (BOLT, AutoFDO): needs branch
  records (Intel LBR, AMD BRS, ARM BRBE or SPE), `perf record -b` and the LLVM
  tools; compared with the PGO build from the standard tier
- [ ] [OS] **Kernel bypass networking**: DPDK, NXP DPAA/DPAA2, AF_XDP, RDMA/verbs,
  Solarflare/Onload class, as transport back ends with the same report charts

## Rust

A separate integration that does not burden the C++ build or the Debian
package.

- [ ] [SW] Decide the shape: separate Rust benchmarks sharing the C HAL through FFI,
  or C++/Rust interop tests, or both
- [ ] [SW] Rust harness writing the same results.json format, so the report shows
  C++ and Rust side by side
- [ ] [SW] Paired benchmarks: SPSC ring, container lookup, `dyn` vs generics,
  `Result` vs exceptions, bounds checks
- [ ] [SW] Rust-specific idioms: `Arc` vs `Rc`, `parking_lot` vs `std` mutex,
  crossbeam vs `std::sync::mpsc`, iterator chains vs index loops, `Box<dyn>` vs
  enum dispatch
- [ ] [SW] Interop cost: calling across the C ABI in both directions

## Platforms

- [ ] ARM boards: still open (none available yet). Candidates: Raspberry Pi 5,
  NVIDIA Jetson, NXP Layerscape/i.MX (also the DPAA target), an Ampere or
  Graviton server
- [ ] Sample reports from real hardware in `docs/` (laptop first)
- [ ] Multi-report comparison view (two or more results.json on one chart)
