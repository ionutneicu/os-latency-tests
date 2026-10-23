# Glossary

Plain explanations of the hardware, operating-system and statistics terms used
in the reports, the [benchmark guides](benchmarks/README.md) and the
[roadmap](ROADMAP.md). Each entry says what the thing is and, where it matters,
what it means for your code. Numbers are typical for recent desktop, laptop and
server cores; your report gives the real ones for your machine.

Contents: [Reading the report](#reading-the-report) ·
[CPU core](#cpu-core) ·
[Instruction sets and vectors](#instruction-sets-and-vectors) ·
[Caches and memory](#caches-and-memory) ·
[Virtual memory and pages](#virtual-memory-and-pages) ·
[Multicore: coherence and atomics](#multicore-coherence-and-atomics) ·
[Power and frequency](#power-and-frequency) ·
[Operating system and scheduling](#operating-system-and-scheduling) ·
[Clocks and timers](#clocks-and-timers) ·
[Compiler and build](#compiler-and-build)

## Reading the report

**Sample.** One timed measurement. Each benchmark first runs untimed warm-up
iterations (to fill the caches, fault in memory and let the clock settle), then
`--samples` timed ones. A sweep point (`--sweep-samples`) times many operations
per sample and reports the average per operation.

**Percentile (p50, p90, p99, p99.9, p99.99).** The value that this share of
samples did not exceed. p50, the median, is the typical case: half the samples
were faster. p99 means 1 sample in 100 was slower. The high percentiles are the
**tail latency**. For latency-critical code they matter more than the average,
because a request that does many operations meets the tail often.

**Unreliable percentile (`~`).** A percentile with fewer than about 10 samples
beyond it rests on too few points. The report marks it with `~`. Raise
`--samples` if you need it.

**min, max.** The fastest and slowest sample. The minimum is close to the cost
without interference. The maximum often includes an interrupt, a page fault or
another program running.

**MAD.** Median absolute deviation: the typical distance of a sample from the
median. It measures spread without being thrown off by a few outliers.

**Band.** The shaded area on the sweep charts, from the minimum to p90 across
samples.

**Units: ns per access, per line, per store, per operation.** What one benchmark
step costs. Bandwidth is derived from it: GB/s = line size in bytes / ns per
line. GB/s is decimal (10^9 bytes per second). Sizes are binary: 1 KiB is 1024
bytes, 1 MiB is 1024 KiB.

**Log scale.** An axis where each step multiplies the value instead of adding to
it, so equal distances are equal ratios. It is used where values span orders of
magnitude, such as 1.5 ns for L1 against 170 ns for DRAM.

**Speed-up.** Time of the baseline divided by time of the variant: 4x means four
times faster.

**Working set.** The memory a piece of code touches over and over. When it fits
in a cache level, accesses cost that level's latency. Most of the memory charts
plot cost against working-set size.

## CPU core

**Core, logical CPU, SMT (Hyper-Threading).** A core is one complete set of
execution hardware. With simultaneous multithreading (SMT; Intel calls it
Hyper-Threading) one core runs two hardware threads, which Linux shows as two
logical CPUs. They share the core's execution units and its L1 and L2 caches.
On the i7-1165G7, CPUs *n* and *n*+4 are such **SMT siblings**. Benchmarks that
need two cores pick CPUs on different physical cores.

**Out-of-order execution.** A modern core fetches instructions in program order
but runs each one as soon as its inputs are ready, keeping a few hundred in
flight (the **reorder buffer**, ROB), and then retires them in order. Because
independent work overlaps, the speed of many loops is set by their longest
chain of dependent instructions, not by how many instructions they contain.

**Execution ports and units.** The adders, multipliers, floating-point units and
load and store units that instructions are sent to. How many of each a core has
sets its throughput for that kind of instruction.

**Latency and throughput of an instruction.** Latency is the number of cycles
from an instruction's inputs being ready to its result being usable, for
example 4 cycles for a double add. Throughput is how many of those instructions
can start per cycle, for example 2 double adds per cycle. A chain of dependent
instructions runs at the latency; independent instructions run at the
throughput. `cpu.ilp` measures both.

**Dependency chain.** A sequence where each operation needs the result of the
previous one, such as `sum += x[i]` or `p = p->next`. It takes latency × length,
however wide the core is.

**Instruction-level parallelism (ILP).** The number of independent instructions
the core can overlap. Splitting a chain into several independent ones (more
accumulators, loop unrolling) exposes more ILP.

**Loop unrolling, multiple accumulators.** Writing several iterations per pass
of a loop, each with its own partial result, so that they do not wait for each
other. `cpu.ilp` tells you how many are worth having.

**Branch prediction, misprediction.** The core guesses the direction and target
of every branch to keep fetching ahead. A wrong guess throws away the work done
on the wrong path, costing about 15 to 20 cycles on big x86 and ARM cores.
Predictable branches are almost free; branches that follow random data are
expensive. **Branchless** code (conditional moves such as `cmov` and `csel`, or
bit masks) avoids the risk by always computing both sides.

**BTB (branch target buffer).** A cache of the targets of recently taken
branches. Hot code with more taken branches than the BTB holds slows the front
end.

**Front end, L1i, uop cache.** The front end fetches and decodes instructions.
The L1 instruction cache (L1i, 32 to 64 KiB) holds instruction bytes. The uop
cache (Intel DSB, AMD Op cache, Arm MOP cache on some cores) holds a few
thousand already-decoded operations and delivers them faster and with less
power. Hot code that outgrows these slows down even when its data fit in cache.

**Speculative execution.** Running instructions before it is certain they are
needed, for example past a predicted branch. It is the source of much of a
core's speed and also of the Spectre family of attacks. The mitigations for
those attacks (KPTI, retpolines, IBRS and others) add cost to system calls,
context switches and indirect branches.

**Store buffer, store forwarding.** Stores wait in the store buffer until they
can be written into L1, and the core carries on without waiting. A later load
from the same address takes its value straight from the buffer (store-to-load
forwarding). This is why stores that miss the cache cost so little in
`mem.write_random`, until the buffer (roughly 50 to 120 entries) fills up.

## Instruction sets and vectors

**ISA (instruction set architecture).** The set of instructions a CPU
understands. **x86-64** (also called amd64 or Intel 64) is used by Intel and AMD
processors. **AArch64** (also called ARM64; the 64-bit state of Armv8-A and
Armv9-A) is used by ARM servers, Apple silicon, the Raspberry Pi 4 and 5 and
most phones.

**SIMD, vector instructions.** Single instruction, multiple data: one instruction
works on several values held in one wide register, for example four doubles in
256 bits. Compilers generate them on their own (auto-vectorisation) or you write
them with intrinsics.

**Vector width.** The register size: 128 bits (16 bytes) for SSE and NEON, 256
bits (32 bytes) for AVX and AVX2, 512 bits (64 bytes) for AVX-512. A wider
vector moves more data per instruction. L1 and L2 can deliver more bytes per
cycle than narrow code asks for, so the bandwidth sweep uses the widest vectors
the CPU has; with 16-byte vectors it would measure the code, not the cache.

**SSE2.** Streaming SIMD Extensions 2: 128-bit vectors of integers and doubles
(the `xmm` registers). Every x86-64 CPU has it, so it is the fallback when
nothing wider is available. SSE3, SSSE3 and SSE4.1/4.2 add instructions on the
same 128-bit registers.

**AVX, AVX2.** Advanced Vector Extensions: 256-bit `ymm` registers. AVX (2011)
added 256-bit floating point; AVX2 (2013 on Intel, 2015 on AMD) extended it to
integers and added gathers. Usually found together with **FMA**. Nearly every
x86 CPU from the last ten years has both, apart from some low-end models.

**AVX-512.** 512-bit `zmm` registers, 32 vector registers and 8 mask registers
for per-element conditions. It is a family of subsets: **AVX-512F**
(Foundation) is the base every AVX-512 CPU has; BW, DQ, VL, VNNI and others add
to it. It is present on Intel Xeon since Skylake-SP (2017), on Ice Lake, Tiger
Lake and Rocket Lake client chips (the i7-1165G7 is Tiger Lake), on Sapphire
Rapids and later Xeons, and on AMD Zen 4 and Zen 5. Intel client chips from
Alder Lake (12th generation) on do not have it. On older Xeons (Skylake-SP,
Cascade Lake), heavy 512-bit code lowers the clock frequency for a few
milliseconds (the **frequency licence**), which can make it slower overall.
Recent CPUs do this much less.

**Why "AVX-512" in the report but `avx512f` in results.json.** "AVX-512" is the
vendor's name for the extension. `avx512f` is the CPU flag Linux lists in
`/proc/cpuinfo` (lower case, no dash, one flag per subset); compiler options use
the same spelling (`-mavx512f`). The report shows the vendor names (AVX-512,
AVX2, SSE2, NEON). results.json keeps the short codes in the `isa` parameter:
`avx512`, `avx2`, `sse2`, `neon`, `generic`.

**x86-64 microarchitecture levels.** Feature bundles that compilers and
distributions target: **x86-64** (baseline, SSE2), **x86-64-v2** (adds SSE4.2
and POPCNT, CPUs from about 2009), **x86-64-v3** (adds AVX2, FMA, BMI1/2, about
2013 on), **x86-64-v4** (adds AVX-512F/BW/CD/DQ/VL). Code built with
`-march=x86-64-v3` stops with an illegal-instruction error (SIGILL) on a CPU
without v3. Some distributions now require v2 or v3.

**FMA (fused multiply-add).** `a * b + c` in one instruction with one rounding.
It is faster and slightly more accurate, so results differ in the last bit from
a separate multiply and add. `-ffp-contract` controls whether the compiler fuses
them.

**NEON (Advanced SIMD).** ARM's 128-bit SIMD, with 32 registers. It is mandatory
in AArch64, so every 64-bit ARM CPU has it.

**SVE, SVE2.** ARM's Scalable Vector Extension. The hardware chooses the vector
length (128 to 2048 bits) and the same binary runs on all of them; it has
per-element conditions like AVX-512. Found on Neoverse V1/V2 servers (AWS
Graviton3 and 4), Fujitsu A64FX and recent Cortex cores. The benchmarks do not
use it yet.

**Intrinsics.** C functions that map to single vector instructions, such as
`_mm512_add_epi64` from `<immintrin.h>` or `vaddq_u64` from `<arm_neon.h>`.

**CPUID, CPU flags, runtime dispatch.** CPUID is the x86 instruction that tells
a program which extensions the CPU has; Linux lists them as flags in
`/proc/cpuinfo`. Runtime dispatch means shipping several versions of a function
and choosing one at start-up from those flags, so one binary is fast on new CPUs
and still runs on old ones. `src/hal/stream.c` does this for the bandwidth
kernels. Compilers can generate it for you with
`__attribute__((target_clones("avx2", "default")))`.

**`-march`, `-mcpu`, `-mtune`.** Compiler options for the target CPU. `-march`
(x86) and `-mcpu` (ARM) allow newer instructions, and the binary then needs a
CPU that has them. `-mtune` only changes instruction choice and scheduling; the
binary still runs everywhere.

## Caches and memory

**Cache line.** The unit that caches store and move around: 64 bytes on almost
all x86 and ARM CPUs (Apple M-series report 128). Touching one byte brings in
the whole line, so data used together should sit together.

**L1, L2, L3 (LLC).** Cache levels, from small and fast to large and slower.
Typical figures for a recent core:

| level | size | latency |
|---|---|---|
| L1d | 32 to 64 KiB per core | 4 to 5 cycles, about 1 to 1.5 ns |
| L2 | 0.5 to 2 MiB per core | 12 to 16 cycles, about 4 to 5 ns |
| L3 (last-level cache, LLC) | several MiB to hundreds of MiB, shared | 10 to 20 ns on client chips, more on big servers |
| DRAM | GiB | 70 to 120 ns, more on servers |

The i7-1165G7 has 48 KiB L1d and 1.25 MiB L2 per core and 12 MiB of shared L3.

**L1d, L1i.** The separate first-level caches for data and for instructions.

**Private and shared caches.** L1 and L2 usually belong to one core (some ARM
and Intel designs share L2 within a small cluster). L3 is shared by all cores
of a chip or a cluster.

**Hit, miss, miss penalty.** A hit finds the line in this level. A miss has to
go to the next level and pays that level's latency.

**DRAM, main memory.** The RAM modules. 70 to 150 ns from one core. One core
gets roughly 10 to 20 GB/s; a whole server socket gets hundreds of GB/s.

**Latency and bandwidth.** Latency is the time for one access to come back.
Bandwidth is bytes per second with many accesses in flight. Following a linked
list is latency-bound; streaming through an array is bandwidth-bound.

**Load-to-use latency.** Cycles from a load being issued until the next
instruction can use the loaded value. `mem.latency_random` measures it with a
pointer chase.

**Pointer chase, Sattolo cycle.** A buffer where each cache line holds the
address of the next line to visit, in random order, forming a single cycle
through every line (built with Sattolo's algorithm). Each load needs the
previous one's result and the next address cannot be guessed, so neither the
out-of-order core nor the prefetchers can hide the latency.

**Hardware prefetcher.** Logic that watches the access pattern (sequential,
constant stride) and fetches lines before they are asked for. It is why reading
an array in order is much faster than reading the same memory at random.
Prefetchers cannot predict random access and generally stop at page boundaries.

**Adjacent-line (spatial) prefetcher.** On many Intel cores a miss also fetches
the other line of the aligned 128-byte pair. As a result, two threads writing
data 64 bytes apart can still slow each other down. This is why
`multicore.false_sharing` often finds 128 bytes, not 64, as the safe distance.

**Software prefetch.** An instruction that hints an address will be needed soon
(`__builtin_prefetch`, `prefetcht0`, `prfm`). It helps when the address is known
early but the pattern is irregular, as in batched hash lookups.

**Memory-level parallelism (MLP).** How many cache misses one core keeps in
flight at once. It is limited by the line fill buffers (about 10 to 16 on recent
x86). `cpu.ilp` measures it with the `load_mem` chains: about 12 on the CI
machine.

**Line fill buffer (LFB), MSHR.** Per-core slots that track outstanding cache
misses ("line fill buffer" is Intel's name; "miss status holding register" is
the general one). When all are busy, further misses wait.

**Little's law.** Concurrency = throughput × latency. For memory: bandwidth =
misses in flight × line size / latency. With 12 misses in flight, 64-byte lines
and 100 ns latency, a core gets at most about 7.7 GB/s from random DRAM
accesses, however fast the memory is. Prefetchers raise the effective number in
flight, which is why sequential bandwidth is higher.

**Read for ownership (RFO), write-allocate, write-back.** Before writing to a
line that is not in its cache, a core must fetch the whole line and take
exclusive ownership of it. The modified line goes back to memory later, when it
is evicted (write-back). So a normal store to uncached memory moves the line
twice, in and out. That is why writing to DRAM is slower than reading, and why
streaming stores help for large buffers.

**Non-temporal (streaming) stores.** Stores that bypass the caches: `movntdq`
and related instructions on x86 (the `_mm_stream_*` intrinsics), `stnp` (a hint)
on ARM. They skip the read for ownership and do not push useful data out of the
cache; writes are gathered into full lines in **write-combining buffers**. They
win for large buffers written once and not read again soon, and lose when the
data would have fit in cache. They are weakly ordered, so an `sfence` is needed
before another thread reads the data.

**Coherence granule.** The block that cores compete over when they write: one
cache line, or in practice two where the adjacent-line prefetcher pairs them.

## Virtual memory and pages

**Virtual memory, page.** Programs use virtual addresses, which the kernel maps
to physical memory in pages. Pages are 4 KiB by default on x86-64 and most ARM
Linux systems; some ARM distributions and Apple silicon use 16 or 64 KiB.

**TLB (translation lookaside buffer).** A cache of virtual-to-physical page
translations: a small first-level data TLB (dTLB), an instruction TLB (iTLB)
and a larger second-level TLB (STLB) of about one to a few thousand entries.

**TLB reach.** The memory the TLB covers: entries × page size. 1536 entries ×
4 KiB is 6 MiB; with 2 MiB pages the same entries cover 3 GiB. Past the reach,
random accesses also pay a page walk, which shows in the cache sweep as an extra
step unrelated to any cache size. Run with `--hugepages` to separate the two.

**Page walk.** On a TLB miss the hardware reads the page tables (4 or 5 levels
on x86-64) to find the translation. Page-table entries are cached too, but a
walk can still add tens of nanoseconds to a DRAM miss.

**Huge pages (2 MiB, 1 GiB).** Larger pages, so far fewer TLB entries are
needed. **Transparent huge pages (THP)** let the kernel use 2 MiB pages
automatically; the mode is always, madvise or never in
`/sys/kernel/mm/transparent_hugepage/enabled`, and the report shows it.
`madvise(MADV_HUGEPAGE)` asks for them on one region. 1 GiB pages are reserved
at boot (expert tier).

**Page fault (minor, major).** The first touch of a page traps into the kernel,
which maps it. A minor fault (memory available, page zeroed) costs around a
microsecond. A major fault reads data from disk and costs milliseconds. The
benchmarks touch their buffers before timing (**pre-faulting**) so faults never
land in a timed loop.

**mlock, mlockall.** Keep memory resident in RAM so it is never paged out.
Locking faults the pages in, so later accesses do not fault. `-m` locks all of
the process's memory (root or a high `RLIMIT_MEMLOCK`).

**NUMA.** On multi-socket servers, and some large single-socket ones, memory is
attached to particular sockets or dies, and reaching another socket's memory is
slower. Laptops and small boards have a single node. Not measured yet.

## Multicore: coherence and atomics

**Cache coherence (MESI).** The hardware protocol that keeps every core's view
of memory consistent. In each cache a line is Modified, Exclusive, Shared or
Invalid (MESI; variants add Owned or Forward). Writing a line invalidates every
other copy; reading a line that another core modified moves it across. These
transfers are what `multicore.c2c` and `multicore.false_sharing` measure.

**Core-to-core latency.** The time for a line written by one core to be seen by
another: a few tens of ns between cores that share an L3, more between chiplets
(AMD CCDs) or sockets. The report's heatmap shows which CPU pairs talk fastest.

**False sharing.** Two threads write different variables that happen to share a
cache line (or an adjacent-line pair). The line bounces between the cores
although no data is shared. The fix is to pad or align per-thread hot data
(`alignas(128)`).

**`std::hardware_destructive_interference_size`.** The C++17 constant meant to
give the padding that avoids false sharing. Compilers often report 64, which is
not enough where the adjacent-line prefetcher pairs lines; prefer the measured
number.

**Atomic read-modify-write (RMW).** An indivisible read, change and write of one
memory location: `fetch_add`, `exchange`, compare-and-swap (CAS,
`compare_exchange`). x86 uses `lock`-prefixed instructions. ARM uses either
load-linked/store-conditional loops (`ldxr`/`stxr`) or the Armv8.1 **LSE**
atomics (`ldadd`, `cas`, `swp`), which hold up much better under contention.
GCC 10 and later pick LSE at run time on AArch64 ("outline atomics").

**Memory ordering.** The rules for when other cores see a core's loads and
stores. x86 is **TSO** (total store order: only a later load may overtake an
earlier store). ARM is weakly ordered: most reorderings are allowed, and
barriers or acquire/release instructions (`ldar`, `stlr`) restore order. In C++,
`std::memory_order` (relaxed, acquire, release, acq_rel, seq_cst) states what
you need and the compiler emits the right instructions. `relaxed` costs nothing
extra on either; a `seq_cst` store costs a full barrier on x86.

**Memory barrier (fence).** An instruction that stops reordering across it:
`mfence`, `sfence`, `lfence` on x86; `dmb`, `dsb` on ARM.

**SPSC, MPSC, MPMC.** Single or multiple producers and consumers of a queue: how
many threads write and how many read. The fewer, the cheaper the
synchronisation.

**Lock-free.** A data structure in which some thread always makes progress
without taking a lock, using atomics. It avoids sleeping in the kernel, but not
the cache-line transfers.

**Spinning and blocking.** Waiting in a loop that checks a flag (spinning: the
fastest wake-up, but it keeps a core busy) or sleeping in the kernel until woken
(blocking: frees the core, but waking takes microseconds). `pause` on x86 and
`yield` on ARM make a spin loop kinder to the SMT sibling and to power use.

## Power and frequency

**C-states (idle states).** Power-saving states an idle core enters. C0 is
running, C1 is halted (wakes in about a microsecond), deeper states such as C6
switch off parts of the core (tens to hundreds of microseconds to wake). A
thread woken on a deeply idle core waits for that exit, and on laptops that is
often most of a blocking hand-off: measured 15 µs on the i7-1165G7 against
2.2 µs with deep idle blocked.

**`/dev/cpu_dma_latency` and `pm_qos_resume_latency_us` (PM QoS).** Writing the
longest acceptable wake-up latency to `/dev/cpu_dma_latency`, and keeping the
file open, stops the kernel from using idle states that take longer to exit, on
every CPU. The per-CPU file
`/sys/devices/system/cpu/cpuN/power/pm_qos_resume_latency_us` does the same for
one CPU and stays set after the process exits; `0` there means no limit and
`n/a` means polling only. The kernel uses the stricter of the two. `--no-idle`
sets C1's exit latency on the benchmark CPUs only. Both need root.

**cpuidle driver.** The kernel component that chooses idle states: `intel_idle`,
`acpi_idle`, `psci_idle` on ARM. The report lists it.

**P-states, DVFS.** Frequency and voltage steps, chosen by the cpufreq governor
or by the CPU itself (Intel HWP, AMD CPPC).

**cpufreq governor.** The kernel's frequency policy: `performance` (highest
frequency), `powersave` (with the intel_pstate driver this is a dynamic,
energy-saving policy, not the minimum frequency), `schedutil` (follows load).
`performance` gives the steadiest numbers; the pre-flight check warns if another
governor is active.

**Turbo boost.** Running above the base frequency while power and temperature
allow; how much depends on how many cores are busy and on cooling, so results
vary between runs. The benchmarks report time, not cycles.

## Operating system and scheduling

**System call, kernel and user space.** User code asks the kernel for a service
with a system call: a switch into kernel mode that costs from roughly 50 ns to a
few hundred ns, depending on the CPU and on which Spectre and Meltdown
mitigations are active.

**vDSO.** Kernel code mapped into every process so that a few calls, chiefly
`clock_gettime`, run without entering the kernel: about 20 ns instead of a
system call.

**Context switch.** The kernel saving one thread's state and running another.
It costs microseconds, more once the new thread has to refill the caches.

**Futex.** Linux's "fast user-space mutex". Mutexes and condition variables stay
in user space while uncontended and enter the kernel only to sleep or to wake a
waiter. A blocking wake-up is mostly futex wake, scheduling and idle exit.

**Scheduler (CFS, EEVDF).** Linux's default policy for normal threads
(`SCHED_OTHER`): CFS until kernel 6.6, EEVDF since.

**SCHED_FIFO, real-time priority.** A real-time policy. A runnable SCHED_FIFO
thread always preempts normal threads and runs until it blocks or a
higher-priority one wants the CPU. Priorities go from 1 to 99. `-p 80` runs the
benchmarks at FIFO 80 (root or `CAP_SYS_NICE`). By default the kernel keeps
about 5% of each second for normal threads (RT throttling, or the fair server
on newer kernels), so a real-time thread that never blocks still pauses briefly.

**CPU affinity, pinning.** Restricting a thread to chosen CPUs
(`sched_setaffinity`, `taskset`, the `--cpus` and `--affinity` options) so that
it does not migrate and its caches stay warm.

**isolcpus, nohz_full, rcu_nocbs, IRQ affinity.** Boot options and settings that
keep the kernel's own work (other tasks, the timer tick, RCU callbacks,
interrupts) off chosen CPUs, for cores with as little jitter as possible.
Expert tier, see [expert-setup.md](expert-setup.md).

**Timer tick (HZ).** A periodic interrupt, 100 to 1000 times per second, for
scheduling and timekeeping. Each tick takes a few microseconds from the CPU it
hits. `nohz_full` stops it on a CPU that runs a single task.

**OS jitter (noise).** Time taken from a running thread by interrupts, kernel
threads, the tick and firmware. It shows as rare large samples (p99.9, max).

**SMI (system management interrupt).** A firmware interrupt that the operating
system cannot see or block; it can take tens to hundreds of microseconds. It is
a common cause of unexplained maximum values on some machines.

**Preemption model.** How soon the kernel can switch to a higher-priority task
while it is running kernel code: `PREEMPT_NONE`, `PREEMPT_VOLUNTARY`,
`PREEMPT` (full), `PREEMPT_DYNAMIC` (chosen at boot with `preempt=`), and
`PREEMPT_RT` (in the mainline kernel since 6.12), which makes almost all kernel
code preemptible for the lowest worst-case latency at some cost in throughput.
The report's header shows the model.

**perf events, PMU.** The CPU's performance monitoring unit counts events such
as cycles, instructions, cache misses and branch mispredictions; Linux exposes
it through `perf_event_open`, gated by `/proc/sys/kernel/perf_event_paranoid`.
Optional in this project (`OLT_WITH_PERF`).

## Clocks and timers

**TSC (time stamp counter).** The x86 counter read by `rdtsc`. On modern CPUs
(**invariant TSC**) it ticks at a constant rate whatever the current frequency.
Reading it with ordering fences takes a few tens of cycles. The harness
calibrates it against `CLOCK_MONOTONIC`.

**Generic timer (`CNTVCT_EL0`).** ARM's constant-rate counter. It often runs at
24 to 100 MHz on current boards, a resolution of 10 to 40 ns; Armv8.6 and later
fix it at 1 GHz.

**CLOCK_MONOTONIC.** The Linux clock that never jumps backwards, unlike wall-clock
time. It is read through the vDSO and is the harness's time base.

**Timer slack, timer overshoot.** Sleeps and timeouts end later than requested:
the kernel groups nearby wake-ups (timer slack, 50 µs by default for normal
threads, none for real-time threads) and the woken thread still has to be
scheduled.

## Compiler and build

**Optimisation levels (`-O0` to `-O3`, `-Os`, `-Oz`).** `-O2` is the usual
release level. `-O3` adds more aggressive inlining, unrolling and vectorisation:
sometimes faster, sometimes only bigger. `-Os` and `-Oz` optimise for size. This
project builds as CMake's RelWithDebInfo by default (`-O2 -g -DNDEBUG`). `-O0`
(no optimisation, GCC's default) keeps every variable in memory and makes every
small function a real call, so a timed loop measures loads and stores to the
stack instead of the operation under test. The tool warns about such a build, and
the report marks its results as not valid.

**Auto-vectorisation.** The compiler turning loops into SIMD code. GCC does it at
`-O2` since version 12 (only when clearly cheap) and fully at `-O3`; Clang does
it at `-O2`.

**Inlining.** Replacing a call with the body of the called function. It removes
the call overhead and lets the optimiser specialise the code, at the cost of
code size (see the instruction-cache item in the roadmap).

**LTO (link-time optimisation).** Optimising across source files at link time
(`-flto`), so functions from other files can be inlined and virtual calls
resolved.

**PGO (profile-guided optimisation).** Building twice: an instrumented build
records which code is hot on a representative run, and the final build uses that
profile for inlining and code layout. BOLT and AutoFDO get the profile from
`perf` samples instead.

**Devirtualisation.** The compiler replacing a virtual call with a direct one,
which can then be inlined, when it can prove the object's type (`final`
classes, LTO, or a PGO guess with a check).

**`-ffast-math`.** Lets the compiler treat floating-point numbers like real
numbers: reorder sums, assume no NaN or infinity, ignore `errno`, flush tiny
values to zero. Faster, but results change and NaN checks can disappear.

**ULP (unit in the last place).** The gap between adjacent floating-point
numbers at a given value; used to express tiny differences in results. One ULP
of a double is about 2.2 × 10^-16 of its value.

**Denormals, FTZ/DAZ.** Denormal (subnormal) numbers are tiny floats near zero
that some CPUs handle on a slow path costing about a hundred cycles. FTZ and DAZ
(bits in the x86 `MXCSR` register; `FZ` in ARM's `FPCR`) flush them to zero.

**Undefined behaviour (UB).** Code the C++ standard gives no meaning to: signed
overflow, out-of-bounds access, a false `[[assume]]`. Compilers optimise on the
assumption that it never happens, which is a source of both speed and
surprising bugs.

**`noexcept`.** A promise that a function does not throw. Callers need less
unwinding bookkeeping, and `std::vector` moves elements instead of copying them
when it grows. If the function throws anyway, the program calls
`std::terminate`.

**Zero-cost exceptions.** The exception model of the Itanium C++ ABI used on
Linux: no run-time cost while nothing is thrown (only tables are added), but a
throw is slow, microseconds, because the stack is unwound using those tables.

**`[[likely]]`, `[[unlikely]]`, `__builtin_expect`.** Tell the compiler which way a
branch usually goes, so it lays out the usual path as straight-line code. They
do not change the CPU's branch predictor.

**`[[assume(expr)]]`.** C++23: the compiler may take `expr` as true and optimise
on it (`__builtin_assume` in Clang; `if (!expr) __builtin_unreachable();` in
older GCC). There is no `std::assume` function. `std::unreachable()` (C++23)
marks code that is never reached, and `std::assume_aligned` (C++20) promises a
pointer's alignment. If the promise is false, the behaviour is undefined.

**`__restrict`.** A promise that two pointers do not point at overlapping memory,
so the compiler can keep values in registers and vectorise.

**Hardening flags.** Compiler and library options that catch or block memory
errors at a run-time cost: `_FORTIFY_SOURCE` (checked `memcpy`, `strcpy` and
others), stack protector (canaries), stack-clash protection, Intel CET
(`-fcf-protection`: branch-target markers and shadow stack), ARM PAC/BTI
(`-mbranch-protection`), `_GLIBCXX_ASSERTIONS` (checked `operator[]` in
libstdc++). Distributions enable many of them by default.

**PIE, PLT, GOT, symbol visibility.** Position-independent executables (the
default on most distributions) and shared libraries reach functions and globals
through tables (PLT and GOT), which adds an indirection to calls between
libraries. `-fno-plt`, `-fvisibility=hidden` and `-fno-semantic-interposition`
reduce it.

**TLS models (`thread_local`).** A thread-local variable in the executable is
reached with a single instruction. In a shared library loaded with `dlopen` it
may need a call to `__tls_get_addr` on every access, unless the
`initial-exec` model is used.
