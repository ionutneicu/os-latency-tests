// Linux: what the kernel boundary costs, how cheap reading a clock is, and how
// late a sleeping thread wakes up. Built on Linux only (CMakeLists.txt).
//
// os.call [call]  ns per call, batches of 256:
//   syscall_getpid     syscall(SYS_getpid): a real kernel entry and exit, the
//                      floor of any system call (glibc's getpid() may cache)
//   clock_monotonic    clock_gettime(CLOCK_MONOTONIC): vDSO, no kernel entry
//   clock_realtime, clock_monotonic_raw, clock_boottime: vDSO on recent kernels
//   clock_monotonic_coarse  vDSO, tick resolution (1-4 ms), cheapest
//   clock_thread_cputime    CLOCK_THREAD_CPUTIME_ID: a real system call
//   cycle_counter      olt_cycles(): rdtsc on x86, cntvct_el0 on ARM
//   A vDSO clock that costs as much as syscall_getpid has fallen back to the
//   system call (unstable TSC, a clock source without vDSO support, some VMs).
//
// os.timer_overshoot [method, request_us]  how much later than requested a
// sleeping thread runs again, for 1, 10, 100 and 1000 us:
//   nanosleep          plain nanosleep(): includes the thread's timer slack,
//                      50 us by default for normal (SCHED_OTHER) threads
//   nanosleep_slack1   the same after prctl(PR_SET_TIMERSLACK, 1)
//   timerfd            a one-shot timerfd, blocking read()
//   epoll_wait         epoll_wait() timeout (whole milliseconds only: 1000 us)
//   Real-time threads (-p) always get zero slack, so plain nanosleep and
//   nanosleep_slack1 should match under -p.
//
// The platform block lists the CPU vulnerability mitigations the kernel has
// enabled, which set much of the system-call cost.

#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "olt/benchmark.hpp"

namespace {

constexpr size_t k_batch = 256;
constexpr uint64_t k_point_budget_ns = 400000000;  // wall time per timer_overshoot point

uint64_t clock_ns(clockid_t id)
{
    timespec ts;
    clock_gettime(id, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

template <class Fn>
void per_call(olt::Context& ctx, const char* name, Fn fn)
{
    ctx.add_result("os.call", {{"call", name}}, ctx.run_samples([&] {
        uint64_t sink = 0;
        const uint64_t t0 = olt::now_ns();
        for (size_t i = 0; i < k_batch; ++i) sink += fn();
        const uint64_t t1 = olt::now_ns();
        olt::do_not_optimize(sink);
        return double(t1 - t0) / double(k_batch);
    }));
    ctx.results().back().unit = "ns/call";
}

// Sleeps `ns` with the given method; returns false if the method is unusable.
struct Sleeper {
    int tfd = -1, efd = -1;
    Sleeper()
    {
        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        efd = epoll_create1(EPOLL_CLOEXEC);
    }
    ~Sleeper()
    {
        if (tfd >= 0) close(tfd);
        if (efd >= 0) close(efd);
    }
    bool nanosleep_for(uint64_t ns) const
    {
        timespec ts{time_t(ns / 1000000000ull), long(ns % 1000000000ull)};
        return nanosleep(&ts, nullptr) == 0;
    }
    bool timerfd_for(uint64_t ns) const
    {
        if (tfd < 0) return false;
        itimerspec its{};
        its.it_value = {time_t(ns / 1000000000ull), long(ns % 1000000000ull)};
        if (timerfd_settime(tfd, 0, &its, nullptr) != 0) return false;
        uint64_t expirations;
        return read(tfd, &expirations, sizeof(expirations)) == ssize_t(sizeof(expirations));
    }
    bool epoll_for(uint64_t ns) const
    {
        if (efd < 0) return false;
        epoll_event ev;
        return epoll_wait(efd, &ev, 1, int(ns / 1000000)) == 0;  // nothing registered: always times out
    }
};

}  // namespace

OLT_BENCHMARK("os.call",
              "Kernel boundary and clocks: ns per raw system call (getpid), per clock_gettime for each clock "
              "(vDSO or system call) and per cycle-counter read")
{
    per_call(ctx, "syscall_getpid", [] { return uint64_t(syscall(SYS_getpid)); });
    per_call(ctx, "clock_monotonic", [] { return clock_ns(CLOCK_MONOTONIC); });
    per_call(ctx, "clock_realtime", [] { return clock_ns(CLOCK_REALTIME); });
    per_call(ctx, "clock_monotonic_raw", [] { return clock_ns(CLOCK_MONOTONIC_RAW); });
    per_call(ctx, "clock_boottime", [] { return clock_ns(CLOCK_BOOTTIME); });
    per_call(ctx, "clock_monotonic_coarse", [] { return clock_ns(CLOCK_MONOTONIC_COARSE); });
    per_call(ctx, "clock_thread_cputime", [] { return clock_ns(CLOCK_THREAD_CPUTIME_ID); });
    per_call(ctx, "cycle_counter", [] { return olt::cycles(); });
}

OLT_BENCHMARK("os.timer_overshoot",
              "Timer accuracy: how much later than requested a thread wakes from nanosleep (default timer slack "
              "and 1 ns slack), a timerfd and epoll_wait, for 1 us to 1 ms (os.timer_overshoot)")
{
    const olt::Config& cfg = ctx.config();
    Sleeper sl;
    const int old_slack = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    struct Method {
        const char* name;
        bool slack1;
        bool (Sleeper::*fn)(uint64_t) const;
        uint64_t min_us;  // shortest request the method can express
    };
    const Method methods[] = {{"nanosleep", false, &Sleeper::nanosleep_for, 1},
                              {"nanosleep_slack1", true, &Sleeper::nanosleep_for, 1},
                              {"timerfd", false, &Sleeper::timerfd_for, 1},
                              {"epoll_wait", false, &Sleeper::epoll_for, 1000}};
    for (const Method& m : methods) {
        if (m.slack1) prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
        for (uint64_t us : {1, 10, 100, 1000}) {
            if (us < m.min_us) continue;
            const uint64_t req = us * 1000;
            // as many samples as fit the time budget, within [50, --samples]
            const size_t n = std::max<size_t>(50, std::min<size_t>(cfg.samples, k_point_budget_ns / (req + 60000)));
            std::vector<double> late;
            late.reserve(n);
            bool ok = true;
            for (size_t i = 0; i < n + 5 && ok; ++i) {  // the first 5 warm up
                const uint64_t t0 = olt::now_ns();
                ok = (sl.*m.fn)(req);
                const uint64_t t1 = olt::now_ns();
                if (i >= 5) late.push_back(double(t1 - t0) - double(req));
            }
            ctx.add_result("os.timer_overshoot", {{"method", m.name}, {"request_us", std::to_string(us)}},
                           ok ? std::move(late) : std::vector<double>{}, ok ? "" : "not available");
        }
        if (m.slack1 && old_slack > 0) prctl(PR_SET_TIMERSLACK, (unsigned long)old_slack, 0, 0, 0);
    }
}
