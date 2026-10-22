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

## Done

- [x] Thread create/join, mutex and condvar wake-up
- [x] malloc/free by block size
- [x] Cache and memory sweep (random-load latency, sequential bandwidth vs working set)
- [x] Container allocation strategies (new/delete, reserve, pmr monotonic, pmr pool)
- [x] SPSC queues (mutex+condvar, mutex polling, spinlock, lock-free, cached indices) with CPU cost
- [x] Core-to-core atomic latency heatmap
- [x] Pre-flight checks (governor, busy CPUs, memory), HTML + Markdown report

## Standard tier: first batch

The six with the biggest x86 vs ARM differences and the clearest hints.

- [x] **False sharing vs padding distance**: two threads on adjacent vs padded
  fields at 8 to 256 B; shows whether this CPU needs 64 or 128 B padding
- [ ] **Atomic RMW and memory ordering**: fetch_add/CAS vs thread count, sharded
  counters; seq_cst vs acq_rel vs relaxed (extends the multicore section)
- [ ] **Branch misprediction and branchless crossover**: sorted vs random data,
  branchy vs cmov/mask; ns per mispredict and the predictability where
  branchless wins
- [ ] **Dynamic dispatch**: virtual, `std::function`, function pointer,
  `std::variant` + visit, CRTP; monomorphic vs megamorphic call sites
- [ ] **Syscall, vDSO and timer accuracy**: no-op syscall, `clock_gettime` per
  clock, rdtsc/cntvct; `nanosleep`/`timerfd`/epoll timeout overshoot; active
  CPU vulnerability mitigations listed in the report
- [ ] **OS jitter on the measurement core**: tight timestamp loop recording gaps
  (osnoise style); interruptions per second and worst gap

## Standard tier: second batch

- [x] **Instruction-level parallelism / loop unrolling**: 1 to 16 independent
  dependency chains of integer multiply, double add/multiply, L1 loads and DRAM
  loads; how many accumulators a loop needs and how many cache misses overlap

- [ ] **SIMD payoff**: scalar vs auto-vectorised vs intrinsics (AVX2/AVX-512,
  NEON/SVE) from L1 to DRAM, with the x86 frequency-licence caveat
- [ ] **memcpy/memset by size**: loop vs memcpy vs `rep movsb`, aligned and not,
  hot and cold; the size thresholds
- [ ] **Container choice by size**: `std::map`, `unordered_map`, sorted vector,
  `flat_map`; lookup/insert at 16 to 1M entries with cache levels marked
- [ ] **Spin-then-park knee**: hybrid wait with spin budget 0 to 100 us vs
  latency and CPU cost
- [ ] **Page faults and mmap**: first touch, minor vs major fault, `MAP_POPULATE`,
  4 KiB vs 2 MiB pages (THP)
- [ ] **Huge pages in the cache sweep**: 4 KiB vs 2 MiB curves on one chart, TLB
  reach marked
- [ ] **Context switch through the kernel**: eventfd/pipe ping-pong on the same
  vs another core
- [ ] **AoS vs SoA**, **software prefetch distance**, **unaligned and split-line
  loads**
- [ ] **Instruction cache and code size**: hot loop of growing size, L1i marked
- [ ] **Integer division**, **denormals and FTZ/DAZ**
- [ ] **MPSC/MPMC queues** and **read-mostly patterns** (`shared_mutex`, seqlock,
  RCU-style pointer swap)
- [ ] **Exceptions vs error codes** (`std::expected`), **small-buffer
  optimisation thresholds**, **bounds checks**

## Standard tier: network (kernel stack)

Measures the stock Linux network stack first. The interfaces stay generic
(a "transport" with send/receive/poll) so kernel-bypass back ends can plug in
later without touching the benchmarks or the report.

- [ ] **Loopback UDP and TCP round trip**: blocking, non-blocking poll and
  `SO_BUSY_POLL`; RTT percentiles
- [ ] **Event loop models**: blocking read vs epoll vs io_uring on eventfd,
  pipes and sockets
- [ ] **Socket options that matter**: `TCP_NODELAY`, `SO_RCVBUF`, `SO_TIMESTAMPING`
  (software vs hardware timestamps where the NIC supports them)
- [ ] **Real NIC round trip** between two hosts or two ports (optional, needs a
  peer): kernel stack baseline for the bypass comparison
- [ ] Transport abstraction so bypass back ends can be added (below)

## Expert tier

Opt-in, documented in [expert-setup.md](expert-setup.md).

- [ ] `--expert` mode / script: detects the boot parameters and kernel features in
  use and records them in results.json
- [ ] Isolated-core runs (`isolcpus`, `nohz_full`, `rcu_nocbs`, IRQ affinity):
  jitter and latency vs a stock core, side by side
- [ ] PREEMPT_RT vs generic vs lowlatency kernel comparison report (same
  machine, several results.json)
- [ ] Mitigations on vs off (`mitigations=off`): syscall and context-switch cost
- [ ] `idle=poll` / C-states disabled in BIOS vs `/dev/cpu_dma_latency`
- [ ] Reserved huge pages at boot (`hugepagesz=1G hugepages=N`)
- [ ] **Kernel bypass networking**: DPDK, NXP DPAA/DPAA2, AF_XDP, RDMA/verbs,
  Solarflare/Onload class, as transport back ends with the same report charts

## Rust

A separate integration that does not burden the C++ build or the Debian
package.

- [ ] Decide the shape: separate Rust benchmarks sharing the C HAL through FFI,
  or C++/Rust interop tests, or both
- [ ] Rust harness writing the same results.json format, so the report shows
  C++ and Rust side by side
- [ ] Paired benchmarks: SPSC ring, container lookup, `dyn` vs generics,
  `Result` vs exceptions, bounds checks
- [ ] Rust-specific idioms: `Arc` vs `Rc`, `parking_lot` vs `std` mutex,
  crossbeam vs `std::sync::mpsc`, iterator chains vs index loops, `Box<dyn>` vs
  enum dispatch
- [ ] Interop cost: calling across the C ABI in both directions

## Platforms

- [ ] ARM boards: still open (none available yet). Candidates: Raspberry Pi 5,
  NVIDIA Jetson, NXP Layerscape/i.MX (also the DPAA target), an Ampere or
  Graviton server
- [ ] Sample reports from real hardware in `docs/` (laptop first)
- [ ] Multi-report comparison view (two or more results.json on one chart)
