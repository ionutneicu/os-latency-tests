# Benchmark dependency map

What else is affected when one part changes. The map is conceptual: it names
the links and says what to re-check. It does not replace reading the code.

**How to use it.** Before changing a benchmark, a shared helper or the engine,
find the part in the tables below. Re-run every benchmark listed against it,
compare the numbers with the run before the change, and update the outputs in
section 3. When you add a shared part or a cross-reference, add it here in the
same PR. See [ARCHITECTURE](ARCHITECTURE.md) for the layers.

There are three kinds of link:

1. **Code**: a shared part, and the benchmarks that use it.
2. **Results**: numbers that are compared with, or explained by, the numbers of another benchmark.
3. **Outputs**: what must change together with a benchmark.

## 1. Code: shared parts and their users

### Engine and HAL

| Shared part | Used by | Re-check when it changes |
|---|---|---|
| Clock: `include/olt/clock.hpp`, `src/core/clock.cpp` (`now_ns`, calibration, clock-read cost) | all | Every number. Start with the one-chain multiply in `cpu.ilp`: it must stay a 3-cycle chain. |
| Timing: `include/olt/benchmark.hpp` (`run_samples`, `time_each`, `time_batch`) | all | Every number. `time_batch` and `time_each` users first: `alloc.malloc_free` today. |
| At-floor rule: `Context::add_timed_result` | results that declare `ops_per_sample` (`alloc.malloc_free` today) | Which results are marked, in the console and in `results.json`. |
| Warm-up and clock check: `Context::warm_cpus`, `Context::clock_check` | all | Short benchmarks most (`alloc.malloc_free`, `os.call`, `cpu.ilp`, `cpu.branch`, `pattern.dispatch`), and `tools/warmup-chart.py`. |
| Statistics: `src/core/stats.cpp` | all | Percentiles in the report, and the "unreliable percentile" rule. |
| Worker setup: `Context::setup_worker_thread` | `sync.mutex_wakeup`, `sync.condvar_wakeup`, `pattern.spsc` | Pinning and priority of the second thread. |
| CPU choice and idle limit: `src/main.cpp` | every benchmark with two threads or a blocking wait: `sync.*`, `thread.create`, `multicore.*`, `pattern.spsc`, `os.timer_overshoot`, `os.jitter` | Wake-up latencies and tails. |
| HAL `arch.h` (cycle counter, prefetch, cache-line size, spin hint) | `mem.cache_sweep`, `cpu.ilp`, `multicore.false_sharing`, `multicore.c2c`, `pattern.spsc`, and the users of `sync_util.hpp` | Those benchmarks on each architecture (x86-64, aarch64, generic). |
| HAL `os.h` (pinning, topology, buffers, OS clock) | the engine, `mem_util.hpp`, `topo_util.hpp`, `mem.cache_sweep`, `cpu.ilp`, `multicore.*`, `pattern.spsc` | Those benchmarks, and the platform section of the report. |
| HAL `perf.h` (perf counters) | `cpu.branch` | Branch-miss counts, with and without `OLT_WITH_PERF`. |
| HAL `stream.h` (sequential kernels) | `mem.cache_sweep` | Sequential read and write results, non-temporal stores. |

### Shared helpers in `src/benchmarks/`

