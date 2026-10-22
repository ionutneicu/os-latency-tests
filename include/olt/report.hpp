// olt/report.hpp - result output: console table, JSON summary, raw CSV.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "olt/benchmark.hpp"
#include "olt/platform.hpp"
#include "olt/stats.hpp"

namespace olt {

struct SummarizedResult {
    const Result* result;
    Summary summary;
};

// "1.23 us"-style formatting with 3 significant digits.
std::string format_ns(double ns);

// "48K", "1.25M"-style size with up to 3 significant digits.
std::string format_bytes(uint64_t bytes);

// Short "key=value,key=value" form of benchmark parameters.
std::string format_params(const Params& params);

void print_platform(std::ostream& os, const PlatformInfo& p, const Config& cfg);
void print_table_header(std::ostream& os);
void print_table_row(std::ostream& os, const SummarizedResult& r);

// results.json: platform, config, and summary stats of every result.
void write_json(const std::string& path, const PlatformInfo& p, const Config& cfg,
                const std::vector<SummarizedResult>& results, const std::vector<std::string>& warnings = {});

// One CSV per result with every raw sample; returns the file name used.
std::string write_raw_csv(const std::string& dir, const Result& r);

}  // namespace olt
