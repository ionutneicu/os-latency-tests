# os-latency-tests

Measures operating system and hardware latencies from user space and reports
them as full distributions (p50, p90, p99, p99.9, p99.99, max), together with a
description of the platform they were measured on.

The goal is to compare kernels (generic, low-latency, `PREEMPT_RT`) and boards,
and to back low-latency / HPC design decisions with data for a given platform:
reuse threads or spawn them, block or spin, allocate per operation or keep
buffers, size working sets to fit a cache level, and so on.

Supported today: **Linux on x86-64 and AArch64**. Everything platform specific
sits behind a small hardware abstraction layer (see [Architecture](#architecture)),
so other architectures and operating systems can be added without touching the
benchmarks.

## Quick start

```sh
sudo apt install build-essential cmake        # see "Build dependencies"
cmake -S . -B build
cmake --build build -j
./build/os-latency-tests                       # all benchmarks, about a minute
python3 tools/olt-report.py olt-results/results.json   # -> olt-results/report.html
```

Open `olt-results/report.html` in a browser for the graphs.

For trustworthy tail latencies run with real-time scheduling and locked memory:

```sh
sudo ./build/os-latency-tests --rt-prio 80 --mlock --samples 100000
```

On laptops and boards with power management, blocking wake-ups are often
dominated by the sleeping core leaving a deep idle state and by the frequency
governor. Run once as is and once with `--no-idle` (and the `performance`
governor) to see both sides.

Example console output (x86-64 VM, trimmed):

```
benchmark                                         n       min       p50       p90       p99     p99.9    p99.99       max
-------------------------------------------------------------------------------------------------------------------------
sync.mutex_wakeup                              2000   9.32 us   53.7 us    141 us    462 us  ~3.97 ms  ~6.10 ms   6.17 ms
thread.create_to_start                         2000   4.56 us   7.64 us   11.4 us   66.5 us   ~396 us   ~466 us    476 us
```

A `~` marks a percentile with fewer than ~10 samples beyond it. Such values are
not statistically reliable; increase `--samples` (p99.9 needs 10 000 samples,
p99.99 needs 100 000).

## Command line

| Option | Description |
|---|---|
| `-l, --list` | list benchmarks and exit |
| `-t, --test PATTERN` | run benchmarks matching a glob (`sync.*`) or substring; repeatable, default all |
| `-n, --samples N` | measured samples per benchmark (default 10000) |
| `-w, --warmup N` | discarded warm-up iterations (default 100) |
| `-a, --affinity MODE` | `none`, `same` (both threads on one CPU) or `different` (two CPUs, default) |
| `--cpus MAIN[,WORKER]` | pin to explicit CPUs instead of the automatic choice (highest-numbered allowed CPUs, away from CPU 0's interrupt load) |
| `-p, --rt-prio N` | run benchmark threads as `SCHED_FIFO` priority N (root or `CAP_SYS_NICE`) |
| `-m, --mlock` | `mlockall()` to avoid page-fault latency |
| `--no-idle` | keep CPUs out of deep idle (C-)states for the run via `/dev/cpu_dma_latency` (root); compare with and without to see how much wake-up latency comes from idle exit |
| `--park-us N` | how long a signalling thread waits so its peer is really blocked in the kernel (default 200) |
| `--mem-min BYTES`, `--mem-max BYTES` | buffer size range of the memory sweep (default 1K to 4x the largest cache, max 512M and RAM/4; `K`/`M`/`G` suffixes) |
| `--sweep-steps N` | buffer sizes per doubling in the memory sweep (default 4) |
| `--sweep-samples N` | samples per buffer size in the memory sweep (default 15) |
| `--hugepages` | back the sweep buffer with transparent huge pages, removing most TLB misses |
| `-o, --out DIR` | output directory (default `olt-results`) |
| `--no-raw` | skip per-sample CSV files |

## Output

* **Console**: platform summary and one table row per result.
* **`results.json`** (`schema: olt-results/1`): platform (CPU, board, kernel,
  preemption model, governor, isolated CPUs, cache hierarchy, cycle counter
  frequency, timer overhead, available perf counters), run configuration, and
  summary statistics per result: count, min, max, mean, standard deviation,
  median absolute deviation, percentiles, and which percentiles are unreliable.
* **`raw/*.csv`**: every individual sample, for histograms, CDFs and your own
  analysis.

Percentiles use linear interpolation between closest ranks (Hyndman & Fan
type 7, the NumPy/R default).

## HTML report

`tools/olt-report.py` (Python 3 standard library only, so it also runs on the
board) turns a `results.json` into one self-contained HTML file:

* **random load latency vs working-set size** (log scale) and **sequential
  read/write bandwidth vs size**, with the cache sizes reported by the OS drawn
  as bands, so every drop in performance can be matched to the L1/L2/LLC
  capacity it comes from; hover shows each measured point;
* a **per-level summary**: typical latency and bandwidth while the data fits
  each level;
* **design hints** derived from the measurements (e.g. the cost ratio of a
  DRAM miss to an L1 hit and the working-set sizes to aim for);
* a percentile table for every other benchmark.

```sh
python3 tools/olt-report.py olt-results/results.json -o report.html
```

Installed packages provide it as `olt-report`.

## Benchmarks

| Name | What is measured |
|---|---|
| `thread.create_to_start` | `std::thread` construction until the first instruction of the new thread |
| `thread.exit_to_join` | last instruction of a thread until `join()` returns |
| `sync.mutex_wakeup` | worker `unlock()` until the blocked main thread's `lock()` returns |
| `sync.condvar_wakeup` | worker sets the predicate (then unlocks and `notify_one()`) until `wait()` returns |
| `mem.latency_random` | dependent loads along a random cycle of cache lines (pointer chase) in a buffer of each size: load-to-use latency of the level the working set fits in |
| `mem.read_seq`, `mem.write_seq` | sequential read / write of the same buffer, cost per cache line (GB/s = line size / value) |
| `alloc.malloc`, `alloc.free` | `malloc()`/`free()` of one block, 16 B to 16 MiB, allocator best case (block reused, memory untouched) |

The memory sweep allocates and pre-faults one buffer up front and reuses
prefixes of it for every size, so page faults never show up in the curves.
By default it uses base pages, so latency past the TLB reach includes page
walks; compare with a `--hugepages` run to separate TLB from cache effects.

Wake-up benchmarks use a persistent worker thread, so thread creation does not
leak into them, and timestamps come from the monotonic clock shared by all CPUs.
Measurements below ~100 ns include the clock read overhead reported in the
platform section.

Planned (see the project plan): thread pool vs spawn vs single thread,
spin/atomic hand-offs, stride and prefetch-distance sweeps, stack copy vs
persistent heap buffers, false sharing, pipes/eventfd/loopback sockets, timer
and sleep jitter, perf counters per benchmark, and more design guidance.

## Build dependencies

Required:

| Dependency | Version | Debian/Ubuntu package |
|---|---|---|
| C11 and C++17 compiler (GCC or Clang) | GCC >= 8, Clang >= 7 | `g++` or `clang` |
| CMake | >= 3.16 | `cmake` |
| POSIX threads | glibc or musl | part of the C library |

Optional:

| Dependency | Needed for | Debian/Ubuntu package |
|---|---|---|
| Linux UAPI headers (`<linux/perf_event.h>`) | perf counters (`OLT_WITH_PERF`) | `linux-libc-dev` |
| AArch64 cross toolchain | cross-compiling for ARM boards | `g++-aarch64-linux-gnu` |
| Python 3 | `tools/olt-report.py` HTML report | `python3` |
| qemu user emulation | running cross-built tests on the build host | `qemu-user` |
| debhelper, dpkg-dev | building `.deb` packages | `debhelper`, `dpkg-dev` |

There are no runtime dependencies beyond the C and C++ standard libraries.

For analysis work there is a conda environment (Python, CMake, Ninja, NumPy,
pandas, Matplotlib, JupyterLab): `conda env create -f environment.yml && conda
activate olt`. Keep building the benchmark with the system compiler, since the
toolchain and C library are part of what is measured; the HTML report needs no
extra packages.

## Build options

Optional features follow one pattern, `OLT_WITH_<FEATURE>=AUTO|ON|OFF`: `AUTO`
enables the feature when its dependencies are found, `ON` fails configuration
if they are missing, and `OFF` builds a stub that reports the feature as
unavailable at run time. This keeps the tool buildable on boards and toolchains
with limited capabilities.

| Option | Default | Description |
|---|---|---|
| `OLT_WITH_PERF` | `AUTO` | hardware/software counters through `perf_event_open()` |
| `OLT_FORCE_GENERIC_ARCH` | `OFF` | use the portable compiler-builtin arch HAL (for bring-up on new CPUs) |
| `OLT_BUILD_TESTS` | `ON` | unit tests and a smoke run, executed with `ctest` |

Even when perf support is compiled in, which counters work depends on the CPU,
the kernel, virtualisation and `/proc/sys/kernel/perf_event_paranoid`. The
platform section shows which ones could be opened. Software counters (context
switches, migrations, page faults) usually work even without a PMU.

## Cross-compiling for ARM

```sh
sudo apt install g++-aarch64-linux-gnu qemu-user
cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
cmake --build build-arm64 -j
(cd build-arm64 && ctest)          # runs under qemu-aarch64 when installed
scp build-arm64/os-latency-tests board:
```

For other toolchains (Linaro, Yocto/Buildroot SDKs) pass
`-DOLT_CROSS_PREFIX=/path/to/bin/aarch64-none-linux-gnu-`. Numbers measured
under qemu are meaningless; always benchmark on the real board.

## Debian packages

The `debian/` directory builds a native package with debhelper and CMake
(build dependencies are listed in `debian/control`):

```sh
sudo apt install build-essential debhelper dpkg-dev cmake linux-libc-dev
dpkg-buildpackage -us -uc -b
sudo apt install ../os-latency-tests_*.deb
```

Cross-building an arm64 package on an amd64 host needs multiarch:

```sh
sudo dpkg --add-architecture arm64 && sudo apt update
sudo apt install crossbuild-essential-arm64 linux-libc-dev:arm64
dpkg-buildpackage -us -uc -b -aarm64
```

## Architecture

```
include/olt/hal/arch.h        CPU primitives: cycle counter, prefetch (L1/L2/L3/NTA/write),
  arch/x86_64.h               barriers, cpu_relax, cache-line flush, cache-line size
  arch/aarch64.h
  arch/generic.h              compiler-builtin fallback
include/olt/hal/os.h          OS services (C API): monotonic clock, affinity, SCHED_FIFO,
  src/hal/linux/os_linux.c    mlockall, cache topology, CPU/board/kernel/preempt info
include/olt/hal/perf.h        optional counters
  src/hal/linux/perf_linux.c  perf_event_open backend
  src/hal/stub/perf_stub.c    no-counter stub
include/olt/*.hpp, src/core/  C++17 harness: clock calibration, statistics, registry, reports
src/benchmarks/               benchmarks, self-registered with OLT_BENCHMARK(...)
tools/olt-report.py           results.json -> self-contained HTML report
```

The HAL headers are plain C, so they can also be used from C code. To port:

* **New CPU architecture**: add `include/olt/hal/arch/<arch>.h` implementing the
  contract documented in `arch.h` and select it there.
* **New OS**: implement `include/olt/hal/os.h` in `src/hal/<os>/` (and
  optionally `perf.h`), then add it to the HAL selection in `CMakeLists.txt`.

## Adding a benchmark

```cpp
#include "olt/benchmark.hpp"

OLT_BENCHMARK("mem.example", "What is measured, precisely")
{
    ctx.add_result("mem.example", {{"size", "4K"}}, ctx.run_samples([&] {
        const uint64_t t0 = olt::now_ns();
        // ... operation under test ...
        return static_cast<double>(olt::now_ns() - t0);
    }));
}
```

Add the file to the `os-latency-tests` target in `CMakeLists.txt`.
