# Expert setup: tuning beyond a stock kernel

The standard benchmarks run on any distribution kernel. This guide covers the
changes that need a reboot, a different kernel or firmware settings. They can
cut tail latency (p99.9 and worse) by an order of magnitude on a dedicated
machine, but each one has a cost, and most applications never need them.

**Read this first**

* Measure before you tune. Run `./run-all.sh` on the stock setup and keep the
  report: every change below should be judged against it.
* Change one thing at a time and re-run, or you will not know what helped.
* These settings affect the whole machine: power draw, heat, throughput of
  other work, sometimes security. Use them on dedicated latency boxes, not
  on a laptop you also browse with.
* Kernel command line changes go in `/etc/default/grub` (`GRUB_CMDLINE_LINUX`)
  followed by `sudo update-grub` (Debian/Ubuntu) or
  `sudo grub2-mkconfig -o /boot/grub2/grub.cfg` (Fedora), then a reboot.
  Keep a known-good boot entry so you can revert. Check what is active with
  `cat /proc/cmdline`.

## Quick guide: is it worth trying?

| Change | Helps with | Typical effect | Cost | Worth it when |
|---|---|---|---|---|
| Performance governor | frequency ramp-up delays | less variance, lower p50 on bursty loads | power, heat | almost always on a latency box (no reboot needed) |
| Idle limit (`--no-idle`, PM QoS) | C-state exit latency | wake-ups from tens of us to a few us | power while held | already used by `run-all.sh`; first thing to try |
| `isolcpus` / `nohz_full` / `rcu_nocbs` | scheduler, timer tick and RCU noise on chosen cores | worst-case gaps from ~100 us down to a few us | those cores run only what you pin there | a dedicated hot thread per core |
| IRQ affinity away from hot cores | interrupts landing on the hot core | removes the largest remaining spikes | needs per-device setup | after isolation, if jitter remains |
| PREEMPT_RT kernel | long non-preemptible kernel sections | bounded worst case (p99.99/max) | lower throughput, different kernel | hard deadlines, control loops, audio |
| `mitigations=off` | syscall and context-switch overhead | syscalls and switches can get noticeably cheaper on affected CPUs | **removes Spectre/Meltdown-class protections** | isolated, trusted, single-tenant machines only |
| `idle=poll` / C-states off in BIOS | idle exits on all cores | similar to `--no-idle`, always on | cores never sleep: high power | when you cannot hold `/dev/cpu_dma_latency` open |
| Reserved huge pages at boot | TLB misses, fragmentation | fewer page walks on large working sets | memory reserved permanently | big tables or buffers on the hot path |
| SMT (Hyper-Threading) off | sibling thread stealing core resources and L1/L2 | 10-40% faster and steadier hot loops when the sibling would be busy; nothing if it stays idle | half the hardware threads; throughput work loses 15-30% | latency matters more than throughput; or isolate both siblings instead |
| Kernel-bypass networking | kernel network stack cost | packet round trips down to a few us | dedicated NICs/cores, new APIs | trading, telecom, packet processing |

The "typical effect" column is a direction, not a promise: it depends on the
CPU, the board and the workload. The point of this project is to measure it on
yours.

## CPU isolation: `isolcpus`, `nohz_full`, `rcu_nocbs`

**What it does.** Keeps the scheduler, the periodic timer tick and RCU
callbacks off a set of cores, so a thread pinned there runs almost
uninterrupted.

```
isolcpus=managed_irq,domain,2-3 nohz_full=2-3 rcu_nocbs=2-3
```

Pick cores that are not CPU 0 and, on SMT machines, isolate both siblings of a
physical core (`lscpu -e` shows the pairs). Then run with
`./build/os-latency-tests --cpus 2,3` to measure on those cores.

**What to expect.** The OS jitter benchmark (roadmap) shows gaps on an isolated
core shrinking from tens or hundreds of microseconds to single digits. Mean
latency barely moves; the tail does.

**Costs.** Isolated cores are not load-balanced: anything you do not pin there
explicitly never runs there. `nohz_full` needs at most one runnable task per
core to stop the tick.

