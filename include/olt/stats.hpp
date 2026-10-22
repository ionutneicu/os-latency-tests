// olt/stats.hpp - descriptive statistics for latency samples.
#pragma once

#include <cstddef>
#include <vector>

namespace olt {

// Percentiles reported for every benchmark.
inline const std::vector<double>& default_percentiles()
{
    static const std::vector<double> p{50.0, 90.0, 99.0, 99.9, 99.99};
    return p;
}

struct Percentile {
    double p = 0;        // e.g. 99.9
    double value = 0;
    // A tail percentile is only meaningful with enough samples beyond it;
    // we require n >= 10 / (1 - p/100), i.e. at least ~10 samples in the tail.
    bool reliable = false;
};

struct Summary {
    size_t count = 0;
    double min = 0;
    double max = 0;
    double mean = 0;
    double stddev = 0;   // sample standard deviation
    double mad = 0;      // median absolute deviation, robust spread
    std::vector<Percentile> percentiles;

    // Value of percentile p if it was computed, otherwise NaN.
    double at(double p) const;
};

// Percentile by linear interpolation between closest ranks (Hyndman & Fan
// type 7, the default of NumPy and R). `sorted` must be ascending.
double percentile_sorted(const std::vector<double>& sorted, double p);

// Minimum sample count for percentile p to be considered reliable.
size_t min_samples_for(double p);

Summary summarize(std::vector<double> samples,
                  const std::vector<double>& percentiles = default_percentiles());

}  // namespace olt
