# Requirements

What every benchmark in this suite must satisfy before its numbers are shown
or used for design advice. A wrong number is worse than no number, so a
benchmark that cannot meet these is left out of the report, or shown as
"not validated" with the reason.

Topics are grouped as in the [roadmap](ROADMAP.md): hardware (HW), OS latency
(OS) and software patterns (SW).

## 1. Measure through C++ first

- A benchmark times what a C++ or POSIX/libc call does: `std::atomic`,
  `std::thread`, `std::mutex`, `clock_gettime`, `nanosleep`, `mmap` and so on.
  The reader can reproduce it from the source without knowing the architecture.
- Assembly is allowed only where C++ or the C library cannot express the thing
  being measured, and only behind the HAL (`include/olt/hal/`):
  - cycle counters (`rdtsc`, `cntvct_el0`);
  - prefetch instructions;
  - barriers and cache maintenance beyond `std::atomic_thread_fence`;
  - reading a hardware detail such as an instruction's real latency, when the
    compiler would otherwise fold the loop away.
- Assembly used to probe hardware must be checked against a C++ version of the
  same measurement (where one exists) and the report must say which
  instructions were hand-written.
- Compiler barriers should use standard C++ where it works
  (`std::atomic_signal_fence`, `volatile` sinks). `__asm__ ("" ::: "memory")`
  is a GNU extension. Existing uses are candidates to replace (see
  `src/benchmarks/branch.cpp` and `src/hal/stream.c`).

## 2. Every result says what it measured

- The benchmark's question, the timed loop and the units are in its guide page.
- Results are full distributions (p50, p99, max), never only an average.
- Benchmarks that run few samples say so (existing "unreliable" rule).

## 3. Plausible numbers only

- Each benchmark declares a plausibility range, expressed in cycles or ns for a
  CPU class (not for one machine). A result outside its range is marked
  "suspect" in the report, and its design hint is withheld.
- Known instruction costs are the first check: integer multiply is a 3-cycle
  chain, an L1 hit is about 5 cycles, a system call is tens of nanoseconds.
  Values that contradict them mean the build or the clock is wrong.

## 4. Self-checks inside each benchmark

- The timed computation must produce the expected result. `pattern.dispatch`
  already checks its sum; every new kernel does the same.
- Orderings that must hold on any machine are checked: for example a shared
  atomic is slower than sharded counters at 4 threads, and branch misses rise
  with the share of unpredictable elements.
- A failed check is reported, not hidden.

## 5. Calibrate every run

- Measure the effective clock at the start and end of each run with a
  dependent multiply chain. Report cycles where that helps.
- Flag runs where the clock drifted by more than about 5%, or sits far below
  the maximum turbo, which happens on laptops on battery or in the balanced
  power profile.

## 6. Know what the run was allowed to do

- Results taken without root (no real-time priority, deep idle allowed, no
  perf counters) are marked "unprivileged: not representative" in the sections
  whose tails depend on scheduling, timers, idle states and jitter.
- Runs with busy benchmark CPUs, or with swap close to full, are flagged.
- The unoptimised-build guard (#12) stays. Numbers from an unoptimised build
  are not shown at all.

## 7. Repeat and compare

- Repeat each run and report the spread between repeats. A median that moves
  by more than a set percentage between repeats is flagged.
- The same benchmark is compared across commits only when its check results
  match.

## 8. Platform evidence

- Each platform has a sign-off row in the docs: commit, date, who ran it, which
  benchmarks matched expectations, and which were not validated there.
- A benchmark with no sign-off on a platform shows "not validated on this
  platform" in that platform's report.
- ARM stays unvalidated until a real board runs it. QEMU and shared CI VMs
  check that the code runs, not that the timings are right.

## 9. Keep design advice honest

- A design hint appears only when the benchmarks behind it pass sections 3 to 5.
- Every hint keeps the "hot path only" warning: the techniques cost
  maintainability and portability.

## Open questions

- Which self-checks are enough for HW items that have no exact C++ equivalent
  (for example prefetch distance)?
- Should the report refuse to show an unprivileged run's tail numbers, or show
  them with the flag?