| Shared part | Used by | Re-check when it changes |
|---|---|---|
| `mem_util.hpp` (largest cache, sweep limit, pointer-chase builder) | `mem.cache_sweep`, `cpu.ilp` | Cache steps in the sweep, and the memory-load chains of `cpu.ilp`. |
| `sync_util.hpp` (wait, park, share over a limit) | `sync.mutex_wakeup`, `sync.condvar_wakeup`, `multicore.atomic_contention`, `multicore.atomic_order` | Wake-up latencies (the sleeper must be truly blocked), and the contention results. |
| `topo_util.hpp` (CPU lists, L1 siblings, one CPU per core) | `multicore.atomic_contention`, `multicore.atomic_order`, `multicore.false_sharing`, `multicore.c2c` | Which CPUs each test runs on, on machines with SMT and on hybrid CPUs. |
| `alloc_util.hpp` (planned, #35): allocator adapters, size classes, off-heap harness, metadata | `alloc.malloc_free` (#35), `alloc.churn` (planned, #36), `alloc.vs_fastest` (planned, #45), `pattern.alloc` | Every allocation number. The malloc results must stay the same code in the same heap state in every test. |

### Options that reach only some benchmarks

| Option (`Config` field) | Used by |
|---|---|
| `--park-us` (`park_us`) | `sync.mutex_wakeup`, `sync.condvar_wakeup` |
| `--mem-min`, `--mem-max`, `--sweep-steps` | `mem.cache_sweep`; `--mem-max` also reaches `cpu.ilp` through `mem_util.hpp` |
| `--sweep-samples` (`sweep_samples`) | `mem.cache_sweep`, `cpu.ilp`, `cpu.branch`, `multicore.*`, `pattern.spsc` |
| `--hugepages` | `mem.cache_sweep`, `cpu.ilp` |
| `--samples`, `--warmup` | all benchmarks that time per call |

## 2. Results: numbers that lean on other numbers

| Link | What must stay consistent |
|---|---|
| `cpu.ilp` one-chain multiply, the warm-up reference loop, the clock calibration | All three read the same clock. The multiply chain is 3 cycles on current x86-64 cores. If it is not, no result of that run can be compared with another run ([REQUIREMENTS](REQUIREMENTS.md) sections 3 and 5). |
| Clock-read cost in the platform section, `os.call` for `clock_gettime`, the at-floor rule | The same call measured three ways. They must agree, and the at-floor limit follows the first. |
| Cache sizes in the platform section, `mem.cache_sweep`, `cpu.ilp` | The sweep's steps must sit at the reported cache sizes. `cpu.ilp` builds its memory-load chains with the same pointer-chase builder and size limit, so its one-chain memory load must match the sweep's latency at the largest size. |
| `multicore.c2c`, `multicore.false_sharing`, `multicore.atomic_contention`, `pattern.spsc` | All four move a cache line between cores. On the same pair of CPUs their costs must be of the same order, and a queue hand-off cannot be cheaper than the core-to-core latency. |
| `sync.mutex_wakeup`, `sync.condvar_wakeup`, `thread.create`, `os.timer_overshoot`, `os.jitter` | All depend on the idle-state limit (`--no-idle`) and on priority. Compare them only within one run. |
| `alloc.malloc_free`, `pattern.alloc` | The container results include the allocator's cost. The guide page of `pattern.alloc` must not contradict the raw costs. |
| `alloc.malloc_free`, `alloc.churn` (planned, #36), `alloc.vs_fastest` (planned, #45) | `alloc.vs_fastest` quotes the matched-pair cost from `alloc.malloc_free` and the aged-heap cost from `alloc.churn`. One implementation of each, in `alloc_util.hpp`. |
| `alloc.side_by_side` (planned, #52): blocked by `alloc.malloc_free` (#35), `alloc.churn` (planned, #36) and `alloc.vs_fastest` (planned, #45), and by the shared core in `alloc_util.hpp` (planned, #35) | It draws the results of those three side by side: the happy path from #35, the stress from #36, the reference ladder from #35 and #45. It adds no measurements of its own. It is re-evaluated when the three are merged, so the row can change then. |
| Design hints in `tools/olt-report.py` (`build_guidance`) | A hint combines results of several benchmarks and the platform data. A changed result name, parameter or unit drops the hint without an error. |

## 3. Outputs: what changes together with a benchmark

The report script finds results by **result name**, which is not always the
benchmark name. Renaming either side removes the section from the report.

| Benchmark | Source | Result names | Guide page |
|---|---|---|---|
| `mem.cache_sweep` | `memory.cpp` | `mem.latency_random`, `mem.read_seq`, `mem.write_seq`, `mem.write_random`, `mem.write_nt` | `memory.md` |
| `cpu.ilp` | `ilp.cpp` | `cpu.ilp` | `ilp.md` |
| `cpu.branch` | `branch.cpp` | `cpu.branch`, `cpu.branch_sorted` | `branch.md` |
| `multicore.c2c` | `multicore.cpp` | `multicore.c2c_latency` | `c2c.md` |
| `multicore.false_sharing` | `multicore.cpp` | `multicore.false_sharing`, `multicore.false_sharing_alone` | `false-sharing.md` |
| `multicore.atomic_contention` | `atomic.cpp` | `multicore.atomic_contention` | `atomics.md` |
| `multicore.atomic_order` | `atomic.cpp` | `multicore.atomic_order` | `atomics.md` |
| `thread.create` | `threads.cpp` | `thread.create_to_start`, `thread.exit_to_join` | `threads.md` |
| `sync.mutex_wakeup` | `sync.cpp` | `sync.mutex_wakeup` | `sync.md` |
| `sync.condvar_wakeup` | `sync.cpp` | `sync.condvar_wakeup` | `sync.md` |
| `os.call` | `syscall_linux.cpp` | `os.call` | `syscall.md` |
| `os.timer_overshoot` | `syscall_linux.cpp` | `os.timer_overshoot` | `syscall.md` |
| `os.jitter` | `jitter.cpp` | `os.jitter_gap`, `os.jitter_rate` | `jitter.md` |
| `alloc.malloc_free` | `alloc.cpp` | `alloc.malloc`, `alloc.free` | `alloc.md` |
| `pattern.alloc` | `pattern_alloc.cpp` | `pattern.alloc_vector`, `pattern.alloc_list` | `pattern-alloc.md` |
| `pattern.spsc` | `pattern_spsc.cpp` | `pattern.spsc_latency`, `pattern.spsc_throughput` | `pattern-spsc.md` |
| `pattern.dispatch` | `pattern_dispatch.cpp` | `pattern.dispatch_one_type`, `pattern.dispatch_mixed` | `pattern-dispatch.md` |

For every benchmark, these change with it:

- its section, chart and hints in `tools/olt-report.py`;
- its guide page in `docs/benchmarks/`, and its row in `docs/benchmarks/README.md`;
- terms in `docs/GLOSSARY.md`;
- `tests/fixtures/report-results.json`, when the report tests need the new results;
- `docs/sample-report/`, which is a recorded run and goes stale when results change;
- the benchmark list in `README.md`.

## 4. Keeping the map true

The `dependency_map` test (`tests/dependency_map.cmake`) fails when a
registered benchmark or a `src/benchmarks/*_util.hpp` helper is not named in
this file, or when this file names a helper that does not exist and is not
marked "planned". It checks coverage only. Whether a link is still correct is
part of reviewing the change.
