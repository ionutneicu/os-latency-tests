# Benchmark guide

One page per benchmark group. Each page says what question the benchmark
answers, what exactly is timed (with the measured loop), how to read the
result, and how to apply it in C++ code. The HTML and Markdown reports link
each section to its page.

> Apply these techniques only to the measured hot path of ultra-low-latency or
> high-performance code. They trade readability, maintainability and
> portability for nanoseconds; everywhere else, write the simple version.

Unfamiliar term? See the [glossary](../GLOSSARY.md).

Three groups. Hardware is what the CPU, caches and cores do to your code. OS
latency is what the kernel and scheduler add when your thread waits, wakes,
sleeps or is interrupted. Software is the C++ choices made in your own code:
containers, allocators, queues and dispatch.

### Hardware

| Page | Benchmarks | Question |
|---|---|---|
| [Memory hierarchy](memory.md) | `mem.cache_sweep` | How big can my working set be before it gets slow? |
| [Instruction-level parallelism](ilp.md) | `cpu.ilp` | How far should I unroll; how many independent lookups at once? |
| [Branch misprediction](branch.md) | `cpu.branch` | Branch or branchless: when does each win? |
| [Core-to-core latency](c2c.md) | `multicore.c2c` | Which CPUs should communicating threads run on? |
| [False sharing](false-sharing.md) | `multicore.false_sharing` | How far apart must per-thread data be? |
| [Atomics](atomics.md) | `multicore.atomic_contention`, `multicore.atomic_order` | Shared atomic or per-thread counters? Which memory order? |

### OS latency

| Page | Benchmarks | Question |
|---|---|---|
| [Threads](threads.md) | `thread.create` | What does it cost to spawn a thread, and is a pool worth it? |
| [Blocking wake-up](sync.md) | `sync.mutex_wakeup`, `sync.condvar_wakeup` | What does it cost to block and be woken? |
| [System calls, clocks, timers](syscall.md) | `os.call`, `os.timer_overshoot` | What does a system call cost? Which clock is cheap? How late does a sleep wake up? |
| [OS jitter](jitter.md) | `os.jitter` | How often and how long is my core taken away from my thread? |

### Software: C++ patterns

| Page | Benchmarks | Question |
|---|---|---|
| [Heap allocator](alloc.md) | `alloc.malloc_free` | How expensive is `malloc`/`new` on the hot path? |
| [Container memory](pattern-alloc.md) | `pattern.alloc` | Default allocator, `reserve()`, or a pmr arena? |
| [SPSC queues](pattern-spsc.md) | `pattern.spsc` | Mutex, spinlock or lock-free queue between two threads? |
| [Dispatch](pattern-dispatch.md) | `pattern.dispatch` | Virtual, `std::function`, `std::variant` or templates? |

Every benchmark runs under the same harness: warm-up iterations first, then
`--samples` timed iterations (or `--sweep-samples` for batch measurements),
with optional pinning (`--cpus`, `--affinity`), real-time priority (`-p`),
locked memory (`-m`) and deep C-states blocked (`--no-idle`). Results are full
distributions; percentiles with fewer than ~10 samples beyond them are marked
unreliable.
