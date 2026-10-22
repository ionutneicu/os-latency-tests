// os-latency-tests - measures OS and hardware latencies from user space and
// reports them with tail percentiles, so platforms and kernels can be
// compared and low-latency design decisions can be backed by data.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/clock.hpp"
#include "olt/hal/os.h"
#include "olt/platform.hpp"
#include "olt/report.hpp"
#include "olt/stats.hpp"

namespace {

struct Options {
    olt::Config config;
    std::vector<std::string> filters;
    std::vector<int> cpus;  // explicit --cpus main[,worker]
    std::string out_dir = "olt-results";
    bool list = false;
    bool raw = true;
};

void usage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  -l, --list                 list benchmarks and exit\n"
        << "  -t, --test PATTERN         run benchmarks matching PATTERN ('*' and '?' globs);\n"
        << "                             repeatable, default: all\n"
        << "  -n, --samples N            measured samples per benchmark (default 10000)\n"
        << "  -w, --warmup N             discarded warm-up iterations (default 100)\n"
        << "  -a, --affinity MODE        none | same | different: pin main and worker threads\n"
        << "                             to no CPU, one shared CPU, or two CPUs (default different)\n"
        << "      --cpus MAIN[,WORKER]   explicit CPUs to pin to (overrides automatic choice)\n"
        << "  -p, --rt-prio N            run threads as SCHED_FIFO with priority N (needs privileges)\n"
        << "  -m, --mlock                lock all memory with mlockall()\n"
        << "      --no-idle              keep CPUs out of deep idle states during the run\n"
        << "                             (/dev/cpu_dma_latency = 0, needs root)\n"
        << "      --park-us N            how long a signalling thread waits so its peer is\n"
        << "                             blocked in the kernel (default 200)\n"
        << "      --mem-min BYTES        smallest buffer for memory sweeps (default 1K; K/M/G suffixes)\n"
        << "      --mem-max BYTES        largest buffer for memory sweeps (default 4x the largest cache)\n"
        << "      --sweep-steps N        buffer sizes per doubling in memory sweeps (default 4)\n"
        << "      --sweep-samples N      samples per buffer size in memory sweeps (default 15)\n"
        << "      --hugepages            back sweep buffers with transparent huge pages (removes most\n"
        << "                             TLB misses; default uses base pages so TLB effects show)\n"
        << "  -o, --out DIR              output directory for results.json and raw/ (default olt-results)\n"
        << "      --no-raw               do not write raw per-sample CSV files\n"
        << "  -h, --help                 show this help\n\n"
        << "Percentiles marked '~' have fewer than ~10 samples beyond them and are not statistically reliable;\n"
        << "increase --samples (p99.99 needs >= 100000).\n";
}

[[noreturn]] void die(const std::string& msg)
{
    std::cerr << "error: " << msg << "\n";
    std::exit(2);
}

size_t parse_size(const std::string& opt, const char* v)
{
    char* end = nullptr;
    const unsigned long long n = std::strtoull(v, &end, 10);
    if (!end || *end != '\0') die("invalid number for " + opt + ": " + v);
    return static_cast<size_t>(n);
}

size_t parse_bytes(const std::string& opt, const char* v)
{
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end && (*end == 'K' || *end == 'k')) { n <<= 10; ++end; }
    else if (end && (*end == 'M' || *end == 'm')) { n <<= 20; ++end; }
    else if (end && (*end == 'G' || *end == 'g')) { n <<= 30; ++end; }
    if (!end || *end != '\0') die("invalid size for " + opt + ": " + v);
    return static_cast<size_t>(n);
}

Options parse_args(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) die("missing value for " + a);
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(argv[0]); std::exit(0); }
        else if (a == "-l" || a == "--list") o.list = true;
        else if (a == "-t" || a == "--test") o.filters.push_back(value());
        else if (a == "-n" || a == "--samples") o.config.samples = parse_size(a, value());
        else if (a == "-w" || a == "--warmup") o.config.warmup = parse_size(a, value());
        else if (a == "-p" || a == "--rt-prio") o.config.rt_priority = static_cast<int>(parse_size(a, value()));
        else if (a == "-m" || a == "--mlock") o.config.lock_memory = true;
        else if (a == "--no-idle") o.config.block_deep_idle = true;
        else if (a == "--park-us") o.config.park_us = static_cast<unsigned>(parse_size(a, value()));
        else if (a == "--mem-min") o.config.mem_min_bytes = parse_bytes(a, value());
        else if (a == "--mem-max") o.config.mem_max_bytes = parse_bytes(a, value());
        else if (a == "--sweep-steps") o.config.sweep_steps_per_octave = static_cast<unsigned>(parse_size(a, value()));
        else if (a == "--sweep-samples") o.config.sweep_samples = parse_size(a, value());
        else if (a == "--hugepages") o.config.hugepages = true;
        else if (a == "-o" || a == "--out") o.out_dir = value();
        else if (a == "--no-raw") o.raw = false;
        else if (a == "-a" || a == "--affinity") {
            const std::string v = value();
            if (v == "none") o.config.affinity = olt::Affinity::None;
            else if (v == "same" || v == "single") o.config.affinity = olt::Affinity::SameCpu;
            else if (v == "different" || v == "multiple") o.config.affinity = olt::Affinity::DifferentCpus;
            else die("unknown affinity mode: " + v);
        } else if (a == "--cpus") {
            std::string v = value();
            size_t pos = 0;
            while (pos <= v.size()) {
                const size_t comma = v.find(',', pos);
                o.cpus.push_back(static_cast<int>(parse_size(a, v.substr(pos, comma - pos).c_str())));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else die("unknown option: " + a + " (see --help)");
    }
    if (o.config.samples == 0) die("--samples must be > 0");
    if (o.config.sweep_samples == 0) die("--sweep-samples must be > 0");
    if (o.config.sweep_steps_per_octave == 0) die("--sweep-steps must be > 0");
    return o;
}