**Check.** `cat /sys/devices/system/cpu/isolated`, `cat /sys/devices/system/cpu/nohz_full`.
The report's platform table shows isolated CPUs.

## Interrupt affinity

**What it does.** Moves device interrupts (NIC, storage, timers where
possible) to housekeeping cores.

```sh
sudo systemctl stop irqbalance          # or configure it to skip the hot cores
for irq in /proc/irq/[0-9]*; do echo 0-1 | sudo tee $irq/smp_affinity_list; done
```

Some interrupts cannot be moved (per-CPU timers, IPIs). `managed_irq` in
`isolcpus` handles many driver-managed queues automatically.

**What to expect.** Removes most of the remaining large spikes after
isolation. Watch `/proc/interrupts` on the hot cores during a run: the counts
should stay flat.

## Real-time kernel (PREEMPT_RT)

**What it does.** Makes almost all kernel code preemptible and turns interrupt
handlers into schedulable threads, so a high-priority task waits a bounded
time. PREEMPT_RT is part of mainline since Linux 6.12. Many distributions ship
it as a separate package (for example `linux-image-rt-amd64` on Debian, or
Ubuntu Pro's real-time kernel).

**What to expect.** p50 often gets slightly worse, while p99.99 and max get
much better and more predictable. Compare by running the suite on generic,
lowlatency (`PREEMPT_DYNAMIC` with `preempt=full`) and RT kernels on the same
machine. The report header shows the preemption model.

**Costs.** Lower throughput for I/O-heavy work. Some out-of-tree drivers
(notably GPU drivers) may lag behind.

**Worth it when** you have deadlines (control loops, audio, industrial I/O)
rather than "faster on average" goals.

## CPU vulnerability mitigations: `mitigations=off`

**What it does.** Disables kernel workarounds for speculative-execution
vulnerabilities (Spectre, Meltdown, MDS, Retbleed and so on).

**What to expect.** Cheaper syscalls, context switches and kernel entries on
CPUs that need heavy mitigations. On recent CPUs with hardware fixes the gain
can be small. The syscall benchmark (roadmap) lists the active mitigations from
`/sys/devices/system/cpu/vulnerabilities/` so you can compare.

**Costs.** This is a security decision, not a tuning knob. Only on isolated,
single-tenant machines that run only trusted code.

## SMT (Hyper-Threading) on or off

**What it changes.** With SMT one physical core runs two hardware threads
(logical CPUs) that share its L1 and L2 caches, TLBs, branch predictors and
execution units. Linux lists the pairs in
`/sys/devices/system/cpu/cpu*/topology/thread_siblings_list`.

**What to expect.** A thread whose sibling is busy loses part of the core: a
latency-sensitive loop typically runs 10 to 40% slower and its tail gets
worse, because the other thread evicts its L1 lines and competes for the same
ports. A thread whose sibling is *idle* loses almost nothing; modern cores give
it the whole core back. So the rule for low latency is one busy hardware thread
per physical core, not necessarily SMT off: isolating both siblings of a core
and pinning only one thread there gets nearly all of the benefit and keeps SMT
for the rest of the machine. Turning SMT off guarantees it machine-wide, and
also closes the cross-thread side channels (L1TF, MDS) that otherwise require
mitigations. Throughput work (builds, many independent requests) loses 15 to
30% with SMT off.

**How.**

* At run time, until reboot (root):
  `echo off > /sys/devices/system/cpu/smt/control` (`on` to restore;
  `cat /sys/devices/system/cpu/smt/active` to check).
* At boot: the `nosmt` kernel parameter (`nosmt=force` prevents turning it
  back on).
* In firmware: "Hyper-Threading" or "SMT" in the BIOS/UEFI setup.

**Measure it.** Run once with SMT on and once off and compare the two
results.json (see Recording what you changed). The planned standard-tier SMT
benchmark measures the sibling effect directly without turning SMT off.

**Costs.** Half the hardware threads; on SMT-off machines `nproc` halves.

## Idle states: `idle=poll`, `intel_idle.max_cstate`, BIOS C-states

**What it does.** Stops cores from entering deep sleep states, whose exit
latency shows up as slow wake-ups.

`--no-idle` (used by `run-all.sh`) already does this for the duration of a run,
on the benchmark CPUs only. It writes C1's exit latency to
`/sys/devices/system/cpu/cpuN/power/pm_qos_resume_latency_us`, so those CPUs
can still halt in C1 but not sleep deeper, while the other CPUs keep their
normal idle states. The old values come back at exit, also on Ctrl-C or a
crash. A `kill -9` leaves them until reboot; reset them with
`echo 0 | sudo tee /sys/devices/system/cpu/cpu*/power/pm_qos_resume_latency_us`.
Where per-CPU limits are not available, it holds `/dev/cpu_dma_latency` for all
CPUs instead.

`--idle-latency-us N` picks another limit. `0` allows only polling, like
`idle=poll` on those CPUs: an idle CPU spins instead of halting. On a
power-limited laptop the spinning uses up the power budget the measuring core
needs for turbo, and a spinning SMT sibling takes execution resources from the
measuring thread on the same core. Use it only to see what polling buys over C1.

Boot options make the limit permanent: `intel_idle.max_cstate=1` or
`processor.max_cstate=1`, or `idle=poll` for always-spinning idle loops.

**What to expect.** On a laptop run, mutex wake-up p50 dropped from 15 us to
2.2 us with `--no-idle`. The boot options give the same effect without needing
root at run time.

**Costs.** Much higher idle power and heat. `idle=poll` also slows SMT siblings,
which share the core with a spinning idle loop.

## Huge pages reserved at boot

```
default_hugepagesz=2M hugepagesz=2M hugepages=1024 hugepagesz=1G hugepages=4
```

**What to expect.** Run the cache sweep with `--hugepages` and compare with the
default curve: past the TLB reach, the random-load latency should drop because
page walks disappear. 1 GiB pages help very large working sets.

**Costs.** The memory is reserved and unavailable to anything else.
Transparent huge pages (`/sys/kernel/mm/transparent_hugepage/enabled`) give
part of the benefit without a reboot.

## Firmware (BIOS/UEFI) settings

Not scriptable from Linux, but often the biggest win on servers:

* turbo on or off (more speed vs steadier frequency), C-states and C1E,
  package C-states;
* SMT on or off;
* "performance" or "low latency" power profile, disabling energy-efficient
  turbo and uncore frequency scaling;
* on some boards, SMI-heavy features (USB legacy, ECC scrubbing intervals) that
  cause unexplained gaps. Jitter that no OS setting removes is often SMIs
  (`hwlat` tracer: `echo hwlat > /sys/kernel/tracing/current_tracer`).

## Kernel-bypass networking

The standard tier measures the kernel network stack. Kernel bypass hands the
NIC to user space and polls it directly:

| Technology | Hardware | Notes |
|---|---|---|
| DPDK | most server NICs (Intel, Mellanox/NVIDIA, Broadcom, and others) | user-space poll-mode drivers, huge pages, dedicated cores |
| NXP DPAA / DPAA2 | NXP Layerscape and QorIQ SoCs | hardware queue managers, used through DPDK or USDPAA |
| AF_XDP | any NIC with XDP support | stays in the kernel driver model; a middle ground |
| RDMA / ibverbs | InfiniBand, RoCE NICs | zero-copy, kernel bypass for host-to-host messaging |
| Onload / TCPDirect class | vendor-specific NICs | socket API preserved, stack moved to user space |

**What to expect.** Round trips from tens of microseconds through the kernel to
low single-digit microseconds. The cost is dedicated cores spinning at 100%,
NICs taken away from the kernel, and different APIs and tooling. The project
keeps a transport abstraction so these back ends can plug into the same
benchmarks and charts (see [ROADMAP.md](ROADMAP.md)).

## Recording what you changed

The report header already records the kernel, preemption model, governor,
cpuidle driver, isolated CPUs and THP mode. The planned `--expert` mode will
also store the kernel command line and active mitigations in results.json, so
two reports can be compared knowing exactly what differed.
