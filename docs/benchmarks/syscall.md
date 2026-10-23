# System calls, clocks and timers

**Benchmarks:** `os.call` (per call), `os.timer_overshoot` (per method and requested time) ·
source: [src/benchmarks/syscall_linux.cpp](../../src/benchmarks/syscall_linux.cpp) (Linux only)

## Goal

Know three costs before designing a hot path that touches the OS:

- what one trip into the kernel costs;
- which clocks can be read cheaply;
- how late a thread that sleeps actually wakes up.

## What is measured

**`os.call`**: ns per call, in batches of 256:

| call | what it is |
|---|---|
| `syscall_getpid` | `syscall(SYS_getpid)`: a real kernel entry and exit with no work inside, the floor of every system call (glibc's `getpid()` may cache the value) |
| `clock_monotonic`, `clock_realtime`, `clock_monotonic_raw`, `clock_boottime` | `clock_gettime()`, served in user space by the vDSO on current kernels |
| `clock_monotonic_coarse` | vDSO, returns the last tick (1 to 4 ms resolution): the cheapest timestamp |
| `clock_thread_cputime` | `CLOCK_THREAD_CPUTIME_ID`: always a real system call |
| `cycle_counter` | `rdtsc` (x86) or `cntvct_el0` (ARM) |

**`os.timer_overshoot`**: the thread asks to sleep 1, 10, 100 or 1000 µs and
records how much longer it actually took, in ns:

```cpp
const uint64_t t0 = now_ns();
nanosleep(&request, nullptr);
const uint64_t late = now_ns() - t0 - request_ns;
```

The methods are:

- `nanosleep`
- `nanosleep_slack1`: the same after `prctl(PR_SET_TIMERSLACK, 1)`
- `timerfd`: a one-shot timer, then a blocking `read()`
- `epoll_wait`: with a timeout, which is whole milliseconds only, so only
  1000 µs is measured

Each point runs as many samples as fit in 0.4 s.

The report's platform block also lists the CPU vulnerability mitigations the
kernel has enabled (`/sys/devices/system/cpu/vulnerabilities`), with the full
list under this section.

## Reading the result

- **System call cost:** about 50 to 100 ns on current x86 with mitigations,
  more on older CPUs that need heavier ones (KPTI, retpolines, buffer
  clearing), and less on ARM.
- **vDSO clocks:** a vDSO clock costs a few tens of ns. If it costs as much as
  `syscall_getpid`, the vDSO fell back to a real system call. Common causes are
  an unstable TSC, a clock source without vDSO support (`hpet`, `acpi_pm`) and
  some VMs. Check
  `/sys/devices/system/clocksource/clocksource0/current_clocksource`.
- **Timer slack:** normal (`SCHED_OTHER`) threads get 50 µs of timer slack, so
  the kernel may wake them up to 50 µs late in order to batch wake-ups.
  `nanosleep_slack1` shows the same request without it. What is left is the
  timer interrupt, the scheduler and the idle-state exit (see `--no-idle`).
  Real-time threads (`-p`) get zero slack automatically.
- **timerfd vs nanosleep:** with the same slack they should behave alike. A
  timerfd is the one to use inside an event loop.

## Using it in C++

```cpp
// Cheap timestamps: steady_clock is CLOCK_MONOTONIC through the vDSO
auto t0 = std::chrono::steady_clock::now();

// Short waits: never sleep for less than the measured overshoot; spin instead
while (!ready.load(std::memory_order_acquire)) _mm_pause();   // or __builtin_arm_yield / std::this_thread::yield

// A latency-sensitive thread that has to sleep: remove the timer slack
prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);        // or run it SCHED_FIFO, which has none
```

- Keep system calls off the hot path. Batch them where the API allows
  (`sendmmsg`, `recvmmsg`, `io_uring`, `writev`), and cache values that do
  not change (pid, page size, CPU count).
- Use `CLOCK_MONOTONIC_COARSE` for cheap, rough timestamps such as timeouts
  and statistics. Use `CLOCK_MONOTONIC` (`steady_clock`) for measurements.
- `epoll_wait` and `poll` time out in whole milliseconds. For finer timeouts,
  add a `timerfd` to the epoll set, or use `epoll_pwait2` (Linux 5.11 and
  later).
- Disabling mitigations (`mitigations=off` on the kernel command line) makes
  system calls cheaper but removes protection against real attacks. It
  belongs only on isolated, trusted machines; see
  [expert-setup.md](../expert-setup.md).
