#include "olt/report.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "olt/hal/perf.h"

namespace olt {

namespace {

const char* affinity_name(Affinity a)
{
    switch (a) {
    case Affinity::None: return "none";
    case Affinity::SameCpu: return "same";
    case Affinity::DifferentCpus: return "different";
    }
    return "?";
}

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::string q(const std::string& s) { return "\"" + json_escape(s) + "\""; }

const char* json_tristate(int v) { return v < 0 ? "null" : v ? "true" : "false"; }

std::string optimization_text(const BuildOptimization& o)
{
    if (o.c == 1 && o.cxx == 1) return "optimised";
    const std::string off = unoptimised_languages(o);
    return off.empty() ? "optimisation unknown" : off + " NOT OPTIMISED, results are wrong";
}

std::string idle_text(const Config& cfg)
{
    if (!cfg.block_deep_idle) return "deep idle allowed";
    const IdleLimit& l = cfg.idle_limit;
    if (!l.applied) return "limit refused, deep idle allowed";
    std::string s = "limited to " + std::to_string(l.latency_us) + " us exit latency on " +
                    (l.per_cpu ? "the benchmark CPUs" : "all CPUs");
    if (!l.states.empty()) s += " (allowed: " + l.states + ")";
    return s;
}

std::string num(double v)
{
    if (!std::isfinite(v)) return "null";
    std::ostringstream ss;
    ss << std::setprecision(10) << v;
    return ss.str();
}

std::string percentile_key(double p)
{
    std::ostringstream ss;
    ss << "p" << p;
    std::string s = ss.str();
    for (auto& c : s)
        if (c == '.') c = '_';
    return s;  // p50, p99_9, p99_99
}

std::string sanitize_filename(const std::string& s)
{
    std::string out;
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
    return out;
}

}  // namespace

std::string format_bytes(uint64_t bytes)
{
    char buf[32];
    if (bytes >= (uint64_t(1) << 30)) std::snprintf(buf, sizeof(buf), "%.3gG", double(bytes) / double(uint64_t(1) << 30));
    else if (bytes >= (uint64_t(1) << 20)) std::snprintf(buf, sizeof(buf), "%.3gM", double(bytes) / double(uint64_t(1) << 20));
    else if (bytes >= 1024) std::snprintf(buf, sizeof(buf), "%.3gK", double(bytes) / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%lluB", static_cast<unsigned long long>(bytes));
    return buf;
}

std::string format_ns(double ns)
{
    if (!std::isfinite(ns)) return "n/a";
    const char* unit = "ns";
    double v = ns;
    if (std::fabs(v) >= 1e9) { v /= 1e9; unit = "s"; }
    else if (std::fabs(v) >= 1e6) { v /= 1e6; unit = "ms"; }
    else if (std::fabs(v) >= 1e3) { v /= 1e3; unit = "us"; }
    char buf[32];
    if (std::fabs(v) >= 100) std::snprintf(buf, sizeof(buf), "%.0f %s", v, unit);
    else if (std::fabs(v) >= 10) std::snprintf(buf, sizeof(buf), "%.1f %s", v, unit);
    else std::snprintf(buf, sizeof(buf), "%.2f %s", v, unit);
    return buf;
}

std::string format_params(const Params& params)
{
    std::string s;
    for (const auto& kv : params) {
        if (!s.empty()) s += ",";
        s += kv.first + "=" + kv.second;
    }
    return s;
}

void print_platform(std::ostream& os, const PlatformInfo& p, const Config& cfg)
{
    os << "Platform\n";
    os << "  arch/os        : " << p.arch << " / " << p.os << "\n";
    os << "  cpu            : " << p.cpu_model << " (" << p.cpu_count << " online)\n";
    if (!p.board.empty()) os << "  board          : " << p.board << "\n";
    os << "  kernel         : " << p.kernel_release << " [" << p.preempt_model << "]\n";
    if (!p.governor.empty()) os << "  governor       : " << p.governor << "\n";
    if (!p.cpuidle_driver.empty()) os << "  cpuidle        : " << p.cpuidle_driver << "\n";
    if (!p.thp_mode.empty()) os << "  huge pages     : THP " << p.thp_mode << ", benchmark buffers "
                                << (cfg.hugepages ? "request huge pages" : "use base pages") << "\n";
    if (!p.isolated_cpus.empty()) os << "  isolated cpus  : " << p.isolated_cpus << "\n";
    os << "  caches         :";
    for (const auto& c : p.caches)
        os << " L" << c.level << (c.type[0] == 'D' ? "d" : c.type[0] == 'I' ? "i" : "") << "="
           << format_bytes(c.size_bytes);
    os << " (line " << p.cache_line_size << " B)\n";
    os << "  cycle counter  : " << std::fixed << std::setprecision(3) << p.ticks.ticks_per_ns * 1e3
       << " MHz" << (p.ticks.from_hardware ? " (reported)" : " (calibrated)") << std::defaultfloat << "\n";
    os << "  timer overhead : now_ns " << format_ns(p.timer.now_ns_median) << " median, cycles "
       << format_ns(p.timer.cycles_ns_median) << " median, clock res " << p.timer.os_resolution_ns << " ns\n";
    os << "  perf counters  : " << p.perf_backend;
    if (p.perf_backend != "none") {
        os << " (" << p.perf_events.size() << "/" << OLT_PERF_EVENT_COUNT << " events";
        for (size_t i = 0; i < p.perf_events.size(); ++i) os << (i ? ", " : ": ") << p.perf_events[i];
        os << ")";
    }
    os << "\n";
    os << "  build          : " << p.compiler << ", " << p.build_type << ", " << optimization_text(p.optimized);
    if (!p.git_commit.empty())
        os << ", commit " << p.git_commit.substr(0, 12) << (p.git_dirty ? " + local changes" : "");
    os << "\n";
    os << "Config\n";
    os << "  samples/warmup : " << cfg.samples << " / " << cfg.warmup << " iterations, CPU warm-up "
       << cfg.cpu_warmup_ms << " ms per benchmark\n";
    os << "  affinity       : " << affinity_name(cfg.affinity) << " (main cpu " << cfg.main_cpu
       << ", worker cpu " << cfg.worker_cpu << ")\n";
    os << "  rt priority    : " << (cfg.rt_priority > 0 ? "SCHED_FIFO " + std::to_string(cfg.rt_priority) : "default")
       << ", mlockall " << (cfg.lock_memory ? "on" : "off") << "\n";
    os << "  idle states    : " << idle_text(cfg) << "\n\n";
}

void print_table_header(std::ostream& os)
{
    os << std::left << std::setw(58) << "benchmark" << std::right << std::setw(7) << "n"
       << std::setw(10) << "min" << std::setw(10) << "p50" << std::setw(10) << "p90" << std::setw(10)
       << "p99" << std::setw(10) << "p99.9" << std::setw(10) << "p99.99" << std::setw(10) << "max" << "\n";
    os << std::string(58 + 7 + 10 * 7, '-') << "\n";
}

void print_table_row(std::ostream& os, const SummarizedResult& r)
{
    std::string label = r.result->name;
    const std::string params = format_params(r.result->params);
    if (!params.empty()) label += " [" + params + "]";
    const Summary& s = r.summary;
    auto cell = [&](double p) {
        for (const auto& pc : s.percentiles)
            if (std::fabs(pc.p - p) < 1e-9) return (pc.reliable ? "" : "~") + format_ns(pc.value);
        return std::string("n/a");
    };
    os << std::left << std::setw(58) << label << std::right << std::setw(7) << s.count << std::setw(10)
       << format_ns(s.min) << std::setw(10) << cell(50) << std::setw(10) << cell(90) << std::setw(10)
       << cell(99) << std::setw(10) << cell(99.9) << std::setw(10) << cell(99.99) << std::setw(10)
       << format_ns(s.max) << "\n";
    if (!r.result->note.empty()) os << "    note: " << r.result->note << "\n";
    if (!r.result->metrics.empty()) {
        os << "    metrics:";
        for (const auto& m : r.result->metrics) os << " " << m.first << "=" << std::setprecision(3) << m.second;
        os << std::defaultfloat << "\n";
    }
}

void write_json(const std::string& path, const PlatformInfo& p, const Config& cfg,
                const std::vector<SummarizedResult>& results, const std::vector<std::string>& warnings)
{
    std::ofstream f(path);
    if (!f) throw std::runtime_error("cannot write " + path);

    f << "{\n  \"schema\": \"olt-results/1\",\n";
    f << "  \"platform\": {\n";
    f << "    \"arch\": " << q(p.arch) << ",\n";
    f << "    \"os\": " << q(p.os) << ",\n";
    f << "    \"cpu_model\": " << q(p.cpu_model) << ",\n";
    f << "    \"board\": " << q(p.board) << ",\n";
    f << "    \"cpu_count\": " << p.cpu_count << ",\n";
    f << "    \"kernel_release\": " << q(p.kernel_release) << ",\n";
    f << "    \"kernel_version\": " << q(p.kernel_version) << ",\n";
    f << "    \"preempt_model\": " << q(p.preempt_model) << ",\n";
    f << "    \"governor\": " << q(p.governor) << ",\n";
    f << "    \"cpuidle_driver\": " << q(p.cpuidle_driver) << ",\n";
    f << "    \"isolated_cpus\": " << q(p.isolated_cpus) << ",\n";
    f << "    \"thp_mode\": " << q(p.thp_mode) << ",\n";
    f << "    \"physical_memory\": " << p.physical_memory << ",\n";
    f << "    \"cache_line_size\": " << p.cache_line_size << ",\n";
    f << "    \"caches\": [";
    for (size_t i = 0; i < p.caches.size(); ++i) {
        const auto& c = p.caches[i];
        f << (i ? ", " : "") << "\n      {\"level\": " << c.level << ", \"type\": " << q(c.type)
          << ", \"size_bytes\": " << c.size_bytes << ", \"line_size\": " << c.line_size
          << ", \"ways\": " << c.ways << ", \"shared_cpus\": " << q(c.shared_cpus) << "}";
    }
    f << (p.caches.empty() ? "],\n" : "\n    ],\n");
    f << "    \"perf\": {\"backend\": " << q(p.perf_backend) << ", \"events\": [";
    for (size_t i = 0; i < p.perf_events.size(); ++i) f << (i ? ", " : "") << q(p.perf_events[i]);
    f << "]},\n";
    f << "    \"compiler\": " << q(p.compiler) << ",\n";
    f << "    \"build_type\": " << q(p.build_type) << ",\n";
    f << "    \"git_commit\": " << q(p.git_commit) << ", \"git_dirty\": " << (p.git_dirty ? "true" : "false") << ",\n";
    f << "    \"optimized\": {\"c\": " << json_tristate(p.optimized.c) << ", \"cxx\": "
      << json_tristate(p.optimized.cxx) << "},\n";
    f << "    \"cycle_counter\": {\"ticks_per_ns\": " << num(p.ticks.ticks_per_ns)
      << ", \"from_hardware\": " << (p.ticks.from_hardware ? "true" : "false") << "},\n";
    f << "    \"timer\": {\"now_ns_min\": " << num(p.timer.now_ns_min) << ", \"now_ns_median\": "
      << num(p.timer.now_ns_median) << ", \"cycles_ns_min\": " << num(p.timer.cycles_ns_min)
      << ", \"cycles_ns_median\": " << num(p.timer.cycles_ns_median)
      << ", \"os_resolution_ns\": " << p.timer.os_resolution_ns << "}\n";
    f << "  },\n";

    f << "  \"config\": {\"samples\": " << cfg.samples << ", \"warmup\": " << cfg.warmup
      << ", \"affinity\": " << q(affinity_name(cfg.affinity)) << ", \"main_cpu\": " << cfg.main_cpu
      << ", \"worker_cpu\": " << cfg.worker_cpu << ", \"rt_priority\": " << cfg.rt_priority
      << ", \"lock_memory\": " << (cfg.lock_memory ? "true" : "false")
      << ", \"block_deep_idle\": " << (cfg.block_deep_idle ? "true" : "false") << ", \"idle_limit\": ";
    if (!cfg.block_deep_idle)
        f << "null";
    else
        f << "{\"requested_us\": " << cfg.idle_latency_us << ", \"applied\": " << (cfg.idle_limit.applied ? "true" : "false")
          << ", \"latency_us\": " << cfg.idle_limit.latency_us << ", \"per_cpu\": "
          << (cfg.idle_limit.per_cpu ? "true" : "false") << ", \"states\": " << q(cfg.idle_limit.states) << "}";
    f << ", \"park_us\": " << cfg.park_us
      << ", \"cpu_warmup_ms\": " << cfg.cpu_warmup_ms << ", \"sweep_samples\": " << cfg.sweep_samples << ", \"hugepages\": " << (cfg.hugepages ? "true" : "false")
      << "},\n";

    f << "  \"warnings\": [";
    for (size_t i = 0; i < warnings.size(); ++i) f << (i ? ", " : "") << q(warnings[i]);
    f << "],\n";
    f << "  \"results\": [";
    for (size_t i = 0; i < results.size(); ++i) {
        const Result& r = *results[i].result;
        const Summary& s = results[i].summary;
        f << (i ? "," : "") << "\n    {\n      \"name\": " << q(r.name) << ",\n      \"params\": {";
        for (size_t k = 0; k < r.params.size(); ++k)
            f << (k ? ", " : "") << q(r.params[k].first) << ": " << q(r.params[k].second);
        f << "},\n      \"unit\": " << q(r.unit) << ",\n";
        if (r.source.file)
            f << "      \"source\": {\"file\": " << q(source_path(r.source.file)) << ", \"line\": " << r.source.line << "},\n";
        if (!r.note.empty()) f << "      \"note\": " << q(r.note) << ",\n";
        if (!r.metrics.empty()) {
            f << "      \"metrics\": {";
            for (size_t k = 0; k < r.metrics.size(); ++k)
                f << (k ? ", " : "") << q(r.metrics[k].first) << ": " << num(r.metrics[k].second);
            f << "},\n";
        }
        f << "      \"raw_csv\": " << q("raw/" + sanitize_filename(r.name + (r.params.empty() ? "" : "_" + format_params(r.params))) + ".csv") << ",\n";
        f << "      \"count\": " << s.count << ", \"min\": " << num(s.min) << ", \"max\": " << num(s.max)
          << ", \"mean\": " << num(s.mean) << ", \"stddev\": " << num(s.stddev) << ", \"mad\": " << num(s.mad)
          << ",\n      \"percentiles\": {";
        for (size_t k = 0; k < s.percentiles.size(); ++k)
            f << (k ? ", " : "") << q(percentile_key(s.percentiles[k].p)) << ": " << num(s.percentiles[k].value);
        f << "},\n      \"unreliable_percentiles\": [";
        bool first = true;
        for (const auto& pc : s.percentiles)
            if (!pc.reliable) {
                f << (first ? "" : ", ") << q(percentile_key(pc.p));
                first = false;
            }
        f << "]\n    }";
    }
    f << "\n  ]\n}\n";
}

std::string write_raw_csv(const std::string& dir, const Result& r)
{
    const std::string name =
        sanitize_filename(r.name + (r.params.empty() ? "" : "_" + format_params(r.params))) + ".csv";
    std::ofstream f(dir + "/" + name);
    if (!f) throw std::runtime_error("cannot write " + dir + "/" + name);
    f << "sample," << r.unit << "\n";
    for (size_t i = 0; i < r.samples.size(); ++i) f << i << "," << num(r.samples[i]) << "\n";
    return name;
}

}  // namespace olt