// Picks CPUs for the main and worker threads. Automatic choice prefers the
// highest-numbered allowed CPUs, since CPU 0 typically services more
// interrupts and housekeeping work.
void resolve_cpus(Options& o)
{
    olt::Config& c = o.config;
    if (!o.cpus.empty()) {
        c.main_cpu = o.cpus[0];
        c.worker_cpu = o.cpus.size() > 1 ? o.cpus[1] : o.cpus[0];
        c.affinity = c.main_cpu == c.worker_cpu ? olt::Affinity::SameCpu : olt::Affinity::DifferentCpus;
        return;
    }
    if (c.affinity == olt::Affinity::None) return;

    int allowed[1024];
    const int n = olt_os_allowed_cpus(allowed, 1024);
    if (n <= 0) {
        std::cerr << "warning: cannot read allowed CPUs, running unpinned\n";
        c.affinity = olt::Affinity::None;
        return;
    }
    c.main_cpu = allowed[n - 1];
    if (c.affinity == olt::Affinity::DifferentCpus && n >= 2) {
        c.worker_cpu = allowed[n - 2];
    } else {
        if (c.affinity == olt::Affinity::DifferentCpus)
            std::cerr << "warning: only one CPU available, using the same CPU for both threads\n";
        c.affinity = olt::Affinity::SameCpu;
        c.worker_cpu = c.main_cpu;
    }
}

void apply_main_thread_settings(const olt::Config& c)
{
    if (c.lock_memory) {
        if (int rc = olt_os_lock_memory())
            std::cerr << "warning: mlockall failed: " << std::strerror(rc) << "\n";
    }
    if (c.block_deep_idle) {
        if (int rc = olt_os_block_deep_idle())
            std::cerr << "warning: cannot block deep idle states: " << std::strerror(rc) << " (run as root)\n";
    }
    if (c.main_cpu >= 0) {
        if (int rc = olt_os_pin_current_thread(c.main_cpu))
            std::cerr << "warning: cannot pin to CPU " << c.main_cpu << ": " << std::strerror(rc) << "\n";
    }
    if (c.rt_priority > 0) {
        if (int rc = olt_os_set_rt_priority(c.rt_priority))
            std::cerr << "warning: cannot set SCHED_FIFO priority " << c.rt_priority << ": " << std::strerror(rc)
                      << " (run as root or grant CAP_SYS_NICE)\n";
    }
}

bool selected(const Options& o, const std::string& name)
{
    if (o.filters.empty()) return true;
    for (const auto& f : o.filters)
        if (olt::glob_match(f, name) || name.find(f) != std::string::npos) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv)
{
    Options opt = parse_args(argc, argv);

    if (opt.list) {
        for (const auto& b : olt::registry()) std::cout << b.name << "\n    " << b.description << "\n";
        return 0;
    }

    std::vector<const olt::BenchmarkInfo*> todo;
    for (const auto& b : olt::registry())
        if (selected(opt, b.name)) todo.push_back(&b);
    if (todo.empty()) die("no benchmark matches the given --test filters (see --list)");

    resolve_cpus(opt);
    apply_main_thread_settings(opt.config);

    const olt::TickCalibration cal = olt::calibrate_ticks();
    const olt::TimerOverhead timer = olt::measure_timer_overhead(cal);
    const olt::PlatformInfo platform = olt::collect_platform_info(opt.config.main_cpu, cal, timer);
    olt::print_platform(std::cout, platform, opt.config);

    olt::Context ctx(opt.config, cal);
    std::vector<olt::SummarizedResult> summaries;
    olt::print_table_header(std::cout);
    for (const auto* b : todo) {
        std::cerr << "running " << b->name << " ...\r" << std::flush;
        const size_t first = ctx.results().size();
        b->run(ctx);
        std::cerr << std::string(b->name.size() + 12, ' ') << "\r" << std::flush;
        for (size_t i = first; i < ctx.results().size(); ++i) {
            olt::SummarizedResult s{nullptr, olt::summarize(ctx.results()[i].samples)};
            summaries.push_back(s);
            // results() may still grow, so pointers are fixed up after the run
            olt::print_table_row(std::cout, {&ctx.results()[i], summaries.back().summary});
        }
    }
    for (size_t i = 0; i < summaries.size(); ++i) summaries[i].result = &ctx.results()[i];

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(opt.out_dir) / "raw", ec);
    if (ec) die("cannot create " + opt.out_dir + ": " + ec.message());
    if (opt.raw)
        for (const auto& r : ctx.results()) olt::write_raw_csv((fs::path(opt.out_dir) / "raw").string(), r);
    const std::string json = (fs::path(opt.out_dir) / "results.json").string();
    olt::write_json(json, platform, opt.config, summaries);

    // under sudo, hand the results back to the invoking user
    olt_os_restore_owner(opt.out_dir.c_str());
    for (const auto& entry : fs::recursive_directory_iterator(opt.out_dir, ec))
        olt_os_restore_owner(entry.path().c_str());
    std::cout << "\nResults written to " << json << (opt.raw ? " (raw samples in raw/)" : "") << "\n";
    return 0;
}
