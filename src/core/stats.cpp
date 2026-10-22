#include "olt/stats.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace olt {

double Summary::at(double p) const
{
    for (const auto& pc : percentiles)
        if (std::fabs(pc.p - p) < 1e-9) return pc.value;
    return std::numeric_limits<double>::quiet_NaN();
}

double percentile_sorted(const std::vector<double>& sorted, double p)
{
    if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
    if (sorted.size() == 1) return sorted.front();
    p = std::clamp(p, 0.0, 100.0);
    const double h = (static_cast<double>(sorted.size()) - 1.0) * p / 100.0;
    const size_t lo = static_cast<size_t>(std::floor(h));
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    return sorted[lo] + (h - static_cast<double>(lo)) * (sorted[hi] - sorted[lo]);
}

size_t min_samples_for(double p)
{
    if (p <= 50.0) return 1;
    if (p >= 100.0) return std::numeric_limits<size_t>::max();
    // tolerance absorbs binary rounding of p (e.g. 100 - 99.9 != 0.1 exactly)
    return static_cast<size_t>(std::ceil(1000.0 / (100.0 - p) - 1e-6));
}

Summary summarize(std::vector<double> samples, const std::vector<double>& percentiles)
{
    Summary s;
    s.count = samples.size();
    if (samples.empty()) return s;

    std::sort(samples.begin(), samples.end());
    s.min = samples.front();
    s.max = samples.back();

    const double n = static_cast<double>(samples.size());
    s.mean = std::accumulate(samples.begin(), samples.end(), 0.0) / n;
    if (samples.size() > 1) {
        double acc = 0;
        for (double v : samples) acc += (v - s.mean) * (v - s.mean);
        s.stddev = std::sqrt(acc / (n - 1.0));
    }

    for (double p : percentiles)
        s.percentiles.push_back({p, percentile_sorted(samples, p), s.count >= min_samples_for(p)});

    const double median = percentile_sorted(samples, 50.0);
    std::vector<double> dev(samples.size());
    std::transform(samples.begin(), samples.end(), dev.begin(),
                   [median](double v) { return std::fabs(v - median); });
    std::sort(dev.begin(), dev.end());
    s.mad = percentile_sorted(dev, 50.0);
    return s;
}

}  // namespace olt
